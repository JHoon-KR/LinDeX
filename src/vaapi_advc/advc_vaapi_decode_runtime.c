#define _GNU_SOURCE

#include "advc_vaapi_decode_runtime.h"

#include "advc_h264_annexb.h"
#include "advc_vaapi_decode_eos.h"
#include "advc_vaapi_decode_hevc.h"
#include "advc_vaapi_decode_vp9.h"
#include "advc_vaapi_modifier_policy.h"
#include "advc_vaapi_slice_layout.h"
#include "advc_vaapi_surface_policy.h"
#include "advc/client.h"

#include <va/va_dec_hevc.h>
#include <va/va_dec_vp9.h>
#include <va/va_drmcommon.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define ADVC_VAAPI_CONFIG_SLOTS 8u
#define ADVC_VAAPI_CONTEXT_SLOTS 4u
#define ADVC_VAAPI_SURFACE_SLOTS 64u
#define ADVC_VAAPI_BUFFER_SLOTS 256u
#define ADVC_VAAPI_TYPE_CONFIG UINT32_C(0x01000000)
#define ADVC_VAAPI_TYPE_CONTEXT UINT32_C(0x02000000)
#define ADVC_VAAPI_TYPE_SURFACE UINT32_C(0x03000000)
#define ADVC_VAAPI_TYPE_BUFFER UINT32_C(0x04000000)
#define ADVC_VAAPI_INDEX_MASK UINT32_C(0x00ffffff)
#define ADVC_VAAPI_SYNC_LIMIT_NS UINT64_C(5000000000)
#define ADVC_VAAPI_PTS_STEP_NS UINT64_C(1000000)
#define DRM_FORMAT_R8 UINT32_C(0x20203852)
#define DRM_FORMAT_GR88 UINT32_C(0x38385247)

static pthread_mutex_t export_timing_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t export_timing_once = PTHREAD_ONCE_INIT;
static int export_timing_enabled;
static uint64_t export_timing_count;
static uint64_t export_timing_total_ns;
static uint64_t export_timing_max_ns;

static uint64_t monotonic_time_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static void initialize_export_timing(void) {
    const char *value = getenv("ADVC_VAAPI_EXPORT_TIMING");
    export_timing_enabled = value != NULL && strcmp(value, "1") == 0;
}

static uint64_t start_export_timing(void) {
    pthread_once(&export_timing_once, initialize_export_timing);
    return export_timing_enabled ? monotonic_time_ns() : 0;
}

static void record_export_timing(uint64_t started_ns, VAStatus status) {
    uint64_t ended_ns;
    uint64_t elapsed_ns;
    if (started_ns == 0) return;
    ended_ns = monotonic_time_ns();
    if (ended_ns < started_ns) return;
    elapsed_ns = ended_ns - started_ns;
    pthread_mutex_lock(&export_timing_mutex);
    ++export_timing_count;
    export_timing_total_ns += elapsed_ns;
    if (elapsed_ns > export_timing_max_ns) export_timing_max_ns = elapsed_ns;
    if ((export_timing_count % 120u) == 0) {
        fprintf(stderr,
                "advc-vaapi-timing: export count=%llu avg_us=%llu max_us=%llu status=%d\n",
                (unsigned long long)export_timing_count,
                (unsigned long long)(export_timing_total_ns /
                                     export_timing_count / 1000u),
                (unsigned long long)(export_timing_max_ns / 1000u),
                status);
    }
    pthread_mutex_unlock(&export_timing_mutex);
}

#ifndef ADVC_VAAPI_INPROCESS_REPACK
#define ADVC_VAAPI_INPROCESS_REPACK 0
#endif

_Static_assert(ADVC_VAAPI_DECODE_EOS_OUTPUT_FLAG_EOS ==
                   ADVC_FLAG_END_OF_STREAM,
               "private EOS flag must match the broker protocol");
struct advc_vaapi_config {
    int used;
    VAProfile profile;
};

enum advc_vaapi_surface_state {
    ADVC_SURFACE_IDLE = 0,
    ADVC_SURFACE_PENDING = 1,
    ADVC_SURFACE_READY = 2,
    ADVC_SURFACE_ERROR = 3,
};

struct advc_vaapi_surface {
    int used;
    uint32_t width;
    uint32_t height;
    enum advc_vaapi_surface_state state;
    uint64_t pts_ns;
    uint32_t owner_context;
    uint32_t owner_session;
    uint64_t broker_buffer_id;
    int prime_reserved;
    struct advc_dmabuf_descriptor prime;
    int acquire_fence_fd;
};

struct advc_vaapi_buffer {
    int used;
    VAContextID context_id;
    VABufferType type;
    unsigned int element_size;
    unsigned int capacity_elements;
    unsigned int num_elements;
    void *data;
    int mapped;
};

enum advc_vaapi_decode_codec {
    ADVC_DECODE_CODEC_H264 = 0,
    ADVC_DECODE_CODEC_HEVC = 1,
    ADVC_DECODE_CODEC_VP9 = 2,
};

struct advc_vaapi_context {
    int used;
    VAConfigID config_id;
    int broker_fd;
    uint32_t broker_session;
    uint32_t width;
    uint32_t height;
    enum advc_vaapi_decode_codec codec;
    int async_decode_prime;
    int broker_qcom_modifier;
    int picture_active;
    VASurfaceID target;
    int have_picture;
    VAPictureParameterBufferH264 picture;
    int have_iq;
    VAIQMatrixBufferH264 iq;
    VASliceParameterBufferH264 *slice_parameters;
    VAPictureParameterBufferHEVC hevc_picture;
    VAIQMatrixBufferHEVC hevc_iq;
    VASliceParameterBufferHEVC *hevc_slice_parameters;
    VADecPictureParameterBufferVP9 vp9_picture;
    VASliceParameterBufferVP9 *vp9_slice_parameters;
    size_t slice_count;
    size_t slice_data_bound_count;
    uint8_t *slice_data;
    size_t slice_data_size;
    struct advc_h264_codec_config last_config;
    struct advc_hevc_codec_config hevc_last_config;
    int configured;
    uint8_t pps_l0_default_active_minus1;
    uint8_t pps_l1_default_active_minus1;
    uint8_t pps_l0_known;
    uint8_t pps_l1_known;
    struct advc_vaapi_decode_eos_state eos;
};

struct advc_vaapi_decode_runtime {
    pthread_mutex_t mutex;
    char *socket_path;
    struct advc_vaapi_policy policy;
    struct advc_vaapi_modifier_policy modifier_policy;
    struct advc_vaapi_consumer_policy consumer;
    uint64_t next_pts_ns;
    struct advc_vaapi_config configs[ADVC_VAAPI_CONFIG_SLOTS];
    struct advc_vaapi_context contexts[ADVC_VAAPI_CONTEXT_SLOTS];
    struct advc_vaapi_surface surfaces[ADVC_VAAPI_SURFACE_SLOTS];
    struct advc_vaapi_buffer buffers[ADVC_VAAPI_BUFFER_SLOTS];
};

/*
 * Optional platform hooks. A hook must return a complete, owned NV12 LINEAR
 * descriptor and an optional acquire fence. The source release fence protects
 * the broker-owned compressed input while the conversion is executing.
 */
#if ADVC_VAAPI_INPROCESS_REPACK
int advc_vaapi_gpu_repack_linear(
    const struct advc_dmabuf_descriptor *source, int source_acquire_fence_fd,
    struct advc_dmabuf_descriptor *linear, int *linear_acquire_fence_fd,
    int *source_release_fence_fd);
#endif
__attribute__((weak)) int advc_vaapi_cpu_copy_linear(
    const struct advc_dmabuf_descriptor *source, int source_acquire_fence_fd,
    struct advc_dmabuf_descriptor *linear, int *linear_acquire_fence_fd,
    int *source_release_fence_fd);

static uint32_t make_id(uint32_t type, unsigned int index) {
    return type | (index + 1u);
}

static int id_index(uint32_t id, uint32_t type, unsigned int limit,
                    unsigned int *index) {
    uint32_t low;
    if ((id & ~ADVC_VAAPI_INDEX_MASK) != type) return -1;
    low = id & ADVC_VAAPI_INDEX_MASK;
    if (low == 0 || low > limit) return -1;
    *index = low - 1u;
    return 0;
}

static struct advc_vaapi_config *get_config(
    struct advc_vaapi_decode_runtime *runtime, VAConfigID id) {
    unsigned int index;
    if (runtime == NULL ||
        id_index(id, ADVC_VAAPI_TYPE_CONFIG, ADVC_VAAPI_CONFIG_SLOTS,
                 &index) < 0 ||
        !runtime->configs[index].used)
        return NULL;
    return &runtime->configs[index];
}

static struct advc_vaapi_context *get_context(
    struct advc_vaapi_decode_runtime *runtime, VAContextID id) {
    unsigned int index;
    if (runtime == NULL ||
        id_index(id, ADVC_VAAPI_TYPE_CONTEXT, ADVC_VAAPI_CONTEXT_SLOTS,
                 &index) < 0 ||
        !runtime->contexts[index].used)
        return NULL;
    return &runtime->contexts[index];
}

static struct advc_vaapi_surface *get_surface(
    struct advc_vaapi_decode_runtime *runtime, VASurfaceID id) {
    unsigned int index;
    if (runtime == NULL ||
        id_index(id, ADVC_VAAPI_TYPE_SURFACE, ADVC_VAAPI_SURFACE_SLOTS,
                 &index) < 0 ||
        !runtime->surfaces[index].used)
        return NULL;
    return &runtime->surfaces[index];
}

static struct advc_vaapi_buffer *get_buffer(
    struct advc_vaapi_decode_runtime *runtime, VABufferID id) {
    unsigned int index;
    if (runtime == NULL ||
        id_index(id, ADVC_VAAPI_TYPE_BUFFER, ADVC_VAAPI_BUFFER_SLOTS,
                 &index) < 0 ||
        !runtime->buffers[index].used)
        return NULL;
    return &runtime->buffers[index];
}

static void reset_staging(struct advc_vaapi_context *context) {
    context->picture_active = 0;
    context->target = VA_INVALID_SURFACE;
    context->have_picture = 0;
    context->have_iq = 0;
    free(context->slice_parameters);
    context->slice_parameters = NULL;
    free(context->hevc_slice_parameters);
    context->hevc_slice_parameters = NULL;
    free(context->vp9_slice_parameters);
    context->vp9_slice_parameters = NULL;
    context->slice_count = 0;
    context->slice_data_bound_count = 0;
    free(context->slice_data);
    context->slice_data = NULL;
    context->slice_data_size = 0;
}

static void close_prime(struct advc_vaapi_surface *surface) {
    if (surface->acquire_fence_fd >= 0) close(surface->acquire_fence_fd);
    surface->acquire_fence_fd = -1;
    advc_dmabuf_descriptor_close(&surface->prime);
}

static void release_surface_output(struct advc_vaapi_decode_runtime *runtime,
                                   struct advc_vaapi_surface *surface) {
    unsigned int index;
    uint32_t detail = 0;
    if (surface->broker_buffer_id != 0 && surface->owner_context != 0 &&
        id_index(surface->owner_context, ADVC_VAAPI_TYPE_CONTEXT,
                 ADVC_VAAPI_CONTEXT_SLOTS, &index) == 0 &&
        runtime->contexts[index].used &&
        runtime->contexts[index].broker_session == surface->owner_session) {
        (void)advc_client_release_output(
            runtime->contexts[index].broker_fd, surface->owner_session,
            surface->broker_buffer_id, &detail);
    }
    surface->broker_buffer_id = 0;
    surface->prime_reserved = 0;
    surface->owner_context = 0;
    surface->owner_session = 0;
    close_prime(surface);
    surface->state = ADVC_SURFACE_IDLE;
    surface->pts_ns = 0;
}

static int exact_env(const char *name, const char *value) {
    const char *actual = getenv(name);
    return actual != NULL && strcmp(actual, value) == 0;
}

static void trace_decode_failure(const char *stage, int error);

static int reserve_surface_linear(
    struct advc_vaapi_context *context, VAContextID context_id,
    struct advc_vaapi_surface *surface, uint64_t pts_ns) {
    struct advc_dmabuf_descriptor reserved;
    uint32_t detail = 0;
    int status;
    if (!context->async_decode_prime ||
        !exact_env("ADVC_VAAPI_ASYNC_EXPORT",
                   "candidate-firefox-bframe-v1"))
        return 0;
    memset(&reserved, 0, sizeof(reserved));
    for (uint32_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        reserved.objects[i].fd = -1;
    status = advc_client_reserve_linear(
        context->broker_fd, context->broker_session, pts_ns, context->width,
        context->height, &reserved, &detail);
    if (status != ADVC_STATUS_OK) {
        trace_decode_failure("reserve-linear",
                             status < 0 ? errno : (int)detail);
        advc_dmabuf_descriptor_close(&reserved);
        return -1;
    }
    surface->prime = reserved;
    surface->broker_buffer_id = reserved.buffer_id;
    surface->prime_reserved = 1;
    surface->pts_ns = pts_ns;
    surface->owner_context = context_id;
    surface->owner_session = context->broker_session;
    return 1;
}

static void trace_decode_failure(const char *stage, int error) {
    if (exact_env("ADVC_VAAPI_TRACE", "1"))
        fprintf(stderr, "advc-vaapi: %s failed errno=%d\n", stage, error);
}

static int configure_consumer_policy(
    struct advc_vaapi_decode_runtime *runtime) {
    if (advc_vaapi_modifier_policy_parse(
            getenv("ADVC_VAAPI_DECODE_OUTPUT"),
            getenv("ADVC_VAAPI_OUTPUT"), &runtime->modifier_policy) < 0) {
        trace_decode_failure("decode-output-policy", errno);
        return -1;
    }
    if (runtime->modifier_policy.mode == ADVC_VAAPI_MODIFIER_AUTO) {
        runtime->consumer.accepts_linear_nv12 = 1;
        runtime->consumer.accepts_qcom_compressed_nv12 =
            exact_env("ADVC_VAAPI_QCOM_IMPORT", "validated-v1");
    } else if (runtime->modifier_policy.mode == ADVC_VAAPI_MODIFIER_LINEAR) {
        runtime->consumer.accepts_linear_nv12 = 1;
    } else {
        runtime->consumer.accepts_qcom_compressed_nv12 =
            exact_env("ADVC_VAAPI_QCOM_IMPORT", "validated-v1");
    }
#if ADVC_VAAPI_INPROCESS_REPACK
    runtime->consumer.gpu_repack_available =
        exact_env("ADVC_VAAPI_GPU_LINEAR_REPACK",
                  "validated-qcom-nv12-v1");
#else
    runtime->consumer.gpu_repack_available = 0;
#endif
    runtime->consumer.cpu_copy_allowed =
        advc_vaapi_cpu_copy_linear != NULL &&
        exact_env("ADVC_VAAPI_ALLOW_CPU_COPY", "explicit");
    if (exact_env("ADVC_VAAPI_TRACE", "1"))
        fprintf(stderr,
                "advc-vaapi: decode-output mode=%s source=%d qcom-import=%d "
                "gpu-linear-repack=%d\n",
                advc_vaapi_modifier_mode_name(runtime->modifier_policy.mode),
                (int)runtime->modifier_policy.source,
                runtime->consumer.accepts_qcom_compressed_nv12,
                runtime->consumer.gpu_repack_available);
    return 0;
}

static VAStatus status_for_errno(int error) {
    if (error == ENOMEM) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    if (error == ENOTSUP || error == EPROTONOSUPPORT)
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    if (error == E2BIG || error == EINVAL || error == EPROTO)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

static int profile_supported(const struct advc_vaapi_decode_runtime *runtime,
                             VAProfile profile) {
    if ((profile == VAProfileH264ConstrainedBaseline ||
         profile == VAProfileH264Main) &&
        (runtime->policy.advertised_codecs & ADVC_VAAPI_CODEC_H264) != 0)
        return 1;
    if (profile == VAProfileHEVCMain &&
        (runtime->policy.advertised_codecs &
         ADVC_VAAPI_CODEC_HEVC_MAIN) != 0)
        return 1;
    return profile == VAProfileVP9Profile0 &&
           (runtime->policy.advertised_codecs &
            ADVC_VAAPI_CODEC_VP9_PROFILE0) != 0;
}

static enum advc_vaapi_decode_codec codec_for_profile(VAProfile profile) {
    if (profile == VAProfileHEVCMain) return ADVC_DECODE_CODEC_HEVC;
    if (profile == VAProfileVP9Profile0) return ADVC_DECODE_CODEC_VP9;
    return ADVC_DECODE_CODEC_H264;
}

static void profile_limits(const struct advc_vaapi_policy *policy,
                           VAProfile profile, uint32_t *width,
                           uint32_t *height) {
    if (profile == VAProfileHEVCMain) {
        *width = policy->hevc_max_width;
        *height = policy->hevc_max_height;
    } else if (profile == VAProfileVP9Profile0) {
        *width = policy->vp9_max_width;
        *height = policy->vp9_max_height;
    } else {
        *width = policy->h264_max_width;
        *height = policy->h264_max_height;
    }
}

static const char *profile_mime(VAProfile profile) {
    if (profile == VAProfileHEVCMain) return "video/hevc";
    if (profile == VAProfileVP9Profile0) return "video/x-vnd.on2.vp9";
    return "video/avc";
}

static void advertised_surface_limits(
    const struct advc_vaapi_policy *policy, uint32_t *width,
    uint32_t *height) {
    *width = 0;
    *height = 0;
    if ((policy->advertised_codecs & ADVC_VAAPI_CODEC_H264) != 0) {
        if (policy->h264_max_width > *width) *width = policy->h264_max_width;
        if (policy->h264_max_height > *height)
            *height = policy->h264_max_height;
    }
    if ((policy->advertised_codecs & ADVC_VAAPI_CODEC_HEVC_MAIN) != 0) {
        if (policy->hevc_max_width > *width) *width = policy->hevc_max_width;
        if (policy->hevc_max_height > *height)
            *height = policy->hevc_max_height;
    }
    if ((policy->advertised_codecs & ADVC_VAAPI_CODEC_VP9_PROFILE0) != 0) {
        if (policy->vp9_max_width > *width) *width = policy->vp9_max_width;
        if (policy->vp9_max_height > *height)
            *height = policy->vp9_max_height;
    }
}

static int create_sealed_memfd(const uint8_t *data, size_t size) {
    size_t offset = 0;
    int fd = memfd_create("lindex-vaapi-au", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    while (offset < size) {
        ssize_t written = write(fd, data + offset, size - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (written == 0) {
            close(fd);
            errno = EIO;
            return -1;
        }
        offset += (size_t)written;
    }
    if (fcntl(fd, F_ADD_SEALS,
              F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int queue_bytes(struct advc_vaapi_context *context, const uint8_t *data,
                       size_t size, uint64_t pts_ns, uint32_t flags) {
    struct advc_client_input input;
    uint32_t detail = 0;
    int fd = -1;
    int status;
    memset(&input, 0, sizeof(input));
    input.size = size;
    /*
     * Firefox installs the RDD seccomp filter before loading libva and denies
     * memfd_create(2).  The ADVC wire format already supports bounded inline
     * compressed input, so use it whenever the access unit fits.  This copies
     * compressed bytes only; decoded pixel surfaces remain dma-buf based.
     */
    if (size <= ADVC_MAX_PAYLOAD - ADVC_QUEUE_INPUT_SIZE) {
        input.data = data;
        input.data_fd = -1;
    } else {
        fd = create_sealed_memfd(data, size);
        if (fd < 0) return -1;
        input.data_fd = fd;
    }
    input.buffer_id = pts_ns;
    input.pts_ns = pts_ns;
    input.flags = flags;
    status = advc_client_queue_input(context->broker_fd,
                                     context->broker_session, &input, &detail);
    if (fd >= 0) close(fd);
    if (status != ADVC_STATUS_OK) {
        if (status == ADVC_STATUS_WOULD_BLOCK)
            errno = EAGAIN;
        else if (status == ADVC_STATUS_NO_RESOURCE)
            errno = ENOSPC;
        else if (status >= 0)
            errno = EIO;
        return -1;
    }
    return 0;
}

static size_t codec_config_sps_size(
    const struct advc_h264_codec_config *config) {
    size_t i;
    if (config == NULL) return 0;
    for (i = 4; i + 4 <= config->size; ++i) {
        if (config->data[i] == 0 && config->data[i + 1] == 0 &&
            config->data[i + 2] == 0 && config->data[i + 3] == 1)
            return i;
    }
    return 0;
}

static int map_h264_parameters(
    const struct advc_vaapi_context *context, VAProfile profile,
    struct advc_h264_parameter_input *output) {
    const VAPictureParameterBufferH264 *picture = &context->picture;
    memset(output, 0, sizeof(*output));
    output->profile_idc =
        profile == VAProfileH264ConstrainedBaseline ? 66u : 77u;
    output->visible_width = context->width;
    output->visible_height = context->height;
    output->picture_width_in_mbs_minus1 = picture->picture_width_in_mbs_minus1;
    output->picture_height_in_mbs_minus1 = picture->picture_height_in_mbs_minus1;
    output->bit_depth_luma_minus8 = picture->bit_depth_luma_minus8;
    output->bit_depth_chroma_minus8 = picture->bit_depth_chroma_minus8;
    output->chroma_format_idc = picture->seq_fields.bits.chroma_format_idc;
    output->separate_colour_plane_flag =
        picture->seq_fields.bits.residual_colour_transform_flag;
    output->gaps_in_frame_num_value_allowed_flag =
        picture->seq_fields.bits.gaps_in_frame_num_value_allowed_flag;
    output->frame_mbs_only_flag =
        picture->seq_fields.bits.frame_mbs_only_flag;
    output->mb_adaptive_frame_field_flag =
        picture->seq_fields.bits.mb_adaptive_frame_field_flag;
    output->direct_8x8_inference_flag =
        picture->seq_fields.bits.direct_8x8_inference_flag;
    output->log2_max_frame_num_minus4 =
        picture->seq_fields.bits.log2_max_frame_num_minus4;
    output->pic_order_cnt_type = picture->seq_fields.bits.pic_order_cnt_type;
    output->log2_max_pic_order_cnt_lsb_minus4 =
        picture->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4;
    output->delta_pic_order_always_zero_flag =
        picture->seq_fields.bits.delta_pic_order_always_zero_flag;
    output->num_ref_frames = picture->num_ref_frames;
    output->pic_init_qp_minus26 = picture->pic_init_qp_minus26;
    output->pic_init_qs_minus26 = picture->pic_init_qs_minus26;
    output->chroma_qp_index_offset = picture->chroma_qp_index_offset;
    output->second_chroma_qp_index_offset =
        picture->second_chroma_qp_index_offset;
    output->entropy_coding_mode_flag =
        picture->pic_fields.bits.entropy_coding_mode_flag;
    output->weighted_pred_flag = picture->pic_fields.bits.weighted_pred_flag;
    output->weighted_bipred_idc =
        picture->pic_fields.bits.weighted_bipred_idc;
    output->transform_8x8_mode_flag =
        picture->pic_fields.bits.transform_8x8_mode_flag;
    output->field_pic_flag = picture->pic_fields.bits.field_pic_flag;
    output->constrained_intra_pred_flag =
        picture->pic_fields.bits.constrained_intra_pred_flag;
    output->bottom_field_pic_order_in_frame_present_flag =
        picture->pic_fields.bits.pic_order_present_flag;
    output->deblocking_filter_control_present_flag =
        picture->pic_fields.bits.deblocking_filter_control_present_flag;
    output->redundant_pic_cnt_present_flag =
        picture->pic_fields.bits.redundant_pic_cnt_present_flag;
    output->num_ref_idx_l0_default_active_minus1 =
        context->pps_l0_default_active_minus1;
    output->num_ref_idx_l1_default_active_minus1 =
        context->pps_l1_default_active_minus1;
    return 0;
}

static void map_hevc_parameters(
    const struct advc_vaapi_context *context,
    struct advc_hevc_parameter_input *output) {
    const VAPictureParameterBufferHEVC *picture = &context->hevc_picture;
    memset(output, 0, sizeof(*output));
    output->visible_width = context->width;
    output->visible_height = context->height;
    output->pic_width_in_luma_samples = picture->pic_width_in_luma_samples;
    output->pic_height_in_luma_samples = picture->pic_height_in_luma_samples;
    output->chroma_format_idc = picture->pic_fields.bits.chroma_format_idc;
    output->separate_colour_plane_flag =
        picture->pic_fields.bits.separate_colour_plane_flag;
    output->bit_depth_luma_minus8 = picture->bit_depth_luma_minus8;
    output->bit_depth_chroma_minus8 = picture->bit_depth_chroma_minus8;
    output->sps_max_dec_pic_buffering_minus1 =
        picture->sps_max_dec_pic_buffering_minus1;
    output->no_pic_reordering_flag =
        picture->pic_fields.bits.NoPicReorderingFlag;
    output->log2_min_luma_coding_block_size_minus3 =
        picture->log2_min_luma_coding_block_size_minus3;
    output->log2_diff_max_min_luma_coding_block_size =
        picture->log2_diff_max_min_luma_coding_block_size;
    output->log2_min_transform_block_size_minus2 =
        picture->log2_min_transform_block_size_minus2;
    output->log2_diff_max_min_transform_block_size =
        picture->log2_diff_max_min_transform_block_size;
    output->max_transform_hierarchy_depth_intra =
        picture->max_transform_hierarchy_depth_intra;
    output->max_transform_hierarchy_depth_inter =
        picture->max_transform_hierarchy_depth_inter;
    output->scaling_list_enabled_flag =
        picture->pic_fields.bits.scaling_list_enabled_flag;
    output->amp_enabled_flag = picture->pic_fields.bits.amp_enabled_flag;
    output->sample_adaptive_offset_enabled_flag =
        picture->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag;
    output->pcm_enabled_flag = picture->pic_fields.bits.pcm_enabled_flag;
    output->strong_intra_smoothing_enabled_flag =
        picture->pic_fields.bits.strong_intra_smoothing_enabled_flag;
    output->num_short_term_ref_pic_sets =
        picture->num_short_term_ref_pic_sets;
    output->long_term_ref_pics_present_flag =
        picture->slice_parsing_fields.bits.long_term_ref_pics_present_flag;
    output->num_long_term_ref_pic_sps = picture->num_long_term_ref_pic_sps;
    output->sps_temporal_mvp_enabled_flag =
        picture->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag;
    output->log2_max_pic_order_cnt_lsb_minus4 =
        picture->log2_max_pic_order_cnt_lsb_minus4;
    output->dependent_slice_segments_enabled_flag =
        picture->slice_parsing_fields.bits
            .dependent_slice_segments_enabled_flag;
    output->output_flag_present_flag =
        picture->slice_parsing_fields.bits.output_flag_present_flag;
    output->num_extra_slice_header_bits = picture->num_extra_slice_header_bits;
    output->sign_data_hiding_enabled_flag =
        picture->pic_fields.bits.sign_data_hiding_enabled_flag;
    output->cabac_init_present_flag =
        picture->slice_parsing_fields.bits.cabac_init_present_flag;
    output->num_ref_idx_l0_default_active_minus1 =
        picture->num_ref_idx_l0_default_active_minus1;
    output->num_ref_idx_l1_default_active_minus1 =
        picture->num_ref_idx_l1_default_active_minus1;
    output->init_qp_minus26 = picture->init_qp_minus26;
    output->constrained_intra_pred_flag =
        picture->pic_fields.bits.constrained_intra_pred_flag;
    output->transform_skip_enabled_flag =
        picture->pic_fields.bits.transform_skip_enabled_flag;
    output->cu_qp_delta_enabled_flag =
        picture->pic_fields.bits.cu_qp_delta_enabled_flag;
    output->diff_cu_qp_delta_depth = picture->diff_cu_qp_delta_depth;
    output->pps_cb_qp_offset = picture->pps_cb_qp_offset;
    output->pps_cr_qp_offset = picture->pps_cr_qp_offset;
    output->pps_slice_chroma_qp_offsets_present_flag =
        picture->slice_parsing_fields.bits
            .pps_slice_chroma_qp_offsets_present_flag;
    output->weighted_pred_flag =
        picture->pic_fields.bits.weighted_pred_flag;
    output->weighted_bipred_flag =
        picture->pic_fields.bits.weighted_bipred_flag;
    output->transquant_bypass_enabled_flag =
        picture->pic_fields.bits.transquant_bypass_enabled_flag;
    output->tiles_enabled_flag = picture->pic_fields.bits.tiles_enabled_flag;
    output->entropy_coding_sync_enabled_flag =
        picture->pic_fields.bits.entropy_coding_sync_enabled_flag;
    output->num_tile_columns_minus1 = picture->num_tile_columns_minus1;
    output->num_tile_rows_minus1 = picture->num_tile_rows_minus1;
    memcpy(output->column_width_minus1, picture->column_width_minus1,
           sizeof(output->column_width_minus1));
    memcpy(output->row_height_minus1, picture->row_height_minus1,
           sizeof(output->row_height_minus1));
    output->loop_filter_across_tiles_enabled_flag =
        picture->pic_fields.bits.loop_filter_across_tiles_enabled_flag;
    output->pps_loop_filter_across_slices_enabled_flag =
        picture->pic_fields.bits
            .pps_loop_filter_across_slices_enabled_flag;
    output->deblocking_filter_override_enabled_flag =
        picture->slice_parsing_fields.bits
            .deblocking_filter_override_enabled_flag;
    output->pps_disable_deblocking_filter_flag =
        picture->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag;
    output->pps_beta_offset_div2 = picture->pps_beta_offset_div2;
    output->pps_tc_offset_div2 = picture->pps_tc_offset_div2;
    output->lists_modification_present_flag =
        picture->slice_parsing_fields.bits.lists_modification_present_flag;
    output->log2_parallel_merge_level_minus2 =
        picture->log2_parallel_merge_level_minus2;
    output->slice_segment_header_extension_present_flag =
        picture->slice_parsing_fields.bits
            .slice_segment_header_extension_present_flag;
}

static int append_slice_parameters(struct advc_vaapi_context *context,
                                   const struct advc_vaapi_buffer *buffer) {
    void *current;
    void *grown;
    size_t expected_size;
    size_t max_slices;
    size_t old_size;
    size_t add_size;
    if (context->codec == ADVC_DECODE_CODEC_HEVC) {
        current = context->hevc_slice_parameters;
        expected_size = sizeof(VASliceParameterBufferHEVC);
        max_slices = ADVC_HEVC_MAX_SLICES;
    } else if (context->codec == ADVC_DECODE_CODEC_VP9) {
        current = context->vp9_slice_parameters;
        expected_size = sizeof(VASliceParameterBufferVP9);
        max_slices = 1;
    } else {
        current = context->slice_parameters;
        expected_size = sizeof(VASliceParameterBufferH264);
        max_slices = ADVC_H264_MAX_SLICES;
    }
    /*
     * A parameter group must be followed by its data buffer.  Accepting a
     * second group first would lose the libva buffer-relative offset domain
     * and could associate slices with the wrong bytes.
     */
    if (context->slice_data_bound_count != context->slice_count ||
        buffer->element_size != expected_size ||
        buffer->num_elements == 0 || buffer->num_elements > max_slices ||
        context->slice_count > max_slices - buffer->num_elements) {
        errno = EINVAL;
        return -1;
    }
    old_size = context->slice_count * expected_size;
    add_size = (size_t)buffer->num_elements * expected_size;
    grown = realloc(current, old_size + add_size);
    if (grown == NULL) return -1;
    memcpy((uint8_t *)grown + old_size, buffer->data, add_size);
    if (context->codec == ADVC_DECODE_CODEC_HEVC)
        context->hevc_slice_parameters = grown;
    else if (context->codec == ADVC_DECODE_CODEC_VP9)
        context->vp9_slice_parameters = grown;
    else
        context->slice_parameters = grown;
    context->slice_count += buffer->num_elements;
    return 0;
}

static int buffer_shape_supported(const struct advc_vaapi_context *context,
                                  VABufferType type, unsigned int size,
                                  unsigned int num_elements) {
    if (type == VASliceDataBufferType) return 1;
    if (context->codec == ADVC_DECODE_CODEC_HEVC) {
        if (type == VAPictureParameterBufferType)
            return size == sizeof(VAPictureParameterBufferHEVC) &&
                   num_elements == 1;
        if (type == VAIQMatrixBufferType)
            return size == sizeof(VAIQMatrixBufferHEVC) && num_elements == 1;
        return type == VASliceParameterBufferType &&
               size == sizeof(VASliceParameterBufferHEVC);
    }
    if (context->codec == ADVC_DECODE_CODEC_VP9) {
        if (type == VAPictureParameterBufferType)
            return size == sizeof(VADecPictureParameterBufferVP9) &&
                   num_elements == 1;
        return type == VASliceParameterBufferType &&
               size == sizeof(VASliceParameterBufferVP9);
    }
    if (type == VAPictureParameterBufferType)
        return size == sizeof(VAPictureParameterBufferH264) &&
               num_elements == 1;
    if (type == VAIQMatrixBufferType)
        return size == sizeof(VAIQMatrixBufferH264) && num_elements == 1;
    return type == VASliceParameterBufferType &&
           size == sizeof(VASliceParameterBufferH264);
}

static int set_slice_data(struct advc_vaapi_context *context,
                          const struct advc_vaapi_buffer *buffer) {
    size_t size = (size_t)buffer->element_size * buffer->num_elements;
    size_t pending_count;
    size_t i;
    uint32_t *offsets;
    uint32_t *sizes;
    uint8_t *grown;
    if (context->slice_data_bound_count >= context->slice_count || size == 0 ||
        context->slice_data_size > ADVC_MAX_INPUT_BYTES ||
        size > ADVC_MAX_INPUT_BYTES - context->slice_data_size) {
        errno = EINVAL;
        return -1;
    }
    pending_count = context->slice_count - context->slice_data_bound_count;
    offsets = calloc(pending_count, sizeof(*offsets));
    sizes = calloc(pending_count, sizeof(*sizes));
    if (offsets == NULL || sizes == NULL) {
        free(offsets);
        free(sizes);
        return -1;
    }
    for (i = 0; i < pending_count; ++i) {
        size_t index = context->slice_data_bound_count + i;
        if (context->codec == ADVC_DECODE_CODEC_HEVC) {
            offsets[i] = context->hevc_slice_parameters[index]
                             .slice_data_offset;
            sizes[i] = context->hevc_slice_parameters[index].slice_data_size;
        } else if (context->codec == ADVC_DECODE_CODEC_VP9) {
            offsets[i] = context->vp9_slice_parameters[index]
                             .slice_data_offset;
            sizes[i] = context->vp9_slice_parameters[index].slice_data_size;
        } else {
            offsets[i] = context->slice_parameters[index].slice_data_offset;
            sizes[i] = context->slice_parameters[index].slice_data_size;
        }
    }
    if (advc_vaapi_rebase_slice_offsets(
            offsets, sizes, pending_count, context->slice_data_size, size,
            ADVC_MAX_INPUT_BYTES) < 0) {
        free(offsets);
        free(sizes);
        return -1;
    }
    grown = realloc(context->slice_data, context->slice_data_size + size);
    if (grown == NULL) {
        free(offsets);
        free(sizes);
        return -1;
    }
    memcpy(grown + context->slice_data_size, buffer->data, size);
    context->slice_data = grown;
    for (i = 0; i < pending_count; ++i) {
        size_t index = context->slice_data_bound_count + i;
        if (context->codec == ADVC_DECODE_CODEC_HEVC)
            context->hevc_slice_parameters[index].slice_data_offset =
                offsets[i];
        else if (context->codec == ADVC_DECODE_CODEC_VP9)
            context->vp9_slice_parameters[index].slice_data_offset =
                offsets[i];
        else
            context->slice_parameters[index].slice_data_offset = offsets[i];
    }
    context->slice_data_size += size;
    context->slice_data_bound_count = context->slice_count;
    free(offsets);
    free(sizes);
    return 0;
}

static int convert_output(
    struct advc_vaapi_decode_runtime *runtime,
    struct advc_vaapi_context *context, struct advc_vaapi_surface *surface,
    struct advc_client_output *output) {
    struct advc_dmabuf_descriptor source;
    struct advc_dmabuf_descriptor converted;
    enum advc_vaapi_surface_route route;
    uint32_t detail = 0;
    int converted_fence = -1;
    int source_release_fence = -1;
    int rc;
    memset(&source, 0, sizeof(source));
    memset(&converted, 0, sizeof(converted));
    if (surface->prime_reserved) {
        if (output->transport != ADVC_TRANSPORT_AHARDWAREBUFFER ||
            output->buffer_id == 0 ||
            surface->prime.drm_fourcc != ADVC_DRM_FORMAT_NV12 ||
            surface->prime.drm_modifier != 0 ||
            advc_dmabuf_descriptor_validate(&surface->prime) < 0)
            return -1;
        surface->acquire_fence_fd = output->acquire_fence_fd;
        output->acquire_fence_fd = -1;
        surface->broker_buffer_id = output->buffer_id;
        surface->prime_reserved = 0;
        surface->state = ADVC_SURFACE_READY;
        return 0;
    }
    if (output->transport != ADVC_TRANSPORT_AHARDWAREBUFFER ||
        advc_client_transfer_prime(context->broker_fd, context->broker_session,
                                   output->buffer_id, &source, &detail) !=
            ADVC_STATUS_OK)
        return -1;
    {
        struct advc_vaapi_consumer_policy consumer = runtime->consumer;
        consumer.accepts_qcom_compressed_nv12 =
            consumer.accepts_qcom_compressed_nv12 &&
            context->broker_qcom_modifier > 0 &&
            !context->async_decode_prime;
        route = advc_vaapi_select_surface_route(&source, &consumer);
        if (exact_env("ADVC_VAAPI_TRACE", "1"))
            fprintf(stderr,
                    "advc-vaapi: decode-route mode=%s modifier=0x%016llx "
                    "broker-qcom=%d async-linear=%d route=%d\n",
                    advc_vaapi_modifier_mode_name(
                        runtime->modifier_policy.mode),
                    (unsigned long long)source.drm_modifier,
                    context->broker_qcom_modifier > 0,
                    context->async_decode_prime, (int)route);
    }
    if (route == ADVC_VAAPI_SURFACE_DIRECT_LINEAR ||
        route == ADVC_VAAPI_SURFACE_DIRECT_QCOM) {
        surface->prime = source;
        memset(&source, 0, sizeof(source));
        surface->acquire_fence_fd = output->acquire_fence_fd;
        output->acquire_fence_fd = -1;
        surface->broker_buffer_id = output->buffer_id;
        surface->state = ADVC_SURFACE_READY;
        return 0;
    }
#if ADVC_VAAPI_INPROCESS_REPACK
    if (route == ADVC_VAAPI_SURFACE_GPU_REPACK_LINEAR) {
        rc = advc_vaapi_gpu_repack_linear(
            &source, output->acquire_fence_fd, &converted, &converted_fence,
            &source_release_fence);
    }
#else
    if (route == ADVC_VAAPI_SURFACE_GPU_REPACK_LINEAR) {
        rc = -1;
        errno = ENOTSUP;
    }
#endif
    else if (route == ADVC_VAAPI_SURFACE_CPU_COPY_LINEAR &&
               advc_vaapi_cpu_copy_linear != NULL) {
        rc = advc_vaapi_cpu_copy_linear(
            &source, output->acquire_fence_fd, &converted, &converted_fence,
            &source_release_fence);
    } else {
        rc = -1;
        errno = ENOTSUP;
    }
    if (rc == 0 && converted.drm_fourcc == ADVC_DRM_FORMAT_NV12 &&
        converted.drm_modifier == 0 &&
        advc_dmabuf_descriptor_validate(&converted) == 0 &&
        advc_client_release_output_fenced(
            context->broker_fd, context->broker_session, output->buffer_id,
            source_release_fence, &detail) == ADVC_STATUS_OK) {
        if (source_release_fence >= 0) close(source_release_fence);
        source_release_fence = -1;
        advc_dmabuf_descriptor_close(&source);
        surface->prime = converted;
        memset(&converted, 0, sizeof(converted));
        surface->acquire_fence_fd = converted_fence;
        converted_fence = -1;
        surface->state = ADVC_SURFACE_READY;
        return 0;
    }
    if (source_release_fence >= 0) close(source_release_fence);
    if (converted_fence >= 0) close(converted_fence);
    advc_dmabuf_descriptor_close(&source);
    advc_dmabuf_descriptor_close(&converted);
    return -1;
}

static int private_eos_drain_one(
    struct advc_vaapi_decode_runtime *runtime,
    struct advc_vaapi_context *context);

static int drain_one_output(struct advc_vaapi_decode_runtime *runtime,
                            struct advc_vaapi_context *context) {
    struct advc_client_output output;
    struct advc_vaapi_surface *surface = NULL;
    uint32_t detail = 0;
    unsigned int i;
    int status;
    if (context->eos.phase != ADVC_VAAPI_DECODE_EOS_OPEN)
        return private_eos_drain_one(runtime, context);
    memset(&output, 0, sizeof(output));
    output.data_fd = -1;
    output.acquire_fence_fd = -1;
    status = advc_client_dequeue_output(context->broker_fd,
                                        context->broker_session, &output,
                                        &detail);
    if (status == ADVC_STATUS_WOULD_BLOCK) return 0;
    if (status != ADVC_STATUS_OK) return -1;
    if (exact_env("ADVC_VAAPI_TRACE", "1"))
        fprintf(stderr,
                "advc-vaapi: output pts=%llu flags=0x%x buffer=%llu\n",
                (unsigned long long)output.pts_ns, output.flags,
                (unsigned long long)output.buffer_id);
    for (i = 0; i < ADVC_VAAPI_SURFACE_SLOTS; ++i) {
        if (runtime->surfaces[i].used &&
            runtime->surfaces[i].state == ADVC_SURFACE_PENDING &&
            runtime->surfaces[i].pts_ns == output.pts_ns &&
            runtime->surfaces[i].owner_session == context->broker_session) {
            surface = &runtime->surfaces[i];
            break;
        }
    }
    if (surface == NULL || convert_output(runtime, context, surface, &output) < 0) {
        if (surface != NULL) surface->state = ADVC_SURFACE_ERROR;
        if (output.buffer_id != 0)
            (void)advc_client_release_output(context->broker_fd,
                                             context->broker_session,
                                             output.buffer_id, &detail);
        advc_client_output_close(&output);
        return -1;
    }
    advc_client_output_close(&output);
    return 1;
}

struct private_eos_adapter {
    struct advc_vaapi_decode_runtime *runtime;
    struct advc_vaapi_context *context;
    struct advc_client_output client_output;
    int have_output;
};

static int32_t private_eos_immediate_status(
    struct advc_vaapi_decode_eos_status_v1 *status, int32_t result,
    uint32_t phase, uint32_t flags) {
    uint32_t caller_size;
    if (status == NULL || status->struct_size < sizeof(*status))
        return ADVC_VAAPI_DECODE_EOS_RESULT_INVALID_ARGUMENT;
    caller_size = status->struct_size;
    memset(status, 0, sizeof(*status));
    status->struct_size = caller_size;
    status->abi_version = ADVC_VAAPI_DECODE_EOS_ABI_VERSION;
    status->phase = phase;
    status->flags = flags;
    status->last_result = result;
    return result;
}

static int32_t private_eos_try_signal(void *opaque) {
    struct private_eos_adapter *adapter = opaque;
    struct advc_client_input input;
    uint32_t detail = 0;
    int status;
    memset(&input, 0, sizeof(input));
    input.data_fd = -1;
    input.flags = ADVC_FLAG_END_OF_STREAM;
    status = advc_client_queue_input(adapter->context->broker_fd,
                                     adapter->context->broker_session, &input,
                                     &detail);
    if (status == ADVC_STATUS_OK) return ADVC_VAAPI_DECODE_EOS_IO_OK;
    if (status == ADVC_STATUS_WOULD_BLOCK)
        return ADVC_VAAPI_DECODE_EOS_IO_RETRY;
    if (status == ADVC_STATUS_NO_RESOURCE)
        return ADVC_VAAPI_DECODE_EOS_IO_OUTPUT_WINDOW_FULL;
    return ADVC_VAAPI_DECODE_EOS_IO_FATAL;
}

static int32_t private_eos_try_dequeue(
    void *opaque, struct advc_vaapi_decode_eos_output *output) {
    struct private_eos_adapter *adapter = opaque;
    uint32_t detail = 0;
    int status;
    if (adapter->have_output || output == NULL)
        return ADVC_VAAPI_DECODE_EOS_IO_FATAL;
    memset(&adapter->client_output, 0, sizeof(adapter->client_output));
    adapter->client_output.data_fd = -1;
    adapter->client_output.acquire_fence_fd = -1;
    status = advc_client_dequeue_output(
        adapter->context->broker_fd, adapter->context->broker_session,
        &adapter->client_output, &detail);
    if (status == ADVC_STATUS_WOULD_BLOCK)
        return ADVC_VAAPI_DECODE_EOS_IO_RETRY;
    if (status == ADVC_STATUS_NO_RESOURCE)
        return ADVC_VAAPI_DECODE_EOS_IO_OUTPUT_WINDOW_FULL;
    if (status != ADVC_STATUS_OK)
        return ADVC_VAAPI_DECODE_EOS_IO_FATAL;
    adapter->have_output = 1;
    memset(output, 0, sizeof(*output));
    output->buffer_id = adapter->client_output.buffer_id;
    output->pts_ns = adapter->client_output.pts_ns;
    output->flags = adapter->client_output.flags;
    output->kind =
        adapter->client_output.transport == ADVC_TRANSPORT_BYTES &&
                adapter->client_output.size == 0 &&
                (adapter->client_output.flags & ADVC_FLAG_END_OF_STREAM) != 0
            ? ADVC_VAAPI_DECODE_EOS_OUTPUT_CONTROL
            : ADVC_VAAPI_DECODE_EOS_OUTPUT_FRAME;
    output->private_data = &adapter->client_output;
    return ADVC_VAAPI_DECODE_EOS_IO_OK;
}

static int32_t private_eos_handle_frame(
    void *opaque, const struct advc_vaapi_decode_eos_output *output) {
    struct private_eos_adapter *adapter = opaque;
    struct advc_vaapi_surface *surface = NULL;
    unsigned int i;
    if (!adapter->have_output || output == NULL ||
        output->private_data != &adapter->client_output)
        return ADVC_VAAPI_DECODE_EOS_IO_FATAL;
    for (i = 0; i < ADVC_VAAPI_SURFACE_SLOTS; ++i) {
        if (adapter->runtime->surfaces[i].used &&
            adapter->runtime->surfaces[i].state == ADVC_SURFACE_PENDING &&
            adapter->runtime->surfaces[i].pts_ns == output->pts_ns &&
            adapter->runtime->surfaces[i].owner_session ==
                adapter->context->broker_session) {
            surface = &adapter->runtime->surfaces[i];
            break;
        }
    }
    if (surface == NULL ||
        convert_output(adapter->runtime, adapter->context, surface,
                       &adapter->client_output) < 0) {
        if (surface != NULL) surface->state = ADVC_SURFACE_ERROR;
        return ADVC_VAAPI_DECODE_EOS_IO_FATAL;
    }
    advc_client_output_close(&adapter->client_output);
    adapter->have_output = 0;
    return ADVC_VAAPI_DECODE_EOS_IO_OK;
}

static int32_t private_eos_release_output(
    void *opaque, const struct advc_vaapi_decode_eos_output *output) {
    struct private_eos_adapter *adapter = opaque;
    uint32_t detail = 0;
    int status;
    if (!adapter->have_output || output == NULL ||
        output->private_data != &adapter->client_output)
        return ADVC_VAAPI_DECODE_EOS_IO_FATAL;
    status = advc_client_release_output(
        adapter->context->broker_fd, adapter->context->broker_session,
        adapter->client_output.buffer_id, &detail);
    advc_client_output_close(&adapter->client_output);
    adapter->have_output = 0;
    return status == ADVC_STATUS_OK ? ADVC_VAAPI_DECODE_EOS_IO_OK
                                    : ADVC_VAAPI_DECODE_EOS_IO_FATAL;
}

static void private_eos_mark_remaining_pending_failed(void *opaque) {
    struct private_eos_adapter *adapter = opaque;
    unsigned int i;
    for (i = 0; i < ADVC_VAAPI_SURFACE_SLOTS; ++i) {
        struct advc_vaapi_surface *surface = &adapter->runtime->surfaces[i];
        if (surface->used && surface->state == ADVC_SURFACE_PENDING &&
            surface->owner_session == adapter->context->broker_session)
            surface->state = ADVC_SURFACE_ERROR;
    }
}

static struct advc_vaapi_decode_eos_ops private_eos_ops(
    struct private_eos_adapter *adapter) {
    struct advc_vaapi_decode_eos_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.opaque = adapter;
    ops.try_signal = private_eos_try_signal;
    ops.try_dequeue = private_eos_try_dequeue;
    ops.handle_frame = private_eos_handle_frame;
    ops.release_output = private_eos_release_output;
    ops.mark_remaining_pending_failed =
        private_eos_mark_remaining_pending_failed;
    return ops;
}

/*
 * The runtime mutex is held by every caller.  Both the private progress API
 * and normal surface sync/query use this helper once EOS has been signalled,
 * so there is exactly one owner of broker dequeue and terminal EOS cannot be
 * consumed without updating context->eos.
 */
static int32_t private_eos_progress_locked(
    struct advc_vaapi_decode_runtime *runtime,
    struct advc_vaapi_context *context, uint32_t max_outputs,
    struct advc_vaapi_decode_eos_status_v1 *status) {
    struct private_eos_adapter adapter;
    struct advc_vaapi_decode_eos_ops ops;
    int32_t result;
    memset(&adapter, 0, sizeof(adapter));
    adapter.runtime = runtime;
    adapter.context = context;
    ops = private_eos_ops(&adapter);
    result = advc_vaapi_decode_eos_state_progress(
        &context->eos, &ops, max_outputs, status);
    if (adapter.have_output)
        (void)private_eos_release_output(
            &adapter, &(struct advc_vaapi_decode_eos_output){
                          .buffer_id = adapter.client_output.buffer_id,
                          .private_data = &adapter.client_output,
                      });
    return result;
}

static int private_eos_drain_one(
    struct advc_vaapi_decode_runtime *runtime,
    struct advc_vaapi_context *context) {
    struct advc_vaapi_decode_eos_status_v1 status;
    int32_t result;
    memset(&status, 0, sizeof(status));
    status.struct_size = sizeof(status);
    result = private_eos_progress_locked(runtime, context, 1, &status);
    if (result == ADVC_VAAPI_DECODE_EOS_RESULT_OK ||
        result == ADVC_VAAPI_DECODE_EOS_RESULT_COMPLETE)
        return status.frames_processed > 0 ? 1 : 0;
    if (result == ADVC_VAAPI_DECODE_EOS_RESULT_WOULD_BLOCK ||
        result == ADVC_VAAPI_DECODE_EOS_RESULT_NEED_OUTPUT_RELEASE)
        return 0;
    return -1;
}

int32_t advc_vaapi_decode_signal_eos_private(
    struct advc_vaapi_decode_runtime *runtime, VAContextID context_id,
    struct advc_vaapi_decode_eos_status_v1 *status) {
    struct private_eos_adapter adapter;
    struct advc_vaapi_decode_eos_ops ops;
    struct advc_vaapi_context *context;
    int32_t result;
    if (!advc_vaapi_decode_eos_gate_enabled())
        return private_eos_immediate_status(
            status, ADVC_VAAPI_DECODE_EOS_RESULT_DISABLED,
            ADVC_VAAPI_DECODE_EOS_OPEN, 0);
    if (runtime == NULL)
        return private_eos_immediate_status(
            status, ADVC_VAAPI_DECODE_EOS_RESULT_INVALID_CONTEXT,
            ADVC_VAAPI_DECODE_EOS_FAILED,
            ADVC_VAAPI_DECODE_EOS_STATUS_FAILED);
    pthread_mutex_lock(&runtime->mutex);
    context = get_context(runtime, context_id);
    if (context == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return private_eos_immediate_status(
            status, ADVC_VAAPI_DECODE_EOS_RESULT_INVALID_CONTEXT,
            ADVC_VAAPI_DECODE_EOS_FAILED,
            ADVC_VAAPI_DECODE_EOS_STATUS_FAILED);
    }
    if (context->picture_active) {
        uint32_t phase = context->eos.phase;
        pthread_mutex_unlock(&runtime->mutex);
        return private_eos_immediate_status(
            status, ADVC_VAAPI_DECODE_EOS_RESULT_INVALID_ARGUMENT, phase, 0);
    }
    memset(&adapter, 0, sizeof(adapter));
    adapter.runtime = runtime;
    adapter.context = context;
    ops = private_eos_ops(&adapter);
    result = advc_vaapi_decode_eos_state_signal(&context->eos, &ops, status);
    pthread_mutex_unlock(&runtime->mutex);
    return result;
}

int32_t advc_vaapi_decode_progress_eos_private(
    struct advc_vaapi_decode_runtime *runtime, VAContextID context_id,
    uint32_t max_outputs,
    struct advc_vaapi_decode_eos_status_v1 *status) {
    struct advc_vaapi_context *context;
    int32_t result;
    if (!advc_vaapi_decode_eos_gate_enabled())
        return private_eos_immediate_status(
            status, ADVC_VAAPI_DECODE_EOS_RESULT_DISABLED,
            ADVC_VAAPI_DECODE_EOS_OPEN, 0);
    if (runtime == NULL)
        return private_eos_immediate_status(
            status, ADVC_VAAPI_DECODE_EOS_RESULT_INVALID_CONTEXT,
            ADVC_VAAPI_DECODE_EOS_FAILED,
            ADVC_VAAPI_DECODE_EOS_STATUS_FAILED);
    pthread_mutex_lock(&runtime->mutex);
    context = get_context(runtime, context_id);
    if (context == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return private_eos_immediate_status(
            status, ADVC_VAAPI_DECODE_EOS_RESULT_INVALID_CONTEXT,
            ADVC_VAAPI_DECODE_EOS_FAILED,
            ADVC_VAAPI_DECODE_EOS_STATUS_FAILED);
    }
    result =
        private_eos_progress_locked(runtime, context, max_outputs, status);
    pthread_mutex_unlock(&runtime->mutex);
    return result;
}

static int queue_bytes_with_drain(
    struct advc_vaapi_decode_runtime *runtime,
    struct advc_vaapi_context *context, const uint8_t *data, size_t size,
    uint64_t pts_ns, uint32_t flags) {
    struct timespec pause = {0, 1000000};
    unsigned int attempt;
    for (attempt = 0; attempt < 5000u; ++attempt) {
        int drained;
        if (queue_bytes(context, data, size, pts_ns, flags) == 0) return 0;
        if (errno != EAGAIN) return -1;
        drained = drain_one_output(runtime, context);
        if (drained < 0) return -1;
        if (drained == 0) nanosleep(&pause, NULL);
    }
    errno = ETIMEDOUT;
    return -1;
}

struct advc_vaapi_decode_runtime *advc_vaapi_decode_runtime_create(
    const char *socket_path, const struct advc_vaapi_policy *policy) {
    struct advc_vaapi_decode_runtime *runtime;
    unsigned int i;
    if (socket_path == NULL || socket_path[0] == '\0' || policy == NULL) {
        errno = EINVAL;
        return NULL;
    }
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
    /* MediaCodec round-trips timestamps at microsecond precision. */
    runtime->next_pts_ns = ADVC_VAAPI_PTS_STEP_NS;
    for (i = 0; i < ADVC_VAAPI_CONTEXT_SLOTS; ++i)
        runtime->contexts[i].broker_fd = -1;
    for (i = 0; i < ADVC_VAAPI_SURFACE_SLOTS; ++i) {
        runtime->surfaces[i].acquire_fence_fd = -1;
    }
    if (configure_consumer_policy(runtime) < 0) {
        pthread_mutex_destroy(&runtime->mutex);
        free(runtime->socket_path);
        free(runtime);
        return NULL;
    }
    return runtime;
}

void advc_vaapi_decode_runtime_destroy(
    struct advc_vaapi_decode_runtime *runtime) {
    unsigned int i;
    uint32_t detail = 0;
    if (runtime == NULL) return;
    pthread_mutex_lock(&runtime->mutex);
    for (i = 0; i < ADVC_VAAPI_SURFACE_SLOTS; ++i) {
        if (runtime->surfaces[i].used)
            release_surface_output(runtime, &runtime->surfaces[i]);
    }
    for (i = 0; i < ADVC_VAAPI_CONTEXT_SLOTS; ++i) {
        int close_status = ADVC_STATUS_BAD_MESSAGE;
        if (!runtime->contexts[i].used) continue;
        reset_staging(&runtime->contexts[i]);
        if (runtime->contexts[i].broker_session != 0)
            close_status = advc_client_close_session(
                runtime->contexts[i].broker_fd,
                runtime->contexts[i].broker_session, &detail);
        if (runtime->contexts[i].broker_fd >= 0 &&
            (close_status != ADVC_STATUS_OK ||
             advc_client_recycle_broker_socket(
                 runtime->contexts[i].broker_fd) < 0))
            close(runtime->contexts[i].broker_fd);
    }
    for (i = 0; i < ADVC_VAAPI_BUFFER_SLOTS; ++i)
        free(runtime->buffers[i].data);
    pthread_mutex_unlock(&runtime->mutex);
    pthread_mutex_destroy(&runtime->mutex);
    free(runtime->socket_path);
    free(runtime);
}

VAStatus advc_vaapi_decode_create_config(
    struct advc_vaapi_decode_runtime *runtime, VAProfile profile,
    VAEntrypoint entrypoint, VAConfigAttrib *attributes, int num_attributes,
    VAConfigID *config_id) {
    unsigned int i;
    int j;
    if (runtime == NULL || config_id == NULL || num_attributes < 0 ||
        (num_attributes > 0 && attributes == NULL))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!profile_supported(runtime, profile))
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    if (entrypoint != VAEntrypointVLD)
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    for (j = 0; j < num_attributes; ++j) {
        if (attributes[j].type != VAConfigAttribRTFormat ||
            (attributes[j].value & VA_RT_FORMAT_YUV420) == 0)
            return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
    }
    pthread_mutex_lock(&runtime->mutex);
    for (i = 0; i < ADVC_VAAPI_CONFIG_SLOTS; ++i) {
        if (!runtime->configs[i].used) {
            runtime->configs[i].used = 1;
            runtime->configs[i].profile = profile;
            *config_id = make_id(ADVC_VAAPI_TYPE_CONFIG, i);
            pthread_mutex_unlock(&runtime->mutex);
            return VA_STATUS_SUCCESS;
        }
    }
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
}

VAStatus advc_vaapi_decode_destroy_config(
    struct advc_vaapi_decode_runtime *runtime, VAConfigID config_id) {
    struct advc_vaapi_config *config;
    unsigned int i;
    if (runtime == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    pthread_mutex_lock(&runtime->mutex);
    config = get_config(runtime, config_id);
    if (config == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    for (i = 0; i < ADVC_VAAPI_CONTEXT_SLOTS; ++i) {
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

VAStatus advc_vaapi_decode_query_config(
    struct advc_vaapi_decode_runtime *runtime, VAConfigID config_id,
    VAProfile *profile, VAEntrypoint *entrypoint, VAConfigAttrib *attributes,
    int *num_attributes) {
    struct advc_vaapi_config *config;
    if (runtime == NULL || profile == NULL || entrypoint == NULL ||
        num_attributes == NULL)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&runtime->mutex);
    config = get_config(runtime, config_id);
    if (config == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    *profile = config->profile;
    *entrypoint = VAEntrypointVLD;
    if (attributes != NULL) {
        attributes[0].type = VAConfigAttribRTFormat;
        attributes[0].value = VA_RT_FORMAT_YUV420;
    }
    *num_attributes = 1;
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_decode_create_surfaces(
    struct advc_vaapi_decode_runtime *runtime, unsigned int format,
    unsigned int width, unsigned int height, VASurfaceID *surfaces,
    unsigned int num_surfaces, VASurfaceAttrib *attributes,
    unsigned int num_attributes) {
    unsigned int allocated[ADVC_VAAPI_SURFACE_SLOTS];
    unsigned int allocated_count = 0;
    uint32_t max_width;
    uint32_t max_height;
    unsigned int i;
    unsigned int j;
    if (runtime == NULL || surfaces == NULL || num_surfaces == 0 ||
        num_surfaces > ADVC_VAAPI_SURFACE_SLOTS ||
        format != VA_RT_FORMAT_YUV420 || width == 0 || height == 0)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    advertised_surface_limits(&runtime->policy, &max_width, &max_height);
    if (width > max_width || height > max_height)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    for (j = 0; j < num_attributes; ++j) {
        if (attributes == NULL) return VA_STATUS_ERROR_INVALID_PARAMETER;
        if (attributes[j].type == VASurfaceAttribPixelFormat &&
            (attributes[j].value.type != VAGenericValueTypeInteger ||
             (uint32_t)attributes[j].value.value.i != VA_FOURCC_NV12))
            return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        if (attributes[j].type == VASurfaceAttribMemoryType &&
            (attributes[j].value.type != VAGenericValueTypeInteger ||
             ((uint32_t)attributes[j].value.value.i &
              VA_SURFACE_ATTRIB_MEM_TYPE_VA) == 0))
            return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        if (attributes[j].type == VASurfaceAttribExternalBufferDescriptor)
            return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }
    pthread_mutex_lock(&runtime->mutex);
    for (i = 0; i < ADVC_VAAPI_SURFACE_SLOTS &&
                allocated_count < num_surfaces;
         ++i) {
        if (!runtime->surfaces[i].used) {
            struct advc_vaapi_surface *surface = &runtime->surfaces[i];
            memset(surface, 0, sizeof(*surface));
            surface->used = 1;
            surface->width = width;
            surface->height = height;
            surface->acquire_fence_fd = -1;
            allocated[allocated_count] = i;
            surfaces[allocated_count] =
                make_id(ADVC_VAAPI_TYPE_SURFACE, i);
            ++allocated_count;
        }
    }
    if (allocated_count != num_surfaces) {
        for (i = 0; i < allocated_count; ++i)
            memset(&runtime->surfaces[allocated[i]], 0,
                   sizeof(runtime->surfaces[allocated[i]]));
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_decode_destroy_surfaces(
    struct advc_vaapi_decode_runtime *runtime, VASurfaceID *surfaces,
    int num_surfaces) {
    int i;
    if (runtime == NULL || num_surfaces < 0 ||
        (num_surfaces > 0 && surfaces == NULL))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&runtime->mutex);
    for (i = 0; i < num_surfaces; ++i) {
        struct advc_vaapi_surface *surface = get_surface(runtime, surfaces[i]);
        if (surface == NULL) {
            pthread_mutex_unlock(&runtime->mutex);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
    }
    for (i = 0; i < num_surfaces; ++i) {
        struct advc_vaapi_surface *surface = get_surface(runtime, surfaces[i]);
        release_surface_output(runtime, surface);
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

VAStatus advc_vaapi_decode_query_surface_attributes(
    struct advc_vaapi_decode_runtime *runtime, VAConfigID config_id,
    VASurfaceAttrib *attributes, unsigned int *num_attributes) {
    const unsigned int needed = 6;
    struct advc_vaapi_config *config;
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
    profile_limits(&runtime->policy, config->profile, &max_width,
                   &max_height);
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
    set_surface_attribute(&attributes[0], VASurfaceAttribPixelFormat,
                          (int)VA_FOURCC_NV12,
                          VA_SURFACE_ATTRIB_GETTABLE |
                              VA_SURFACE_ATTRIB_SETTABLE);
    set_surface_attribute(&attributes[1], VASurfaceAttribMinWidth, 16,
                          VA_SURFACE_ATTRIB_GETTABLE);
    set_surface_attribute(&attributes[2], VASurfaceAttribMaxWidth,
                          (int)max_width,
                          VA_SURFACE_ATTRIB_GETTABLE);
    set_surface_attribute(&attributes[3], VASurfaceAttribMinHeight, 16,
                          VA_SURFACE_ATTRIB_GETTABLE);
    set_surface_attribute(&attributes[4], VASurfaceAttribMaxHeight,
                          (int)max_height,
                          VA_SURFACE_ATTRIB_GETTABLE);
    set_surface_attribute(&attributes[5], VASurfaceAttribMemoryType,
                          VA_SURFACE_ATTRIB_MEM_TYPE_VA |
                              VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                          VA_SURFACE_ATTRIB_GETTABLE |
                              VA_SURFACE_ATTRIB_SETTABLE);
    *num_attributes = needed;
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_decode_create_context(
    struct advc_vaapi_decode_runtime *runtime, VAConfigID config_id, int width,
    int height, int flag, VASurfaceID *targets, int num_targets,
    VAContextID *context_id) {
    struct advc_client_session_config session_config;
    struct advc_vaapi_config *config;
    uint64_t features = 0;
    uint32_t max_payload = 0;
    uint32_t detail = 0;
    uint32_t session_id = 0;
    uint32_t max_width;
    uint32_t max_height;
    unsigned int i;
    int fd = -1;
    int status;
    (void)flag;
    if (runtime == NULL || context_id == NULL || width < 16 || height < 16 ||
        num_targets < 0 || (num_targets > 0 && targets == NULL))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&runtime->mutex);
    config = get_config(runtime, config_id);
    if (config == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    profile_limits(&runtime->policy, config->profile, &max_width,
                   &max_height);
    if ((uint32_t)width > max_width || (uint32_t)height > max_height) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    }
    for (i = 0; i < (unsigned int)num_targets; ++i) {
        struct advc_vaapi_surface *surface = get_surface(runtime, targets[i]);
        if (surface == NULL || surface->width != (uint32_t)width ||
            surface->height != (uint32_t)height) {
            pthread_mutex_unlock(&runtime->mutex);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
    }
    for (i = 0; i < ADVC_VAAPI_CONTEXT_SLOTS; ++i)
        if (!runtime->contexts[i].used) break;
    if (i == ADVC_VAAPI_CONTEXT_SLOTS) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }
    fd = advc_client_connect_bounded(
        runtime->socket_path, ADVC_CLIENT_CONNECT_TIMEOUT_MS);
    if (fd < 0 ||
        advc_client_hello(fd,
                          ADVC_FEATURE_DECODE | ADVC_FEATURE_DECODE_PRIME |
                              ADVC_FEATURE_DMABUF | ADVC_FEATURE_NATIVE_FENCE |
                              ADVC_FEATURE_ASYNC_DECODE_PRIME |
                              ADVC_FEATURE_DECODE_QCOM_MODIFIER,
                          &features, &max_payload) < 0 ||
        (features & (ADVC_FEATURE_DECODE | ADVC_FEATURE_DECODE_PRIME)) !=
            (ADVC_FEATURE_DECODE | ADVC_FEATURE_DECODE_PRIME) ||
        max_payload < ADVC_TRANSFER_PRIME_REPLY_SIZE) {
        if (fd >= 0) close(fd);
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    memset(&session_config, 0, sizeof(session_config));
    session_config.mime = profile_mime(config->profile);
    session_config.direction = ADVC_DIRECTION_DECODE;
    session_config.width = (uint32_t)width;
    session_config.height = (uint32_t)height;
    session_config.transport = ADVC_TRANSPORT_AHARDWAREBUFFER;
    status = advc_client_create_session(fd, &session_config, &session_id,
                                        &detail);
    if (status != ADVC_STATUS_OK) {
        close(fd);
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    memset(&runtime->contexts[i], 0, sizeof(runtime->contexts[i]));
    runtime->contexts[i].used = 1;
    runtime->contexts[i].config_id = config_id;
    runtime->contexts[i].broker_fd = fd;
    runtime->contexts[i].broker_session = session_id;
    runtime->contexts[i].width = (uint32_t)width;
    runtime->contexts[i].height = (uint32_t)height;
    runtime->contexts[i].codec = codec_for_profile(config->profile);
    runtime->contexts[i].async_decode_prime =
        (features & ADVC_FEATURE_ASYNC_DECODE_PRIME) != 0 &&
        exact_env("ADVC_VAAPI_ASYNC_EXPORT",
                  "candidate-firefox-bframe-v1");
    runtime->contexts[i].broker_qcom_modifier =
        (features & ADVC_FEATURE_DECODE_QCOM_MODIFIER) != 0 ? 1 : 0;
    if (runtime->modifier_policy.mode == ADVC_VAAPI_MODIFIER_QCOM) {
        runtime->contexts[i].async_decode_prime = 0;
        if (!runtime->contexts[i].broker_qcom_modifier ||
            !runtime->consumer.accepts_qcom_compressed_nv12) {
            uint32_t close_detail = 0;
            trace_decode_failure("forced-qcom-unsupported", ENOTSUP);
            (void)advc_client_close_session(fd, session_id, &close_detail);
            close(fd);
            memset(&runtime->contexts[i], 0,
                   sizeof(runtime->contexts[i]));
            runtime->contexts[i].broker_fd = -1;
            pthread_mutex_unlock(&runtime->mutex);
            return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        }
    } else if (runtime->modifier_policy.mode ==
               ADVC_VAAPI_MODIFIER_LINEAR) {
        runtime->contexts[i].broker_qcom_modifier = -1;
    }
    runtime->contexts[i].target = VA_INVALID_SURFACE;
    advc_vaapi_decode_eos_state_init(&runtime->contexts[i].eos);
    *context_id = make_id(ADVC_VAAPI_TYPE_CONTEXT, i);
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_decode_destroy_context(
    struct advc_vaapi_decode_runtime *runtime, VAContextID context_id) {
    struct advc_vaapi_context *context;
    unsigned int i;
    uint32_t detail = 0;
    if (runtime == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    pthread_mutex_lock(&runtime->mutex);
    context = get_context(runtime, context_id);
    if (context == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    for (i = 0; i < ADVC_VAAPI_SURFACE_SLOTS; ++i) {
        if (runtime->surfaces[i].used &&
            runtime->surfaces[i].owner_context == context_id)
            release_surface_output(runtime, &runtime->surfaces[i]);
    }
    reset_staging(context);
    (void)advc_client_flush(context->broker_fd, context->broker_session,
                            &detail);
    int close_status = advc_client_close_session(
        context->broker_fd, context->broker_session, &detail);
    if (close_status != ADVC_STATUS_OK ||
        advc_client_recycle_broker_socket(context->broker_fd) < 0)
        close(context->broker_fd);
    memset(context, 0, sizeof(*context));
    context->broker_fd = -1;
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_decode_create_buffer(
    struct advc_vaapi_decode_runtime *runtime, VAContextID context_id,
    VABufferType type, unsigned int size, unsigned int num_elements, void *data,
    VABufferID *buffer_id) {
    struct advc_vaapi_buffer *buffer;
    struct advc_vaapi_context *context;
    size_t total;
    unsigned int i;
    if (runtime == NULL || buffer_id == NULL || size == 0 || num_elements == 0 ||
        size > SIZE_MAX / num_elements)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    total = (size_t)size * num_elements;
    if (total > ADVC_MAX_INPUT_BYTES)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&runtime->mutex);
    context = get_context(runtime, context_id);
    if (context == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (!buffer_shape_supported(context, type, size, num_elements)) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    }
    for (i = 0; i < ADVC_VAAPI_BUFFER_SLOTS; ++i)
        if (!runtime->buffers[i].used) break;
    if (i == ADVC_VAAPI_BUFFER_SLOTS) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }
    buffer = &runtime->buffers[i];
    memset(buffer, 0, sizeof(*buffer));
    buffer->data = calloc(1, total);
    if (buffer->data == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    buffer->used = 1;
    buffer->context_id = context_id;
    buffer->type = type;
    buffer->element_size = size;
    buffer->capacity_elements = num_elements;
    buffer->num_elements = num_elements;
    if (data != NULL) memcpy(buffer->data, data, total);
    *buffer_id = make_id(ADVC_VAAPI_TYPE_BUFFER, i);
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_decode_buffer_set_num_elements(
    struct advc_vaapi_decode_runtime *runtime, VABufferID buffer_id,
    unsigned int num_elements) {
    struct advc_vaapi_buffer *buffer;
    if (runtime == NULL || num_elements == 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&runtime->mutex);
    buffer = get_buffer(runtime, buffer_id);
    if (buffer == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (num_elements > buffer->capacity_elements) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    buffer->num_elements = num_elements;
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_decode_map_buffer(
    struct advc_vaapi_decode_runtime *runtime, VABufferID buffer_id,
    void **mapped) {
    struct advc_vaapi_buffer *buffer;
    if (runtime == NULL || mapped == NULL)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&runtime->mutex);
    buffer = get_buffer(runtime, buffer_id);
    if (buffer == NULL || buffer->mapped) {
        pthread_mutex_unlock(&runtime->mutex);
        return buffer == NULL ? VA_STATUS_ERROR_INVALID_BUFFER :
                                VA_STATUS_ERROR_OPERATION_FAILED;
    }
    buffer->mapped = 1;
    *mapped = buffer->data;
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_decode_unmap_buffer(
    struct advc_vaapi_decode_runtime *runtime, VABufferID buffer_id) {
    struct advc_vaapi_buffer *buffer;
    if (runtime == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    pthread_mutex_lock(&runtime->mutex);
    buffer = get_buffer(runtime, buffer_id);
    if (buffer == NULL || !buffer->mapped) {
        pthread_mutex_unlock(&runtime->mutex);
        return buffer == NULL ? VA_STATUS_ERROR_INVALID_BUFFER :
                                VA_STATUS_ERROR_OPERATION_FAILED;
    }
    buffer->mapped = 0;
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_decode_destroy_buffer(
    struct advc_vaapi_decode_runtime *runtime, VABufferID buffer_id) {
    struct advc_vaapi_buffer *buffer;
    if (runtime == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    pthread_mutex_lock(&runtime->mutex);
    buffer = get_buffer(runtime, buffer_id);
    if (buffer == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_decode_begin_picture(
    struct advc_vaapi_decode_runtime *runtime, VAContextID context_id,
    VASurfaceID target) {
    struct advc_vaapi_context *context;
    struct advc_vaapi_surface *surface;
    VAStatus sync_status;
    if (runtime == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    pthread_mutex_lock(&runtime->mutex);
    context = get_context(runtime, context_id);
    surface = get_surface(runtime, target);
    if (context == NULL || surface == NULL || context->picture_active ||
        context->eos.phase != ADVC_VAAPI_DECODE_EOS_OPEN ||
        surface->width != context->width || surface->height != context->height) {
        pthread_mutex_unlock(&runtime->mutex);
        if (context == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
        if (surface == NULL) return VA_STATUS_ERROR_INVALID_SURFACE;
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (surface->state == ADVC_SURFACE_PENDING) {
        pthread_mutex_unlock(&runtime->mutex);
        sync_status = advc_vaapi_decode_sync_surface(
            runtime, target, ADVC_VAAPI_SYNC_LIMIT_NS);
        if (sync_status != VA_STATUS_SUCCESS) return sync_status;
        pthread_mutex_lock(&runtime->mutex);
        context = get_context(runtime, context_id);
        surface = get_surface(runtime, target);
        if (context == NULL || surface == NULL || context->picture_active ||
            context->eos.phase != ADVC_VAAPI_DECODE_EOS_OPEN ||
            surface->width != context->width ||
            surface->height != context->height) {
            pthread_mutex_unlock(&runtime->mutex);
            return context == NULL ? VA_STATUS_ERROR_INVALID_CONTEXT :
                   surface == NULL ? VA_STATUS_ERROR_INVALID_SURFACE :
                                     VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }
    if (surface->state == ADVC_SURFACE_ERROR) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    release_surface_output(runtime, surface);
    reset_staging(context);
    context->picture_active = 1;
    context->target = target;
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_decode_render_picture(
    struct advc_vaapi_decode_runtime *runtime, VAContextID context_id,
    VABufferID *buffers, int num_buffers) {
    struct advc_vaapi_context *context;
    VAStatus result = VA_STATUS_SUCCESS;
    int i;
    if (runtime == NULL || num_buffers < 0 ||
        (num_buffers > 0 && buffers == NULL))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&runtime->mutex);
    context = get_context(runtime, context_id);
    if (context == NULL || !context->picture_active) {
        pthread_mutex_unlock(&runtime->mutex);
        return context == NULL ? VA_STATUS_ERROR_INVALID_CONTEXT :
                                 VA_STATUS_ERROR_OPERATION_FAILED;
    }
    for (i = 0; i < num_buffers; ++i) {
        struct advc_vaapi_buffer *buffer = get_buffer(runtime, buffers[i]);
        if (buffer == NULL || buffer->context_id != context_id ||
            buffer->mapped) {
            result = buffer == NULL || buffer->context_id != context_id
                         ? VA_STATUS_ERROR_INVALID_BUFFER
                         : VA_STATUS_ERROR_OPERATION_FAILED;
            break;
        }
        if (buffer->type == VAPictureParameterBufferType) {
            if (context->have_picture) {
                result = VA_STATUS_ERROR_INVALID_PARAMETER;
                break;
            }
            if (context->codec == ADVC_DECODE_CODEC_HEVC)
                memcpy(&context->hevc_picture, buffer->data,
                       sizeof(context->hevc_picture));
            else if (context->codec == ADVC_DECODE_CODEC_VP9)
                memcpy(&context->vp9_picture, buffer->data,
                       sizeof(context->vp9_picture));
            else
                memcpy(&context->picture, buffer->data,
                       sizeof(context->picture));
            context->have_picture = 1;
        } else if (buffer->type == VAIQMatrixBufferType) {
            if (context->have_iq) {
                result = VA_STATUS_ERROR_INVALID_PARAMETER;
                break;
            }
            if (context->codec == ADVC_DECODE_CODEC_HEVC)
                memcpy(&context->hevc_iq, buffer->data,
                       sizeof(context->hevc_iq));
            else if (context->codec == ADVC_DECODE_CODEC_H264)
                memcpy(&context->iq, buffer->data, sizeof(context->iq));
            else {
                result = VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
                break;
            }
            context->have_iq = 1;
        } else if (buffer->type == VASliceParameterBufferType) {
            if (append_slice_parameters(context, buffer) < 0) {
                result = status_for_errno(errno);
                break;
            }
        } else if (buffer->type == VASliceDataBufferType) {
            if (set_slice_data(context, buffer) < 0) {
                result = status_for_errno(errno);
                break;
            }
        } else {
            result = VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
            break;
        }
    }
    /* VA buffers remain application-owned until vaDestroyBuffer(). */
    pthread_mutex_unlock(&runtime->mutex);
    return result;
}

static VAStatus end_hevc_picture_locked(
    struct advc_vaapi_decode_runtime *runtime, VAContextID context_id,
    struct advc_vaapi_context *context, struct advc_vaapi_surface *surface) {
    struct advc_hevc_parameter_input parameters;
    struct advc_hevc_codec_config codec_config;
    struct advc_hevc_slice_input *slices = NULL;
    uint8_t *access_unit = NULL;
    size_t access_unit_size = 0;
    uint32_t pps_id = 0;
    uint64_t pts_ns;
    int is_irap = 0;
    VAStatus result = VA_STATUS_ERROR_DECODING_ERROR;
    size_t i;
    if (surface == NULL || !context->have_picture ||
        context->slice_count == 0 || context->slice_data == NULL ||
        context->slice_data_bound_count != context->slice_count) {
        trace_decode_failure("hevc-incomplete-picture", EINVAL);
        goto out;
    }
    slices = calloc(context->slice_count, sizeof(*slices));
    if (slices == NULL) {
        result = VA_STATUS_ERROR_ALLOCATION_FAILED;
        goto out;
    }
    for (i = 0; i < context->slice_count; ++i) {
        const VASliceParameterBufferHEVC *slice =
            &context->hevc_slice_parameters[i];
        if (slice->slice_data_flag != VA_SLICE_DATA_FLAG_ALL ||
            slice->slice_data_offset > context->slice_data_size ||
            slice->slice_data_size >
                context->slice_data_size - slice->slice_data_offset) {
            result = VA_STATUS_ERROR_INVALID_PARAMETER;
            trace_decode_failure("hevc-slice-range", EINVAL);
            goto out;
        }
        slices[i].data = context->slice_data + slice->slice_data_offset;
        slices[i].size = slice->slice_data_size;
        slices[i].expected_slice_segment_address =
            slice->slice_segment_address;
        slices[i].expected_last_slice =
            (uint8_t)slice->LongSliceFlags.fields.LastSliceOfPic;
    }
    if (advc_hevc_build_access_unit(
            slices, context->slice_count, &access_unit, &access_unit_size,
            &pps_id, &is_irap) < 0) {
        result = status_for_errno(errno);
        trace_decode_failure("hevc-access-unit", errno);
        goto out;
    }
    map_hevc_parameters(context, &parameters);
    if (advc_hevc_build_codec_config(&parameters, pps_id, &codec_config) < 0) {
        result = status_for_errno(errno);
        trace_decode_failure("hevc-codec-config", errno);
        goto out;
    }
    if (!context->configured && !is_irap) {
        trace_decode_failure("hevc-initial-non-irap", EPROTO);
        goto out;
    }
    if (context->configured && !is_irap &&
        (context->hevc_last_config.size != codec_config.size ||
         memcmp(context->hevc_last_config.data, codec_config.data,
                codec_config.size) != 0)) {
        trace_decode_failure("hevc-non-irap-config-change", EPROTO);
        goto out;
    }
    if (runtime->next_pts_ns >
        (uint64_t)INT64_MAX - ADVC_VAAPI_PTS_STEP_NS) {
        result = VA_STATUS_ERROR_OPERATION_FAILED;
        trace_decode_failure("hevc-pts-exhausted", EOVERFLOW);
        goto out;
    }
    pts_ns = runtime->next_pts_ns;
    runtime->next_pts_ns += ADVC_VAAPI_PTS_STEP_NS;
    if (!context->configured ||
        context->hevc_last_config.size != codec_config.size ||
        memcmp(context->hevc_last_config.data, codec_config.data,
               codec_config.size) != 0) {
        if (queue_bytes_with_drain(runtime, context, codec_config.data,
                                   codec_config.size, pts_ns,
                                   ADVC_FLAG_CODEC_CONFIG) < 0) {
            result = VA_STATUS_ERROR_OPERATION_FAILED;
            trace_decode_failure("hevc-queue-codec-config", errno);
            goto out;
        }
        context->hevc_last_config = codec_config;
        context->configured = 1;
    }
    if (reserve_surface_linear(context, context_id, surface, pts_ns) < 0) {
        result = VA_STATUS_ERROR_OPERATION_FAILED;
        goto out;
    }
    if (queue_bytes_with_drain(runtime, context, access_unit,
                               access_unit_size, pts_ns,
                               is_irap ? ADVC_FLAG_KEY_FRAME : 0) < 0) {
        if (surface->prime_reserved)
            release_surface_output(runtime, surface);
        result = VA_STATUS_ERROR_OPERATION_FAILED;
        trace_decode_failure("hevc-queue-access-unit", errno);
        goto out;
    }
    surface->state = ADVC_SURFACE_PENDING;
    surface->pts_ns = pts_ns;
    surface->owner_context = context_id;
    surface->owner_session = context->broker_session;
    result = VA_STATUS_SUCCESS;
out:
    free(slices);
    free(access_unit);
    reset_staging(context);
    return result;
}

static VAStatus end_vp9_picture_locked(
    struct advc_vaapi_decode_runtime *runtime, VAContextID context_id,
    struct advc_vaapi_context *context, struct advc_vaapi_surface *surface) {
    uint8_t *access_unit = NULL;
    size_t access_unit_size = 0;
    uint64_t pts_ns;
    int key_frame = 0;
    VAStatus result = VA_STATUS_ERROR_DECODING_ERROR;
    if (surface == NULL || !context->have_picture ||
        context->slice_count != 1 || context->slice_data == NULL ||
        context->slice_data_bound_count != context->slice_count) {
        trace_decode_failure("vp9-incomplete-picture", EINVAL);
        goto out;
    }
    if (advc_vp9_build_access_unit(
            &context->vp9_picture, &context->vp9_slice_parameters[0],
            context->slice_data, context->slice_data_size, context->width,
            context->height, &access_unit, &access_unit_size,
            &key_frame) < 0) {
        result = status_for_errno(errno);
        trace_decode_failure("vp9-access-unit", errno);
        goto out;
    }
    if (!context->configured && !key_frame) {
        trace_decode_failure("vp9-initial-inter-frame", EPROTO);
        goto out;
    }
    if (runtime->next_pts_ns >
        (uint64_t)INT64_MAX - ADVC_VAAPI_PTS_STEP_NS) {
        result = VA_STATUS_ERROR_OPERATION_FAILED;
        trace_decode_failure("vp9-pts-exhausted", EOVERFLOW);
        goto out;
    }
    pts_ns = runtime->next_pts_ns;
    runtime->next_pts_ns += ADVC_VAAPI_PTS_STEP_NS;
    if (reserve_surface_linear(context, context_id, surface, pts_ns) < 0) {
        result = VA_STATUS_ERROR_OPERATION_FAILED;
        goto out;
    }
    if (queue_bytes_with_drain(runtime, context, access_unit,
                               access_unit_size, pts_ns,
                               key_frame ? ADVC_FLAG_KEY_FRAME : 0) < 0) {
        if (surface->prime_reserved)
            release_surface_output(runtime, surface);
        result = VA_STATUS_ERROR_OPERATION_FAILED;
        trace_decode_failure("vp9-queue-access-unit", errno);
        goto out;
    }
    context->configured = 1;
    surface->state = ADVC_SURFACE_PENDING;
    surface->pts_ns = pts_ns;
    surface->owner_context = context_id;
    surface->owner_session = context->broker_session;
    result = VA_STATUS_SUCCESS;
out:
    free(access_unit);
    reset_staging(context);
    return result;
}

VAStatus advc_vaapi_decode_end_picture(
    struct advc_vaapi_decode_runtime *runtime, VAContextID context_id) {
    struct advc_vaapi_context *context;
    struct advc_vaapi_surface *surface;
    struct advc_vaapi_config *config;
    struct advc_h264_parameter_input parameters;
    struct advc_h264_codec_config codec_config;
    struct advc_h264_reference_override reference_override;
    struct advc_h264_slice_input *slices = NULL;
    uint8_t *access_unit = NULL;
    size_t access_unit_size = 0;
    uint32_t pps_id = 0;
    uint64_t pts_ns;
    int is_idr = 0;
    int config_changed;
    int inband_config_update = 0;
    VAStatus result = VA_STATUS_ERROR_DECODING_ERROR;
    size_t i;
    if (runtime == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    pthread_mutex_lock(&runtime->mutex);
    context = get_context(runtime, context_id);
    if (context == NULL || !context->picture_active) {
        pthread_mutex_unlock(&runtime->mutex);
        return context == NULL ? VA_STATUS_ERROR_INVALID_CONTEXT :
                                 VA_STATUS_ERROR_OPERATION_FAILED;
    }
    surface = get_surface(runtime, context->target);
    config = get_config(runtime, context->config_id);
    if (surface == NULL || config == NULL || !context->have_picture ||
        context->slice_count == 0 || context->slice_data == NULL ||
        context->slice_data_bound_count != context->slice_count) {
        trace_decode_failure("incomplete-picture", EINVAL);
        goto out;
    }
    if (config->profile == VAProfileHEVCMain) {
        result = end_hevc_picture_locked(runtime, context_id, context,
                                         surface);
        pthread_mutex_unlock(&runtime->mutex);
        return result;
    }
    if (config->profile == VAProfileVP9Profile0) {
        result = end_vp9_picture_locked(runtime, context_id, context,
                                        surface);
        pthread_mutex_unlock(&runtime->mutex);
        return result;
    }
    slices = calloc(context->slice_count, sizeof(*slices));
    if (slices == NULL) {
        result = VA_STATUS_ERROR_ALLOCATION_FAILED;
        goto out;
    }
    for (i = 0; i < context->slice_count; ++i) {
        const VASliceParameterBufferH264 *slice =
            &context->slice_parameters[i];
        if (slice->slice_data_flag != VA_SLICE_DATA_FLAG_ALL ||
            slice->slice_data_offset > context->slice_data_size ||
            slice->slice_data_size >
                context->slice_data_size - slice->slice_data_offset) {
            result = VA_STATUS_ERROR_INVALID_PARAMETER;
            trace_decode_failure("slice-range", EINVAL);
            goto out;
        }
        slices[i].data = context->slice_data + slice->slice_data_offset;
        slices[i].size = slice->slice_data_size;
        slices[i].expected_first_mb_in_slice = slice->first_mb_in_slice;
        slices[i].expected_slice_type = slice->slice_type;
    }
    if (advc_h264_build_access_unit(slices, context->slice_count, &access_unit,
                                    &access_unit_size, &pps_id, &is_idr) < 0) {
        trace_decode_failure("access-unit", errno);
        result = status_for_errno(errno);
        goto out;
    }
    if (exact_env("ADVC_VAAPI_TRACE", "1")) {
        fprintf(stderr,
                "advc-vaapi: slices=%zu first-size=%zu first-bytes=%02x%02x%02x%02x "
                "is-idr=%d pps=%u expected-type=%u\n",
                context->slice_count, slices[0].size,
                slices[0].size > 0 ? slices[0].data[0] : 0,
                slices[0].size > 1 ? slices[0].data[1] : 0,
                slices[0].size > 2 ? slices[0].data[2] : 0,
                slices[0].size > 3 ? slices[0].data[3] : 0, is_idr, pps_id,
                slices[0].expected_slice_type);
    }
    map_h264_parameters(context, config->profile, &parameters);
    if (config->profile == VAProfileH264Main) {
        if (exact_env("ADVC_VAAPI_H264_REORDER_BOUND",
                      "validated-main-reorder4-v1")) {
            parameters.vui_bitstream_restriction_flag = 1;
            parameters.max_num_reorder_frames = 4;
            parameters.max_dec_frame_buffering =
                parameters.num_ref_frames > 4 ? parameters.num_ref_frames : 4;
        } else if (exact_env("ADVC_VAAPI_H264_REORDER_BOUND",
                             "candidate-firefox-pts-reorder2-v1")) {
            /*
             * The Firefox acceptance stream reports two B-frames.  Preserve
             * reference buffering while advertising the matching display
             * reorder depth to Codec2; the release candidate is kept behind
             * an exact token until real seek/teardown playback passes.
             */
            parameters.vui_bitstream_restriction_flag = 1;
            parameters.max_num_reorder_frames = 2;
            parameters.max_dec_frame_buffering =
                parameters.num_ref_frames > 2 ? parameters.num_ref_frames : 2;
        } else if (exact_env("ADVC_VAAPI_H264_REORDER_BOUND",
                             "validated-main-reorder1-v1") ||
                   exact_env("ADVC_VAAPI_H264_REORDER_BOUND",
                             "candidate-firefox-pts-reorder1-v1")) {
            /*
             * Real Firefox ESR/RDD playback on the target Qualcomm decoder
             * needs one frame of advertised display reorder.  Zero changes
             * decode order and two-or-more holds the first VA surface beyond
             * Firefox's submission window.  Reference buffering remains the
             * stream-provided value (four for the acceptance stream).
             */
            parameters.vui_bitstream_restriction_flag = 1;
            parameters.max_num_reorder_frames = 1;
            parameters.max_dec_frame_buffering =
                parameters.num_ref_frames > 1 ? parameters.num_ref_frames : 1;
        } else if (exact_env("ADVC_VAAPI_H264_REORDER_BOUND",
                             "candidate-firefox-pts-reorder0-v1")) {
            /*
             * Firefox blocks in vaExportSurfaceHandle() after submitting only
             * IDR/P/B.  It owns presentation ordering by PTS, so this isolated
             * candidate asks MediaCodec to publish decode-complete surfaces
             * without also retaining them for display ordering.  Reference
             * buffering remains four frames; compressed data and PTS are not
             * changed.  Keep this behind an exact candidate token until the
             * displayed seek/teardown gate proves it on the target device.
             */
            parameters.vui_bitstream_restriction_flag = 1;
            parameters.max_num_reorder_frames = 0;
            parameters.max_dec_frame_buffering =
                parameters.num_ref_frames > 4 ? parameters.num_ref_frames : 4;
        }
    }
    if (advc_h264_parse_reference_override(
            slices[0].data, slices[0].size, &parameters,
            &reference_override) < 0) {
        trace_decode_failure("slice-reference-defaults", errno);
        result = status_for_errno(errno);
        goto out;
    }
    if (reference_override.uses_reference_lists != 0 &&
        reference_override.override_present == 0) {
        context->pps_l0_default_active_minus1 =
            context->slice_parameters[0].num_ref_idx_l0_active_minus1;
        context->pps_l0_known = 1;
        if (reference_override.uses_list1 != 0) {
            context->pps_l1_default_active_minus1 =
                context->slice_parameters[0].num_ref_idx_l1_active_minus1;
            context->pps_l1_known = 1;
        }
    }
    parameters.num_ref_idx_l0_default_active_minus1 =
        context->pps_l0_known != 0 ?
            context->pps_l0_default_active_minus1 : 0;
    parameters.num_ref_idx_l1_default_active_minus1 =
        context->pps_l1_known != 0 ?
            context->pps_l1_default_active_minus1 : 0;
    if (exact_env("ADVC_VAAPI_TRACE", "1")) {
        fprintf(stderr,
                "advc-vaapi: pps-values qp=%d qs=%d chroma=%d/%d refs=%u/%u "
                "entropy=%u wp=%u wb=%u intra=%u poc-present=%u deblock=%u "
                "redundant=%u reorder=%u/%u\n",
                parameters.pic_init_qp_minus26,
                parameters.pic_init_qs_minus26,
                parameters.chroma_qp_index_offset,
                parameters.second_chroma_qp_index_offset,
                parameters.num_ref_idx_l0_default_active_minus1,
                parameters.num_ref_idx_l1_default_active_minus1,
                parameters.entropy_coding_mode_flag,
                parameters.weighted_pred_flag,
                parameters.weighted_bipred_idc,
                parameters.constrained_intra_pred_flag,
                parameters.bottom_field_pic_order_in_frame_present_flag,
                parameters.deblocking_filter_control_present_flag,
                parameters.redundant_pic_cnt_present_flag,
                parameters.max_num_reorder_frames,
                parameters.max_dec_frame_buffering);
    }
    if (advc_h264_build_codec_config(&parameters, pps_id, &codec_config) < 0) {
        trace_decode_failure("codec-config", errno);
        if (exact_env("ADVC_VAAPI_TRACE", "1")) {
            fprintf(stderr,
                    "advc-vaapi: profile=%u visible=%ux%u mbs=%ux%u "
                    "bitdepth=%u/%u chroma=%u sep=%u frame=%u mbaff=%u "
                    "field=%u poc=%u t8=%u wb=%u framebits=%u pocbits=%u "
                    "refs=%u\n",
                    parameters.profile_idc, parameters.visible_width,
                    parameters.visible_height,
                    parameters.picture_width_in_mbs_minus1,
                    parameters.picture_height_in_mbs_minus1,
                    parameters.bit_depth_luma_minus8,
                    parameters.bit_depth_chroma_minus8,
                    parameters.chroma_format_idc,
                    parameters.separate_colour_plane_flag,
                    parameters.frame_mbs_only_flag,
                    parameters.mb_adaptive_frame_field_flag,
                    parameters.field_pic_flag, parameters.pic_order_cnt_type,
                    parameters.transform_8x8_mode_flag,
                    parameters.weighted_bipred_idc,
                    parameters.log2_max_frame_num_minus4,
                    parameters.log2_max_pic_order_cnt_lsb_minus4,
                    parameters.num_ref_frames);
        }
        result = status_for_errno(errno);
        goto out;
    }
    if (exact_env("ADVC_VAAPI_TRACE", "1")) {
        size_t trace_byte;
        fputs("advc-vaapi: codec-config-hex=", stderr);
        for (trace_byte = 0; trace_byte < codec_config.size; ++trace_byte)
            fprintf(stderr, "%02x", codec_config.data[trace_byte]);
        fputc('\n', stderr);
    }
    config_changed = !context->configured ||
        context->last_config.size != codec_config.size ||
        memcmp(context->last_config.data, codec_config.data,
               codec_config.size) != 0;
    if (context->configured && !is_idr && config_changed) {
        size_t old_sps_size = codec_config_sps_size(&context->last_config);
        size_t new_sps_size = codec_config_sps_size(&codec_config);
        if (old_sps_size == 0 || new_sps_size == 0 ||
            old_sps_size != new_sps_size ||
            memcmp(context->last_config.data, codec_config.data,
                   new_sps_size) != 0) {
            result = VA_STATUS_ERROR_DECODING_ERROR;
            trace_decode_failure("non-idr-sps-change", EPROTO);
            goto out;
        }
    }
    if (runtime->next_pts_ns >
        (uint64_t)INT64_MAX - ADVC_VAAPI_PTS_STEP_NS) {
        result = VA_STATUS_ERROR_OPERATION_FAILED;
        trace_decode_failure("pts-exhausted", EOVERFLOW);
        goto out;
    }
    pts_ns = runtime->next_pts_ns;
    runtime->next_pts_ns += ADVC_VAAPI_PTS_STEP_NS;
    if (config_changed) {
        if (context->configured &&
            exact_env("ADVC_VAAPI_H264_INBAND_CONFIG_UPDATE",
                      "validated-pps-v1")) {
            if (advc_h264_prepend_codec_config(
                    &codec_config, &access_unit, &access_unit_size) < 0) {
                trace_decode_failure("prepend-in-band-codec-config", errno);
                result = status_for_errno(errno);
                goto out;
            }
            if (exact_env("ADVC_VAAPI_TRACE", "1"))
                fprintf(stderr,
                        "advc-vaapi: in-band-codec-config bytes=%zu idr=%d\n",
                        codec_config.size, is_idr);
            inband_config_update = 1;
        } else if (queue_bytes_with_drain(
                       runtime, context, codec_config.data,
                       codec_config.size, pts_ns,
                       ADVC_FLAG_CODEC_CONFIG) < 0) {
                trace_decode_failure("queue-codec-config", errno);
                result = VA_STATUS_ERROR_OPERATION_FAILED;
                goto out;
        }
    if (!inband_config_update) {
            context->last_config = codec_config;
            context->configured = 1;
        }
    }
    if (reserve_surface_linear(context, context_id, surface, pts_ns) < 0) {
        result = VA_STATUS_ERROR_OPERATION_FAILED;
        goto out;
    }
    if (queue_bytes_with_drain(runtime, context, access_unit,
                               access_unit_size, pts_ns,
                               is_idr ? ADVC_FLAG_KEY_FRAME : 0) < 0) {
        if (surface->prime_reserved)
            release_surface_output(runtime, surface);
        trace_decode_failure("queue-access-unit", errno);
        result = VA_STATUS_ERROR_OPERATION_FAILED;
        goto out;
    }
    if (inband_config_update) {
        /* Commit PPS state only after the prefixed access unit was queued. */
        context->last_config = codec_config;
        context->configured = 1;
    }
    surface->state = ADVC_SURFACE_PENDING;
    surface->pts_ns = pts_ns;
    surface->owner_context = context_id;
    surface->owner_session = context->broker_session;
    result = VA_STATUS_SUCCESS;
out:
    free(slices);
    free(access_unit);
    reset_staging(context);
    pthread_mutex_unlock(&runtime->mutex);
    return result;
}

static uint64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static int wait_fence(int fd, uint64_t timeout_ns) {
    struct pollfd poll_fd;
    int timeout_ms;
    int rc;
    if (fd < 0) return 0;
    timeout_ms = timeout_ns >= (uint64_t)INT_MAX * UINT64_C(1000000)
                     ? INT_MAX
                     : (int)((timeout_ns + UINT64_C(999999)) /
                             UINT64_C(1000000));
    poll_fd.fd = fd;
    poll_fd.events = POLLIN;
    do {
        rc = poll(&poll_fd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);
    if (rc > 0 && (poll_fd.revents & (POLLIN | POLLERR | POLLHUP)) != 0)
        return 0;
    if (rc == 0) errno = ETIMEDOUT;
    return -1;
}

VAStatus advc_vaapi_decode_sync_surface(
    struct advc_vaapi_decode_runtime *runtime, VASurfaceID surface_id,
    uint64_t timeout_ns) {
    struct advc_vaapi_surface *surface;
    struct advc_vaapi_context *context;
    uint64_t start;
    uint64_t limit;
    uint64_t now;
    struct timespec pause = {0, 1000000};
    int fence_fd = -1;
    if (runtime == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    limit = timeout_ns > ADVC_VAAPI_SYNC_LIMIT_NS ?
                ADVC_VAAPI_SYNC_LIMIT_NS : timeout_ns;
    start = monotonic_ns();
    for (;;) {
        unsigned int context_index;
        pthread_mutex_lock(&runtime->mutex);
        surface = get_surface(runtime, surface_id);
        if (surface == NULL) {
            pthread_mutex_unlock(&runtime->mutex);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        if (surface->state == ADVC_SURFACE_ERROR) {
            pthread_mutex_unlock(&runtime->mutex);
            return VA_STATUS_ERROR_DECODING_ERROR;
        }
        if (surface->state == ADVC_SURFACE_READY) {
            int had_fence = surface->acquire_fence_fd >= 0;
            if (had_fence)
                fence_fd = fcntl(surface->acquire_fence_fd, F_DUPFD_CLOEXEC, 0);
            pthread_mutex_unlock(&runtime->mutex);
            if (!had_fence) return VA_STATUS_SUCCESS;
            if (fence_fd < 0)
                return VA_STATUS_ERROR_OPERATION_FAILED;
            now = monotonic_ns();
            if (now < start || now - start >= limit) {
                if (fence_fd >= 0) close(fence_fd);
                return VA_STATUS_ERROR_TIMEDOUT;
            }
            if (wait_fence(fence_fd, limit - (now - start)) < 0) {
                if (fence_fd >= 0) close(fence_fd);
                return errno == ETIMEDOUT ? VA_STATUS_ERROR_TIMEDOUT :
                                            VA_STATUS_ERROR_OPERATION_FAILED;
            }
            if (fence_fd >= 0) close(fence_fd);
            return VA_STATUS_SUCCESS;
        }
        if (surface->state == ADVC_SURFACE_IDLE ||
            id_index(surface->owner_context, ADVC_VAAPI_TYPE_CONTEXT,
                     ADVC_VAAPI_CONTEXT_SLOTS, &context_index) < 0 ||
            !runtime->contexts[context_index].used) {
            pthread_mutex_unlock(&runtime->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        context = &runtime->contexts[context_index];
        (void)drain_one_output(runtime, context);
        pthread_mutex_unlock(&runtime->mutex);
        now = monotonic_ns();
        if (now < start || now - start >= limit)
            return VA_STATUS_ERROR_TIMEDOUT;
        nanosleep(&pause, NULL);
    }
}

VAStatus advc_vaapi_decode_query_surface_status(
    struct advc_vaapi_decode_runtime *runtime, VASurfaceID surface_id,
    VASurfaceStatus *status) {
    struct advc_vaapi_surface *surface;
    struct advc_vaapi_context *context;
    unsigned int context_index;
    unsigned int drain_count;
    if (runtime == NULL || status == NULL)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&runtime->mutex);
    surface = get_surface(runtime, surface_id);
    if (surface == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    /*
     * GStreamer may poll vaQuerySurfaceStatus without first calling
     * vaSyncSurface.  Dequeue is non-blocking, so service the broker here as
     * well; otherwise a short stream can remain PENDING forever even though
     * decoded output is already queued.  Drain at most the bounded surface
     * count so this status query never becomes an unbounded wait.
     */
    if (surface->state == ADVC_SURFACE_PENDING &&
        id_index(surface->owner_context, ADVC_VAAPI_TYPE_CONTEXT,
                 ADVC_VAAPI_CONTEXT_SLOTS, &context_index) == 0 &&
        runtime->contexts[context_index].used) {
        context = &runtime->contexts[context_index];
        for (drain_count = 0;
             drain_count < ADVC_VAAPI_SURFACE_SLOTS &&
             surface->state == ADVC_SURFACE_PENDING;
             ++drain_count) {
            int drained = drain_one_output(runtime, context);
            if (drained <= 0) break;
        }
    }
    if (surface->state == ADVC_SURFACE_ERROR) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    *status = surface->state == ADVC_SURFACE_PENDING ? VASurfaceRendering :
                                                       VASurfaceReady;
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_decode_export_surface(
    struct advc_vaapi_decode_runtime *runtime, VASurfaceID surface_id,
    uint32_t mem_type, uint32_t flags, void *descriptor) {
    struct advc_vaapi_surface *surface;
    VADRMPRIMESurfaceDescriptor *output = descriptor;
    VAStatus sync_status;
    uint64_t timing_started_ns = start_export_timing();
    unsigned int i;
    unsigned int p;
    int separate;
    int reserved_export = 0;
    if (runtime == NULL || descriptor == NULL ||
        mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 ||
        (flags & VA_EXPORT_SURFACE_READ_WRITE) != VA_EXPORT_SURFACE_READ_ONLY ||
        (flags & ~(VA_EXPORT_SURFACE_READ_WRITE |
                   VA_EXPORT_SURFACE_SEPARATE_LAYERS |
                   VA_EXPORT_SURFACE_COMPOSED_LAYERS)) != 0 ||
        ((flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) != 0 &&
         (flags & VA_EXPORT_SURFACE_COMPOSED_LAYERS) != 0))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&runtime->mutex);
    surface = get_surface(runtime, surface_id);
    if (surface != NULL && surface->state == ADVC_SURFACE_PENDING &&
        surface->prime_reserved &&
        advc_dmabuf_descriptor_validate(&surface->prime) == 0)
        reserved_export = 1;
    pthread_mutex_unlock(&runtime->mutex);
    /*
     * Protocol 1.8 may already own an exportable LINEAR destination while the
     * Android decoder is still waiting for future reference pictures.  Return
     * that stable dma-buf immediately; vaSyncSurface remains the content/fence
     * completion point.  Older gateways retain the bounded synchronous path.
     */
    if (!reserved_export) {
        sync_status = advc_vaapi_decode_sync_surface(
            runtime, surface_id, ADVC_VAAPI_SYNC_LIMIT_NS);
        if (sync_status != VA_STATUS_SUCCESS) {
            record_export_timing(timing_started_ns, sync_status);
            return sync_status;
        }
    }
    separate = (flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) != 0;
    pthread_mutex_lock(&runtime->mutex);
    surface = get_surface(runtime, surface_id);
    if (surface == NULL ||
        (surface->state != ADVC_SURFACE_READY &&
         !(surface->state == ADVC_SURFACE_PENDING &&
           surface->prime_reserved))) {
        pthread_mutex_unlock(&runtime->mutex);
        return surface == NULL ? VA_STATUS_ERROR_INVALID_SURFACE :
                                 VA_STATUS_ERROR_TIMEDOUT;
    }
    if (surface->prime.drm_fourcc != ADVC_DRM_FORMAT_NV12 ||
        surface->prime.object_count == 0 || surface->prime.object_count > 4 ||
        surface->prime.plane_count != 2 ||
        surface->prime.planes[0].offset > UINT32_MAX ||
        surface->prime.planes[1].offset > UINT32_MAX) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    memset(output, 0, sizeof(*output));
    for (i = 0; i < 4; ++i) output->objects[i].fd = -1;
    output->fourcc = VA_FOURCC_NV12;
    output->width = surface->prime.width;
    output->height = surface->prime.height;
    output->num_objects = surface->prime.object_count;
    for (i = 0; i < output->num_objects; ++i) {
        if (surface->prime.objects[i].size > UINT32_MAX) goto export_fail;
        output->objects[i].fd =
            fcntl(surface->prime.objects[i].fd, F_DUPFD_CLOEXEC, 0);
        if (output->objects[i].fd < 0) goto export_fail;
        output->objects[i].size = (uint32_t)surface->prime.objects[i].size;
        output->objects[i].drm_format_modifier =
            surface->prime.drm_modifier;
    }
    if (separate) {
        output->num_layers = 2;
        output->layers[0].drm_format = DRM_FORMAT_R8;
        output->layers[1].drm_format = DRM_FORMAT_GR88;
        for (p = 0; p < 2; ++p) {
            output->layers[p].num_planes = 1;
            output->layers[p].object_index[0] =
                surface->prime.planes[p].object_index;
            output->layers[p].offset[0] =
                (uint32_t)surface->prime.planes[p].offset;
            output->layers[p].pitch[0] = surface->prime.planes[p].pitch;
        }
    } else {
        output->num_layers = 1;
        output->layers[0].drm_format = ADVC_DRM_FORMAT_NV12;
        output->layers[0].num_planes = 2;
        for (p = 0; p < 2; ++p) {
            output->layers[0].object_index[p] =
                surface->prime.planes[p].object_index;
            output->layers[0].offset[p] =
                (uint32_t)surface->prime.planes[p].offset;
            output->layers[0].pitch[p] = surface->prime.planes[p].pitch;
        }
    }
    pthread_mutex_unlock(&runtime->mutex);
    record_export_timing(timing_started_ns, VA_STATUS_SUCCESS);
    return VA_STATUS_SUCCESS;
export_fail:
    for (i = 0; i < 4; ++i) {
        if (output->objects[i].fd >= 0) close(output->objects[i].fd);
        output->objects[i].fd = -1;
    }
    pthread_mutex_unlock(&runtime->mutex);
    record_export_timing(timing_started_ns,
                         VA_STATUS_ERROR_OPERATION_FAILED);
    return VA_STATUS_ERROR_OPERATION_FAILED;
}
