// SPDX-License-Identifier: MIT
// off/lease/on guarded Android-to-Debian DP handoff launcher.

#define main lease_creator_embedded_main
#include "create_drm_lease.c"
#undef main

#include <sys/mount.h>
#include <sys/wait.h>
#include <time.h>

struct handoff_options {
    const char *rootfs;
    int display_id;
    int keep_display_disabled;
    const char *object_text[MAX_LEASE_OBJECTS];
    uint32_t objects[MAX_LEASE_OBJECTS];
    uint32_t object_count;
    int exec_index;
};

struct path_state {
    uint32_t connector_id;
    uint32_t crtc_id;
    unsigned int planes_on_crtc;
    uint32_t connector_encoder;
    uint32_t connector_crtc;
    uint32_t crtc_mode_valid;
    uint32_t crtc_fb;
    int connected;
    int active;
};

static volatile sig_atomic_t handoff_child_pid = -1;

static void handoff_signal(int signal_number)
{
    pid_t child = (pid_t)handoff_child_pid;
    if (child > 0)
        (void)kill(-child, signal_number);
}

static void handoff_usage(const char *program)
{
    printf("usage: %s [--rootfs PATH] [--display-id N] "
           "[--keep-display-disabled] OBJECT... --exec PROGRAM [ARG...]\n",
           program);
    puts("  default rootfs: /data/local/debian");
    puts("  --display-id is accepted for older command-line compatibility but ignored");
    puts("  automatically finds the process and fd holding DRM master");
    puts("  mounts rootfs /dev, /sys, and /proc only when missing");
    puts("  forces the selected connector off before creating the lease");
    puts("  forces it on-digital only after the connector/CRTC/planes are leased");
    puts("  restores the original connected state after the child exits unless --keep-display-disabled");
}

static int parse_handoff_options(int argc, char **argv,
                             struct handoff_options *options)
{
    int index;
    memset(options, 0, sizeof(*options));
    options->rootfs = "/data/local/debian";
    options->display_id = -1;
    options->exec_index = -1;

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--rootfs") == 0) {
            if (++index == argc || !argv[index][0])
                return -1;
            options->rootfs = argv[index];
        } else if (strcmp(argv[index], "--display-id") == 0) {
            if (++index == argc ||
                parse_nonnegative_int(argv[index], &options->display_id) != 0)
                return -1;
        } else if (strcmp(argv[index], "--keep-display-disabled") == 0) {
            options->keep_display_disabled = 1;
        } else if (strcmp(argv[index], "--exec") == 0) {
            if (++index == argc)
                return -1;
            options->exec_index = index;
            break;
        } else if (argv[index][0] == '-') {
            return -1;
        } else {
            uint32_t value;
            if (options->object_count == MAX_LEASE_OBJECTS ||
                parse_u32(argv[index], &value) != 0)
                return -1;
            options->objects[options->object_count] = value;
            options->object_text[options->object_count] = argv[index];
            ++options->object_count;
        }
    }
    return options->object_count >= 3 && options->exec_index >= 0 ? 0 : -1;
}

static int path_is_mounted(const char *target)
{
    FILE *file = fopen("/proc/self/mountinfo", "r");
    char line[8192];
    int found = 0;
    if (!file)
        return -1;
    while (fgets(line, sizeof(line), file)) {
        char mountpoint[PATH_MAX];
        if (sscanf(line, "%*u %*u %*s %*s %4095s", mountpoint) == 1 &&
            strcmp(mountpoint, target) == 0) {
            found = 1;
            break;
        }
    }
    fclose(file);
    return found;
}

static int ensure_directory(const char *path)
{
    struct stat state;
    if (stat(path, &state) == 0)
        return S_ISDIR(state.st_mode) ? 0 : -1;
    if (errno != ENOENT || mkdir(path, 0755) != 0) {
        printf("cannot create mountpoint %s errno=%d (%s)\n",
               path, errno, strerror(errno));
        return -1;
    }
    return 0;
}

static int ensure_one_mount(const char *rootfs, const char *suffix,
                                const char *source, const char *filesystem,
                                unsigned long flags)
{
    char target[PATH_MAX];
    int mounted;
    if (snprintf(target, sizeof(target), "%s%s", rootfs, suffix) >=
        (int)sizeof(target)) {
        puts("rootfs mountpoint path is too long");
        return -1;
    }
    if (ensure_directory(target) != 0)
        return -1;
    mounted = path_is_mounted(target);
    if (mounted < 0) {
        printf("cannot inspect /proc/self/mountinfo errno=%d (%s)\n",
               errno, strerror(errno));
        return -1;
    }
    if (mounted) {
        printf("mount ready: %s\n", target);
        return 0;
    }
    if (mount(source, target, filesystem, flags, NULL) != 0) {
        printf("mount failed: %s -> %s errno=%d (%s)\n",
               source, target, errno, strerror(errno));
        return -1;
    }
    printf("mount created: %s -> %s\n", source, target);
    return 0;
}

static int ensure_rootfs_mounts(const char *rootfs)
{
    char path[PATH_MAX];
    struct stat state;
    if (stat(rootfs, &state) != 0 || !S_ISDIR(state.st_mode)) {
        printf("rootfs is not a directory: %s\n", rootfs);
        return -1;
    }
    puts("\n== Rootfs mount check ==");
    if (ensure_one_mount(rootfs, "/dev", "/dev", NULL,
                             MS_BIND | MS_REC) != 0 ||
        ensure_one_mount(rootfs, "/sys", "/sys", NULL,
                             MS_BIND | MS_REC) != 0 ||
        ensure_one_mount(rootfs, "/proc", "proc", "proc", 0) != 0)
        return -1;

    /* Android commonly has no host /dev/shm.  Create a private tmpfs after
     * binding /dev so wlroots can allocate its dma-buf format table and
     * keyboard keymap even when active_drm_handoff is used directly. */
    if (ensure_one_mount(rootfs, "/dev/shm", "shm", "tmpfs",
                             MS_NOSUID | MS_NODEV) != 0 ||
        snprintf(path, sizeof(path), "%s/dev/shm", rootfs) >=
            (int)sizeof(path) || chmod(path, 01777) != 0)
        return -1;

    if (ensure_one_mount(rootfs, "/run", "run", "tmpfs",
                             MS_NOSUID | MS_NODEV) != 0)
        return -1;
    if (snprintf(path, sizeof(path), "%s/run/user", rootfs) >=
            (int)sizeof(path) || ensure_directory(path) != 0 ||
        snprintf(path, sizeof(path), "%s/run/user/0", rootfs) >=
            (int)sizeof(path) || ensure_directory(path) != 0 ||
        chmod(path, 0700) != 0 ||
        snprintf(path, sizeof(path), "%s/run/lock", rootfs) >=
            (int)sizeof(path) || ensure_directory(path) != 0 ||
        chmod(path, 0775) != 0 ||
        snprintf(path, sizeof(path), "%s/tmp", rootfs) >=
            (int)sizeof(path) || ensure_directory(path) != 0 ||
        chmod(path, 01777) != 0)
        return -1;
    puts("volatile runtime ready: /dev/shm /run/user/0 /run/lock /tmp");
    return 0;
}

static int process_has_card0(int pid, const struct stat *card0)
{
    char directory_path[64];
    DIR *directory;
    struct dirent *entry;
    int found = 0;
    snprintf(directory_path, sizeof(directory_path), "/proc/%d/fd", pid);
    directory = opendir(directory_path);
    if (!directory)
        return 0;
    while ((entry = readdir(directory)) != NULL) {
        char path[96];
        struct stat descriptor_state;
        int descriptor;
        if (parse_nonnegative_int(entry->d_name, &descriptor) != 0)
            continue;
        snprintf(path, sizeof(path), "%s/%d", directory_path, descriptor);
        if (stat(path, &descriptor_state) == 0 &&
            same_char_device(&descriptor_state, card0)) {
            found = 1;
            break;
        }
    }
    closedir(directory);
    return found;
}

static int auto_find_master(const struct stat *card0, int *pid_out,
                                int *pidfd_out, int *master_out)
{
    DIR *proc = opendir("/proc");
    struct dirent *entry;
    int best_pid = -1, best_pidfd = -1, best_master = -1;
    int best_target_fd = -1;
    uint64_t best_score = 0;
    if (!proc) {
        printf("cannot scan /proc errno=%d (%s)\n", errno, strerror(errno));
        return -1;
    }
    while ((entry = readdir(proc)) != NULL) {
        int pid, pidfd, master, target_fd = -1;
        if (parse_positive_int(entry->d_name, &pid) != 0 ||
            !process_has_card0(pid, card0))
            continue;
        pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
        if (pidfd < 0)
            continue;
        master = find_master(pid, pidfd, card0, -1, &target_fd);
        if (master >= 0) {
            struct resources candidate_resources;
            uint64_t score = 0;
            memset(&candidate_resources, 0, sizeof(candidate_resources));
            if (load_resources(master, &candidate_resources) == 0) {
                score = (uint64_t)candidate_resources.res.count_connectors * 1000000ULL +
                        (uint64_t)candidate_resources.res.count_crtcs * 1000ULL +
                        candidate_resources.count_planes;
            }
            free_resources(&candidate_resources);
            printf("master candidate pid=%d target_fd=%d score=%llu\n",
                   pid, target_fd, (unsigned long long)score);
            if (score > best_score) {
                if (best_master >= 0)
                    close(best_master);
                if (best_pidfd >= 0)
                    close(best_pidfd);
                best_pid = pid;
                best_pidfd = pidfd;
                best_master = master;
                best_target_fd = target_fd;
                best_score = score;
                continue;
            }
            close(master);
        }
        close(pidfd);
    }
    closedir(proc);
    if (best_master >= 0) {
        *pid_out = best_pid;
        *pidfd_out = best_pidfd;
        *master_out = best_master;
        printf("AUTO MASTER: pid=%d target_fd=%d local_fd=%d score=%llu\n",
               best_pid, best_target_fd, best_master,
               (unsigned long long)best_score);
        return 0;
    }
    puts("AUTO MASTER failed: no process owns a confirmed card0 master fd");
    return -1;
}

static int select_path_ids(const struct resources *resources,
                               const struct handoff_options *options,
                               uint32_t *connector_id, uint32_t *crtc_id)
{
    uint32_t index;
    *connector_id = 0;
    *crtc_id = 0;
    for (index = 0; index < options->object_count; ++index) {
        uint32_t object = options->objects[index];
        if (index_of(resources->connectors,
                     resources->res.count_connectors, object) >= 0) {
            if (*connector_id)
                return -1;
            *connector_id = object;
        } else if (index_of(resources->crtcs,
                            resources->res.count_crtcs, object) >= 0) {
            if (*crtc_id)
                return -1;
            *crtc_id = object;
        }
    }
    return *connector_id && *crtc_id ? 0 : -1;
}

static int read_path_state(int fd, const struct resources *resources,
                               uint32_t connector_id, uint32_t crtc_id,
                               struct path_state *state)
{
    struct drm_mode_get_connector connector;
    struct drm_mode_crtc crtc;
    uint32_t *encoders = NULL;
    uint32_t index;
    memset(state, 0, sizeof(*state));
    state->connector_id = connector_id;
    state->crtc_id = crtc_id;
    if (get_connector(fd, connector_id, &connector, &encoders) != 0)
        return -1;
    free(encoders);
    memset(&crtc, 0, sizeof(crtc));
    crtc.crtc_id = crtc_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) != 0)
        return -1;
    state->connected = connector.connection == DRM_MODE_CONNECTED;
    state->connector_encoder = connector.encoder_id;
    if (connector.encoder_id) {
        struct drm_mode_get_encoder encoder;
        memset(&encoder, 0, sizeof(encoder));
        encoder.encoder_id = connector.encoder_id;
        if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &encoder) != 0)
            return -1;
        state->connector_crtc = encoder.crtc_id;
    }
    state->crtc_mode_valid = crtc.mode_valid;
    state->crtc_fb = crtc.fb_id;
    for (index = 0; index < resources->count_planes; ++index) {
        struct drm_mode_get_plane plane;
        memset(&plane, 0, sizeof(plane));
        plane.plane_id = resources->planes[index];
        if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane) != 0)
            return -1;
        if (plane.crtc_id == crtc_id)
            ++state->planes_on_crtc;
    }
    /* Atomic drivers may leave a stale legacy GETCRTC fb_id while inactive. */
    state->active = state->connector_encoder || state->crtc_mode_valid ||
                    state->planes_on_crtc;
    return 0;
}

static void print_path_state(const char *label,
                                 const struct path_state *state)
{
    printf("%s: connector=%u connected=%s encoder=%u encoder_CRTC=%u "
           "CRTC=%u mode_valid=%u "
           "fb=%u attached_planes=%u state=%s\n",
           label, state->connector_id, state->connected ? "YES" : "no",
           state->connector_encoder, state->connector_crtc, state->crtc_id,
           state->crtc_mode_valid,
           state->crtc_fb, state->planes_on_crtc,
           state->active ? "ACTIVE" : "IDLE");
}

static int read_u32_file(const char *path, uint32_t *value)
{
    FILE *file = fopen(path, "r");
    char text[64];
    char *end;
    unsigned long parsed;
    if (!file)
        return -1;
    if (!fgets(text, sizeof(text), file)) {
        fclose(file);
        return -1;
    }
    fclose(file);
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || end == text || parsed > UINT32_MAX)
        return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static const char *connector_type_sysfs_name(uint32_t type)
{
    static const char *const names[] = {
        "Unknown", "VGA", "DVI-I", "DVI-D", "DVI-A", "Composite",
        "SVIDEO", "LVDS", "Component", "DIN", "DP", "HDMI-A",
        "HDMI-B", "TV", "eDP", "Virtual", "DSI", "DPI",
        "Writeback", "SPI", "USB"
    };
    return type < sizeof(names) / sizeof(names[0]) ? names[type] : NULL;
}

static int resolve_connector_status_path(int fd, uint32_t connector_id,
                                             char *output,
                                             size_t output_size)
{
    const char *sysfs_root = "/sys/class/drm";
    DIR *directory = opendir(sysfs_root);
    struct dirent *entry;
    char matched_status[PATH_MAX];
    unsigned int matches = 0;
    matched_status[0] = '\0';
    if (!directory) {
        printf("cannot scan %s errno=%d (%s)\n",
               sysfs_root, errno, strerror(errno));
        return -1;
    }
    while ((entry = readdir(directory)) != NULL) {
        char id_path[PATH_MAX];
        char status_path[PATH_MAX];
        uint32_t id;
        if (strncmp(entry->d_name, "card0-", 6) != 0)
            continue;
        if (snprintf(id_path, sizeof(id_path), "%s/%s/connector_id",
                     sysfs_root, entry->d_name) >= (int)sizeof(id_path) ||
            read_u32_file(id_path, &id) != 0 || id != connector_id)
            continue;
        if (snprintf(status_path, sizeof(status_path), "%s/%s/status",
                     sysfs_root, entry->d_name) >= (int)sizeof(status_path))
            continue;
        ++matches;
        snprintf(matched_status, sizeof(matched_status), "%s", status_path);
    }
    closedir(directory);
    if (matches == 0) {
        struct drm_mode_get_connector connector;
        uint32_t *encoders = NULL;
        const char *type_name;
        if (get_connector(fd, connector_id, &connector, &encoders) == 0) {
            free(encoders);
            type_name = connector_type_sysfs_name(connector.connector_type);
            if (type_name &&
                snprintf(matched_status, sizeof(matched_status),
                         "%s/card0-%s-%u/status", sysfs_root, type_name,
                         connector.connector_type_id) < (int)sizeof(matched_status) &&
                access(matched_status, F_OK) == 0) {
                matches = 1;
                printf("sysfs connector fallback: DRM id=%u type=%s-%u\n",
                       connector_id, type_name, connector.connector_type_id);
            }
        }
    }
    if (matches != 1) {
        printf("cannot uniquely map DRM connector %u to sysfs (matches=%u)\n",
               connector_id, matches);
        return -1;
    }
    if (snprintf(output, output_size, "%s", matched_status) >=
        (int)output_size)
        return -1;
    printf("SYSFS CONNECTOR MAP: DRM id=%u status=%s\n",
           connector_id, output);
    return 0;
}

static int write_connector_force(const char *status_path,
                                     const char *value)
{
    char text[64];
    int descriptor;
    int length;
    ssize_t written;
    length = snprintf(text, sizeof(text), "%s\n", value);
    if (length <= 0 || length >= (int)sizeof(text))
        return -1;
    descriptor = open(status_path, O_WRONLY | O_CLOEXEC);
    if (descriptor < 0) {
        printf("cannot open %s errno=%d (%s)\n",
               status_path, errno, strerror(errno));
        return -1;
    }
    errno = 0;
    written = write(descriptor, text, (size_t)length);
    close(descriptor);
    if (written != length) {
        printf("cannot write %s to %s errno=%d (%s)\n",
               value, status_path, errno, strerror(errno));
        return -1;
    }
    printf("SYSFS CONNECTOR FORCE: status=%s value=%s\n", status_path, value);
    return 0;
}

static int wait_for_forced_off(int fd, const struct resources *resources,
                                   uint32_t connector_id, uint32_t crtc_id)
{
    const struct timespec delay = {0, 250000000L};
    struct path_state current, previous;
    int iteration, consecutive_idle = 0, have_previous = 0;
    memset(&previous, 0, sizeof(previous));
    for (iteration = 0; iteration < 40; ++iteration) {
        if (read_path_state(fd, resources, connector_id, crtc_id,
                                &current) != 0)
            return -1;
        if (!have_previous || memcmp(&current, &previous, sizeof(current)) != 0) {
            print_path_state("FORCE-OFF transition", &current);
            previous = current;
            have_previous = 1;
        }
        if (!current.connected && !current.active) {
            if (++consecutive_idle >= 8) {
                puts("FORCE-OFF VERIFIED: disconnected and KMS-idle for 2 seconds");
                return 0;
            }
        } else {
            consecutive_idle = 0;
        }
        nanosleep(&delay, NULL);
    }
    puts("REFUSE: connector/CRTC did not become safely off within 10 seconds");
    return -1;
}

static int validate_disconnected_lease_path(
    int fd, const struct resources *resources,
    const struct handoff_options *options,
    uint32_t connector_id, uint32_t crtc_id)
{
    int crtc_index = index_of(resources->crtcs,
                              resources->res.count_crtcs, crtc_id);
    uint32_t index, plane_count = 0;
    if (crtc_index < 0 || crtc_index >= 32)
        return -1;
    for (index = 0; index < options->object_count; ++index) {
        uint32_t object = options->objects[index];
        if (object == connector_id || object == crtc_id)
            continue;
        if (index_of(resources->planes, resources->count_planes, object) >= 0) {
            struct drm_mode_get_plane plane;
            memset(&plane, 0, sizeof(plane));
            plane.plane_id = object;
            if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane) != 0 ||
                plane.crtc_id ||
                !(plane.possible_crtcs & (1U << (unsigned int)crtc_index))) {
                printf("REFUSE: plane %u is active or incompatible after force-off\n",
                       object);
                return -1;
            }
            printf("lease plane %u idle, possible_mask=0x%x\n",
                   object, plane.possible_crtcs);
            ++plane_count;
        } else {
            printf("REFUSE: object %u is not selected connector/CRTC/plane\n",
                   object);
            return -1;
        }
    }
    return plane_count ? 0 : -1;
}

static int create_lease_fd(int master_fd,
                               const struct handoff_options *options,
                               uint32_t *lessee_id)
{
    struct drm_mode_create_lease request;
    int lease_fd;
    memset(&request, 0, sizeof(request));
    request.object_ids = ptr64(options->objects);
    request.object_count = options->object_count;
    request.flags = O_CLOEXEC;
    request.fd = UINT32_MAX;
    puts("\n== CREATE DISCONNECTED DRM LEASE ==");
    if (ioctl(master_fd, DRM_IOCTL_MODE_CREATE_LEASE, &request) != 0) {
        printf("CREATE_LEASE failed errno=%d (%s)\n",
               errno, strerror(errno));
        return -1;
    }
    lease_fd = (int)(int32_t)request.fd;
    *lessee_id = request.lessee_id;
    printf("CREATE_LEASE success: lessee_id=%u lease_fd=%d owner_pid=%d\n",
           request.lessee_id, lease_fd, (int)getpid());
    if (verify_lease_fd(lease_fd, options->objects,
                        options->object_count) != 0) {
        close(lease_fd);
        return -1;
    }
    return lease_fd;
}

static int wait_for_lease_connector(int lease_fd, uint32_t connector_id)
{
    const struct timespec delay = {0, 250000000L};
    int iteration, consecutive_ready = 0;
    for (iteration = 0; iteration < 40; ++iteration) {
        struct drm_mode_get_connector connector;
        uint32_t *encoders = NULL;
        if (get_connector(lease_fd, connector_id, &connector, &encoders) != 0)
            return -1;
        free(encoders);
        if (iteration == 0 || connector.connection == DRM_MODE_CONNECTED)
            printf("lease connector=%u status=%u modes=%u encoder=%u\n",
                   connector_id, connector.connection,
                   connector.count_modes, connector.encoder_id);
        if (connector.connection == DRM_MODE_CONNECTED && connector.count_modes) {
            if (++consecutive_ready >= 4) {
                puts("LEASE CONNECTOR READY: connected with modes for 1 second");
                return 0;
            }
        } else {
            consecutive_ready = 0;
        }
        nanosleep(&delay, NULL);
    }
    puts("lease connector did not become ready within 10 seconds");
    return -1;
}

static void wait_for_lease_disconnect(int lease_fd, uint32_t connector_id)
{
    const struct timespec delay = {0, 250000000L};
    int iteration;
    for (iteration = 0; iteration < 20; ++iteration) {
        struct drm_mode_get_connector connector;
        uint32_t *encoders = NULL;
        if (get_connector(lease_fd, connector_id, &connector, &encoders) != 0)
            return;
        free(encoders);
        if (connector.connection != DRM_MODE_CONNECTED) {
            puts("lease connector disconnected before revoke");
            return;
        }
        nanosleep(&delay, NULL);
    }
    puts("WARNING: lease connector still reports connected before revoke");
}

static int prepare_lease_environment(
    int lease_fd, uint32_t lessee_id,
    const struct handoff_options *options)
{
    char fd_text[32], lessee_text[32], object_text[512];
    size_t used = 0;
    uint32_t index;
    int descriptor_flags = fcntl(lease_fd, F_GETFD);
    snprintf(fd_text, sizeof(fd_text), "%d", lease_fd);
    snprintf(lessee_text, sizeof(lessee_text), "%u", lessee_id);
    object_text[0] = '\0';
    for (index = 0; index < options->object_count; ++index) {
        int written = snprintf(object_text + used, sizeof(object_text) - used,
                               "%s%u", index ? "," : "",
                               options->objects[index]);
        if (written < 0 || (size_t)written >= sizeof(object_text) - used)
            return -1;
        used += (size_t)written;
    }
    if (descriptor_flags < 0 ||
        fcntl(lease_fd, F_SETFD, descriptor_flags & ~FD_CLOEXEC) != 0 ||
        setenv("DRM_LEASE_FD", fd_text, 1) != 0 ||
        setenv("DRM_LEASE_LESSEE_ID", lessee_text, 1) != 0 ||
        setenv("DRM_LEASE_OBJECTS", object_text, 1) != 0)
        return -1;
    return 0;
}

static int run_inherited_child(char **argv,
                                   const struct handoff_options *options,
                                   int lease_fd, uint32_t lessee_id)
{
    pid_t child;
    int status = 0;
    struct sigaction action;
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
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    handoff_child_pid = -1;
    if (WIFEXITED(status)) {
        printf("lease child exited status=%d\n", WEXITSTATUS(status));
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        printf("lease child terminated by signal=%d\n", WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return -1;
}

#ifndef DRM_HANDOFF_MAIN
#define DRM_HANDOFF_MAIN main
#endif

int DRM_HANDOFF_MAIN(int argc, char **argv)
{
    struct handoff_options options;
    struct stat card0;
    struct resources resources;
    struct path_state initial_state;
    uint32_t connector_id = 0, crtc_id = 0;
    uint32_t lessee_id = 0;
    int master_pid = -1, pidfd = -1, master_fd = -1;
    int lease_fd = -1;
    int restore_android = 0;
    int connector_forced_off = 0;
    char connector_status_path[PATH_MAX];
    int result = 1;

    setvbuf(stdout, NULL, _IONBF, 0);
    puts("START sysfs-off guarded Android/DeX to Debian DRM lease launcher");
    memset(&resources, 0, sizeof(resources));
    connector_status_path[0] = '\0';
    if (parse_handoff_options(argc, argv, &options) != 0) {
        handoff_usage(argv[0]);
        goto done;
    }
    printf("rootfs=%s display-id=%s restore=%s objects=",
           options.rootfs, options.display_id >= 0 ? "ignored-compat" : "unused",
           options.keep_display_disabled ? "no" : "yes");
    for (uint32_t i = 0; i < options.object_count; ++i)
        printf("%u%s", options.objects[i],
               i + 1U == options.object_count ? "" : ",");
    putchar('\n');

    if (ensure_rootfs_mounts(options.rootfs) != 0)
        goto done;
    if (stat("/dev/dri/card0", &card0) != 0 || !S_ISCHR(card0.st_mode)) {
        printf("/dev/dri/card0 unavailable errno=%d (%s)\n",
               errno, strerror(errno));
        goto done;
    }

    puts("\n== Automatic DRM master discovery ==");
    if (auto_find_master(&card0, &master_pid, &pidfd, &master_fd) != 0)
        goto done;
    if (load_resources(master_fd, &resources) != 0 ||
        select_path_ids(&resources, &options,
                            &connector_id, &crtc_id) != 0) {
        puts("cannot classify exactly one connector and one CRTC from OBJECTs");
        goto done;
    }
    if (read_path_state(master_fd, &resources, connector_id, crtc_id,
                            &initial_state) != 0) {
        printf("initial KMS state read failed errno=%d (%s)\n",
               errno, strerror(errno));
        goto done;
    }
    print_path_state("initial KMS", &initial_state);
    restore_android = initial_state.connected || initial_state.active;
    if (options.keep_display_disabled)
        restore_android = 0;
    if (options.display_id >= 0)
        printf("note: --display-id %d ignored; this mode does not call cmd display\n",
               options.display_id);
    if (resolve_connector_status_path(master_fd, connector_id,
                                          connector_status_path,
                                          sizeof(connector_status_path)) != 0)
        goto done;

    puts("\n== Guarded physical connector force-off ==");
    if (write_connector_force(connector_status_path, "off") != 0)
        goto done;
    connector_forced_off = 1;
    if (wait_for_forced_off(master_fd, &resources,
                               connector_id, crtc_id) != 0 ||
        validate_disconnected_lease_path(master_fd, &resources, &options,
                                             connector_id, crtc_id) != 0)
        goto done;

    lease_fd = create_lease_fd(master_fd, &options, &lessee_id);
    if (lease_fd < 0)
        goto done;

    free_resources(&resources);
    close(master_fd);
    master_fd = -1;
    close(pidfd);
    pidfd = -1;

    puts("\n== Lease-owned connector activation ==");
    if (write_connector_force(connector_status_path, "detect") != 0 ||
        wait_for_lease_connector(lease_fd, connector_id) != 0)
        goto done;

    printf("launching lease child (master auto-discovered at pid %d)\n",
           master_pid);
    result = run_inherited_child(argv, &options, lease_fd, lessee_id);
    if (result < 0)
        result = 1;

done:
    free_resources(&resources);
    if (master_fd >= 0)
        close(master_fd);
    if (pidfd >= 0)
        close(pidfd);
    if (lease_fd >= 0) {
        puts("\n== Guarded lease teardown ==");
        if (connector_status_path[0] &&
            write_connector_force(connector_status_path, "off") == 0)
            wait_for_lease_disconnect(lease_fd, connector_id);
        else
            puts("WARNING: connector force-off failed before lease revoke");
        close(lease_fd);
        lease_fd = -1;
        puts("lease fd closed/revoked");
    }
    if (restore_android && connector_status_path[0]) {
        puts("\n== Restoring original Android DP-connected state ==");
        if (write_connector_force(connector_status_path, "detect") != 0)
            puts("WARNING: Android DP restore failed; request connector detection manually");
    } else if (connector_forced_off) {
        puts("clearing connector force after non-restoring handoff");
        if (write_connector_force(connector_status_path, "detect") != 0)
            puts("WARNING: failed to clear connector force after handoff");
    }
    printf("DONE exit=%d\n", result);
    return result;
}
