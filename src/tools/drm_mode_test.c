// SPDX-License-Identifier: MIT
// Static AArch64 Android/Linux renderer for an inherited DRM lease FD.
// Default behavior is atomic TEST_ONLY.  --commit is required to scan out.

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define DRM_IOCTL_BASE 'd'
#define DRM_DISPLAY_MODE_LEN 32
#define DRM_PROP_NAME_LEN 32
#define MAX_OBJECTS 4096U

#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2ULL
#define DRM_CLIENT_CAP_ATOMIC 3ULL
#define DRM_MODE_CONNECTED 1U
#define DRM_MODE_TYPE_PREFERRED (1U << 3)
#define DRM_MODE_OBJECT_CRTC 0xccccccccU
#define DRM_MODE_OBJECT_CONNECTOR 0xc0c0c0c0U
#define DRM_MODE_OBJECT_PLANE 0xeeeeeeeeU
#define DRM_MODE_ATOMIC_TEST_ONLY 0x0100U
#define DRM_MODE_ATOMIC_ALLOW_MODESET 0x0400U
#define DRM_FORMAT_XRGB8888 0x34325258U /* XR24 */

struct drm_set_client_cap {
    uint64_t capability, value;
};

struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char name[DRM_DISPLAY_MODE_LEN];
};

struct drm_mode_card_res {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};

struct drm_mode_get_connector {
    uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    uint32_t count_modes, count_props, count_encoders, encoder_id;
    uint32_t connector_id, connector_type, connector_type_id, connection;
    uint32_t mm_width, mm_height, subpixel, pad;
};

struct drm_mode_get_plane_res {
    uint64_t plane_id_ptr;
    uint32_t count_planes;
};

struct drm_mode_get_plane {
    uint32_t plane_id, crtc_id, fb_id, possible_crtcs;
    uint32_t gamma_size, count_format_types;
    uint64_t format_type_ptr;
};

struct drm_mode_obj_get_properties {
    uint64_t props_ptr, prop_values_ptr;
    uint32_t count_props, obj_id, obj_type;
};

struct drm_mode_get_property {
    uint64_t values_ptr, enum_blob_ptr;
    uint32_t prop_id, flags;
    char name[DRM_PROP_NAME_LEN];
    uint32_t count_values, count_enum_blobs;
};

struct drm_mode_create_blob {
    uint64_t data;
    uint32_t length, blob_id;
};

struct drm_mode_destroy_blob {
    uint32_t blob_id;
};

struct drm_mode_create_dumb {
    uint32_t height, width, bpp, flags;
    uint32_t handle, pitch;
    uint64_t size;
};

struct drm_mode_map_dumb {
    uint32_t handle, pad;
    uint64_t offset;
};

struct drm_mode_destroy_dumb {
    uint32_t handle;
};

struct drm_mode_fb_cmd2 {
    uint32_t fb_id, width, height, pixel_format, flags;
    uint32_t handles[4], pitches[4], offsets[4];
    uint64_t modifiers[4];
};

struct drm_mode_atomic {
    uint32_t flags, count_objs;
    uint64_t objs_ptr, count_props_ptr, props_ptr, prop_values_ptr;
    uint64_t reserved, user_data;
};

struct drm_mode_get_lease {
    uint32_t count_objects, pad;
    uint64_t objects_ptr;
};

_Static_assert(sizeof(struct drm_set_client_cap) == 16, "client cap ABI");
_Static_assert(sizeof(struct drm_mode_modeinfo) == 68, "modeinfo ABI");
_Static_assert(sizeof(struct drm_mode_card_res) == 64, "resources ABI");
_Static_assert(sizeof(struct drm_mode_get_connector) == 80, "connector ABI");
_Static_assert(sizeof(struct drm_mode_get_plane_res) == 16, "plane res ABI");
_Static_assert(sizeof(struct drm_mode_get_plane) == 32, "plane ABI");
_Static_assert(sizeof(struct drm_mode_obj_get_properties) == 32, "obj props ABI");
_Static_assert(sizeof(struct drm_mode_get_property) == 64, "property ABI");
_Static_assert(sizeof(struct drm_mode_create_blob) == 16, "blob ABI");
_Static_assert(sizeof(struct drm_mode_create_dumb) == 32, "create dumb ABI");
_Static_assert(sizeof(struct drm_mode_map_dumb) == 16, "map dumb ABI");
_Static_assert(sizeof(struct drm_mode_fb_cmd2) == 104, "fb2 ABI");
_Static_assert(sizeof(struct drm_mode_atomic) == 56, "atomic ABI");
_Static_assert(sizeof(struct drm_mode_get_lease) == 16, "lease ABI");

#define DRM_IOCTL_SET_CLIENT_CAP \
    _IOW(DRM_IOCTL_BASE, 0x0D, struct drm_set_client_cap)
#define DRM_IOCTL_MODE_GETRESOURCES \
    _IOWR(DRM_IOCTL_BASE, 0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCONNECTOR \
    _IOWR(DRM_IOCTL_BASE, 0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_RMFB \
    _IOWR(DRM_IOCTL_BASE, 0xAF, uint32_t)
#define DRM_IOCTL_MODE_CREATE_DUMB \
    _IOWR(DRM_IOCTL_BASE, 0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB \
    _IOWR(DRM_IOCTL_BASE, 0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB \
    _IOWR(DRM_IOCTL_BASE, 0xB4, struct drm_mode_destroy_dumb)
#define DRM_IOCTL_MODE_GETPLANERESOURCES \
    _IOWR(DRM_IOCTL_BASE, 0xB5, struct drm_mode_get_plane_res)
#define DRM_IOCTL_MODE_GETPLANE \
    _IOWR(DRM_IOCTL_BASE, 0xB6, struct drm_mode_get_plane)
#define DRM_IOCTL_MODE_ADDFB2 \
    _IOWR(DRM_IOCTL_BASE, 0xB8, struct drm_mode_fb_cmd2)
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES \
    _IOWR(DRM_IOCTL_BASE, 0xB9, struct drm_mode_obj_get_properties)
#define DRM_IOCTL_MODE_ATOMIC \
    _IOWR(DRM_IOCTL_BASE, 0xBC, struct drm_mode_atomic)
#define DRM_IOCTL_MODE_CREATEPROPBLOB \
    _IOWR(DRM_IOCTL_BASE, 0xBD, struct drm_mode_create_blob)
#define DRM_IOCTL_MODE_DESTROYPROPBLOB \
    _IOWR(DRM_IOCTL_BASE, 0xBE, struct drm_mode_destroy_blob)
#define DRM_IOCTL_MODE_GETPROPERTY \
    _IOWR(DRM_IOCTL_BASE, 0xAA, struct drm_mode_get_property)
#define DRM_IOCTL_MODE_GET_LEASE \
    _IOWR(DRM_IOCTL_BASE, 0xC8, struct drm_mode_get_lease)

struct resources {
    struct drm_mode_card_res res;
    uint32_t *crtcs, *connectors, *planes;
    uint32_t count_planes;
};

struct property_set {
    uint32_t connector_crtc_id;
    uint32_t crtc_mode_id, crtc_active;
    uint32_t plane_fb_id, plane_crtc_id;
    uint32_t plane_src_x, plane_src_y, plane_src_w, plane_src_h;
    uint32_t plane_crtc_x, plane_crtc_y, plane_crtc_w, plane_crtc_h;
};

struct frame_buffer {
    uint32_t handle, fb_id, pitch;
    uint64_t size;
    void *map;
};

struct lease_proof {
    uint32_t *objects;
    uint32_t count;
};

static volatile sig_atomic_t stop_requested;

static uint64_t ptr64(const void *pointer)
{
    return (uint64_t)(uintptr_t)pointer;
}

static void *bounded_calloc(uint32_t count, size_t size)
{
    if (!count)
        return NULL;
    if (count > MAX_OBJECTS || size > SIZE_MAX / count)
        return NULL;
    return calloc(count, size);
}

static int parse_nonnegative(const char *text, int *value)
{
    char *end = NULL;
    long parsed;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno || !text[0] || !end || *end || parsed < 0 || parsed > INT_MAX)
        return -1;
    *value = (int)parsed;
    return 0;
}

static int parse_positive_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;

    if (!text || !text[0] || text[0] < '0' || text[0] > '9')
        return -1;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || !end || *end || parsed == 0 || parsed > UINT32_MAX)
        return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int set_client_cap(int fd, uint64_t capability, const char *name)
{
    struct drm_set_client_cap cap = {capability, 1};
    if (ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) != 0) {
        printf("SET_CLIENT_CAP %s failed errno=%d (%s)\n",
               name, errno, strerror(errno));
        return -1;
    }
    printf("client cap %s = enabled on lease fd\n", name);
    return 0;
}

static int verify_lease(int fd, struct lease_proof *proof)
{
    struct drm_mode_get_lease request;
    uint32_t *objects = NULL, capacity, index;
    memset(&request, 0, sizeof(request));
    if (ioctl(fd, DRM_IOCTL_MODE_GET_LEASE, &request) != 0) {
        printf("GET_LEASE failed errno=%d (%s)\n", errno, strerror(errno));
        return -1;
    }
    capacity = request.count_objects;
    if (capacity > MAX_OBJECTS)
        return -1;
    objects = bounded_calloc(capacity, sizeof(*objects));
    request.count_objects = capacity;
    request.objects_ptr = ptr64(objects);
    if (capacity && ioctl(fd, DRM_IOCTL_MODE_GET_LEASE, &request) != 0) {
        free(objects);
        return -1;
    }
    if (request.count_objects > capacity || request.count_objects < 3) {
        free(objects);
        return -1;
    }
    printf("lease objects=%u:", request.count_objects);
    for (index = 0; index < request.count_objects && index < capacity; ++index)
        printf(" %u", objects[index]);
    putchar('\n');
    proof->objects = objects;
    proof->count = request.count_objects;
    return 0;
}

static int parse_approved_objects(const char *text, uint32_t **objects_out,
                                  uint32_t *count_out)
{
    const char *cursor = text;
    uint32_t *objects;
    uint32_t count = 0;

    if (!cursor || !*cursor)
        return -1;
    objects = bounded_calloc(MAX_OBJECTS, sizeof(*objects));
    if (!objects)
        return -1;
    while (*cursor) {
        char *end = NULL;
        unsigned long value;

        if (*cursor < '0' || *cursor > '9' || count == MAX_OBJECTS)
            goto fail;
        errno = 0;
        value = strtoul(cursor, &end, 10);
        if (errno || end == cursor || value == 0 || value > UINT32_MAX)
            goto fail;
        for (uint32_t index = 0; index < count; ++index) {
            if (objects[index] == (uint32_t)value)
                goto fail;
        }
        objects[count++] = (uint32_t)value;
        if (*end == '\0')
            break;
        if (*end != ',' || end[1] == '\0')
            goto fail;
        cursor = end + 1;
    }
    if (count < 3)
        goto fail;
    *objects_out = objects;
    *count_out = count;
    return 0;

fail:
    free(objects);
    return -1;
}

static int contains_id(const uint32_t *objects, uint32_t count, uint32_t id)
{
    for (uint32_t index = 0; index < count; ++index) {
        if (objects[index] == id)
            return 1;
    }
    return 0;
}

static int verify_canonical_lease(const struct lease_proof *proof,
                                  const struct resources *resources,
                                  const struct drm_mode_get_connector *connector,
                                  const char *approved_text,
                                  uint32_t **approved_out,
                                  uint32_t *approved_count_out)
{
    uint32_t *approved = NULL;
    uint32_t approved_count = 0;

    if (parse_approved_objects(approved_text, &approved,
                               &approved_count) != 0)
        return -1;
    if (proof->count != approved_count ||
        resources->res.count_connectors != 1 ||
        resources->res.count_crtcs != 1 ||
        resources->count_planes != approved_count - 2 ||
        resources->connectors[0] != approved[0] ||
        resources->crtcs[0] != approved[1] ||
        connector->connector_id != approved[0] ||
        connector->connector_type != 10 ||
        connector->connection != DRM_MODE_CONNECTED)
        goto fail;
    for (uint32_t index = 0; index < approved_count; ++index) {
        if (!contains_id(proof->objects, proof->count, approved[index]))
            goto fail;
    }
    for (uint32_t index = 0; index < proof->count; ++index) {
        if (!contains_id(approved, approved_count, proof->objects[index]))
            goto fail;
    }
    for (uint32_t index = 2; index < approved_count; ++index) {
        if (!contains_id(resources->planes, resources->count_planes,
                         approved[index]))
            goto fail;
    }
    *approved_out = approved;
    *approved_count_out = approved_count;
    return 0;

fail:
    free(approved);
    return -1;
}

static void print_canonical_lease(const char *approved_text,
                                  const uint32_t *approved,
                                  uint32_t approved_count)
{
    printf("MUTTER_DRM_LEASE_V1 role=lessee get_lease=success objects=%s "
           "connectors=1 crtcs=1 planes=%u\n",
           approved_text, approved_count - 2);
    printf("MUTTER_DRM_OBJECT_V1 id=%u type=connector "
           "connector_type_id=10 connector_type=DisplayPort "
           "connection=connected\n", approved[0]);
    printf("MUTTER_DRM_OBJECT_V1 id=%u type=crtc\n", approved[1]);
    for (uint32_t index = 2; index < approved_count; ++index)
        printf("MUTTER_DRM_OBJECT_V1 id=%u type=plane\n", approved[index]);
}

static void free_resources(struct resources *resources)
{
    free(resources->crtcs);
    free(resources->connectors);
    free(resources->planes);
    memset(resources, 0, sizeof(*resources));
}

static int load_resources(int fd, struct resources *resources)
{
    struct drm_mode_card_res first, second;
    struct drm_mode_get_plane_res plane_first, plane_second;
    uint32_t *fbs = NULL, *encoders = NULL;

    memset(&first, 0, sizeof(first));
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &first) != 0 ||
        first.count_crtcs > MAX_OBJECTS ||
        first.count_connectors > MAX_OBJECTS ||
        first.count_fbs > MAX_OBJECTS ||
        first.count_encoders > MAX_OBJECTS)
        return -1;
    resources->crtcs = bounded_calloc(first.count_crtcs, sizeof(uint32_t));
    resources->connectors = bounded_calloc(first.count_connectors, sizeof(uint32_t));
    fbs = bounded_calloc(first.count_fbs, sizeof(uint32_t));
    encoders = bounded_calloc(first.count_encoders, sizeof(uint32_t));
    if ((first.count_crtcs && !resources->crtcs) ||
        (first.count_connectors && !resources->connectors) ||
        (first.count_fbs && !fbs) || (first.count_encoders && !encoders))
        goto fail;

    second = first;
    second.crtc_id_ptr = ptr64(resources->crtcs);
    second.connector_id_ptr = ptr64(resources->connectors);
    second.fb_id_ptr = ptr64(fbs);
    second.encoder_id_ptr = ptr64(encoders);
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &second) != 0 ||
        second.count_crtcs > first.count_crtcs ||
        second.count_connectors > first.count_connectors)
        goto fail;
    resources->res = second;
    free(fbs);
    free(encoders);
    fbs = encoders = NULL;

    memset(&plane_first, 0, sizeof(plane_first));
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &plane_first) != 0 ||
        plane_first.count_planes > MAX_OBJECTS)
        goto fail;
    resources->planes = bounded_calloc(plane_first.count_planes,
                                        sizeof(uint32_t));
    if (plane_first.count_planes && !resources->planes)
        goto fail;
    plane_second = plane_first;
    plane_second.plane_id_ptr = ptr64(resources->planes);
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &plane_second) != 0 ||
        plane_second.count_planes > plane_first.count_planes)
        goto fail;
    resources->count_planes = plane_second.count_planes;
    printf("lessee resources: connectors=%u crtcs=%u planes=%u\n",
           second.count_connectors, second.count_crtcs,
           resources->count_planes);
    if (!second.count_connectors || !second.count_crtcs ||
        !resources->count_planes)
        goto fail;
    return 0;

fail:
    printf("resource enumeration failed errno=%d (%s)\n", errno, strerror(errno));
    free(fbs);
    free(encoders);
    free_resources(resources);
    return -1;
}

static int get_connector_and_mode(int fd, uint32_t connector_id,
                                  const char *requested_mode,
                                  struct drm_mode_get_connector *connector,
                                  struct drm_mode_modeinfo *selected)
{
    struct drm_mode_get_connector first, second;
    struct drm_mode_modeinfo *modes = NULL;
    uint32_t *encoders = NULL, index, chosen = 0;
    unsigned requested_width = 0, requested_height = 0, requested_refresh = 0;
    int requested = requested_mode && *requested_mode;

    if (requested &&
        sscanf(requested_mode, "%ux%u@%u", &requested_width,
               &requested_height, &requested_refresh) != 3) {
        printf("invalid requested mode '%s' (expected WIDTHxHEIGHT@HZ)\n",
               requested_mode);
        errno = EINVAL;
        return -1;
    }
    memset(&first, 0, sizeof(first));
    first.connector_id = connector_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &first) != 0 ||
        first.count_modes > MAX_OBJECTS || first.count_encoders > MAX_OBJECTS)
        return -1;
    modes = bounded_calloc(first.count_modes, sizeof(*modes));
    encoders = bounded_calloc(first.count_encoders, sizeof(*encoders));
    if ((first.count_modes && !modes) ||
        (first.count_encoders && !encoders))
        goto fail;
    second = first;
    second.modes_ptr = ptr64(modes);
    second.encoders_ptr = ptr64(encoders);
    /* We only need modes/encoders here.  A nonzero property count with a
     * null property pointer would make the second ioctl copy to NULL. */
    second.count_props = 0;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &second) != 0 ||
        second.count_modes > first.count_modes)
        goto fail;
    if (second.connection != DRM_MODE_CONNECTED || !second.count_modes) {
        errno = ENODEV;
        goto fail;
    }
    if (requested) {
        int found = 0;
        for (index = 0; index < second.count_modes; ++index) {
            if (modes[index].hdisplay == requested_width &&
                modes[index].vdisplay == requested_height &&
                modes[index].vrefresh == requested_refresh) {
                chosen = index;
                found = 1;
                break;
            }
        }
        if (!found) {
            printf("requested mode %s is not exposed; available modes:\n",
                   requested_mode);
            for (index = 0; index < second.count_modes; ++index)
                printf("  %s %ux%u@%u%s\n", modes[index].name,
                       modes[index].hdisplay, modes[index].vdisplay,
                       modes[index].vrefresh,
                       (modes[index].type & DRM_MODE_TYPE_PREFERRED) ?
                           " preferred" : "");
            errno = ENOENT;
            goto fail;
        }
    } else {
        for (index = 0; index < second.count_modes; ++index) {
            if (modes[index].type & DRM_MODE_TYPE_PREFERRED) {
                chosen = index;
                break;
            }
        }
    }
    *connector = second;
    *selected = modes[chosen];
    printf("connector=%u status=connected modes=%u selected=%s %ux%u@%u source=%s\n",
           connector_id, second.count_modes, selected->name,
           selected->hdisplay, selected->vdisplay, selected->vrefresh,
           requested ? "explicit" : "preferred");
    free(modes);
    free(encoders);
    return 0;
fail:
    free(modes);
    free(encoders);
    return -1;
}

static int plane_supports_xrgb8888(int fd, uint32_t plane_id)
{
    struct drm_mode_get_plane first, second;
    uint32_t *formats = NULL, capacity, index;
    memset(&first, 0, sizeof(first));
    first.plane_id = plane_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &first) != 0 ||
        first.count_format_types > MAX_OBJECTS)
        return -1;
    capacity = first.count_format_types;
    formats = bounded_calloc(capacity, sizeof(*formats));
    if (capacity && !formats)
        return -1;
    second = first;
    second.format_type_ptr = ptr64(formats);
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &second) != 0 ||
        second.count_format_types > capacity) {
        free(formats);
        return -1;
    }
    for (index = 0; index < second.count_format_types; ++index) {
        if (formats[index] == DRM_FORMAT_XRGB8888) {
            free(formats);
            return 1;
        }
    }
    free(formats);
    errno = EINVAL;
    return 0;
}

static uint32_t find_property(int fd, uint32_t object_id,
                              uint32_t object_type, const char *wanted)
{
    struct drm_mode_obj_get_properties first, second;
    uint32_t *ids = NULL, capacity, index, found = 0;
    uint64_t *values = NULL;
    memset(&first, 0, sizeof(first));
    first.obj_id = object_id;
    first.obj_type = object_type;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &first) != 0 ||
        first.count_props > MAX_OBJECTS)
        return 0;
    capacity = first.count_props;
    ids = bounded_calloc(capacity, sizeof(*ids));
    values = bounded_calloc(capacity, sizeof(*values));
    if ((capacity && !ids) || (capacity && !values))
        goto done;
    second = first;
    second.props_ptr = ptr64(ids);
    second.prop_values_ptr = ptr64(values);
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &second) != 0 ||
        second.count_props > capacity)
        goto done;
    for (index = 0; index < second.count_props; ++index) {
        struct drm_mode_get_property property;
        memset(&property, 0, sizeof(property));
        property.prop_id = ids[index];
        if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &property) == 0 &&
            strncmp(property.name, wanted, DRM_PROP_NAME_LEN) == 0) {
            found = ids[index];
            break;
        }
    }
done:
    free(ids);
    free(values);
    return found;
}

static int load_properties(int fd, uint32_t connector_id,
                           uint32_t crtc_id, uint32_t plane_id,
                           struct property_set *properties)
{
#define LOAD_PROP(field, object, type, name) do { \
    properties->field = find_property(fd, object, type, name); \
    if (!properties->field) { \
        printf("missing required property %s on object %u\n", name, object); \
        return -1; \
    } \
} while (0)
    memset(properties, 0, sizeof(*properties));
    LOAD_PROP(connector_crtc_id, connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    LOAD_PROP(crtc_mode_id, crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    LOAD_PROP(crtc_active, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    LOAD_PROP(plane_fb_id, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
    LOAD_PROP(plane_crtc_id, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    LOAD_PROP(plane_src_x, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X");
    LOAD_PROP(plane_src_y, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    LOAD_PROP(plane_src_w, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
    LOAD_PROP(plane_src_h, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");
    LOAD_PROP(plane_crtc_x, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    LOAD_PROP(plane_crtc_y, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    LOAD_PROP(plane_crtc_w, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    LOAD_PROP(plane_crtc_h, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
#undef LOAD_PROP
    puts("required atomic properties = present");
    return 0;
}

static int create_frame_buffer(int fd, uint32_t width, uint32_t height,
                               struct frame_buffer *frame)
{
    struct drm_mode_create_dumb dumb;
    struct drm_mode_map_dumb map;
    struct drm_mode_fb_cmd2 fb;
    memset(frame, 0, sizeof(*frame));
    frame->map = MAP_FAILED;
    memset(&dumb, 0, sizeof(dumb));
    dumb.width = width;
    dumb.height = height;
    dumb.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) != 0) {
        printf("CREATE_DUMB failed errno=%d (%s)\n", errno, strerror(errno));
        return -1;
    }
    frame->handle = dumb.handle;
    frame->pitch = dumb.pitch;
    frame->size = dumb.size;

    memset(&fb, 0, sizeof(fb));
    fb.width = width;
    fb.height = height;
    fb.pixel_format = DRM_FORMAT_XRGB8888;
    fb.handles[0] = dumb.handle;
    fb.pitches[0] = dumb.pitch;
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) != 0) {
        printf("ADDFB2 XR24 failed errno=%d (%s)\n", errno, strerror(errno));
        return -1;
    }
    frame->fb_id = fb.fb_id;

    memset(&map, 0, sizeof(map));
    map.handle = dumb.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        printf("MAP_DUMB failed errno=%d (%s)\n", errno, strerror(errno));
        return -1;
    }
    frame->map = mmap(NULL, (size_t)frame->size,
                      PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)map.offset);
    if (frame->map == MAP_FAILED) {
        printf("mmap dumb buffer failed errno=%d (%s)\n", errno, strerror(errno));
        return -1;
    }
    printf("framebuffer: %ux%u XR24 handle=%u fb=%u pitch=%u size=%llu\n",
           width, height, frame->handle, frame->fb_id, frame->pitch,
           (unsigned long long)frame->size);
    return 0;
}

static void destroy_frame_buffer(int fd, struct frame_buffer *frame)
{
    if (frame->map != MAP_FAILED && frame->map)
        munmap(frame->map, (size_t)frame->size);
    if (frame->fb_id) {
        uint32_t fb_id = frame->fb_id;
        (void)ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_id);
    }
    if (frame->handle) {
        struct drm_mode_destroy_dumb destroy = {frame->handle};
        (void)ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
    memset(frame, 0, sizeof(*frame));
    frame->map = MAP_FAILED;
}

static void fill_pattern(struct frame_buffer *frame,
                         uint32_t width, uint32_t height)
{
    static const uint32_t bars[8] = {
        0x00ffffffU, 0x00ffff00U, 0x0000ffffU, 0x0000ff00U,
        0x00ff00ffU, 0x00ff0000U, 0x000000ffU, 0x00000000U
    };
    uint32_t y, x;
    for (y = 0; y < height; ++y) {
        uint32_t *row = (uint32_t *)((uint8_t *)frame->map +
                                     (size_t)y * frame->pitch);
        for (x = 0; x < width; ++x) {
            uint32_t color;
            if (y < height * 3U / 4U) {
                uint32_t bar = (uint32_t)((uint64_t)x * 8U / width);
                if (bar > 7)
                    bar = 7;
                color = bars[bar];
            } else {
                uint32_t checker = ((x / 16U) ^ (y / 16U)) & 1U;
                color = checker ? 0x00202020U : 0x00d0d0d0U;
            }
            if (x < 8 || y < 8 || x + 8 >= width || y + 8 >= height)
                color = 0x00ff4000U;
            row[x] = color;
        }
    }
    (void)msync(frame->map, (size_t)frame->size, MS_SYNC);
    puts("pattern: 8 color bars + checkerboard + orange border");
}

static int create_mode_blob(int fd, const struct drm_mode_modeinfo *mode,
                            uint32_t *blob_id)
{
    struct drm_mode_create_blob blob;
    memset(&blob, 0, sizeof(blob));
    blob.data = ptr64(mode);
    blob.length = sizeof(*mode);
    if (ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob) != 0) {
        printf("CREATEPROPBLOB failed errno=%d (%s)\n", errno, strerror(errno));
        return -1;
    }
    *blob_id = blob.blob_id;
    printf("mode blob=%u\n", *blob_id);
    return 0;
}

static void destroy_mode_blob(int fd, uint32_t blob_id)
{
    if (blob_id) {
        struct drm_mode_destroy_blob blob = {blob_id};
        (void)ioctl(fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &blob);
    }
}

static int atomic_modeset(int fd, uint32_t connector_id, uint32_t crtc_id,
                          uint32_t plane_id, const struct property_set *p,
                          uint32_t mode_blob, uint32_t fb_id,
                          uint32_t width, uint32_t height, int test_only)
{
    uint32_t objects[3] = {connector_id, crtc_id, plane_id};
    uint32_t counts[3] = {1, 2, 10};
    uint32_t props[13] = {
        p->connector_crtc_id,
        p->crtc_mode_id, p->crtc_active,
        p->plane_fb_id, p->plane_crtc_id,
        p->plane_src_x, p->plane_src_y, p->plane_src_w, p->plane_src_h,
        p->plane_crtc_x, p->plane_crtc_y, p->plane_crtc_w, p->plane_crtc_h
    };
    uint64_t values[13] = {
        crtc_id,
        mode_blob, 1,
        fb_id, crtc_id,
        0, 0, (uint64_t)width << 16, (uint64_t)height << 16,
        0, 0, width, height
    };
    struct drm_mode_atomic request;
    memset(&request, 0, sizeof(request));
    request.flags = DRM_MODE_ATOMIC_ALLOW_MODESET |
                    (test_only ? DRM_MODE_ATOMIC_TEST_ONLY : 0);
    request.count_objs = 3;
    request.objs_ptr = ptr64(objects);
    request.count_props_ptr = ptr64(counts);
    request.props_ptr = ptr64(props);
    request.prop_values_ptr = ptr64(values);
    errno = 0;
    if (ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &request) != 0) {
        printf("ATOMIC %s failed errno=%d (%s)\n",
               test_only ? "TEST_ONLY" : "COMMIT", errno, strerror(errno));
        return -1;
    }
    printf("ATOMIC %s = SUCCESS\n", test_only ? "TEST_ONLY" : "COMMIT");
    return 0;
}

static int atomic_disable(int fd, uint32_t connector_id, uint32_t crtc_id,
                          uint32_t plane_id, const struct property_set *p)
{
    uint32_t objects[3] = {plane_id, connector_id, crtc_id};
    uint32_t counts[3] = {2, 1, 2};
    uint32_t props[5] = {p->plane_fb_id, p->plane_crtc_id,
                         p->connector_crtc_id,
                         p->crtc_active, p->crtc_mode_id};
    uint64_t values[5] = {0, 0, 0, 0, 0};
    struct drm_mode_atomic request;
    memset(&request, 0, sizeof(request));
    request.flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
    request.count_objs = 3;
    request.objs_ptr = ptr64(objects);
    request.count_props_ptr = ptr64(counts);
    request.props_ptr = ptr64(props);
    request.prop_values_ptr = ptr64(values);
    if (ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &request) != 0) {
        printf("cleanup ATOMIC disable failed errno=%d (%s)\n",
               errno, strerror(errno));
        return -1;
    }
    puts("cleanup ATOMIC disable = SUCCESS");
    return 0;
}

static void usage(const char *program)
{
    printf("usage: %s [--fd N] [--mode WIDTHxHEIGHT@HZ] [--commit] [--seconds N]\n",
           program);
    puts("  default:     use DRM_LEASE_FD and perform TEST_ONLY only");
    puts("  --mode:      select an exact connector mode instead of preferred");
    puts("  --commit:    perform the real modeset after TEST_ONLY succeeds");
    puts("  --seconds N: with --commit, hold N seconds; 0 means until Ctrl-C");
}

int main(int argc, char **argv)
{
    const char *fd_environment;
    const char *lessee_environment = getenv("DRM_LEASE_LESSEE_ID");
    const char *approved_text = getenv("DRM_LEASE_OBJECTS");
    const char *requested_mode = getenv("DRM_TEST_MODE");
    struct resources resources;
    struct lease_proof proof;
    struct drm_mode_get_connector connector;
    struct drm_mode_modeinfo mode;
    struct property_set properties;
    struct frame_buffer frame;
    struct stat descriptor_stat;
    uint32_t lessee_id = 0;
    uint32_t *approved_objects = NULL, approved_count = 0;
    uint32_t connector_id, crtc_id, plane_id, mode_blob = 0;
    int fd = -1, commit = 0, seconds = 0, index, result = 1;
    int committed = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    puts("START - inherited DRM lease atomic pattern renderer with mode selection");
    memset(&resources, 0, sizeof(resources));
    memset(&proof, 0, sizeof(proof));
    memset(&frame, 0, sizeof(frame));
    frame.map = MAP_FAILED;

    fd_environment = getenv("DRM_LEASE_FD");
    if (fd_environment && parse_nonnegative(fd_environment, &fd) != 0)
        fd = -1;
    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--commit") == 0) {
            commit = 1;
        } else if (strcmp(argv[index], "--fd") == 0) {
            if (++index == argc || parse_nonnegative(argv[index], &fd) != 0) {
                usage(argv[0]);
                goto done;
            }
        } else if (strcmp(argv[index], "--seconds") == 0) {
            if (++index == argc || parse_nonnegative(argv[index], &seconds) != 0) {
                usage(argv[0]);
                goto done;
            }
        } else if (strcmp(argv[index], "--mode") == 0) {
            if (++index == argc) {
                usage(argv[0]);
                goto done;
            }
            requested_mode = argv[index];
        } else {
            usage(argv[0]);
            goto done;
        }
    }
    if (fd < 0 || (seconds && !commit)) {
        usage(argv[0]);
        goto done;
    }
    printf("lease fd=%d action=%s hold=%s\n", fd,
           commit ? "TEST_ONLY+COMMIT" : "TEST_ONLY",
           commit ? (seconds ? "timed" : "until Ctrl-C") : "no");
    if (parse_positive_u32(lessee_environment, &lessee_id) != 0) {
        puts("DRM_LEASE_LESSEE_ID must be a positive decimal uint32");
        goto done;
    }
    printf("environment: LESSEE_ID=%u OBJECTS=%s\n",
           lessee_id, approved_text ?: "unset");
    if (fstat(fd, &descriptor_stat) != 0 || !S_ISCHR(descriptor_stat.st_mode)) {
        printf("lease fd is not a character device errno=%d (%s)\n",
               errno, strerror(errno));
        goto done;
    }
    printf("lease device=char %u:%u\n",
           major(descriptor_stat.st_rdev), minor(descriptor_stat.st_rdev));
    if (verify_lease(fd, &proof) != 0)
        goto done;
    if (set_client_cap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES,
                       "UNIVERSAL_PLANES") != 0 ||
        set_client_cap(fd, DRM_CLIENT_CAP_ATOMIC, "ATOMIC") != 0)
        goto done;
    if (load_resources(fd, &resources) != 0)
        goto done;

    connector_id = resources.connectors[0];
    crtc_id = resources.crtcs[0];
    plane_id = resources.planes[0];
    printf("selected leased path: connector=%u CRTC=%u plane=%u\n",
           connector_id, crtc_id, plane_id);
    if (get_connector_and_mode(fd, connector_id, requested_mode,
                               &connector, &mode) != 0) {
        printf("cannot get a connected mode errno=%d (%s)\n",
               errno, strerror(errno));
        goto done;
    }
    if (verify_canonical_lease(&proof, &resources, &connector,
                               approved_text, &approved_objects,
                               &approved_count) != 0) {
        puts("canonical lease proof rejected: environment and kernel objects differ");
        goto done;
    }
    print_canonical_lease(approved_text, approved_objects, approved_count);
    if (plane_supports_xrgb8888(fd, plane_id) != 1) {
        printf("plane %u does not expose XR24 errno=%d (%s)\n",
               plane_id, errno, strerror(errno));
        goto done;
    }
    puts("plane format XR24 = supported");
    if (load_properties(fd, connector_id, crtc_id, plane_id,
                        &properties) != 0)
        goto done;
    if (create_frame_buffer(fd, mode.hdisplay, mode.vdisplay, &frame) != 0)
        goto done;
    fill_pattern(&frame, mode.hdisplay, mode.vdisplay);
    if (create_mode_blob(fd, &mode, &mode_blob) != 0)
        goto done;
    if (atomic_modeset(fd, connector_id, crtc_id, plane_id, &properties,
                       mode_blob, frame.fb_id, mode.hdisplay, mode.vdisplay,
                       1) != 0)
        goto done;

    if (!commit) {
        puts("MUTTER_DRM_TEST_V1 atomic_test_only=success real_commit=0");
        puts("TEST_ONLY COMPLETE: no real atomic commit was issued");
        result = 0;
        goto done;
    }
    puts("\n== REAL ATOMIC COMMIT (DP takeover) ==");
    if (atomic_modeset(fd, connector_id, crtc_id, plane_id, &properties,
                       mode_blob, frame.fb_id, mode.hdisplay, mode.vdisplay,
                       0) != 0)
        goto done;
    committed = 1;
    result = 0;

    {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = handle_signal;
        sigemptyset(&action.sa_mask);
        sigaction(SIGINT, &action, NULL);
        sigaction(SIGTERM, &action, NULL);
    }

    if (seconds) {
        printf("HOLDING pattern for %d second(s)\n", seconds);
        while (seconds-- > 0 && !stop_requested)
            sleep(1);
    } else {
        puts("HOLDING pattern until Ctrl-C");
        while (!stop_requested)
            pause();
    }

done:
    if (committed) {
        puts("releasing scanout before lease close");
        (void)atomic_disable(fd, connector_id, crtc_id, plane_id, &properties);
    }
    if (fd >= 0) {
        destroy_mode_blob(fd, mode_blob);
        destroy_frame_buffer(fd, &frame);
    }
    free_resources(&resources);
    free(proof.objects);
    free(approved_objects);
    printf("DONE exit=%d (lease closes when exec chain exits)\n", result);
    return result;
}
