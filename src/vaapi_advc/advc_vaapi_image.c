#define _GNU_SOURCE

#include "advc_vaapi_image.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define ADVC_VAAPI_IMAGE_SLOTS 64u
#define ADVC_VAAPI_IMAGE_ID_TYPE UINT32_C(0x71000000)
#define ADVC_VAAPI_IMAGE_BUFFER_ID_TYPE UINT32_C(0x72000000)
#define ADVC_VAAPI_IMAGE_ID_MASK UINT32_C(0x00ffffff)
#define ADVC_DRM_FORMAT_R8 UINT32_C(0x20203852)
#define ADVC_DRM_FORMAT_GR88 UINT32_C(0x38385247)

struct advc_dma_buf_sync {
    uint64_t flags;
};

#define ADVC_DMA_BUF_BASE 'b'
#define ADVC_DMA_BUF_IOCTL_SYNC \
    _IOW(ADVC_DMA_BUF_BASE, 0, struct advc_dma_buf_sync)
#define ADVC_DMA_BUF_SYNC_READ UINT64_C(1)
#define ADVC_DMA_BUF_SYNC_WRITE UINT64_C(2)
#define ADVC_DMA_BUF_SYNC_RW \
    (ADVC_DMA_BUF_SYNC_READ | ADVC_DMA_BUF_SYNC_WRITE)
#define ADVC_DMA_BUF_SYNC_START UINT64_C(0)
#define ADVC_DMA_BUF_SYNC_END UINT64_C(4)

struct advc_vaapi_image_slot {
    int used;
    int mapped;
    int derived;
    VAImage info;
    uint8_t *heap;
    size_t heap_size;
    struct advc_dmabuf_descriptor prime;
    VASurfaceID derived_surface;
    int acquire_fence_fd;
    void *mapping;
    size_t mapping_size;
};

struct advc_vaapi_image_runtime {
    pthread_mutex_t mutex;
    struct advc_vaapi_image_surface_ops ops;
    uint64_t cpu_pixel_copy_count;
    struct advc_vaapi_image_slot images[ADVC_VAAPI_IMAGE_SLOTS];
};

struct advc_object_maps {
    void *address[ADVC_MAX_DMABUF_OBJECTS];
    size_t length[ADVC_MAX_DMABUF_OBJECTS];
    uint32_t mapped_mask;
    uint32_t sync_mask;
};

static void descriptor_init(struct advc_dmabuf_descriptor *descriptor) {
    uint32_t i;
    memset(descriptor, 0, sizeof(*descriptor));
    for (i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        descriptor->objects[i].fd = -1;
}

static VAStatus status_from_errno(int error) {
    if (error == ENOMEM) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    if (error == ETIMEDOUT) return VA_STATUS_ERROR_TIMEDOUT;
    if (error == ENOTSUP || error == EOPNOTSUPP)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (error == EBUSY) return VA_STATUS_ERROR_SURFACE_BUSY;
    if (error == EINVAL || error == EOVERFLOW || error == EBADF)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

static uint32_t make_id(uint32_t type, unsigned int index) {
    return type | (index + 1u);
}

static int id_index(uint32_t id, uint32_t type, unsigned int *index) {
    uint32_t low;
    if ((id & ~ADVC_VAAPI_IMAGE_ID_MASK) != type) return -1;
    low = id & ADVC_VAAPI_IMAGE_ID_MASK;
    if (low == 0 || low > ADVC_VAAPI_IMAGE_SLOTS) return -1;
    *index = low - 1u;
    return 0;
}

static struct advc_vaapi_image_slot *find_image(
    struct advc_vaapi_image_runtime *runtime, VAImageID image) {
    unsigned int index;
    if (runtime == NULL ||
        id_index(image, ADVC_VAAPI_IMAGE_ID_TYPE, &index) < 0 ||
        !runtime->images[index].used)
        return NULL;
    return &runtime->images[index];
}

static struct advc_vaapi_image_slot *find_buffer(
    struct advc_vaapi_image_runtime *runtime, VABufferID buffer) {
    unsigned int index;
    if (runtime == NULL ||
        id_index(buffer, ADVC_VAAPI_IMAGE_BUFFER_ID_TYPE, &index) < 0 ||
        !runtime->images[index].used ||
        runtime->images[index].info.buf != buffer)
        return NULL;
    return &runtime->images[index];
}

static int checked_mul_size(size_t a, size_t b, size_t *result) {
    if (a != 0 && b > SIZE_MAX / a) {
        errno = EOVERFLOW;
        return -1;
    }
    *result = a * b;
    return 0;
}

static int checked_add_size(size_t a, size_t b, size_t *result) {
    if (b > SIZE_MAX - a) {
        errno = EOVERFLOW;
        return -1;
    }
    *result = a + b;
    return 0;
}

static int plane_extent(uint64_t offset, uint32_t pitch, uint32_t rows,
                        uint32_t row_bytes, uint64_t object_size) {
    uint64_t last;
    if (rows == 0 || row_bytes == 0 || pitch < row_bytes ||
        offset > object_size)
        return -1;
    last = (uint64_t)pitch * (rows - 1u);
    if (last > object_size - offset ||
        row_bytes > object_size - offset - last)
        return -1;
    return 0;
}

static int validate_linear_nv12(
    const struct advc_dmabuf_descriptor *descriptor, int require_one_object) {
    uint32_t chroma_rows;
    uint32_t p;
    if (descriptor == NULL ||
        advc_dmabuf_descriptor_validate(descriptor) < 0 ||
        descriptor->drm_fourcc != VA_FOURCC_NV12 ||
        descriptor->drm_modifier != 0 || descriptor->plane_count != 2 ||
        (require_one_object && descriptor->object_count != 1) ||
        (descriptor->width & 1u) != 0 || (descriptor->height & 1u) != 0)
        return -1;
    chroma_rows = descriptor->height / 2u;
    for (p = 0; p < 2; ++p) {
        const struct advc_dmabuf_plane *plane = &descriptor->planes[p];
        uint32_t rows = p == 0 ? descriptor->height : chroma_rows;
        if (plane_extent(plane->offset, plane->pitch, rows,
                         descriptor->width,
                         descriptor->objects[plane->object_index].size) < 0)
            return -1;
    }
    return 0;
}

int advc_vaapi_wait_sync_file(int fd, uint32_t timeout_ms) {
    struct pollfd poll_fd;
    int status;
    if (fd < 0 || timeout_ms == 0 ||
        advc_dmabuf_sync_file_validate(fd) < 0) {
        if (errno == 0) errno = EINVAL;
        return -1;
    }
    memset(&poll_fd, 0, sizeof(poll_fd));
    poll_fd.fd = fd;
    poll_fd.events = POLLIN;
    do {
        status = poll(&poll_fd, 1, (int)timeout_ms);
    } while (status < 0 && errno == EINTR);
    if (status == 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    if (status < 0 || (poll_fd.revents & (POLLERR | POLLNVAL)) != 0) {
        if (status >= 0) errno = EIO;
        return -1;
    }
    return 0;
}

static int runtime_wait_fence(struct advc_vaapi_image_runtime *runtime,
                              int fd) {
    if (fd < 0) return 0;
    if (runtime->ops.wait_fence != NULL)
        return runtime->ops.wait_fence(
            runtime->ops.opaque, fd, ADVC_VAAPI_IMAGE_SYNC_TIMEOUT_MS);
    return advc_vaapi_wait_sync_file(fd, ADVC_VAAPI_IMAGE_SYNC_TIMEOUT_MS);
}

static int runtime_dma_sync(struct advc_vaapi_image_runtime *runtime, int fd,
                            uint64_t flags) {
    struct advc_dma_buf_sync sync;
    if (runtime->ops.dma_buf_sync != NULL)
        return runtime->ops.dma_buf_sync(runtime->ops.opaque, fd, flags);
    memset(&sync, 0, sizeof(sync));
    sync.flags = flags;
    return ioctl(fd, ADVC_DMA_BUF_IOCTL_SYNC, &sync);
}

static void fill_format(VAImageFormat *format, uint32_t fourcc) {
    memset(format, 0, sizeof(*format));
    format->fourcc = fourcc;
    format->byte_order = VA_LSB_FIRST;
    format->bits_per_pixel = 12;
}

VAStatus advc_vaapi_image_query_formats(VAImageFormat *formats,
                                        int *num_formats) {
    if (num_formats == NULL) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (formats != NULL) {
        fill_format(&formats[0], VA_FOURCC_NV12);
        fill_format(&formats[1], VA_FOURCC_I420);
    }
    *num_formats = ADVC_VAAPI_IMAGE_FORMAT_COUNT;
    return VA_STATUS_SUCCESS;
}

struct advc_vaapi_image_runtime *advc_vaapi_image_runtime_create(
    const struct advc_vaapi_image_surface_ops *ops) {
    struct advc_vaapi_image_runtime *runtime = calloc(1, sizeof(*runtime));
    if (runtime == NULL) return NULL;
    if (pthread_mutex_init(&runtime->mutex, NULL) != 0) {
        free(runtime);
        errno = ENOMEM;
        return NULL;
    }
    if (ops != NULL) runtime->ops = *ops;
    return runtime;
}

static void unmap_slot(struct advc_vaapi_image_runtime *runtime,
                       struct advc_vaapi_image_slot *slot) {
    if (!slot->mapped) return;
    if (slot->derived) {
        (void)runtime_dma_sync(runtime, slot->prime.objects[0].fd,
                               ADVC_DMA_BUF_SYNC_END |
                                   ADVC_DMA_BUF_SYNC_RW);
        if (slot->mapping != NULL && slot->mapping != MAP_FAILED)
            munmap(slot->mapping, slot->mapping_size);
    }
    slot->mapping = NULL;
    slot->mapping_size = 0;
    slot->mapped = 0;
}

static void destroy_slot(struct advc_vaapi_image_runtime *runtime,
                         struct advc_vaapi_image_slot *slot) {
    VASurfaceID surface = slot->derived_surface;
    int derived = slot->derived;
    unmap_slot(runtime, slot);
    if (slot->acquire_fence_fd >= 0) close(slot->acquire_fence_fd);
    free(slot->heap);
    advc_dmabuf_descriptor_close(&slot->prime);
    memset(slot, 0, sizeof(*slot));
    slot->acquire_fence_fd = -1;
    if (derived && runtime->ops.release_surface != NULL)
        runtime->ops.release_surface(runtime->ops.opaque, surface,
                                     ADVC_VAAPI_SURFACE_ACCESS_READ_WRITE,
                                     1);
}

void advc_vaapi_image_runtime_destroy(
    struct advc_vaapi_image_runtime *runtime) {
    unsigned int i;
    if (runtime == NULL) return;
    pthread_mutex_lock(&runtime->mutex);
    for (i = 0; i < ADVC_VAAPI_IMAGE_SLOTS; ++i) {
        if (runtime->images[i].used) destroy_slot(runtime, &runtime->images[i]);
    }
    pthread_mutex_unlock(&runtime->mutex);
    pthread_mutex_destroy(&runtime->mutex);
    free(runtime);
}

VAStatus advc_vaapi_image_create(struct advc_vaapi_image_runtime *runtime,
                                 const VAImageFormat *format, int width,
                                 int height, VAImage *image) {
    struct advc_vaapi_image_slot *slot = NULL;
    size_t y_size;
    size_t chroma_size;
    size_t total;
    unsigned int i;
    if (runtime == NULL || format == NULL || image == NULL || width <= 0 ||
        height <= 0 || width > UINT16_MAX || height > UINT16_MAX ||
        (width & 1) != 0 || (height & 1) != 0 ||
        (format->fourcc != VA_FOURCC_NV12 &&
         format->fourcc != VA_FOURCC_I420))
        return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
    if (checked_mul_size((size_t)width, (size_t)height, &y_size) < 0 ||
        checked_mul_size((size_t)(width / 2), (size_t)(height / 2),
                         &chroma_size) < 0 ||
        checked_add_size(y_size, chroma_size * 2u, &total) < 0 ||
        total > UINT32_MAX)
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    pthread_mutex_lock(&runtime->mutex);
    for (i = 0; i < ADVC_VAAPI_IMAGE_SLOTS; ++i) {
        if (!runtime->images[i].used) {
            slot = &runtime->images[i];
            break;
        }
    }
    if (slot == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }
    memset(slot, 0, sizeof(*slot));
    descriptor_init(&slot->prime);
    slot->acquire_fence_fd = -1;
    slot->heap = calloc(1, total);
    if (slot->heap == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    slot->used = 1;
    slot->heap_size = total;
    memset(&slot->info, 0, sizeof(slot->info));
    slot->info.image_id = make_id(ADVC_VAAPI_IMAGE_ID_TYPE, i);
    slot->info.buf = make_id(ADVC_VAAPI_IMAGE_BUFFER_ID_TYPE, i);
    fill_format(&slot->info.format, format->fourcc);
    slot->info.width = (uint16_t)width;
    slot->info.height = (uint16_t)height;
    slot->info.data_size = (uint32_t)total;
    slot->info.pitches[0] = (uint32_t)width;
    slot->info.offsets[0] = 0;
    if (format->fourcc == VA_FOURCC_NV12) {
        slot->info.num_planes = 2;
        slot->info.pitches[1] = (uint32_t)width;
        slot->info.offsets[1] = (uint32_t)y_size;
    } else {
        slot->info.num_planes = 3;
        slot->info.pitches[1] = (uint32_t)(width / 2);
        slot->info.pitches[2] = (uint32_t)(width / 2);
        slot->info.offsets[1] = (uint32_t)y_size;
        slot->info.offsets[2] = (uint32_t)(y_size + chroma_size);
    }
    *image = slot->info;
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_image_derive(struct advc_vaapi_image_runtime *runtime,
                                 VASurfaceID surface, VAImage *image) {
    struct advc_dmabuf_descriptor descriptor;
    struct advc_vaapi_image_slot *slot = NULL;
    int fence_fd = -1;
    unsigned int i;
    VAStatus status;
    if (runtime == NULL || image == NULL ||
        runtime->ops.acquire_surface == NULL)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    descriptor_init(&descriptor);
    status = runtime->ops.acquire_surface(
        runtime->ops.opaque, surface, ADVC_VAAPI_SURFACE_ACCESS_READ_WRITE,
        &descriptor, &fence_fd);
    if (status != VA_STATUS_SUCCESS) {
        if (fence_fd >= 0) close(fence_fd);
        advc_dmabuf_descriptor_close(&descriptor);
        return status;
    }
    if (fence_fd < -1 || validate_linear_nv12(&descriptor, 1) < 0 ||
        descriptor.planes[0].object_index != 0 ||
        descriptor.planes[1].object_index != 0 ||
        descriptor.objects[0].size > UINT32_MAX ||
        descriptor.planes[0].offset > UINT32_MAX ||
        descriptor.planes[1].offset > UINT32_MAX ||
        descriptor.width > UINT16_MAX || descriptor.height > UINT16_MAX) {
        status = VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        goto fail;
    }
    pthread_mutex_lock(&runtime->mutex);
    for (i = 0; i < ADVC_VAAPI_IMAGE_SLOTS; ++i) {
        if (!runtime->images[i].used) {
            slot = &runtime->images[i];
            break;
        }
    }
    if (slot == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        status = VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
        goto fail;
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = 1;
    slot->derived = 1;
    slot->prime = descriptor;
    descriptor_init(&descriptor);
    slot->derived_surface = surface;
    slot->acquire_fence_fd = fence_fd;
    fence_fd = -1;
    slot->info.image_id = make_id(ADVC_VAAPI_IMAGE_ID_TYPE, i);
    slot->info.buf = make_id(ADVC_VAAPI_IMAGE_BUFFER_ID_TYPE, i);
    fill_format(&slot->info.format, VA_FOURCC_NV12);
    slot->info.width = (uint16_t)slot->prime.width;
    slot->info.height = (uint16_t)slot->prime.height;
    slot->info.data_size = (uint32_t)slot->prime.objects[0].size;
    slot->info.num_planes = 2;
    slot->info.pitches[0] = slot->prime.planes[0].pitch;
    slot->info.pitches[1] = slot->prime.planes[1].pitch;
    slot->info.offsets[0] = (uint32_t)slot->prime.planes[0].offset;
    slot->info.offsets[1] = (uint32_t)slot->prime.planes[1].offset;
    *image = slot->info;
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
fail:
    if (fence_fd >= 0) close(fence_fd);
    advc_dmabuf_descriptor_close(&descriptor);
    if (runtime->ops.release_surface != NULL)
        runtime->ops.release_surface(runtime->ops.opaque, surface,
                                     ADVC_VAAPI_SURFACE_ACCESS_READ_WRITE,
                                     0);
    return status;
}

VAStatus advc_vaapi_image_destroy(struct advc_vaapi_image_runtime *runtime,
                                  VAImageID image) {
    struct advc_vaapi_image_slot *slot;
    if (runtime == NULL) return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&runtime->mutex);
    slot = find_image(runtime, image);
    if (slot == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }
    destroy_slot(runtime, slot);
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

int advc_vaapi_image_owns_buffer(
    struct advc_vaapi_image_runtime *runtime, VABufferID buffer) {
    int result;
    if (runtime == NULL) return 0;
    pthread_mutex_lock(&runtime->mutex);
    result = find_buffer(runtime, buffer) != NULL;
    pthread_mutex_unlock(&runtime->mutex);
    return result;
}

VAStatus advc_vaapi_image_map_buffer(
    struct advc_vaapi_image_runtime *runtime, VABufferID buffer,
    void **mapped) {
    struct advc_vaapi_image_slot *slot;
    if (runtime == NULL || mapped == NULL)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    *mapped = NULL;
    pthread_mutex_lock(&runtime->mutex);
    slot = find_buffer(runtime, buffer);
    if (slot == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (slot->mapped) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (!slot->derived) {
        slot->mapped = 1;
        slot->mapping = slot->heap;
        slot->mapping_size = slot->heap_size;
        *mapped = slot->heap;
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_SUCCESS;
    }
    if (slot->acquire_fence_fd >= 0) {
        if (runtime_wait_fence(runtime, slot->acquire_fence_fd) < 0) {
            VAStatus status = status_from_errno(errno);
            pthread_mutex_unlock(&runtime->mutex);
            return status;
        }
        close(slot->acquire_fence_fd);
        slot->acquire_fence_fd = -1;
    }
    if (runtime_dma_sync(runtime, slot->prime.objects[0].fd,
                         ADVC_DMA_BUF_SYNC_START | ADVC_DMA_BUF_SYNC_RW) < 0) {
        VAStatus status = status_from_errno(errno);
        pthread_mutex_unlock(&runtime->mutex);
        return status;
    }
    slot->mapping_size = (size_t)slot->prime.objects[0].size;
    slot->mapping = mmap(NULL, slot->mapping_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, slot->prime.objects[0].fd, 0);
    if (slot->mapping == MAP_FAILED) {
        VAStatus status = status_from_errno(errno);
        (void)runtime_dma_sync(runtime, slot->prime.objects[0].fd,
                               ADVC_DMA_BUF_SYNC_END |
                                   ADVC_DMA_BUF_SYNC_RW);
        slot->mapping = NULL;
        slot->mapping_size = 0;
        pthread_mutex_unlock(&runtime->mutex);
        return status;
    }
    slot->mapped = 1;
    *mapped = slot->mapping;
    pthread_mutex_unlock(&runtime->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_image_unmap_buffer(
    struct advc_vaapi_image_runtime *runtime, VABufferID buffer) {
    struct advc_vaapi_image_slot *slot;
    int error = 0;
    if (runtime == NULL) return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&runtime->mutex);
    slot = find_buffer(runtime, buffer);
    if (slot == NULL) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (!slot->mapped) {
        pthread_mutex_unlock(&runtime->mutex);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (slot->derived) {
        if (munmap(slot->mapping, slot->mapping_size) < 0) error = errno;
        if (runtime_dma_sync(runtime, slot->prime.objects[0].fd,
                             ADVC_DMA_BUF_SYNC_END |
                                 ADVC_DMA_BUF_SYNC_RW) < 0 &&
            error == 0)
            error = errno;
    }
    slot->mapped = 0;
    slot->mapping = NULL;
    slot->mapping_size = 0;
    pthread_mutex_unlock(&runtime->mutex);
    return error == 0 ? VA_STATUS_SUCCESS : status_from_errno(error);
}

static int begin_object_maps(struct advc_vaapi_image_runtime *runtime,
                             const struct advc_dmabuf_descriptor *descriptor,
                             struct advc_object_maps *maps) {
    uint32_t i;
    memset(maps, 0, sizeof(*maps));
    for (i = 0; i < descriptor->object_count; ++i) {
        if (runtime_dma_sync(runtime, descriptor->objects[i].fd,
                             ADVC_DMA_BUF_SYNC_START |
                                 ADVC_DMA_BUF_SYNC_RW) < 0)
            goto fail;
        maps->sync_mask |= UINT32_C(1) << i;
        maps->length[i] = (size_t)descriptor->objects[i].size;
        maps->address[i] =
            mmap(NULL, maps->length[i], PROT_READ | PROT_WRITE, MAP_SHARED,
                 descriptor->objects[i].fd, 0);
        if (maps->address[i] == MAP_FAILED) {
            maps->address[i] = NULL;
            goto fail;
        }
        maps->mapped_mask |= UINT32_C(1) << i;
    }
    return 0;
fail:
    {
        int saved = errno;
        for (i = 0; i < descriptor->object_count; ++i) {
            if ((maps->mapped_mask & (UINT32_C(1) << i)) != 0)
                munmap(maps->address[i], maps->length[i]);
            if ((maps->sync_mask & (UINT32_C(1) << i)) != 0)
                (void)runtime_dma_sync(runtime, descriptor->objects[i].fd,
                                       ADVC_DMA_BUF_SYNC_END |
                                           ADVC_DMA_BUF_SYNC_RW);
        }
        memset(maps, 0, sizeof(*maps));
        errno = saved;
    }
    return -1;
}

static int end_object_maps(struct advc_vaapi_image_runtime *runtime,
                           const struct advc_dmabuf_descriptor *descriptor,
                           struct advc_object_maps *maps) {
    int error = 0;
    uint32_t i;
    for (i = 0; i < descriptor->object_count; ++i) {
        if ((maps->mapped_mask & (UINT32_C(1) << i)) != 0 &&
            munmap(maps->address[i], maps->length[i]) < 0 && error == 0)
            error = errno;
        if ((maps->sync_mask & (UINT32_C(1) << i)) != 0 &&
            runtime_dma_sync(runtime, descriptor->objects[i].fd,
                             ADVC_DMA_BUF_SYNC_END |
                                 ADVC_DMA_BUF_SYNC_RW) < 0 && error == 0)
            error = errno;
    }
    memset(maps, 0, sizeof(*maps));
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

static int copy_image_to_nv12(
    const struct advc_vaapi_image_slot *source, int src_x, int src_y,
    unsigned int width, unsigned int height,
    const struct advc_dmabuf_descriptor *destination, int dst_x, int dst_y,
    const struct advc_object_maps *maps) {
    const uint8_t *src = source->heap;
    uint8_t *dst_y_plane;
    uint8_t *dst_uv_plane;
    uint32_t row;
    if (src == NULL) {
        errno = EINVAL;
        return -1;
    }
    dst_y_plane = (uint8_t *)maps->address[
                      destination->planes[0].object_index] +
                  destination->planes[0].offset;
    dst_uv_plane = (uint8_t *)maps->address[
                       destination->planes[1].object_index] +
                   destination->planes[1].offset;
    for (row = 0; row < height; ++row) {
        const uint8_t *src_row = src + source->info.offsets[0] +
                                 (size_t)(src_y + (int)row) *
                                     source->info.pitches[0] +
                                 (size_t)src_x;
        uint8_t *dst_row = dst_y_plane +
                           (size_t)(dst_y + (int)row) *
                               destination->planes[0].pitch +
                           (size_t)dst_x;
        memcpy(dst_row, src_row, width);
    }
    if (source->info.format.fourcc == VA_FOURCC_NV12) {
        for (row = 0; row < height / 2u; ++row) {
            const uint8_t *src_row = src + source->info.offsets[1] +
                                     (size_t)(src_y / 2 + (int)row) *
                                         source->info.pitches[1] +
                                     (size_t)src_x;
            uint8_t *dst_row = dst_uv_plane +
                               (size_t)(dst_y / 2 + (int)row) *
                                   destination->planes[1].pitch +
                               (size_t)dst_x;
            memcpy(dst_row, src_row, width);
        }
    } else {
        for (row = 0; row < height / 2u; ++row) {
            const uint8_t *src_u = src + source->info.offsets[1] +
                                   (size_t)(src_y / 2 + (int)row) *
                                       source->info.pitches[1] +
                                   (size_t)(src_x / 2);
            const uint8_t *src_v = src + source->info.offsets[2] +
                                   (size_t)(src_y / 2 + (int)row) *
                                       source->info.pitches[2] +
                                   (size_t)(src_x / 2);
            uint8_t *dst_row = dst_uv_plane +
                               (size_t)(dst_y / 2 + (int)row) *
                                   destination->planes[1].pitch +
                               (size_t)dst_x;
            uint32_t column;
            for (column = 0; column < width / 2u; ++column) {
                dst_row[column * 2u] = src_u[column];
                dst_row[column * 2u + 1u] = src_v[column];
            }
        }
    }
    return 0;
}

VAStatus advc_vaapi_image_put(struct advc_vaapi_image_runtime *runtime,
                              VASurfaceID surface, VAImageID image,
                              int src_x, int src_y, unsigned int src_width,
                              unsigned int src_height, int dst_x, int dst_y,
                              unsigned int dst_width,
                              unsigned int dst_height) {
    struct advc_vaapi_image_slot *source;
    struct advc_dmabuf_descriptor destination;
    struct advc_object_maps maps;
    int fence_fd = -1;
    int acquired = 0;
    int copied = 0;
    VAStatus status;
    if (runtime == NULL || runtime->ops.acquire_surface == NULL || src_x < 0 ||
        src_y < 0 || dst_x < 0 || dst_y < 0 || src_width == 0 ||
        src_height == 0 || src_width != dst_width ||
        src_height != dst_height || ((unsigned int)src_x & 1u) != 0 ||
        ((unsigned int)src_y & 1u) != 0 || ((unsigned int)dst_x & 1u) != 0 ||
        ((unsigned int)dst_y & 1u) != 0 || (src_width & 1u) != 0 ||
        (src_height & 1u) != 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    descriptor_init(&destination);
    pthread_mutex_lock(&runtime->mutex);
    source = find_image(runtime, image);
    if (source == NULL) {
        status = VA_STATUS_ERROR_INVALID_IMAGE;
        goto done;
    }
    if (source->derived || source->mapped ||
        (uint64_t)(unsigned int)src_x + src_width > source->info.width ||
        (uint64_t)(unsigned int)src_y + src_height > source->info.height) {
        status = source->mapped ? VA_STATUS_ERROR_SURFACE_BUSY :
                                  VA_STATUS_ERROR_INVALID_PARAMETER;
        goto done;
    }
    status = runtime->ops.acquire_surface(
        runtime->ops.opaque, surface, ADVC_VAAPI_SURFACE_ACCESS_WRITE,
        &destination, &fence_fd);
    if (status != VA_STATUS_SUCCESS) goto done;
    acquired = 1;
    if (fence_fd < -1 || validate_linear_nv12(&destination, 0) < 0 ||
        (uint64_t)(unsigned int)dst_x + dst_width > destination.width ||
        (uint64_t)(unsigned int)dst_y + dst_height > destination.height) {
        status = VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        goto done;
    }
    if (fence_fd >= 0 && runtime_wait_fence(runtime, fence_fd) < 0) {
        status = status_from_errno(errno);
        goto done;
    }
    if (begin_object_maps(runtime, &destination, &maps) < 0) {
        status = status_from_errno(errno);
        goto done;
    }
    if (copy_image_to_nv12(source, src_x, src_y, src_width, src_height,
                           &destination, dst_x, dst_y, &maps) < 0) {
        int error = errno;
        (void)end_object_maps(runtime, &destination, &maps);
        status = status_from_errno(error);
        goto done;
    }
    if (end_object_maps(runtime, &destination, &maps) < 0) {
        status = status_from_errno(errno);
        goto done;
    }
    copied = 1;
    ++runtime->cpu_pixel_copy_count;
    status = VA_STATUS_SUCCESS;
done:
    if (fence_fd >= 0) close(fence_fd);
    advc_dmabuf_descriptor_close(&destination);
    if (acquired && runtime->ops.release_surface != NULL)
        runtime->ops.release_surface(runtime->ops.opaque, surface,
                                     ADVC_VAAPI_SURFACE_ACCESS_WRITE,
                                     copied);
    pthread_mutex_unlock(&runtime->mutex);
    return status;
}

uint64_t advc_vaapi_image_cpu_pixel_copy_count(
    struct advc_vaapi_image_runtime *runtime) {
    uint64_t count = 0;
    if (runtime == NULL) return 0;
    pthread_mutex_lock(&runtime->mutex);
    count = runtime->cpu_pixel_copy_count;
    pthread_mutex_unlock(&runtime->mutex);
    return count;
}

static int copy_prime_layout(const VADRMPRIMESurfaceDescriptor *prime,
                             struct advc_dmabuf_descriptor *descriptor) {
    uint32_t p;
    if (prime->num_layers == 1 &&
        prime->layers[0].drm_format == VA_FOURCC_NV12 &&
        prime->layers[0].num_planes == 2) {
        for (p = 0; p < 2; ++p) {
            descriptor->planes[p].object_index =
                prime->layers[0].object_index[p];
            descriptor->planes[p].offset = prime->layers[0].offset[p];
            descriptor->planes[p].pitch = prime->layers[0].pitch[p];
        }
        return 0;
    }
    if (prime->num_layers == 2 &&
        prime->layers[0].drm_format == ADVC_DRM_FORMAT_R8 &&
        prime->layers[1].drm_format == ADVC_DRM_FORMAT_GR88 &&
        prime->layers[0].num_planes == 1 &&
        prime->layers[1].num_planes == 1) {
        for (p = 0; p < 2; ++p) {
            descriptor->planes[p].object_index =
                prime->layers[p].object_index[0];
            descriptor->planes[p].offset = prime->layers[p].offset[0];
            descriptor->planes[p].pitch = prime->layers[p].pitch[0];
        }
        return 0;
    }
    errno = ENOTSUP;
    return -1;
}

VAStatus advc_vaapi_prime_import_nv12_linear(
    const VADRMPRIMESurfaceDescriptor *prime, uint64_t buffer_id,
    uint32_t expected_width, uint32_t expected_height,
    struct advc_dmabuf_descriptor *descriptor) {
    uint32_t i;
    if (prime == NULL || descriptor == NULL || buffer_id == 0 ||
        expected_width < 16 || expected_height < 16 ||
        expected_width > 8192 || expected_height > 8192 ||
        (expected_width & 1u) != 0 || (expected_height & 1u) != 0 ||
        prime->fourcc != VA_FOURCC_NV12 || prime->width != expected_width ||
        prime->height != expected_height || prime->num_objects == 0 ||
        prime->num_objects > ADVC_MAX_DMABUF_OBJECTS) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    descriptor_init(descriptor);
    descriptor->buffer_id = buffer_id;
    descriptor->width = expected_width;
    descriptor->height = expected_height;
    descriptor->drm_fourcc = VA_FOURCC_NV12;
    descriptor->explicit_flags = ADVC_DMABUF_EXPLICIT_ALL;
    descriptor->drm_modifier = 0;
    descriptor->crop_width = expected_width;
    descriptor->crop_height = expected_height;
    descriptor->object_count = prime->num_objects;
    descriptor->plane_count = 2;
    descriptor->color_primaries = ADVC_COLOR_PRIMARIES_BT709;
    descriptor->color_transfer = ADVC_COLOR_TRANSFER_BT709;
    descriptor->color_matrix = ADVC_COLOR_MATRIX_BT709;
    descriptor->color_range = ADVC_COLOR_RANGE_LIMITED;
    descriptor->chroma_horizontal = ADVC_CHROMA_SITING_MIDPOINT;
    descriptor->chroma_vertical = ADVC_CHROMA_SITING_MIDPOINT;
    for (i = 0; i < prime->num_objects; ++i) {
        if (prime->objects[i].fd < 0 || prime->objects[i].size == 0 ||
            prime->objects[i].drm_format_modifier != 0) {
            advc_dmabuf_descriptor_close(descriptor);
            return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        }
        descriptor->objects[i].fd =
            fcntl(prime->objects[i].fd, F_DUPFD_CLOEXEC, 0);
        if (descriptor->objects[i].fd < 0) {
            VAStatus status = status_from_errno(errno);
            advc_dmabuf_descriptor_close(descriptor);
            return status;
        }
        descriptor->objects[i].size = prime->objects[i].size;
    }
    if (copy_prime_layout(prime, descriptor) < 0 ||
        validate_linear_nv12(descriptor, 0) < 0) {
        advc_dmabuf_descriptor_close(descriptor);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    return VA_STATUS_SUCCESS;
}

VAStatus advc_vaapi_prime_import_nv12_modifier(
    const VADRMPRIMESurfaceDescriptor *prime, uint64_t buffer_id,
    uint32_t expected_width, uint32_t expected_height,
    uint64_t allowed_modifier, struct advc_dmabuf_descriptor *descriptor) {
    uint32_t i;
    const uint64_t qcom_compressed = UINT64_C(0x0500000000000001);
    if (allowed_modifier == 0)
        return advc_vaapi_prime_import_nv12_linear(
            prime, buffer_id, expected_width, expected_height, descriptor);
    if (allowed_modifier != qcom_compressed || prime == NULL ||
        descriptor == NULL || buffer_id == 0 || expected_width < 16 ||
        expected_height < 16 || expected_width > 8192 ||
        expected_height > 8192 || (expected_width & 1u) != 0 ||
        (expected_height & 1u) != 0 || prime->fourcc != VA_FOURCC_NV12 ||
        prime->width != expected_width || prime->height != expected_height ||
        prime->num_objects != 1 || prime->objects[0].fd < 0 ||
        prime->objects[0].size == 0 ||
        prime->objects[0].drm_format_modifier != allowed_modifier) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    descriptor_init(descriptor);
    descriptor->buffer_id = buffer_id;
    descriptor->width = expected_width;
    descriptor->height = expected_height;
    descriptor->drm_fourcc = VA_FOURCC_NV12;
    descriptor->explicit_flags = ADVC_DMABUF_EXPLICIT_ALL;
    descriptor->drm_modifier = allowed_modifier;
    descriptor->crop_width = expected_width;
    descriptor->crop_height = expected_height;
    descriptor->object_count = 1;
    descriptor->plane_count = 2;
    descriptor->color_primaries = ADVC_COLOR_PRIMARIES_BT709;
    descriptor->color_transfer = ADVC_COLOR_TRANSFER_BT709;
    descriptor->color_matrix = ADVC_COLOR_MATRIX_BT709;
    descriptor->color_range = ADVC_COLOR_RANGE_LIMITED;
    descriptor->chroma_horizontal = ADVC_CHROMA_SITING_MIDPOINT;
    descriptor->chroma_vertical = ADVC_CHROMA_SITING_MIDPOINT;
    descriptor->objects[0].fd =
        fcntl(prime->objects[0].fd, F_DUPFD_CLOEXEC, 0);
    if (descriptor->objects[0].fd < 0) return status_from_errno(errno);
    descriptor->objects[0].size = prime->objects[0].size;
    if (copy_prime_layout(prime, descriptor) < 0)
        goto unsupported;
    for (i = 0; i < 2; ++i) {
        if (descriptor->planes[i].object_index != 0 ||
            descriptor->planes[i].pitch == 0 ||
            descriptor->planes[i].offset >= descriptor->objects[0].size)
            goto unsupported;
    }
    if (advc_dmabuf_descriptor_validate(descriptor) < 0)
        goto unsupported;
    return VA_STATUS_SUCCESS;
unsupported:
    advc_dmabuf_descriptor_close(descriptor);
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
}

VAStatus advc_vaapi_prime_import_surface_attributes(
    unsigned int format, unsigned int width, unsigned int height,
    const VASurfaceAttrib *attributes, unsigned int num_attributes,
    unsigned int surface_index, unsigned int num_surfaces,
    uint64_t buffer_id, struct advc_dmabuf_descriptor *descriptor) {
    const VADRMPRIMESurfaceDescriptor *primes = NULL;
    int have_memory_type = 0;
    int have_external = 0;
    unsigned int i;
    if (format != VA_RT_FORMAT_YUV420 || attributes == NULL ||
        num_attributes == 0 || surface_index >= num_surfaces ||
        num_surfaces == 0 || descriptor == NULL)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    for (i = 0; i < num_attributes; ++i) {
        const VASurfaceAttrib *attribute = &attributes[i];
        if (attribute->type == VASurfaceAttribMemoryType) {
            if (have_memory_type ||
                attribute->value.type != VAGenericValueTypeInteger ||
                (uint32_t)attribute->value.value.i !=
                    VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)
                return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
            have_memory_type = 1;
        } else if (attribute->type ==
                   VASurfaceAttribExternalBufferDescriptor) {
            if (have_external ||
                attribute->value.type != VAGenericValueTypePointer ||
                attribute->value.value.p == NULL)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            primes = attribute->value.value.p;
            have_external = 1;
        } else if (attribute->type == VASurfaceAttribPixelFormat) {
            if (attribute->value.type != VAGenericValueTypeInteger ||
                (uint32_t)attribute->value.value.i != VA_FOURCC_NV12)
                return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        } else if (attribute->type == VASurfaceAttribUsageHint) {
            if (attribute->value.type != VAGenericValueTypeInteger)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
        } else if (attribute->type != VASurfaceAttribNone) {
            return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
        }
    }
    if (!have_memory_type || !have_external)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    {
        const VADRMPRIMESurfaceDescriptor *prime = &primes[surface_index];
        uint64_t modifier;
        if (prime->num_objects == 0) return VA_STATUS_ERROR_INVALID_PARAMETER;
        modifier = prime->objects[0].drm_format_modifier;
        for (i = 1; i < prime->num_objects; ++i)
            if (prime->objects[i].drm_format_modifier != modifier)
                return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        return advc_vaapi_prime_import_nv12_modifier(
            prime, buffer_id, width, height, modifier, descriptor);
    }
}

void advc_vaapi_prime_export_close(VADRMPRIMESurfaceDescriptor *prime) {
    uint32_t i;
    if (prime == NULL) return;
    for (i = 0; i < 4; ++i) {
        if (prime->objects[i].fd >= 0) close(prime->objects[i].fd);
    }
    memset(prime, 0, sizeof(*prime));
    for (i = 0; i < 4; ++i) prime->objects[i].fd = -1;
}

VAStatus advc_vaapi_prime_export_nv12(
    const struct advc_dmabuf_descriptor *descriptor, int acquire_fence_fd,
    uint32_t flags, VADRMPRIMESurfaceDescriptor *prime) {
    uint32_t access = flags & VA_EXPORT_SURFACE_READ_WRITE;
    int separate;
    int linear;
    int qcom;
    uint32_t i;
    uint32_t p;
    linear = descriptor != NULL && validate_linear_nv12(descriptor, 0) == 0;
    qcom = descriptor != NULL &&
            descriptor->drm_modifier == UINT64_C(0x0500000000000001) &&
            descriptor->drm_fourcc == VA_FOURCC_NV12 &&
            descriptor->object_count == 1 && descriptor->plane_count == 2 &&
            descriptor->planes[0].object_index == 0 &&
            descriptor->planes[1].object_index == 0 &&
            descriptor->planes[0].offset <= UINT32_MAX &&
            descriptor->planes[1].offset <= UINT32_MAX &&
            advc_dmabuf_descriptor_validate(descriptor) == 0;
    if (descriptor == NULL || prime == NULL || acquire_fence_fd < -1 ||
        (access != VA_EXPORT_SURFACE_READ_ONLY &&
         access != VA_EXPORT_SURFACE_WRITE_ONLY) ||
        (flags & ~(VA_EXPORT_SURFACE_READ_WRITE |
                   VA_EXPORT_SURFACE_SEPARATE_LAYERS |
                   VA_EXPORT_SURFACE_COMPOSED_LAYERS)) != 0 ||
        ((flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) != 0 &&
         (flags & VA_EXPORT_SURFACE_COMPOSED_LAYERS) != 0) ||
        (!linear && !qcom))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (access == VA_EXPORT_SURFACE_WRITE_ONLY && !linear)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (acquire_fence_fd >= 0 &&
        advc_vaapi_wait_sync_file(acquire_fence_fd,
                                  ADVC_VAAPI_IMAGE_SYNC_TIMEOUT_MS) < 0)
        return status_from_errno(errno);
    memset(prime, 0, sizeof(*prime));
    for (i = 0; i < 4; ++i) prime->objects[i].fd = -1;
    prime->fourcc = VA_FOURCC_NV12;
    prime->width = descriptor->width;
    prime->height = descriptor->height;
    prime->num_objects = descriptor->object_count;
    for (i = 0; i < prime->num_objects; ++i) {
        if (descriptor->objects[i].size > UINT32_MAX) goto fail;
        prime->objects[i].fd =
            fcntl(descriptor->objects[i].fd, F_DUPFD_CLOEXEC, 0);
        if (prime->objects[i].fd < 0) goto fail;
        prime->objects[i].size = (uint32_t)descriptor->objects[i].size;
        prime->objects[i].drm_format_modifier = descriptor->drm_modifier;
    }
    separate = (flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) != 0;
    if (separate) {
        prime->num_layers = 2;
        prime->layers[0].drm_format = ADVC_DRM_FORMAT_R8;
        prime->layers[1].drm_format = ADVC_DRM_FORMAT_GR88;
        for (p = 0; p < 2; ++p) {
            prime->layers[p].num_planes = 1;
            prime->layers[p].object_index[0] =
                descriptor->planes[p].object_index;
            prime->layers[p].offset[0] =
                (uint32_t)descriptor->planes[p].offset;
            prime->layers[p].pitch[0] = descriptor->planes[p].pitch;
        }
    } else {
        prime->num_layers = 1;
        prime->layers[0].drm_format = VA_FOURCC_NV12;
        prime->layers[0].num_planes = 2;
        for (p = 0; p < 2; ++p) {
            prime->layers[0].object_index[p] =
                descriptor->planes[p].object_index;
            prime->layers[0].offset[p] =
                (uint32_t)descriptor->planes[p].offset;
            prime->layers[0].pitch[p] = descriptor->planes[p].pitch;
        }
    }
    return VA_STATUS_SUCCESS;
fail:
    advc_vaapi_prime_export_close(prime);
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

void advc_vaapi_encode_surface_link_init(
    struct advc_vaapi_encode_surface_link *link,
    const struct advc_dmabuf_descriptor *descriptor) {
    if (link == NULL) return;
    memset(link, 0, sizeof(*link));
    link->descriptor = descriptor;
    link->acquire_fence_fd = -1;
    link->release_fence_fd = -1;
}

int advc_vaapi_encode_surface_link_set_acquire_fence(
    struct advc_vaapi_encode_surface_link *link, int owned_fence_fd) {
    if (link == NULL || owned_fence_fd < -1 || link->release_fence_fd >= 0) {
        errno = EINVAL;
        return -1;
    }
    if (owned_fence_fd >= 0 &&
        advc_dmabuf_sync_file_validate(owned_fence_fd) < 0)
        return -1;
    if (link->acquire_fence_fd >= 0) close(link->acquire_fence_fd);
    link->acquire_fence_fd = owned_fence_fd;
    return 0;
}

int advc_vaapi_encode_surface_link_register(
    struct advc_vaapi_encode_surface_link *link,
    struct advc_vaapi_encode_broker *broker) {
    int descriptor_ok;
    const uint64_t qcom_compressed = UINT64_C(0x0500000000000001);
    descriptor_ok = link != NULL && link->descriptor != NULL &&
                    (validate_linear_nv12(link->descriptor, 0) == 0 ||
                     (link->descriptor->drm_modifier == qcom_compressed &&
                      link->descriptor->drm_fourcc == VA_FOURCC_NV12 &&
                      link->descriptor->object_count == 1 &&
                      link->descriptor->plane_count == 2 &&
                      advc_dmabuf_descriptor_validate(link->descriptor) == 0));
    if (link == NULL || broker == NULL || link->descriptor == NULL ||
        link->registered || !descriptor_ok ||
        (link->descriptor->drm_modifier == qcom_compressed &&
         (broker->features & ADVC_FEATURE_ENCODE_QCOM_MODIFIER) == 0)) {
        errno = EINVAL;
        return -1;
    }
    if (advc_vaapi_encode_broker_register_surface(broker,
                                                   link->descriptor) < 0)
        return -1;
    link->registered = 1;
    return 0;
}

int advc_vaapi_encode_surface_link_submit(
    struct advc_vaapi_encode_surface_link *link,
    struct advc_vaapi_encode_broker *broker, uint64_t pts_ns) {
    int release_fence_fd = -1;
    int saved;
    if (link == NULL || broker == NULL || link->descriptor == NULL ||
        !link->registered || link->release_fence_fd >= 0) {
        errno = EBUSY;
        return -1;
    }
    if (advc_vaapi_encode_broker_submit_surface(
            broker, link->descriptor->buffer_id, pts_ns,
            link->acquire_fence_fd, &release_fence_fd) < 0)
        return -1;
    saved = errno;
    if (link->acquire_fence_fd >= 0) close(link->acquire_fence_fd);
    link->acquire_fence_fd = -1;
    link->release_fence_fd = release_fence_fd;
    errno = saved;
    return 0;
}

int advc_vaapi_encode_surface_link_wait(
    struct advc_vaapi_encode_surface_link *link, uint32_t timeout_ms) {
    if (link == NULL || timeout_ms == 0) {
        errno = EINVAL;
        return -1;
    }
    if (link->release_fence_fd < 0) return 0;
    if (advc_vaapi_wait_sync_file(link->release_fence_fd, timeout_ms) < 0)
        return -1;
    close(link->release_fence_fd);
    link->release_fence_fd = -1;
    return 0;
}

int advc_vaapi_encode_surface_link_take_release_fence(
    struct advc_vaapi_encode_surface_link *link) {
    int fd;
    if (link == NULL) {
        errno = EINVAL;
        return -1;
    }
    fd = link->release_fence_fd;
    link->release_fence_fd = -1;
    return fd;
}

int advc_vaapi_encode_surface_link_unregister(
    struct advc_vaapi_encode_surface_link *link,
    struct advc_vaapi_encode_broker *broker, uint32_t timeout_ms) {
    if (link == NULL || broker == NULL || !link->registered) {
        errno = EINVAL;
        return -1;
    }
    if (link->release_fence_fd >= 0 &&
        advc_vaapi_encode_surface_link_wait(link, timeout_ms) < 0)
        return -1;
    if (advc_vaapi_encode_broker_unregister_surface(
            broker, link->descriptor->buffer_id) < 0)
        return -1;
    link->registered = 0;
    return 0;
}

void advc_vaapi_encode_surface_link_close(
    struct advc_vaapi_encode_surface_link *link) {
    if (link == NULL) return;
    if (link->acquire_fence_fd >= 0) close(link->acquire_fence_fd);
    if (link->release_fence_fd >= 0) close(link->release_fence_fd);
    memset(link, 0, sizeof(*link));
    link->acquire_fence_fd = -1;
    link->release_fence_fd = -1;
}
