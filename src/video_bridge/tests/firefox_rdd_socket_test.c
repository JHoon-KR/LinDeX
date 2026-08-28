#define _GNU_SOURCE
#define LINDEX_RDD_SOCKET_CONNECT_TIMEOUT_MS 100u

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "../../firefox_advc_rdd_socket/firefox_advc_rdd_socket.c"

static size_t open_fd_count(void) {
    DIR *directory = opendir("/proc/self/fd");
    struct dirent *entry;
    size_t count = 0;
    assert(directory != NULL);
    while ((entry = readdir(directory)) != NULL)
        if (strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0)
            ++count;
    closedir(directory);
    return count;
}

static long elapsed_ms(const struct timespec *start,
                       const struct timespec *finish) {
    return (finish->tv_sec - start->tv_sec) * 1000L +
           (finish->tv_nsec - start->tv_nsec) / 1000000L;
}

static int make_listener(char *path, size_t capacity, int backlog) {
    struct sockaddr_un address;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    assert(snprintf(path, capacity, "/tmp/lindex-rdd-connect-%ld.sock",
                    (long)getpid()) > 0);
    unlink(path);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(fd, backlog) == 0);
    return fd;
}

static void reset_adapter(void) {
    adapter_enabled = true;
    debug_enabled = false;
    memset(broker_path, 0, sizeof(broker_path));
    strcpy(broker_path, "/run/android-drm/test-rdd.sock");
    for (size_t i = 0; i < PRECONNECTED_SOCKET_COUNT; ++i) {
        if (socket_pool[i] >= 0)
            (void)syscall(SYS_close, socket_pool[i]);
        if (active_sockets[i] >= 0 &&
            socket_identity_matches(active_sockets[i],
                                    &active_identities[i]))
            (void)syscall(SYS_close, active_sockets[i]);
        socket_pool[i] = -1;
        active_sockets[i] = -1;
        memset(&active_identities[i], 0, sizeof(active_identities[i]));
    }
}

static void test_healthy_recycle_and_stale_rejection(void) {
    int sockets[2];
    int fd;
    reset_adapter();
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) ==
           0);
    socket_pool[0] = sockets[0];
    fd = lindex_firefox_rdd_take_broker_socket(broker_path);
    assert(fd == sockets[0] && active_sockets[0] == fd);
    assert(lindex_firefox_rdd_recycle_broker_socket(fd) == 0);
    assert(socket_pool[0] == fd && active_sockets[0] == -1);
    assert(send(sockets[1], "x", 1, MSG_NOSIGNAL) == 1);
    assert(lindex_firefox_rdd_take_broker_socket(broker_path) < 0);
    assert(errno == EMFILE && socket_pool[0] == -1);
    (void)syscall(SYS_close, sockets[1]);
}

static void test_close_forgets_active_fd_number(void) {
    int sockets[2];
    int replacement;
    int fd;
    reset_adapter();
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) ==
           0);
    socket_pool[0] = sockets[0];
    fd = lindex_firefox_rdd_take_broker_socket(broker_path);
    assert(fd >= 0 && active_sockets[0] == fd);
    assert(close(fd) == 0);
    assert(active_sockets[0] == fd && active_identities[0].valid);
    replacement = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(replacement >= 0);
    if (replacement != fd) {
        assert(dup2(replacement, fd) == fd);
        assert(close(replacement) == 0);
        replacement = fd;
    }
    close_unused_sockets();
    assert(fcntl(replacement, F_GETFD) >= 0);
    assert(active_sockets[0] == -1 && !active_identities[0].valid);
    assert(close(replacement) == 0);
    (void)syscall(SYS_close, sockets[1]);
}

static void test_unhealthy_recycle_is_fail_closed(void) {
    int sockets[2];
    int fd;
    reset_adapter();
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) ==
           0);
    socket_pool[0] = sockets[0];
    fd = lindex_firefox_rdd_take_broker_socket(broker_path);
    assert(fd >= 0);
    (void)syscall(SYS_close, sockets[1]);
    assert(lindex_firefox_rdd_recycle_broker_socket(fd) < 0);
    assert(errno == ECONNRESET && active_sockets[0] == -1 &&
           socket_pool[0] == -1);
    assert(close(fd) == 0);
}

static void test_raw_preconnect_restores_blocking(void) {
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int listener = make_listener(path, sizeof(path), 1);
    int client = raw_preconnect(path);
    int accepted;
    assert(client >= 0);
    assert((fcntl(client, F_GETFL) & O_NONBLOCK) == 0);
    accepted = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    assert(accepted >= 0);
    close(accepted);
    close(client);
    close(listener);
    unlink(path);
}

static void test_raw_preconnect_full_backlog(void) {
    enum { MAX_FILLERS = 16 };
    struct sockaddr_un address;
    struct timespec start;
    struct timespec finish;
    char path[sizeof(address.sun_path)];
    int fillers[MAX_FILLERS];
    size_t baseline = open_fd_count();
    size_t filler_count = 0;
    int listener = make_listener(path, sizeof(path), 1);

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    while (filler_count < MAX_FILLERS) {
        int fd = socket(AF_UNIX,
                        SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        assert(fd >= 0);
        if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
            fillers[filler_count++] = fd;
            continue;
        }
        assert(errno == EAGAIN);
        close(fd);
        break;
    }
    assert(filler_count > 0 && filler_count < MAX_FILLERS);
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    errno = 0;
    assert(raw_preconnect(path) < 0);
    assert(errno == ETIMEDOUT);
    assert(clock_gettime(CLOCK_MONOTONIC, &finish) == 0);
    assert(elapsed_ms(&start, &finish) >= 50L);
    assert(elapsed_ms(&start, &finish) < 1000L);
    while (filler_count > 0) close(fillers[--filler_count]);
    close(listener);
    unlink(path);
    assert(open_fd_count() == baseline);
}

int main(void) {
    test_raw_preconnect_restores_blocking();
    test_raw_preconnect_full_backlog();
    test_healthy_recycle_and_stale_rejection();
    test_close_forgets_active_fd_number();
    test_unhealthy_recycle_is_fail_closed();
    reset_adapter();
    adapter_enabled = false;
    return 0;
}
