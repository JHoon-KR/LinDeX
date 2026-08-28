#ifndef ADVC_GLIBC_IMPORT_H
#define ADVC_GLIBC_IMPORT_H

#include "advc/client.h"
#include "advc/protocol.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * AHardwareBuffer_sendHandleToUnixSocket has a public API but its record format
 * is not a public ABI. A glibc process may preserve the record and its owned
 * file descriptors, but must not interpret native_handle integer slots.
 */
#define ADVC_OPAQUE_NATIVE_HANDLE_MAX_BYTES 4096u

struct advc_glibc_opaque_ahb {
    uint8_t payload[ADVC_OPAQUE_NATIVE_HANDLE_MAX_BYTES];
    size_t payload_size;
    int fds[ADVC_MAX_FDS];
    uint16_t fd_count;
};

/* Callback compatible with advc_client_transfer_ahb(). */
int advc_glibc_receive_opaque_ahb(int socket_fd, void **native_buffer,
                                  void *userdata);
void advc_glibc_opaque_ahb_close(struct advc_glibc_opaque_ahb *handle);

/*
 * Checks the exact public ADVC/AHardwareBuffer metadata contract. This does
 * not claim that any opaque native-handle FD is a dma-buf or reveal its layout.
 */
int advc_glibc_ahb_validate(const struct advc_client_output *output,
                            const struct advc_glibc_opaque_ahb *handle);

#define ADVC_PRIME_EXPLICIT_FOURCC (UINT32_C(1) << 0)
#define ADVC_PRIME_EXPLICIT_MODIFIER (UINT32_C(1) << 1)
#define ADVC_PRIME_EXPLICIT_PLANES (UINT32_C(1) << 2)
#define ADVC_PRIME_EXPLICIT_ALL                                              \
    (ADVC_PRIME_EXPLICIT_FOURCC | ADVC_PRIME_EXPLICIT_MODIFIER |             \
     ADVC_PRIME_EXPLICIT_PLANES)

struct advc_drm_prime_plane {
    int fd; /* Borrowed; the contract owner keeps it open during import. */
    uint32_t offset;
    uint32_t pitch;
};

/*
 * A future broker-side mapper may populate this only from authoritative DRM
 * metadata. Modifier zero is valid LINEAR only when EXPLICIT_MODIFIER is set.
 */
struct advc_drm_prime_import {
    uint32_t width;
    uint32_t height;
    uint32_t drm_fourcc;
    uint64_t drm_modifier;
    uint32_t plane_count;
    uint32_t explicit_flags;
    struct advc_drm_prime_plane planes[4];
    int acquire_fence_fd; /* Borrowed, or -1. */
};

int advc_drm_prime_import_validate(
    const struct advc_drm_prime_import *import_contract);

/*
 * Deliberately fails with ENOTSUP for ADVC 1.2 AHB records: the public AHB
 * socket API does not specify native-handle slots, DRM fourcc, plane mapping,
 * or modifier/UBWC metadata.
 */
int advc_glibc_ahb_to_prime(const struct advc_client_output *output,
                            const struct advc_glibc_opaque_ahb *handle,
                            struct advc_drm_prime_import *import_contract);

#ifdef __cplusplus
}
#endif

#endif
