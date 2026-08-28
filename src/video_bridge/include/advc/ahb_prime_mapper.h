#ifndef ADVC_AHB_PRIME_MAPPER_H
#define ADVC_AHB_PRIME_MAPPER_H

#include "advc/dmabuf_ingress.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public AHardwareBuffer properties already reported by the Android API. */
struct advc_ahb_public_metadata {
    uint32_t width;
    uint32_t height;
    uint32_t android_format;
    uint32_t stride;
    uint32_t layers;
    uint64_t usage;
    /* Logical MediaCodec crop, expressed as origin plus extent. */
    uint32_t crop_left;
    uint32_t crop_top;
    uint32_t crop_width;
    uint32_t crop_height;
};

struct advc_ahb_prime_export {
    /* Owned PRIME FDs and authoritative plane/modifier metadata. */
    struct advc_dmabuf_descriptor descriptor;
    /* Owned decoder acquire sync_file, or -1 when already complete. */
    int acquire_fence_fd;
};

/*
 * Implemented inside Android by mapper/gralloc. hardware_buffer is deliberately
 * opaque: glibc code must never parse native_handle_t integer slots.
 *
 * export_prime returns owned CLOEXEC PRIME FDs in descriptor. It must provide
 * explicit fourcc, modifier, object index, offset and stride for every plane.
 * release consumes release_fence_fd on every return and retires the codec
 * output buffer only after the Debian consumer is finished.
 */
struct advc_ahb_prime_mapper_ops {
    int (*export_prime)(void *userdata, void *hardware_buffer,
                        const struct advc_ahb_public_metadata *public_metadata,
                        struct advc_dmabuf_descriptor *descriptor);
    int (*release)(void *userdata, void *lifetime_token,
                   int release_fence_fd);
    void (*destroy)(void *userdata);
};

struct advc_ahb_prime_mapper;

struct advc_ahb_prime_mapper *advc_ahb_prime_mapper_create(
    const struct advc_ahb_prime_mapper_ops *ops, void *userdata);
void advc_ahb_prime_mapper_destroy(struct advc_ahb_prime_mapper *mapper);

/* acquire_fence_fd is borrowed; the successful export owns a duplicate. */
int advc_ahb_prime_mapper_export(
    struct advc_ahb_prime_mapper *mapper, void *hardware_buffer,
    const struct advc_ahb_public_metadata *public_metadata, uint64_t buffer_id,
    int acquire_fence_fd, struct advc_ahb_prime_export *exported);
void advc_ahb_prime_export_close(struct advc_ahb_prime_export *exported);

/* Consumes release_fence_fd on every return. */
int advc_ahb_prime_mapper_release(struct advc_ahb_prime_mapper *mapper,
                                  void *lifetime_token,
                                  int release_fence_fd);

#ifdef __cplusplus
}
#endif

#endif
