#ifndef ADVC_TURNIP_PRIME_IMPORT_H
#define ADVC_TURNIP_PRIME_IMPORT_H

#include "advc/dmabuf_ingress.h"

#include <stddef.h>
#include <stdint.h>

struct advc_turnip_prime_result {
    uint64_t content_hash;
    uint64_t content_bytes;
    uint32_t distinct_sample_values;
    int release_fence_fd; /* Owned sync_file on success. */
    char device_name[256];
};

struct advc_turnip_linear_repack_result {
    /* Owned explicit-modifier LINEAR descriptor on success. */
    struct advc_dmabuf_descriptor descriptor;
    /* Owned sync_file for the destination's next importer. */
    int acquire_fence_fd;
    /* Owned sync_file releasing the borrowed source back to its producer. */
    int source_release_fence_fd;
    /* Non-zero destination-pool lease; returned exactly once by release/discard. */
    uint64_t lease_token;
    char device_name[256];
};

struct advc_turnip_linear_repack_pool;

/* A gateway-local bounded pool. max_slots must be in [1, 32]. */
struct advc_turnip_linear_repack_pool *
advc_turnip_linear_repack_pool_create(size_t max_slots);
void advc_turnip_linear_repack_pool_destroy(
    struct advc_turnip_linear_repack_pool *pool);

/*
 * Recycles a leased output. release_fence_fd is borrowed and, when present,
 * becomes a Vulkan wait before that destination is written again.
 */
int advc_turnip_linear_repack_pool_release(
    struct advc_turnip_linear_repack_pool *pool, uint64_t lease_token,
    int release_fence_fd);
/* Invalidates rather than reuses a lease (flush, disconnect, or failed send). */
int advc_turnip_linear_repack_pool_discard(
    struct advc_turnip_linear_repack_pool *pool, uint64_t lease_token);

/*
 * Validation-only offscreen consumer for lfdevs/mesa-for-android-container.
 * It imports an exact NV12 LINEAR/QCOM descriptor into Turnip, waits on a
 * duplicate of acquire_fence_fd, performs a GPU plane copy for content proof,
 * and returns a real Vulkan release sync_file. Descriptor FDs are borrowed.
 */
int advc_turnip_prime_consume(
    const struct advc_dmabuf_descriptor *descriptor, int acquire_fence_fd,
    struct advc_turnip_prime_result *result);

/*
 * Bounded Turnip GPU repack used by the validation client and by the
 * explicitly gated VA-API decode runtime. The source must be an exact QCOM
 * compressed NV12 descriptor. A separate exportable modifier=0 NV12 image is
 * allocated and populated with one Vulkan image-to-image transfer. No source
 * FD is mapped and no raw pixels pass through CPU memory.
 *
 * source and source_acquire_fence_fd are borrowed. The result owns its dma-buf
 * and both sync_file FDs; close it with advc_turnip_linear_repack_close().
 * Failure is fail-closed and leaves result inert.
 */
int advc_turnip_prime_repack_linear(
    const struct advc_dmabuf_descriptor *source,
    int source_acquire_fence_fd, uint64_t output_buffer_id,
    struct advc_turnip_linear_repack_result *result);
int advc_turnip_prime_repack_linear_pooled(
    struct advc_turnip_linear_repack_pool *pool,
    const struct advc_dmabuf_descriptor *source,
    int source_acquire_fence_fd, uint64_t output_buffer_id,
    struct advc_turnip_linear_repack_result *result);
/*
 * Reserves and exports an empty LINEAR NV12 destination.  The returned lease
 * remains owned by the caller until repack_reserved, release, or discard.
 */
int advc_turnip_linear_repack_pool_reserve(
    struct advc_turnip_linear_repack_pool *pool, uint32_t width,
    uint32_t height, uint64_t reservation_id,
    struct advc_turnip_linear_repack_result *result);
/* Populates an already exported reservation without changing its dma-buf. */
int advc_turnip_prime_repack_linear_reserved(
    struct advc_turnip_linear_repack_pool *pool, uint64_t lease_token,
    const struct advc_dmabuf_descriptor *source,
    int source_acquire_fence_fd, uint64_t output_buffer_id,
    struct advc_turnip_linear_repack_result *result);
void advc_turnip_linear_repack_close(
    struct advc_turnip_linear_repack_result *result);

#endif
