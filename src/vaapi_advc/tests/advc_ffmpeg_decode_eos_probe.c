#define _GNU_SOURCE

/*
 * Opt-in live-test shim for an unmodified FFmpeg binary.
 *
 * FFmpeg's public VA-API decode API has no end-of-stream operation and a null
 * output does not require it to export a decoded surface.  This shim observes
 * the ordinary libva calls made by stock FFmpeg, invokes LinDeX's private
 * decode-EOS interface during teardown, then forces a bounded sync/PRIME
 * export for every submitted surface before the real VA context is destroyed.
 *
 * This file is a developer probe, not a runtime dependency and not a reason to
 * advertise a codec.  It is fail-closed, owns no persistent state, and is
 * intended only for LD_PRELOAD use with a transient driver and broker.
 */

#include <va/va.h>
#include <va/va_dec_hevc.h>
#include <va/va_drmcommon.h>

#include "turnip_prime_import.h"

#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROBE_MAX_CONTEXTS 32u
#define PROBE_MAX_SURFACES 64u
#define PROBE_EOS_STEPS 5000u
#define PROBE_SYNC_TIMEOUT_NS UINT64_C(5000000000)
#define PROBE_EOS_ABI_VERSION UINT32_C(0x00010000)
#define PROBE_EOS_MAX_OUTPUTS 8u
#define PROBE_EOS_PHASE_COMPLETE 3u
#define PROBE_EOS_RESULT_WOULD_BLOCK 1
#define PROBE_EOS_RESULT_NEED_OUTPUT_RELEASE 2
#define PROBE_EOS_RESULT_COMPLETE 3
#define PROBE_EOS_STATUS_OUTPUT_EOS_SEEN (1u << 5)
#define PROBE_EOS_SYMBOL "advcVaGetDecodeEosInterface_1_0"
#define PROBE_SYNC_DRAIN_ENV "ADVC_EOS_PROBE_SYNC_DRAIN"
#define PROBE_SYNC_DRAIN_VALUE "validated-four-frame-v1"
#define PROBE_PIXEL_HASH_ENV "ADVC_EOS_PROBE_PIXEL_HASH"
#define PROBE_PIXEL_HASH_VALUE "validated-turnip-poc-v1"

struct probe_eos_status {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t phase;
    uint32_t flags;
    int32_t last_result;
    uint32_t outputs_processed;
    uint32_t frames_processed;
    uint32_t controls_released;
    uint64_t outputs_processed_total;
    uint64_t frames_processed_total;
    uint64_t controls_released_total;
    uint32_t reserved[8];
};

typedef int32_t (*probe_eos_signal_fn)(
    VADisplay display, VAContextID context, uint32_t flags,
    struct probe_eos_status *status);
typedef int32_t (*probe_eos_progress_fn)(
    VADisplay display, VAContextID context, uint32_t max_outputs,
    uint32_t flags, struct probe_eos_status *status);

struct probe_eos_interface {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t capabilities;
    uint32_t reserved0;
    probe_eos_signal_fn signal;
    probe_eos_progress_fn progress;
    uintptr_t reserved[8];
};

typedef const struct probe_eos_interface *(*probe_eos_getter_fn)(void);

struct probe_surface {
    VASurfaceID id;
    unsigned int submitted;
    unsigned int exported;
    int have_poc;
    int32_t poc;
};

struct probe_context {
    int used;
    int eos_done;
    VADisplay display;
    VAContextID id;
    VASurfaceID current;
    unsigned int submitted;
    unsigned int export_count;
    struct probe_surface surfaces[PROBE_MAX_SURFACES];
    unsigned int surface_count;
};

static pthread_mutex_t probe_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t symbols_once = PTHREAD_ONCE_INIT;
static struct probe_context contexts[PROBE_MAX_CONTEXTS];

static VAStatus (*next_create_context)(
    VADisplay, VAConfigID, int, int, int, VASurfaceID *, int, VAContextID *);
static VAStatus (*next_create_buffer)(
    VADisplay, VAContextID, VABufferType, unsigned int, unsigned int, void *,
    VABufferID *);
static VAStatus (*next_destroy_context)(VADisplay, VAContextID);
static VAStatus (*next_begin_picture)(VADisplay, VAContextID, VASurfaceID);
static VAStatus (*next_end_picture)(VADisplay, VAContextID);
static VAStatus (*next_sync_surface)(VADisplay, VASurfaceID);
static VAStatus (*next_sync_surface2)(VADisplay, VASurfaceID, uint64_t);
static VAStatus (*next_export_surface)(VADisplay, VASurfaceID, uint32_t,
                                       uint32_t, void *);
static VAStatus (*next_destroy_surfaces)(VADisplay, VASurfaceID *, int);
static VAPrivFunc (*next_get_lib_func)(VADisplay, const char *);

static void copy_symbol(void *target, size_t target_size, const char *name) {
    void *symbol = dlsym(RTLD_NEXT, name);
    if (target_size == sizeof(symbol)) memcpy(target, &symbol, sizeof(symbol));
}

static void resolve_symbols(void) {
    copy_symbol(&next_create_context, sizeof(next_create_context),
                "vaCreateContext");
    copy_symbol(&next_create_buffer, sizeof(next_create_buffer),
                "vaCreateBuffer");
    copy_symbol(&next_destroy_context, sizeof(next_destroy_context),
                "vaDestroyContext");
    copy_symbol(&next_begin_picture, sizeof(next_begin_picture),
                "vaBeginPicture");
    copy_symbol(&next_end_picture, sizeof(next_end_picture), "vaEndPicture");
    copy_symbol(&next_sync_surface, sizeof(next_sync_surface),
                "vaSyncSurface");
    copy_symbol(&next_sync_surface2, sizeof(next_sync_surface2),
                "vaSyncSurface2");
    copy_symbol(&next_export_surface, sizeof(next_export_surface),
                "vaExportSurfaceHandle");
    copy_symbol(&next_destroy_surfaces, sizeof(next_destroy_surfaces),
                "vaDestroySurfaces");
    copy_symbol(&next_get_lib_func, sizeof(next_get_lib_func), "vaGetLibFunc");
}

static int symbols_ready(void) {
    (void)pthread_once(&symbols_once, resolve_symbols);
    return next_create_context != NULL && next_create_buffer != NULL &&
           next_destroy_context != NULL &&
           next_begin_picture != NULL && next_end_picture != NULL &&
           next_sync_surface != NULL && next_export_surface != NULL &&
           next_destroy_surfaces != NULL && next_get_lib_func != NULL;
}

static struct probe_context *find_context_locked(VADisplay display,
                                                  VAContextID id) {
    unsigned int i;
    for (i = 0; i < PROBE_MAX_CONTEXTS; ++i) {
        if (contexts[i].used && contexts[i].display == display &&
            contexts[i].id == id)
            return &contexts[i];
    }
    return NULL;
}

static struct probe_surface *find_surface_locked(struct probe_context *context,
                                                  VASurfaceID id,
                                                  int create) {
    unsigned int i;
    for (i = 0; i < context->surface_count; ++i) {
        if (context->surfaces[i].id == id) return &context->surfaces[i];
    }
    if (!create || context->surface_count >= PROBE_MAX_SURFACES) return NULL;
    i = context->surface_count++;
    memset(&context->surfaces[i], 0, sizeof(context->surfaces[i]));
    context->surfaces[i].id = id;
    return &context->surfaces[i];
}

static int context_uses_surface_locked(const struct probe_context *context,
                                       VASurfaceID id) {
    unsigned int i;
    for (i = 0; i < context->surface_count; ++i) {
        if (context->surfaces[i].id == id &&
            context->surfaces[i].submitted != 0)
            return 1;
    }
    return 0;
}

static int pixel_hash_enabled(void) {
    const char *value = getenv(PROBE_PIXEL_HASH_ENV);
    return value != NULL && strcmp(value, PROBE_PIXEL_HASH_VALUE) == 0;
}

static int descriptor_from_prime(
    const VADRMPRIMESurfaceDescriptor *prime, VASurfaceID surface,
    struct advc_dmabuf_descriptor *descriptor) {
    unsigned int i;
    memset(descriptor, 0, sizeof(*descriptor));
    for (i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        descriptor->objects[i].fd = -1;
    if (prime->width == 0 || prime->height == 0 || prime->num_objects == 0 ||
        prime->num_objects > ADVC_MAX_DMABUF_OBJECTS ||
        prime->num_layers != 1 || prime->layers[0].num_planes != 2 ||
        prime->layers[0].drm_format != VA_FOURCC_NV12)
        return -1;
    descriptor->buffer_id = surface;
    descriptor->width = prime->width;
    descriptor->height = prime->height;
    descriptor->drm_fourcc = prime->fourcc;
    descriptor->explicit_flags = ADVC_DMABUF_EXPLICIT_ALL;
    descriptor->drm_modifier = prime->objects[0].drm_format_modifier;
    descriptor->crop_width = prime->width;
    descriptor->crop_height = prime->height;
    descriptor->object_count = prime->num_objects;
    descriptor->plane_count = 2;
    for (i = 0; i < prime->num_objects; ++i) {
        if (prime->objects[i].drm_format_modifier !=
            descriptor->drm_modifier)
            return -1;
        descriptor->objects[i].fd = prime->objects[i].fd;
        descriptor->objects[i].size = prime->objects[i].size;
    }
    for (i = 0; i < 2u; ++i) {
        descriptor->planes[i].object_index =
            prime->layers[0].object_index[i];
        descriptor->planes[i].offset = prime->layers[0].offset[i];
        descriptor->planes[i].pitch = prime->layers[0].pitch[i];
    }
    return advc_dmabuf_descriptor_validate(descriptor);
}

static int sync_export_surface(VADisplay display, VASurfaceID surface,
                               const struct probe_surface *tracked) {
    VADRMPRIMESurfaceDescriptor prime;
    struct advc_dmabuf_descriptor descriptor;
    struct advc_turnip_prime_result pixels;
    VAStatus status;
    unsigned int i;

    if (next_sync_surface2 != NULL)
        status = next_sync_surface2(display, surface, PROBE_SYNC_TIMEOUT_NS);
    else
        status = next_sync_surface(display, surface);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr,
                "lindex-eos-probe: sync-fail surface=%u status=0x%08x\n",
                surface, status);
        return -1;
    }

    memset(&prime, 0, sizeof(prime));
    for (i = 0; i < 4u; ++i) prime.objects[i].fd = -1;
    status = next_export_surface(
        display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
        &prime);
    if (status != VA_STATUS_SUCCESS || prime.num_objects == 0 ||
        prime.num_objects > 4u || prime.num_layers == 0 ||
        prime.objects[0].fd < 0) {
        fprintf(stderr,
                "lindex-eos-probe: prime-fail surface=%u status=0x%08x "
                "objects=%u layers=%u\n",
                surface, status, prime.num_objects, prime.num_layers);
        for (i = 0; i < 4u; ++i)
            if (prime.objects[i].fd >= 0) close(prime.objects[i].fd);
        return -1;
    }
    fprintf(stderr,
            "lindex-eos-probe: prime-sync-pass surface=%u fourcc=0x%08x "
            "modifier=0x%016" PRIx64 " objects=%u layers=%u\n",
            surface, prime.fourcc,
            (uint64_t)prime.objects[0].drm_format_modifier,
            prime.num_objects, prime.num_layers);
    if (pixel_hash_enabled()) {
        memset(&pixels, 0, sizeof(pixels));
        pixels.release_fence_fd = -1;
        if (tracked == NULL || !tracked->have_poc ||
            descriptor_from_prime(&prime, surface, &descriptor) < 0 ||
            advc_turnip_prime_consume(&descriptor, -1, &pixels) < 0) {
            fprintf(stderr,
                    "lindex-eos-probe: pixel-hash-fail surface=%u poc=%d "
                    "errno=%d\n",
                    surface,
                    tracked != NULL && tracked->have_poc ? tracked->poc : -1,
                    errno);
            if (pixels.release_fence_fd >= 0)
                close(pixels.release_fence_fd);
            for (i = 0; i < prime.num_objects; ++i)
                if (prime.objects[i].fd >= 0) close(prime.objects[i].fd);
            return -1;
        }
        fprintf(stderr,
                "lindex-eos-probe: pixel-hash-pass surface=%u poc=%d "
                "fnv1a64=%016" PRIx64 " bytes=%" PRIu64
                " distinct=%u device=%s\n",
                surface, tracked->poc, pixels.content_hash,
                pixels.content_bytes, pixels.distinct_sample_values,
                pixels.device_name);
        if (pixels.release_fence_fd >= 0)
            close(pixels.release_fence_fd);
    }
    for (i = 0; i < prime.num_objects; ++i)
        if (prime.objects[i].fd >= 0) close(prime.objects[i].fd);
    return 0;
}

static int get_eos_interface(VADisplay display,
                             const struct probe_eos_interface **interface) {
    VAPrivFunc private_function;
    probe_eos_getter_fn getter = NULL;

    private_function = next_get_lib_func(display, PROBE_EOS_SYMBOL);
    if (private_function == NULL || sizeof(private_function) != sizeof(getter))
        return -1;
    memcpy(&getter, &private_function, sizeof(getter));
    *interface = getter();
    if (*interface == NULL ||
        (*interface)->struct_size < sizeof(**interface) ||
        (*interface)->abi_version != PROBE_EOS_ABI_VERSION ||
        (*interface)->signal == NULL || (*interface)->progress == NULL)
        return -1;
    return 0;
}

static int drain_eos(VADisplay display, VAContextID context) {
    const struct probe_eos_interface *interface = NULL;
    struct probe_eos_status status;
    int32_t result;
    unsigned int step;

    if (get_eos_interface(display, &interface) < 0) {
        fprintf(stderr, "lindex-eos-probe: eos-interface-missing\n");
        return -1;
    }
    memset(&status, 0, sizeof(status));
    status.struct_size = sizeof(status);
    result = interface->signal(display, context, 0, &status);
    for (step = 0; step < PROBE_EOS_STEPS; ++step) {
        if (result < 0) break;
        if (result == PROBE_EOS_RESULT_COMPLETE ||
            status.phase == PROBE_EOS_PHASE_COMPLETE)
            break;
        if (result == PROBE_EOS_RESULT_WOULD_BLOCK ||
            result == PROBE_EOS_RESULT_NEED_OUTPUT_RELEASE)
            usleep(1000);
        memset(&status, 0, sizeof(status));
        status.struct_size = sizeof(status);
        result = interface->progress(display, context,
                                     PROBE_EOS_MAX_OUTPUTS, 0, &status);
    }
    if ((result != PROBE_EOS_RESULT_COMPLETE &&
         status.phase != PROBE_EOS_PHASE_COMPLETE) ||
        (status.flags & PROBE_EOS_STATUS_OUTPUT_EOS_SEEN) == 0 ||
        status.controls_released_total == 0) {
        fprintf(stderr,
                "lindex-eos-probe: eos-fail result=%d phase=%u flags=0x%x "
                "outputs=%" PRIu64 " frames=%" PRIu64
                " controls=%" PRIu64 " steps=%u\n",
                result, status.phase, status.flags,
                status.outputs_processed_total, status.frames_processed_total,
                status.controls_released_total, step);
        return -1;
    }
    fprintf(stderr,
            "lindex-eos-probe: eos-pass result=%d phase=%u flags=0x%x "
            "outputs=%" PRIu64 " frames=%" PRIu64
            " controls=%" PRIu64 " steps=%u\n",
            result, status.phase, status.flags,
            status.outputs_processed_total, status.frames_processed_total,
            status.controls_released_total, step);
    return 0;
}

static int sync_drain_enabled(void) {
    const char *value = getenv(PROBE_SYNC_DRAIN_ENV);
    return value != NULL && strcmp(value, PROBE_SYNC_DRAIN_VALUE) == 0;
}

static int drain_context_before_sync(VADisplay display, VASurfaceID surface) {
    struct probe_context *matched = NULL;
    unsigned int i;
    int result = -1;

    pthread_mutex_lock(&probe_lock);
    for (i = 0; i < PROBE_MAX_CONTEXTS; ++i) {
        if (!contexts[i].used || contexts[i].display != display ||
            !context_uses_surface_locked(&contexts[i], surface))
            continue;
        matched = &contexts[i];
        break;
    }
    if (matched == NULL) {
        fprintf(stderr,
                "lindex-eos-probe: eos-before-sync-no-context surface=%u\n",
                surface);
    } else if (matched->submitted != 4u) {
        /*
         * This opt-in is deliberately fixture-specific.  Draining a normal
         * streaming decoder on its first presented picture would truncate the
         * stream, so fail closed unless all four I/P/B test pictures have
         * already reached vaEndPicture.
         */
        fprintf(stderr,
                "lindex-eos-probe: eos-before-sync-wrong-submitted "
                "surface=%u submitted=%u\n",
                surface, matched->submitted);
    } else if (matched->eos_done ||
               drain_eos(matched->display, matched->id) == 0) {
        matched->eos_done = 1;
        result = 0;
        fprintf(stderr,
                "lindex-eos-probe: eos-before-sync-pass surface=%u "
                "submitted=%u\n",
                surface, matched->submitted);
    }
    pthread_mutex_unlock(&probe_lock);
    return result;
}

static int finish_context(struct probe_context *context) {
    unsigned int i;
    int failed = 0;

    /*
     * MediaCodec may retain a short stream's final frame until input EOS is
     * queued. Signal and drain EOS before waiting on any submitted VASurface;
     * syncing first creates a deterministic timeout on one-frame streams.
     */
    if (!context->eos_done) {
        if (drain_eos(context->display, context->id) < 0)
            failed = 1;
        else
            context->eos_done = 1;
    }
    for (i = 0; i < context->surface_count; ++i) {
        struct probe_surface *surface = &context->surfaces[i];
        if (surface->submitted == 0 || surface->exported != 0) continue;
        if (sync_export_surface(context->display, surface->id, surface) < 0)
            failed = 1;
        else {
            surface->exported = 1;
            ++context->export_count;
        }
    }
    fprintf(stderr,
            "lindex-eos-probe: context-summary context=%u submitted=%u "
            "prime_exports=%u eos=%s\n",
            context->id, context->submitted, context->export_count,
            context->eos_done ? "complete" : "failed");
    return failed ? -1 : 0;
}

VAStatus vaCreateContext(VADisplay display, VAConfigID config, int width,
                         int height, int flag, VASurfaceID *targets,
                         int target_count, VAContextID *context_id) {
    VAStatus status;
    unsigned int i;
    struct probe_context *entry = NULL;

    if (!symbols_ready()) return VA_STATUS_ERROR_OPERATION_FAILED;
    status = next_create_context(display, config, width, height, flag, targets,
                                 target_count, context_id);
    if (status != VA_STATUS_SUCCESS || context_id == NULL) return status;
    pthread_mutex_lock(&probe_lock);
    for (i = 0; i < PROBE_MAX_CONTEXTS; ++i) {
        if (!contexts[i].used) {
            entry = &contexts[i];
            memset(entry, 0, sizeof(*entry));
            entry->used = 1;
            entry->display = display;
            entry->id = *context_id;
            entry->current = VA_INVALID_SURFACE;
            break;
        }
    }
    if (entry != NULL) {
        int target;
        for (target = 0; target < target_count; ++target)
            (void)find_surface_locked(entry, targets[target], 1);
    }
    pthread_mutex_unlock(&probe_lock);
    if (entry == NULL)
        fprintf(stderr, "lindex-eos-probe: context-table-full\n");
    return status;
}

VAStatus vaCreateBuffer(VADisplay display, VAContextID context,
                        VABufferType type, unsigned int size,
                        unsigned int num_elements, void *data,
                        VABufferID *buffer_id) {
    VAStatus status;
    if (!symbols_ready()) return VA_STATUS_ERROR_OPERATION_FAILED;
    status = next_create_buffer(display, context, type, size, num_elements,
                                data, buffer_id);
    if (status == VA_STATUS_SUCCESS && pixel_hash_enabled() &&
        type == VAPictureParameterBufferType && data != NULL &&
        num_elements != 0 && size >= sizeof(VAPictureParameterBufferHEVC)) {
        const VAPictureParameterBufferHEVC *picture = data;
        struct probe_context *entry;
        struct probe_surface *surface;
        pthread_mutex_lock(&probe_lock);
        entry = find_context_locked(display, context);
        surface = entry != NULL
                      ? find_surface_locked(entry, picture->CurrPic.picture_id,
                                            1)
                      : NULL;
        if (surface != NULL) {
            surface->poc = picture->CurrPic.pic_order_cnt;
            surface->have_poc = 1;
            fprintf(stderr,
                    "lindex-eos-probe: hevc-poc surface=%u poc=%d\n",
                    surface->id, surface->poc);
        }
        pthread_mutex_unlock(&probe_lock);
    }
    return status;
}

VAStatus vaBeginPicture(VADisplay display, VAContextID context,
                        VASurfaceID target) {
    VAStatus status;
    struct probe_context *entry;
    if (!symbols_ready()) return VA_STATUS_ERROR_OPERATION_FAILED;
    status = next_begin_picture(display, context, target);
    if (status != VA_STATUS_SUCCESS) return status;
    pthread_mutex_lock(&probe_lock);
    entry = find_context_locked(display, context);
    if (entry != NULL) {
        entry->current = target;
        (void)find_surface_locked(entry, target, 1);
    }
    pthread_mutex_unlock(&probe_lock);
    return status;
}

VAStatus vaEndPicture(VADisplay display, VAContextID context) {
    VAStatus status;
    struct probe_context *entry;
    if (!symbols_ready()) return VA_STATUS_ERROR_OPERATION_FAILED;
    status = next_end_picture(display, context);
    pthread_mutex_lock(&probe_lock);
    entry = find_context_locked(display, context);
    if (entry != NULL && status == VA_STATUS_SUCCESS &&
        entry->current != VA_INVALID_SURFACE) {
        struct probe_surface *surface =
            find_surface_locked(entry, entry->current, 1);
        if (surface != NULL) ++surface->submitted;
        ++entry->submitted;
    }
    if (entry != NULL) entry->current = VA_INVALID_SURFACE;
    pthread_mutex_unlock(&probe_lock);
    return status;
}

static void mark_exported(VADisplay display, VASurfaceID surface) {
    unsigned int i;
    pthread_mutex_lock(&probe_lock);
    for (i = 0; i < PROBE_MAX_CONTEXTS; ++i) {
        struct probe_surface *tracked;
        if (!contexts[i].used || contexts[i].display != display) continue;
        tracked = find_surface_locked(&contexts[i], surface, 0);
        if (tracked != NULL && tracked->submitted != 0 &&
            tracked->exported == 0) {
            tracked->exported = 1;
            ++contexts[i].export_count;
        }
    }
    pthread_mutex_unlock(&probe_lock);
}

VAStatus vaSyncSurface(VADisplay display, VASurfaceID surface) {
    VAStatus status;
    if (!symbols_ready()) return VA_STATUS_ERROR_OPERATION_FAILED;
    if (sync_drain_enabled() &&
        drain_context_before_sync(display, surface) < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    status = next_sync_surface(display, surface);
    if (status == VA_STATUS_SUCCESS &&
        sync_export_surface(display, surface, NULL) == 0)
        mark_exported(display, surface);
    return status;
}

VAStatus vaSyncSurface2(VADisplay display, VASurfaceID surface,
                        uint64_t timeout_ns) {
    VAStatus status;
    if (!symbols_ready()) return VA_STATUS_ERROR_OPERATION_FAILED;
    if (sync_drain_enabled() &&
        drain_context_before_sync(display, surface) < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    status = next_sync_surface2 != NULL
                 ? next_sync_surface2(display, surface, timeout_ns)
                 : next_sync_surface(display, surface);
    if (status == VA_STATUS_SUCCESS &&
        sync_export_surface(display, surface, NULL) == 0)
        mark_exported(display, surface);
    return status;
}

VAStatus vaDestroyContext(VADisplay display, VAContextID context) {
    VAStatus status;
    int failed = 0;
    struct probe_context *entry;
    if (!symbols_ready()) return VA_STATUS_ERROR_OPERATION_FAILED;
    pthread_mutex_lock(&probe_lock);
    entry = find_context_locked(display, context);
    if (entry != NULL) failed = finish_context(entry);
    pthread_mutex_unlock(&probe_lock);
    status = next_destroy_context(display, context);
    pthread_mutex_lock(&probe_lock);
    entry = find_context_locked(display, context);
    if (entry != NULL) memset(entry, 0, sizeof(*entry));
    pthread_mutex_unlock(&probe_lock);
    if (status == VA_STATUS_SUCCESS && failed != 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    return status;
}

VAStatus vaDestroySurfaces(VADisplay display, VASurfaceID *surfaces,
                           int surface_count) {
    VAStatus status;
    unsigned int i;
    int failed = 0;
    if (!symbols_ready()) return VA_STATUS_ERROR_OPERATION_FAILED;
    pthread_mutex_lock(&probe_lock);
    for (i = 0; i < PROBE_MAX_CONTEXTS; ++i) {
        int surface;
        if (!contexts[i].used || contexts[i].display != display) continue;
        for (surface = 0; surface < surface_count; ++surface) {
            if (context_uses_surface_locked(&contexts[i], surfaces[surface])) {
                if (finish_context(&contexts[i]) < 0) failed = 1;
                break;
            }
        }
    }
    pthread_mutex_unlock(&probe_lock);
    status = next_destroy_surfaces(display, surfaces, surface_count);
    fprintf(stderr,
            "lindex-eos-probe: surface-release-%s count=%d status=0x%08x\n",
            status == VA_STATUS_SUCCESS ? "pass" : "fail", surface_count,
            status);
    if (status == VA_STATUS_SUCCESS && failed != 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    return status;
}
