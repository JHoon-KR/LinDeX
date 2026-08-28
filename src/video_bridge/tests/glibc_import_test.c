#define _GNU_SOURCE
#include "advc/glibc_import.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static size_t count_open_fds(void) {
    DIR *directory = opendir("/proc/self/fd");
    struct dirent *entry;
    size_t count = 0;
    assert(directory != NULL);
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
            ++count;
    }
    closedir(directory);
    return count;
}

static int send_record(int fd, const void *payload, size_t payload_size,
                       const int *fds, size_t fd_count) {
    struct iovec iov = {.iov_base = (void *)payload, .iov_len = payload_size};
    struct msghdr message;
    union {
        struct cmsghdr align;
        unsigned char bytes[CMSG_SPACE(sizeof(int) * (ADVC_MAX_FDS + 1))];
    } control;
    memset(&message, 0, sizeof(message));
    memset(&control, 0, sizeof(control));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    if (fd_count > 0) {
        struct cmsghdr *cmsg;
        message.msg_control = control.bytes;
        message.msg_controllen = CMSG_SPACE(sizeof(int) * fd_count);
        cmsg = CMSG_FIRSTHDR(&message);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fd_count);
        memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * fd_count);
    }
    return sendmsg(fd, &message, 0) < 0 ? -1 : 0;
}

static void init_output(struct advc_client_output *output, int fence_fd) {
    memset(output, 0, sizeof(*output));
    output->buffer_id = 7;
    output->transport = ADVC_TRANSPORT_AHARDWAREBUFFER;
    output->width = 1920;
    output->height = 1080;
    output->android_format = 0x22;
    output->stride = 1920;
    output->layers = 1;
    output->usage = UINT64_C(0x900);
    output->slice_height = output->height;
    output->crop_right = output->width - 1;
    output->crop_bottom = output->height - 1;
    output->data_fd = -1;
    output->acquire_fence_fd = fence_fd;
}

static struct advc_glibc_opaque_ahb *receive_valid_record(void) {
    int sockets[2];
    int source_fd;
    const uint8_t payload[] = {0x41, 0x48, 0x42, 0x01, 0, 0, 0, 0};
    void *opaque = NULL;
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
    source_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(source_fd >= 0);
    assert(send_record(sockets[1], payload, sizeof(payload), &source_fd, 1) == 0);
    close(source_fd);
    assert(advc_glibc_receive_opaque_ahb(sockets[0], &opaque, NULL) == 0);
    close(sockets[0]);
    close(sockets[1]);
    assert(opaque != NULL);
    assert(((struct advc_glibc_opaque_ahb *)opaque)->payload_size == sizeof(payload));
    assert(((struct advc_glibc_opaque_ahb *)opaque)->fd_count == 1);
    assert((fcntl(((struct advc_glibc_opaque_ahb *)opaque)->fds[0], F_GETFD) &
            FD_CLOEXEC) != 0);
    return (struct advc_glibc_opaque_ahb *)opaque;
}

static void test_opaque_receive_and_fail_closed_prime(void) {
    struct advc_glibc_opaque_ahb *handle = receive_valid_record();
    struct advc_client_output output;
    struct advc_drm_prime_import prime;
    int fence = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(fence >= 0);
    init_output(&output, fence);
    assert(advc_glibc_ahb_validate(&output, handle) == 0);
    errno = 0;
    assert(advc_glibc_ahb_to_prime(&output, handle, &prime) == -1);
    assert(errno == ENOTSUP);
    assert(prime.drm_fourcc == 0);
    assert(prime.explicit_flags == 0);
    assert(prime.plane_count == 0);
    assert(prime.acquire_fence_fd == -1);
    advc_glibc_opaque_ahb_close(handle);
    close(fence);
}

static void test_record_without_fd_is_rejected(void) {
    int sockets[2];
    const uint8_t payload[] = {1, 2, 3, 4};
    void *opaque = (void *)1;
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
    assert(send_record(sockets[1], payload, sizeof(payload), NULL, 0) == 0);
    errno = 0;
    assert(advc_glibc_receive_opaque_ahb(sockets[0], &opaque, NULL) == -1);
    assert(errno == EPROTO);
    assert(opaque == NULL);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_too_many_fds_are_rejected(void) {
    int sockets[2];
    int fds[ADVC_MAX_FDS + 1];
    const uint8_t payload[] = {1};
    void *opaque = (void *)1;
    size_t before_receive;
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
    for (size_t i = 0; i < ADVC_MAX_FDS + 1; ++i) {
        fds[i] = open("/dev/null", O_RDONLY | O_CLOEXEC);
        assert(fds[i] >= 0);
    }
    assert(send_record(sockets[1], payload, sizeof(payload), fds,
                       ADVC_MAX_FDS + 1) == 0);
    for (size_t i = 0; i < ADVC_MAX_FDS + 1; ++i) close(fds[i]);
    before_receive = count_open_fds();
    errno = 0;
    assert(advc_glibc_receive_opaque_ahb(sockets[0], &opaque, NULL) == -1);
    assert(errno == EMSGSIZE);
    assert(opaque == NULL);
    assert(count_open_fds() == before_receive);
    close(sockets[0]);
    close(sockets[1]);
}

static void init_prime(struct advc_drm_prime_import *prime, int plane_fd) {
    memset(prime, 0, sizeof(*prime));
    prime->width = 1280;
    prime->height = 720;
    prime->drm_fourcc = UINT32_C(0x3231564e); /* NV12, explicitly supplied. */
    prime->drm_modifier = 0; /* LINEAR only because the explicit bit is set. */
    prime->plane_count = 1;
    prime->explicit_flags = ADVC_PRIME_EXPLICIT_ALL;
    prime->planes[0].fd = plane_fd;
    prime->planes[0].pitch = 1280;
    for (size_t i = 1; i < 4; ++i) prime->planes[i].fd = -1;
    prime->acquire_fence_fd = -1;
}

static void test_explicit_prime_contract(void) {
    struct advc_drm_prime_import prime;
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    init_prime(&prime, fd);
    assert(advc_drm_prime_import_validate(&prime) == 0);

    prime.explicit_flags &= ~ADVC_PRIME_EXPLICIT_MODIFIER;
    errno = 0;
    assert(advc_drm_prime_import_validate(&prime) == -1 && errno == EINVAL);
    prime.explicit_flags = ADVC_PRIME_EXPLICIT_ALL;
    prime.drm_fourcc = 0;
    errno = 0;
    assert(advc_drm_prime_import_validate(&prime) == -1 && errno == EINVAL);
    prime.drm_fourcc = UINT32_C(0x3231564e);
    prime.planes[0].pitch = 0;
    errno = 0;
    assert(advc_drm_prime_import_validate(&prime) == -1 && errno == EINVAL);
    close(fd);
}

int main(void) {
    test_opaque_receive_and_fail_closed_prime();
    test_record_without_fd_is_rejected();
    test_too_many_fds_are_rejected();
    test_explicit_prime_contract();
    return 0;
}
