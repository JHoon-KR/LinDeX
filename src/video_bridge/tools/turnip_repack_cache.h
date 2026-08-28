#ifndef ADVC_TURNIP_REPACK_CACHE_H
#define ADVC_TURNIP_REPACK_CACHE_H

#include "advc/dmabuf_ingress.h"

#include <stddef.h>
#include <stdint.h>

struct advc_repack_fd_identity {
    uint64_t device;
    uint64_t inode;
    uint64_t rdevice;
};

/* Canonical descriptor data which affects a Vulkan dma-buf import. */
struct advc_repack_descriptor_signature {
    uint32_t width;
    uint32_t height;
    uint32_t drm_fourcc;
    uint32_t explicit_flags;
    uint64_t drm_modifier;
    uint32_t crop_left;
    uint32_t crop_top;
    uint32_t crop_width;
    uint32_t crop_height;
    uint32_t object_count;
    uint32_t plane_count;
    uint32_t color_primaries;
    uint32_t color_transfer;
    uint32_t color_matrix;
    uint32_t color_range;
    uint32_t chroma_horizontal;
    uint32_t chroma_vertical;
    uint64_t object_sizes[ADVC_MAX_DMABUF_OBJECTS];
    struct advc_dmabuf_plane planes[ADVC_MAX_DMABUF_PLANES];
};

struct advc_repack_source_key {
    int occupied;
    struct advc_repack_fd_identity identity;
    struct advc_repack_descriptor_signature signature;
    uint64_t last_use;
};

struct advc_repack_lease_key {
    int leased;
    uint64_t token;
};

int advc_repack_fd_identity_from_fd(int fd,
                                    struct advc_repack_fd_identity *identity);
void advc_repack_descriptor_signature_make(
    const struct advc_dmabuf_descriptor *descriptor,
    struct advc_repack_descriptor_signature *signature);
int advc_repack_descriptor_signature_equal(
    const struct advc_repack_descriptor_signature *left,
    const struct advc_repack_descriptor_signature *right);

/*
 * Selects an exact source import, an invalidated same-object entry, an empty
 * entry, or finally the LRU entry. The caller owns destruction/recreation.
 */
size_t advc_repack_source_key_select(
    const struct advc_repack_source_key *keys, size_t count,
    const struct advc_repack_fd_identity *identity,
    const struct advc_repack_descriptor_signature *signature,
    int *exact_match);

/* Bounded, stale-token-safe lease bookkeeping for exported destination slots. */
int advc_repack_lease_acquire(struct advc_repack_lease_key *keys,
                              size_t count, uint64_t *next_token,
                              size_t *slot_index, uint64_t *token);
int advc_repack_lease_find(const struct advc_repack_lease_key *keys,
                           size_t count, uint64_t token,
                           size_t *slot_index);
void advc_repack_lease_clear(struct advc_repack_lease_key *key);

#endif
