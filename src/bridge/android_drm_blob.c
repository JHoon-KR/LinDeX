#include "android_drm_blob.h"

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef DRM_FORMAT_MOD_QCOM_COMPRESSED
#define DRM_FORMAT_MOD_QCOM_COMPRESSED fourcc_mod_code(QCOM, 1)
#endif

static bool checked_region(size_t offset, size_t count, size_t element_size,
                           size_t length, size_t *end_out)
{
    size_t bytes;

    if (element_size != 0 && count > SIZE_MAX / element_size) {
        return false;
    }
    bytes = count * element_size;
    if (offset > length || bytes > length - offset) {
        return false;
    }
    *end_out = offset + bytes;
    return true;
}

static bool modifier_covers_format(const struct drm_format_modifier *entry,
                                   uint32_t format_index)
{
    uint64_t relative;

    if (format_index < entry->offset) {
        return false;
    }
    relative = (uint64_t)format_index - entry->offset;
    return relative < 64 && (entry->formats & (UINT64_C(1) << relative)) != 0;
}

int android_drm_blob_append_xb24_qcom(const void *source_data,
                                      size_t source_length,
                                      void **output_data,
                                      size_t *output_length)
{
    const struct drm_format_modifier_blob *header;
    const uint32_t *formats;
    const struct drm_format_modifier *modifiers;
    struct drm_format_modifier_blob *new_header;
    struct drm_format_modifier *new_entry;
    uint32_t target_index = 0;
    uint32_t target_count = 0;
    bool has_linear = false;
    bool has_qcom = false;
    size_t formats_end;
    size_t modifiers_end;
    size_t new_length;
    void *new_data;

    if (output_data == NULL || output_length == NULL) {
        return 0;
    }
    *output_data = NULL;
    *output_length = 0;

    if (source_data == NULL || source_length < sizeof(*header)) {
        return 0;
    }
    header = source_data;
    if (header->version != 1 || header->flags != 0 ||
        header->count_formats == 0 || header->count_modifiers == UINT32_MAX ||
        (header->formats_offset % _Alignof(uint32_t)) != 0 ||
        (header->modifiers_offset % _Alignof(struct drm_format_modifier)) != 0 ||
        !checked_region(header->formats_offset, header->count_formats,
                        sizeof(*formats), source_length, &formats_end) ||
        !checked_region(header->modifiers_offset, header->count_modifiers,
                        sizeof(*modifiers), source_length, &modifiers_end) ||
        header->formats_offset < sizeof(*header) ||
        header->modifiers_offset < formats_end || modifiers_end != source_length) {
        return 0;
    }

    formats = (const uint32_t *)((const unsigned char *)source_data +
                                 header->formats_offset);
    modifiers = (const struct drm_format_modifier *)(
        (const unsigned char *)source_data + header->modifiers_offset);

    for (uint32_t i = 0; i < header->count_formats; ++i) {
        if (formats[i] == DRM_FORMAT_XBGR8888) {
            target_index = i;
            ++target_count;
        }
    }
    if (target_count != 1) {
        return 0;
    }

    for (uint32_t i = 0; i < header->count_modifiers; ++i) {
        uint64_t valid_bits = modifiers[i].formats;

        if (modifiers[i].offset >= header->count_formats) {
            return 0;
        }
        if (header->count_formats - modifiers[i].offset < 64) {
            uint32_t valid_count = header->count_formats - modifiers[i].offset;
            uint64_t mask = valid_count == 64 ? UINT64_MAX :
                ((UINT64_C(1) << valid_count) - 1);
            if ((valid_bits & ~mask) != 0) {
                return 0;
            }
        }
        if (!modifier_covers_format(&modifiers[i], target_index)) {
            continue;
        }
        if (modifiers[i].modifier == DRM_FORMAT_MOD_LINEAR) {
            has_linear = true;
        } else if (modifiers[i].modifier == DRM_FORMAT_MOD_QCOM_COMPRESSED) {
            has_qcom = true;
        }
    }
    if (!has_linear || has_qcom ||
        source_length > SIZE_MAX - sizeof(struct drm_format_modifier)) {
        return 0;
    }

    new_length = source_length + sizeof(struct drm_format_modifier);
    new_data = malloc(new_length);
    if (new_data == NULL) {
        return 0;
    }
    memcpy(new_data, source_data, source_length);
    new_header = new_data;
    new_header->count_modifiers++;
    new_entry = (struct drm_format_modifier *)((unsigned char *)new_data +
                                               source_length);
    memset(new_entry, 0, sizeof(*new_entry));
    new_entry->offset = target_index & ~UINT32_C(63);
    new_entry->formats = UINT64_C(1) << (target_index - new_entry->offset);
    new_entry->modifier = DRM_FORMAT_MOD_QCOM_COMPRESSED;

    *output_data = new_data;
    *output_length = new_length;
    return 1;
}
