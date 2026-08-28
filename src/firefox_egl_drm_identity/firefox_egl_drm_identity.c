#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Keep this adapter independent of EGL development headers. */
typedef void *EGLDeviceEXT;
typedef int EGLint;
typedef void (*EGLProc)(void);
typedef EGLProc (*EglGetProcAddressFn)(const char *name);
typedef const char *(*EglQueryDeviceStringFn)(EGLDeviceEXT device,
                                              EGLint name);
typedef void *(*DlsymFn)(void *handle, const char *name);

#define EGL_EXTENSIONS 0x3055
#define EGL_DRM_DEVICE_FILE_EXT 0x3233
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377

static EglGetProcAddressFn real_get_proc;
static EglQueryDeviceStringFn real_query_device_string;
static DlsymFn real_dlsym;
static bool active_checked;
static bool active_value;
static char extension_buffer[4096];

static void debug_log(const char *format, ...) {
  if (strcmp(getenv("LINDEX_EGL_DRM_IDENTITY_DEBUG") ?: "", "1") != 0) {
    return;
  }
  va_list ap;
  va_start(ap, format);
  fputs("lindex-egl-drm-identity: ", stderr);
  vfprintf(stderr, format, ap);
  fputc('\n', stderr);
  va_end(ap);
}

static bool exact_env(const char *name, const char *expected) {
  const char *value = getenv(name);
  return value && strcmp(value, expected) == 0;
}

static bool is_character_device(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISCHR(st.st_mode);
}

static bool process_is_glxtest(void) {
  char path[PATH_MAX];
  ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (length <= 0 || (size_t)length >= sizeof(path)) {
    return false;
  }
  path[length] = '\0';
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  return strcmp(base, "glxtest") == 0;
}

static bool adapter_active(void) {
  if (active_checked) {
    return active_value;
  }
  active_checked = true;
  active_value =
      process_is_glxtest() &&
      exact_env("LINDEX_EGL_DRM_IDENTITY_ENABLE", "1") &&
      exact_env("LINDEX_EGL_DRM_IDENTITY_ACK",
                "kgsl-card0-renderD128-firefox-glxtest-v1") &&
      exact_env("MESA_LOADER_DRIVER_OVERRIDE", "kgsl") &&
      is_character_device("/dev/kgsl-3d0") &&
      is_character_device("/dev/dri/card0") &&
      is_character_device("/dev/dri/renderD128");
  debug_log("active=%d", active_value ? 1 : 0);
  return active_value;
}

static DlsymFn resolve_dlsym(void) {
  if (real_dlsym) {
    return real_dlsym;
  }
  real_dlsym = (DlsymFn)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
  if (!real_dlsym) {
    real_dlsym = (DlsymFn)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.17");
  }
  return real_dlsym;
}

static EglGetProcAddressFn resolve_get_proc(void) {
  if (!real_get_proc) {
    DlsymFn lookup = resolve_dlsym();
    if (lookup) {
      real_get_proc =
          (EglGetProcAddressFn)lookup(RTLD_NEXT, "eglGetProcAddress");
    }
  }
  return real_get_proc;
}

static EglQueryDeviceStringFn resolve_query_device_string(void) {
  if (real_query_device_string) {
    return real_query_device_string;
  }
  EglGetProcAddressFn get_proc = resolve_get_proc();
  if (get_proc) {
    EGLProc proc = get_proc("eglQueryDeviceStringEXT");
    real_query_device_string = (EglQueryDeviceStringFn)proc;
  }
  return real_query_device_string;
}

static bool append_token(char *output, size_t capacity, size_t *used,
                         const char *token, size_t length) {
  size_t extra = length + (*used ? 1U : 0U);
  if (*used + extra + 1U > capacity) {
    return false;
  }
  if (*used) {
    output[(*used)++] = ' ';
  }
  memcpy(output + *used, token, length);
  *used += length;
  output[*used] = '\0';
  return true;
}

static const char *filtered_extensions(const char *native) {
  size_t used = 0;
  extension_buffer[0] = '\0';
  const char *cursor = native ?: "";
  while (*cursor) {
    while (*cursor == ' ') {
      ++cursor;
    }
    const char *start = cursor;
    while (*cursor && *cursor != ' ') {
      ++cursor;
    }
    size_t length = (size_t)(cursor - start);
    if (!length || (length == strlen("EGL_MESA_device_software") &&
                    memcmp(start, "EGL_MESA_device_software", length) == 0)) {
      continue;
    }
    if (!append_token(extension_buffer, sizeof(extension_buffer), &used,
                      start, length)) {
      return native;
    }
  }

  static const char drm_ext[] = "EGL_EXT_device_drm";
  static const char render_ext[] = "EGL_EXT_device_drm_render_node";
  if (!strstr(extension_buffer, drm_ext) &&
      !append_token(extension_buffer, sizeof(extension_buffer), &used,
                    drm_ext, sizeof(drm_ext) - 1U)) {
    return native;
  }
  if (!strstr(extension_buffer, render_ext) &&
      !append_token(extension_buffer, sizeof(extension_buffer), &used,
                    render_ext, sizeof(render_ext) - 1U)) {
    return native;
  }
  return extension_buffer;
}

static const char *query_device_string(EGLDeviceEXT device, EGLint name) {
  EglQueryDeviceStringFn real = resolve_query_device_string();
  const char *native = real ? real(device, name) : NULL;
  if (!adapter_active()) {
    return native;
  }
  switch (name) {
    case EGL_EXTENSIONS:
      debug_log("filtering device extensions: %s", native ?: "(null)");
      return filtered_extensions(native);
    case EGL_DRM_DEVICE_FILE_EXT:
    case EGL_DRM_RENDER_NODE_FILE_EXT:
      debug_log("mapping EGL device attribute 0x%x to renderD128", name);
      return "/dev/dri/renderD128";
    default:
      return native;
  }
}

EGLProc eglGetProcAddress(const char *name) {
  EglGetProcAddressFn real = resolve_get_proc();
  EGLProc native = real ? real(name) : NULL;
  if (name && strcmp(name, "eglQueryDeviceStringEXT") == 0) {
    real_query_device_string = (EglQueryDeviceStringFn)native;
    if (adapter_active()) {
      return (EGLProc)query_device_string;
    }
  }
  return native;
}

void *dlsym(void *handle, const char *name) {
  DlsymFn lookup = resolve_dlsym();
  void *native = lookup ? lookup(handle, name) : NULL;
  if (!adapter_active()) {
    return native;
  }
  if (strcmp(name, "eglGetProcAddress") == 0) {
    real_get_proc = (EglGetProcAddressFn)native;
    debug_log("intercepted dlsym(eglGetProcAddress)");
    return (void *)eglGetProcAddress;
  }
  if (strcmp(name, "eglQueryDeviceStringEXT") == 0) {
    real_query_device_string = (EglQueryDeviceStringFn)native;
    debug_log("intercepted dlsym(eglQueryDeviceStringEXT)");
    return (void *)query_device_string;
  }
  return native;
}
