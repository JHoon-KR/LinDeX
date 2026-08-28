/* SPDX-License-Identifier: MIT */
#include "android_drm_bridge.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROBE_MAX_ENV 4096u
#define PROBE_FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
     ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define PROBE_FOURCC_XB24 PROBE_FOURCC('X', 'B', '2', '4')

enum probe_operation {
    PROBE_OPERATION_NONE,
    PROBE_OPERATION_LEASE,
    PROBE_OPERATION_PLANE,
    PROBE_OPERATION_PRELOAD_GATE,
};

static const char *operation_name(enum probe_operation operation)
{
    switch (operation) {
    case PROBE_OPERATION_LEASE:
        return "lease";
    case PROBE_OPERATION_PLANE:
        return "plane";
    case PROBE_OPERATION_PRELOAD_GATE:
        return "preload-gate";
    default:
        return "arguments";
    }
}

static int fail_result(enum probe_operation operation, const char *stage,
                       int status)
{
    if (status >= 0)
        status = -EIO;
    printf("ADBR-PROBE schema=1 operation=%s result=FAIL stage=%s status=%d\n",
           operation_name(operation), stage, status);
    return 1;
}

static int parse_decimal_span(const char *text, size_t length,
                              uint64_t maximum, uint64_t *out)
{
    size_t i;
    uint64_t value = 0;
    if (!text || !out || length == 0 || length > 20)
        return -EINVAL;
    for (i = 0; i < length; ++i) {
        unsigned int digit;
        if (text[i] < '0' || text[i] > '9')
            return -EINVAL;
        digit = (unsigned int)(text[i] - '0');
        if (value > (maximum - digit) / 10)
            return -ERANGE;
        value = value * 10 + digit;
    }
    *out = value;
    return 0;
}

static int parse_decimal_argument(const char *text, uint64_t maximum,
                                  uint64_t *out)
{
    size_t length;
    if (!text)
        return -EINVAL;
    length = strnlen(text, 21);
    if (length == 21)
        return -E2BIG;
    return parse_decimal_span(text, length, maximum, out);
}

static int parse_objects(const char *text, uint32_t *objects,
                         size_t *out_count)
{
    size_t length;
    size_t begin = 0;
    size_t count = 0;
    if (!text || !objects || !out_count)
        return -EINVAL;
    length = strnlen(text, PROBE_MAX_ENV + 1);
    if (length == 0)
        return -EINVAL;
    if (length > PROBE_MAX_ENV)
        return -E2BIG;
    while (begin < length) {
        size_t end = begin;
        uint64_t value;
        size_t i;
        int status;
        while (end < length && text[end] != ',')
            ++end;
        if (count >= ADBR_MAX_LEASE_OBJECTS)
            return -E2BIG;
        status = parse_decimal_span(text + begin, end - begin, UINT32_MAX,
                                    &value);
        if (status < 0 || value == 0)
            return status < 0 ? status : -EINVAL;
        for (i = 0; i < count; ++i) {
            if (objects[i] == (uint32_t)value)
                return -EINVAL;
        }
        objects[count++] = (uint32_t)value;
        if (end == length)
            break;
        begin = end + 1;
        if (begin == length)
            return -EINVAL;
    }
    if (count == 0)
        return -EINVAL;
    *out_count = count;
    return 0;
}

static int object_is_expected(const uint32_t *objects, size_t count,
                              uint32_t object_id)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        if (objects[i] == object_id)
            return 1;
    }
    return 0;
}

static int read_exact_environment(int *out_fd, uint32_t *objects,
                                  size_t *out_count)
{
    const char *fd_text = getenv("DRM_LEASE_FD");
    const char *objects_text = getenv("DRM_LEASE_OBJECTS");
    uint64_t fd_value;
    int status;
    if (!fd_text || !objects_text)
        return -ENOENT;
    status = parse_decimal_argument(fd_text, INT_MAX, &fd_value);
    if (status < 0)
        return status;
    status = parse_objects(objects_text, objects, out_count);
    if (status < 0)
        return status;
    *out_fd = (int)fd_value;
    return 0;
}

static int enumerate_lease(enum probe_operation operation, int lease_fd,
                           uint64_t capabilities)
{
    uint32_t objects[ADBR_MAX_LEASE_OBJECTS];
    size_t count = ADBR_MAX_LEASE_OBJECTS;
    size_t i;
    int status = adbr_lease_get_objects_v1(lease_fd, objects, &count);
    if (status < 0)
        return fail_result(operation, "lease-enumeration", status);
    if (count == 0 || count > ADBR_MAX_LEASE_OBJECTS)
        return fail_result(operation, "lease-enumeration", -EBADMSG);
    printf("ADBR-PROBE schema=1 operation=lease result=PASS abi=%" PRIu32
           " capabilities=0x%016" PRIx64 " objects=%zu\n",
           adbr_get_abi_version(), capabilities, count);
    for (i = 0; i < count; ++i) {
        printf("ADBR-OBJECT index=%zu id=%" PRIu32 "\n", i, objects[i]);
    }
    return 0;
}

static const char *format_source_name(uint32_t source_flags)
{
    switch (source_flags) {
    case ADBR_FORMAT_SOURCE_IN_FORMATS:
        return "in-formats";
    case ADBR_FORMAT_SOURCE_QCOM_CANDIDATE:
        return "qcom-candidate";
    case ADBR_FORMAT_SOURCE_IN_FORMATS |
         ADBR_FORMAT_SOURCE_QCOM_CANDIDATE:
        return "in-formats+qcom-candidate";
    default:
        return NULL;
    }
}

static int enumerate_plane(int lease_fd, uint32_t plane_id,
                           uint64_t capabilities)
{
    struct adbr_format_modifier_v1 *entries = NULL;
    size_t count = 0;
    size_t capacity;
    size_t i;
    int status = adbr_plane_query_formats_v1(
        lease_fd, plane_id, ADBR_PLANE_QUERY_QCOM_CANDIDATES, NULL, &count);
    if (status < 0)
        return fail_result(PROBE_OPERATION_PLANE, "plane-query-size", status);
    if (count == 0 || count > ADBR_MAX_FORMAT_MODIFIERS)
        return fail_result(PROBE_OPERATION_PLANE, "plane-query-size",
                           -E2BIG);
    entries = calloc(count, sizeof(*entries));
    if (!entries)
        return fail_result(PROBE_OPERATION_PLANE, "allocation", -ENOMEM);
    capacity = count;
    status = adbr_plane_query_formats_v1(
        lease_fd, plane_id, ADBR_PLANE_QUERY_QCOM_CANDIDATES, entries,
        &capacity);
    if (status < 0 || capacity != count) {
        free(entries);
        return fail_result(PROBE_OPERATION_PLANE, "plane-query",
                           status < 0 ? status : -EAGAIN);
    }
    for (i = 0; i < count; ++i) {
        if (!format_source_name(entries[i].source_flags) ||
            entries[i].fourcc == 0) {
            free(entries);
            return fail_result(PROBE_OPERATION_PLANE, "format-source",
                               -EBADMSG);
        }
    }
    printf("ADBR-PROBE schema=1 operation=plane result=PASS abi=%" PRIu32
           " capabilities=0x%016" PRIx64 " plane=%" PRIu32
           " pairs=%zu\n",
           adbr_get_abi_version(), capabilities, plane_id, count);
    for (i = 0; i < count; ++i) {
        printf("ADBR-FORMAT index=%zu fourcc=0x%08" PRIx32
               " modifier=0x%016" PRIx64 " source=%s\n",
               i, entries[i].fourcc, entries[i].modifier,
               format_source_name(entries[i].source_flags));
    }
    free(entries);
    return 0;
}

int main(int argc, char **argv)
{
    enum probe_operation operation = PROBE_OPERATION_NONE;
    uint32_t expected_objects[ADBR_MAX_LEASE_OBJECTS];
    size_t expected_count = 0;
    uint32_t plane_id = 0;
    uint32_t blob_id = 0;
    uint64_t parsed;
    uint64_t capabilities;
    uint64_t required_capabilities = ADBR_CAP_LEASE_VALIDATE;
    struct adbr_lease_info_v1 lease_info = {
        .struct_size = sizeof(lease_info),
        .abi_version = ADBR_ABI_VERSION_1,
    };
    int borrowed_fd = -1;
    int lease_fd = -1;
    int status;
    int result;

    if (argc == 2 && strcmp(argv[1], "--lease") == 0) {
        operation = PROBE_OPERATION_LEASE;
    } else if (argc == 3 && strcmp(argv[1], "--plane") == 0) {
        operation = PROBE_OPERATION_PLANE;
        status = parse_decimal_argument(argv[2], UINT32_MAX, &parsed);
        if (status < 0 || parsed == 0)
            return fail_result(operation, "arguments",
                               status < 0 ? status : -EINVAL);
        plane_id = (uint32_t)parsed;
        required_capabilities |= ADBR_CAP_PLANE_FORMATS |
                                 ADBR_CAP_QCOM_CANDIDATES;
    } else if (argc == 4 && strcmp(argv[1], "--preload-gate") == 0) {
        operation = PROBE_OPERATION_PRELOAD_GATE;
        status = parse_decimal_argument(argv[2], UINT32_MAX, &parsed);
        if (status < 0 || parsed == 0)
            return fail_result(operation, "arguments",
                               status < 0 ? status : -EINVAL);
        plane_id = (uint32_t)parsed;
        status = parse_decimal_argument(argv[3], UINT32_MAX, &parsed);
        if (status < 0 || parsed == 0)
            return fail_result(operation, "arguments",
                               status < 0 ? status : -EINVAL);
        blob_id = (uint32_t)parsed;
        required_capabilities |= ADBR_CAP_PRELOAD_VALIDATE;
    } else {
        return fail_result(operation, "arguments", -EINVAL);
    }

    if (adbr_get_abi_version() != ADBR_ABI_VERSION_1)
        return fail_result(operation, "abi", -EPROTONOSUPPORT);
    capabilities = adbr_get_capabilities();
    if ((capabilities & required_capabilities) != required_capabilities)
        return fail_result(operation, "capabilities", -ENOTSUP);
    status = read_exact_environment(&borrowed_fd, expected_objects,
                                    &expected_count);
    if (status < 0)
        return fail_result(operation, "environment", status);
    if ((operation == PROBE_OPERATION_PLANE ||
         operation == PROBE_OPERATION_PRELOAD_GATE) &&
        !object_is_expected(expected_objects, expected_count, plane_id))
        return fail_result(operation, "plane-not-in-object-list", -EPERM);
    status = adbr_lease_dup_validate_v1(
        borrowed_fd, expected_objects, expected_count, 0, &lease_fd,
        &lease_info);
    if (status < 0)
        return fail_result(operation, "lease-validation", status);
    if (lease_info.object_count != expected_count) {
        close(lease_fd);
        return fail_result(operation, "lease-object-count", -EBADMSG);
    }

    switch (operation) {
    case PROBE_OPERATION_LEASE:
        result = enumerate_lease(operation, lease_fd, capabilities);
        break;
    case PROBE_OPERATION_PLANE:
        result = enumerate_plane(lease_fd, plane_id, capabilities);
        break;
    case PROBE_OPERATION_PRELOAD_GATE:
        status = adbr_preload_validate_plane_blob_v1(
            lease_fd, plane_id, blob_id, PROBE_FOURCC_XB24,
            ADBR_MODIFIER_QCOM_COMPRESSED);
        if (status < 0) {
            result = fail_result(operation, "preload-gate", status);
        } else {
            printf("ADBR-PROBE schema=1 operation=preload-gate result=PASS "
                   "abi=%" PRIu32 " capabilities=0x%016" PRIx64
                   " plane=%" PRIu32 " in_formats_blob=%" PRIu32
                   " fourcc=0x%08" PRIx32 " modifier=0x%016" PRIx64
                   " authority=vendor-candidate-not-active-proof\n",
                   adbr_get_abi_version(), capabilities, plane_id, blob_id,
                   (uint32_t)PROBE_FOURCC_XB24,
                   (uint64_t)ADBR_MODIFIER_QCOM_COMPRESSED);
            result = 0;
        }
        break;
    default:
        result = fail_result(operation, "internal", -EIO);
        break;
    }
    close(lease_fd);
    return result;
}
