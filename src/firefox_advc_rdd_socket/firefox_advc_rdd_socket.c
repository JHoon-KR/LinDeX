#define _GNU_SOURCE

#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/*
 * Firefox enables seccomp in its RDD process before libva loads a driver.
 * The sandbox permits using inherited sockets but rejects connect(2) to the
 * LinDeX AF_UNIX broker.  This adapter creates a small pool before seccomp is
 * installed and hands one connected descriptor to each exact broker connect.
 */

#define PRECONNECTED_SOCKET_COUNT 8
#define EXPECTED_ACK "firefox-rdd-advc-socket-v1"
#define SOCKET_PREFIX "/run/android-drm/"
#define DEFAULT_BROKER_PATH "/run/android-drm/advc-broker-1.1.sock"
#define VA_DRIVER_PATH "/opt/android-drm-lease-kit/codec/advc_drv_video.so"
#ifndef LINDEX_RDD_SOCKET_CONNECT_TIMEOUT_MS
#define LINDEX_RDD_SOCKET_CONNECT_TIMEOUT_MS 5000u
#endif

static int socket_pool[PRECONNECTED_SOCKET_COUNT];
static int active_sockets[PRECONNECTED_SOCKET_COUNT];
struct socket_identity {
  dev_t device;
  ino_t inode;
  bool valid;
};
static struct socket_identity active_identities[PRECONNECTED_SOCKET_COUNT];
static pthread_mutex_t socket_pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool adapter_enabled;
static bool debug_enabled;
static bool avc_decode_enabled;
static char broker_path[sizeof(((struct sockaddr_un *)0)->sun_path)];

#ifndef LINDEX_RDD_SOCKET_NO_AUTOINIT
static void *preloaded_va_driver;

static bool va_driver_is_trusted(void) {
  struct stat status;
  return lstat(VA_DRIVER_PATH, &status) == 0 && S_ISREG(status.st_mode) &&
         status.st_uid == 0 &&
         (status.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

static void abandon_preconnected_pool(void) {
  for (size_t i = 0; i < PRECONNECTED_SOCKET_COUNT; ++i) {
    if (socket_pool[i] >= 0) {
      (void)syscall(SYS_close, socket_pool[i]);
      socket_pool[i] = -1;
    }
  }
  adapter_enabled = false;
}
#endif

static bool exact_env(const char *name, const char *expected) {
  const char *value = getenv(name);
  return value && strcmp(value, expected) == 0;
}

static void debug_log(const char *message, int value) {
  if (!debug_enabled) {
    return;
  }
  char buffer[192];
  int length = snprintf(buffer, sizeof(buffer),
                        "lindex-firefox-rdd-socket: %s%d\n", message, value);
  if (length > 0) {
    size_t count = (size_t)length < sizeof(buffer) ? (size_t)length
                                                   : sizeof(buffer) - 1U;
    (void)syscall(SYS_write, STDERR_FILENO, buffer, count);
  }
}

static bool executable_is_firefox(void) {
  char path[PATH_MAX];
  ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1U);
  if (length <= 0 || (size_t)length >= sizeof(path)) {
    return false;
  }
  path[length] = '\0';
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  return strcmp(base, "firefox-esr") == 0 || strcmp(base, "firefox") == 0;
}

static bool command_line_has_rdd_role(void) {
  char buffer[8192];
  int fd = (int)syscall(SYS_openat, AT_FDCWD, "/proc/self/cmdline",
                        O_RDONLY | O_CLOEXEC, 0);
  if (fd < 0) {
    return false;
  }
  ssize_t length = syscall(SYS_read, fd, buffer, sizeof(buffer));
  (void)syscall(SYS_close, fd);
  if (length <= 0) {
    return false;
  }

  bool content_process = false;
  bool rdd_role = false;
  size_t offset = 0;
  while (offset < (size_t)length) {
    const char *argument = buffer + offset;
    size_t remaining = (size_t)length - offset;
    size_t argument_length = strnlen(argument, remaining);
    if (argument_length == remaining) {
      break;
    }
    if (strcmp(argument, "-contentproc") == 0) {
      content_process = true;
    } else if (strcmp(argument, "rdd") == 0) {
      rdd_role = true;
    }
    offset += argument_length + 1U;
  }
  return content_process && rdd_role;
}

static bool valid_broker_path(const char *path) {
  struct stat st;
  size_t prefix_length = sizeof(SOCKET_PREFIX) - 1U;
  if (!path || strncmp(path, SOCKET_PREFIX, prefix_length) != 0 ||
      path[prefix_length] == '\0' || strlen(path) >= sizeof(broker_path)) {
    return false;
  }
  return stat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}

static int64_t raw_monotonic_ms(void) {
  struct timespec now;
  if (syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &now) < 0 ||
      now.tv_sec > (time_t)(INT64_MAX / 1000)) {
    return -1;
  }
  return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int raw_remaining_connect_ms(int64_t deadline) {
  int64_t now = raw_monotonic_ms();
  int64_t remaining;
  if (now < 0) {
    return -1;
  }
  if (now >= deadline) {
    errno = ETIMEDOUT;
    return -1;
  }
  remaining = deadline - now;
  return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

static int raw_ppoll_ms(struct pollfd *item, nfds_t count, int timeout_ms) {
  struct timespec timeout = {
      .tv_sec = timeout_ms / 1000,
      .tv_nsec = (long)(timeout_ms % 1000) * 1000000L,
  };
  return (int)syscall(SYS_ppoll, item, count, &timeout, NULL, 0);
}

static int raw_wait_for_connect(int fd, int64_t deadline) {
  struct pollfd item = {.fd = fd, .events = POLLOUT};
  for (;;) {
    int remaining = raw_remaining_connect_ms(deadline);
    int result;
    int socket_error = 0;
    socklen_t socket_error_size = sizeof(socket_error);
    if (remaining < 0) {
      return -1;
    }
    result = raw_ppoll_ms(&item, 1, remaining);
    if (result == 0) {
      errno = ETIMEDOUT;
      return -1;
    }
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (syscall(SYS_getsockopt, fd, SOL_SOCKET, SO_ERROR, &socket_error,
                &socket_error_size) < 0) {
      return -1;
    }
    if (socket_error != 0) {
      errno = socket_error;
      return -1;
    }
    return 0;
  }
}

static int raw_wait_before_connect_retry(int64_t deadline) {
  for (;;) {
    int remaining = raw_remaining_connect_ms(deadline);
    int delay;
    int result;
    if (remaining < 0) {
      return -1;
    }
    delay = remaining > 10 ? 10 : remaining;
    result = raw_ppoll_ms(NULL, 0, delay);
    if (result == 0) {
      return 0;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result < 0) {
      return -1;
    }
  }
}

static int raw_restore_blocking(int fd) {
  long flags = syscall(SYS_fcntl, fd, F_GETFL, 0);
  if (flags < 0 ||
      syscall(SYS_fcntl, fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
    return -1;
  }
  return 0;
}

static int raw_preconnect(const char *path) {
  struct sockaddr_un address;
  int64_t now = raw_monotonic_ms();
  int64_t deadline;
  if (now < 0 ||
      now > INT64_MAX - LINDEX_RDD_SOCKET_CONNECT_TIMEOUT_MS) {
    errno = EOVERFLOW;
    return -1;
  }
  deadline = now + LINDEX_RDD_SOCKET_CONNECT_TIMEOUT_MS;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  memcpy(address.sun_path, path, strlen(path) + 1U);

  for (;;) {
    int fd = (int)syscall(SYS_socket, AF_UNIX,
                          SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    int saved;
    if (fd < 0) {
      return -1;
    }
    if (syscall(SYS_connect, fd, &address, sizeof(address)) == 0) {
      if (raw_restore_blocking(fd) == 0) {
        return fd;
      }
      saved = errno;
      (void)syscall(SYS_close, fd);
      errno = saved;
      return -1;
    }
    saved = errno;
    if (saved == EINPROGRESS || saved == EALREADY) {
      if (raw_wait_for_connect(fd, deadline) == 0) {
        if (raw_restore_blocking(fd) == 0) {
          return fd;
        }
        saved = errno;
        (void)syscall(SYS_close, fd);
        errno = saved;
        return -1;
      }
      saved = errno;
    }
    (void)syscall(SYS_close, fd);
    if (saved != EAGAIN) {
      errno = saved;
      return -1;
    }
    if (raw_wait_before_connect_retry(deadline) < 0) {
      return -1;
    }
  }
}

static bool connected_socket_is_healthy(int fd) {
  unsigned char byte;
  ssize_t result;
  if (fd < 0) {
    return false;
  }
  result = syscall(SYS_recvfrom, fd, &byte, sizeof(byte),
                   MSG_PEEK | MSG_DONTWAIT, NULL, NULL);
  return result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

static bool read_socket_identity(int fd, struct socket_identity *identity) {
  struct stat status;
  if (identity == NULL || fstat(fd, &status) < 0 || !S_ISSOCK(status.st_mode)) {
    return false;
  }
  identity->device = status.st_dev;
  identity->inode = status.st_ino;
  identity->valid = true;
  return true;
}

static bool socket_identity_matches(
    int fd, const struct socket_identity *expected) {
  struct socket_identity current = {0};
  return expected != NULL && expected->valid &&
         read_socket_identity(fd, &current) &&
         current.device == expected->device && current.inode == expected->inode;
}

#ifndef LINDEX_RDD_SOCKET_NO_AUTOINIT
__attribute__((constructor)) static void prepare_socket_pool(void) {
  debug_enabled = exact_env("LINDEX_FIREFOX_RDD_SOCKET_DEBUG", "1");
  avc_decode_enabled =
      exact_env("ADVC_VAAPI_ENABLE_AVC", "validated-v1");
  for (size_t i = 0; i < PRECONNECTED_SOCKET_COUNT; ++i) {
    socket_pool[i] = -1;
    active_sockets[i] = -1;
    memset(&active_identities[i], 0, sizeof(active_identities[i]));
  }
  const char *path = getenv("ADVC_VAAPI_SOCKET");
  if (!path || !*path) {
    path = getenv("ADVC_BROKER_SOCKET");
  }
  if (!executable_is_firefox() || !command_line_has_rdd_role() ||
      !exact_env("LINDEX_FIREFOX_RDD_SOCKET_ENABLE", "1") ||
      !exact_env("LINDEX_FIREFOX_RDD_SOCKET_ACK", EXPECTED_ACK) ||
      !valid_broker_path(path)) {
    return;
  }
  memcpy(broker_path, path, strlen(path) + 1U);
  for (size_t i = 0; i < PRECONNECTED_SOCKET_COUNT; ++i) {
    socket_pool[i] = raw_preconnect(broker_path);
    if (socket_pool[i] < 0) {
      debug_log("preconnect failed errno=", errno);
      break;
    }
    debug_log("preconnected fd=", socket_pool[i]);
  }
  adapter_enabled = socket_pool[0] >= 0;
  if (adapter_enabled && avc_decode_enabled) {
    /*
     * Firefox installs the RDD seccomp filter before libva opens its vendor
     * driver.  The exact RDD role, immutable socket and AVC release gate have
     * already been verified above, so map the root-owned driver now while the
     * process is still in its pre-sandbox constructor phase.  A later libva
     * dlopen of this exact path reuses the existing object; RDD receives no
     * filesystem or KGSL permission and keeps Seccomp mode 2.
     */
    if (!va_driver_is_trusted()) {
      debug_log("VA driver trust failed errno=", errno);
      abandon_preconnected_pool();
      return;
    }
    preloaded_va_driver = dlopen(VA_DRIVER_PATH, RTLD_NOW | RTLD_LOCAL);
    if (preloaded_va_driver == NULL) {
      debug_log("VA driver preload failed errno=", errno);
      abandon_preconnected_pool();
      return;
    }
    debug_log("VA driver preloaded fd=", 0);
  }
}
#endif

static bool is_exact_broker_address(const struct sockaddr *address,
                                    socklen_t length) {
  if (!adapter_enabled || !address || address->sa_family != AF_UNIX ||
      length < offsetof(struct sockaddr_un, sun_path) + 1U) {
    return false;
  }
  const struct sockaddr_un *unix_address =
      (const struct sockaddr_un *)address;
  if (unix_address->sun_path[0] == '\0') {
    return false;
  }
  size_t available = (size_t)length - offsetof(struct sockaddr_un, sun_path);
  if (strnlen(unix_address->sun_path, available) >= available) {
    return false;
  }
  return strcmp(unix_address->sun_path, broker_path) == 0 ||
         strcmp(unix_address->sun_path, DEFAULT_BROKER_PATH) == 0;
}

int connect(int fd, const struct sockaddr *address, socklen_t length) {
  if (!is_exact_broker_address(address, length)) {
    return (int)syscall(SYS_connect, fd, address, length);
  }

  size_t slot;
  int prepared = -1;
  pthread_mutex_lock(&socket_pool_mutex);
  for (slot = 0; slot < PRECONNECTED_SOCKET_COUNT; ++slot) {
    if (socket_pool[slot] >= 0) {
      if (!connected_socket_is_healthy(socket_pool[slot])) {
        (void)syscall(SYS_close, socket_pool[slot]);
        socket_pool[slot] = -1;
        continue;
      }
      prepared = socket_pool[slot];
      socket_pool[slot] = -1;
      active_sockets[slot] = -2;
      break;
    }
  }
  pthread_mutex_unlock(&socket_pool_mutex);
  if (prepared < 0) {
    errno = EMFILE;
    debug_log("pool exhausted errno=", errno);
    return -1;
  }

  if (syscall(SYS_dup3, prepared, fd, O_CLOEXEC) < 0) {
    int saved = errno;
    (void)syscall(SYS_close, prepared);
    pthread_mutex_lock(&socket_pool_mutex);
    active_sockets[slot] = -1;
    memset(&active_identities[slot], 0, sizeof(active_identities[slot]));
    pthread_mutex_unlock(&socket_pool_mutex);
    errno = saved;
    debug_log("dup3 failed errno=", errno);
    return -1;
  }
  (void)syscall(SYS_close, prepared);
  pthread_mutex_lock(&socket_pool_mutex);
  active_sockets[slot] = fd;
  if (!read_socket_identity(fd, &active_identities[slot])) {
    active_sockets[slot] = -1;
    memset(&active_identities[slot], 0, sizeof(active_identities[slot]));
    pthread_mutex_unlock(&socket_pool_mutex);
    (void)syscall(SYS_close, fd);
    errno = EBADF;
    return -1;
  }
  pthread_mutex_unlock(&socket_pool_mutex);
  debug_log("handed off fd=", fd);
  return 0;
}

int lindex_firefox_rdd_take_broker_socket(const char *path) {
  if (!adapter_enabled || !path || strcmp(path, broker_path) != 0) {
    errno = ENOSYS;
    return -1;
  }
  size_t slot;
  int prepared = -1;
  pthread_mutex_lock(&socket_pool_mutex);
  for (slot = 0; slot < PRECONNECTED_SOCKET_COUNT; ++slot) {
    if (socket_pool[slot] >= 0) {
      if (!connected_socket_is_healthy(socket_pool[slot])) {
        (void)syscall(SYS_close, socket_pool[slot]);
        socket_pool[slot] = -1;
        continue;
      }
      prepared = socket_pool[slot];
      socket_pool[slot] = -1;
      active_sockets[slot] = prepared;
      if (!read_socket_identity(prepared, &active_identities[slot])) {
        active_sockets[slot] = -1;
        memset(&active_identities[slot], 0, sizeof(active_identities[slot]));
        (void)syscall(SYS_close, prepared);
        prepared = -1;
        continue;
      }
      break;
    }
  }
  pthread_mutex_unlock(&socket_pool_mutex);
  if (prepared < 0) {
    errno = EMFILE;
    debug_log("pool exhausted errno=", errno);
    return -1;
  }
  debug_log("returned fd=", prepared);
  return prepared;
}

/*
 * A successful ADVC CLOSE_SESSION leaves the connection reusable.  The VA
 * driver calls this hook instead of close(2), returning the descriptor to the
 * pool that was created before Firefox installed its RDD seccomp filter.
 */
int lindex_firefox_rdd_recycle_broker_socket(int fd) {
  if (!adapter_enabled || fd < 0) {
    errno = ENOSYS;
    return -1;
  }
  pthread_mutex_lock(&socket_pool_mutex);
  for (size_t slot = 0; slot < PRECONNECTED_SOCKET_COUNT; ++slot) {
    if (active_sockets[slot] == fd && socket_pool[slot] < 0) {
      if (!socket_identity_matches(fd, &active_identities[slot])) {
        active_sockets[slot] = -1;
        memset(&active_identities[slot], 0,
               sizeof(active_identities[slot]));
        pthread_mutex_unlock(&socket_pool_mutex);
        errno = ESTALE;
        return -1;
      }
      if (!connected_socket_is_healthy(fd)) {
        active_sockets[slot] = -1;
        memset(&active_identities[slot], 0,
               sizeof(active_identities[slot]));
        pthread_mutex_unlock(&socket_pool_mutex);
        errno = ECONNRESET;
        return -1;
      }
      active_sockets[slot] = -1;
      memset(&active_identities[slot], 0, sizeof(active_identities[slot]));
      socket_pool[slot] = fd;
      pthread_mutex_unlock(&socket_pool_mutex);
      debug_log("recycled fd=", fd);
      return 0;
    }
  }
  pthread_mutex_unlock(&socket_pool_mutex);
  errno = ENOSYS;
  return -1;
}

/*
 * Firefox sanitizes custom environment entries after this preload constructor
 * has run but before libva loads its vendor driver. Preserve only the exact
 * socket path already validated and connected above, so the later VA client
 * never falls back to the unsandboxed/default broker path.
 */
const char *lindex_firefox_rdd_broker_path(void) {
  return adapter_enabled ? broker_path : NULL;
}

int lindex_firefox_rdd_debug_enabled(void) { return debug_enabled ? 1 : 0; }

/*
 * Firefox deliberately removes non-whitelisted environment variables from
 * its RDD child after preload constructors have run.  Preserve only the
 * exact decode gate that was already present when this tightly scoped
 * adapter validated the Firefox RDD role and preconnected its broker pool.
 * The vendor driver uses this accessor instead of weakening Firefox's
 * environment sanitisation or disabling seccomp.
 */
int lindex_firefox_rdd_avc_decode_enabled(void) {
  return adapter_enabled && avc_decode_enabled ? 1 : 0;
}

#ifndef LINDEX_RDD_SOCKET_NO_AUTOINIT
__attribute__((destructor)) static void close_unused_sockets(void) {
  for (size_t i = 0; i < PRECONNECTED_SOCKET_COUNT; ++i) {
    if (socket_pool[i] >= 0) {
      (void)syscall(SYS_close, socket_pool[i]);
      socket_pool[i] = -1;
    }
    if (active_sockets[i] >= 0) {
      if (socket_identity_matches(active_sockets[i],
                                  &active_identities[i]))
        (void)syscall(SYS_close, active_sockets[i]);
      active_sockets[i] = -1;
      memset(&active_identities[i], 0, sizeof(active_identities[i]));
    }
  }
  if (preloaded_va_driver != NULL) {
    (void)dlclose(preloaded_va_driver);
    preloaded_va_driver = NULL;
  }
}
#endif
