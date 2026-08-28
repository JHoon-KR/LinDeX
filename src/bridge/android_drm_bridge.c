/* SPDX-License-Identifier: MIT */
#include "android_drm_bridge.h"

#if defined(__has_include)
# if __has_include(<drm/drm.h>)
#  include <drm/drm.h>
#  include <drm/drm_fourcc.h>
#  include <drm/drm_mode.h>
# elif __has_include(<libdrm/drm.h>)
#  include <libdrm/drm.h>
#  include <libdrm/drm_fourcc.h>
#  include <libdrm/drm_mode.h>
# else
#  include <drm.h>
#  include <drm_fourcc.h>
#  include <drm_mode.h>
# endif
#else
# include <drm/drm.h>
# include <drm/drm_fourcc.h>
# include <drm/drm_mode.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define ADBR_MAX_OBJECT_PROPERTIES 128u
#define ADBR_MAX_PROPERTY_BLOB (1024u * 1024u)
#define ADBR_MAX_QCOM_BLOB (64u * 1024u)

struct adbr_fb {
    int kms_fd;
    uint32_t fb_id;
    uint32_t handles[ADBR_MAX_PLANES];
    uint32_t handle_count;
};

struct adbr_object_properties {
    uint32_t count;
    uint32_t ids[ADBR_MAX_OBJECT_PROPERTIES];
    uint64_t values[ADBR_MAX_OBJECT_PROPERTIES];
};

static int neg_errno(void)
{
    return errno > 0 ? -errno : -EIO;
}

static int ioctl_retry(int fd, unsigned long request, void *argument)
{
    int result;
    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}

static int fd_is_open(int fd)
{
    if (fd < 0)
        return -EBADF;
    if (fcntl(fd, F_GETFD) < 0)
        return neg_errno();
    return 0;
}

static int dup_cloexec(int fd)
{
#ifdef F_DUPFD_CLOEXEC
    int duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate >= 0 || errno != EINVAL)
        return duplicate;
#endif
    {
        int duplicate = dup(fd);
        if (duplicate < 0)
            return -1;
        if (fcntl(duplicate, F_SETFD, FD_CLOEXEC) < 0) {
            int saved_errno = errno;
            close(duplicate);
            errno = saved_errno;
            return -1;
        }
        return duplicate;
    }
}

static int validate_output_header(uint32_t struct_size, uint32_t abi_version,
                                  size_t required_size)
{
    if (abi_version != ADBR_ABI_VERSION_1)
        return -EPROTONOSUPPORT;
    if ((size_t)struct_size < required_size)
        return -EINVAL;
    return 0;
}

static int has_duplicates(const uint32_t *values, size_t count)
{
    size_t i, j;
    for (i = 0; i < count; ++i) {
        if (values[i] == 0)
            return 1;
        for (j = i + 1; j < count; ++j) {
            if (values[i] == values[j])
                return 1;
        }
    }
    return 0;
}

static int equal_object_sets(const uint32_t *left, size_t left_count,
                             const uint32_t *right, size_t right_count)
{
    size_t i, j;
    if (left_count != right_count || has_duplicates(left, left_count) ||
        has_duplicates(right, right_count))
        return 0;
    for (i = 0; i < left_count; ++i) {
        int found = 0;
        for (j = 0; j < right_count; ++j) {
            if (left[i] == right[j]) {
                found = 1;
                break;
            }
        }
        if (!found)
            return 0;
    }
    return 1;
}

static int get_lease_objects_alloc(int fd, uint32_t **out_objects,
                                   size_t *out_count)
{
    unsigned int attempt;
    int status = fd_is_open(fd);
    if (status < 0)
        return status;
    if (!out_objects || !out_count)
        return -EINVAL;
    *out_objects = NULL;
    *out_count = 0;

    for (attempt = 0; attempt < 3; ++attempt) {
        struct drm_mode_get_lease query;
        uint32_t *objects;
        uint32_t capacity;
        memset(&query, 0, sizeof(query));
        if (ioctl_retry(fd, DRM_IOCTL_MODE_GET_LEASE, &query) < 0)
            return neg_errno();
        if (query.count_objects == 0)
            return -ENODATA;
        if (query.count_objects > ADBR_MAX_LEASE_OBJECTS)
            return -E2BIG;
        capacity = query.count_objects;
        objects = calloc(capacity, sizeof(*objects));
        if (!objects)
            return -ENOMEM;
        memset(&query, 0, sizeof(query));
        query.count_objects = capacity;
        query.objects_ptr = (uint64_t)(uintptr_t)objects;
        if (ioctl_retry(fd, DRM_IOCTL_MODE_GET_LEASE, &query) < 0) {
            status = neg_errno();
            free(objects);
            return status;
        }
        if (query.count_objects <= capacity) {
            if (query.count_objects == 0 ||
                has_duplicates(objects, query.count_objects)) {
                free(objects);
                return -EBADMSG;
            }
            *out_objects = objects;
            *out_count = query.count_objects;
            return 0;
        }
        free(objects);
    }
    return -EAGAIN;
}

static int kms_fd_validate(int fd)
{
    struct drm_version version;
    struct drm_mode_card_res resources;
    struct stat status;
    int result = fd_is_open(fd);
    if (result < 0)
        return result;
    if (fstat(fd, &status) < 0)
        return neg_errno();
    if (!S_ISCHR(status.st_mode))
        return -ENOTTY;
    memset(&version, 0, sizeof(version));
    if (ioctl_retry(fd, DRM_IOCTL_VERSION, &version) < 0)
        return neg_errno();
    memset(&resources, 0, sizeof(resources));
    if (ioctl_retry(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) < 0)
        return neg_errno();
    return 0;
}

uint32_t adbr_get_abi_version(void)
{
    return ADBR_ABI_VERSION_1;
}

uint64_t adbr_get_capabilities(void)
{
    return ADBR_CAP_LEASE_VALIDATE | ADBR_CAP_PLANE_FORMATS |
           ADBR_CAP_QCOM_CANDIDATES | ADBR_CAP_PRIME_FB_IMPORT |
           ADBR_CAP_GETFB2_OBSERVE | ADBR_CAP_PRELOAD_VALIDATE;
}

int adbr_lease_get_objects_v1(int borrowed_fd, uint32_t *objects,
                              size_t *inout_count)
{
    uint32_t *actual = NULL;
    size_t count = 0;
    size_t capacity;
    int status;
    if (!inout_count)
        return -EINVAL;
    capacity = *inout_count;
    status = get_lease_objects_alloc(borrowed_fd, &actual, &count);
    if (status < 0)
        return status;
    *inout_count = count;
    if (!objects) {
        free(actual);
        return 0;
    }
    if (capacity < count) {
        free(actual);
        return -ENOSPC;
    }
    memcpy(objects, actual, count * sizeof(*objects));
    free(actual);
    return 0;
}

int adbr_lease_dup_validate_v1(int borrowed_fd,
                               const uint32_t *expected_objects,
                               size_t expected_count, uint32_t flags,
                               int *out_owned_fd,
                               struct adbr_lease_info_v1 *out_info)
{
    uint32_t *actual = NULL;
    size_t actual_count = 0;
    int duplicate;
    int status;
    if (!out_owned_fd || !out_info || flags != 0)
        return -EINVAL;
    *out_owned_fd = -1;
    status = validate_output_header(out_info->struct_size,
                                    out_info->abi_version,
                                    sizeof(*out_info));
    if (status < 0)
        return status;
    if (expected_count > ADBR_MAX_LEASE_OBJECTS ||
        (expected_count != 0 && !expected_objects))
        return -EINVAL;
    if (expected_count != 0 && has_duplicates(expected_objects, expected_count))
        return -EINVAL;
    status = kms_fd_validate(borrowed_fd);
    if (status < 0)
        return status;
    status = get_lease_objects_alloc(borrowed_fd, &actual, &actual_count);
    if (status < 0)
        return status;
    if (expected_count != 0 &&
        !equal_object_sets(actual, actual_count,
                           expected_objects, expected_count)) {
        free(actual);
        return -EPERM;
    }
    duplicate = dup_cloexec(borrowed_fd);
    if (duplicate < 0) {
        status = neg_errno();
        free(actual);
        return status;
    }
    out_info->object_count = (uint32_t)actual_count;
    out_info->reserved = 0;
    *out_owned_fd = duplicate;
    free(actual);
    return 0;
}

static int get_object_properties(int fd, uint32_t object_id,
                                 uint32_t object_type,
                                 struct adbr_object_properties *out)
{
    struct drm_mode_obj_get_properties query;
    uint32_t count;
    if (!out || object_id == 0)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    memset(&query, 0, sizeof(query));
    query.obj_id = object_id;
    query.obj_type = object_type;
    if (ioctl_retry(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &query) < 0)
        return neg_errno();
    if (query.count_props > ADBR_MAX_OBJECT_PROPERTIES)
        return -E2BIG;
    count = query.count_props;
    if (count == 0)
        return -ENODATA;
    memset(&query, 0, sizeof(query));
    query.obj_id = object_id;
    query.obj_type = object_type;
    query.count_props = count;
    query.props_ptr = (uint64_t)(uintptr_t)out->ids;
    query.prop_values_ptr = (uint64_t)(uintptr_t)out->values;
    if (ioctl_retry(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &query) < 0)
        return neg_errno();
    if (query.count_props > count)
        return -EAGAIN;
    out->count = query.count_props;
    return 0;
}

static int get_property_metadata(int fd, uint32_t property_id,
                                 char name[DRM_PROP_NAME_LEN],
                                 uint32_t *out_flags)
{
    struct drm_mode_get_property property;
    memset(&property, 0, sizeof(property));
    property.prop_id = property_id;
    if (ioctl_retry(fd, DRM_IOCTL_MODE_GETPROPERTY, &property) < 0)
        return neg_errno();
    memcpy(name, property.name, DRM_PROP_NAME_LEN);
    name[DRM_PROP_NAME_LEN - 1] = '\0';
    if (out_flags)
        *out_flags = property.flags;
    return 0;
}

static int get_blob_alloc(int fd, uint32_t blob_id, size_t maximum,
                          uint8_t **out_data, size_t *out_length)
{
    struct drm_mode_get_blob blob;
    uint8_t *data;
    uint32_t length;
    if (!out_data || !out_length || blob_id == 0)
        return -EINVAL;
    *out_data = NULL;
    *out_length = 0;
    memset(&blob, 0, sizeof(blob));
    blob.blob_id = blob_id;
    if (ioctl_retry(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) < 0)
        return neg_errno();
    if (blob.length == 0)
        return -ENODATA;
    if ((size_t)blob.length > maximum)
        return -E2BIG;
    length = blob.length;
    data = malloc(length);
    if (!data)
        return -ENOMEM;
    memset(&blob, 0, sizeof(blob));
    blob.blob_id = blob_id;
    blob.length = length;
    blob.data = (uint64_t)(uintptr_t)data;
    if (ioctl_retry(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) < 0) {
        int status = neg_errno();
        free(data);
        return status;
    }
    if (blob.length > length) {
        free(data);
        return -EAGAIN;
    }
    *out_data = data;
    *out_length = blob.length;
    return 0;
}

static int add_format_entry(struct adbr_format_modifier_v1 *entries,
                            size_t *count, uint32_t fourcc, uint64_t modifier,
                            uint32_t source)
{
    size_t i;
    for (i = 0; i < *count; ++i) {
        if (entries[i].fourcc == fourcc && entries[i].modifier == modifier) {
            entries[i].source_flags |= source;
            return 0;
        }
    }
    if (*count >= ADBR_MAX_FORMAT_MODIFIERS)
        return -E2BIG;
    entries[*count].fourcc = fourcc;
    entries[*count].modifier = modifier;
    entries[*count].source_flags = source;
    ++*count;
    return 0;
}

static int parse_in_formats_blob(const uint8_t *data, size_t length,
                                 struct adbr_format_modifier_v1 *entries,
                                 size_t *count)
{
    struct drm_format_modifier_blob header;
    uint64_t formats_bytes, modifiers_bytes;
    uint32_t modifier_index;
    if (!data || !entries || !count || length < sizeof(header))
        return -EBADMSG;
    memcpy(&header, data, sizeof(header));
    if (header.version != FORMAT_BLOB_CURRENT || header.flags != 0)
        return -EPROTONOSUPPORT;
    formats_bytes = (uint64_t)header.count_formats * sizeof(uint32_t);
    modifiers_bytes = (uint64_t)header.count_modifiers *
                      sizeof(struct drm_format_modifier);
    if (header.formats_offset > length || formats_bytes > length - header.formats_offset ||
        header.modifiers_offset > length || modifiers_bytes > length - header.modifiers_offset)
        return -EBADMSG;
    for (modifier_index = 0; modifier_index < header.count_formats;
         ++modifier_index) {
        uint32_t fourcc;
        int status;
        memcpy(&fourcc, data + header.formats_offset +
                        (size_t)modifier_index * sizeof(fourcc), sizeof(fourcc));
        if (fourcc == 0)
            return -EBADMSG;
        status = add_format_entry(entries, count, fourcc,
                                  ADBR_MODIFIER_INVALID,
                                  ADBR_FORMAT_SOURCE_IN_FORMATS);
        if (status < 0)
            return status;
    }
    for (modifier_index = 0; modifier_index < header.count_modifiers;
         ++modifier_index) {
        struct drm_format_modifier modifier;
        unsigned int bit;
        size_t offset = header.modifiers_offset +
                        (size_t)modifier_index * sizeof(modifier);
        memcpy(&modifier, data + offset, sizeof(modifier));
        if (modifier.pad != 0)
            return -EBADMSG;
        for (bit = 0; bit < 64; ++bit) {
            uint64_t format_index;
            uint32_t fourcc;
            int status;
            if ((modifier.formats & (UINT64_C(1) << bit)) == 0)
                continue;
            format_index = (uint64_t)modifier.offset + bit;
            if (format_index >= header.count_formats)
                return -EBADMSG;
            memcpy(&fourcc, data + header.formats_offset +
                            (size_t)format_index * sizeof(fourcc), sizeof(fourcc));
            if (fourcc == 0)
                return -EBADMSG;
            status = add_format_entry(entries, count, fourcc,
                                      modifier.modifier,
                                      ADBR_FORMAT_SOURCE_IN_FORMATS);
            if (status < 0)
                return status;
        }
    }
    return *count == 0 ? -ENODATA : 0;
}

static int qcom_separator(uint8_t value)
{
    return value == 0 || value == ' ' || value == '\t' || value == '\r' ||
           value == '\n' || value == '=';
}

static int fourcc_token_valid(const uint8_t token[4])
{
    unsigned int i;
    for (i = 0; i < 4; ++i) {
        if (token[i] < 0x20 || token[i] > 0x7e || token[i] == '/')
            return 0;
    }
    return 1;
}

static int has_standard_linear(const struct adbr_format_modifier_v1 *entries,
                               size_t count, uint32_t fourcc)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        if (entries[i].fourcc == fourcc &&
            entries[i].modifier == ADBR_MODIFIER_LINEAR &&
            (entries[i].source_flags & ADBR_FORMAT_SOURCE_IN_FORMATS) != 0)
            return 1;
    }
    return 0;
}

static int parse_qcom_candidates(const uint8_t *data, size_t length,
                                 struct adbr_format_modifier_v1 *entries,
                                 size_t *count)
{
    size_t offset;
    size_t standard_count = *count;
    for (offset = 0; offset + 8 <= length; ++offset) {
        uint32_t fourcc;
        int status;
        if ((offset != 0 && !qcom_separator(data[offset - 1])) ||
            data[offset + 4] != '/' || data[offset + 5] != '5' ||
            data[offset + 6] != '/' || data[offset + 7] != '1' ||
            (offset + 8 != length && !qcom_separator(data[offset + 8])) ||
            !fourcc_token_valid(data + offset))
            continue;
        fourcc = (uint32_t)data[offset] |
                 ((uint32_t)data[offset + 1] << 8) |
                 ((uint32_t)data[offset + 2] << 16) |
                 ((uint32_t)data[offset + 3] << 24);
        if (!has_standard_linear(entries, standard_count, fourcc))
            continue;
        status = add_format_entry(entries, count, fourcc,
                                  ADBR_MODIFIER_QCOM_COMPRESSED,
                                  ADBR_FORMAT_SOURCE_QCOM_CANDIDATE);
        if (status < 0)
            return status;
    }
    return 0;
}

int adbr_plane_query_formats_v1(int borrowed_fd, uint32_t plane_id,
                                uint32_t query_flags,
                                struct adbr_format_modifier_v1 *output,
                                size_t *inout_count)
{
    struct adbr_object_properties properties;
    struct adbr_format_modifier_v1 entries[ADBR_MAX_FORMAT_MODIFIERS];
    uint32_t in_formats_blob = 0;
    uint32_t qcom_blob = 0;
    int saw_in_formats = 0;
    int saw_qcom = 0;
    size_t count = 0;
    size_t capacity;
    uint32_t i;
    int status;
    if (!inout_count || plane_id == 0 ||
        (query_flags & ~ADBR_PLANE_QUERY_QCOM_CANDIDATES) != 0)
        return -EINVAL;
    capacity = *inout_count;
    status = fd_is_open(borrowed_fd);
    if (status < 0)
        return status;
    status = get_object_properties(borrowed_fd, plane_id,
                                   DRM_MODE_OBJECT_PLANE, &properties);
    if (status < 0)
        return status;
    for (i = 0; i < properties.count; ++i) {
        char name[DRM_PROP_NAME_LEN];
        uint32_t property_flags = 0;
        status = get_property_metadata(borrowed_fd, properties.ids[i], name,
                                       &property_flags);
        if (status < 0)
            return status;
        if (strcmp(name, "IN_FORMATS") == 0) {
            if (saw_in_formats ||
                (property_flags & DRM_MODE_PROP_BLOB) == 0 ||
                properties.values[i] == 0 ||
                properties.values[i] > UINT32_MAX)
                return -EBADMSG;
            saw_in_formats = 1;
            in_formats_blob = (uint32_t)properties.values[i];
        } else if (strcmp(name, "capabilities") == 0) {
            if (saw_qcom || (property_flags & DRM_MODE_PROP_BLOB) == 0 ||
                properties.values[i] == 0 ||
                properties.values[i] > UINT32_MAX)
                return -EBADMSG;
            saw_qcom = 1;
            qcom_blob = (uint32_t)properties.values[i];
        }
    }
    if (in_formats_blob == 0)
        return -ENODATA;
    {
        uint8_t *blob = NULL;
        size_t length = 0;
        status = get_blob_alloc(borrowed_fd, in_formats_blob,
                                ADBR_MAX_PROPERTY_BLOB, &blob, &length);
        if (status < 0)
            return status;
        status = parse_in_formats_blob(blob, length, entries, &count);
        free(blob);
        if (status < 0)
            return status;
    }
    if ((query_flags & ADBR_PLANE_QUERY_QCOM_CANDIDATES) != 0 &&
        qcom_blob != 0) {
        uint8_t *blob = NULL;
        size_t length = 0;
        status = get_blob_alloc(borrowed_fd, qcom_blob, ADBR_MAX_QCOM_BLOB,
                                &blob, &length);
        if (status < 0)
            return status;
        status = parse_qcom_candidates(blob, length, entries, &count);
        free(blob);
        if (status < 0)
            return status;
    }
    *inout_count = count;
    if (!output)
        return 0;
    if (capacity < count)
        return -ENOSPC;
    memcpy(output, entries, count * sizeof(*output));
    return 0;
}

static int object_set_contains(const uint32_t *objects, size_t count,
                               uint32_t object_id)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        if (objects[i] == object_id)
            return 1;
    }
    return 0;
}

int adbr_preload_validate_plane_blob_v1(int borrowed_fd, uint32_t plane_id,
                                        uint32_t in_formats_blob_id,
                                        uint32_t fourcc, uint64_t modifier)
{
    struct adbr_object_properties properties;
    struct adbr_format_modifier_v1 entries[ADBR_MAX_FORMAT_MODIFIERS];
    uint32_t *lease_objects = NULL;
    size_t lease_count = 0;
    size_t entry_count = ADBR_MAX_FORMAT_MODIFIERS;
    uint32_t i;
    int saw_type = 0;
    int saw_in_formats = 0;
    int saw_capabilities = 0;
    int found_linear = 0;
    int found_candidate = 0;
    int status;

    if (borrowed_fd < 0 || plane_id == 0 || in_formats_blob_id == 0 ||
        fourcc != DRM_FORMAT_XBGR8888 ||
        modifier != ADBR_MODIFIER_QCOM_COMPRESSED)
        return -EINVAL;
    status = kms_fd_validate(borrowed_fd);
    if (status < 0)
        return status;
    status = get_lease_objects_alloc(borrowed_fd, &lease_objects,
                                     &lease_count);
    if (status < 0)
        return status;
    if (!object_set_contains(lease_objects, lease_count, plane_id)) {
        free(lease_objects);
        return -EPERM;
    }
    free(lease_objects);

    status = get_object_properties(borrowed_fd, plane_id,
                                   DRM_MODE_OBJECT_PLANE, &properties);
    if (status < 0)
        return status;
    for (i = 0; i < properties.count; ++i) {
        char name[DRM_PROP_NAME_LEN];
        uint32_t property_flags = 0;
        status = get_property_metadata(borrowed_fd, properties.ids[i], name,
                                       &property_flags);
        if (status < 0)
            return status;
        if (strcmp(name, "type") == 0) {
            if (saw_type || (property_flags & DRM_MODE_PROP_ENUM) == 0 ||
                properties.values[i] != 1)
                return -EBADMSG;
            saw_type = 1;
        } else if (strcmp(name, "IN_FORMATS") == 0) {
            if (saw_in_formats ||
                (property_flags & DRM_MODE_PROP_BLOB) == 0 ||
                properties.values[i] != in_formats_blob_id)
                return -EBADMSG;
            saw_in_formats = 1;
        } else if (strcmp(name, "capabilities") == 0) {
            if (saw_capabilities ||
                (property_flags & DRM_MODE_PROP_BLOB) == 0 ||
                properties.values[i] == 0 ||
                properties.values[i] > UINT32_MAX)
                return -EBADMSG;
            saw_capabilities = 1;
        }
    }
    if (!saw_type || !saw_in_formats || !saw_capabilities)
        return -ENODATA;
    status = adbr_plane_query_formats_v1(
        borrowed_fd, plane_id,
        ADBR_PLANE_QUERY_QCOM_CANDIDATES, entries, &entry_count);
    if (status < 0)
        return status;
    for (i = 0; i < entry_count; ++i) {
        if (entries[i].fourcc != fourcc)
            continue;
        if (entries[i].modifier == ADBR_MODIFIER_LINEAR &&
            (entries[i].source_flags & ADBR_FORMAT_SOURCE_IN_FORMATS) != 0)
            found_linear = 1;
        if (entries[i].modifier == modifier &&
            (entries[i].source_flags &
             ADBR_FORMAT_SOURCE_QCOM_CANDIDATE) != 0)
            found_candidate = 1;
    }
    return found_linear && found_candidate ? 0 : -ENOTSUP;
}

int adbr_dmabuf_validate_v1(const struct adbr_dmabuf_v1 *dmabuf)
{
    uint32_t i;
    uint64_t modifier;
    if (!dmabuf)
        return -EINVAL;
    if (validate_output_header(dmabuf->struct_size, dmabuf->abi_version,
                               sizeof(*dmabuf)) < 0)
        return dmabuf->abi_version == ADBR_ABI_VERSION_1 ?
               -EINVAL : -EPROTONOSUPPORT;
    if (dmabuf->width == 0 || dmabuf->height == 0 || dmabuf->fourcc == 0 ||
        dmabuf->plane_count == 0 || dmabuf->plane_count > ADBR_MAX_PLANES ||
        dmabuf->flags != 0 || dmabuf->reserved != 0)
        return -EINVAL;
    modifier = dmabuf->planes[0].modifier;
    if (modifier == ADBR_MODIFIER_INVALID)
        return -EINVAL;
    for (i = 0; i < ADBR_MAX_PLANES; ++i) {
        const struct adbr_dmabuf_plane_v1 *plane = &dmabuf->planes[i];
        if (i < dmabuf->plane_count) {
            int status;
            if (plane->stride == 0 || plane->reserved != 0 ||
                plane->modifier != modifier)
                return -EINVAL;
            status = fd_is_open(plane->fd);
            if (status < 0)
                return status;
        } else if (plane->fd != 0 || plane->stride != 0 || plane->offset != 0 ||
                   plane->reserved != 0 || plane->modifier != 0) {
            return -EINVAL;
        }
    }
    return 0;
}

static void close_gem_handles(int drm_fd, uint32_t *handles, uint32_t count)
{
    uint32_t i, j;
    for (i = 0; i < count; ++i) {
        struct drm_gem_close close_arg;
        if (handles[i] == 0)
            continue;
        for (j = 0; j < i; ++j) {
            if (handles[i] == handles[j])
                break;
        }
        if (j != i)
            continue;
        memset(&close_arg, 0, sizeof(close_arg));
        close_arg.handle = handles[i];
        (void)ioctl_retry(drm_fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
    }
}

int adbr_fb_import_v1(int borrowed_kms_fd,
                      const struct adbr_dmabuf_v1 *dmabuf, uint32_t flags,
                      struct adbr_fb **out_fb)
{
    struct drm_mode_fb_cmd2 command;
    struct adbr_fb *fb;
    int kms_fd;
    uint32_t i;
    int status;
    if (!out_fb || flags != 0)
        return -EINVAL;
    *out_fb = NULL;
    status = adbr_dmabuf_validate_v1(dmabuf);
    if (status < 0)
        return status;
    status = kms_fd_validate(borrowed_kms_fd);
    if (status < 0)
        return status;
    kms_fd = dup_cloexec(borrowed_kms_fd);
    if (kms_fd < 0)
        return neg_errno();
    memset(&command, 0, sizeof(command));
    command.width = dmabuf->width;
    command.height = dmabuf->height;
    command.pixel_format = dmabuf->fourcc;
    command.flags = DRM_MODE_FB_MODIFIERS;
    for (i = 0; i < dmabuf->plane_count; ++i) {
        struct drm_prime_handle prime;
        memset(&prime, 0, sizeof(prime));
        prime.fd = dmabuf->planes[i].fd;
        if (ioctl_retry(kms_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) < 0) {
            status = neg_errno();
            close_gem_handles(kms_fd, command.handles, i);
            close(kms_fd);
            return status;
        }
        if (prime.handle == 0) {
            close_gem_handles(kms_fd, command.handles, i);
            close(kms_fd);
            return -EBADMSG;
        }
        command.handles[i] = prime.handle;
        command.pitches[i] = dmabuf->planes[i].stride;
        command.offsets[i] = dmabuf->planes[i].offset;
        command.modifier[i] = dmabuf->planes[i].modifier;
    }
    fb = calloc(1, sizeof(*fb));
    if (!fb) {
        close_gem_handles(kms_fd, command.handles, dmabuf->plane_count);
        close(kms_fd);
        return -ENOMEM;
    }
    if (ioctl_retry(kms_fd, DRM_IOCTL_MODE_ADDFB2, &command) < 0) {
        status = neg_errno();
        free(fb);
        close_gem_handles(kms_fd, command.handles, dmabuf->plane_count);
        close(kms_fd);
        return status;
    }
    if (command.fb_id == 0) {
        free(fb);
        close_gem_handles(kms_fd, command.handles, dmabuf->plane_count);
        close(kms_fd);
        return -EBADMSG;
    }
    fb->kms_fd = kms_fd;
    fb->fb_id = command.fb_id;
    fb->handle_count = dmabuf->plane_count;
    memcpy(fb->handles, command.handles, sizeof(fb->handles));
    *out_fb = fb;
    return 0;
}

uint32_t adbr_fb_get_id_v1(const struct adbr_fb *fb)
{
    return fb ? fb->fb_id : 0;
}

int adbr_fb_destroy_v1(struct adbr_fb *fb)
{
    if (!fb)
        return -EINVAL;
    if (fb->fb_id != 0 &&
        ioctl_retry(fb->kms_fd, DRM_IOCTL_MODE_RMFB, &fb->fb_id) < 0)
        return neg_errno();
    close_gem_handles(fb->kms_fd, fb->handles, fb->handle_count);
    close(fb->kms_fd);
    memset(fb, 0, sizeof(*fb));
    free(fb);
    return 0;
}

int adbr_fb_observe_v1(int borrowed_kms_fd, uint32_t fb_id,
                       struct adbr_fb_observation_v1 *out)
{
    struct drm_mode_fb_cmd2 command;
    uint32_t plane_count = 0;
    uint32_t i;
    int saw_unused_plane = 0;
    int status;
    if (!out || fb_id == 0)
        return -EINVAL;
    status = validate_output_header(out->struct_size, out->abi_version,
                                    sizeof(*out));
    if (status < 0)
        return status;
    status = kms_fd_validate(borrowed_kms_fd);
    if (status < 0)
        return status;
    memset(&command, 0, sizeof(command));
    command.fb_id = fb_id;
    if (ioctl_retry(borrowed_kms_fd, DRM_IOCTL_MODE_GETFB2, &command) < 0)
        return neg_errno();
    for (i = 0; i < ADBR_MAX_PLANES; ++i) {
        if (command.handles[i] != 0) {
            if (saw_unused_plane) {
                close_gem_handles(borrowed_kms_fd, command.handles,
                                  ADBR_MAX_PLANES);
                return -EBADMSG;
            }
            plane_count = i + 1;
        } else {
            if (command.pitches[i] != 0 || command.offsets[i] != 0 ||
                command.modifier[i] != 0) {
                close_gem_handles(borrowed_kms_fd, command.handles,
                                  ADBR_MAX_PLANES);
                return -EBADMSG;
            }
            saw_unused_plane = 1;
        }
    }
    if (plane_count == 0 || command.fb_id != fb_id || command.width == 0 ||
        command.height == 0 || command.pixel_format == 0) {
        close_gem_handles(borrowed_kms_fd, command.handles, ADBR_MAX_PLANES);
        return -EBADMSG;
    }
    if ((command.flags & DRM_MODE_FB_MODIFIERS) != 0) {
        for (i = 1; i < plane_count; ++i) {
            if (command.modifier[i] != command.modifier[0]) {
                close_gem_handles(borrowed_kms_fd, command.handles,
                                  ADBR_MAX_PLANES);
                return -EBADMSG;
            }
        }
    }
    out->fb_id = command.fb_id;
    out->width = command.width;
    out->height = command.height;
    out->fourcc = command.pixel_format;
    out->plane_count = plane_count;
    out->flags = command.flags;
    for (i = 0; i < ADBR_MAX_PLANES; ++i) {
        out->strides[i] = command.pitches[i];
        out->offsets[i] = command.offsets[i];
        out->modifiers[i] = i < plane_count &&
                            (command.flags & DRM_MODE_FB_MODIFIERS) != 0 ?
                            command.modifier[i] : ADBR_MODIFIER_INVALID;
    }
    close_gem_handles(borrowed_kms_fd, command.handles, ADBR_MAX_PLANES);
    return 0;
}
