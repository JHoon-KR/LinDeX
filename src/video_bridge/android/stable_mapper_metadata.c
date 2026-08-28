/*
 * Copyright 2026 JHoon
 * SPDX-License-Identifier: Apache-2.0
 *
 * Decoder for the standard IMapper metadata stream described by AOSP's
 * IMapperMetadataTypes.h (Copyright 2022 The Android Open Source Project,
 * Apache-2.0).  No vendor-private native-handle layout is interpreted here.
 */
#include "stable_mapper_metadata.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

static const char standard_metadata_name[] =
    "android.hardware.graphics.common.StandardMetadataType";

struct metadata_reader {
    const uint8_t *cursor;
    size_t remaining;
};

static int take(struct metadata_reader *reader, void *value, size_t size) {
    if (size > reader->remaining) {
        errno = EPROTO;
        return -1;
    }
    if (value != NULL) memcpy(value, reader->cursor, size);
    reader->cursor += size;
    reader->remaining -= size;
    return 0;
}

static int skip_string(struct metadata_reader *reader, int require_standard) {
    int64_t length;
    if (take(reader, &length, sizeof(length)) < 0 || length < 0 ||
        (uint64_t)length > SIZE_MAX || (size_t)length > reader->remaining) {
        errno = EPROTO;
        return -1;
    }
    if (require_standard &&
        ((size_t)length != sizeof(standard_metadata_name) - 1u ||
         memcmp(reader->cursor, standard_metadata_name, (size_t)length) != 0)) {
        errno = EPROTO;
        return -1;
    }
    return take(reader, NULL, (size_t)length);
}

static int read_string(struct metadata_reader *reader, char *value,
                       size_t value_size) {
    int64_t length;
    if (value == NULL || value_size == 0 ||
        take(reader, &length, sizeof(length)) < 0 || length < 0 ||
        (uint64_t)length > SIZE_MAX || (size_t)length > reader->remaining ||
        (size_t)length >= value_size) {
        errno = EPROTO;
        return -1;
    }
    memcpy(value, reader->cursor, (size_t)length);
    value[length] = '\0';
    return take(reader, NULL, (size_t)length);
}

static int read_header(struct metadata_reader *reader, const void *data,
                       size_t size, int64_t expected_type) {
    int64_t actual_type;
    if (reader == NULL || data == NULL || size == 0) {
        errno = EINVAL;
        return -1;
    }
    reader->cursor = (const uint8_t *)data;
    reader->remaining = size;
    if (skip_string(reader, 1) < 0 ||
        take(reader, &actual_type, sizeof(actual_type)) < 0 ||
        actual_type != expected_type) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

int advc_mapper_metadata_decode_u32(const void *data, size_t size,
                                    int64_t metadata_type, uint32_t *value) {
    struct metadata_reader reader;
    if (value == NULL || read_header(&reader, data, size, metadata_type) < 0 ||
        take(&reader, value, sizeof(*value)) < 0 || reader.remaining != 0) {
        if (errno == 0) errno = EPROTO;
        return -1;
    }
    return 0;
}

int advc_mapper_metadata_decode_u64(const void *data, size_t size,
                                    int64_t metadata_type, uint64_t *value) {
    struct metadata_reader reader;
    if (value == NULL || read_header(&reader, data, size, metadata_type) < 0 ||
        take(&reader, value, sizeof(*value)) < 0 || reader.remaining != 0) {
        if (errno == 0) errno = EPROTO;
        return -1;
    }
    return 0;
}

int advc_mapper_metadata_decode_plane_layouts(
    const void *data, size_t size, struct advc_mapper_plane_layouts *layouts) {
    struct metadata_reader reader;
    int64_t count;
    if (layouts == NULL ||
        read_header(&reader, data, size,
                    ADVC_STANDARD_METADATA_PLANE_LAYOUTS) < 0 ||
        take(&reader, &count, sizeof(count)) < 0 || count <= 0 ||
        count > ADVC_MAX_DMABUF_PLANES) {
        errno = EPROTO;
        return -1;
    }
    memset(layouts, 0, sizeof(*layouts));
    layouts->count = (uint32_t)count;
    for (uint32_t i = 0; i < layouts->count; ++i) {
        struct advc_mapper_plane_layout *plane = &layouts->planes[i];
        int64_t components;
        int64_t fields[8];
        if (take(&reader, &components, sizeof(components)) < 0 ||
            components < 0 ||
            components > (int64_t)ADVC_MAX_MAPPER_PLANE_COMPONENTS) {
            errno = EPROTO;
            return -1;
        }
        plane->component_count = (uint32_t)components;
        for (int64_t component = 0; component < components; ++component) {
            struct advc_mapper_plane_component *decoded =
                &plane->components[component];
            int64_t offset_bits;
            int64_t size_bits;
            if (read_string(&reader, decoded->type_name,
                            sizeof(decoded->type_name)) < 0 ||
                take(&reader, &decoded->type_value,
                     sizeof(decoded->type_value)) < 0 ||
                take(&reader, &offset_bits, sizeof(offset_bits)) < 0 ||
                take(&reader, &size_bits, sizeof(size_bits)) < 0)
                return -1;
            decoded->offset_bits = offset_bits;
            decoded->size_bits = size_bits;
        }
        for (size_t field = 0; field < 8; ++field) {
            if (take(&reader, &fields[field], sizeof(fields[field])) < 0 ||
                fields[field] < 0) {
                errno = EPROTO;
                return -1;
            }
        }
        plane->offset_bytes = (uint64_t)fields[0];
        plane->sample_increment_bits = (uint64_t)fields[1];
        plane->stride_bytes = (uint64_t)fields[2];
        plane->width_samples = (uint64_t)fields[3];
        plane->height_samples = (uint64_t)fields[4];
        plane->total_size_bytes = (uint64_t)fields[5];
        plane->horizontal_subsampling = (uint64_t)fields[6];
        plane->vertical_subsampling = (uint64_t)fields[7];
        if (plane->stride_bytes == 0 || plane->stride_bytes > UINT32_MAX ||
            plane->total_size_bytes == 0) {
            errno = EPROTO;
            return -1;
        }
    }
    if (reader.remaining != 0) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

int advc_mapper_metadata_decode_crops(const void *data, size_t size,
                                      struct advc_mapper_crops *crops) {
    struct metadata_reader reader;
    int64_t count;
    if (crops == NULL ||
        read_header(&reader, data, size, ADVC_STANDARD_METADATA_CROP) < 0 ||
        take(&reader, &count, sizeof(count)) < 0 || count <= 0 ||
        count > ADVC_MAX_DMABUF_PLANES) {
        errno = EPROTO;
        return -1;
    }
    memset(crops, 0, sizeof(*crops));
    crops->count = (uint32_t)count;
    for (uint32_t i = 0; i < crops->count; ++i) {
        struct advc_mapper_crop *crop = &crops->crops[i];
        if (take(&reader, &crop->left, sizeof(crop->left)) < 0 ||
            take(&reader, &crop->top, sizeof(crop->top)) < 0 ||
            take(&reader, &crop->right, sizeof(crop->right)) < 0 ||
            take(&reader, &crop->bottom, sizeof(crop->bottom)) < 0 ||
            crop->left < 0 || crop->top < 0 || crop->right <= crop->left ||
            crop->bottom <= crop->top) {
            errno = EPROTO;
            return -1;
        }
    }
    if (reader.remaining != 0) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

enum advc_prime_layout_gate_status advc_mapper_prime_layout_gate(
    uint32_t transport_fds, uint32_t plane_count, uint32_t crop_count) {
    /*
     * Standard PLANE_LAYOUTS has no plane-to-transport-FD/object-index field,
     * and standard CROP is defined per plane. Both contracts must therefore be
     * exact before LinDeX may advertise or export decode PRIME.
     */
    if (crop_count != plane_count)
        return ADVC_PRIME_LAYOUT_GATE_CROP_PLANE_COUNT_MISMATCH;
    if (transport_fds != 1)
        return ADVC_PRIME_LAYOUT_GATE_AMBIGUOUS_MULTI_FD;
    return ADVC_PRIME_LAYOUT_GATE_PASS;
}

#define ADVC_DRM_FORMAT_NV12 UINT32_C(0x3231564e)
#define ADVC_DRM_FORMAT_MOD_LINEAR UINT64_C(0)
#define ADVC_DRM_FORMAT_MOD_QCOM_COMPRESSED UINT64_C(0x0500000000000001)
#define ADVC_STANDARD_COMPONENT_Y INT64_C(1)
#define ADVC_STANDARD_COMPONENT_CB INT64_C(2)
#define ADVC_STANDARD_COMPONENT_CR INT64_C(4)
#define ADVC_QTI_COMPONENT_METADATA INT64_C(-2147483648)

static const char standard_component_name[] =
    "android.hardware.graphics.common.PlaneLayoutComponentType";

static int component_is(const struct advc_mapper_plane_component *component,
                        const char *name, int64_t value, int64_t offset_bits,
                        int64_t size_bits) {
    return component != NULL && strcmp(component->type_name, name) == 0 &&
           component->type_value == value &&
           component->offset_bits == offset_bits &&
           component->size_bits == size_bits;
}

static int layout_is_y_data(const struct advc_mapper_plane_layout *layout) {
    return layout != NULL && layout->component_count == 1 &&
           component_is(&layout->components[0], standard_component_name,
                        ADVC_STANDARD_COMPONENT_Y, 0, 8);
}

static int layout_is_uv_data(const struct advc_mapper_plane_layout *layout) {
    int cb = 0;
    int cr = 0;
    if (layout == NULL || layout->component_count != 2) return 0;
    for (uint32_t i = 0; i < layout->component_count; ++i) {
        cb += component_is(&layout->components[i], standard_component_name,
                           ADVC_STANDARD_COMPONENT_CB, 0, 8);
        cr += component_is(&layout->components[i], standard_component_name,
                           ADVC_STANDARD_COMPONENT_CR, 8, 8);
    }
    return cb == 1 && cr == 1;
}

static int layout_has_qti_metadata(
    const struct advc_mapper_plane_layout *layout) {
    int qti = 0;
    if (layout == NULL) return 0;
    for (uint32_t i = 0; i < layout->component_count; ++i) {
        qti += component_is(&layout->components[i], "QTI",
                            ADVC_QTI_COMPONENT_METADATA, 0, 0);
    }
    return qti == 1;
}

static int layout_is_y_metadata(
    const struct advc_mapper_plane_layout *layout) {
    int y = 0;
    if (layout == NULL || layout->component_count != 2 ||
        !layout_has_qti_metadata(layout))
        return 0;
    for (uint32_t i = 0; i < layout->component_count; ++i) {
        y += component_is(&layout->components[i], standard_component_name,
                          ADVC_STANDARD_COMPONENT_Y, 0, 0);
    }
    return y == 1;
}

static int layout_is_uv_metadata(
    const struct advc_mapper_plane_layout *layout) {
    int cb = 0;
    int cr = 0;
    if (layout == NULL || layout->component_count != 3 ||
        !layout_has_qti_metadata(layout))
        return 0;
    for (uint32_t i = 0; i < layout->component_count; ++i) {
        cb += component_is(&layout->components[i], standard_component_name,
                           ADVC_STANDARD_COMPONENT_CB, 0, 0);
        cr += component_is(&layout->components[i], standard_component_name,
                           ADVC_STANDARD_COMPONENT_CR, 0, 0);
    }
    return cb == 1 && cr == 1;
}

static int layout_extent_valid(const struct advc_mapper_plane_layout *layout,
                               uint64_t allocation_size) {
    return layout != NULL && layout->stride_bytes > 0 &&
           layout->stride_bytes <= UINT32_MAX &&
           layout->offset_bytes < allocation_size &&
           layout->total_size_bytes > 0 &&
           layout->total_size_bytes <= allocation_size - layout->offset_bytes;
}

static int crop_covers_image(const struct advc_mapper_crop *crop,
                             uint32_t width, uint32_t height,
                             uint32_t stride,
                             const struct advc_mapper_plane_layout *y_data) {
    uint64_t allocated_rows;
    if (crop == NULL || y_data == NULL || crop->left != 0 || crop->top != 0 ||
        crop->right < 0 || crop->bottom < 0 ||
        (uint64_t)crop->right < width || (uint64_t)crop->bottom < height ||
        (uint64_t)crop->right > stride)
        return 0;
    allocated_rows = y_data->total_size_bytes / y_data->stride_bytes;
    return (uint64_t)crop->bottom <= allocated_rows;
}

int advc_mapper_qti_nv12_normalize(
    uint32_t fourcc, uint64_t modifier, uint64_t allocation_size,
    uint32_t width, uint32_t height, uint32_t stride,
    uint32_t transport_fds,
    const uint64_t transport_fd_sizes[ADVC_MAX_DMABUF_OBJECTS],
    int32_t qti_data_fd_transport_index, uint32_t qti_data_fd_valid,
    uint64_t qti_data_fd_size,
    const struct advc_mapper_plane_layouts *layouts,
    const struct advc_mapper_crops *crops,
    struct advc_mapper_qti_nv12_layout *normalized) {
    const struct advc_mapper_plane_layout *y_data = NULL;
    const struct advc_mapper_plane_layout *uv_data = NULL;
    const struct advc_mapper_plane_layout *y_metadata = NULL;
    const struct advc_mapper_plane_layout *uv_metadata = NULL;

    if (normalized == NULL || layouts == NULL || crops == NULL ||
        transport_fd_sizes == NULL || fourcc != ADVC_DRM_FORMAT_NV12 ||
        (modifier != ADVC_DRM_FORMAT_MOD_LINEAR &&
         modifier != ADVC_DRM_FORMAT_MOD_QCOM_COMPRESSED) ||
        allocation_size == 0 || width == 0 || height == 0 || stride < width ||
        transport_fds == 0 || transport_fds > ADVC_MAX_DMABUF_OBJECTS ||
        !qti_data_fd_valid || qti_data_fd_transport_index < 0 ||
        (uint32_t)qti_data_fd_transport_index >= transport_fds ||
        qti_data_fd_size != allocation_size ||
        transport_fd_sizes[qti_data_fd_transport_index] != allocation_size ||
        crops->count != 1) {
        errno = ENOTSUP;
        return -1;
    }
    memset(normalized, 0, sizeof(*normalized));
    for (uint32_t i = 0; i < layouts->count; ++i) {
        const struct advc_mapper_plane_layout *layout = &layouts->planes[i];
        if (!layout_extent_valid(layout, allocation_size)) {
            errno = EPROTO;
            return -1;
        }
        if (layout_is_y_data(layout)) {
            if (y_data != NULL) goto ambiguous;
            y_data = layout;
        } else if (layout_is_uv_data(layout)) {
            if (uv_data != NULL) goto ambiguous;
            uv_data = layout;
        } else if (layout_is_y_metadata(layout)) {
            if (y_metadata != NULL) goto ambiguous;
            y_metadata = layout;
        } else if (layout_is_uv_metadata(layout)) {
            if (uv_metadata != NULL) goto ambiguous;
            uv_metadata = layout;
        } else {
            goto ambiguous;
        }
    }
    if (y_data == NULL || uv_data == NULL ||
        y_data->stride_bytes != stride || uv_data->stride_bytes != stride ||
        y_data->width_samples != width || y_data->height_samples != height ||
        y_data->horizontal_subsampling != 1 ||
        y_data->vertical_subsampling != 1 ||
        uv_data->width_samples != (width + 1u) / 2u ||
        uv_data->height_samples != (height + 1u) / 2u ||
        uv_data->horizontal_subsampling != 2 ||
        uv_data->vertical_subsampling != 2 ||
        !crop_covers_image(&crops->crops[0], width, height, stride, y_data))
        goto ambiguous;

    normalized->image_transport_index =
        (uint32_t)qti_data_fd_transport_index;
    normalized->plane_count = 2;
    normalized->plane_strides[0] = (uint32_t)y_data->stride_bytes;
    normalized->plane_strides[1] = (uint32_t)uv_data->stride_bytes;
    if (modifier == ADVC_DRM_FORMAT_MOD_LINEAR) {
        if (layouts->count != 2 || y_metadata != NULL || uv_metadata != NULL ||
            y_data->offset_bytes + y_data->total_size_bytes >
                uv_data->offset_bytes)
            goto ambiguous;
        normalized->kind = ADVC_MAPPER_QTI_NV12_LINEAR;
        normalized->plane_offsets[0] = y_data->offset_bytes;
        normalized->plane_offsets[1] = uv_data->offset_bytes;
        return 0;
    }
    if (layouts->count != 4 || y_metadata == NULL || uv_metadata == NULL ||
        y_metadata->offset_bytes + y_metadata->total_size_bytes !=
            y_data->offset_bytes ||
        y_data->offset_bytes + y_data->total_size_bytes !=
            uv_metadata->offset_bytes ||
        uv_metadata->offset_bytes + uv_metadata->total_size_bytes !=
            uv_data->offset_bytes ||
        y_metadata->stride_bytes >= y_data->stride_bytes ||
        uv_metadata->stride_bytes >= uv_data->stride_bytes)
        goto ambiguous;
    normalized->kind = ADVC_MAPPER_QTI_NV12_UBWC;
    normalized->plane_offsets[0] = y_metadata->offset_bytes;
    normalized->plane_offsets[1] = uv_metadata->offset_bytes;
    return 0;

ambiguous:
    errno = EPROTO;
    return -1;
}
