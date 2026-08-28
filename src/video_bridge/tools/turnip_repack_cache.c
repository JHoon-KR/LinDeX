#define _GNU_SOURCE
#include "turnip_repack_cache.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

int advc_repack_fd_identity_from_fd(int fd,
                                    struct advc_repack_fd_identity *identity) {
    struct stat status;
    if (fd < 0 || identity == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (fstat(fd, &status) < 0) return -1;
    identity->device = (uint64_t)status.st_dev;
    identity->inode = (uint64_t)status.st_ino;
    identity->rdevice = (uint64_t)status.st_rdev;
    return 0;
}

void advc_repack_descriptor_signature_make(
    const struct advc_dmabuf_descriptor *descriptor,
    struct advc_repack_descriptor_signature *signature) {
    memset(signature, 0, sizeof(*signature));
    if (descriptor == NULL) return;
    signature->width = descriptor->width;
    signature->height = descriptor->height;
    signature->drm_fourcc = descriptor->drm_fourcc;
    signature->explicit_flags = descriptor->explicit_flags;
    signature->drm_modifier = descriptor->drm_modifier;
    signature->crop_left = descriptor->crop_left;
    signature->crop_top = descriptor->crop_top;
    signature->crop_width = descriptor->crop_width;
    signature->crop_height = descriptor->crop_height;
    signature->object_count = descriptor->object_count;
    signature->plane_count = descriptor->plane_count;
    signature->color_primaries = descriptor->color_primaries;
    signature->color_transfer = descriptor->color_transfer;
    signature->color_matrix = descriptor->color_matrix;
    signature->color_range = descriptor->color_range;
    signature->chroma_horizontal = descriptor->chroma_horizontal;
    signature->chroma_vertical = descriptor->chroma_vertical;
    for (uint32_t i = 0; i < descriptor->object_count &&
                         i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        signature->object_sizes[i] = descriptor->objects[i].size;
    for (uint32_t i = 0; i < descriptor->plane_count &&
                         i < ADVC_MAX_DMABUF_PLANES; ++i) {
        signature->planes[i].object_index =
            descriptor->planes[i].object_index;
        signature->planes[i].offset = descriptor->planes[i].offset;
        signature->planes[i].pitch = descriptor->planes[i].pitch;
    }
}

int advc_repack_descriptor_signature_equal(
    const struct advc_repack_descriptor_signature *left,
    const struct advc_repack_descriptor_signature *right) {
    return left != NULL && right != NULL &&
           memcmp(left, right, sizeof(*left)) == 0;
}

static int identity_equal(const struct advc_repack_fd_identity *left,
                          const struct advc_repack_fd_identity *right) {
    return left->device == right->device && left->inode == right->inode &&
           left->rdevice == right->rdevice;
}

size_t advc_repack_source_key_select(
    const struct advc_repack_source_key *keys, size_t count,
    const struct advc_repack_fd_identity *identity,
    const struct advc_repack_descriptor_signature *signature,
    int *exact_match) {
    size_t same_identity = SIZE_MAX;
    size_t empty = SIZE_MAX;
    size_t lru = SIZE_MAX;
    uint64_t oldest = UINT64_MAX;
    if (exact_match != NULL) *exact_match = 0;
    if (keys == NULL || identity == NULL || signature == NULL || count == 0)
        return SIZE_MAX;
    for (size_t i = 0; i < count; ++i) {
        if (!keys[i].occupied) {
            if (empty == SIZE_MAX) empty = i;
            continue;
        }
        if (identity_equal(&keys[i].identity, identity)) {
            if (advc_repack_descriptor_signature_equal(&keys[i].signature,
                                                        signature)) {
                if (exact_match != NULL) *exact_match = 1;
                return i;
            }
            if (same_identity == SIZE_MAX) same_identity = i;
        }
        if (lru == SIZE_MAX || keys[i].last_use < oldest) {
            lru = i;
            oldest = keys[i].last_use;
        }
    }
    if (same_identity != SIZE_MAX) return same_identity;
    if (empty != SIZE_MAX) return empty;
    return lru;
}

static uint64_t allocate_token(uint64_t *next_token,
                               const struct advc_repack_lease_key *keys,
                               size_t count) {
    for (size_t attempt = 0; attempt <= count; ++attempt) {
        uint64_t candidate = ++*next_token;
        int collision = 0;
        if (candidate == 0) candidate = ++*next_token;
        for (size_t i = 0; i < count; ++i)
            if (keys[i].leased && keys[i].token == candidate) collision = 1;
        if (!collision) return candidate;
    }
    return 0;
}

int advc_repack_lease_acquire(struct advc_repack_lease_key *keys,
                              size_t count, uint64_t *next_token,
                              size_t *slot_index, uint64_t *token) {
    if (keys == NULL || next_token == NULL || slot_index == NULL ||
        token == NULL || count == 0) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < count; ++i) {
        uint64_t allocated;
        if (keys[i].leased) continue;
        allocated = allocate_token(next_token, keys, count);
        if (allocated == 0) {
            errno = EOVERFLOW;
            return -1;
        }
        keys[i].leased = 1;
        keys[i].token = allocated;
        *slot_index = i;
        *token = allocated;
        return 0;
    }
    errno = ENOSPC;
    return -1;
}

int advc_repack_lease_find(const struct advc_repack_lease_key *keys,
                           size_t count, uint64_t token,
                           size_t *slot_index) {
    if (keys == NULL || slot_index == NULL || token == 0) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < count; ++i) {
        if (keys[i].leased && keys[i].token == token) {
            *slot_index = i;
            return 0;
        }
    }
    errno = ENOENT;
    return -1;
}

void advc_repack_lease_clear(struct advc_repack_lease_key *key) {
    if (key == NULL) return;
    key->leased = 0;
    key->token = 0;
}
