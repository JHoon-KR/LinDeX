#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/major.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <unistd.h>

/*
 * Session-scoped USB input ownership for the Debian compositor.
 *
 * The library deliberately grabs only event nodes that are both below a USB
 * device and identifiable as a keyboard, mouse, or touchpad. EVIOCGRAB is
 * issued on the descriptor returned to the compositor, so libinput continues
 * to consume that descriptor while Android's InputReader no longer receives
 * the same events. The kernel releases the grab when the final duplicate of
 * that open file description is closed.
 */

#define ARRAY_LENGTH(a) (sizeof(a) / sizeof((a)[0]))
#define BITS_PER_ULONG (sizeof(unsigned long) * 8U)
#define BIT_ARRAY_LENGTH(maximum) (((maximum) / BITS_PER_ULONG) + 1U)

enum input_kind {
    INPUT_KIND_NONE = 0,
    INPUT_KIND_KEYBOARD,
    INPUT_KIND_MOUSE,
    INPUT_KIND_TOUCHPAD,
};

struct udev_tags {
    bool bus_usb;
    bool keyboard;
    bool mouse;
    bool touchpad;
    bool touchscreen;
};

typedef int (*open_fn)(const char *, int, ...);
typedef int (*openat_fn)(int, const char *, int, ...);

static open_fn next_open;
static open_fn next_open64;
static openat_fn next_openat;
static openat_fn next_openat64;
static __thread bool hook_active;
static int exclusive_mode = -1;
static int debug_mode = -1;

static bool bit_is_set(unsigned int bit, const unsigned long *bits,
                       size_t words)
{
    const size_t word = bit / BITS_PER_ULONG;

    if (word >= words)
        return false;
    return (bits[word] & (1UL << (bit % BITS_PER_ULONG))) != 0;
}

static bool environment_truthy(const char *name)
{
    const char *value = getenv(name);

    return value != NULL &&
           (strcmp(value, "1") == 0 || strcmp(value, "yes") == 0 ||
            strcmp(value, "true") == 0 || strcmp(value, "on") == 0);
}

static bool input_grab_enabled(void)
{
    const char *mode;

    if (exclusive_mode >= 0)
        return exclusive_mode != 0;

    mode = getenv("ANDROID_USB_INPUT_MODE");
    exclusive_mode = mode != NULL &&
                     (strcmp(mode, "linux-exclusive") == 0 ||
                      strcmp(mode, "exclusive") == 0);
    return exclusive_mode != 0;
}

static bool input_grab_debug_enabled(void)
{
    if (debug_mode < 0)
        debug_mode = environment_truthy("ANDROID_USB_INPUT_GRAB_DEBUG");
    return debug_mode != 0;
}

static const char *kind_name(enum input_kind kind)
{
    switch (kind) {
    case INPUT_KIND_KEYBOARD:
        return "keyboard";
    case INPUT_KIND_MOUSE:
        return "mouse";
    case INPUT_KIND_TOUCHPAD:
        return "touchpad";
    default:
        return "ignored";
    }
}

static void debug_event(int fd, enum input_kind kind, const char *result,
                        int error_number)
{
    char name[128] = "unknown";
    char message[384];
    int length;

    if (!input_grab_debug_enabled())
        return;

    (void)ioctl(fd, EVIOCGNAME(sizeof(name)), name);
    length = snprintf(message, sizeof(message),
                      "android-usb-input-grab: fd=%d kind=%s device=\"%s\" "
                      "result=%s errno=%d\n",
                      fd, kind_name(kind), name, result, error_number);
    if (length > 0) {
        size_t count = (size_t)length;
        ssize_t written;
        if (count >= sizeof(message))
            count = sizeof(message) - 1U;
        written = write(STDERR_FILENO, message, count);
        (void)written;
    }
}

static void resolve_symbols(void)
{
    if (next_open == NULL)
        *(void **)(&next_open) = dlsym(RTLD_NEXT, "open");
    if (next_open64 == NULL)
        *(void **)(&next_open64) = dlsym(RTLD_NEXT, "open64");
    if (next_openat == NULL)
        *(void **)(&next_openat) = dlsym(RTLD_NEXT, "openat");
    if (next_openat64 == NULL)
        *(void **)(&next_openat64) = dlsym(RTLD_NEXT, "openat64");
}

static int unhooked_open(const char *path, int flags, mode_t mode,
                         bool has_mode, bool use_largefile)
{
    open_fn function = use_largefile ? next_open64 : next_open;

    if (function != NULL)
        return has_mode ? function(path, flags, mode) : function(path, flags);
    return (int)syscall(SYS_openat, AT_FDCWD, path, flags, mode);
}

static int unhooked_openat(int directory_fd, const char *path, int flags,
                           mode_t mode, bool has_mode, bool use_largefile)
{
    openat_fn function = use_largefile ? next_openat64 : next_openat;

    if (function != NULL)
        return has_mode ? function(directory_fd, path, flags, mode)
                        : function(directory_fd, path, flags);
    return (int)syscall(SYS_openat, directory_fd, path, flags, mode);
}

static bool flags_have_mode(int flags)
{
    if ((flags & O_CREAT) != 0)
        return true;
#ifdef O_TMPFILE
    if ((flags & O_TMPFILE) == O_TMPFILE)
        return true;
#endif
    return false;
}

static bool parse_enabled_property(const char *line, const char *property)
{
    const size_t property_length = strlen(property);

    return strncmp(line, "E:", 2) == 0 &&
           strncmp(line + 2, property, property_length) == 0 &&
           strcmp(line + 2 + property_length, "=1") == 0;
}

static void read_udev_tags(dev_t device, struct udev_tags *tags)
{
    char path[128];
    char line[512];
    FILE *stream;

    (void)snprintf(path, sizeof(path), "/run/udev/data/c%u:%u",
                   major(device), minor(device));
    stream = fopen(path, "re");
    if (stream == NULL)
        return;

    while (fgets(line, sizeof(line), stream) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, "E:ID_BUS=usb") == 0)
            tags->bus_usb = true;
        else if (parse_enabled_property(line, "ID_INPUT_KEYBOARD"))
            tags->keyboard = true;
        else if (parse_enabled_property(line, "ID_INPUT_MOUSE"))
            tags->mouse = true;
        else if (parse_enabled_property(line, "ID_INPUT_TOUCHPAD"))
            tags->touchpad = true;
        else if (parse_enabled_property(line, "ID_INPUT_TOUCHSCREEN"))
            tags->touchscreen = true;
    }
    (void)fclose(stream);
}

static bool path_parent(char *path)
{
    char *slash;

    slash = strrchr(path, '/');
    if (slash == NULL || slash == path)
        return false;
    *slash = '\0';
    return true;
}

static bool sysfs_device_is_usb(dev_t device)
{
    char class_path[128];
    char current[4096];
    char subsystem_path[4096];
    char subsystem[4096];
    unsigned int depth;

    (void)snprintf(class_path, sizeof(class_path), "/sys/dev/char/%u:%u",
                   major(device), minor(device));
    if (realpath(class_path, current) == NULL)
        return false;

    for (depth = 0; depth < 32; ++depth) {
        int written = snprintf(subsystem_path, sizeof(subsystem_path),
                               "%s/subsystem", current);
        if (written > 0 && (size_t)written < sizeof(subsystem_path) &&
            realpath(subsystem_path, subsystem) != NULL) {
            const char *base = strrchr(subsystem, '/');
            if (base != NULL && strcmp(base + 1, "usb") == 0)
                return true;
        }
        if (!path_parent(current))
            break;
    }
    return false;
}

static enum input_kind classify_input_fd(int fd)
{
    struct stat status;
    struct udev_tags tags = {0};
    unsigned long event_bits[BIT_ARRAY_LENGTH(EV_MAX)] = {0};
    unsigned long key_bits[BIT_ARRAY_LENGTH(KEY_MAX)] = {0};
    unsigned long relative_bits[BIT_ARRAY_LENGTH(REL_MAX)] = {0};
    unsigned long absolute_bits[BIT_ARRAY_LENGTH(ABS_MAX)] = {0};
    unsigned long property_bits[BIT_ARRAY_LENGTH(INPUT_PROP_MAX)] = {0};
    bool has_key;
    bool has_relative;
    bool has_absolute;
    bool keyboard;
    bool mouse;
    bool touchpad;
    bool absolute_position;
    bool finger_contact;
    bool direct;

    if (fstat(fd, &status) != 0 || !S_ISCHR(status.st_mode))
        return INPUT_KIND_NONE;
    if (major(status.st_rdev) != INPUT_MAJOR)
        return INPUT_KIND_NONE;

    read_udev_tags(status.st_rdev, &tags);
    if (!tags.bus_usb && !sysfs_device_is_usb(status.st_rdev))
        return INPUT_KIND_NONE;

    if (ioctl(fd, EVIOCGBIT(0, sizeof(event_bits)), event_bits) < 0)
        return INPUT_KIND_NONE;

    has_key = bit_is_set(EV_KEY, event_bits, ARRAY_LENGTH(event_bits));
    has_relative = bit_is_set(EV_REL, event_bits, ARRAY_LENGTH(event_bits));
    has_absolute = bit_is_set(EV_ABS, event_bits, ARRAY_LENGTH(event_bits));

    if (has_key)
        (void)ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);
    if (has_relative)
        (void)ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relative_bits)),
                    relative_bits);
    if (has_absolute)
        (void)ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absolute_bits)),
                    absolute_bits);
    (void)ioctl(fd, EVIOCGPROP(sizeof(property_bits)), property_bits);

    direct = bit_is_set(INPUT_PROP_DIRECT, property_bits,
                        ARRAY_LENGTH(property_bits));
    keyboard = has_key &&
               (tags.keyboard ||
                (bit_is_set(KEY_A, key_bits, ARRAY_LENGTH(key_bits)) &&
                 bit_is_set(KEY_Z, key_bits, ARRAY_LENGTH(key_bits))));
    mouse = has_key && has_relative &&
            (tags.mouse ||
             (bit_is_set(REL_X, relative_bits,
                         ARRAY_LENGTH(relative_bits)) &&
              bit_is_set(REL_Y, relative_bits,
                         ARRAY_LENGTH(relative_bits)) &&
              bit_is_set(BTN_LEFT, key_bits, ARRAY_LENGTH(key_bits))));
    absolute_position =
        (bit_is_set(ABS_X, absolute_bits, ARRAY_LENGTH(absolute_bits)) &&
         bit_is_set(ABS_Y, absolute_bits, ARRAY_LENGTH(absolute_bits))) ||
        (bit_is_set(ABS_MT_POSITION_X, absolute_bits,
                    ARRAY_LENGTH(absolute_bits)) &&
         bit_is_set(ABS_MT_POSITION_Y, absolute_bits,
                    ARRAY_LENGTH(absolute_bits)));
    finger_contact =
        bit_is_set(BTN_TOUCH, key_bits, ARRAY_LENGTH(key_bits)) ||
        bit_is_set(BTN_TOOL_FINGER, key_bits, ARRAY_LENGTH(key_bits));
    touchpad = has_key && has_absolute && !direct &&
               (tags.touchpad || (absolute_position && finger_contact));

    /* A touchscreen must never be captured merely because it also has keys. */
    if (tags.touchscreen && !tags.touchpad)
        return INPUT_KIND_NONE;
    if (touchpad)
        return INPUT_KIND_TOUCHPAD;
    if (mouse)
        return INPUT_KIND_MOUSE;
    if (keyboard)
        return INPUT_KIND_KEYBOARD;
    return INPUT_KIND_NONE;
}

static void maybe_grab_input(int fd)
{
    enum input_kind kind;
    int saved_errno;

    if (fd < 0 || !input_grab_enabled() || hook_active)
        return;

    saved_errno = errno;
    hook_active = true;
    kind = classify_input_fd(fd);
    if (kind != INPUT_KIND_NONE) {
        if (ioctl(fd, EVIOCGRAB, 1) == 0)
            debug_event(fd, kind, "exclusive", 0);
        else
            debug_event(fd, kind, "shared-grab-failed", errno);
    }
    hook_active = false;
    errno = saved_errno;
}

__attribute__((constructor)) static void android_usb_input_grab_init(void)
{
    hook_active = true;
    resolve_symbols();
    (void)input_grab_enabled();
    (void)input_grab_debug_enabled();
    hook_active = false;
}

int open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    bool has_mode = flags_have_mode(flags);
    int fd;

    if (has_mode) {
        va_list arguments;
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
    }
    if (next_open == NULL)
        resolve_symbols();
    fd = unhooked_open(path, flags, mode, has_mode, false);
    maybe_grab_input(fd);
    return fd;
}

int open64(const char *path, int flags, ...)
{
    mode_t mode = 0;
    bool has_mode = flags_have_mode(flags);
    int fd;

    if (has_mode) {
        va_list arguments;
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
    }
    if (next_open64 == NULL)
        resolve_symbols();
    fd = unhooked_open(path, flags, mode, has_mode, true);
    maybe_grab_input(fd);
    return fd;
}

int openat(int directory_fd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    bool has_mode = flags_have_mode(flags);
    int fd;

    if (has_mode) {
        va_list arguments;
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
    }
    if (next_openat == NULL)
        resolve_symbols();
    fd = unhooked_openat(directory_fd, path, flags, mode, has_mode, false);
    maybe_grab_input(fd);
    return fd;
}

int openat64(int directory_fd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    bool has_mode = flags_have_mode(flags);
    int fd;

    if (has_mode) {
        va_list arguments;
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
    }
    if (next_openat64 == NULL)
        resolve_symbols();
    fd = unhooked_openat(directory_fd, path, flags, mode, has_mode, true);
    maybe_grab_input(fd);
    return fd;
}
