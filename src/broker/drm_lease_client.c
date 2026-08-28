// SPDX-License-Identifier: MIT
// Receive a DRM lease fd from the Android broker and exec a Debian program.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_LEASE_OBJECTS 64U
struct broker_reply {
    uint32_t version, lessee_id, connector_id, crtc_id, object_count;
    uint32_t objects[MAX_LEASE_OBJECTS];
};

int main(int argc, char **argv)
{
    const char *socket_path = "/data/local/tmp/debian-drm-lease.sock";
    struct sockaddr_un address;
    struct broker_reply reply;
    char control[CMSG_SPACE(sizeof(int))], fd_text[32], lessee_text[32];
    char objects_text[768];
    struct iovec iov = { .iov_base = &reply, .iov_len = sizeof(reply) };
    struct msghdr message;
    int socket_fd = -1, lease_fd = -1, exec_index = 1;

    if (argc > 3 && strcmp(argv[1], "--socket") == 0) {
        socket_path = argv[2];
        exec_index = 3;
    }
    if (exec_index >= argc || strlen(socket_path) >= sizeof(address.sun_path)) {
        fprintf(stderr, "usage: %s [--socket PATH] PROGRAM [ARG...]\n", argv[0]);
        return 2;
    }
    socket_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path);
    if (socket_fd < 0 || connect(socket_fd, (struct sockaddr *)&address,
                                 sizeof(address)) != 0) {
        fprintf(stderr, "broker connect failed errno=%d (%s)\n", errno, strerror(errno));
        return 1;
    }
    memset(&reply, 0, sizeof(reply));
    memset(&message, 0, sizeof(message));
    memset(control, 0, sizeof(control));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    if (recvmsg(socket_fd, &message, 0) != sizeof(reply)) {
        fprintf(stderr, "broker reply failed errno=%d (%s)\n", errno, strerror(errno));
        return 1;
    }
    /* This protocol carries exactly one control message containing one fd. */
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
    if (!(message.msg_flags & MSG_CTRUNC) && cmsg != NULL &&
        cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
        cmsg->cmsg_len >= CMSG_LEN(sizeof(int)))
        memcpy(&lease_fd, CMSG_DATA(cmsg), sizeof(lease_fd));
    if (reply.version != 1 || reply.object_count > MAX_LEASE_OBJECTS || lease_fd < 0) {
        fputs("invalid broker reply\n", stderr);
        return 1;
    }
    int flags = fcntl(lease_fd, F_GETFD);
    if (flags < 0 || fcntl(lease_fd, F_SETFD, flags & ~FD_CLOEXEC) != 0)
        return 1;
    snprintf(fd_text, sizeof(fd_text), "%d", lease_fd);
    snprintf(lessee_text, sizeof(lessee_text), "%u", reply.lessee_id);
    size_t used = 0;
    objects_text[0] = '\0';
    for (uint32_t i = 0; i < reply.object_count; i++) {
        int written = snprintf(objects_text + used, sizeof(objects_text) - used,
                               "%s%u", i ? "," : "", reply.objects[i]);
        if (written < 0 || (size_t)written >= sizeof(objects_text) - used)
            return 1;
        used += (size_t)written;
    }
    setenv("DRM_LEASE_FD", fd_text, 1);
    setenv("DRM_LEASE_LESSEE_ID", lessee_text, 1);
    setenv("DRM_LEASE_OBJECTS", objects_text, 1);
    printf("LEASE RECEIVED: fd=%d lessee_id=%u connector=%u CRTC=%u objects=%s\n",
           lease_fd, reply.lessee_id, reply.connector_id, reply.crtc_id,
           objects_text);
    execvp(argv[exec_index], &argv[exec_index]);
    fprintf(stderr, "exec failed errno=%d (%s)\n", errno, strerror(errno));
    return 127;
}
