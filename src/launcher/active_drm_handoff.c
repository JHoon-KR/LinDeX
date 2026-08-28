// SPDX-License-Identifier: MIT
// preserve Android-initialized DP timing while handing an active lease to Debian.

#define DRM_HANDOFF_MAIN drm_handoff_embedded_main
#include "drm_handoff.c"
#undef DRM_HANDOFF_MAIN

#include <grp.h>

/* Validate the selected scanout plane with the preserved physical timing.
 * Qualcomm SDE can expose several apparently interchangeable primary planes,
 * while only a subset may pass atomic validation in the current resource
 * allocation.  Capability/idle checks alone are therefore insufficient. */
#define AH_DRM_PROP_NAME_LEN 32
#define AH_DRM_CLIENT_CAP_UNIVERSAL_PLANES 2ULL
#define AH_DRM_CLIENT_CAP_ATOMIC 3ULL
#define AH_DRM_MODE_OBJECT_CRTC 0xccccccccU
#define AH_DRM_MODE_OBJECT_CONNECTOR 0xc0c0c0c0U
#define AH_DRM_MODE_OBJECT_PLANE 0xeeeeeeeeU
#define AH_DRM_MODE_ATOMIC_TEST_ONLY 0x0100U
#define AH_DRM_MODE_ATOMIC_ALLOW_MODESET 0x0400U
#define AH_DRM_FORMAT_XRGB8888 0x34325258U

struct ah_drm_set_client_cap { uint64_t capability, value; };
struct ah_drm_mode_obj_get_properties {
    uint64_t props_ptr, prop_values_ptr;
    uint32_t count_props, obj_id, obj_type;
};
struct ah_drm_mode_get_property {
    uint64_t values_ptr, enum_blob_ptr;
    uint32_t prop_id, flags;
    char name[AH_DRM_PROP_NAME_LEN];
    uint32_t count_values, count_enum_blobs;
};
struct ah_drm_mode_create_blob { uint64_t data; uint32_t length, blob_id; };
struct ah_drm_mode_destroy_blob { uint32_t blob_id; };
struct ah_drm_mode_create_dumb {
    uint32_t height, width, bpp, flags;
    uint32_t handle, pitch;
    uint64_t size;
};
struct ah_drm_mode_destroy_dumb { uint32_t handle; };
struct ah_drm_mode_fb_cmd2 {
    uint32_t fb_id, width, height, pixel_format, flags;
    uint32_t handles[4], pitches[4], offsets[4];
    uint64_t modifiers[4];
};
struct ah_drm_mode_atomic {
    uint32_t flags, count_objs;
    uint64_t objs_ptr, count_props_ptr, props_ptr, prop_values_ptr;
    uint64_t reserved, user_data;
};
struct ah_drm_mode_revoke_lease { uint32_t lessee_id; };
struct ah_plane_properties {
    uint32_t connector_crtc_id;
    uint32_t crtc_mode_id, crtc_active;
    uint32_t plane_fb_id, plane_crtc_id;
    uint32_t plane_src_x, plane_src_y, plane_src_w, plane_src_h;
    uint32_t plane_crtc_x, plane_crtc_y, plane_crtc_w, plane_crtc_h;
};

#define AH_IOCTL_SET_CLIENT_CAP \
    _IOW(DRM_IOCTL_BASE, 0x0D, struct ah_drm_set_client_cap)
#define AH_IOCTL_MODE_RMFB _IOWR(DRM_IOCTL_BASE, 0xAF, uint32_t)
#define AH_IOCTL_MODE_CREATE_DUMB \
    _IOWR(DRM_IOCTL_BASE, 0xB2, struct ah_drm_mode_create_dumb)
#define AH_IOCTL_MODE_DESTROY_DUMB \
    _IOWR(DRM_IOCTL_BASE, 0xB4, struct ah_drm_mode_destroy_dumb)
#define AH_IOCTL_MODE_ADDFB2 \
    _IOWR(DRM_IOCTL_BASE, 0xB8, struct ah_drm_mode_fb_cmd2)
#define AH_IOCTL_MODE_OBJ_GETPROPERTIES \
    _IOWR(DRM_IOCTL_BASE, 0xB9, struct ah_drm_mode_obj_get_properties)
#define AH_IOCTL_MODE_GETPROPERTY \
    _IOWR(DRM_IOCTL_BASE, 0xAA, struct ah_drm_mode_get_property)
#define AH_IOCTL_MODE_ATOMIC \
    _IOWR(DRM_IOCTL_BASE, 0xBC, struct ah_drm_mode_atomic)
#define AH_IOCTL_MODE_CREATEPROPBLOB \
    _IOWR(DRM_IOCTL_BASE, 0xBD, struct ah_drm_mode_create_blob)
#define AH_IOCTL_MODE_DESTROYPROPBLOB \
    _IOWR(DRM_IOCTL_BASE, 0xBE, struct ah_drm_mode_destroy_blob)
#define AH_IOCTL_MODE_REVOKE_LEASE \
    _IOWR(DRM_IOCTL_BASE, 0xC9, struct ah_drm_mode_revoke_lease)

static int ah_lessor_controls_lessee(int fd, uint32_t wanted)
{
    struct drm_mode_list_lessees request;
    uint64_t *ids = NULL;
    int found = 0;

    memset(&request, 0, sizeof(request));
    if (ioctl(fd, DRM_IOCTL_MODE_LIST_LESSEES, &request) != 0 ||
        request.count_lessees > MAX_OBJECTS)
        return 0;
    ids = bounded_calloc(request.count_lessees, sizeof(*ids));
    if (request.count_lessees && !ids)
        return 0;
    uint32_t capacity = request.count_lessees;
    request.lessees_ptr = ptr64(ids);
    if (capacity && ioctl(fd, DRM_IOCTL_MODE_LIST_LESSEES, &request) != 0)
        goto done;
    for (uint32_t index = 0;
         index < request.count_lessees && index < capacity; ++index) {
        if (ids[index] == wanted) {
            found = 1;
            break;
        }
    }
done:
    free(ids);
    return found;
}

static int ah_find_lessor(const struct stat *card0, uint32_t lessee_id,
                          int *pid_out, int *pidfd_out, int *master_out)
{
    DIR *proc = opendir("/proc");
    struct dirent *entry;
    if (!proc)
        return -1;
    while ((entry = readdir(proc)) != NULL) {
        int pid, pidfd, candidate, target_fd = -1;
        if (parse_positive_int(entry->d_name, &pid) != 0 ||
            !process_has_card0(pid, card0))
            continue;
        pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
        if (pidfd < 0)
            continue;
        candidate = find_master(pid, pidfd, card0, -1, &target_fd);
        if (candidate >= 0 &&
            ah_lessor_controls_lessee(candidate, lessee_id)) {
            closedir(proc);
            *pid_out = pid;
            *pidfd_out = pidfd;
            *master_out = candidate;
            printf("AUTO LESSOR: pid=%d target_fd=%d local_fd=%d lessee=%u\n",
                   pid, target_fd, candidate, lessee_id);
            return 0;
        }
        if (candidate >= 0)
            close(candidate);
        close(pidfd);
    }
    closedir(proc);
    return -1;
}

static int ah_revoke_lessee(uint32_t lessee_id)
{
    struct stat card0;
    struct ah_drm_mode_revoke_lease request = { .lessee_id = lessee_id };
    int master_pid = -1, pidfd = -1, master_fd = -1;
    int result = -1;

    if (stat("/dev/dri/card0", &card0) != 0 || !S_ISCHR(card0.st_mode) ||
        ah_find_lessor(&card0, lessee_id,
                       &master_pid, &pidfd, &master_fd) != 0) {
        puts("WARNING: could not reacquire the DRM lessor controlling this lessee");
        goto done;
    }
    printf("revoking lessee_id=%u via lessor pid=%d\n",
           lessee_id, master_pid);
    if (ioctl(master_fd, AH_IOCTL_MODE_REVOKE_LEASE, &request) != 0) {
        printf("WARNING: REVOKE_LEASE failed errno=%d (%s)\n",
               errno, strerror(errno));
        goto done;
    }
    puts("REVOKE_LEASE success; inherited descendant fds invalidated");
    result = 0;

done:
    if (master_fd >= 0)
        close(master_fd);
    if (pidfd >= 0)
        close(pidfd);
    return result;
}

static uint32_t ah_find_property(int fd, uint32_t object_id,
                                 uint32_t object_type, const char *wanted)
{
    struct ah_drm_mode_obj_get_properties first, second;
    uint32_t *ids = NULL, index, found = 0;
    uint64_t *values = NULL;
    memset(&first, 0, sizeof(first));
    first.obj_id = object_id;
    first.obj_type = object_type;
    if (ioctl(fd, AH_IOCTL_MODE_OBJ_GETPROPERTIES, &first) != 0 ||
        first.count_props > MAX_OBJECTS)
        return 0;
    ids = bounded_calloc(first.count_props, sizeof(*ids));
    values = bounded_calloc(first.count_props, sizeof(*values));
    if ((first.count_props && !ids) || (first.count_props && !values))
        goto done;
    second = first;
    second.props_ptr = ptr64(ids);
    second.prop_values_ptr = ptr64(values);
    if (ioctl(fd, AH_IOCTL_MODE_OBJ_GETPROPERTIES, &second) != 0 ||
        second.count_props > first.count_props)
        goto done;
    for (index = 0; index < second.count_props; ++index) {
        struct ah_drm_mode_get_property property;
        memset(&property, 0, sizeof(property));
        property.prop_id = ids[index];
        if (ioctl(fd, AH_IOCTL_MODE_GETPROPERTY, &property) == 0 &&
            strncmp(property.name, wanted, AH_DRM_PROP_NAME_LEN) == 0) {
            found = ids[index];
            break;
        }
    }
done:
    free(ids);
    free(values);
    return found;
}

static int ah_get_property_value(int fd, uint32_t object_id,
                                 uint32_t object_type, const char *wanted,
                                 uint64_t *value)
{
    struct ah_drm_mode_obj_get_properties first, second;
    uint32_t *ids = NULL;
    uint64_t *values = NULL;
    int found = 0;
    memset(&first, 0, sizeof(first));
    first.obj_id = object_id;
    first.obj_type = object_type;
    if (ioctl(fd, AH_IOCTL_MODE_OBJ_GETPROPERTIES, &first) != 0 ||
        first.count_props > MAX_OBJECTS)
        return 0;
    ids = bounded_calloc(first.count_props, sizeof(*ids));
    values = bounded_calloc(first.count_props, sizeof(*values));
    if ((first.count_props && !ids) || (first.count_props && !values))
        goto done;
    second = first;
    second.props_ptr = ptr64(ids);
    second.prop_values_ptr = ptr64(values);
    if (ioctl(fd, AH_IOCTL_MODE_OBJ_GETPROPERTIES, &second) != 0 ||
        second.count_props > first.count_props)
        goto done;
    for (uint32_t index = 0; index < second.count_props; ++index) {
        struct ah_drm_mode_get_property property;
        memset(&property, 0, sizeof(property));
        property.prop_id = ids[index];
        if (ioctl(fd, AH_IOCTL_MODE_GETPROPERTY, &property) == 0 &&
            strncmp(property.name, wanted, AH_DRM_PROP_NAME_LEN) == 0) {
            *value = values[index];
            found = 1;
            break;
        }
    }
done:
    free(ids);
    free(values);
    return found;
}

static int ah_load_plane_properties(int fd, uint32_t connector_id,
                                    uint32_t crtc_id, uint32_t plane_id,
                                    struct ah_plane_properties *p)
{
#define AH_PROP(field, object, type, name) do { \
    p->field = ah_find_property(fd, object, type, name); \
    if (!p->field) return -1; \
} while (0)
    memset(p, 0, sizeof(*p));
    AH_PROP(connector_crtc_id, connector_id, AH_DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    AH_PROP(crtc_mode_id, crtc_id, AH_DRM_MODE_OBJECT_CRTC, "MODE_ID");
    AH_PROP(crtc_active, crtc_id, AH_DRM_MODE_OBJECT_CRTC, "ACTIVE");
    AH_PROP(plane_fb_id, plane_id, AH_DRM_MODE_OBJECT_PLANE, "FB_ID");
    AH_PROP(plane_crtc_id, plane_id, AH_DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    AH_PROP(plane_src_x, plane_id, AH_DRM_MODE_OBJECT_PLANE, "SRC_X");
    AH_PROP(plane_src_y, plane_id, AH_DRM_MODE_OBJECT_PLANE, "SRC_Y");
    AH_PROP(plane_src_w, plane_id, AH_DRM_MODE_OBJECT_PLANE, "SRC_W");
    AH_PROP(plane_src_h, plane_id, AH_DRM_MODE_OBJECT_PLANE, "SRC_H");
    AH_PROP(plane_crtc_x, plane_id, AH_DRM_MODE_OBJECT_PLANE, "CRTC_X");
    AH_PROP(plane_crtc_y, plane_id, AH_DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    AH_PROP(plane_crtc_w, plane_id, AH_DRM_MODE_OBJECT_PLANE, "CRTC_W");
    AH_PROP(plane_crtc_h, plane_id, AH_DRM_MODE_OBJECT_PLANE, "CRTC_H");
#undef AH_PROP
    return 0;
}

static int ah_plane_supports_xr24(int fd, uint32_t plane_id)
{
    struct drm_mode_get_plane first, second;
    uint32_t *formats = NULL, index;
    int found = 0;
    memset(&first, 0, sizeof(first));
    first.plane_id = plane_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &first) != 0 ||
        first.count_format_types > MAX_OBJECTS)
        return 0;
    formats = bounded_calloc(first.count_format_types, sizeof(*formats));
    if (first.count_format_types && !formats)
        return 0;
    second = first;
    second.format_type_ptr = ptr64(formats);
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &second) == 0 &&
        second.count_format_types <= first.count_format_types) {
        for (index = 0; index < second.count_format_types; ++index)
            if (formats[index] == AH_DRM_FORMAT_XRGB8888)
                found = 1;
    }
    free(formats);
    return found;
}

static int ah_atomic_test_plane(int fd, uint32_t connector_id,
                                uint32_t crtc_id, uint32_t plane_id,
                                const struct drm_mode_modeinfo *mode)
{
    struct ah_drm_mode_create_dumb dumb;
    struct ah_drm_mode_fb_cmd2 fb;
    struct ah_drm_mode_create_blob blob;
    struct ah_plane_properties p;
    uint32_t objects[3] = {connector_id, crtc_id, plane_id};
    uint32_t counts[3] = {1, 2, 10};
    uint32_t props[13];
    uint64_t values[13];
    struct ah_drm_mode_atomic request;
    int saved_errno, result = -1;

    memset(&dumb, 0, sizeof(dumb));
    memset(&fb, 0, sizeof(fb));
    memset(&blob, 0, sizeof(blob));
    if (ah_load_plane_properties(fd, connector_id, crtc_id, plane_id, &p) != 0)
        return -1;
    dumb.width = mode->hdisplay;
    dumb.height = mode->vdisplay;
    dumb.bpp = 32;
    if (ioctl(fd, AH_IOCTL_MODE_CREATE_DUMB, &dumb) != 0)
        return -1;
    fb.width = dumb.width;
    fb.height = dumb.height;
    fb.pixel_format = AH_DRM_FORMAT_XRGB8888;
    fb.handles[0] = dumb.handle;
    fb.pitches[0] = dumb.pitch;
    if (ioctl(fd, AH_IOCTL_MODE_ADDFB2, &fb) != 0)
        goto done;
    blob.data = ptr64(mode);
    blob.length = sizeof(*mode);
    if (ioctl(fd, AH_IOCTL_MODE_CREATEPROPBLOB, &blob) != 0)
        goto done;

    {
        uint32_t local_props[13] = {
            p.connector_crtc_id, p.crtc_mode_id, p.crtc_active,
            p.plane_fb_id, p.plane_crtc_id,
            p.plane_src_x, p.plane_src_y, p.plane_src_w, p.plane_src_h,
            p.plane_crtc_x, p.plane_crtc_y, p.plane_crtc_w, p.plane_crtc_h
        };
        uint64_t local_values[13] = {
            crtc_id, blob.blob_id, 1, fb.fb_id, crtc_id,
            0, 0, (uint64_t)mode->hdisplay << 16,
            (uint64_t)mode->vdisplay << 16,
            0, 0, mode->hdisplay, mode->vdisplay
        };
        memcpy(props, local_props, sizeof(props));
        memcpy(values, local_values, sizeof(values));
    }
    memset(&request, 0, sizeof(request));
    request.flags = AH_DRM_MODE_ATOMIC_ALLOW_MODESET |
                    AH_DRM_MODE_ATOMIC_TEST_ONLY;
    request.count_objs = 3;
    request.objs_ptr = ptr64(objects);
    request.count_props_ptr = ptr64(counts);
    request.props_ptr = ptr64(props);
    request.prop_values_ptr = ptr64(values);
    if (ioctl(fd, AH_IOCTL_MODE_ATOMIC, &request) == 0)
        result = 0;
done:
    saved_errno = errno;
    if (blob.blob_id) {
        struct ah_drm_mode_destroy_blob destroy_blob = {blob.blob_id};
        (void)ioctl(fd, AH_IOCTL_MODE_DESTROYPROPBLOB, &destroy_blob);
    }
    if (fb.fb_id) {
        uint32_t fb_id = fb.fb_id;
        (void)ioctl(fd, AH_IOCTL_MODE_RMFB, &fb_id);
    }
    if (dumb.handle) {
        struct ah_drm_mode_destroy_dumb destroy_dumb = {dumb.handle};
        (void)ioctl(fd, AH_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
    }
    errno = saved_errno;
    return result;
}

static int auto_select_atomic_plane(int fd, const struct resources *resources,
                                    struct handoff_options *options,
                                    uint32_t connector_id, uint32_t crtc_id)
{
    struct ah_drm_set_client_cap cap;
    struct drm_mode_crtc crtc;
    uint32_t selected_index = UINT32_MAX, selected_count = 0;
    uint32_t hinted_plane = 0, crtc_mask;
    int crtc_index;
    uint32_t primary_plane = 0;

    for (uint32_t i = 0; i < options->object_count; ++i) {
        if (index_of(resources->planes, resources->count_planes,
                     options->objects[i]) >= 0) {
            selected_index = i;
            hinted_plane = options->objects[i];
            ++selected_count;
        }
    }
    if (selected_count != 1) {
        puts("AUTO PLANE TEST skipped: exactly one hinted plane is required");
        return selected_count ? 0 : -1;
    }
    crtc_index = index_of(resources->crtcs, resources->res.count_crtcs,
                          crtc_id);
    if (crtc_index < 0 || crtc_index >= 32)
        return -1;
    crtc_mask = 1U << (unsigned int)crtc_index;
    memset(&crtc, 0, sizeof(crtc));
    crtc.crtc_id = crtc_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) != 0 || !crtc.mode_valid) {
        puts("AUTO PLANE TEST requires preserved active CRTC timing");
        return -1;
    }
    cap.value = 1;
    cap.capability = AH_DRM_CLIENT_CAP_UNIVERSAL_PLANES;
    if (ioctl(fd, AH_IOCTL_SET_CLIENT_CAP, &cap) != 0)
        return -1;
    cap.capability = AH_DRM_CLIENT_CAP_ATOMIC;
    if (ioctl(fd, AH_IOCTL_SET_CLIENT_CAP, &cap) != 0)
        return -1;

    puts("\n== Automatic atomic plane validation ==");
    printf("preserved mode=%s %ux%u@%u hinted_plane=%u\n",
           crtc.mode.name, crtc.mode.hdisplay, crtc.mode.vdisplay,
           crtc.mode.vrefresh, hinted_plane);
    for (uint32_t pass = 0; pass < resources->count_planes + 1U; ++pass) {
        uint32_t candidate = pass == 0 ? hinted_plane :
                             resources->planes[pass - 1U];
        struct drm_mode_get_plane plane;
        if (pass != 0 && candidate == hinted_plane)
            continue;
        memset(&plane, 0, sizeof(plane));
        plane.plane_id = candidate;
        if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane) != 0 ||
            plane.crtc_id || plane.fb_id ||
            !(plane.possible_crtcs & crtc_mask) ||
            !ah_plane_supports_xr24(fd, candidate))
            continue;
        errno = 0;
        if (ah_atomic_test_plane(fd, connector_id, crtc_id, candidate,
                                 &crtc.mode) == 0) {
            options->objects[selected_index] = candidate;
            primary_plane = candidate;
            printf("AUTO PLANE SELECTED: id=%u atomic TEST_ONLY=SUCCESS%s\n",
                   candidate, candidate == hinted_plane ? "" : " (fallback)");
            break;
        }
        printf("plane %u atomic TEST_ONLY rejected errno=%d (%s)\n",
               candidate, errno, strerror(errno));
    }
    if (!primary_plane) {
        puts("REFUSE: no idle compatible XR24 plane passed atomic TEST_ONLY");
        return -1;
    }

    puts("\n== Conservative additional-plane lease selection ==");
    puts("policy: idle cursor planes are allowed; overlay planes must be exclusive to the selected CRTC");
    for (uint32_t index = 0; index < resources->count_planes; ++index) {
        struct drm_mode_get_plane plane;
        uint64_t type = UINT64_MAX;
        uint32_t candidate = resources->planes[index];
        int already_selected = 0;
        if (candidate == primary_plane)
            continue;
        for (uint32_t object_index = 0;
             object_index < options->object_count; ++object_index) {
            if (options->objects[object_index] == candidate)
                already_selected = 1;
        }
        if (already_selected)
            continue;
        memset(&plane, 0, sizeof(plane));
        plane.plane_id = candidate;
        if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane) != 0 ||
            plane.crtc_id || plane.fb_id || !(plane.possible_crtcs & crtc_mask) ||
            !ah_get_property_value(fd, candidate, AH_DRM_MODE_OBJECT_PLANE,
                                   "type", &type))
            continue;
        if (type != 2 && !(type == 0 && plane.possible_crtcs == crtc_mask)) {
            if (type == 0)
                printf("plane %u overlay skipped: shared possible_mask=0x%x\n",
                       candidate, plane.possible_crtcs);
            continue;
        }
        if (options->object_count >= MAX_LEASE_OBJECTS)
            break;
        options->objects[options->object_count++] = candidate;
        printf("AUTO EXTRA PLANE: id=%u type=%s possible_mask=0x%x state=idle\n",
               candidate, type == 2 ? "cursor" : "overlay-exclusive",
               plane.possible_crtcs);
    }
    printf("lease plane count=%u (primary plus safe extras)\n",
           options->object_count - 2U);
    return 0;
}

static int capture_external_display_ids(int *ids, size_t capacity,
                                             size_t *count_out, int verbose)
{
    int descriptors[2];
    pid_t child;
    char output[4096];
    size_t used = 0, count = 0;
    int status = 0;

    if (pipe(descriptors) != 0)
        return -1;
    child = fork();
    if (child < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        return -1;
    }
    if (child == 0) {
        char *const command[] = {
            "/system/bin/cmd", "display", "get-displays",
            "--ids-only", "--type", "external", NULL
        };
        close(descriptors[0]);
        (void)dup2(descriptors[1], STDOUT_FILENO);
        (void)dup2(descriptors[1], STDERR_FILENO);
        close(descriptors[1]);
        execv(command[0], command);
        _exit(127);
    }
    close(descriptors[1]);
    while (used + 1 < sizeof(output)) {
        ssize_t amount = read(descriptors[0], output + used,
                              sizeof(output) - used - 1);
        if (amount > 0) {
            used += (size_t)amount;
            continue;
        }
        if (amount < 0 && errno == EINTR)
            continue;
        break;
    }
    close(descriptors[0]);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR)
        ;
    output[used] = '\0';
    if (verbose)
        printf("external display query: %s", output[0] ? output : "(none)\n");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -1;
    {
        const char *cursor = output;
        while (*cursor) {
            char *end;
            long value;
            while (*cursor && (*cursor < '0' || *cursor > '9'))
                ++cursor;
            if (!*cursor)
                break;
            errno = 0;
            value = strtol(cursor, &end, 10);
            if (!errno && end != cursor && value >= 0 && value <= INT_MAX) {
                size_t index;
                int duplicate = 0;
                for (index = 0; index < count; ++index)
                    if (ids[index] == (int)value)
                        duplicate = 1;
                if (!duplicate) {
                    if (count == capacity)
                        return -1;
                    ids[count++] = (int)value;
                }
            }
            cursor = end != cursor ? end : cursor + 1;
        }
    }
    *count_out = count;
    return 0;
}

static int run_display_action(const char *action, int display_id)
{
    char id_text[32];
    char command_text[128];
    pid_t child;
    int status = 0;
    snprintf(id_text, sizeof(id_text), "%d", display_id);
    if (snprintf(command_text, sizeof(command_text),
                 "/system/bin/cmd display %s %s", action, id_text) >=
        (int)sizeof(command_text))
        return -1;
    printf("cmd display %s %s\n", action, id_text);
    child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        char *const command[] = {
            "/system/bin/su", "2000", "-c", command_text, NULL
        };
        /* Samsung's DisplayManager accepts KernelSU's `su 2000` command path,
         * while a plain setuid(2000) child still receives FAILED_TRANSACTION.
         * Use the same narrowly scoped shell-identity transition verified over
         * ADB; the DRM handoff parent remains root with full capabilities. */
        execv(command[0], command);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("cmd display %s failed status=0x%x\n", action, status);
        return -1;
    }
    return 0;
}

static void remember_physical_dp_display_id(int display_id)
{
    const char *path = getenv("DRM_HANDOFF_DISPLAY_STATE");
    char value[32];
    int descriptor, length;

    if (!path || !path[0] || display_id < 0)
        return;
    descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        printf("WARNING: cannot record Android DP Display ID at %s errno=%d (%s)\n",
               path, errno, strerror(errno));
        return;
    }
    length = snprintf(value, sizeof(value), "%d\n", display_id);
    if (length <= 0 || write(descriptor, value, (size_t)length) != length)
        printf("WARNING: failed to record Android DP Display ID at %s errno=%d (%s)\n",
               path, errno, strerror(errno));
    else
        printf("ANDROID DP DISPLAY ID RECORDED: id=%d path=%s\n",
               display_id, path);
    close(descriptor);
}

static int observe_android_dp_ownership(
    int fd, const struct resources *resources,
    uint32_t connector_id, uint32_t crtc_id,
    const struct handoff_options *options,
    int *occupied, int *display_id)
{
    const struct timespec delay = {0, 250000000L};
    int ids[16];
    size_t count = 0;
    int iteration, saw_planes = 0, display_query_works = 0;

    *occupied = 0;
    *display_id = -1;
    puts("observing KMS planes and Android external Display IDs for five seconds");
    for (iteration = 0; iteration < 20; ++iteration) {
        struct path_state current;
        if (read_path_state(fd, resources, connector_id, crtc_id,
                                &current) != 0)
            return -1;
        if (current.planes_on_crtc != 0)
            saw_planes = 1;

        count = 0;
        if (capture_external_display_ids(ids, 16, &count, 0) == 0) {
            display_query_works = 1;
            if (count > 1) {
                printf("REFUSE: ambiguous Android external-display ownership; found %zu Display IDs\n",
                       count);
                return -1;
            }
        }
        if (count == 1) {
            *display_id = ids[0];
            *occupied = 1;
            printf("ANDROID EXTERNAL DISPLAY OWNER: id=%d\n", *display_id);
            return 0;
        }
        nanosleep(&delay, NULL);
    }

    if (!saw_planes) {
        puts("ANDROID OWNERSHIP ABSENT: no scanout plane and no external Display ID observed");
        if (!display_query_works)
            puts("note: Android display query is unavailable; idle KMS plane state is authoritative");
        return 0;
    }

    *occupied = 1;
    if (options->display_id >= 0 && display_query_works) {
        *display_id = options->display_id;
        printf("ANDROID KMS OWNER: scanout planes observed; guarded explicit Display ID=%d\n",
               *display_id);
        return 0;
    }
    puts("REFUSE: Android scanout ownership was observed but no unique external Display ID can be detached");
    return -1;
}

static int wait_for_physical_edid(const char *status_path)
{
    const struct timespec delay = {0, 250000000L};
    static const unsigned char header[8] = {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00
    };
    char edid_path[PATH_MAX];
    size_t length;
    int iteration;
    unsigned char edid[1024];

    length = strlen(status_path);
    if (length < 7 || strcmp(status_path + length - 7, "/status") != 0 ||
        length - 7 + 6 >= sizeof(edid_path))
        return -1;
    memcpy(edid_path, status_path, length - 7);
    memcpy(edid_path + length - 7, "/edid", 6);

    puts("waiting for a physical sink EDID (forced fallback modes are not sufficient)");
    for (iteration = 0; iteration < 80; ++iteration) {
        int descriptor = open(edid_path, O_RDONLY | O_CLOEXEC);
        ssize_t amount = -1;
        if (descriptor >= 0) {
            amount = read(descriptor, edid, sizeof(edid));
            close(descriptor);
        }
        if (amount >= 128 && memcmp(edid, header, sizeof(header)) == 0) {
            printf("PHYSICAL EDID READY: path=%s bytes=%zd\n",
                   edid_path, amount);
            return 0;
        }
        if (iteration == 8) {
            puts("physical EDID still absent; requesting a standard DRM detect reprobe");
            if (write_connector_force(status_path, "detect") != 0)
                puts("WARNING: DRM detect reprobe request failed");
        }
        nanosleep(&delay, NULL);
    }
    printf("REFUSE: physical DP EDID absent at %s; forced fallback timing has no reliable vblank events\n",
           edid_path);
    return -1;
}

static int wait_for_active_quiescence(
    int fd, const struct resources *resources,
    uint32_t connector_id, uint32_t crtc_id)
{
    const struct timespec delay = {0, 250000000L};
    struct path_state current, previous;
    int iteration, consecutive_quiet = 0, have_previous = 0;
    memset(&previous, 0, sizeof(previous));

    puts("waiting five seconds for Android/HWC teardown while preserving timing");
    for (iteration = 0; iteration < 80; ++iteration) {
        if (read_path_state(fd, resources, connector_id, crtc_id,
                                &current) != 0)
            return -1;
        if (!have_previous || memcmp(&current, &previous, sizeof(current)) != 0) {
            print_path_state("post-disable KMS", &current);
            previous = current;
            have_previous = 1;
        }
        if (iteration < 20) {
            consecutive_quiet = 0;
        } else if (current.connected && current.connector_encoder &&
                   current.connector_crtc == crtc_id &&
                   current.crtc_mode_valid && current.planes_on_crtc == 0) {
            if (++consecutive_quiet >= 12) {
                puts("ACTIVE-VBLANK HANDOFF READY: timing active, all scanout planes quiet for 3 seconds");
                return 0;
            }
        } else {
            consecutive_quiet = 0;
        }
        nanosleep(&delay, NULL);
    }
    puts("REFUSE: could not preserve active timing with quiet planes within 20 seconds");
    return -1;
}

static int physical_sink_present_now(const char *status_path);

static void wait_for_restore(const char *status_path)
{
    const struct timespec delay = {0, 250000000L};
    int iteration;
    for (iteration = 0; iteration < 40; ++iteration) {
        if (physical_sink_present_now(status_path)) {
            puts("Android DP physical state restored: connected with valid EDID");
            return;
        }
        nanosleep(&delay, NULL);
    }
    puts("WARNING: Android DP did not report connected with valid EDID within 10 seconds");
}

static int physical_sink_present_now(const char *status_path)
{
    static const unsigned char edid_header[8] = {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00
    };
    char edid_path[PATH_MAX], status[64];
    unsigned char header[8];
    size_t length = strlen(status_path);
    FILE *file;
    int descriptor;

    file = fopen(status_path, "r");
    if (!file)
        return 0;
    status[0] = '\0';
    if (!fgets(status, sizeof(status), file)) {
        fclose(file);
        return 0;
    }
    fclose(file);
    if (strncmp(status, "connected", 9) != 0 ||
        length < 7 || strcmp(status_path + length - 7, "/status") != 0 ||
        length - 7 + 6 >= sizeof(edid_path))
        return 0;
    memcpy(edid_path, status_path, length - 7);
    memcpy(edid_path + length - 7, "/edid", 6);
    descriptor = open(edid_path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return 0;
    length = (size_t)read(descriptor, header, sizeof(header));
    close(descriptor);
    return length == sizeof(header) &&
           memcmp(header, edid_header, sizeof(header)) == 0;
}

static int run_inherited_child_with_hotplug_monitor(
    char **argv, const struct handoff_options *options,
    int lease_fd, uint32_t lessee_id, const char *status_path,
    int *physical_unplugged)
{
    const struct timespec delay = {0, 250000000L};
    struct sigaction action;
    pid_t child;
    int status = 0, absent_samples = 0, terminating = 0;
    int terminate_samples = 0;

    *physical_unplugged = 0;
    child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        if (setpgid(0, 0) != 0)
            _exit(125);
        if (prepare_lease_environment(lease_fd, lessee_id, options) != 0)
            _exit(126);
        printf("EXEC handoff: lease_fd=%d lessee_id=%u program=%s\n",
               lease_fd, lessee_id, argv[options->exec_index]);
        execvp(argv[options->exec_index], &argv[options->exec_index]);
        printf("execvp failed errno=%d (%s)\n", errno, strerror(errno));
        _exit(127);
    }
    (void)setpgid(child, child);
    handoff_child_pid = child;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handoff_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    for (;;) {
        pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child)
            break;
        if (waited < 0 && errno != EINTR) {
            handoff_child_pid = -1;
            return -1;
        }
        if (!terminating) {
            if (physical_sink_present_now(status_path)) {
                absent_samples = 0;
            } else if (++absent_samples >= 4) {
                puts("PHYSICAL DP HOT-UNPLUG: connector/EDID absent for 1 second");
                puts("stopping Debian renderer and revoking its DRM lease");
                *physical_unplugged = 1;
                terminating = 1;
                (void)kill(-child, SIGTERM);
            }
        } else if (++terminate_samples >= 20) {
            puts("renderer did not stop within 5 seconds; sending SIGKILL");
            (void)kill(-child, SIGKILL);
        }
        nanosleep(&delay, NULL);
    }
    handoff_child_pid = -1;
    if (WIFEXITED(status)) {
        printf("lease child exited status=%d\n", WEXITSTATUS(status));
        return *physical_unplugged ? 0 : WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        printf("lease child terminated by signal=%d\n", WTERMSIG(status));
        return *physical_unplugged ? 0 : 128 + WTERMSIG(status);
    }
    return -1;
}

#ifndef ACTIVE_HANDOFF_MAIN
#define ACTIVE_HANDOFF_MAIN main
#endif

int ACTIVE_HANDOFF_MAIN(int argc, char **argv)
{
    struct handoff_options options;
    struct stat card0;
    struct resources resources;
    struct path_state initial_state;
    uint32_t connector_id = 0, crtc_id = 0, lessee_id = 0;
    int master_pid = -1, pidfd = -1, master_fd = -1, lease_fd = -1;
    int display_id = -1, display_disabled = 0, connector_touched = 0;
    int restore_android = 0, restore_idle_connector = 0;
    int android_occupied = 0, physical_unplugged = 0, result = 1;
    char connector_status_path[PATH_MAX];

    setvbuf(stdout, NULL, _IONBF, 0);
    puts("START automatic active-vblank Android/DeX to Debian DRM lease launcher");
    memset(&resources, 0, sizeof(resources));
    connector_status_path[0] = '\0';
    if (parse_handoff_options(argc, argv, &options) != 0) {
        handoff_usage(argv[0]);
        goto done;
    }
    printf("rootfs=%s display=%s restore=%s objects=",
           options.rootfs, options.display_id >= 0 ? "auto+fallback" : "auto",
           options.keep_display_disabled ? "no" : "yes");
    for (uint32_t index = 0; index < options.object_count; ++index)
        printf("%u%s", options.objects[index],
               index + 1U == options.object_count ? "" : ",");
    putchar('\n');

    if (ensure_rootfs_mounts(options.rootfs) != 0)
        goto done;
    if (stat("/dev/dri/card0", &card0) != 0 || !S_ISCHR(card0.st_mode)) {
        printf("/dev/dri/card0 unavailable errno=%d (%s)\n",
               errno, strerror(errno));
        goto done;
    }
    puts("\n== Automatic DRM master discovery ==");
    if (auto_find_master(&card0, &master_pid, &pidfd, &master_fd) != 0 ||
        load_resources(master_fd, &resources) != 0 ||
        select_path_ids(&resources, &options,
                            &connector_id, &crtc_id) != 0)
        goto done;
    if (read_path_state(master_fd, &resources, connector_id, crtc_id,
                            &initial_state) != 0)
        goto done;
    print_path_state("initial KMS", &initial_state);
    if (resolve_connector_status_path(master_fd, connector_id,
                                          connector_status_path,
                                          sizeof(connector_status_path)) != 0)
        goto done;
    if (observe_android_dp_ownership(master_fd, &resources,
                                         connector_id, crtc_id, &options,
                                         &android_occupied, &display_id) != 0)
        goto done;
    if (android_occupied && display_id >= 0)
        remember_physical_dp_display_id(display_id);
    restore_android = android_occupied && !options.keep_display_disabled;
    restore_idle_connector = !android_occupied && initial_state.connected &&
                             !options.keep_display_disabled;

    if (android_occupied) {
        puts("\n== Android DP ownership detected ==");
        puts("DETACH POLICY: Android display disable is required because scanout ownership is present");
        if (wait_for_physical_edid(connector_status_path) != 0)
            goto done;

        puts("\n== Logical Android display disable, physical timing retained ==");
        if (run_display_action("disable-display", display_id) != 0)
            goto done;
        display_disabled = 1;
        if (wait_for_active_quiescence(master_fd, &resources,
                                           connector_id, crtc_id) != 0)
            goto done;
    } else {
        puts("\n== Android DP ownership not detected ==");
        puts("DETACH SKIPPED: selected connector/CRTC/planes are already idle");
        if (!initial_state.connected ||
            wait_for_physical_edid(connector_status_path) != 0) {
            puts("REFUSE: an idle lease requires a connected physical sink with valid EDID");
            goto done;
        }
    }
    if (auto_select_atomic_plane(master_fd, &resources, &options,
                                      connector_id, crtc_id) != 0)
        goto done;
    if (validate_disconnected_lease_path(master_fd, &resources, &options,
                                             connector_id, crtc_id) != 0)
        goto done;

    puts("\n== CREATE ACTIVE-VBLANK DRM LEASE ==");
    lease_fd = create_lease_fd(master_fd, &options, &lessee_id);
    if (lease_fd < 0)
        goto done;

    free_resources(&resources);
    close(master_fd);
    master_fd = -1;
    close(pidfd);
    pidfd = -1;
    printf("launching lease child with preserved DP timing (master pid was %d)\n",
           master_pid);
    result = run_inherited_child_with_hotplug_monitor(
        argv, &options, lease_fd, lessee_id, connector_status_path,
        &physical_unplugged);
    if (result < 0)
        result = 1;

done:
    free_resources(&resources);
    if (master_fd >= 0)
        close(master_fd);
    if (pidfd >= 0)
        close(pidfd);
    if (lease_fd >= 0) {
        puts("\n== Guarded active-lease teardown ==");
        if (connector_status_path[0] &&
            write_connector_force(connector_status_path, "off") == 0)
            wait_for_lease_disconnect(lease_fd, connector_id);
        else
            puts("WARNING: connector force-off failed before lease revoke");
        (void)ah_revoke_lessee(lessee_id);
        close(lease_fd);
        lease_fd = -1;
        puts("local lease fd closed");
    } else if ((display_disabled || connector_touched) &&
               connector_status_path[0]) {
        puts("\n== Failed-handoff physical cleanup ==");
        (void)write_connector_force(connector_status_path, "off");
    }
    if (restore_android && !physical_unplugged && connector_status_path[0]) {
        puts("\n== Restoring original Android DP-connected state ==");
        if (write_connector_force(connector_status_path, "detect") == 0) {
            wait_for_restore(connector_status_path);
            if (wait_for_physical_edid(connector_status_path) != 0)
                puts("WARNING: Android DP restored with fallback modes only");
            if (display_disabled && display_id >= 0) {
                if (run_display_action("enable-display", display_id) == 0)
                    puts("Android external Display ID re-enabled");
                else
                    printf("WARNING: physical DP restored but Android Display ID %d could not be re-enabled\n",
                           display_id);
            }
        } else {
            puts("WARNING: Android DP restore failed; request connector detection manually");
        }
    } else if (restore_idle_connector && !physical_unplugged &&
               connector_status_path[0]) {
        puts("\n== Restoring physical DP without Android/DeX display action ==");
        if (write_connector_force(connector_status_path, "detect") == 0) {
            wait_for_restore(connector_status_path);
            if (wait_for_physical_edid(connector_status_path) != 0)
                puts("WARNING: physical DP returned without a valid EDID");
            else
                puts("physical DP restored; no Android Display ID was enabled or disabled");
        } else {
            puts("WARNING: physical DP detect restore failed");
        }
    } else if (physical_unplugged) {
        puts("physical DP detached; clearing connector force for the next hotplug");
        if (connector_status_path[0] &&
            write_connector_force(connector_status_path, "detect") != 0)
            puts("WARNING: failed to clear connector force after physical unplug");
        puts("stale Android Display ID was not re-enabled");
    } else if ((display_disabled || connector_touched) &&
               connector_status_path[0]) {
        puts("clearing connector force after failed or non-restoring handoff");
        if (write_connector_force(connector_status_path, "detect") != 0)
            puts("WARNING: failed to clear connector force after handoff");
    }
    printf("DONE exit=%d\n", result);
    return result;
}
