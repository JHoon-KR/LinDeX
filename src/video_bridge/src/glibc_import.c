#define _GNU_SOURCE
#include "advc/glibc_import.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void close_fds(int *fds, uint16_t count) {
    for (uint16_t i = 0; i < count; ++i) {
        if (fds[i] >= 0) close(fds[i]);
        fds[i] = -1;
    }
}

void advc_glibc_opaque_ahb_close(struct advc_glibc_opaque_ahb *handle) {
    if (handle == NULL) return;
    close_fds(handle->fds, handle->fd_count);
    memset(handle, 0, sizeof(*handle));
    for (size_t i = 0; i < ADVC_MAX_FDS; ++i) handle->fds[i] = -1;
    free(handle);
}

int advc_glibc_receive_opaque_ahb(int socket_fd, void **native_buffer,
                                  void *userdata) {
    struct advc_glibc_opaque_ahb *handle;
    struct iovec iov;
    struct msghdr message;
    union {
        struct cmsghdr align;
        unsigned char bytes[CMSG_SPACE(sizeof(int) * ADVC_MAX_FDS)];
    } control;
    ssize_t received;
    uint16_t fd_count = 0;
    (void)userdata;
    if (socket_fd < 0 || native_buffer == NULL) {
        errno = EINVAL;
        return -1;
    }
    *native_buffer = NULL;
    handle = (struct advc_glibc_opaque_ahb *)calloc(1, sizeof(*handle));
    if (handle == NULL) return -1;
    for (size_t i = 0; i < ADVC_MAX_FDS; ++i) handle->fds[i] = -1;
    memset(&message, 0, sizeof(message));
    memset(&control, 0, sizeof(control));
    iov.iov_base = handle->payload;
    iov.iov_len = sizeof(handle->payload);
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    received = recvmsg(socket_fd, &message, MSG_CMSG_CLOEXEC);
    if (received <= 0) {
        if (received == 0) errno = EPROTO;
        goto fail;
    }
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message); cmsg != NULL;
         cmsg = CMSG_NXTHDR(&message, cmsg)) {
        size_t bytes;
        size_t count;
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len < CMSG_LEN(0)) {
            errno = EPROTO;
            goto fail;
        }
        bytes = cmsg->cmsg_len - CMSG_LEN(0);
        if (bytes == 0 || bytes % sizeof(int) != 0) {
            errno = EPROTO;
            goto fail;
        }
        count = bytes / sizeof(int);
        if (count > ADVC_MAX_FDS - fd_count) {
            errno = EMSGSIZE;
            goto fail;
        }
        memcpy(handle->fds + fd_count, CMSG_DATA(cmsg), bytes);
        fd_count = (uint16_t)(fd_count + count);
        handle->fd_count = fd_count;
    }
    /* Close every descriptor visible in a truncated control record on failure. */
    if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
        errno = EMSGSIZE;
        goto fail;
    }
    if (fd_count == 0) {
        errno = EPROTO;
        goto fail;
    }
    for (uint16_t i = 0; i < fd_count; ++i) {
        int flags = fcntl(handle->fds[i], F_GETFD);
        if (flags < 0 || (flags & FD_CLOEXEC) == 0) {
            errno = EPROTO;
            goto fail;
        }
    }
    handle->payload_size = (size_t)received;
    *native_buffer = handle;
    return 0;

fail:
    {
        int saved = errno;
        advc_glibc_opaque_ahb_close(handle);
        errno = saved;
    }
    return -1;
}

int advc_glibc_ahb_validate(const struct advc_client_output *output,
                            const struct advc_glibc_opaque_ahb *handle) {
    if (output == NULL || handle == NULL ||
        output->transport != ADVC_TRANSPORT_AHARDWAREBUFFER ||
        output->buffer_id == 0 || output->size != 0 || output->data_fd != -1 ||
        output->width == 0 || output->height == 0 || output->layers == 0 ||
        output->android_format == 0 || output->stride == 0 || output->usage == 0 ||
        output->slice_height != output->height || output->crop_left != 0 ||
        output->crop_top != 0 || output->crop_right != output->width - 1 ||
        output->crop_bottom != output->height - 1 ||
        (output->flags & ~(ADVC_FLAG_END_OF_STREAM | ADVC_FLAG_KEY_FRAME |
                           ADVC_FLAG_CODEC_CONFIG)) != 0 ||
        handle->payload_size == 0 ||
        handle->payload_size > ADVC_OPAQUE_NATIVE_HANDLE_MAX_BYTES ||
        handle->fd_count == 0 || handle->fd_count > ADVC_MAX_FDS) {
        errno = EPROTO;
        return -1;
    }
    for (uint16_t i = 0; i < handle->fd_count; ++i) {
        int flags = fcntl(handle->fds[i], F_GETFD);
        if (flags < 0 || (flags & FD_CLOEXEC) == 0) {
            errno = EPROTO;
            return -1;
        }
    }
    if (output->acquire_fence_fd >= 0) {
        int flags = fcntl(output->acquire_fence_fd, F_GETFD);
        if (flags < 0 || (flags & FD_CLOEXEC) == 0) {
            errno = EPROTO;
            return -1;
        }
    }
    return 0;
}

int advc_drm_prime_import_validate(
    const struct advc_drm_prime_import *import_contract) {
    if (import_contract == NULL || import_contract->width == 0 ||
        import_contract->height == 0 || import_contract->drm_fourcc == 0 ||
        import_contract->drm_modifier == UINT64_MAX ||
        import_contract->plane_count == 0 || import_contract->plane_count > 4 ||
        import_contract->explicit_flags != ADVC_PRIME_EXPLICIT_ALL) {
        errno = EINVAL;
        return -1;
    }
    for (uint32_t i = 0; i < import_contract->plane_count; ++i) {
        int flags;
        if (import_contract->planes[i].fd < 0 ||
            import_contract->planes[i].pitch == 0) {
            errno = EINVAL;
            return -1;
        }
        flags = fcntl(import_contract->planes[i].fd, F_GETFD);
        if (flags < 0 || (flags & FD_CLOEXEC) == 0) {
            errno = EBADF;
            return -1;
        }
    }
    for (uint32_t i = import_contract->plane_count; i < 4; ++i) {
        if (import_contract->planes[i].fd != -1 ||
            import_contract->planes[i].offset != 0 ||
            import_contract->planes[i].pitch != 0) {
            errno = EINVAL;
            return -1;
        }
    }
    if (import_contract->acquire_fence_fd >= 0) {
        int flags = fcntl(import_contract->acquire_fence_fd, F_GETFD);
        if (flags < 0 || (flags & FD_CLOEXEC) == 0) {
            errno = EBADF;
            return -1;
        }
    }
    return 0;
}

int advc_glibc_ahb_to_prime(const struct advc_client_output *output,
                            const struct advc_glibc_opaque_ahb *handle,
                            struct advc_drm_prime_import *import_contract) {
    if (import_contract == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(import_contract, 0, sizeof(*import_contract));
    for (size_t i = 0; i < 4; ++i) import_contract->planes[i].fd = -1;
    import_contract->acquire_fence_fd = -1;
    if (advc_glibc_ahb_validate(output, handle) < 0) return -1;
    errno = ENOTSUP;
    return -1;
}
