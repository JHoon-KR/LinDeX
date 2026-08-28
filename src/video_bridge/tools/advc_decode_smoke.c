#define _GNU_SOURCE
#include "advc/client.h"
#include "advc_annexb.h"
#if defined(ADVC_TURNIP_VALIDATE)
#include "turnip_prime_import.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#include "android_prime_mapper.h"
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001u
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002u
#endif

#if defined(ADVC_TURNIP_VALIDATE)
#ifndef DMA_HEAP_IOCTL_ALLOC
struct smoke_dma_heap_allocation_data {
    uint64_t len;
    uint32_t fd;
    uint32_t fd_flags;
    uint64_t heap_flags;
};
#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC                                                \
    _IOWR(DMA_HEAP_IOC_MAGIC, 0, struct smoke_dma_heap_allocation_data)
#endif
#ifndef DMA_BUF_IOCTL_SYNC
struct smoke_dma_buf_sync {
    uint64_t flags;
};
#define DMA_BUF_SYNC_READ UINT64_C(1)
#define DMA_BUF_SYNC_WRITE UINT64_C(2)
#define DMA_BUF_SYNC_START UINT64_C(0)
#define DMA_BUF_SYNC_END UINT64_C(4)
#define DMA_BUF_BASE 'b'
#define DMA_BUF_IOCTL_SYNC _IOW(DMA_BUF_BASE, 0, struct smoke_dma_buf_sync)
#else
struct smoke_dma_buf_sync {
    uint64_t flags;
};
#endif

#define SMOKE_ANDROID_COLOR_FORMAT_YUV420_SEMIPLANAR UINT32_C(21)
#define SMOKE_DRM_FORMAT_NV12 UINT32_C(0x3231564e)
#endif

#define SMOKE_DEADLINE_MS UINT64_C(10000)

struct smoke_result {
    const char *stage;
    const char *validation_reason;
    int status;
    int error_number;
    uint32_t outputs;
    uint32_t nonempty_outputs;
    uint32_t ahb_outputs;
    uint64_t bytes;
    uint64_t config_bytes;
    uint64_t frame_bytes;
    uint32_t width;
    uint32_t height;
    uint32_t wire_format;
    uint32_t wire_stride;
    uint32_t wire_slice_height;
    uint32_t wire_layers;
    uint64_t wire_usage;
    uint32_t ahb_format;
    uint32_t ahb_width;
    uint32_t ahb_height;
    uint32_t ahb_stride;
    uint32_t ahb_layers;
    uint64_t ahb_usage;
    uint32_t prime_exports;
    uint32_t prime_fourcc;
    uint64_t prime_modifier;
    uint64_t prime_allocation_size;
    uint32_t prime_objects;
    uint32_t prime_planes;
    uint64_t prime_plane_offsets[ADVC_MAX_DMABUF_PLANES];
    uint32_t prime_plane_strides[ADVC_MAX_DMABUF_PLANES];
    uint32_t prime_crop_left;
    uint32_t prime_crop_top;
    uint32_t prime_crop_width;
    uint32_t prime_crop_height;
    uint32_t prime_crop_count;
    uint32_t release_fences;
    uint32_t release_calls;
    uint32_t async_reservations;
    uint64_t turnip_content_hash;
    uint64_t turnip_content_bytes;
    uint32_t turnip_distinct_values;
    int prime_errno;
    const char *prime_status;
    uint32_t prime_transport_fds;
    uint32_t prime_transport_ints;
    uint64_t prime_transport_fd_sizes[ADVC_MAX_DMABUF_OBJECTS];
    uint64_t prime_mapper_width;
    uint64_t prime_mapper_height;
    uint64_t prime_mapper_layers;
    uint32_t prime_mapper_stride;
    int32_t prime_qti_data_fd_value;
    int32_t prime_qti_data_fd_query_errno;
    int32_t prime_qti_data_fd_transport_index;
    uint32_t prime_qti_data_fd_valid;
    uint64_t prime_qti_data_fd_size;
    int eos;
};

#ifdef __ANDROID__
static struct advc_ahb_prime_mapper *prime_mapper;
#endif

static uint64_t monotonic_ms(void);

#ifdef __ANDROID__
static int receive_ahb(int socket_fd, void **native_buffer, void *userdata) {
    AHardwareBuffer *buffer = NULL;
    (void)userdata;
    if (AHardwareBuffer_recvHandleFromUnixSocket(socket_fd, &buffer) != 0 ||
        buffer == NULL) return -1;
    *native_buffer = buffer;
    return 0;
}
#endif

#ifdef __ANDROID__
static int wait_acquire_fence(int fence_fd, uint64_t deadline_ms) {
    struct pollfd poll_fd;
    uint64_t now;
    int timeout;
    int result;
    if (fence_fd < 0) return 0;
    for (;;) {
        now = monotonic_ms();
        if (now == UINT64_MAX || now >= deadline_ms) {
            errno = ETIMEDOUT;
            return -1;
        }
        timeout = (int)(deadline_ms - now > INT_MAX ? INT_MAX : deadline_ms - now);
        poll_fd.fd = fence_fd;
        poll_fd.events = POLLIN;
        poll_fd.revents = 0;
        result = poll(&poll_fd, 1, timeout);
        if (result > 0) return 0;
        if (result == 0) { errno = ETIMEDOUT; return -1; }
        if (errno != EINTR) return -1;
    }
}
#endif

static uint64_t monotonic_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) < 0) return UINT64_MAX;
    return (uint64_t)value.tv_sec * UINT64_C(1000) + (uint64_t)value.tv_nsec / 1000000u;
}

static int apply_remaining_timeout(int fd, uint64_t deadline_ms) {
    struct timeval timeout;
    uint64_t now = monotonic_ms();
    uint64_t remaining;
    if (now == UINT64_MAX || now >= deadline_ms) {
        errno = ETIMEDOUT;
        return -1;
    }
    remaining = deadline_ms - now;
    timeout.tv_sec = (time_t)(remaining / 1000u);
    timeout.tv_usec = (suseconds_t)((remaining % 1000u) * 1000u);
    if (timeout.tv_sec == 0 && timeout.tv_usec == 0) timeout.tv_usec = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0)
        return -1;
    return 0;
}

static int parse_dimension(const char *text, uint32_t *value) {
    char *end;
    unsigned long parsed;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < 16 || parsed > 8192 ||
        parsed > UINT32_MAX)
        return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static int connect_argument(const char *argument) {
    char *end;
    long inherited;
    if (strncmp(argument, "fd:", 3) != 0)
        return advc_client_connect(argument);
    errno = 0;
    inherited = strtol(argument + 3, &end, 10);
    if (errno != 0 || end == argument + 3 || *end != '\0' || inherited < 0 ||
        inherited > INT_MAX) {
        errno = EINVAL;
        return -1;
    }
    return fcntl((int)inherited, F_DUPFD_CLOEXEC, 3);
}

static int create_input_memfd(const char *path, uint64_t *size_out) {
    uint8_t buffer[65536];
    struct stat statbuf;
    uint64_t total = 0;
    int input_fd = -1;
    int memfd = -1;
    input_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (input_fd < 0 || fstat(input_fd, &statbuf) < 0 || !S_ISREG(statbuf.st_mode) ||
        statbuf.st_size <= 0 || (uint64_t)statbuf.st_size > ADVC_MAX_INPUT_BYTES) {
        if (input_fd >= 0) close(input_fd);
        errno = EINVAL;
        return -1;
    }
    memfd = (int)syscall(SYS_memfd_create, "advc-smoke-input",
                         MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (memfd < 0) {
        close(input_fd);
        return -1;
    }
    for (;;) {
        ssize_t received;
        do {
            received = read(input_fd, buffer, sizeof(buffer));
        } while (received < 0 && errno == EINTR);
        if (received < 0) goto fail;
        if (received == 0) break;
        for (ssize_t done = 0; done < received;) {
            ssize_t written;
            do {
                written = write(memfd, buffer + done, (size_t)(received - done));
            } while (written < 0 && errno == EINTR);
            if (written <= 0) goto fail;
            done += written;
        }
        total += (uint64_t)received;
        if (total > ADVC_MAX_INPUT_BYTES) {
            errno = EFBIG;
            goto fail;
        }
    }
    close(input_fd);
    if (total != (uint64_t)statbuf.st_size ||
        fcntl(memfd, F_ADD_SEALS,
              F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) < 0)
        goto fail_memfd;
    *size_out = total;
    return memfd;

fail:
    close(input_fd);
fail_memfd:
    {
        int saved = errno;
        close(memfd);
        errno = saved;
    }
    return -1;
}

static int validate_content(const struct advc_client_output *output) {
    uint8_t byte;
    if (output->size == 0) return 0;
    if (pread(output->data_fd, &byte, 1, 0) != 1) return -1;
    if (output->size > 1 &&
        pread(output->data_fd, &byte, 1, (off_t)(output->size - 1)) != 1)
        return -1;
    return 0;
}

#if defined(ADVC_TURNIP_VALIDATE)
static int allocate_linear_dmabuf(size_t size) {
    static const char *const heaps[] = {
        "/dev/dma_heap/system",
        "/dev/dma_heap/system-uncached",
    };
    for (size_t i = 0; i < sizeof(heaps) / sizeof(heaps[0]); ++i) {
        struct smoke_dma_heap_allocation_data request;
        int heap = open(heaps[i], O_RDONLY | O_CLOEXEC);
        if (heap < 0) continue;
        memset(&request, 0, sizeof(request));
        request.len = size;
        request.fd_flags = O_RDWR | O_CLOEXEC;
        if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &request) == 0) {
            close(heap);
            return (int)request.fd;
        }
        close(heap);
    }
    return -1;
}

static int populate_linear_dmabuf(int dmabuf_fd, size_t allocation_size,
                                  const struct advc_client_output *output) {
    struct smoke_dma_buf_sync sync;
    uint8_t *mapping = MAP_FAILED;
    size_t copied = 0;
    int sync_started = 0;
    int saved_errno;

    sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE;
    if (ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) return -1;
    sync_started = 1;
    mapping = mmap(NULL, allocation_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   dmabuf_fd, 0);
    if (mapping == MAP_FAILED) goto fail;
    memset(mapping, 0, allocation_size);
    while (copied < output->size) {
        ssize_t count = pread(output->data_fd, mapping + copied,
                              (size_t)output->size - copied, (off_t)copied);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            if (count == 0) errno = EIO;
            goto fail;
        }
        copied += (size_t)count;
    }
    /* DMA_BUF_IOCTL_SYNC is authoritative for dma-buf cache ownership. Some
     * Android dma-heaps reject msync(2) with EINVAL even though the mapping and
     * explicit END sync are valid, so msync is only a best-effort hint here. */
    (void)msync(mapping, allocation_size, MS_SYNC);
    if (munmap(mapping, allocation_size) < 0) {
        mapping = MAP_FAILED;
        goto fail;
    }
    mapping = MAP_FAILED;
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    if (ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) return -1;
    return 0;

fail:
    saved_errno = errno == 0 ? EIO : errno;
    if (mapping != MAP_FAILED) (void)munmap(mapping, allocation_size);
    if (sync_started) {
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
        (void)ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync);
    }
    errno = saved_errno;
    return -1;
}

/*
 * Validation-only compatibility path. It proves that a byte-output hardware
 * decode can be copied once into an explicit modifier=0 NV12 dma-buf and then
 * consumed by Turnip. It must never be reported as original-buffer zero-copy.
 */
static int validate_linear_repack(const struct advc_client_output *output,
                                  struct smoke_result *result) {
    struct advc_dmabuf_descriptor descriptor;
    struct advc_turnip_prime_result imported;
    uint64_t y_size;
    uint64_t allocation_size;
    uint32_t stride;
    uint32_t slice_height;
    int saved_errno;

    memset(&descriptor, 0, sizeof(descriptor));
    for (size_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        descriptor.objects[i].fd = -1;
    memset(&imported, 0, sizeof(imported));
    imported.release_fence_fd = -1;

    stride = output->stride != 0 ? output->stride : output->width;
    slice_height = output->slice_height != 0 ? output->slice_height : output->height;
    if (output->data_fd < 0 || output->size == 0 ||
        output->android_format != SMOKE_ANDROID_COLOR_FORMAT_YUV420_SEMIPLANAR ||
        output->width == 0 || output->height == 0 ||
        (output->width & 1u) != 0 || (output->height & 1u) != 0 ||
        stride < output->width || slice_height < output->height ||
        (slice_height & 1u) != 0 ||
        (uint64_t)stride > UINT64_MAX / slice_height) {
        errno = EINVAL;
        return -1;
    }
    y_size = (uint64_t)stride * slice_height;
    if (y_size > UINT64_MAX - y_size / 2u) {
        errno = EOVERFLOW;
        return -1;
    }
    allocation_size = y_size + y_size / 2u;
    if (allocation_size > SIZE_MAX || output->size > allocation_size) {
        errno = EOVERFLOW;
        return -1;
    }

    descriptor.objects[0].fd = allocate_linear_dmabuf((size_t)allocation_size);
    if (descriptor.objects[0].fd < 0) return -1;
    /* Publish ownership before any fallible operation so cleanup closes it. */
    descriptor.object_count = 1;
    if (populate_linear_dmabuf(descriptor.objects[0].fd,
                               (size_t)allocation_size, output) < 0)
        goto fail;

    descriptor.buffer_id = output->buffer_id;
    descriptor.width = output->width;
    descriptor.height = output->height;
    descriptor.drm_fourcc = SMOKE_DRM_FORMAT_NV12;
    descriptor.explicit_flags = ADVC_DMABUF_EXPLICIT_ALL;
    descriptor.drm_modifier = 0;
    descriptor.crop_left = output->crop_left;
    descriptor.crop_top = output->crop_top;
    descriptor.crop_width = output->crop_right - output->crop_left + 1u;
    descriptor.crop_height = output->crop_bottom - output->crop_top + 1u;
    descriptor.plane_count = 2;
    descriptor.objects[0].size = allocation_size;
    descriptor.planes[0].object_index = 0;
    descriptor.planes[0].offset = 0;
    descriptor.planes[0].pitch = stride;
    descriptor.planes[1].object_index = 0;
    descriptor.planes[1].offset = y_size;
    descriptor.planes[1].pitch = stride;

    ++result->prime_exports;
    result->prime_fourcc = descriptor.drm_fourcc;
    result->prime_modifier = descriptor.drm_modifier;
    result->prime_allocation_size = allocation_size;
    result->prime_objects = descriptor.object_count;
    result->prime_planes = descriptor.plane_count;
    result->prime_plane_offsets[0] = descriptor.planes[0].offset;
    result->prime_plane_offsets[1] = descriptor.planes[1].offset;
    result->prime_plane_strides[0] = descriptor.planes[0].pitch;
    result->prime_plane_strides[1] = descriptor.planes[1].pitch;
    result->prime_crop_left = descriptor.crop_left;
    result->prime_crop_top = descriptor.crop_top;
    result->prime_crop_width = descriptor.crop_width;
    result->prime_crop_height = descriptor.crop_height;
    result->prime_crop_count = 1;
    if (advc_turnip_prime_consume(&descriptor, -1, &imported) < 0)
        goto fail;
    result->turnip_content_hash = imported.content_hash;
    result->turnip_content_bytes = imported.content_bytes;
    result->turnip_distinct_values = imported.distinct_sample_values;
    result->prime_status = "turnip-linear-repack-content-fence-pass";
    if (imported.release_fence_fd >= 0) {
        ++result->release_fences;
        close(imported.release_fence_fd);
        imported.release_fence_fd = -1;
    }
    advc_dmabuf_descriptor_close(&descriptor);
    return 0;

fail:
    saved_errno = errno == 0 ? EIO : errno;
    if (imported.release_fence_fd >= 0) close(imported.release_fence_fd);
    advc_dmabuf_descriptor_close(&descriptor);
    errno = saved_errno;
    return -1;
}
#endif

static int sleep_until_retry(uint64_t deadline_ms) {
    uint64_t now = monotonic_ms();
    int delay;
    if (now == UINT64_MAX || now >= deadline_ms) {
        errno = ETIMEDOUT;
        return -1;
    }
    delay = (int)(deadline_ms - now > 10 ? 10 : deadline_ms - now);
    while (poll(NULL, 0, delay) < 0) {
        if (errno != EINTR) return -1;
        now = monotonic_ms();
        if (now == UINT64_MAX || now >= deadline_ms) {
            errno = ETIMEDOUT;
            return -1;
        }
        delay = (int)(deadline_ms - now > 10 ? 10 : deadline_ms - now);
    }
    return 0;
}

static int call_failed(struct smoke_result *result, const char *stage, int status) {
    result->stage = stage;
    result->status = status;
    result->error_number = status < 0 ? errno : 0;
    return -1;
}

/* Returns 0 when no output is ready, 1 for a frame, 2 for EOS, and -1 on error. */
static int consume_one_output(int socket_fd, uint32_t session_id,
                              struct smoke_result *result, uint64_t deadline_ms,
                              uint32_t *detail) {
    struct advc_client_output output;
    uint64_t buffer_id;
    int release_fence = -1;
#ifdef __ANDROID__
    void *native_buffer = NULL;
#endif
    int content_status;
    int status;

    if (apply_remaining_timeout(socket_fd, deadline_ms) < 0)
        return call_failed(result, "dequeue", -1);
    status = advc_client_dequeue_output(socket_fd, session_id, &output, detail);
    if (status == ADVC_STATUS_WOULD_BLOCK) return 0;
    if (status != ADVC_STATUS_OK) return call_failed(result, "dequeue", status);
    if (output.transport == ADVC_TRANSPORT_AHARDWAREBUFFER) {
#ifdef __ANDROID__
        AHardwareBuffer_Desc desc;
        if (apply_remaining_timeout(socket_fd, deadline_ms) < 0 ||
            advc_client_transfer_ahb(socket_fd, session_id, output.buffer_id,
                                     receive_ahb, NULL, &native_buffer, detail) !=
                ADVC_STATUS_OK || native_buffer == NULL ||
            wait_acquire_fence(output.acquire_fence_fd, deadline_ms) < 0) {
            if (native_buffer != NULL)
                AHardwareBuffer_release((AHardwareBuffer *)native_buffer);
            advc_client_output_close(&output);
            return call_failed(result, "transfer_ahb", -1);
        }
        AHardwareBuffer_describe((AHardwareBuffer *)native_buffer, &desc);
        result->ahb_format = desc.format;
        result->ahb_width = desc.width;
        result->ahb_height = desc.height;
        result->ahb_stride = desc.stride;
        result->ahb_layers = desc.layers;
        result->ahb_usage = desc.usage;
        content_status = desc.width == output.width && desc.height == output.height &&
                         desc.layers == output.layers && desc.format == output.android_format &&
                         desc.stride == output.stride && desc.usage == output.usage ? 0 : -1;
        if (output.crop_left > output.crop_right ||
            output.crop_top > output.crop_bottom ||
            output.crop_right >= output.width ||
            output.crop_bottom >= output.height)
            content_status = -1;
        ++result->ahb_outputs;
        if (content_status == 0 && prime_mapper != NULL) {
            struct advc_ahb_public_metadata metadata;
            struct advc_ahb_prime_export exported;
            metadata.width = desc.width;
            metadata.height = desc.height;
            metadata.android_format = desc.format;
            metadata.stride = desc.stride;
            metadata.layers = desc.layers;
            metadata.usage = desc.usage;
            metadata.crop_left = output.crop_left;
            metadata.crop_top = output.crop_top;
            metadata.crop_width = output.crop_right - output.crop_left + 1u;
            metadata.crop_height = output.crop_bottom - output.crop_top + 1u;
            if (advc_ahb_prime_mapper_export(
                    prime_mapper, native_buffer, &metadata, output.buffer_id,
                    output.acquire_fence_fd, &exported) < 0) {
                struct advc_android_prime_diagnostics diagnostics;
                result->prime_errno = errno;
                result->prime_status =
                    advc_android_prime_mapper_last_status();
                result->prime_transport_fds =
                    advc_android_prime_mapper_last_transport_fds();
                result->prime_transport_ints =
                    advc_android_prime_mapper_last_transport_ints();
                advc_android_prime_mapper_last_diagnostics(&diagnostics);
                result->prime_fourcc = diagnostics.fourcc;
                result->prime_mapper_width = diagnostics.mapper_width;
                result->prime_mapper_height = diagnostics.mapper_height;
                result->prime_mapper_layers = diagnostics.mapper_layers;
                result->prime_mapper_stride = diagnostics.mapper_stride;
                result->prime_qti_data_fd_transport_index =
                    diagnostics.qti_data_fd_transport_index;
                result->prime_qti_data_fd_value =
                    diagnostics.qti_data_fd_value;
                result->prime_qti_data_fd_query_errno =
                    diagnostics.qti_data_fd_query_errno;
                result->prime_qti_data_fd_valid =
                    diagnostics.qti_data_fd_valid;
                result->prime_qti_data_fd_size =
                    diagnostics.qti_data_fd_size;
                for (uint32_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
                    result->prime_transport_fd_sizes[i] =
                        diagnostics.transport_fd_sizes[i];
                result->prime_modifier = diagnostics.modifier;
                result->prime_allocation_size = diagnostics.allocation_size;
                result->prime_objects = diagnostics.transport_fds;
                result->prime_planes = diagnostics.plane_count;
                result->prime_crop_left = diagnostics.crop_left;
                result->prime_crop_count = diagnostics.crop_count;
                result->prime_crop_top = diagnostics.crop_top;
                result->prime_crop_width = diagnostics.crop_width;
                result->prime_crop_height = diagnostics.crop_height;
                for (uint32_t i = 0; i < ADVC_MAX_DMABUF_PLANES; ++i) {
                    result->prime_plane_offsets[i] =
                        diagnostics.plane_offsets[i];
                    result->prime_plane_strides[i] =
                        diagnostics.plane_strides[i];
                }
                result->validation_reason = "stable_mapper_prime_unavailable";
                content_status = -1;
            } else {
                result->prime_status =
                    advc_android_prime_mapper_last_status();
                result->prime_transport_fds =
                    advc_android_prime_mapper_last_transport_fds();
                result->prime_transport_ints =
                    advc_android_prime_mapper_last_transport_ints();
                ++result->prime_exports;
                result->prime_fourcc = exported.descriptor.drm_fourcc;
                result->prime_modifier = exported.descriptor.drm_modifier;
                result->prime_objects = exported.descriptor.object_count;
                result->prime_planes = exported.descriptor.plane_count;
                result->prime_allocation_size =
                    exported.descriptor.objects[0].size;
                result->prime_crop_left = exported.descriptor.crop_left;
                result->prime_crop_top = exported.descriptor.crop_top;
                result->prime_crop_width = exported.descriptor.crop_width;
                result->prime_crop_height = exported.descriptor.crop_height;
                for (uint32_t i = 0; i < exported.descriptor.plane_count; ++i) {
                    result->prime_plane_offsets[i] =
                        exported.descriptor.planes[i].offset;
                    result->prime_plane_strides[i] =
                        exported.descriptor.planes[i].pitch;
                }
                advc_ahb_prime_export_close(&exported);
            }
            if (output.acquire_fence_fd >= 0) {
                release_fence = fcntl(output.acquire_fence_fd,
                                      F_DUPFD_CLOEXEC, 0);
                if (release_fence < 0) {
                    result->validation_reason = "release_fence_duplicate";
                    content_status = -1;
                } else {
                    ++result->release_fences;
                }
            }
        }
        AHardwareBuffer_release((AHardwareBuffer *)native_buffer);
        native_buffer = NULL;
        if (content_status < 0 && result->validation_reason == NULL) {
            result->validation_reason = "ahb_descriptor_mismatch";
            errno = EPROTO;
        }
#elif defined(ADVC_TURNIP_VALIDATE)
        struct advc_dmabuf_descriptor prime;
        struct advc_turnip_prime_result imported;
        struct advc_turnip_linear_repack_result repacked;
        const char *gpu_linear_repack =
            getenv("ADVC_SMOKE_GPU_LINEAR_REPACK");
        memset(&prime, 0, sizeof(prime));
        for (size_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
            prime.objects[i].fd = -1;
        memset(&imported, 0, sizeof(imported));
        imported.release_fence_fd = -1;
        memset(&repacked, 0, sizeof(repacked));
        for (size_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
            repacked.descriptor.objects[i].fd = -1;
        repacked.acquire_fence_fd = -1;
        repacked.source_release_fence_fd = -1;
        status = advc_client_transfer_prime(socket_fd, session_id,
                                            output.buffer_id, &prime, detail);
        if (status != ADVC_STATUS_OK) {
            result->prime_status = "transfer-prime-failed";
            result->validation_reason = "transfer_prime_failed";
            if (status >= 0) errno = EPROTO;
            content_status = -1;
        } else {
            ++result->prime_exports;
            result->prime_fourcc = prime.drm_fourcc;
            result->prime_modifier = prime.drm_modifier;
            result->prime_allocation_size = prime.objects[0].size;
            result->prime_objects = prime.object_count;
            result->prime_planes = prime.plane_count;
            result->prime_crop_left = prime.crop_left;
            result->prime_crop_top = prime.crop_top;
            result->prime_crop_width = prime.crop_width;
            result->prime_crop_height = prime.crop_height;
            for (uint32_t i = 0; i < prime.plane_count; ++i) {
                result->prime_plane_offsets[i] = prime.planes[i].offset;
                result->prime_plane_strides[i] = prime.planes[i].pitch;
            }
            if (gpu_linear_repack != NULL &&
                strcmp(gpu_linear_repack, "1") == 0) {
                if (advc_turnip_prime_repack_linear(
                        &prime, output.acquire_fence_fd, prime.buffer_id,
                        &repacked) < 0) {
                    result->prime_errno = errno;
                    result->prime_status =
                        "turnip-gpu-linear-repack-failed";
                    result->validation_reason =
                        "turnip_gpu_linear_repack_failed";
                    content_status = -1;
                } else {
                    release_fence = repacked.source_release_fence_fd;
                    repacked.source_release_fence_fd = -1;
                    if (release_fence >= 0) ++result->release_fences;
                    result->prime_fourcc =
                        repacked.descriptor.drm_fourcc;
                    result->prime_modifier =
                        repacked.descriptor.drm_modifier;
                    result->prime_allocation_size =
                        repacked.descriptor.objects[0].size;
                    result->prime_objects =
                        repacked.descriptor.object_count;
                    result->prime_planes = repacked.descriptor.plane_count;
                    result->prime_crop_left = repacked.descriptor.crop_left;
                    result->prime_crop_top = repacked.descriptor.crop_top;
                    result->prime_crop_width = repacked.descriptor.crop_width;
                    result->prime_crop_height =
                        repacked.descriptor.crop_height;
                    for (uint32_t i = 0;
                         i < repacked.descriptor.plane_count; ++i) {
                        result->prime_plane_offsets[i] =
                            repacked.descriptor.planes[i].offset;
                        result->prime_plane_strides[i] =
                            repacked.descriptor.planes[i].pitch;
                    }
                    if (advc_turnip_prime_consume(
                            &repacked.descriptor,
                            repacked.acquire_fence_fd, &imported) < 0) {
                        result->prime_errno = errno;
                        result->prime_status =
                            "turnip-gpu-linear-import-failed";
                        result->validation_reason =
                            "turnip_gpu_linear_import_failed";
                        content_status = -1;
                    } else {
                        result->turnip_content_hash = imported.content_hash;
                        result->turnip_content_bytes = imported.content_bytes;
                        result->turnip_distinct_values =
                            imported.distinct_sample_values;
                        result->prime_status =
                            "turnip-gpu-linear-repack-content-fence-pass";
                        content_status = 0;
                        if (imported.release_fence_fd >= 0) {
                            ++result->release_fences;
                            close(imported.release_fence_fd);
                            imported.release_fence_fd = -1;
                        }
                    }
                }
            } else {
                if (advc_turnip_prime_consume(
                        &prime, output.acquire_fence_fd, &imported) < 0) {
                    result->prime_errno = errno;
                    result->prime_status = "turnip-import-failed";
                    result->validation_reason =
                        "turnip_prime_import_failed";
                    content_status = -1;
                } else {
                    release_fence = imported.release_fence_fd;
                    imported.release_fence_fd = -1;
                    result->turnip_content_hash = imported.content_hash;
                    result->turnip_content_bytes = imported.content_bytes;
                    result->turnip_distinct_values =
                        imported.distinct_sample_values;
                    result->prime_status =
                        "turnip-import-content-fence-pass";
                    content_status = 0;
                    if (release_fence >= 0) ++result->release_fences;
                }
            }
        }
        ++result->ahb_outputs;
        advc_turnip_linear_repack_close(&repacked);
        advc_dmabuf_descriptor_close(&prime);
#else
        errno = ENOTSUP;
        content_status = -1;
#endif
    } else {
        content_status = validate_content(&output);
#if defined(ADVC_TURNIP_VALIDATE)
        {
            const char *linear_repack = getenv("ADVC_SMOKE_LINEAR_REPACK");
            if (content_status == 0 && output.size > 0 &&
                linear_repack != NULL && strcmp(linear_repack, "1") == 0) {
                if (validate_linear_repack(&output, result) < 0) {
                    result->prime_errno = errno;
                    result->prime_status = "turnip-linear-repack-failed";
                    result->validation_reason =
                        "turnip_linear_repack_failed";
                    content_status = -1;
                }
            }
        }
#endif
    }
    result->wire_format = output.android_format;
    result->wire_stride = output.stride;
    result->wire_slice_height = output.slice_height;
    result->wire_layers = output.layers;
    result->wire_usage = output.usage;
    if (output.width == 0 || output.width > 8192 || output.height == 0 ||
        output.height > 8192) {
        result->validation_reason = "invalid_dimensions";
        errno = EPROTO;
    } else if (output.crop_left > output.crop_right ||
               output.crop_top > output.crop_bottom ||
               output.crop_right >= output.width ||
               output.crop_bottom >= output.height) {
        result->validation_reason = "invalid_crop";
        errno = EPROTO;
    } else if (content_status < 0) {
        if (result->validation_reason == NULL)
            result->validation_reason = "content_validation_failed";
        if (errno == 0) errno = EPROTO;
    }
    if (result->validation_reason == NULL) {
        ++result->outputs;
        if (output.size > 0 ||
            output.transport == ADVC_TRANSPORT_AHARDWAREBUFFER) {
            ++result->nonempty_outputs;
            result->bytes += output.size;
        }
        result->width = output.width;
        result->height = output.height;
        result->eos = (output.flags & ADVC_FLAG_END_OF_STREAM) != 0;
    }
    buffer_id = output.buffer_id;
    advc_client_output_close(&output);
    if (apply_remaining_timeout(socket_fd, deadline_ms) < 0) {
        if (release_fence >= 0) close(release_fence);
        return call_failed(result, "release_output", -1);
    }
    status = advc_client_release_output_fenced(
        socket_fd, session_id, buffer_id, release_fence, detail);
    if (release_fence >= 0) close(release_fence);
    if (status != ADVC_STATUS_OK)
        return call_failed(result, "release_output", status);
    ++result->release_calls;
    if (result->validation_reason != NULL) {
        if (result->prime_errno != 0) errno = result->prime_errno;
        return call_failed(result, "validate_output", -1);
    }
    return result->eos ? 2 : 1;
}

static int queue_with_drain(int socket_fd, uint32_t session_id,
                            const struct advc_client_input *input,
                            struct smoke_result *result, uint64_t deadline_ms,
                            uint32_t *detail, const char *stage) {
    for (;;) {
        int drained;
        int status;
        if (apply_remaining_timeout(socket_fd, deadline_ms) < 0)
            return call_failed(result, stage, -1);
        status = advc_client_queue_input(socket_fd, session_id, input, detail);
        if (status == ADVC_STATUS_OK) return 0;
        if (status != ADVC_STATUS_WOULD_BLOCK)
            return call_failed(result, stage, status);
        drained = consume_one_output(socket_fd, session_id, result, deadline_ms, detail);
        if (drained < 0) return -1;
        if (drained == 2) {
            errno = EPROTO;
            return call_failed(result, "premature_eos", -1);
        }
        if (drained == 0 && sleep_until_retry(deadline_ms) < 0)
            return call_failed(result, stage, -1);
    }
}

int main(int argc, char **argv) {
    struct advc_client_session_config config;
    struct advc_client_input input;
    struct advc_avc_annexb_parts avc_parts;
    struct advc_avc_annexb_stream avc_stream;
    struct smoke_result result;
    uint64_t features = 0;
    uint64_t input_size = 0;
    uint64_t deadline;
    uint32_t max_payload = 0;
    uint32_t session_id = 0;
    uint32_t detail = 0;
    uint32_t next_buffer_id = 1;
    uint32_t repeat_frames = 1;
    int input_memfd = -1;
    int socket_fd = -1;
    int status;
    int is_avc;
    int stream_avc = 0;
    int rc = EXIT_FAILURE;

    memset(&result, 0, sizeof(result));
    result.stage = "arguments";
    result.validation_reason = NULL;
    result.status = -1;
    result.prime_status = "not-requested";
    result.prime_qti_data_fd_value = -1;
    result.prime_qti_data_fd_transport_index = -1;
    if (argc != 6 || strnlen(argv[2], ADVC_MAX_MIME) >= ADVC_MAX_MIME ||
        strncmp(argv[2], "video/", 6) != 0) {
        result.error_number = EINVAL;
        goto done;
    }
    memset(&config, 0, sizeof(config));
    if (parse_dimension(argv[3], &config.width) < 0 ||
        parse_dimension(argv[4], &config.height) < 0) {
        result.error_number = EINVAL;
        goto done;
    }
    config.mime = argv[2];
    config.direction = ADVC_DIRECTION_DECODE;
    config.transport = (getenv("ADVC_SMOKE_AHB") != NULL ||
                        getenv("ADVC_SMOKE_PRIME") != NULL) ?
                       ADVC_TRANSPORT_AHARDWAREBUFFER : ADVC_TRANSPORT_BYTES;
    config.framerate_milli = 60000;
    {
        const char *repeat = getenv("ADVC_SMOKE_REPEAT_FRAMES");
        if (repeat != NULL) {
            uint32_t parsed;
            if (parse_dimension(repeat, &parsed) < 0 ||
                parsed > ADVC_AVC_MAX_SMOKE_FRAMES) {
                result.error_number = EINVAL;
                goto done;
            }
            repeat_frames = parsed;
        }
    }

    input_memfd = create_input_memfd(argv[5], &input_size);
    if (input_memfd < 0) {
        result.stage = "input";
        result.error_number = errno;
        goto done;
    }
#ifdef __ANDROID__
    if (getenv("ADVC_SMOKE_PRIME") != NULL) {
        prime_mapper = advc_android_prime_mapper_create();
        if (prime_mapper == NULL) {
            result.stage = "stable_mapper_create";
            result.error_number = errno;
            result.prime_status =
                advc_android_prime_mapper_last_status();
            goto done;
        }
    }
#endif
    is_avc = strcmp(config.mime, "video/avc") == 0;
    memset(&avc_parts, 0, sizeof(avc_parts));
    memset(&avc_stream, 0, sizeof(avc_stream));
    if (is_avc) {
        void *mapping = mmap(NULL, (size_t)input_size, PROT_READ, MAP_PRIVATE,
                             input_memfd, 0);
        stream_avc = getenv("ADVC_SMOKE_STREAM_AVC") != NULL;
        if (mapping == MAP_FAILED ||
            (stream_avc
                 ? advc_avc_annexb_split_single_slice_stream(
                       (const uint8_t *)mapping, (size_t)input_size, &avc_stream)
                 : advc_avc_annexb_split((const uint8_t *)mapping,
                                         (size_t)input_size, &avc_parts)) < 0) {
            if (mapping != MAP_FAILED) munmap(mapping, (size_t)input_size);
            result.stage = "parse_annexb";
            result.error_number = EINVAL;
            goto done;
        }
        munmap(mapping, (size_t)input_size);
        if (stream_avc) {
            avc_parts.config_offset = avc_stream.config_offset;
            avc_parts.config_size = avc_stream.config_size;
            avc_parts.frame_offset = avc_stream.frames[0].offset;
            avc_parts.frame_size = (size_t)input_size - avc_stream.config_size;
        }
        if (getenv("ADVC_SMOKE_COMBINED_AVC") != NULL) {
            avc_parts.config_offset = 0;
            avc_parts.config_size = 0;
            avc_parts.frame_offset = 0;
            avc_parts.frame_size = (size_t)input_size;
        }
    } else {
        avc_parts.frame_offset = 0;
        avc_parts.frame_size = (size_t)input_size;
    }
    result.config_bytes = avc_parts.config_size;
    result.frame_bytes = avc_parts.frame_size;
    socket_fd = connect_argument(argv[1]);
    if (socket_fd < 0) {
        result.stage = "connect";
        result.error_number = errno;
        goto done;
    }
    deadline = monotonic_ms();
    if (deadline == UINT64_MAX || deadline > UINT64_MAX - SMOKE_DEADLINE_MS) {
        result.stage = "clock";
        result.error_number = EOVERFLOW;
        goto done;
    }
    deadline += SMOKE_DEADLINE_MS;

#define PREPARE_CALL(name) \
    do { if (apply_remaining_timeout(socket_fd, deadline) < 0) { \
        call_failed(&result, (name), -1); goto cleanup; } } while (0)

    PREPARE_CALL("hello");
    {
    uint64_t required_features = ADVC_FEATURE_MEMFD | ADVC_FEATURE_DECODE;
    if (config.transport == ADVC_TRANSPORT_AHARDWAREBUFFER)
        required_features |= ADVC_FEATURE_AHARDWAREBUFFER | ADVC_FEATURE_NATIVE_FENCE;
    if (getenv("ADVC_SMOKE_ASYNC_RESERVE") != NULL)
        required_features |= ADVC_FEATURE_ASYNC_DECODE_PRIME;
#if defined(ADVC_TURNIP_VALIDATE)
    if (getenv("ADVC_SMOKE_PRIME") != NULL)
        required_features |= ADVC_FEATURE_DECODE_PRIME;
#endif
    if (advc_client_hello(socket_fd, required_features,
                          &features, &max_payload) < 0) {
        call_failed(&result, "hello", -1);
        goto cleanup;
    }
    if ((features & required_features) != required_features ||
        max_payload < ADVC_QUEUE_INPUT_SIZE) {
        errno = EPROTO;
        call_failed(&result, "hello", -1);
        goto cleanup;
    }
    }
    PREPARE_CALL("create");
    status = advc_client_create_session(socket_fd, &config, &session_id, &detail);
    if (status != ADVC_STATUS_OK) {
        call_failed(&result, "create", status);
        goto cleanup;
    }

    if (is_avc && avc_parts.config_size > 0) {
        memset(&input, 0, sizeof(input));
        input.data_fd = input_memfd;
        input.data_offset = avc_parts.config_offset;
        input.size = avc_parts.config_size;
        input.buffer_id = next_buffer_id++;
        input.flags = ADVC_FLAG_CODEC_CONFIG;
        if (queue_with_drain(socket_fd, session_id, &input, &result, deadline,
                             &detail, "queue_config") < 0)
            goto cleanup;
    }

    {
        uint32_t frames_to_queue = stream_avc ? (uint32_t)avc_stream.frame_count
                                              : repeat_frames;
    for (uint32_t frame = 0; frame < frames_to_queue; ++frame) {
        if (getenv("ADVC_SMOKE_ASYNC_RESERVE") != NULL) {
            struct advc_dmabuf_descriptor reserved;
            memset(&reserved, 0, sizeof(reserved));
            for (uint32_t object = 0; object < ADVC_MAX_DMABUF_OBJECTS;
                 ++object)
                reserved.objects[object].fd = -1;
            status = advc_client_reserve_linear(
                socket_fd, session_id,
                (uint64_t)frame * UINT64_C(16666667), config.width,
                config.height, &reserved, &detail);
            if (status != ADVC_STATUS_OK) {
                advc_dmabuf_descriptor_close(&reserved);
                call_failed(&result, "reserve_linear", status);
                goto cleanup;
            }
            ++result.async_reservations;
            advc_dmabuf_descriptor_close(&reserved);
        }
        memset(&input, 0, sizeof(input));
        input.data_fd = input_memfd;
        input.data_offset = stream_avc ? avc_stream.frames[frame].offset
                                       : avc_parts.frame_offset;
        input.size = stream_avc ? avc_stream.frames[frame].size
                                : avc_parts.frame_size;
        input.buffer_id = next_buffer_id++;
        input.pts_ns = (uint64_t)frame * UINT64_C(16666667);
        input.flags = (!stream_avc || avc_stream.frames[frame].key_frame)
                          ? ADVC_FLAG_KEY_FRAME
                          : 0;
        if (queue_with_drain(socket_fd, session_id, &input, &result, deadline,
                             &detail, "queue_input") < 0)
            goto cleanup;
    }
    repeat_frames = frames_to_queue;
    }

    memset(&input, 0, sizeof(input));
    input.data_fd = -1;
    input.buffer_id = next_buffer_id;
    input.pts_ns = (uint64_t)repeat_frames * UINT64_C(16666667);
    input.flags = ADVC_FLAG_END_OF_STREAM;
    if (queue_with_drain(socket_fd, session_id, &input, &result, deadline,
                         &detail, "queue_eos") < 0)
        goto cleanup;

    while (!result.eos) {
        int consumed = consume_one_output(socket_fd, session_id, &result, deadline,
                                          &detail);
        if (consumed < 0) goto cleanup;
        if (consumed == 0) {
            if (sleep_until_retry(deadline) < 0) {
                call_failed(&result, "deadline", -1);
                goto cleanup;
            }
        }
    }
    if (result.nonempty_outputs == 0) {
        result.stage = "no_frame";
        result.status = ADVC_STATUS_CODEC_ERROR;
        goto cleanup;
    }
    PREPARE_CALL("flush");
    status = advc_client_flush(socket_fd, session_id, &detail);
    if (status != ADVC_STATUS_OK) {
        call_failed(&result, "flush", status);
        goto cleanup;
    }
    PREPARE_CALL("close");
    status = advc_client_close_session(socket_fd, session_id, &detail);
    if (status != ADVC_STATUS_OK) {
        call_failed(&result, "close", status);
        goto cleanup;
    }
    session_id = 0;
    result.stage = "complete";
    result.status = ADVC_STATUS_OK;
    result.error_number = 0;
    rc = EXIT_SUCCESS;

cleanup:
#undef PREPARE_CALL
done:
#ifdef __ANDROID__
    advc_ahb_prime_mapper_destroy(prime_mapper);
    prime_mapper = NULL;
#endif
    if (socket_fd >= 0) close(socket_fd);
    if (input_memfd >= 0) close(input_memfd);
    printf("{\"ok\":%s,\"stage\":\"%s\",\"status\":%d,\"errno\":%d,"
           "\"outputs\":%" PRIu32 ",\"nonempty_outputs\":%" PRIu32 ","
           "\"ahb_outputs\":%" PRIu32 ","
           "\"bytes\":%" PRIu64 ",\"config_bytes\":%" PRIu64 ","
           "\"frame_bytes\":%" PRIu64 ",\"eos\":%s,\"width\":%" PRIu32 ","
           "\"height\":%" PRIu32 ",\"validation_reason\":\"%s\","
           "\"wire_format\":%" PRIu32 ",\"wire_stride\":%" PRIu32 ","
           "\"wire_slice_height\":%" PRIu32 ","
           "\"wire_layers\":%" PRIu32 ",\"wire_usage\":%" PRIu64 ","
           "\"ahb_format\":%" PRIu32 ",\"ahb_width\":%" PRIu32 ","
           "\"ahb_height\":%" PRIu32 ",\"ahb_stride\":%" PRIu32 ","
           "\"ahb_layers\":%" PRIu32 ",\"ahb_usage\":%" PRIu64 ","
           "\"prime_exports\":%" PRIu32 ",\"prime_fourcc\":%" PRIu32 ","
           "\"prime_modifier\":%" PRIu64 ","
           "\"prime_allocation_size\":%" PRIu64 ","
           "\"prime_objects\":%" PRIu32 ",\"prime_planes\":%" PRIu32 ","
           "\"prime_plane_offsets\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64
           ",%" PRIu64 "],\"prime_plane_strides\":[%" PRIu32 ",%" PRIu32
           ",%" PRIu32 ",%" PRIu32 "],"
           "\"prime_crop_count\":%" PRIu32 ","
           "\"prime_crop_left\":%" PRIu32 ",\"prime_crop_top\":%" PRIu32 ","
           "\"prime_crop_width\":%" PRIu32 ","
           "\"prime_crop_height\":%" PRIu32 ","
           "\"release_fences\":%" PRIu32 ",\"release_calls\":%" PRIu32 ","
           "\"async_reservations\":%" PRIu32 ","
           "\"turnip_content_hash\":%" PRIu64 ","
           "\"turnip_content_bytes\":%" PRIu64 ","
           "\"turnip_distinct_values\":%" PRIu32 ","
           "\"prime_errno\":%d,"
           "\"prime_status\":\"%s\",\"prime_transport_fds\":%" PRIu32 ","
           "\"prime_transport_ints\":%" PRIu32 ","
           "\"prime_transport_fd_sizes\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64
           ",%" PRIu64 "],"
           "\"prime_mapper_width\":%" PRIu64 ","
           "\"prime_mapper_height\":%" PRIu64 ","
           "\"prime_mapper_layers\":%" PRIu64 ","
           "\"prime_mapper_stride\":%" PRIu32 ","
           "\"prime_qti_data_fd_value\":%" PRId32 ","
           "\"prime_qti_data_fd_query_errno\":%" PRId32 ","
           "\"prime_qti_data_fd_transport_index\":%" PRId32 ","
           "\"prime_qti_data_fd_valid\":%" PRIu32 ","
           "\"prime_qti_data_fd_size\":%" PRIu64 "}\n",
           rc == EXIT_SUCCESS ? "true" : "false", result.stage, result.status,
           result.error_number, result.outputs, result.nonempty_outputs,
           result.ahb_outputs, result.bytes,
           result.config_bytes, result.frame_bytes, result.eos ? "true" : "false",
           result.width, result.height,
           result.validation_reason != NULL ? result.validation_reason : "none",
           result.wire_format, result.wire_stride, result.wire_slice_height,
           result.wire_layers,
           result.wire_usage, result.ahb_format, result.ahb_width,
           result.ahb_height, result.ahb_stride, result.ahb_layers,
           result.ahb_usage, result.prime_exports,
           result.prime_fourcc, result.prime_modifier,
           result.prime_allocation_size, result.prime_objects,
           result.prime_planes, result.prime_plane_offsets[0],
           result.prime_plane_offsets[1], result.prime_plane_offsets[2],
           result.prime_plane_offsets[3], result.prime_plane_strides[0],
           result.prime_plane_strides[1], result.prime_plane_strides[2],
           result.prime_plane_strides[3], result.prime_crop_count,
           result.prime_crop_left,
           result.prime_crop_top, result.prime_crop_width,
           result.prime_crop_height, result.release_fences,
            result.release_calls, result.async_reservations,
            result.turnip_content_hash,
           result.turnip_content_bytes, result.turnip_distinct_values,
           result.prime_errno,
           result.prime_status != NULL ? result.prime_status : "unknown",
           result.prime_transport_fds, result.prime_transport_ints,
           result.prime_transport_fd_sizes[0],
           result.prime_transport_fd_sizes[1],
           result.prime_transport_fd_sizes[2],
           result.prime_transport_fd_sizes[3],
           result.prime_mapper_width, result.prime_mapper_height,
           result.prime_mapper_layers, result.prime_mapper_stride,
           result.prime_qti_data_fd_value,
           result.prime_qti_data_fd_query_errno,
           result.prime_qti_data_fd_transport_index,
           result.prime_qti_data_fd_valid,
           result.prime_qti_data_fd_size);
    return rc;
}
