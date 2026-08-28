#define _GNU_SOURCE

#include "../bridge/android_drm_bridge.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

struct libseat;

typedef int (*open_device_fn)(struct libseat *, const char *, int *);
typedef int (*close_device_fn)(struct libseat *, int);

enum { MAX_LEASE_DUPLICATES = 16, SYNTHETIC_ID_BASE = 0x4c530000 };

struct lease_duplicate {
    int device_id;
    int fd;
};

static pthread_mutex_t duplicate_lock = PTHREAD_MUTEX_INITIALIZER;
static struct lease_duplicate duplicates[MAX_LEASE_DUPLICATES];
static open_device_fn real_open_device;
static close_device_fn real_close_device;

static void log_message(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    fputs("DRM lease bridge: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}

static void resolve_symbols(void) {
    if (real_open_device == NULL) {
        real_open_device = (open_device_fn)dlsym(RTLD_NEXT,
                                                  "libseat_open_device");
    }
    if (real_close_device == NULL) {
        real_close_device = (close_device_fn)dlsym(RTLD_NEXT,
                                                    "libseat_close_device");
    }
}

static const char *target_drm_device(void) {
    const char *configured = getenv("DRM_LEASE_DEVICE");

    if (configured == NULL || configured[0] == '\0') {
        return "/dev/dri/card0";
    }
    return configured;
}

static bool is_target_drm_device(const char *path) {
    return strcmp(path, target_drm_device()) == 0;
}

static bool target_identity_matches(const char *path, int lease_fd) {
    struct stat requested;
    struct stat leased;

    if (fstat(lease_fd, &leased) < 0 || stat(path, &requested) < 0) {
        return false;
    }
    return S_ISCHR(leased.st_mode) && S_ISCHR(requested.st_mode) &&
           leased.st_rdev == requested.st_rdev;
}

static int parse_decimal(const char *text, uint64_t maximum,
                         const char **end_out, uint64_t *value_out) {
    const char *cursor = text;
    uint64_t value = 0;

    if (text == NULL || text[0] == '\0' || text[0] == '+' ||
        text[0] == '-') {
        return -EINVAL;
    }
    while (*cursor >= '0' && *cursor <= '9') {
        uint64_t digit = (uint64_t)(*cursor - '0');
        if (value > (maximum - digit) / 10) {
            return -ERANGE;
        }
        value = value * 10 + digit;
        ++cursor;
    }
    *end_out = cursor;
    *value_out = value;
    return 0;
}

static int parse_lease_fd(const char *text, int *fd_out) {
    const char *end = NULL;
    uint64_t parsed = 0;
    int status = parse_decimal(text, INT32_MAX, &end, &parsed);

    if (status < 0 || end == NULL || *end != '\0') {
        return status < 0 ? status : -EINVAL;
    }
    *fd_out = (int)parsed;
    return 0;
}

static int parse_expected_objects(const char *text,
                                  uint32_t objects[ADBR_MAX_LEASE_OBJECTS],
                                  size_t *count_out) {
    size_t count = 0;
    const char *cursor = text;

    if (text == NULL || text[0] == '\0' || count_out == NULL) {
        return -EINVAL;
    }
    while (*cursor != '\0') {
        const char *end = NULL;
        uint64_t value = 0;
        size_t before;
        int status;

        if (*cursor < '0' || *cursor > '9' ||
            count >= ADBR_MAX_LEASE_OBJECTS) {
            return count >= ADBR_MAX_LEASE_OBJECTS ? -E2BIG : -EINVAL;
        }
        status = parse_decimal(cursor, UINT32_MAX, &end, &value);
        if (status < 0 || end == cursor || value == 0) {
            return status < 0 ? status : -EINVAL;
        }
        for (before = 0; before < count; ++before) {
            if (objects[before] == (uint32_t)value) {
                return -EINVAL;
            }
        }
        objects[count++] = (uint32_t)value;
        if (*end == '\0') {
            break;
        }
        if (*end != ',') {
            return -EINVAL;
        }
        cursor = end + 1;
        if (*cursor == '\0') {
            return -EINVAL;
        }
    }
    *count_out = count;
    return count == 0 ? -EINVAL : 0;
}

int libseat_open_device(struct libseat *seat, const char *path, int *fd_out) {
    const char *lease_text = getenv("DRM_LEASE_FD");
    const char *objects_text = getenv("DRM_LEASE_OBJECTS");
    struct adbr_lease_info_v1 lease_info = {
        .struct_size = sizeof(lease_info),
        .abi_version = ADBR_ABI_VERSION_1,
    };
    uint32_t expected_objects[ADBR_MAX_LEASE_OBJECTS];
    size_t expected_count = 0;
    int inherited = -1;
    int duplicate_fd;
    int slot;
    int status;

    if (path == NULL || fd_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (lease_text != NULL && lease_text[0] != '\0' &&
        is_target_drm_device(path)) {
        status = parse_lease_fd(lease_text, &inherited);
        if (status == 0) {
            status = parse_expected_objects(objects_text, expected_objects,
                                            &expected_count);
        }
        if (status == 0 && !target_identity_matches(path, inherited)) {
            status = -EXDEV;
        }
        if (status == 0) {
            status = adbr_lease_dup_validate_v1(
                inherited, expected_objects, expected_count, 0,
                &duplicate_fd, &lease_info);
        }
        if (status == 0) {
            int inherited_flags = fcntl(inherited, F_GETFD);
            if (inherited_flags < 0 ||
                fcntl(inherited, F_SETFD,
                      inherited_flags | FD_CLOEXEC) != 0) {
                status = -errno;
                close(duplicate_fd);
            }
        }
        if (status < 0) {
            errno = -status;
            log_message("refused target=%s inherited-fd=%s objects=%s: %s",
                        path, lease_text,
                        objects_text != NULL ? objects_text : "(missing)",
                        strerror(errno));
            return -1;
        }

        pthread_mutex_lock(&duplicate_lock);
        for (slot = 0; slot < MAX_LEASE_DUPLICATES; ++slot) {
            if (duplicates[slot].device_id == 0) {
                duplicates[slot].device_id = SYNTHETIC_ID_BASE + slot;
                duplicates[slot].fd = duplicate_fd;
                *fd_out = duplicate_fd;
                pthread_mutex_unlock(&duplicate_lock);
                log_message("substituted %s with validated lease fd=%d "
                            "duplicate=%d objects=%u device_id=%d",
                            path, inherited, duplicate_fd,
                            lease_info.object_count,
                            SYNTHETIC_ID_BASE + slot);
                return SYNTHETIC_ID_BASE + slot;
            }
        }
        pthread_mutex_unlock(&duplicate_lock);
        close(duplicate_fd);
        errno = EMFILE;
        log_message("duplicate table is full");
        return -1;
    }

    resolve_symbols();
    if (real_open_device == NULL) {
        errno = ENOSYS;
        log_message("could not resolve real libseat_open_device");
        return -1;
    }
    return real_open_device(seat, path, fd_out);
}

int libseat_close_device(struct libseat *seat, int device_id) {
    int slot;
    int fd = -1;

    if (device_id >= SYNTHETIC_ID_BASE &&
        device_id < SYNTHETIC_ID_BASE + MAX_LEASE_DUPLICATES) {
        pthread_mutex_lock(&duplicate_lock);
        slot = device_id - SYNTHETIC_ID_BASE;
        if (duplicates[slot].device_id == device_id) {
            fd = duplicates[slot].fd;
            duplicates[slot].device_id = 0;
            duplicates[slot].fd = -1;
        }
        pthread_mutex_unlock(&duplicate_lock);
        if (fd >= 0) {
            /*
             * libseat_close_device releases session-manager ownership. The
             * application still owns and closes the fd returned by
             * libseat_open_device. wlroots follows that contract explicitly;
             * closing here would race its later close() against fd reuse.
             */
            log_message("released synthetic device_id=%d application-fd=%d",
                        device_id, fd);
            return 0;
        }
        errno = ENOENT;
        return -1;
    }

    resolve_symbols();
    if (real_close_device == NULL) {
        errno = ENOSYS;
        log_message("could not resolve real libseat_close_device");
        return -1;
    }
    return real_close_device(seat, device_id);
}
