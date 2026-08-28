#ifndef ADVC_DMABUF_INGRESS_H
#define ADVC_DMABUF_INGRESS_H

#include "advc/protocol.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum advc_color_primaries {
    ADVC_COLOR_PRIMARIES_UNSPECIFIED = 0,
    ADVC_COLOR_PRIMARIES_BT709 = 1,
    ADVC_COLOR_PRIMARIES_BT601_625 = 2,
    ADVC_COLOR_PRIMARIES_BT601_525 = 3,
    ADVC_COLOR_PRIMARIES_BT2020 = 4,
};

enum advc_color_transfer {
    ADVC_COLOR_TRANSFER_UNSPECIFIED = 0,
    ADVC_COLOR_TRANSFER_SRGB = 1,
    ADVC_COLOR_TRANSFER_BT709 = 2,
    ADVC_COLOR_TRANSFER_LINEAR = 3,
    ADVC_COLOR_TRANSFER_PQ = 4,
    ADVC_COLOR_TRANSFER_HLG = 5,
};

enum advc_color_matrix {
    ADVC_COLOR_MATRIX_UNSPECIFIED = 0,
    ADVC_COLOR_MATRIX_RGB = 1,
    ADVC_COLOR_MATRIX_BT601 = 2,
    ADVC_COLOR_MATRIX_BT709 = 3,
    ADVC_COLOR_MATRIX_BT2020 = 4,
};

enum advc_color_range {
    ADVC_COLOR_RANGE_UNSPECIFIED = 0,
    ADVC_COLOR_RANGE_FULL = 1,
    ADVC_COLOR_RANGE_LIMITED = 2,
};

enum advc_chroma_siting {
    ADVC_CHROMA_SITING_UNSPECIFIED = 0,
    ADVC_CHROMA_SITING_COSITED = 1,
    ADVC_CHROMA_SITING_MIDPOINT = 2,
};

struct advc_dmabuf_object {
    int fd; /* Borrowed during decode/validation; registry stores a CLOEXEC duplicate. */
    uint64_t size;
};

struct advc_dmabuf_plane {
    uint32_t object_index;
    uint64_t offset;
    uint32_t pitch;
};

struct advc_dmabuf_descriptor {
    uint64_t buffer_id;
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
    struct advc_dmabuf_object objects[ADVC_MAX_DMABUF_OBJECTS];
    struct advc_dmabuf_plane planes[ADVC_MAX_DMABUF_PLANES];
};

struct advc_dmabuf_submission {
    uint64_t buffer_id;
    uint64_t pts_ns;
    int acquire_fence_fd; /* Borrowed; -1 means producer work is already complete. */
};

struct advc_dmabuf_job {
    uint64_t buffer_id;
    uint64_t pts_ns;
    int acquire_fence_fd; /* Owned CLOEXEC duplicate, or -1. */
    /* Borrowed from the registry and stable until registry_finish(buffer_id). */
    const struct advc_dmabuf_descriptor *descriptor;
};

typedef int (*advc_dmabuf_format_allowed_fn)(
    void *userdata, const struct advc_dmabuf_descriptor *descriptor);

struct advc_dmabuf_registry;

/*
 * Encode/decode only the fixed v1.5 descriptor record. Decode aliases the
 * supplied FDs; it does not duplicate or take ownership of them. The caller
 * that owns the received FD array remains responsible for closing it.
 */
int advc_dmabuf_registration_encode(
    uint8_t payload[ADVC_REGISTER_DMABUF_SIZE],
    const struct advc_dmabuf_descriptor *descriptor,
    int fds[ADVC_MAX_DMABUF_OBJECTS], uint16_t *fd_count);
int advc_dmabuf_registration_decode(const uint8_t *payload, size_t payload_size,
                                    const int *fds, uint16_t fd_count,
                                    struct advc_dmabuf_descriptor *descriptor);
int advc_dmabuf_unregister_decode(const uint8_t *payload, size_t payload_size,
                                  uint16_t fd_count, uint64_t *buffer_id);
int advc_dmabuf_submission_decode(const uint8_t *payload, size_t payload_size,
                                  const int *fds, uint16_t fd_count,
                                  struct advc_dmabuf_submission *submission);
int advc_dmabuf_completion_validate(const uint8_t *payload, size_t payload_size,
                                    const int *fds, uint16_t fd_count);
/* Requires a real Linux sync_file, not merely an arbitrary CLOEXEC FD. */
int advc_dmabuf_sync_file_validate(int fd);

/* Structural validation only. Real EGL/Vulkan import remains authoritative. */
int advc_dmabuf_descriptor_validate(
    const struct advc_dmabuf_descriptor *descriptor);
/*
 * Closes unique owned FDs declared by object_count and resets the descriptor.
 * Producers must publish contiguous ownership before a later operation can
 * fail; undeclared slots are deliberately ignored.
 */
void advc_dmabuf_descriptor_close(struct advc_dmabuf_descriptor *descriptor);

/*
 * The policy callback is mandatory and represents a separately established exact
 * runtime allowlist. Registration still does not perform or advertise an import.
 */
struct advc_dmabuf_registry *advc_dmabuf_registry_create(
    advc_dmabuf_format_allowed_fn allowed, void *userdata);
void advc_dmabuf_registry_destroy(struct advc_dmabuf_registry *registry);
int advc_dmabuf_registry_register(
    struct advc_dmabuf_registry *registry,
    const struct advc_dmabuf_descriptor *descriptor);
int advc_dmabuf_registry_unregister(struct advc_dmabuf_registry *registry,
                                    uint64_t buffer_id);
int advc_dmabuf_registry_begin(struct advc_dmabuf_registry *registry,
                               const struct advc_dmabuf_submission *submission,
                               struct advc_dmabuf_job *job);
int advc_dmabuf_registry_finish(struct advc_dmabuf_registry *registry,
                                uint64_t buffer_id);
void advc_dmabuf_job_close(struct advc_dmabuf_job *job);
size_t advc_dmabuf_registry_registered_count(
    const struct advc_dmabuf_registry *registry);
size_t advc_dmabuf_registry_inflight_count(
    const struct advc_dmabuf_registry *registry);

#ifdef __cplusplus
}
#endif

#endif
