// SPDX-License-Identifier: MIT
// AArch64 Android/Linux DRM lease creator with conservative preflight checks.

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 438
#endif

#define DRM_IOCTL_BASE 'd'
#define DRM_DISPLAY_MODE_LEN 32
#define MAX_OBJECTS 4096U
#define MAX_LEASE_OBJECTS 64U
#define DRM_MODE_CONNECTED 1U

struct drm_set_version {
    int drm_di_major, drm_di_minor, drm_dd_major, drm_dd_minor;
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

struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid;
    struct drm_mode_modeinfo mode;
};

struct drm_mode_get_encoder {
    uint32_t encoder_id, encoder_type, crtc_id;
    uint32_t possible_crtcs, possible_clones;
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

struct drm_mode_create_lease {
    uint64_t object_ids;
    uint32_t object_count;
    uint32_t flags;
    uint32_t lessee_id;
    uint32_t fd;
};

struct drm_mode_list_lessees {
    uint32_t count_lessees, pad;
    uint64_t lessees_ptr;
};

struct drm_mode_get_lease {
    uint32_t count_objects, pad;
    uint64_t objects_ptr;
};

_Static_assert(sizeof(struct drm_set_version) == 16, "set_version ABI");
_Static_assert(sizeof(struct drm_mode_modeinfo) == 68, "modeinfo ABI");
_Static_assert(sizeof(struct drm_mode_card_res) == 64, "resources ABI");
_Static_assert(sizeof(struct drm_mode_crtc) == 104, "crtc ABI");
_Static_assert(sizeof(struct drm_mode_get_encoder) == 20, "encoder ABI");
_Static_assert(sizeof(struct drm_mode_get_connector) == 80, "connector ABI");
_Static_assert(sizeof(struct drm_mode_get_plane_res) == 16, "plane res ABI");
_Static_assert(sizeof(struct drm_mode_get_plane) == 32, "plane ABI");
_Static_assert(sizeof(struct drm_mode_create_lease) == 24, "create lease ABI");
_Static_assert(sizeof(struct drm_mode_list_lessees) == 16, "list lessees ABI");
_Static_assert(sizeof(struct drm_mode_get_lease) == 16, "get lease ABI");

#define DRM_IOCTL_SET_VERSION \
    _IOWR(DRM_IOCTL_BASE, 0x07, struct drm_set_version)
#define DRM_IOCTL_MODE_GETRESOURCES \
    _IOWR(DRM_IOCTL_BASE, 0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC \
    _IOWR(DRM_IOCTL_BASE, 0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETENCODER \
    _IOWR(DRM_IOCTL_BASE, 0xA6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCONNECTOR \
    _IOWR(DRM_IOCTL_BASE, 0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_GETPLANERESOURCES \
    _IOWR(DRM_IOCTL_BASE, 0xB5, struct drm_mode_get_plane_res)
#define DRM_IOCTL_MODE_GETPLANE \
    _IOWR(DRM_IOCTL_BASE, 0xB6, struct drm_mode_get_plane)
#define DRM_IOCTL_MODE_CREATE_LEASE \
    _IOWR(DRM_IOCTL_BASE, 0xC6, struct drm_mode_create_lease)
#define DRM_IOCTL_MODE_LIST_LESSEES \
    _IOWR(DRM_IOCTL_BASE, 0xC7, struct drm_mode_list_lessees)
#define DRM_IOCTL_MODE_GET_LEASE \
    _IOWR(DRM_IOCTL_BASE, 0xC8, struct drm_mode_get_lease)

struct resources {
    struct drm_mode_card_res res;
    uint32_t *fbs, *crtcs, *connectors, *encoders, *planes;
    uint32_t count_planes;
};

struct options {
    int pid;
    int requested_master_fd;
    int create;
    int hold;
    char **exec_argv;
    uint32_t objects[MAX_LEASE_OBJECTS];
    uint32_t object_count;
};

static volatile sig_atomic_t stop_holding;

static uint64_t ptr64(const void *pointer)
{
    return (uint64_t)(uintptr_t)pointer;
}

static void *bounded_calloc(uint32_t count, size_t size)
{
    if (count == 0)
        return NULL;
    if (count > MAX_OBJECTS || size > SIZE_MAX / count)
        return NULL;
    return calloc(count, size);
}

static int parse_positive_int(const char *text, int *value)
{
    char *end = NULL;
    long parsed;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno || !text[0] || !end || *end || parsed <= 0 || parsed > INT_MAX)
        return -1;
    *value = (int)parsed;
    return 0;
}

static int parse_nonnegative_int(const char *text, int *value)
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

static int parse_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;
    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno || !text[0] || !end || *end || parsed == 0 || parsed > UINT32_MAX)
        return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static int compare_ints(const void *left, const void *right)
{
    int a = *(const int *)left;
    int b = *(const int *)right;
    return (a > b) - (a < b);
}

static int same_char_device(const struct stat *left, const struct stat *right)
{
    return S_ISCHR(left->st_mode) && S_ISCHR(right->st_mode) &&
           left->st_rdev == right->st_rdev;
}

static size_t scan_card0_fds(int pid, const struct stat *card0,
                             int *fds, size_t capacity)
{
    char directory_path[64];
    DIR *directory;
    struct dirent *entry;
    size_t count = 0;

    snprintf(directory_path, sizeof(directory_path), "/proc/%d/fd", pid);
    directory = opendir(directory_path);
    if (!directory) {
        printf("scan %s failed: errno=%d (%s)\n",
               directory_path, errno, strerror(errno));
        return 0;
    }
    while ((entry = readdir(directory)) != NULL) {
        char path[96];
        struct stat descriptor_stat;
        int descriptor;

        if (parse_nonnegative_int(entry->d_name, &descriptor) != 0)
            continue;
        snprintf(path, sizeof(path), "%s/%d", directory_path, descriptor);
        if (stat(path, &descriptor_stat) == 0 &&
            same_char_device(&descriptor_stat, card0)) {
            if (count == capacity)
                break;
            fds[count++] = descriptor;
        }
    }
    closedir(directory);
    qsort(fds, count, sizeof(fds[0]), compare_ints);
    printf("scan %s = %zu /dev/dri/card0 fd(s)\n", directory_path, count);
    return count;
}

/* Returns 1 for current master, 0 for not current master, -1 otherwise. */
static int master_gate(int fd)
{
    struct drm_set_version version = {-1, -1, -1, -1};
    errno = 0;
    if (ioctl(fd, DRM_IOCTL_SET_VERSION, &version) == 0)
        return 1;
    if (errno == EACCES)
        return 0;
    return -1;
}

static int duplicate_target_fd(int pidfd, int target_fd)
{
    int local_fd;
    errno = 0;
    local_fd = (int)syscall(SYS_pidfd_getfd, pidfd, target_fd, 0);
    if (local_fd < 0)
        printf("pidfd_getfd(pidfd,%d,0) failed: errno=%d (%s)\n",
               target_fd, errno, strerror(errno));
    return local_fd;
}

static int find_master(int pid, int pidfd, const struct stat *card0,
                       int requested, int *target_master_fd)
{
    int candidates[MAX_OBJECTS];
    size_t count, index;

    if (requested >= 0) {
        candidates[0] = requested;
        count = 1;
    } else {
        count = scan_card0_fds(pid, card0, candidates, MAX_OBJECTS);
    }
    for (index = 0; index < count; ++index) {
        int local_fd = duplicate_target_fd(pidfd, candidates[index]);
        int result;
        if (local_fd < 0)
            continue;
        result = master_gate(local_fd);
        if (result == 1) {
            printf("fd %d master check = CURRENT MASTER\n", candidates[index]);
            *target_master_fd = candidates[index];
            return local_fd;
        }
        if (result == 0)
            printf("fd %d master check = not current master\n", candidates[index]);
        else
            printf("fd %d master check = inconclusive errno=%d (%s)\n",
                   candidates[index], errno, strerror(errno));
        close(local_fd);
    }
    return -1;
}

static void free_resources(struct resources *resources)
{
    free(resources->fbs);
    free(resources->crtcs);
    free(resources->connectors);
    free(resources->encoders);
    free(resources->planes);
    memset(resources, 0, sizeof(*resources));
}

static int load_resources(int fd, struct resources *resources)
{
    struct drm_mode_card_res first, second;
    struct drm_mode_get_plane_res plane_first, plane_second;

    memset(&first, 0, sizeof(first));
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &first) != 0) {
        printf("GETRESOURCES failed: errno=%d (%s)\n", errno, strerror(errno));
        return -1;
    }
    if (first.count_fbs > MAX_OBJECTS || first.count_crtcs > MAX_OBJECTS ||
        first.count_connectors > MAX_OBJECTS || first.count_encoders > MAX_OBJECTS)
        return -1;

    resources->fbs = bounded_calloc(first.count_fbs, sizeof(uint32_t));
    resources->crtcs = bounded_calloc(first.count_crtcs, sizeof(uint32_t));
    resources->connectors = bounded_calloc(first.count_connectors, sizeof(uint32_t));
    resources->encoders = bounded_calloc(first.count_encoders, sizeof(uint32_t));
    if ((first.count_fbs && !resources->fbs) ||
        (first.count_crtcs && !resources->crtcs) ||
        (first.count_connectors && !resources->connectors) ||
        (first.count_encoders && !resources->encoders))
        return -1;

    second = first;
    second.fb_id_ptr = ptr64(resources->fbs);
    second.crtc_id_ptr = ptr64(resources->crtcs);
    second.connector_id_ptr = ptr64(resources->connectors);
    second.encoder_id_ptr = ptr64(resources->encoders);
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &second) != 0 ||
        second.count_fbs > first.count_fbs || second.count_crtcs > first.count_crtcs ||
        second.count_connectors > first.count_connectors ||
        second.count_encoders > first.count_encoders) {
        printf("GETRESOURCES second pass failed/raced: errno=%d (%s)\n",
               errno, strerror(errno));
        return -1;
    }
    resources->res = second;

    memset(&plane_first, 0, sizeof(plane_first));
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &plane_first) != 0 ||
        plane_first.count_planes > MAX_OBJECTS) {
        printf("GETPLANERESOURCES failed: errno=%d (%s)\n", errno, strerror(errno));
        return -1;
    }
    resources->planes = bounded_calloc(plane_first.count_planes, sizeof(uint32_t));
    if (plane_first.count_planes && !resources->planes)
        return -1;
    plane_second = plane_first;
    plane_second.plane_id_ptr = ptr64(resources->planes);
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &plane_second) != 0 ||
        plane_second.count_planes > plane_first.count_planes) {
        printf("GETPLANERESOURCES second pass failed/raced: errno=%d (%s)\n",
               errno, strerror(errno));
        return -1;
    }
    resources->count_planes = plane_second.count_planes;
    printf("visible objects: connectors=%u crtcs=%u planes=%u existing_fbs=%u\n",
           second.count_connectors, second.count_crtcs,
           resources->count_planes, second.count_fbs);
    return 0;
}

static int index_of(const uint32_t *ids, uint32_t count, uint32_t wanted)
{
    uint32_t index;
    for (index = 0; index < count; ++index)
        if (ids[index] == wanted)
            return (int)index;
    return -1;
}

static int get_connector(int fd, uint32_t connector_id,
                         struct drm_mode_get_connector *connector,
                         uint32_t **encoder_ids)
{
    struct drm_mode_get_connector first, second;
    struct drm_mode_modeinfo sentinel;
    uint32_t encoder_capacity;

    memset(&sentinel, 0, sizeof(sentinel));
    memset(&first, 0, sizeof(first));
    first.connector_id = connector_id;
    first.count_modes = 1;
    first.modes_ptr = ptr64(&sentinel);
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &first) != 0)
        return -1;
    if (first.count_encoders > MAX_OBJECTS)
        return -1;
    encoder_capacity = first.count_encoders;
    *encoder_ids = bounded_calloc(encoder_capacity, sizeof(uint32_t));
    if (encoder_capacity && !*encoder_ids)
        return -1;

    memset(&second, 0, sizeof(second));
    second.connector_id = connector_id;
    second.count_modes = 1;
    second.modes_ptr = ptr64(&sentinel);
    second.count_encoders = encoder_capacity;
    second.encoders_ptr = ptr64(*encoder_ids);
    /* Properties and complete mode data are intentionally not requested. */
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &second) != 0 ||
        second.count_encoders > encoder_capacity) {
        free(*encoder_ids);
        *encoder_ids = NULL;
        return -1;
    }
    *connector = second;
    return 0;
}

static int connector_possible_crtcs(int fd,
                                    const struct drm_mode_get_connector *connector,
                                    const uint32_t *encoder_ids,
                                    uint32_t *possible)
{
    uint32_t index;
    *possible = 0;
    for (index = 0; index < connector->count_encoders; ++index) {
        struct drm_mode_get_encoder encoder;
        memset(&encoder, 0, sizeof(encoder));
        encoder.encoder_id = encoder_ids[index];
        if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &encoder) != 0)
            return -1;
        *possible |= encoder.possible_crtcs;
    }
    return connector->count_encoders ? 0 : -1;
}

static int preflight(int fd, const struct resources *resources,
                     const uint32_t *objects, uint32_t object_count)
{
    uint32_t connector_id = 0, crtc_id = 0;
    uint32_t plane_ids[MAX_LEASE_OBJECTS];
    uint32_t plane_count = 0, index;
    int crtc_index;
    struct drm_mode_get_connector connector;
    struct drm_mode_crtc crtc;
    uint32_t *connector_encoders = NULL;
    uint32_t possible_crtcs = 0;

    puts("\n== Preflight ==");
    for (index = 0; index < object_count; ++index) {
        uint32_t id = objects[index];
        uint32_t earlier;
        for (earlier = 0; earlier < index; ++earlier)
            if (objects[earlier] == id) {
                printf("REFUSE: duplicate object id %u\n", id);
                return -1;
            }
        if (index_of(resources->connectors, resources->res.count_connectors, id) >= 0) {
            if (connector_id) {
                puts("REFUSE: this tool requires exactly one connector per lease");
                return -1;
            }
            connector_id = id;
        } else if (index_of(resources->crtcs, resources->res.count_crtcs, id) >= 0) {
            if (crtc_id) {
                puts("REFUSE: this tool requires exactly one CRTC per lease");
                return -1;
            }
            crtc_id = id;
        } else if (index_of(resources->planes, resources->count_planes, id) >= 0) {
            plane_ids[plane_count++] = id;
        } else {
            printf("REFUSE: object id %u is not a visible connector/CRTC/plane\n", id);
            return -1;
        }
    }
    if (!connector_id || !crtc_id || !plane_count) {
        puts("REFUSE: lease needs one connector, one CRTC, and at least one plane");
        return -1;
    }

    crtc_index = index_of(resources->crtcs, resources->res.count_crtcs, crtc_id);
    if (crtc_index < 0 || crtc_index >= 32) {
        puts("REFUSE: CRTC index cannot be represented in possible_crtcs mask");
        return -1;
    }

    if (get_connector(fd, connector_id, &connector, &connector_encoders) != 0) {
        printf("REFUSE: GETCONNECTOR %u failed errno=%d (%s)\n",
               connector_id, errno, strerror(errno));
        return -1;
    }
    printf("connector %u: status=%u current_encoder=%u encoders=%u\n",
           connector_id, connector.connection, connector.encoder_id,
           connector.count_encoders);
    if (connector.connection != DRM_MODE_CONNECTED || connector.encoder_id != 0) {
        puts("REFUSE: connector is disconnected/unknown or already in use");
        free(connector_encoders);
        return -1;
    }
    if (connector_possible_crtcs(fd, &connector, connector_encoders,
                                 &possible_crtcs) != 0 ||
        !(possible_crtcs & (1U << (unsigned int)crtc_index))) {
        puts("REFUSE: connector is not compatible with selected CRTC");
        free(connector_encoders);
        return -1;
    }
    free(connector_encoders);

    memset(&crtc, 0, sizeof(crtc));
    crtc.crtc_id = crtc_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) != 0) {
        printf("REFUSE: GETCRTC %u failed errno=%d (%s)\n",
               crtc_id, errno, strerror(errno));
        return -1;
    }
    printf("CRTC %u: mode_valid=%u fb=%u\n", crtc_id, crtc.mode_valid, crtc.fb_id);
    if (crtc.mode_valid || crtc.fb_id) {
        puts("REFUSE: CRTC has a configured mode or framebuffer");
        return -1;
    }

    for (index = 0; index < plane_count; ++index) {
        struct drm_mode_get_plane plane;
        memset(&plane, 0, sizeof(plane));
        plane.plane_id = plane_ids[index];
        if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane) != 0) {
            printf("REFUSE: GETPLANE %u failed errno=%d (%s)\n",
                   plane_ids[index], errno, strerror(errno));
            return -1;
        }
        printf("plane %u: current_crtc=%u fb=%u possible_mask=0x%x\n",
               plane.plane_id, plane.crtc_id, plane.fb_id, plane.possible_crtcs);
        if (plane.crtc_id || plane.fb_id ||
            !(plane.possible_crtcs & (1U << (unsigned int)crtc_index))) {
            puts("REFUSE: plane is in use or incompatible with selected CRTC");
            return -1;
        }
    }
    printf("PREFLIGHT PASS: connector=%u CRTC=%u planes=", connector_id, crtc_id);
    for (index = 0; index < plane_count; ++index)
        printf("%u%s", plane_ids[index], index + 1 == plane_count ? "" : ",");
    putchar('\n');
    return 0;
}

static int list_lessees(int fd, const char *label)
{
    struct drm_mode_list_lessees request;
    uint64_t *ids = NULL;
    uint32_t capacity, index;

    memset(&request, 0, sizeof(request));
    if (ioctl(fd, DRM_IOCTL_MODE_LIST_LESSEES, &request) != 0) {
        printf("%s: LIST_LESSEES failed errno=%d (%s)\n",
               label, errno, strerror(errno));
        return -1;
    }
    capacity = request.count_lessees;
    if (capacity > MAX_OBJECTS)
        return -1;
    ids = bounded_calloc(capacity, sizeof(uint64_t));
    request.count_lessees = capacity;
    request.lessees_ptr = ptr64(ids);
    if (capacity && ioctl(fd, DRM_IOCTL_MODE_LIST_LESSEES, &request) != 0) {
        free(ids);
        return -1;
    }
    printf("%s: lessees=%u", label, request.count_lessees);
    for (index = 0; index < request.count_lessees && index < capacity; ++index)
        printf(" %llu", (unsigned long long)ids[index]);
    putchar('\n');
    free(ids);
    return 0;
}

static int verify_lease_fd(int lease_fd, const uint32_t *expected,
                           uint32_t expected_count)
{
    struct drm_mode_get_lease request;
    uint32_t *objects = NULL;
    uint32_t capacity, index;

    memset(&request, 0, sizeof(request));
    if (ioctl(lease_fd, DRM_IOCTL_MODE_GET_LEASE, &request) != 0) {
        printf("GET_LEASE failed errno=%d (%s)\n", errno, strerror(errno));
        return -1;
    }
    capacity = request.count_objects;
    if (capacity > MAX_OBJECTS)
        return -1;
    objects = bounded_calloc(capacity, sizeof(uint32_t));
    request.count_objects = capacity;
    request.objects_ptr = ptr64(objects);
    if (capacity && ioctl(lease_fd, DRM_IOCTL_MODE_GET_LEASE, &request) != 0) {
        free(objects);
        return -1;
    }
    printf("GET_LEASE verified objects=%u:", request.count_objects);
    for (index = 0; index < request.count_objects && index < capacity; ++index)
        printf(" %u", objects[index]);
    putchar('\n');
    if (request.count_objects != expected_count) {
        puts("verification warning: returned object count differs from request");
        free(objects);
        return -1;
    }
    for (index = 0; index < expected_count; ++index)
        if (index_of(objects, request.count_objects, expected[index]) < 0) {
            printf("verification warning: requested object %u missing\n", expected[index]);
            free(objects);
            return -1;
        }
    free(objects);
    return 0;
}

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_holding = 1;
}

static int create_lease(int master_fd, const struct options *options)
{
    struct drm_mode_create_lease request;
    int lease_fd;

    memset(&request, 0, sizeof(request));
    request.object_ids = ptr64(options->objects);
    request.object_count = options->object_count;
    request.flags = O_CLOEXEC;
    request.fd = UINT32_MAX;

    puts("\n== CREATE_LEASE (state-changing operation) ==");
    errno = 0;
    if (ioctl(master_fd, DRM_IOCTL_MODE_CREATE_LEASE, &request) != 0) {
        printf("CREATE_LEASE failed errno=%d (%s)\n", errno, strerror(errno));
        return -1;
    }
    lease_fd = (int)(int32_t)request.fd;
    printf("CREATE_LEASE success: lessee_id=%u lease_fd=%d owner_pid=%d\n",
           request.lessee_id, lease_fd, (int)getpid());
    (void)list_lessees(master_fd, "after create");
    if (verify_lease_fd(lease_fd, options->objects, options->object_count) != 0)
        puts("lease verification reported a mismatch");

    if (options->exec_argv) {
        char fd_text[32];
        char lessee_text[32];
        char object_text[512];
        size_t used = 0;
        uint32_t index;
        int descriptor_flags;

        snprintf(fd_text, sizeof(fd_text), "%d", lease_fd);
        snprintf(lessee_text, sizeof(lessee_text), "%u", request.lessee_id);
        object_text[0] = '\0';
        for (index = 0; index < options->object_count; ++index) {
            int written = snprintf(object_text + used, sizeof(object_text) - used,
                                   "%s%u", index ? "," : "", options->objects[index]);
            if (written < 0 || (size_t)written >= sizeof(object_text) - used)
                break;
            used += (size_t)written;
        }
        descriptor_flags = fcntl(lease_fd, F_GETFD);
        if (descriptor_flags < 0 ||
            fcntl(lease_fd, F_SETFD, descriptor_flags & ~FD_CLOEXEC) != 0 ||
            setenv("DRM_LEASE_FD", fd_text, 1) != 0 ||
            setenv("DRM_LEASE_LESSEE_ID", lessee_text, 1) != 0 ||
            setenv("DRM_LEASE_OBJECTS", object_text, 1) != 0) {
            printf("cannot prepare inherited lease fd: errno=%d (%s)\n",
                   errno, strerror(errno));
            close(lease_fd);
            (void)list_lessees(master_fd, "after failed exec preparation");
            return -1;
        }
        printf("EXEC handoff: DRM_LEASE_FD=%s DRM_LEASE_LESSEE_ID=%s program=%s\n",
               fd_text, lessee_text, options->exec_argv[0]);
        execvp(options->exec_argv[0], options->exec_argv);
        printf("execvp failed errno=%d (%s)\n", errno, strerror(errno));
        close(lease_fd);
        (void)list_lessees(master_fd, "after failed exec");
        return -1;
    }

    if (options->hold) {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = handle_signal;
        sigemptyset(&action.sa_mask);
        sigaction(SIGINT, &action, NULL);
        sigaction(SIGTERM, &action, NULL);
        printf("HOLDING lease: pid=%d fd=%d; press Ctrl-C to close/revoke\n",
               (int)getpid(), lease_fd);
        while (!stop_holding)
            pause();
    } else {
        puts("test mode: closing lease fd immediately (automatic revoke)");
    }

    close(lease_fd);
    puts("lease fd closed");
    (void)list_lessees(master_fd, "after close");
    return 0;
}

static void usage(const char *program)
{
    printf("usage: %s PID [--master-fd N] [--create] [--hold] OBJECT... "
           "[--exec PROGRAM [ARG...]]\n",
           program);
    puts("  default:  preflight only; no lease is created");
    puts("  --create: create, verify, then immediately close/revoke lease");
    puts("  --hold:   with --create, hold lease until Ctrl-C");
    puts("  --exec:   create lease and exec PROGRAM with inherited DRM_LEASE_FD");
    puts("examples:");
    printf("  %s 1829 33 281 94\n", program);
    printf("  %s 1829 --create 33 281 94\n", program);
    printf("  %s 1829 --create --hold 33 281 94\n", program);
    printf("  %s 1829 33 281 94 --exec /data/local/tmp/inspect_inherited_drm_lease\n",
           program);
}

static int parse_options(int argc, char **argv, struct options *options)
{
    int index;
    memset(options, 0, sizeof(*options));
    options->requested_master_fd = -1;
    if (argc < 3 || parse_positive_int(argv[1], &options->pid) != 0)
        return -1;
    for (index = 2; index < argc; ++index) {
        if (strcmp(argv[index], "--create") == 0) {
            options->create = 1;
        } else if (strcmp(argv[index], "--hold") == 0) {
            options->hold = 1;
        } else if (strcmp(argv[index], "--master-fd") == 0) {
            if (++index == argc ||
                parse_nonnegative_int(argv[index], &options->requested_master_fd) != 0)
                return -1;
        } else if (strcmp(argv[index], "--exec") == 0) {
            if (++index == argc)
                return -1;
            options->exec_argv = &argv[index];
            options->create = 1;
            break;
        } else {
            if (options->object_count == MAX_LEASE_OBJECTS ||
                parse_u32(argv[index], &options->objects[options->object_count]) != 0)
                return -1;
            options->object_count++;
        }
    }
    if (options->object_count < 3 || (options->hold && !options->create) ||
        (options->hold && options->exec_argv))
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    struct options options;
    struct resources resources;
    struct stat card0;
    int pidfd = -1, master_fd = -1, target_master_fd = -1;
    int result = 1;

    setvbuf(stdout, NULL, _IONBF, 0);
    puts("START guarded DRM lease creator with exec handoff");
    memset(&resources, 0, sizeof(resources));

    if (parse_options(argc, argv, &options) != 0) {
        usage(argv[0]);
        goto done;
    }
    printf("target pid=%d action=%s hold=%s exec=%s objects=",
           options.pid, options.exec_argv ? "CREATE+EXEC" :
                        (options.create ? "CREATE" : "PREFLIGHT-ONLY"),
           options.hold ? "yes" : "no",
           options.exec_argv ? options.exec_argv[0] : "none");
    for (uint32_t i = 0; i < options.object_count; ++i)
        printf("%u%s", options.objects[i],
               i + 1 == options.object_count ? "" : ",");
    putchar('\n');

    if (stat("/dev/dri/card0", &card0) != 0 || !S_ISCHR(card0.st_mode)) {
        printf("/dev/dri/card0 unavailable errno=%d (%s)\n", errno, strerror(errno));
        goto done;
    }
    printf("/dev/dri/card0=char %u:%u\n", major(card0.st_rdev), minor(card0.st_rdev));
    pidfd = (int)syscall(SYS_pidfd_open, options.pid, 0);
    if (pidfd < 0) {
        printf("pidfd_open failed errno=%d (%s)\n", errno, strerror(errno));
        goto done;
    }
    if (fcntl(pidfd, F_SETFD, FD_CLOEXEC) != 0) {
        printf("cannot mark pidfd close-on-exec errno=%d (%s)\n",
               errno, strerror(errno));
        goto done;
    }
    master_fd = find_master(options.pid, pidfd, &card0,
                            options.requested_master_fd, &target_master_fd);
    if (master_fd < 0) {
        puts("no confirmed current master fd found");
        goto done;
    }
    if (fcntl(master_fd, F_SETFD, FD_CLOEXEC) != 0) {
        printf("cannot mark duplicated master fd close-on-exec errno=%d (%s)\n",
               errno, strerror(errno));
        goto done;
    }
    printf("selected target master fd=%d local_fd=%d\n", target_master_fd, master_fd);
    if (load_resources(master_fd, &resources) != 0)
        goto done;
    (void)list_lessees(master_fd, "before");
    if (preflight(master_fd, &resources, options.objects, options.object_count) != 0)
        goto done;

    if (!options.create) {
        puts("DRY RUN COMPLETE: no CREATE_LEASE ioctl was called");
        result = 0;
        goto done;
    }
    if (create_lease(master_fd, &options) != 0)
        goto done;
    result = 0;

done:
    free_resources(&resources);
    if (master_fd >= 0)
        close(master_fd);
    if (pidfd >= 0)
        close(pidfd);
    printf("DONE exit=%d\n", result);
    return result;
}
