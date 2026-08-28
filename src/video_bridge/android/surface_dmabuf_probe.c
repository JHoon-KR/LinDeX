#define _GNU_SOURCE
#include "surface_encode_probe.h"

#include "encode_surface.h"
#include "encode_surface_egl.h"
#include "encode_surface_vulkan.h"

#include <fcntl.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef DMA_HEAP_IOCTL_ALLOC
struct dma_heap_allocation_data {
    uint64_t len;
    uint32_t fd;
    uint32_t fd_flags;
    uint64_t heap_flags;
};
#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC \
    _IOWR(DMA_HEAP_IOC_MAGIC, 0, struct dma_heap_allocation_data)
#endif
#ifndef DMA_BUF_IOCTL_SYNC
struct advc_dma_buf_sync {
    uint64_t flags;
};
#define DMA_BUF_SYNC_READ UINT64_C(1)
#define DMA_BUF_SYNC_WRITE UINT64_C(2)
#define DMA_BUF_SYNC_RW (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_SYNC_START UINT64_C(0)
#define DMA_BUF_SYNC_END UINT64_C(4)
#define DMA_BUF_BASE 'b'
#define DMA_BUF_IOCTL_SYNC _IOW(DMA_BUF_BASE, 0, struct advc_dma_buf_sync)
#else
struct advc_dma_buf_sync {
    uint64_t flags;
};
#endif

#define ADVC_DRM_FORMAT_ABGR8888 UINT32_C(0x34324241) /* AB24 */

static int dmabuf_backend_cached[3] = {-1, -1, -1};
static char dmabuf_backend_status[3][96] = {
    "not-applicable", "not-probed", "not-probed",
};
static int dmabuf_selection_cached = -1;
static int dmabuf_selected_route = ADVC_DMABUF_SURFACE_NONE;
static char dmabuf_selection_status[224] = "not-probed";

static int allocate_linear_dmabuf(size_t size) {
    static const char *const heaps[] = {
        "/dev/dma_heap/system",
        "/dev/dma_heap/system-uncached",
    };
    for (size_t i = 0; i < sizeof(heaps) / sizeof(heaps[0]); ++i) {
        struct dma_heap_allocation_data request;
        int heap = open(heaps[i], O_RDONLY | O_CLOEXEC);
        if (heap < 0) continue;
        memset(&request, 0, sizeof(request));
        request.len = size;
        request.fd_flags = O_RDWR | O_CLOEXEC;
        if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &request) == 0) {
            close(heap);
            return (int)request.fd;
        }
        close(heap);
    }
    return -1;
}

static int seed_probe_pixels(int fd, size_t size) {
    struct advc_dma_buf_sync sync;
    uint32_t *pixels;
    sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE;
    if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) return -1;
    pixels = (uint32_t *)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                              fd, 0);
    if (pixels == MAP_FAILED) return -1;
    for (size_t i = 0; i < size / sizeof(*pixels); ++i)
        pixels[i] = UINT32_C(0xff3366cc);
    (void)msync(pixels, size, MS_SYNC);
    (void)munmap(pixels, size);
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    return ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0 ? 0 : -1;
}

static int drain_probe(AMediaCodec *codec, unsigned int expected_frames) {
    unsigned int frames = 0;
    unsigned int non_key_frames = 0;
    int64_t last_pts_us = INT64_MIN;
    int eos = 0;
    for (unsigned int attempt = 0; attempt < 300 && !eos; ++attempt) {
        AMediaCodecBufferInfo info;
        ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, 10000);
        if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER ||
            index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED ||
            index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
            continue;
        if (index < 0 || info.offset < 0 || info.size < 0) return 0;
        if (info.size > 0) {
            size_t capacity = 0;
            uint8_t *data = AMediaCodec_getOutputBuffer(codec, (size_t)index,
                                                        &capacity);
            uint64_t offset = (uint64_t)info.offset;
            uint64_t size = (uint64_t)info.size;
            if (data == NULL || offset > capacity || size > capacity - offset) {
                (void)AMediaCodec_releaseOutputBuffer(codec, (size_t)index,
                                                       false);
                return 0;
            }
            if ((info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) == 0) {
                if (frames > 0 && info.presentationTimeUs <= last_pts_us) {
                    (void)AMediaCodec_releaseOutputBuffer(
                        codec, (size_t)index, false);
                    return 0;
                }
                last_pts_us = info.presentationTimeUs;
                ++frames;
                if ((info.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) == 0)
                    ++non_key_frames;
            }
        }
        if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) eos = 1;
        if (AMediaCodec_releaseOutputBuffer(codec, (size_t)index, false) !=
            AMEDIA_OK)
            return 0;
    }
    return frames >= expected_frames && non_key_frames > 0 && eos;
}

static const char *route_name(int route) {
    switch (route) {
    case ADVC_DMABUF_SURFACE_VULKAN: return "vulkan";
    case ADVC_DMABUF_SURFACE_EGL: return "egl";
    default: return "none";
    }
}

static int requested_route(void) {
    const char *value = getenv("ADVC_DMABUF_BACKEND");
    if (value == NULL || value[0] == '\0' || strcmp(value, "auto") == 0)
        return ADVC_DMABUF_SURFACE_NONE;
    if (strcmp(value, "vulkan") == 0)
        return ADVC_DMABUF_SURFACE_VULKAN;
    if (strcmp(value, "egl") == 0)
        return ADVC_DMABUF_SURFACE_EGL;
    return -1;
}

int advc_probe_android_dmabuf_surface_backend(int requested_backend) {
    static const uint32_t width = 320;
    static const uint32_t height = 240;
    static const uint32_t pitch = width * 4u;
    static const size_t allocation_size = (size_t)pitch * height;
    static const int32_t color_format_surface = (int32_t)0x7f000789u;
    struct advc_dmabuf_descriptor descriptor;
    AMediaCodec *codec = NULL;
    AMediaFormat *format = NULL;
    struct advc_encode_surface *surface = NULL;
    struct advc_egl_surface_producer *producer = NULL;
    struct advc_vk_surface_producer *vk_producer = NULL;
    void *window = NULL;
    int dmabuf_fd = -1;
    int release_fence = -1;
    int started = 0;
    int ok = 0;
    const char *stage = "dma_heap_allocate";
    if (requested_backend != ADVC_DMABUF_SURFACE_EGL &&
        requested_backend != ADVC_DMABUF_SURFACE_VULKAN)
        return 0;
    if (dmabuf_backend_cached[requested_backend] >= 0)
        return dmabuf_backend_cached[requested_backend];

    dmabuf_fd = allocate_linear_dmabuf(allocation_size);
    if (dmabuf_fd < 0) goto done;
    stage = "seed_probe_pixels";
    if (seed_probe_pixels(dmabuf_fd, allocation_size) < 0) goto done;
    memset(&descriptor, 0, sizeof(descriptor));
    for (size_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        descriptor.objects[i].fd = -1;
    descriptor.buffer_id = 1;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.drm_fourcc = ADVC_DRM_FORMAT_ABGR8888;
    descriptor.explicit_flags = ADVC_DMABUF_EXPLICIT_ALL;
    descriptor.drm_modifier = 0;
    descriptor.crop_width = width;
    descriptor.crop_height = height;
    descriptor.object_count = 1;
    descriptor.plane_count = 1;
    descriptor.color_matrix = ADVC_COLOR_MATRIX_RGB;
    descriptor.color_range = ADVC_COLOR_RANGE_FULL;
    descriptor.objects[0].fd = dmabuf_fd;
    descriptor.objects[0].size = allocation_size;
    descriptor.planes[0].pitch = pitch;

    stage = "codec_create";
    codec = AMediaCodec_createEncoderByType("video/avc");
    format = AMediaFormat_new();
    if (codec == NULL || format == NULL) goto done;
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, (int32_t)width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, (int32_t)height);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, 500000);
    AMediaFormat_setFloat(format, AMEDIAFORMAT_KEY_FRAME_RATE, 30.0f);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT,
                          color_format_surface);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
    stage = "codec_configure";
    if (AMediaCodec_configure(codec, format, NULL, NULL,
                              AMEDIACODEC_CONFIGURE_FLAG_ENCODE) != AMEDIA_OK)
        goto done;
    stage = "codec_input_surface";
    if (advc_encode_surface_create(codec, ADVC_ENCODE_SURFACE_CODEC_CONFIGURED,
            ADVC_ENCODE_SURFACE_ROUTE_DIRECT_WINDOW, advc_encode_surface_ndk_ops(),
            NULL, &surface) != ADVC_ENCODE_SURFACE_OK)
        goto done;
    stage = "input_window";
    if (advc_encode_surface_acquire_window(surface, &window) !=
        ADVC_ENCODE_SURFACE_OK)
        goto done;
    if (requested_backend == ADVC_DMABUF_SURFACE_VULKAN) {
        stage = "vulkan_surface_create";
        if (advc_vk_surface_producer_create(window, width, height,
                                            &vk_producer) < 0)
            goto done;
        stage = "vulkan_dmabuf_import";
        if (advc_vk_surface_producer_validate_dmabuf(vk_producer,
                                                     &descriptor) < 0)
            goto done;
    } else {
        stage = "egl_surface_create";
        if (advc_egl_surface_producer_create(window, width, height,
                                             &producer) < 0)
            goto done;
        stage = "egl_dmabuf_import";
        if (advc_egl_surface_producer_validate_dmabuf(producer,
                                                      &descriptor) < 0)
            goto done;
    }
    advc_encode_surface_release_window(surface, window);
    window = NULL;
    stage = "codec_start";
    if (AMediaCodec_start(codec) != AMEDIA_OK)
        goto done;
    started = 1;
    stage = "surface_started";
    if (advc_encode_surface_mark_started(surface) != ADVC_ENCODE_SURFACE_OK)
        goto done;
    for (uint32_t frame = 0; frame < 3; ++frame) {
        int64_t pts_ns = (int64_t)frame * INT64_C(33333333);
        stage = "dmabuf_draw_3vcl";
        if ((requested_backend == ADVC_DMABUF_SURFACE_EGL &&
             advc_egl_surface_producer_render_dmabuf(
                 producer, &descriptor, frame, pts_ns, -1,
                 &release_fence) < 0) ||
            (requested_backend == ADVC_DMABUF_SURFACE_VULKAN &&
             advc_vk_surface_producer_render_dmabuf(
                 vk_producer, &descriptor, frame, pts_ns, -1,
                 &release_fence) < 0))
            goto done;
        stage = "release_fence_3vcl";
        if (release_fence < 0 ||
            advc_dmabuf_sync_file_validate(release_fence) < 0)
            goto done;
        close(release_fence);
        release_fence = -1;
    }
    stage = "surface_eos";
    if (advc_encode_surface_signal_eos(surface) != ADVC_ENCODE_SURFACE_OK)
        goto done;
    stage = "codec_drain";
    if (!drain_probe(codec, 3))
        goto done;
    ok = 1;

done:
    snprintf(dmabuf_backend_status[requested_backend],
             sizeof(dmabuf_backend_status[requested_backend]), "%s:%s",
             ok ? "probe-pass" : "probe-failed",
             ok ? "encoded-3vcl-monotonic-eos" : stage);
    if (!ok && getenv("ADVC_DEBUG") != NULL)
        fprintf(stderr,
                "advc-dmabuf-probe: backend=%s result=unsupported stage=%s\n",
                route_name(requested_backend), stage);
    if (release_fence >= 0) close(release_fence);
    if (window != NULL && surface != NULL)
        advc_encode_surface_release_window(surface, window);
    if (started) (void)AMediaCodec_stop(codec);
    advc_egl_surface_producer_destroy(producer);
    advc_vk_surface_producer_destroy(vk_producer);
    advc_encode_surface_destroy(surface);
    if (format != NULL) AMediaFormat_delete(format);
    if (codec != NULL) AMediaCodec_delete(codec);
    if (dmabuf_fd >= 0) close(dmabuf_fd);
    dmabuf_backend_cached[requested_backend] = ok;
    return ok;
}

int advc_probe_android_dmabuf_surface(void) {
    int preference;
    int vulkan_ok;
    int egl_ok;
    if (dmabuf_selection_cached >= 0) return dmabuf_selection_cached;
    preference = requested_route();
    if (preference < 0) {
        snprintf(dmabuf_selection_status, sizeof(dmabuf_selection_status),
                 "selected=none;reason=invalid-policy");
        dmabuf_selection_cached = 0;
        return 0;
    }
    if (preference != ADVC_DMABUF_SURFACE_NONE) {
        if (advc_probe_android_dmabuf_surface_backend(preference)) {
            dmabuf_selected_route = preference;
            snprintf(dmabuf_selection_status, sizeof(dmabuf_selection_status),
                     "selected=%s;reason=forced-real-probe-pass",
                     route_name(preference));
            dmabuf_selection_cached = 1;
            return 1;
        }
        snprintf(dmabuf_selection_status, sizeof(dmabuf_selection_status),
                 "selected=none;reason=forced-%s-unavailable;%s",
                 route_name(preference), dmabuf_backend_status[preference]);
        dmabuf_selection_cached = 0;
        return 0;
    }

    /* Default policy is Vulkan, then EGL. Each call owns a fresh codec session. */
    vulkan_ok = advc_probe_android_dmabuf_surface_backend(
        ADVC_DMABUF_SURFACE_VULKAN);
    if (vulkan_ok) {
        dmabuf_selected_route = ADVC_DMABUF_SURFACE_VULKAN;
        snprintf(dmabuf_selection_status, sizeof(dmabuf_selection_status),
                 "selected=vulkan;reason=default-real-probe-pass");
        dmabuf_selection_cached = 1;
        return 1;
    }
    egl_ok = advc_probe_android_dmabuf_surface_backend(
        ADVC_DMABUF_SURFACE_EGL);
    if (egl_ok) {
        dmabuf_selected_route = ADVC_DMABUF_SURFACE_EGL;
        snprintf(dmabuf_selection_status, sizeof(dmabuf_selection_status),
                 "selected=egl;reason=vulkan-rejected-new-session");
        dmabuf_selection_cached = 1;
        return 1;
    }
    snprintf(dmabuf_selection_status, sizeof(dmabuf_selection_status),
             "selected=none;reason=backends-exhausted;%s;%s",
             dmabuf_backend_status[ADVC_DMABUF_SURFACE_VULKAN],
             dmabuf_backend_status[ADVC_DMABUF_SURFACE_EGL]);
    dmabuf_selection_cached = 0;
    return 0;
}

int advc_android_dmabuf_surface_route(void) {
    if (!advc_probe_android_dmabuf_surface())
        return ADVC_DMABUF_SURFACE_NONE;
    return dmabuf_selected_route;
}

const char *advc_android_dmabuf_surface_status(void) {
    (void)advc_probe_android_dmabuf_surface();
    return dmabuf_selection_status;
}

const char *advc_android_dmabuf_surface_backend_status(int route) {
    if (route != ADVC_DMABUF_SURFACE_EGL &&
        route != ADVC_DMABUF_SURFACE_VULKAN)
        return "invalid-backend";
    (void)advc_probe_android_dmabuf_surface_backend(route);
    return dmabuf_backend_status[route];
}
