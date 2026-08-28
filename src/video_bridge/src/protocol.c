#define _GNU_SOURCE
#include "advc/protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

void advc_put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

void advc_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void advc_put_u64(uint8_t *p, uint64_t v) {
    advc_put_u32(p, (uint32_t)v);
    advc_put_u32(p + 4, (uint32_t)(v >> 32));
}

uint16_t advc_get_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t advc_get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t advc_get_u64(const uint8_t *p) {
    return (uint64_t)advc_get_u32(p) | ((uint64_t)advc_get_u32(p + 4) << 32);
}

int advc_header_encode(uint8_t out[ADVC_HEADER_SIZE], const struct advc_header *h) {
    if (out == NULL || h == NULL || advc_header_validate(h) != 0) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, ADVC_HEADER_SIZE);
    advc_put_u32(out + 0, ADVC_MAGIC);
    advc_put_u16(out + 4, h->version_major);
    advc_put_u16(out + 6, h->version_minor);
    advc_put_u16(out + 8, h->message_type);
    advc_put_u16(out + 10, h->opcode);
    advc_put_u32(out + 12, h->request_id);
    advc_put_u32(out + 16, h->session_id);
    advc_put_u32(out + 20, h->flags);
    advc_put_u32(out + 24, h->payload_size);
    advc_put_u16(out + 28, h->fd_count);
    advc_put_u16(out + 30, 0);
    return ADVC_HEADER_SIZE;
}

int advc_header_decode(struct advc_header *h, const uint8_t in[ADVC_HEADER_SIZE]) {
    if (h == NULL || in == NULL || advc_get_u32(in) != ADVC_MAGIC) {
        errno = EPROTO;
        return -1;
    }
    h->version_major = advc_get_u16(in + 4);
    h->version_minor = advc_get_u16(in + 6);
    h->message_type = advc_get_u16(in + 8);
    h->opcode = advc_get_u16(in + 10);
    h->request_id = advc_get_u32(in + 12);
    h->session_id = advc_get_u32(in + 16);
    h->flags = advc_get_u32(in + 20);
    h->payload_size = advc_get_u32(in + 24);
    h->fd_count = advc_get_u16(in + 28);
    h->reserved = advc_get_u16(in + 30);
    return advc_header_validate(h);
}

int advc_header_validate(const struct advc_header *h) {
    if (h == NULL || h->version_major != ADVC_VERSION_MAJOR ||
        h->message_type < ADVC_MSG_REQUEST || h->message_type > ADVC_MSG_EVENT ||
        h->opcode < ADVC_OP_HELLO || h->opcode > ADVC_OP_RESERVE_LINEAR ||
        h->payload_size > ADVC_MAX_PAYLOAD || h->fd_count > ADVC_MAX_FDS ||
        h->reserved != 0) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

int advc_send_message(int fd, const struct advc_message *m) {
    uint8_t header[ADVC_HEADER_SIZE];
    struct iovec iov[2];
    struct msghdr msg;
    char control[CMSG_SPACE(sizeof(int) * ADVC_MAX_FDS)];
    struct cmsghdr *cmsg;
    ssize_t sent;

    if (m == NULL || (m->header.payload_size > 0 && m->payload == NULL) ||
        advc_header_encode(header, &m->header) < 0) {
        return -1;
    }
    memset(&msg, 0, sizeof(msg));
    iov[0].iov_base = header;
    iov[0].iov_len = sizeof(header);
    iov[1].iov_base = m->payload;
    iov[1].iov_len = m->header.payload_size;
    msg.msg_iov = iov;
    msg.msg_iovlen = m->header.payload_size ? 2 : 1;
    if (m->header.fd_count > 0) {
        memset(control, 0, sizeof(control));
        msg.msg_control = control;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * m->header.fd_count);
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * m->header.fd_count);
        memcpy(CMSG_DATA(cmsg), m->fds, sizeof(int) * m->header.fd_count);
    }
    do {
        sent = sendmsg(fd, &msg, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0) return -1;
    if ((size_t)sent != ADVC_HEADER_SIZE + m->header.payload_size) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int advc_receive_message(int fd, struct advc_message *m) {
    uint8_t header_bytes[ADVC_HEADER_SIZE];
    char control[CMSG_SPACE(sizeof(int) * ADVC_MAX_FDS)];
    struct iovec iov[2];
    struct msghdr msg;
    struct cmsghdr *cmsg;
    struct advc_header header;
    size_t received_fds = 0;
    ssize_t received;

    if (m == NULL || (m->payload_capacity > 0 && m->payload == NULL)) {
        errno = EINVAL;
        return -1;
    }
    memset(&msg, 0, sizeof(msg));
    memset(control, 0, sizeof(control));
    iov[0].iov_base = header_bytes;
    iov[0].iov_len = sizeof(header_bytes);
    iov[1].iov_base = m->payload;
    iov[1].iov_len = m->payload_capacity;
    msg.msg_iov = iov;
    msg.msg_iovlen = m->payload_capacity > 0 ? 2 : 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    do {
        received = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
    } while (received < 0 && errno == EINTR);
    if (received < 0) return -1;
    if (received == 0) {
        errno = ECONNRESET;
        return -1;
    }
    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        size_t bytes;
        size_t count;
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
        bytes = cmsg->cmsg_len - CMSG_LEN(0);
        count = bytes / sizeof(int);
        if (received_fds + count > ADVC_MAX_FDS) {
            int *incoming = (int *)CMSG_DATA(cmsg);
            for (size_t i = 0; i < count; ++i) close(incoming[i]);
            errno = EMSGSIZE;
            return -1;
        }
        memcpy(m->fds + received_fds, CMSG_DATA(cmsg), count * sizeof(int));
        received_fds += count;
    }
    if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
        received < (ssize_t)ADVC_HEADER_SIZE ||
        advc_header_decode(&header, header_bytes) < 0 ||
        (size_t)received != ADVC_HEADER_SIZE + header.payload_size ||
        received_fds != header.fd_count ||
        header.payload_size > m->payload_capacity) {
        for (size_t i = 0; i < received_fds; ++i) close(m->fds[i]);
        errno = EMSGSIZE;
        return -1;
    }
    m->header = header;
    return 0;
}

void advc_close_message_fds(struct advc_message *m) {
    if (m == NULL) return;
    for (uint16_t i = 0; i < m->header.fd_count && i < ADVC_MAX_FDS; ++i) {
        if (m->fds[i] >= 0) close(m->fds[i]);
        m->fds[i] = -1;
    }
    m->header.fd_count = 0;
}
