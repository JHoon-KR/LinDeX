/*
 * Copyright 2026 JHoon
 * SPDX-License-Identifier: Apache-2.0
 *
 * The wire decoding implemented by this interface follows the Android Open
 * Source Project IMapper stable-C metadata contract.  It intentionally covers
 * only metadata required for LinDeX DRM PRIME export.
 */
#ifndef ADVC_STABLE_MAPPER_METADATA_H
#define ADVC_STABLE_MAPPER_METADATA_H

#include <stddef.h>
#include <stdint.h>

#include "advc/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

enum advc_standard_metadata_type {
    ADVC_STANDARD_METADATA_BUFFER_ID = 1,
    ADVC_STANDARD_METADATA_WIDTH = 3,
    ADVC_STANDARD_METADATA_HEIGHT = 4,
    ADVC_STANDARD_METADATA_LAYER_COUNT = 5,
    ADVC_STANDARD_METADATA_PIXEL_FORMAT_FOURCC = 7,
    ADVC_STANDARD_METADATA_PIXEL_FORMAT_MODIFIER = 8,
    ADVC_STANDARD_METADATA_ALLOCATION_SIZE = 10,
    ADVC_STANDARD_METADATA_PLANE_LAYOUTS = 15,
    ADVC_STANDARD_METADATA_CROP = 16,
    ADVC_STANDARD_METADATA_STRIDE = 23,
};

#define ADVC_MAX_MAPPER_PLANE_COMPONENTS 16u
#define ADVC_MAX_MAPPER_COMPONENT_NAME 256u

struct advc_mapper_plane_component {
    char type_name[ADVC_MAX_MAPPER_COMPONENT_NAME];
    int64_t type_value;
    int64_t offset_bits;
    int64_t size_bits;
};

struct advc_mapper_plane_layout {
    uint32_t component_count;
    struct advc_mapper_plane_component
        components[ADVC_MAX_MAPPER_PLANE_COMPONENTS];
    uint64_t offset_bytes;
    uint64_t sample_increment_bits;
    uint64_t stride_bytes;
    uint64_t width_samples;
    uint64_t height_samples;
    uint64_t total_size_bytes;
    uint64_t horizontal_subsampling;
    uint64_t vertical_subsampling;
};

struct advc_mapper_plane_layouts {
    uint32_t count;
    struct advc_mapper_plane_layout planes[ADVC_MAX_DMABUF_PLANES];
};

struct advc_mapper_crop {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

struct advc_mapper_crops {
    uint32_t count;
    struct advc_mapper_crop crops[ADVC_MAX_DMABUF_PLANES];
};

enum advc_prime_layout_gate_status {
    ADVC_PRIME_LAYOUT_GATE_PASS = 0,
    ADVC_PRIME_LAYOUT_GATE_CROP_PLANE_COUNT_MISMATCH = 1,
    ADVC_PRIME_LAYOUT_GATE_AMBIGUOUS_MULTI_FD = 2,
};

enum advc_mapper_qti_nv12_layout_kind {
    ADVC_MAPPER_QTI_NV12_LINEAR = 1,
    ADVC_MAPPER_QTI_NV12_UBWC = 2,
};

struct advc_mapper_qti_nv12_layout {
    enum advc_mapper_qti_nv12_layout_kind kind;
    uint32_t image_transport_index;
    uint32_t plane_count;
    uint64_t plane_offsets[2];
    uint32_t plane_strides[2];
};

int advc_mapper_metadata_decode_u32(const void *data, size_t size,
                                    int64_t metadata_type, uint32_t *value);
int advc_mapper_metadata_decode_u64(const void *data, size_t size,
                                    int64_t metadata_type, uint64_t *value);
int advc_mapper_metadata_decode_plane_layouts(
    const void *data, size_t size, struct advc_mapper_plane_layouts *layouts);
int advc_mapper_metadata_decode_crops(const void *data, size_t size,
                                      struct advc_mapper_crops *crops);
enum advc_prime_layout_gate_status advc_mapper_prime_layout_gate(
    uint32_t transport_fds, uint32_t plane_count, uint32_t crop_count);

/*
 * Normalize only the measured QTI NV12 conventions.  This helper never guesses
 * an image object or a modifier: the QTI FD metadata, allocation size, component
 * types and all plane relationships must agree exactly.  It accepts both the
 * four-layout UBWC form and a conforming two-data-layout LINEAR form.
 */
int advc_mapper_qti_nv12_normalize(
    uint32_t fourcc, uint64_t modifier, uint64_t allocation_size,
    uint32_t width, uint32_t height, uint32_t stride,
    uint32_t transport_fds,
    const uint64_t transport_fd_sizes[ADVC_MAX_DMABUF_OBJECTS],
    int32_t qti_data_fd_transport_index, uint32_t qti_data_fd_valid,
    uint64_t qti_data_fd_size,
    const struct advc_mapper_plane_layouts *layouts,
    const struct advc_mapper_crops *crops,
    struct advc_mapper_qti_nv12_layout *normalized);

#ifdef __cplusplus
}
#endif

#endif
