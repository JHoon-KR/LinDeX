#ifndef ADVC_VAAPI_IMAGE_H
#define ADVC_VAAPI_IMAGE_H

#include "advc/dmabuf_ingress.h"
#include "advc_vaapi_encode.h"

#include <va/va.h>
#include <va/va_drmcommon.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADVC_VAAPI_IMAGE_FORMAT_COUNT 2
#define ADVC_VAAPI_IMAGE_SYNC_TIMEOUT_MS 5000u

enum advc_vaapi_surface_access {
    ADVC_VAAPI_SURFACE_ACCESS_READ = 1,
    ADVC_VAAPI_SURFACE_ACCESS_WRITE = 2,
    ADVC_VAAPI_SURFACE_ACCESS_READ_WRITE = 3,
};

/*
 * acquire_surface returns an owned descriptor and an owned sync_file FD (or
 * -1). The image runtime always closes both. release_surface is a notification
 * that the CPU access has finished; it does not own either object.
 *
 * dma_buf_sync and wait_fence exist so host tests can provide kernel-neutral
 * implementations. Production callers should leave them NULL to use
 * DMA_BUF_IOCTL_SYNC and a validated sync_file poll respectively.
 */
struct advc_vaapi_image_surface_ops {
    void *opaque;
    VAStatus (*acquire_surface)(
        void *opaque, VASurfaceID surface,
        enum advc_vaapi_surface_access access,
        struct advc_dmabuf_descriptor *descriptor, int *acquire_fence_fd);
    void (*release_surface)(void *opaque, VASurfaceID surface,
                            enum advc_vaapi_surface_access access,
                            int access_succeeded);
    int (*dma_buf_sync)(void *opaque, int fd, uint64_t flags);
    int (*wait_fence)(void *opaque, int fd, uint32_t timeout_ms);
};

struct advc_vaapi_image_runtime;

struct advc_vaapi_image_runtime *advc_vaapi_image_runtime_create(
    const struct advc_vaapi_image_surface_ops *ops);
void advc_vaapi_image_runtime_destroy(
    struct advc_vaapi_image_runtime *runtime);

VAStatus advc_vaapi_image_query_formats(VAImageFormat *formats,
                                        int *num_formats);
VAStatus advc_vaapi_image_create(struct advc_vaapi_image_runtime *runtime,
                                 const VAImageFormat *format, int width,
                                 int height, VAImage *image);
VAStatus advc_vaapi_image_derive(struct advc_vaapi_image_runtime *runtime,
                                 VASurfaceID surface, VAImage *image);
VAStatus advc_vaapi_image_destroy(struct advc_vaapi_image_runtime *runtime,
                                  VAImageID image);
int advc_vaapi_image_owns_buffer(
    struct advc_vaapi_image_runtime *runtime, VABufferID buffer);
VAStatus advc_vaapi_image_map_buffer(
    struct advc_vaapi_image_runtime *runtime, VABufferID buffer,
    void **mapped);
VAStatus advc_vaapi_image_unmap_buffer(
    struct advc_vaapi_image_runtime *runtime, VABufferID buffer);

/*
 * Compatibility upload for the standard FFmpeg/GStreamer/OBS CPU-frame path.
 * It performs one explicit CPU pixel upload (and I420-to-NV12 interleave when
 * needed). It accepts only an unscaled, chroma-aligned region and an actual
 * modifier=0 NV12 destination; compressed buffers are never relabelled.
 */
VAStatus advc_vaapi_image_put(struct advc_vaapi_image_runtime *runtime,
                              VASurfaceID surface, VAImageID image,
                              int src_x, int src_y, unsigned int src_width,
                              unsigned int src_height, int dst_x, int dst_y,
                              unsigned int dst_width,
                              unsigned int dst_height);

/*
 * Monotonic accounting for successful CPU-origin pixel uploads. One call to
 * vaPutImage that copies a complete accepted region increments this counter
 * once, regardless of how many plane/row memcpy operations implement it.
 * PRIME writable exports never increment it.
 */
uint64_t advc_vaapi_image_cpu_pixel_copy_count(
    struct advc_vaapi_image_runtime *runtime);

/*
 * Strict DRM PRIME 2 bridge for GPU producers such as OBS. Import duplicates
 * every application-owned FD. Export returns duplicated FDs owned by the
 * caller. LINEAR retains strict byte-layout validation; the QCOM compressed
 * layout is accepted only as an opaque, single-object NV12 descriptor for a
 * separately gated Vulkan importer.
 * The encode runtime separately gates writable export and converts the
 * producer's implicit dma-buf fence into an explicit broker acquire fence.
 */
VAStatus advc_vaapi_prime_import_nv12_linear(
    const VADRMPRIMESurfaceDescriptor *prime, uint64_t buffer_id,
    uint32_t expected_width, uint32_t expected_height,
    struct advc_dmabuf_descriptor *descriptor);
VAStatus advc_vaapi_prime_import_nv12_modifier(
    const VADRMPRIMESurfaceDescriptor *prime, uint64_t buffer_id,
    uint32_t expected_width, uint32_t expected_height,
    uint64_t allowed_modifier, struct advc_dmabuf_descriptor *descriptor);
VAStatus advc_vaapi_prime_import_surface_attributes(
    unsigned int format, unsigned int width, unsigned int height,
    const VASurfaceAttrib *attributes, unsigned int num_attributes,
    unsigned int surface_index, unsigned int num_surfaces,
    uint64_t buffer_id, struct advc_dmabuf_descriptor *descriptor);
VAStatus advc_vaapi_prime_export_nv12(
    const struct advc_dmabuf_descriptor *descriptor, int acquire_fence_fd,
    uint32_t flags, VADRMPRIMESurfaceDescriptor *prime);
void advc_vaapi_prime_export_close(VADRMPRIMESurfaceDescriptor *prime);

/* Production sync helpers. Invalid/non-sync_file FDs fail closed. */
int advc_vaapi_wait_sync_file(int fd, uint32_t timeout_ms);

/*
 * Small encode lifecycle adapter. The descriptor remains owned by the caller.
 * The broker duplicates object FDs during registration. acquire_fence_fd is
 * consumed by submit; release_fence_fd is retained until wait or take.
 */
struct advc_vaapi_encode_surface_link {
    const struct advc_dmabuf_descriptor *descriptor;
    int acquire_fence_fd;
    int release_fence_fd;
    int registered;
};

void advc_vaapi_encode_surface_link_init(
    struct advc_vaapi_encode_surface_link *link,
    const struct advc_dmabuf_descriptor *descriptor);
int advc_vaapi_encode_surface_link_set_acquire_fence(
    struct advc_vaapi_encode_surface_link *link, int owned_fence_fd);
int advc_vaapi_encode_surface_link_register(
    struct advc_vaapi_encode_surface_link *link,
    struct advc_vaapi_encode_broker *broker);
int advc_vaapi_encode_surface_link_submit(
    struct advc_vaapi_encode_surface_link *link,
    struct advc_vaapi_encode_broker *broker, uint64_t pts_ns);
int advc_vaapi_encode_surface_link_wait(
    struct advc_vaapi_encode_surface_link *link, uint32_t timeout_ms);
int advc_vaapi_encode_surface_link_take_release_fence(
    struct advc_vaapi_encode_surface_link *link);
int advc_vaapi_encode_surface_link_unregister(
    struct advc_vaapi_encode_surface_link *link,
    struct advc_vaapi_encode_broker *broker, uint32_t timeout_ms);
void advc_vaapi_encode_surface_link_close(
    struct advc_vaapi_encode_surface_link *link);

#ifdef __cplusplus
}
#endif

#endif
