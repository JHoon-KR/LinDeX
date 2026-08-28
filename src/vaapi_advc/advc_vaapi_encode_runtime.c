#define _GNU_SOURCE

#include "advc_vaapi_encode_runtime.h"
#include "advc_vaapi_encode_eos.h"
#include "advc_vaapi_modifier_policy.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/dma-buf.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ADVC_ENCODE_CONFIG_SLOTS 8u
#define ADVC_ENCODE_CONTEXT_SLOTS 4u
#define ADVC_ENCODE_SURFACE_SLOTS 64u
#define ADVC_ENCODE_BUFFER_SLOTS 256u
#define ADVC_ENCODE_ID_INDEX_MASK UINT32_C(0x00ffffff)
#define ADVC_ENCODE_SYNC_LIMIT_NS UINT64_C(5000000000)
#define ADVC_ENCODE_DEFAULT_BITRATE UINT32_C(8000000)
#define ADVC_ENCODE_DEFAULT_FPS_MILLI UINT32_C(60000)
#define ADVC_ENCODE_MAX_DIMENSION UINT32_C(8192)

enum advc_encode_surface_state {
    ADVC_ENCODE_SURFACE_IDLE = 0,
    ADVC_ENCODE_SURFACE_PENDING = 1,
    ADVC_ENCODE_SURFACE_READY = 2,
    ADVC_ENCODE_SURFACE_ERROR = 3,
};

struct advc_encode_config_slot {
    int used;
    struct advc_vaapi_encode_config config;
};

struct advc_encode_surface_slot {
    int used;
    uint32_t width;
    uint32_t height;
    enum advc_encode_surface_state state;
    VAContextID owner_context;
    VABufferID coded_buffer;
    uint64_t pts_ns;
    uint64_t submit_index;
    int write_exported;
    uint64_t cpu_pixel_copies;
    uint64_t writable_prime_exports;
    uint64_t gpu_conversion_submits;
    uint64_t gpu_modifier_repack_submits;
    uint32_t pending_cpu_pixel_copies;
    uint32_t pending_writable_prime_exports;
    struct advc_dmabuf_descriptor prime;
    struct advc_dmabuf_descriptor broker_prime;
    int broker_prime_valid;
    int external_import;
    struct advc_vaapi_encode_surface_link link;
};

struct advc_encode_buffer_slot {
    int used;
    int mapped;
    VAContextID owner_context;
    VABufferType type;
    unsigned int element_size;
    unsigned int capacity_elements;
    unsigned int num_elements;
    size_t coded_capacity;
    VASurfaceID pending_surface;
    void *data;
    struct advc_vaapi_encode_coded_output output;
};

struct advc_encode_context_slot {
    int used;
    VAConfigID config_id;
    uint32_t width;
    uint32_t height;
    int picture_active;
    VASurfaceID target;
    uint32_t pending_count;
    uint64_t frame_index;
    uint64_t last_pts_ns;
    uint32_t encode_profile;
    struct advc_vaapi_encode_frame_params frame;
    struct advc_vaapi_encode_broker broker;
    int broker_open;
    int broker_frame_session;
    int broker_eos_received;
    struct advc_vaapi_encode_eos_state eos;
};

static uint32_t wire_encode_profile(VAProfile profile) {
    if (profile == VAProfileH264ConstrainedBaseline)
        return ADVC_ENCODE_PROFILE_H264_CONSTRAINED_BASELINE;
    if (profile == VAProfileHEVCMain)
        return ADVC_ENCODE_PROFILE_HEVC_MAIN;
    return ADVC_ENCODE_PROFILE_NONE;
}

struct advc_vaapi_encode_runtime {
    pthread_mutex_t mutex;
    char *socket_path;
    struct advc_vaapi_encode_policy policy;
    struct advc_vaapi_modifier_policy modifier_policy;
    int write_export_ready;
    struct advc_vaapi_image_runtime *images;
    struct advc_encode_config_slot configs[ADVC_ENCODE_CONFIG_SLOTS];
    struct advc_encode_context_slot contexts[ADVC_ENCODE_CONTEXT_SLOTS];
    struct advc_encode_surface_slot surfaces[ADVC_ENCODE_SURFACE_SLOTS];
    struct advc_encode_buffer_slot buffers[ADVC_ENCODE_BUFFER_SLOTS];
};

__attribute__((weak)) int advc_vaapi_gpu_repack_linear(
    const struct advc_dmabuf_descriptor *source,
    int source_acquire_fence_fd,
    struct advc_dmabuf_descriptor *linear,
    int *linear_acquire_fence_fd,
    int *source_release_fence_fd);

static int export_implicit_read_fence(
    const struct advc_dmabuf_descriptor *descriptor) {
    struct dma_buf_export_sync_file request;
    int status;
    if (descriptor == NULL || descriptor->object_count != 1 ||
        descriptor->objects[0].fd < 0) {
        errno = ENOTSUP;
        return -1;
    }
    memset(&request, 0, sizeof(request));
    request.flags = DMA_BUF_SYNC_READ;
    request.fd = -1;
    do {
        status = ioctl(descriptor->objects[0].fd,
                       DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &request);
    } while (status < 0 && errno == EINTR);
    if (status < 0) return -1;
    if (request.fd < 0 || advc_dmabuf_sync_file_validate(request.fd) < 0) {
        int saved = errno == 0 ? EPROTO : errno;
        if (request.fd >= 0) close(request.fd);
        errno = saved;
        return -1;
    }
    return request.fd;
}

static uint32_t make_id(uint32_t type, unsigned int index) {
    return type | (index + 1u);
}

static int id_index(uint32_t id, uint32_t type, unsigned int limit,
                    unsigned int *index) {
    uint32_t low;
    if ((id & ADVC_VAAPI_OBJECT_TYPE_MASK) != type) return -1;
    low = id & ADVC_ENCODE_ID_INDEX_MASK;
    if (low == 0 || low > limit) return -1;
    *index = low - 1u;
    return 0;
}

int advc_vaapi_encode_owns_config(VAConfigID id) {
    return (id & ADVC_VAAPI_OBJECT_TYPE_MASK) ==
           ADVC_VAAPI_ENCODE_CONFIG_TYPE;
}

int advc_vaapi_encode_owns_context(VAContextID id) {
    return (id & ADVC_VAAPI_OBJECT_TYPE_MASK) ==
           ADVC_VAAPI_ENCODE_CONTEXT_TYPE;
}

int advc_vaapi_encode_owns_surface(VASurfaceID id) {
    return (id & ADVC_VAAPI_OBJECT_TYPE_MASK) ==
           ADVC_VAAPI_ENCODE_SURFACE_TYPE;
}

int advc_vaapi_encode_owns_buffer(VABufferID id) {
    return (id & ADVC_VAAPI_OBJECT_TYPE_MASK) ==
           ADVC_VAAPI_ENCODE_BUFFER_TYPE;
}

static struct advc_encode_config_slot *get_config(
    struct advc_vaapi_encode_runtime *runtime, VAConfigID id) {
    unsigned int index;
    if (runtime == NULL ||
        id_index(id, ADVC_VAAPI_ENCODE_CONFIG_TYPE,
                 ADVC_ENCODE_CONFIG_SLOTS, &index) < 0 ||
        !runtime->configs[index].used)
        return NULL;
    return &runtime->configs[index];
}

static struct advc_encode_context_slot *get_context(
    struct advc_vaapi_encode_runtime *runtime, VAContextID id) {
    unsigned int index;
    if (runtime == NULL ||
        id_index(id, ADVC_VAAPI_ENCODE_CONTEXT_TYPE,
                 ADVC_ENCODE_CONTEXT_SLOTS, &index) < 0 ||
        !runtime->contexts[index].used)
        return NULL;
    return &runtime->contexts[index];
}

static struct advc_encode_surface_slot *get_surface(
    struct advc_vaapi_encode_runtime *runtime, VASurfaceID id) {
    unsigned int index;
    if (runtime == NULL ||
        id_index(id, ADVC_VAAPI_ENCODE_SURFACE_TYPE,
                 ADVC_ENCODE_SURFACE_SLOTS, &index) < 0 ||
        !runtime->surfaces[index].used)
        return NULL;
    return &runtime->surfaces[index];
}

static struct advc_encode_buffer_slot *get_buffer(
    struct advc_vaapi_encode_runtime *runtime, VABufferID id) {
    unsigned int index;
    if (runtime == NULL ||
        id_index(id, ADVC_VAAPI_ENCODE_BUFFER_TYPE,
                 ADVC_ENCODE_BUFFER_SLOTS, &index) < 0 ||
        !runtime->buffers[index].used)
        return NULL;
    return &runtime->buffers[index];
}

int advc_vaapi_encode_buffer_is_coded(
    struct advc_vaapi_encode_runtime *runtime, VABufferID id) {
    struct advc_encode_buffer_slot *buffer;
    int coded = 0;
    if (runtime == NULL) return 0;
    pthread_mutex_lock(&runtime->mutex);
    buffer = get_buffer(runtime, id);
    if (buffer != NULL && buffer->type == VAEncCodedBufferType) coded = 1;
    pthread_mutex_unlock(&runtime->mutex);
    return coded;
}

static VAStatus status_from_errno(int error) {
    if (error == ENOMEM) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    if (error == ETIMEDOUT) return VA_STATUS_ERROR_TIMEDOUT;
    if (error == EBUSY || error == EAGAIN)
        return VA_STATUS_ERROR_SURFACE_BUSY;
    if (error == ENOTSUP || error == EOPNOTSUPP)
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    if (error == EINVAL || error == EOVERFLOW || error == EBADF)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

static void trace_errno(const char *stage, int error) {
    if (getenv("ADVC_VAAPI_TRACE") != NULL)
        fprintf(stderr, "advc-vaapi-encode: %s errno=%d (%s)\n", stage,
                error, strerror(error));
}

static int frame_session_mode(void) {
    const char *gate = getenv("ADVC_VAAPI_ENCODE_FRAME_SESSION");
    return gate != NULL && strcmp(gate, "validated-v1") == 0;
}

static int exact_env(const char *name, const char *value) {
    const char *actual = getenv(name);
    return actual != NULL && strcmp(actual, value) == 0;
}

static void descriptor_init(struct advc_dmabuf_descriptor *descriptor) {
    uint32_t i;
    memset(descriptor, 0, sizeof(*descriptor));
    for (i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        descriptor->objects[i].fd = -1;
}

static int duplicate_descriptor(
    const struct advc_dmabuf_descriptor *source,
    struct advc_dmabuf_descriptor *destination) {
    uint32_t i;
    if (source == NULL || destination == NULL ||
        advc_dmabuf_descriptor_validate(source) < 0)
        return -1;
    *destination = *source;
    for (i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        destination->objects[i].fd = -1;
    for (i = 0; i < source->object_count; ++i) {
        destination->objects[i].fd =
            fcntl(source->objects[i].fd, F_DUPFD_CLOEXEC, 0);
        if (destination->objects[i].fd < 0) {
            advc_dmabuf_descriptor_close(destination);
            return -1;
        }
    }
    return 0;
}

static int profile_allowed(VAProfile profile) {
    /* These are the only two profiles in the current real-device gate. */
    return profile == VAProfileH264ConstrainedBaseline ||
           profile == VAProfileHEVCMain;
}

static uint32_t codec_max_width(
    const struct advc_vaapi_encode_runtime *runtime,
    enum advc_vaapi_encode_codec codec) {
    uint32_t value = codec == ADVC_VAAPI_ENCODE_CODEC_H264
                         ? runtime->policy.h264_max_width
                         : runtime->policy.hevc_max_width;
    return value == 0 ? ADVC_ENCODE_MAX_DIMENSION : value;
}

static uint32_t codec_max_height(
    const struct advc_vaapi_encode_runtime *runtime,
    enum advc_vaapi_encode_codec codec) {
    uint32_t value = codec == ADVC_VAAPI_ENCODE_CODEC_H264
                         ? runtime->policy.h264_max_height
                         : runtime->policy.hevc_max_height;
    return value == 0 ? ADVC_ENCODE_MAX_DIMENSION : value;
}

static int checked_allocation_size(unsigned int size,
                                   unsigned int num_elements,
                                   size_t *total) {
    if (size == 0 || num_elements == 0 ||
        (size_t)num_elements > SIZE_MAX / size)
        return -1;
    *total = (size_t)size * num_elements;
    return 0;
}

static int supported_buffer_type(VABufferType type) {
    return type == VAEncCodedBufferType ||
           type == VAEncSequenceParameterBufferType ||
           type == VAEncPictureParameterBufferType ||
           type == VAEncSliceParameterBufferType ||
           type == VAEncMiscParameterBufferType;
}

static size_t coded_output_size(
    const struct advc_vaapi_encode_coded_output *output) {
    size_t total = 0;
    uint32_t i;
    for (i = 0; i < output->count; ++i) {
        if (output->segments[i].size > SIZE_MAX - total) return SIZE_MAX;
        total += output->segments[i].size;
    }
    return total;
}

static uint64_t media_codec_pts_ns(uint64_t pts_ns) {
    return (pts_ns / UINT64_C(1000)) * UINT64_C(1000);
}

static struct advc_encode_surface_slot *find_oldest_pending_surface(
    struct advc_vaapi_encode_runtime *runtime,
    struct advc_encode_context_slot *context) {
    struct advc_encode_surface_slot *oldest = NULL;
    unsigned int i;
    for (i = 0; i < ADVC_ENCODE_SURFACE_SLOTS; ++i) {
        struct advc_encode_surface_slot *surface = &runtime->surfaces[i];
        if (!surface->used ||
            surface->state != ADVC_ENCODE_SURFACE_PENDING ||
            surface->owner_context == 0 ||
            get_context(runtime, surface->owner_context) != context)
            continue;
        if (oldest == NULL || surface->submit_index < oldest->submit_index)
            oldest = surface;
    }
    return oldest;
}

static void mark_context_pending_error(
    struct advc_vaapi_encode_runtime *runtime,
    struct advc_encode_context_slot *context) {
    unsigned int i;
    for (i = 0; i < ADVC_ENCODE_SURFACE_SLOTS; ++i) {
        struct advc_encode_surface_slot *surface = &runtime->surfaces[i];
        struct advc_encode_buffer_slot *coded;
        if (!surface->used ||
            surface->state != ADVC_ENCODE_SURFACE_PENDING ||
            surface->owner_context == 0 ||
            get_context(runtime, surface->owner_context) != context)
            continue;
        coded = get_buffer(runtime, surface->coded_buffer);
        if (coded != NULL) coded->pending_surface = VA_INVALID_SURFACE;
        surface->state = ADVC_ENCODE_SURFACE_ERROR;
    }
    context->pending_count = 0;
}

static VAStatus sync_surface_locked(
    struct advc_vaapi_encode_runtime *runtime,
    struct advc_encode_surface_slot *surface, uint32_t timeout_ms) {
    struct advc_encode_context_slot *context;
    if (surface->state == ADVC_ENCODE_SURFACE_READY)
        return VA_STATUS_SUCCESS;
    if (surface->state == ADVC_ENCODE_SURFACE_ERROR)
        return VA_STATUS_ERROR_ENCODING_ERROR;
    if (surface->state != ADVC_ENCODE_SURFACE_PENDING)
        return VA_STATUS_SUCCESS;
    context = get_context(runtime, surface->owner_context);
    if (context == NULL || !context->broker_open ||
        context->pending_count == 0) {
        surface->state = ADVC_ENCODE_SURFACE_ERROR;
        return VA_STATUS_ERROR_ENCODING_ERROR;
    }
    while (surface->state == ADVC_ENCODE_SURFACE_PENDING) {
        struct advc_vaapi_encode_coded_output output;
        struct advc_encode_surface_slot *completed;
        struct advc_encode_buffer_slot *coded;
        memset(&output, 0, sizeof(output));
        if (advc_vaapi_encode_broker_receive(&context->broker, timeout_ms,
                                              &output) < 0) {
            trace_errno("receive-coded", errno);
            if (errno == ETIMEDOUT) return VA_STATUS_ERROR_TIMEDOUT;
            mark_context_pending_error(runtime, context);
            return VA_STATUS_ERROR_ENCODING_ERROR;
        }
        if ((output.flags & ADVC_FLAG_END_OF_STREAM) != 0)
            context->broker_eos_received = 1;
        if (coded_output_size(&output) == 0) {
            advc_vaapi_encode_coded_output_close(&output);
            mark_context_pending_error(runtime, context);
            return VA_STATUS_ERROR_ENCODING_ERROR;
        }
        completed = find_oldest_pending_surface(runtime, context);
        if (completed == NULL) {
            if (getenv("ADVC_VAAPI_TRACE") != NULL) {
                unsigned int i;
                fprintf(stderr,
                        "advc-vaapi-encode: unmatched-output pts=%llu "
                        "pending=%u\n",
                        (unsigned long long)output.pts_ns,
                        context->pending_count);
                for (i = 0; i < ADVC_ENCODE_SURFACE_SLOTS; ++i) {
                    struct advc_encode_surface_slot *candidate =
                        &runtime->surfaces[i];
                    if (candidate->used &&
                        candidate->state == ADVC_ENCODE_SURFACE_PENDING &&
                        candidate->owner_context != 0 &&
                        get_context(runtime, candidate->owner_context) ==
                            context)
                        fprintf(stderr,
                                "advc-vaapi-encode: pending-surface id=%u "
                                "pts=%llu media-pts=%llu submit=%llu\n",
                                make_id(ADVC_VAAPI_ENCODE_SURFACE_TYPE, i),
                                (unsigned long long)candidate->pts_ns,
                                (unsigned long long)media_codec_pts_ns(
                                    candidate->pts_ns),
                                (unsigned long long)candidate->submit_index);
                }
            }
            advc_vaapi_encode_coded_output_close(&output);
            mark_context_pending_error(runtime, context);
            return VA_STATUS_ERROR_ENCODING_ERROR;
        }
        coded = get_buffer(runtime, completed->coded_buffer);
        if (coded == NULL || coded->type != VAEncCodedBufferType) {
            advc_vaapi_encode_coded_output_close(&output);
            mark_context_pending_error(runtime, context);
            return VA_STATUS_ERROR_ENCODING_ERROR;
        }
        if (advc_vaapi_encode_surface_link_wait(&completed->link,
                                                timeout_ms) < 0) {
            int saved = errno;
            advc_vaapi_encode_coded_output_close(&output);
            trace_errno("wait-release-fence", saved);
            if (saved == ETIMEDOUT) return VA_STATUS_ERROR_TIMEDOUT;
            mark_context_pending_error(runtime, context);
            return VA_STATUS_ERROR_ENCODING_ERROR;
        }
        if (coded_output_size(&output) > coded->coded_capacity) {
            advc_vaapi_encode_coded_output_close(&output);
            mark_context_pending_error(runtime, context);
            return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
        }
        advc_vaapi_encode_coded_output_close(&coded->output);
        coded->output = output;
        /* Android Vulkan swapchain presentation timestamps are absolute
         * display times, not MediaCodec packet PTS.  With B-frames disabled
         * by the encode contract, output order is input order; restore the
         * logical VA timestamp assigned at submission. */
        coded->output.pts_ns = media_codec_pts_ns(completed->pts_ns);
        {
            uint32_t i;
            for (i = 0; i < coded->output.count; ++i)
                coded->output.segments[i].next =
                    i + 1u < coded->output.count
                        ? &coded->output.segments[i + 1u]
                        : NULL;
        }
        coded->pending_surface = VA_INVALID_SURFACE;
        completed->state = ADVC_ENCODE_SURFACE_READY;
        if (context->pending_count > 0) --context->pending_count;
        if (getenv("ADVC_VAAPI_TRACE") != NULL)
            fprintf(stderr,
                    "advc-vaapi-encode: sync-complete pts=%llu "
                    "coded-segments=%u coded-bytes=%zu pending=%u\n",
                    (unsigned long long)coded->output.pts_ns,
                    coded->output.count, coded_output_size(&coded->output),
                    context->pending_count);
        if (context->broker_frame_session && context->pending_count == 0) {
            unsigned int i;
            advc_vaapi_encode_broker_close(&context->broker);
            context->broker_open = 0;
            for (i = 0; i < ADVC_ENCODE_SURFACE_SLOTS; ++i) {
                struct advc_encode_surface_slot *candidate =
                    &runtime->surfaces[i];
                if (candidate->used && candidate->owner_context != 0 &&
                    get_context(runtime, candidate->owner_context) == context)
                    candidate->link.registered = 0;
            }
        }
    }
    return surface->state == ADVC_ENCODE_SURFACE_READY
               ? VA_STATUS_SUCCESS
               : VA_STATUS_ERROR_ENCODING_ERROR;
}

static VAStatus image_acquire_surface(
    void *opaque, VASurfaceID id, enum advc_vaapi_surface_access access,
    struct advc_dmabuf_descriptor *descriptor, int *acquire_fence_fd) {
    struct advc_vaapi_encode_runtime *runtime = opaque;
    struct advc_encode_surface_slot *surface;
    VAStatus status = VA_STATUS_SUCCESS;
    if (runtime == NULL || descriptor == NULL || acquire_fence_fd == NULL)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    descriptor_init(descriptor);
    *acquire_fence_fd = -1;
    pthread_mutex_lock(&runtime->mutex);
    surface = get_surface(runtime, id);
    if (surface == NULL) {
        status = VA_STATUS_ERROR_INVALID_SURFACE;
        goto out;
    }
    if (surface->state == ADVC_ENCODE_SURFACE_PENDING) {
        if ((access & ADVC_VAAPI_SURFACE_ACCESS_WRITE) != 0) {
            status = VA_STATUS_ERROR_SURFACE_BUSY;
            goto out;
        }
        status = sync_surface_locked(runtime, surface,
                                     ADVC_VAAPI_IMAGE_SYNC_TIMEOUT_MS);
        if (status != VA_STATUS_SUCCESS) goto out;
    }
    if (surface->state == ADVC_ENCODE_SURFACE_ERROR) {
        status = VA_STATUS_ERROR_ENCODING_ERROR;
        goto out;
    }
    if (duplicate_descriptor(&surface->prime, descriptor) < 0)
        status = status_from_errno(errno);
out:
    pthread_mutex_unlock(&runtime->mutex);
    return status;
}

static void image_release_surface(void *opaque, VASurfaceID surface,
                                  enum advc_vaapi_surface_access access,
                                  int access_succeeded) {
    struct advc_vaapi_encode_runtime *runtime = opaque;
    struct advc_encode_surface_slot *slot;
    if (runtime == NULL || !access_succeeded ||
        (access & ADVC_VAAPI_SURFACE_ACCESS_WRITE) == 0)
        return;
    pthread_mutex_lock(&runtime->mutex);
    slot = get_surface(runtime, surface);
    if (slot != NULL) {
        ++slot->cpu_pixel_copies;
        ++slot->pending_cpu_pixel_copies;
    }
    pthread_mutex_unlock(&runtime->mutex);
}

struct advc_vaapi_encode_runtime *advc_vaapi_encode_runtime_create(
    const char *socket_path, const struct advc_vaapi_encode_policy *policy) {
    struct advc_vaapi_encode_runtime *runtime;
    struct advc_vaapi_image_surface_ops image_ops;
    unsigned int i;
    if (socket_path == NULL || socket_path[0] == '\0' || policy == NULL)
        return NULL;
    runtime = calloc(1, sizeof(*runtime));
    if (runtime == NULL) return NULL;
    runtime->socket_path = strdup(socket_path);
    if (runtime->socket_path == NULL ||
        pthread_mutex_init(&runtime->mutex, NULL) != 0) {
        free(runtime->socket_path);
        free(runtime);
        return NULL;
    }
    runtime->policy = *policy;
    if (advc_vaapi_modifier_policy_parse(
            getenv("ADVC_VAAPI_ENCODE_INPUT"),
            getenv("ADVC_VAAPI_OUTPUT"), &runtime->modifier_policy) < 0) {
        trace_errno("encode-input-policy", errno);
        pthread_mutex_destroy(&runtime->mutex);
        free(runtime->socket_path);
        free(runtime);
        return NULL;
    }
    runtime->write_export_ready =
        getenv("ADVC_VAAPI_ENABLE_WRITE_EXPORT") != NULL &&
        strcmp(getenv("ADVC_VAAPI_ENABLE_WRITE_EXPORT"),
               "validated-dmabuf-syncfile-v1") == 0;
    if (getenv("ADVC_VAAPI_TRACE") != NULL)
        fprintf(stderr,
                "advc-vaapi-encode: input-policy mode=%s source=%d "
                "qcom-import=%d gpu-linear-repack=%d\n",
                advc_vaapi_modifier_mode_name(runtime->modifier_policy.mode),
                (int)runtime->modifier_policy.source,
                exact_env("ADVC_VAAPI_ENCODE_QCOM_IMPORT", "validated-v1"),
                advc_vaapi_gpu_repack_linear != NULL &&
                    exact_env("ADVC_VAAPI_GPU_LINEAR_REPACK",
                              "validated-qcom-nv12-v1"));
    for (i = 0; i < ADVC_ENCODE_CONTEXT_SLOTS; ++i) {
        runtime->contexts[i].target = VA_INVALID_SURFACE;
        runtime->contexts[i].broker.fd = -1;
    }
    memset(&image_ops, 0, sizeof(image_ops));
    image_ops.opaque = runtime;
    image_ops.acquire_surface = image_acquire_surface;
    image_ops.release_surface = image_release_surface;
    runtime->images = advc_vaapi_image_runtime_create(&image_ops);
    if (runtime->images == NULL) {
        pthread_mutex_destroy(&runtime->mutex);
        free(runtime->socket_path);
        free(runtime);
        return NULL;
    }
    return runtime;
}

static void close_context_locked(struct advc_vaapi_encode_runtime *runtime,
                                 struct advc_encode_context_slot *context) {
    unsigned int i;
    for (i = 0; i < ADVC_ENCODE_SURFACE_SLOTS; ++i) {
        struct advc_encode_surface_slot *surface = &runtime->surfaces[i];
        if (!surface->used || surface->owner_context == 0 ||
            get_context(runtime, surface->owner_context) != context)
            continue;
        if (context->broker_open && surface->link.registered &&
            advc_vaapi_encode_surface_link_unregister(
                &surface->link, &context->broker,
                ADVC_VAAPI_IMAGE_SYNC_TIMEOUT_MS) < 0) {
            /* Registration belongs to the session being closed.  Do not let
             * a later context mistake a stale registration for its own. */
            surface->link.registered = 0;
            surface->state = ADVC_ENCODE_SURFACE_ERROR;
        }
        surface->owner_context = 0;
        surface->coded_buffer = VA_INVALID_ID;
        if (surface->state == ADVC_ENCODE_SURFACE_PENDING)
            surface->state = ADVC_ENCODE_SURFACE_ERROR;
    }
    if (context->broker_open)
        advc_vaapi_encode_broker_close(&context->broker);
    memset(context, 0, sizeof(*context));
    context->target = VA_INVALID_SURFACE;
    context->broker.fd = -1;
}

struct context_eos_adapter {
    struct advc_vaapi_encode_runtime *runtime;
    struct advc_encode_context_slot *context;
};

static uint64_t context_eos_now_ms(void *opaque) {
    struct timespec now;
    (void)opaque;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return UINT64_MAX;
    if ((uint64_t)now.tv_sec >
        (UINT64_MAX - (uint64_t)now.tv_nsec / UINT64_C(1000000)) /
            UINT64_C(1000)) {
        errno = EOVERFLOW;
        return UINT64_MAX;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000) +
           (uint64_t)now.tv_nsec / UINT64_C(1000000);
}

static int context_eos_signal(void *opaque, uint64_t pts_ns) {
    struct context_eos_adapter *adapter = opaque;
    return advc_vaapi_encode_broker_signal_eos(&adapter->context->broker,
                                                pts_ns);
}

static int status_to_errno(VAStatus status) {
    if (status == VA_STATUS_ERROR_TIMEDOUT) return ETIMEDOUT;
    if (status == VA_STATUS_ERROR_SURFACE_BUSY) return EBUSY;
    if (status == VA_STATUS_ERROR_NOT_ENOUGH_BUFFER) return ENOSPC;
    return EIO;
}

static int context_eos_drain_once(void *opaque, uint32_t timeout_ms,
                                  int *eos_received) {
    struct context_eos_adapter *adapter = opaque;
    struct advc_vaapi_encode_runtime *runtime = adapter->runtime;
    struct advc_encode_context_slot *context = adapter->context;
    if (context->pending_count > 0) {
        struct advc_encode_surface_slot *surface =
            find_oldest_pending_surface(runtime, context);
        VAStatus status;
        if (surface == NULL) {
            errno = EPROTO;
            return -1;
        }
        status = sync_surface_locked(runtime, surface, timeout_ms);
        if (status != VA_STATUS_SUCCESS) {
            errno = status_to_errno(status);
            return -1;
        }
        if (context->broker_eos_received && context->pending_count != 0) {
            errno = EPROTO;
            return -1;
        }
        *eos_received = context->broker_eos_received;
        return 0;
    }
    {
        struct advc_vaapi_encode_coded_output output;
        memset(&output, 0, sizeof(output));
        if (advc_vaapi_encode_broker_receive(&context->broker, timeout_ms,
                                              &output) < 0)
            return -1;
        if ((output.flags & ADVC_FLAG_END_OF_STREAM) == 0) {
            advc_vaapi_encode_coded_output_close(&output);
            errno = EPROTO;
            return -1;
        }
        context->broker_eos_received = 1;
        *eos_received = 1;
        advc_vaapi_encode_coded_output_close(&output);
    }
    return 0;
}

static void context_eos_fail_closed(void *opaque) {
    struct context_eos_adapter *adapter = opaque;
    mark_context_pending_error(adapter->runtime, adapter->context);
}

static VAStatus drain_frame_session_locked(
    struct advc_vaapi_encode_runtime *runtime,
    struct advc_encode_context_slot *context) {
    uint64_t start = context_eos_now_ms(NULL);
    uint64_t deadline;
    if (start == UINT64_MAX ||
        UINT64_MAX - start < ADVC_VAAPI_IMAGE_SYNC_TIMEOUT_MS) {
        mark_context_pending_error(runtime, context);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    deadline = start + ADVC_VAAPI_IMAGE_SYNC_TIMEOUT_MS;
    while (context->pending_count > 0) {
        struct advc_encode_surface_slot *surface =
            find_oldest_pending_surface(runtime, context);
        uint64_t now = context_eos_now_ms(NULL);
        uint64_t remaining;
        VAStatus status;
        if (surface == NULL || now == UINT64_MAX || now >= deadline) {
            mark_context_pending_error(runtime, context);
            return surface == NULL ? VA_STATUS_ERROR_ENCODING_ERROR
                                   : VA_STATUS_ERROR_TIMEDOUT;
        }
        remaining = deadline - now;
        status = sync_surface_locked(
            runtime, surface,
            remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining);
        if (status != VA_STATUS_SUCCESS) {
            mark_context_pending_error(runtime, context);
            return status;
        }
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus finish_context_locked(
    struct advc_vaapi_encode_runtime *runtime,
    struct advc_encode_context_slot *context) {
    struct context_eos_adapter adapter;
    struct advc_vaapi_encode_eos_ops ops;
    int saved;
    if (!context->broker_open) return VA_STATUS_SUCCESS;
    if (context->broker_frame_session)
        return drain_frame_session_locked(runtime, context);
    memset(&adapter, 0, sizeof(adapter));
    adapter.runtime = runtime;
    adapter.context = context;
    memset(&ops, 0, sizeof(ops));
    ops.opaque = &adapter;
    ops.now_ms = context_eos_now_ms;
    ops.signal_eos = context_eos_signal;
    ops.drain_once = context_eos_drain_once;
    ops.fail_closed = context_eos_fail_closed;
    if (advc_vaapi_encode_eos_finish(
            &context->eos, context->last_pts_ns,
            ADVC_VAAPI_IMAGE_SYNC_TIMEOUT_MS, &ops) == 0)
        return VA_STATUS_SUCCESS;
    saved = errno;
    trace_errno("finish-context-eos", saved);
    return status_from_errno(saved);
}

void advc_vaapi_encode_runtime_destroy(
    struct advc_vaapi_encode_runtime *runtime) {
    unsigned int i;
    if (runtime == NULL) return;
    advc_vaapi_image_runtime_destroy(runtime->images);
    pthread_mutex_lock(&runtime->mutex);
    for (i = 0; i < ADVC_ENCODE_CONTEXT_SLOTS; ++i) {
        if (runtime->contexts[i].used) {
            (void)finish_context_locked(runtime, &runtime->contexts[i]);
            close_context_locked(runtime, &runtime->contexts[i]);
        }
    }
    for (i = 0; i < ADVC_ENCODE_BUFFER_SLOTS; ++i) {
        free(runtime->buffers[i].data);
        advc_vaapi_encode_coded_output_close(&runtime->buffers[i].output);
    }
    for (i = 0; i < ADVC_ENCODE_SURFACE_SLOTS; ++i) {
        advc_vaapi_encode_surface_link_close(&runtime->surfaces[i].link);
        advc_dmabuf_descriptor_close(&runtime->surfaces[i].broker_prime);
        advc_dmabuf_descriptor_close(&runtime->surfaces[i].prime);
    }
    pthread_mutex_unlock(&runtime->mutex);
    pthread_mutex_destroy(&runtime->mutex);
    free(runtime->socket_path);
    free(runtime);
}

struct advc_vaapi_image_runtime *advc_vaapi_encode_image_runtime(
    struct advc_vaapi_encode_runtime *runtime) {
    return runtime == NULL ? NULL : runtime->images;
}

VAStatus advc_vaapi_encode_create_config(
    struct advc_vaapi_encode_runtime *runtime, VAProfile profile,
    VAEntrypoint entrypoint, VAConfigAttrib *attributes, int num_attributes,
    VAConfigID *config_id) {
    struct advc_vaapi_encode_config config;
    unsigned int i;
    if (runtime == NULL || config_id == NULL)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!profile_allowed(profile) ||
        advc_vaapi_encode_config_init(&runtime->policy, profile, entrypoint,
                                      attributes, num_attributes,
                                      &config) < 0)
        return entrypoint == VAEntrypointEncSlice
                   ? VA_STATUS_ERROR_UNSUPPORTED_PROFILE
                   : VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    pthread_mutex_lock(&runtime->mutex);
    for (i = 0; i < ADVC_ENCODE_CONFIG_SLOTS; ++i)
        if (!runtime->configs[i].used) break;
    if (i == ADVC_ENCODE_CONFIG_SLOTS) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }
    runtime->configs[i].used = 1;
    runtime->configs[i].config = config;
    *config_id = make_id(ADVC_VAAPI_ENCODE_CONFIG_TYPE, i);
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_encode_destroy_config(
    struct advc_vaapi_encode_runtime *runtime, VAConfigID config_id) {
    struct advc_encode_config_slot *config;
    unsigned int i;
    if (runtime == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    pthread_mutex_lock(&runtime->mutex);
    config = get_config(runtime, config_id);
    if (config == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    for (i = 0; i < ADVC_ENCODE_CONTEXT_SLOTS; ++i) {
        if (runtime->contexts[i].used &&
            runtime->contexts[i].config_id == config_id) {
            pthread_mutex_unlock(&runtime->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }
    memset(config, 0, sizeof(*config));
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_encode_query_config(
    struct advc_vaapi_encode_runtime *runtime, VAConfigID config_id,
    VAProfile *profile, VAEntrypoint *entrypoint, VAConfigAttrib *attributes,
    int *num_attributes) {
    struct advc_encode_config_slot *config;
    if (runtime == NULL || profile == NULL || entrypoint == NULL ||
        num_attributes == NULL)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&runtime->mutex);
    config = get_config(runtime, config_id);
    if (config == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    *profile = config->config.profile;
    *entrypoint = config->config.entrypoint;
    if (attributes != NULL) {
        attributes[0].type = VAConfigAttribRTFormat;
        attributes[0].value = config->config.rt_format;
        attributes[1].type = VAConfigAttribRateControl;
        attributes[1].value = config->config.rate_control;
    }
    *num_attributes = 2;
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

static int surface_has_prime_attributes(const VASurfaceAttrib *attributes,
                                        unsigned int num_attributes) {
    unsigned int i;
    for (i = 0; i < num_attributes; ++i) {
        if (attributes[i].type == VASurfaceAttribMemoryType &&
            attributes[i].value.type == VAGenericValueTypeInteger &&
            (uint32_t)attributes[i].value.value.i ==
                VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)
            return 1;
    }
    return 0;
}

VAStatus advc_vaapi_encode_create_surfaces(
    struct advc_vaapi_encode_runtime *runtime, unsigned int format,
    unsigned int width, unsigned int height, VASurfaceID *surfaces,
    unsigned int num_surfaces, VASurfaceAttrib *attributes,
    unsigned int num_attributes) {
    unsigned int allocated[ADVC_ENCODE_SURFACE_SLOTS];
    unsigned int allocated_count = 0;
    int import_prime;
    unsigned int i;
    unsigned int j;
    VAStatus status = VA_STATUS_SUCCESS;
    if (runtime == NULL || surfaces == NULL || num_surfaces == 0 ||
        num_surfaces > ADVC_ENCODE_SURFACE_SLOTS ||
        format != VA_RT_FORMAT_YUV420 || width < 16 || height < 16 ||
        width > ADVC_ENCODE_MAX_DIMENSION ||
        height > ADVC_ENCODE_MAX_DIMENSION || (width & 1u) != 0 ||
        (height & 1u) != 0 ||
        (num_attributes > 0 && attributes == NULL))
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    import_prime = surface_has_prime_attributes(attributes, num_attributes);
    for (j = 0; j < num_attributes; ++j) {
        const VASurfaceAttrib *attribute = &attributes[j];
        if (attribute->type == VASurfaceAttribPixelFormat) {
            if (attribute->value.type != VAGenericValueTypeInteger ||
                (uint32_t)attribute->value.value.i != VA_FOURCC_NV12)
                return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        } else if (attribute->type == VASurfaceAttribMemoryType) {
            uint32_t memory_type;
            if (attribute->value.type != VAGenericValueTypeInteger)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            memory_type = (uint32_t)attribute->value.value.i;
            if (memory_type != VA_SURFACE_ATTRIB_MEM_TYPE_VA &&
                memory_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)
                return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        } else if (attribute->type == VASurfaceAttribUsageHint) {
            uint32_t usage;
            if (attribute->value.type != VAGenericValueTypeInteger)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            usage = (uint32_t)attribute->value.value.i;
            if (usage != VA_SURFACE_ATTRIB_USAGE_HINT_GENERIC &&
                (usage & VA_SURFACE_ATTRIB_USAGE_HINT_ENCODER) == 0)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
        } else if (attribute->type ==
                   VASurfaceAttribExternalBufferDescriptor) {
            if (!import_prime ||
                attribute->value.type != VAGenericValueTypePointer ||
                attribute->value.value.p == NULL)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
        } else if (attribute->type != VASurfaceAttribNone) {
            return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
        }
    }
    pthread_mutex_lock(&runtime->mutex);
    for (i = 0; i < ADVC_ENCODE_SURFACE_SLOTS &&
                allocated_count < num_surfaces;
         ++i) {
        struct advc_encode_surface_slot *surface;
        VASurfaceID id;
        if (runtime->surfaces[i].used) continue;
        surface = &runtime->surfaces[i];
        memset(surface, 0, sizeof(*surface));
        descriptor_init(&surface->prime);
        descriptor_init(&surface->broker_prime);
        id = make_id(ADVC_VAAPI_ENCODE_SURFACE_TYPE, i);
        if (import_prime) {
            status = advc_vaapi_prime_import_surface_attributes(
                format, width, height, attributes, num_attributes,
                allocated_count, num_surfaces, id, &surface->prime);
        } else if (num_attributes == 0 || attributes != NULL) {
            status = advc_vaapi_encode_surface_allocate_linear(
                         id, width, height, &surface->prime) == 0
                         ? VA_STATUS_SUCCESS
                         : status_from_errno(errno);
        }
        if (status != VA_STATUS_SUCCESS) break;
        if (import_prime && surface->prime.drm_modifier != 0 &&
            surface->prime.drm_modifier != ADVC_VAAPI_QCOM_MODIFIER)
            status = VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        if (import_prime &&
            runtime->modifier_policy.mode == ADVC_VAAPI_MODIFIER_LINEAR &&
            surface->prime.drm_modifier != 0)
            status = VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        if (import_prime &&
            runtime->modifier_policy.mode == ADVC_VAAPI_MODIFIER_QCOM &&
            surface->prime.drm_modifier != ADVC_VAAPI_QCOM_MODIFIER)
            status = VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        if (status != VA_STATUS_SUCCESS) {
            advc_dmabuf_descriptor_close(&surface->prime);
            break;
        }
        surface->used = 1;
        surface->external_import = import_prime;
        surface->width = width;
        surface->height = height;
        surface->state = ADVC_ENCODE_SURFACE_IDLE;
        surface->coded_buffer = VA_INVALID_ID;
        advc_vaapi_encode_surface_link_init(&surface->link,
                                            &surface->prime);
        allocated[allocated_count] = i;
        surfaces[allocated_count++] = id;
    }
    if (allocated_count != num_surfaces) {
        if (status == VA_STATUS_SUCCESS)
            status = VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
        for (i = 0; i < allocated_count; ++i) {
            struct advc_encode_surface_slot *surface =
                &runtime->surfaces[allocated[i]];
            advc_vaapi_encode_surface_link_close(&surface->link);
            advc_dmabuf_descriptor_close(&surface->broker_prime);
            advc_dmabuf_descriptor_close(&surface->prime);
            memset(surface, 0, sizeof(*surface));
        }
    }
    pthread_mutex_unlock(&runtime->mutex);
    return status;
}

VAStatus advc_vaapi_encode_destroy_surfaces(
    struct advc_vaapi_encode_runtime *runtime, VASurfaceID *surfaces,
    int num_surfaces) {
    int i;
    if (runtime == NULL || num_surfaces < 0 ||
        (num_surfaces > 0 && surfaces == NULL))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&runtime->mutex);
    for (i = 0; i < num_surfaces; ++i) {
        struct advc_encode_surface_slot *surface =
            get_surface(runtime, surfaces[i]);
        if (surface == NULL) {
            pthread_mutex_unlock(&runtime->mutex);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        if (surface->state == ADVC_ENCODE_SURFACE_PENDING ||
            surface->link.registered) {
            pthread_mutex_unlock(&runtime->mutex);
            return VA_STATUS_ERROR_SURFACE_BUSY;
        }
    }
    for (i = 0; i < num_surfaces; ++i) {
        struct advc_encode_surface_slot *surface =
            get_surface(runtime, surfaces[i]);
        advc_vaapi_encode_surface_link_close(&surface->link);
        advc_dmabuf_descriptor_close(&surface->broker_prime);
        advc_dmabuf_descriptor_close(&surface->prime);
        memset(surface, 0, sizeof(*surface));
    }
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

static void set_surface_attribute(VASurfaceAttrib *attribute,
                                  VASurfaceAttribType type, int value,
                                  uint32_t flags) {
    memset(attribute, 0, sizeof(*attribute));
    attribute->type = type;
    attribute->flags = flags;
    attribute->value.type = VAGenericValueTypeInteger;
    attribute->value.value.i = value;
}

VAStatus advc_vaapi_encode_query_surface_attributes(
    struct advc_vaapi_encode_runtime *runtime, VAConfigID config_id,
    VASurfaceAttrib *attributes, unsigned int *num_attributes) {
    const unsigned int needed = 7;
    struct advc_encode_config_slot *config;
    uint32_t max_width;
    uint32_t max_height;
    if (runtime == NULL || num_attributes == NULL)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&runtime->mutex);
    config = get_config(runtime, config_id);
    if (config == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    if (attributes == NULL) {
        *num_attributes = needed;
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_SUCCESS;
    }
    if (*num_attributes < needed) {
        *num_attributes = needed;
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }
    max_width = codec_max_width(runtime, config->config.codec);
    max_height = codec_max_height(runtime, config->config.codec);
    set_surface_attribute(&attributes[0], VASurfaceAttribPixelFormat,
                          (int)VA_FOURCC_NV12,
                          VA_SURFACE_ATTRIB_GETTABLE |
                              VA_SURFACE_ATTRIB_SETTABLE);
    set_surface_attribute(&attributes[1], VASurfaceAttribMinWidth, 16,
                          VA_SURFACE_ATTRIB_GETTABLE);
    set_surface_attribute(&attributes[2], VASurfaceAttribMaxWidth,
                          (int)max_width, VA_SURFACE_ATTRIB_GETTABLE);
    set_surface_attribute(&attributes[3], VASurfaceAttribMinHeight, 16,
                          VA_SURFACE_ATTRIB_GETTABLE);
    set_surface_attribute(&attributes[4], VASurfaceAttribMaxHeight,
                          (int)max_height, VA_SURFACE_ATTRIB_GETTABLE);
    set_surface_attribute(&attributes[5], VASurfaceAttribMemoryType,
                          VA_SURFACE_ATTRIB_MEM_TYPE_VA |
                              VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                          VA_SURFACE_ATTRIB_GETTABLE |
                              VA_SURFACE_ATTRIB_SETTABLE);
    set_surface_attribute(&attributes[6], VASurfaceAttribUsageHint,
                          VA_SURFACE_ATTRIB_USAGE_HINT_ENCODER,
                          VA_SURFACE_ATTRIB_GETTABLE |
                              VA_SURFACE_ATTRIB_SETTABLE);
    *num_attributes = needed;
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_encode_create_context(
    struct advc_vaapi_encode_runtime *runtime, VAConfigID config_id, int width,
    int height, int flag, VASurfaceID *targets, int num_targets,
    VAContextID *context_id) {
    struct advc_encode_config_slot *config;
    unsigned int i;
    (void)flag;
    if (runtime == NULL || context_id == NULL || width < 16 || height < 16 ||
        (width & 1) != 0 || (height & 1) != 0 || num_targets < 0 ||
        (num_targets > 0 && targets == NULL))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&runtime->mutex);
    config = get_config(runtime, config_id);
    if (config == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    if ((uint32_t)width > codec_max_width(runtime, config->config.codec) ||
        (uint32_t)height > codec_max_height(runtime, config->config.codec)) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    }
    for (i = 0; i < (unsigned int)num_targets; ++i) {
        struct advc_encode_surface_slot *surface =
            get_surface(runtime, targets[i]);
        if (surface == NULL || surface->width != (uint32_t)width ||
            surface->height != (uint32_t)height) {
            pthread_mutex_unlock(&runtime->mutex);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
    }
    for (i = 0; i < ADVC_ENCODE_CONTEXT_SLOTS; ++i)
        if (!runtime->contexts[i].used) break;
    if (i == ADVC_ENCODE_CONTEXT_SLOTS) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }
    memset(&runtime->contexts[i], 0, sizeof(runtime->contexts[i]));
    runtime->contexts[i].used = 1;
    runtime->contexts[i].config_id = config_id;
    runtime->contexts[i].width = (uint32_t)width;
    runtime->contexts[i].height = (uint32_t)height;
    runtime->contexts[i].target = VA_INVALID_SURFACE;
    runtime->contexts[i].broker.fd = -1;
    runtime->contexts[i].encode_profile =
        wire_encode_profile(config->config.profile);
    if (runtime->contexts[i].encode_profile == ADVC_ENCODE_PROFILE_NONE) {
        memset(&runtime->contexts[i], 0, sizeof(runtime->contexts[i]));
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    advc_vaapi_encode_frame_init(&runtime->contexts[i].frame,
                                 config->config.codec, (uint32_t)width,
                                 (uint32_t)height,
                                 ADVC_ENCODE_DEFAULT_BITRATE,
                                 ADVC_ENCODE_DEFAULT_FPS_MILLI);
    *context_id = make_id(ADVC_VAAPI_ENCODE_CONTEXT_TYPE, i);
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_encode_destroy_context(
    struct advc_vaapi_encode_runtime *runtime, VAContextID context_id) {
    struct advc_encode_context_slot *context;
    VAStatus status;
    if (runtime == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    pthread_mutex_lock(&runtime->mutex);
    context = get_context(runtime, context_id);
    if (context == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    status = finish_context_locked(runtime, context);
    close_context_locked(runtime, context);
    pthread_mutex_unlock(&runtime->mutex);
    return status;
}

VAStatus advc_vaapi_encode_create_buffer(
    struct advc_vaapi_encode_runtime *runtime, VAContextID context_id,
    VABufferType type, unsigned int size, unsigned int num_elements, void *data,
    VABufferID *buffer_id) {
    struct advc_encode_buffer_slot *buffer;
    size_t total;
    unsigned int i;
    if (runtime == NULL || buffer_id == NULL ||
        checked_allocation_size(size, num_elements, &total) < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!supported_buffer_type(type))
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    pthread_mutex_lock(&runtime->mutex);
    if (get_context(runtime, context_id) == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    for (i = 0; i < ADVC_ENCODE_BUFFER_SLOTS; ++i)
        if (!runtime->buffers[i].used) break;
    if (i == ADVC_ENCODE_BUFFER_SLOTS) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }
    buffer = &runtime->buffers[i];
    memset(buffer, 0, sizeof(*buffer));
    buffer->used = 1;
    buffer->owner_context = context_id;
    buffer->type = type;
    buffer->element_size = size;
    buffer->capacity_elements = num_elements;
    buffer->num_elements = num_elements;
    buffer->pending_surface = VA_INVALID_SURFACE;
    if (type == VAEncCodedBufferType) {
        buffer->coded_capacity = total;
    } else {
        buffer->data = calloc(1, total);
        if (buffer->data == NULL) {
            memset(buffer, 0, sizeof(*buffer));
            pthread_mutex_unlock(&runtime->mutex);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        if (data != NULL) memcpy(buffer->data, data, total);
    }
    *buffer_id = make_id(ADVC_VAAPI_ENCODE_BUFFER_TYPE, i);
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_encode_buffer_set_num_elements(
    struct advc_vaapi_encode_runtime *runtime, VABufferID buffer_id,
    unsigned int num_elements) {
    struct advc_encode_buffer_slot *buffer;
    if (runtime == NULL || num_elements == 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&runtime->mutex);
    buffer = get_buffer(runtime, buffer_id);
    if (buffer == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (buffer->mapped || buffer->type == VAEncCodedBufferType ||
        num_elements > buffer->capacity_elements) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    buffer->num_elements = num_elements;
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_encode_map_buffer(
    struct advc_vaapi_encode_runtime *runtime, VABufferID buffer_id,
    void **mapped) {
    struct advc_encode_buffer_slot *buffer;
    VAStatus status;
    if (runtime == NULL || mapped == NULL)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    *mapped = NULL;
    pthread_mutex_lock(&runtime->mutex);
    buffer = get_buffer(runtime, buffer_id);
    if (buffer == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (buffer->mapped) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (buffer->type == VAEncCodedBufferType) {
        if (buffer->pending_surface != VA_INVALID_SURFACE) {
            struct advc_encode_surface_slot *surface =
                get_surface(runtime, buffer->pending_surface);
            if (surface == NULL) {
                pthread_mutex_unlock(&runtime->mutex);
                return VA_STATUS_ERROR_ENCODING_ERROR;
            }
            status = sync_surface_locked(runtime, surface,
                                          ADVC_VAAPI_IMAGE_SYNC_TIMEOUT_MS);
            if (status != VA_STATUS_SUCCESS) {
                pthread_mutex_unlock(&runtime->mutex);
                return status;
            }
        }
        if (buffer->output.count == 0) {
            pthread_mutex_unlock(&runtime->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        *mapped = &buffer->output.segments[0];
    } else {
        *mapped = buffer->data;
    }
    buffer->mapped = 1;
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_encode_unmap_buffer(
    struct advc_vaapi_encode_runtime *runtime, VABufferID buffer_id) {
    struct advc_encode_buffer_slot *buffer;
    if (runtime == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    pthread_mutex_lock(&runtime->mutex);
    buffer = get_buffer(runtime, buffer_id);
    if (buffer == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (!buffer->mapped) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    buffer->mapped = 0;
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_encode_destroy_buffer(
    struct advc_vaapi_encode_runtime *runtime, VABufferID buffer_id) {
    struct advc_encode_buffer_slot *buffer;
    if (runtime == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    pthread_mutex_lock(&runtime->mutex);
    buffer = get_buffer(runtime, buffer_id);
    if (buffer == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (buffer->mapped || buffer->pending_surface != VA_INVALID_SURFACE) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    free(buffer->data);
    advc_vaapi_encode_coded_output_close(&buffer->output);
    memset(buffer, 0, sizeof(*buffer));
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_encode_begin_picture(
    struct advc_vaapi_encode_runtime *runtime, VAContextID context_id,
    VASurfaceID target) {
    struct advc_encode_context_slot *context;
    struct advc_encode_surface_slot *surface;
    if (runtime == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    pthread_mutex_lock(&runtime->mutex);
    context = get_context(runtime, context_id);
    surface = get_surface(runtime, target);
    if (context == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (surface == NULL || surface->width != context->width ||
        surface->height != context->height) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (surface->state == ADVC_ENCODE_SURFACE_ERROR) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_ENCODING_ERROR;
    }
    if (context->picture_active ||
        (frame_session_mode() && context->pending_count != 0) ||
        context->pending_count >= ADVC_MAX_INFLIGHT_DMABUFS ||
        surface->state == ADVC_ENCODE_SURFACE_PENDING ||
        (surface->owner_context != 0 &&
         surface->owner_context != context_id)) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    advc_vaapi_encode_frame_begin(&context->frame);
    context->picture_active = 1;
    context->target = target;
    surface->state = ADVC_ENCODE_SURFACE_IDLE;
    surface->coded_buffer = VA_INVALID_ID;
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_encode_render_picture(
    struct advc_vaapi_encode_runtime *runtime, VAContextID context_id,
    VABufferID *buffers, int num_buffers) {
    struct advc_encode_context_slot *context;
    int i;
    if (runtime == NULL || num_buffers < 0 ||
        (num_buffers > 0 && buffers == NULL))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&runtime->mutex);
    context = get_context(runtime, context_id);
    if (context == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (!context->picture_active) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    for (i = 0; i < num_buffers; ++i) {
        struct advc_encode_buffer_slot *buffer =
            get_buffer(runtime, buffers[i]);
        if (buffer == NULL || buffer->owner_context != context_id ||
            buffer->mapped ||
            buffer->type == VAEncCodedBufferType || buffer->data == NULL) {
            pthread_mutex_unlock(&runtime->mutex);
            return buffer == NULL ? VA_STATUS_ERROR_INVALID_BUFFER
                                  : VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        if (advc_vaapi_encode_frame_consume(
                &context->frame, buffer->type, buffer->data,
                buffer->element_size, buffer->num_elements) < 0) {
            pthread_mutex_unlock(&runtime->mutex);
            return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
        }
    }
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

static uint64_t frame_pts_ns(uint64_t frame_index, uint32_t fps_milli) {
    if (fps_milli == 0 || frame_index > UINT64_MAX / UINT64_C(1000000000000))
        return UINT64_MAX;
    return frame_index * UINT64_C(1000000000000) / fps_milli;
}

static int same_dmabuf_object(const struct advc_dmabuf_descriptor *a,
                              const struct advc_dmabuf_descriptor *b) {
    struct stat a_stat;
    struct stat b_stat;
    if (a == NULL || b == NULL || a->object_count != 1 ||
        b->object_count != 1 || a->objects[0].fd < 0 ||
        b->objects[0].fd < 0 || fstat(a->objects[0].fd, &a_stat) < 0 ||
        fstat(b->objects[0].fd, &b_stat) < 0)
        return 0;
    return a_stat.st_dev == b_stat.st_dev && a_stat.st_ino == b_stat.st_ino;
}

static int prepare_encode_input(
    struct advc_vaapi_encode_runtime *runtime,
    struct advc_encode_context_slot *context,
    struct advc_encode_surface_slot *surface) {
    enum advc_vaapi_modifier_route route;
    struct advc_dmabuf_descriptor converted;
    int source_acquire_fence = -1;
    int converted_acquire_fence = -1;
    int source_release_fence = -1;
    int saved;

    if (surface->prime.drm_modifier == 0) return 0;
    route = advc_vaapi_modifier_route_encode(
        &runtime->modifier_policy, surface->prime.drm_modifier,
        advc_dmabuf_descriptor_validate(&surface->prime) == 0,
        (context->broker.features &
         ADVC_FEATURE_ENCODE_QCOM_MODIFIER) != 0,
        exact_env("ADVC_VAAPI_ENCODE_QCOM_IMPORT", "validated-v1"),
        advc_vaapi_gpu_repack_linear != NULL &&
            exact_env("ADVC_VAAPI_GPU_LINEAR_REPACK",
                      "validated-qcom-nv12-v1"));
    if (getenv("ADVC_VAAPI_TRACE") != NULL)
        fprintf(stderr,
                "advc-vaapi-encode: modifier-route mode=%s "
                "modifier=0x%016llx broker-qcom=%d route=%d\n",
                advc_vaapi_modifier_mode_name(runtime->modifier_policy.mode),
                (unsigned long long)surface->prime.drm_modifier,
                (context->broker.features &
                 ADVC_FEATURE_ENCODE_QCOM_MODIFIER) != 0,
                (int)route);
    if (route == ADVC_VAAPI_MODIFIER_ROUTE_DIRECT_QCOM) {
        source_acquire_fence = export_implicit_read_fence(&surface->prime);
        if (source_acquire_fence < 0 ||
            advc_vaapi_encode_surface_link_set_acquire_fence(
                &surface->link, source_acquire_fence) < 0) {
            saved = errno;
            if (source_acquire_fence >= 0) close(source_acquire_fence);
            errno = saved;
            return -1;
        }
        return 0;
    }
    if (route != ADVC_VAAPI_MODIFIER_ROUTE_GPU_REPACK_LINEAR) {
        errno = ENOTSUP;
        return -1;
    }
    descriptor_init(&converted);
    source_acquire_fence = export_implicit_read_fence(&surface->prime);
    if (source_acquire_fence < 0 ||
        advc_vaapi_gpu_repack_linear(
            &surface->prime, source_acquire_fence, &converted,
            &converted_acquire_fence, &source_release_fence) < 0)
        goto fail;
    close(source_acquire_fence);
    source_acquire_fence = -1;
    if (converted.drm_modifier != 0 ||
        converted.drm_fourcc != VA_FOURCC_NV12 ||
        advc_dmabuf_descriptor_validate(&converted) < 0) {
        errno = EPROTO;
        goto fail;
    }
    if (!surface->broker_prime_valid ||
        !same_dmabuf_object(&surface->broker_prime, &converted)) {
        if (surface->link.registered &&
            advc_vaapi_encode_surface_link_unregister(
                &surface->link, &context->broker,
                ADVC_VAAPI_IMAGE_SYNC_TIMEOUT_MS) < 0)
            goto fail;
        advc_vaapi_encode_surface_link_close(&surface->link);
        advc_dmabuf_descriptor_close(&surface->broker_prime);
        surface->broker_prime = converted;
        descriptor_init(&converted);
        surface->broker_prime_valid = 1;
        advc_vaapi_encode_surface_link_init(&surface->link,
                                            &surface->broker_prime);
    }
    if (advc_vaapi_encode_surface_link_set_acquire_fence(
            &surface->link, converted_acquire_fence) < 0)
        goto fail;
    converted_acquire_fence = -1;
    if (source_release_fence >= 0) close(source_release_fence);
    advc_dmabuf_descriptor_close(&converted);
    ++surface->gpu_modifier_repack_submits;
    return 0;
fail:
    saved = errno == 0 ? EIO : errno;
    if (source_acquire_fence >= 0) close(source_acquire_fence);
    if (converted_acquire_fence >= 0) close(converted_acquire_fence);
    if (source_release_fence >= 0) close(source_release_fence);
    advc_dmabuf_descriptor_close(&converted);
    errno = saved;
    return -1;
}

VAStatus advc_vaapi_encode_end_picture(
    struct advc_vaapi_encode_runtime *runtime, VAContextID context_id) {
    struct advc_encode_context_slot *context;
    struct advc_encode_surface_slot *surface;
    struct advc_encode_buffer_slot *coded;
    uint64_t pts_ns;
    if (runtime == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    pthread_mutex_lock(&runtime->mutex);
    context = get_context(runtime, context_id);
    if (context == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (!context->picture_active) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    surface = get_surface(runtime, context->target);
    if (surface == NULL ||
        advc_vaapi_encode_frame_validate(&context->frame) < 0) {
        context->picture_active = 0;
        context->target = VA_INVALID_SURFACE;
        pthread_mutex_unlock(&runtime->mutex);
        return surface == NULL ? VA_STATUS_ERROR_INVALID_SURFACE
                               : VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    coded = get_buffer(runtime, context->frame.coded_buffer);
    if (coded == NULL || coded->owner_context != context_id ||
        coded->type != VAEncCodedBufferType ||
        coded->mapped || coded->pending_surface != VA_INVALID_SURFACE) {
        context->picture_active = 0;
        context->target = VA_INVALID_SURFACE;
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (!context->broker_open) {
        if (advc_vaapi_encode_broker_open(
                &context->broker, runtime->socket_path, context->frame.codec,
                context->encode_profile, context->width, context->height,
                context->frame.bitrate,
                context->frame.framerate_milli) < 0) {
            trace_errno("broker-open", errno);
            context->picture_active = 0;
            context->target = VA_INVALID_SURFACE;
            pthread_mutex_unlock(&runtime->mutex);
            return status_from_errno(errno);
        }
        context->broker_open = 1;
        context->broker_frame_session = frame_session_mode();
        context->broker_eos_received = 0;
        memset(&context->eos, 0, sizeof(context->eos));
    } else if (context->broker.config.bitrate != context->frame.bitrate ||
               context->broker.config.framerate_milli !=
                   context->frame.framerate_milli) {
        context->picture_active = 0;
        context->target = VA_INVALID_SURFACE;
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
    }
    if (surface->owner_context == 0) surface->owner_context = context_id;
    if (surface->owner_context != context_id) {
        context->picture_active = 0;
        context->target = VA_INVALID_SURFACE;
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (prepare_encode_input(runtime, context, surface) < 0) {
        int saved = errno;
        trace_errno("prepare-modifier-input", saved);
        context->picture_active = 0;
        context->target = VA_INVALID_SURFACE;
        pthread_mutex_unlock(&runtime->mutex);
        return status_from_errno(saved);
    }
    if (surface->write_exported) {
        int acquire_fence_fd = export_implicit_read_fence(&surface->prime);
        if (acquire_fence_fd < 0 ||
            advc_vaapi_encode_surface_link_set_acquire_fence(
                &surface->link, acquire_fence_fd) < 0) {
            int saved = errno;
            if (acquire_fence_fd >= 0) close(acquire_fence_fd);
            context->picture_active = 0;
            context->target = VA_INVALID_SURFACE;
            surface->state = ADVC_ENCODE_SURFACE_ERROR;
            pthread_mutex_unlock(&runtime->mutex);
            return status_from_errno(saved);
        }
        surface->write_exported = 0;
    }
    if (!surface->link.registered &&
        advc_vaapi_encode_surface_link_register(&surface->link,
                                                &context->broker) < 0) {
        trace_errno("register-surface", errno);
        context->picture_active = 0;
        context->target = VA_INVALID_SURFACE;
        pthread_mutex_unlock(&runtime->mutex);
        return status_from_errno(errno);
    }
    pts_ns = frame_pts_ns(context->frame_index,
                          context->frame.framerate_milli);
    if (pts_ns == UINT64_MAX ||
        advc_vaapi_encode_surface_link_submit(&surface->link,
                                              &context->broker,
                                              pts_ns) < 0) {
        trace_errno("submit-surface", errno);
        context->picture_active = 0;
        context->target = VA_INVALID_SURFACE;
        surface->state = ADVC_ENCODE_SURFACE_ERROR;
        pthread_mutex_unlock(&runtime->mutex);
        return status_from_errno(errno);
    }
    ++surface->gpu_conversion_submits;
    if (getenv("ADVC_VAAPI_TRACE") != NULL) {
        const char *origin = surface->pending_cpu_pixel_copies != 0
                                 ? surface->pending_writable_prime_exports != 0
                                       ? "mixed"
                                       : "cpu-upload"
                                 : surface->pending_writable_prime_exports != 0
                                       ? "prime-write-export"
                                       : "unchanged-surface";
        fprintf(stderr,
                "advc-vaapi-encode: input-accounting surface=%u "
                "origin=%s cpu_pixel_copies=%u writable_exports=%u "
                "gpu_conversions=1 modifier_repacks=%llu\n",
                context->target, origin,
                surface->pending_cpu_pixel_copies,
                surface->pending_writable_prime_exports,
                (unsigned long long)
                    surface->gpu_modifier_repack_submits);
    }
    surface->pending_cpu_pixel_copies = 0;
    surface->pending_writable_prime_exports = 0;
    if (frame_session_mode() &&
        advc_vaapi_encode_broker_signal_eos(&context->broker, pts_ns) < 0) {
        trace_errno("signal-frame-eos", errno);
        context->picture_active = 0;
        context->target = VA_INVALID_SURFACE;
        surface->state = ADVC_ENCODE_SURFACE_ERROR;
        pthread_mutex_unlock(&runtime->mutex);
        return status_from_errno(errno);
    }
    advc_vaapi_encode_coded_output_close(&coded->output);
    coded->pending_surface = context->target;
    surface->coded_buffer = context->frame.coded_buffer;
    surface->pts_ns = pts_ns;
    context->last_pts_ns = pts_ns;
    surface->submit_index = context->frame_index;
    surface->state = ADVC_ENCODE_SURFACE_PENDING;
    ++context->pending_count;
    ++context->frame_index;
    context->picture_active = 0;
    context->target = VA_INVALID_SURFACE;
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

static uint32_t timeout_ns_to_ms(uint64_t timeout_ns) {
    if (timeout_ns == 0) return 0;
    uint64_t limited = timeout_ns > ADVC_ENCODE_SYNC_LIMIT_NS
                           ? ADVC_ENCODE_SYNC_LIMIT_NS
                           : timeout_ns;
    uint64_t millis = (limited + UINT64_C(999999)) / UINT64_C(1000000);
    if (millis == 0) millis = 1;
    if (millis > UINT32_MAX) millis = UINT32_MAX;
    return (uint32_t)millis;
}

VAStatus advc_vaapi_encode_sync_surface(
    struct advc_vaapi_encode_runtime *runtime, VASurfaceID surface_id,
    uint64_t timeout_ns) {
    struct advc_encode_surface_slot *surface;
    VAStatus status;
    if (runtime == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    pthread_mutex_lock(&runtime->mutex);
    surface = get_surface(runtime, surface_id);
    if (surface == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    status = sync_surface_locked(runtime, surface,
                                 timeout_ns_to_ms(timeout_ns));
    pthread_mutex_unlock(&runtime->mutex);
    return status;
}

VAStatus advc_vaapi_encode_sync_buffer(
    struct advc_vaapi_encode_runtime *runtime, VABufferID buffer_id,
    uint64_t timeout_ns) {
    struct advc_encode_buffer_slot *buffer;
    struct advc_encode_surface_slot *surface;
    VAStatus status = VA_STATUS_SUCCESS;
    if (runtime == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    pthread_mutex_lock(&runtime->mutex);
    buffer = get_buffer(runtime, buffer_id);
    if (buffer == NULL || buffer->type != VAEncCodedBufferType) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (buffer->pending_surface != VA_INVALID_SURFACE) {
        surface = get_surface(runtime, buffer->pending_surface);
        if (surface == NULL)
            status = VA_STATUS_ERROR_ENCODING_ERROR;
        else
            status = sync_surface_locked(runtime, surface,
                                         timeout_ns_to_ms(timeout_ns));
    }
    pthread_mutex_unlock(&runtime->mutex);
    return status;
}

VAStatus advc_vaapi_encode_query_surface_status(
    struct advc_vaapi_encode_runtime *runtime, VASurfaceID surface_id,
    VASurfaceStatus *status) {
    struct advc_encode_surface_slot *surface;
    if (runtime == NULL || status == NULL)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&runtime->mutex);
    surface = get_surface(runtime, surface_id);
    if (surface == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    *status = surface->state == ADVC_ENCODE_SURFACE_PENDING
                  ? VASurfaceRendering
                  : surface->state == ADVC_ENCODE_SURFACE_ERROR
                        ? VASurfaceSkipped
                        : VASurfaceReady;
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_encode_export_surface(
    struct advc_vaapi_encode_runtime *runtime, VASurfaceID surface_id,
    uint32_t mem_type, uint32_t flags, void *descriptor) {
    struct advc_encode_surface_slot *surface;
    uint32_t access = flags & VA_EXPORT_SURFACE_READ_WRITE;
    VAStatus status;
    if (runtime == NULL || descriptor == NULL)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    pthread_mutex_lock(&runtime->mutex);
    surface = get_surface(runtime, surface_id);
    if (surface == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (surface->state == ADVC_ENCODE_SURFACE_PENDING) {
        status = sync_surface_locked(runtime, surface,
                                     ADVC_VAAPI_IMAGE_SYNC_TIMEOUT_MS);
        if (status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&runtime->mutex);
            return status;
        }
    }
    if (access == VA_EXPORT_SURFACE_WRITE_ONLY) {
        int fence_fd;
        if (!runtime->write_export_ready ||
            surface->state == ADVC_ENCODE_SURFACE_ERROR) {
            if (getenv("ADVC_VAAPI_TRACE") != NULL)
                fprintf(stderr,
                        "advc-vaapi-encode: export-write-precheck "
                        "surface=%u gate=%d already_exported=%d state=%d\n",
                        surface_id, runtime->write_export_ready,
                        surface->write_exported, (int)surface->state);
            pthread_mutex_unlock(&runtime->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        /* OBS can request a fresh PRIME descriptor for the same writable VA
         * surface more than once before EndPicture. Each descriptor owns
         * duplicated FDs but refers to the same dma-buf, so repeated export
         * is valid. Keep write_exported asserted and snapshot the single
         * implicit producer fence at EndPicture after the application's last
         * GPU write. Preflight the kernel sync-file UAPI on every export. */
        fence_fd = export_implicit_read_fence(&surface->prime);
        if (fence_fd < 0) {
            status = status_from_errno(errno);
            trace_errno("export-write-fence", errno);
            pthread_mutex_unlock(&runtime->mutex);
            return status;
        }
        close(fence_fd);
    } else if (access != VA_EXPORT_SURFACE_READ_ONLY) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    status = advc_vaapi_prime_export_nv12(&surface->prime, -1, flags,
        (VADRMPRIMESurfaceDescriptor *)descriptor);
    if (status != VA_STATUS_SUCCESS && getenv("ADVC_VAAPI_TRACE") != NULL)
        fprintf(stderr,
                "advc-vaapi-encode: export-prime surface=%u access=0x%x "
                "flags=0x%x status=%d\n",
                surface_id, access, flags, status);
    if (status == VA_STATUS_SUCCESS &&
        access == VA_EXPORT_SURFACE_WRITE_ONLY) {
        surface->write_exported = 1;
        ++surface->writable_prime_exports;
        ++surface->pending_writable_prime_exports;
    }
    pthread_mutex_unlock(&runtime->mutex);
    return status;
}

VAStatus advc_vaapi_encode_query_surface_accounting(
    struct advc_vaapi_encode_runtime *runtime, VASurfaceID surface_id,
    struct advc_vaapi_encode_surface_accounting *accounting) {
    struct advc_encode_surface_slot *surface;
    if (runtime == NULL || accounting == NULL)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&runtime->mutex);
    surface = get_surface(runtime, surface_id);
    if (surface == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    accounting->cpu_pixel_copies = surface->cpu_pixel_copies;
    accounting->writable_prime_exports = surface->writable_prime_exports;
    accounting->gpu_conversion_submits = surface->gpu_conversion_submits;
    accounting->gpu_modifier_repack_submits =
        surface->gpu_modifier_repack_submits;
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}
