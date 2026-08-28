#include "advc/dmabuf_ingress.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#if defined(__has_include)
#if __has_include(<linux/sync_file.h>)
#include <linux/sync_file.h>
#define ADVC_HAVE_SYNC_FILE_INFO 1
#endif
#endif

#if !defined(ADVC_HAVE_SYNC_FILE_INFO)
/* UAPI layout from linux/sync_file.h, provided here for Android NDK sysroots. */
struct sync_file_info {
    char name[32];
    int32_t status;
    uint32_t flags;
    uint32_t num_fences;
    uint32_t pad;
    uint64_t sync_fence_info;
};
#define ADVC_SYNC_IOC_MAGIC '>'
#define SYNC_IOC_FILE_INFO \
    _IOWR(ADVC_SYNC_IOC_MAGIC, 4, struct sync_file_info)
#endif

struct advc_dmabuf_entry {
    struct advc_dmabuf_descriptor descriptor;
    int in_flight;
    int in_use;
};

struct advc_dmabuf_registry {
    advc_dmabuf_format_allowed_fn allowed;
    void *userdata;
    struct advc_dmabuf_entry entries[ADVC_MAX_REGISTERED_DMABUFS];
    size_t registered_count;
    size_t inflight_count;
};

static int bytes_are_zero(const uint8_t *p, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        if (p[i] != 0) return 0;
    }
    return 1;
}

static int valid_cloexec_fd(int fd) {
    int flags;
    if (fd < 0) return 0;
    flags = fcntl(fd, F_GETFD);
    return flags >= 0 && (flags & FD_CLOEXEC) != 0;
}

int advc_dmabuf_sync_file_validate(int fd) {
    if (!valid_cloexec_fd(fd)) {
        errno = EBADF;
        return -1;
    }
    {
        struct sync_file_info info;
        memset(&info, 0, sizeof(info));
        if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) < 0) {
            if (errno == 0) errno = EINVAL;
            return -1;
        }
    }
    return 0;
}

static int valid_color(const struct advc_dmabuf_descriptor *d) {
    if (d->color_primaries > ADVC_COLOR_PRIMARIES_BT2020 ||
        d->color_transfer > ADVC_COLOR_TRANSFER_HLG ||
        d->color_matrix > ADVC_COLOR_MATRIX_BT2020 ||
        d->color_range > ADVC_COLOR_RANGE_LIMITED ||
        d->chroma_horizontal > ADVC_CHROMA_SITING_MIDPOINT ||
        d->chroma_vertical > ADVC_CHROMA_SITING_MIDPOINT)
        return 0;
    if (d->color_matrix == ADVC_COLOR_MATRIX_RGB &&
        (d->chroma_horizontal != ADVC_CHROMA_SITING_UNSPECIFIED ||
         d->chroma_vertical != ADVC_CHROMA_SITING_UNSPECIFIED))
        return 0;
    return 1;
}

int advc_dmabuf_descriptor_validate(
    const struct advc_dmabuf_descriptor *d) {
    uint64_t crop_right;
    uint64_t crop_bottom;

    if (d == NULL || d->buffer_id == 0 || d->width < 16 || d->width > 8192 ||
        d->height < 16 || d->height > 8192 || d->drm_fourcc == 0 ||
        d->drm_modifier == UINT64_MAX ||
        d->explicit_flags != ADVC_DMABUF_EXPLICIT_ALL ||
        d->object_count == 0 || d->object_count > ADVC_MAX_DMABUF_OBJECTS ||
        d->plane_count == 0 || d->plane_count > ADVC_MAX_DMABUF_PLANES ||
        d->crop_width == 0 || d->crop_height == 0 || !valid_color(d)) {
        errno = EINVAL;
        return -1;
    }
    crop_right = (uint64_t)d->crop_left + d->crop_width;
    crop_bottom = (uint64_t)d->crop_top + d->crop_height;
    if (crop_right > d->width || crop_bottom > d->height) {
        errno = EINVAL;
        return -1;
    }
    for (uint32_t i = 0; i < d->object_count; ++i) {
        if (!valid_cloexec_fd(d->objects[i].fd) || d->objects[i].size == 0 ||
            d->objects[i].size > ADVC_MAX_DMABUF_OBJECT_BYTES) {
            errno = EINVAL;
            return -1;
        }
        for (uint32_t j = 0; j < i; ++j) {
            if (d->objects[j].fd == d->objects[i].fd) {
                errno = EINVAL;
                return -1;
            }
        }
    }
    for (uint32_t i = d->object_count; i < ADVC_MAX_DMABUF_OBJECTS; ++i) {
        if (d->objects[i].fd != -1 || d->objects[i].size != 0) {
            errno = EINVAL;
            return -1;
        }
    }
    for (uint32_t i = 0; i < d->plane_count; ++i) {
        const struct advc_dmabuf_plane *plane = &d->planes[i];
        uint64_t object_size;
        if (plane->object_index >= d->object_count || plane->pitch == 0 ||
            plane->pitch > ADVC_MAX_DMABUF_PITCH) {
            errno = EINVAL;
            return -1;
        }
        object_size = d->objects[plane->object_index].size;
        /*
         * The generic contract cannot infer a chroma plane's row count or a
         * modifier's metadata layout. It can still require one complete declared
         * row inside the object; the exact-format policy and real importer must
         * validate the remaining format-specific extent.
         */
        if (plane->offset >= object_size || plane->pitch > object_size - plane->offset) {
            errno = EINVAL;
            return -1;
        }
    }
    for (uint32_t i = d->plane_count; i < ADVC_MAX_DMABUF_PLANES; ++i) {
        if (d->planes[i].object_index != 0 || d->planes[i].offset != 0 ||
            d->planes[i].pitch != 0) {
            errno = EINVAL;
            return -1;
        }
    }
    return 0;
}

void advc_dmabuf_descriptor_close(struct advc_dmabuf_descriptor *descriptor) {
    uint32_t object_count;
    if (descriptor == NULL) return;
    object_count = descriptor->object_count;
    if (object_count > ADVC_MAX_DMABUF_OBJECTS)
        object_count = ADVC_MAX_DMABUF_OBJECTS;
    for (uint32_t i = 0; i < object_count; ++i) {
        int duplicate = 0;
        for (uint32_t j = 0; j < i; ++j) {
            if (descriptor->objects[j].fd == descriptor->objects[i].fd) {
                duplicate = 1;
                break;
            }
        }
        if (!duplicate && descriptor->objects[i].fd >= 0)
            close(descriptor->objects[i].fd);
    }
    memset(descriptor, 0, sizeof(*descriptor));
    for (uint32_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        descriptor->objects[i].fd = -1;
}

int advc_dmabuf_registration_encode(
    uint8_t payload[ADVC_REGISTER_DMABUF_SIZE],
    const struct advc_dmabuf_descriptor *descriptor,
    int fds[ADVC_MAX_DMABUF_OBJECTS], uint16_t *fd_count) {
    if (payload == NULL || descriptor == NULL || fds == NULL ||
        fd_count == NULL || advc_dmabuf_descriptor_validate(descriptor) < 0)
        return -1;
    memset(payload, 0, ADVC_REGISTER_DMABUF_SIZE);
    for (uint32_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i) fds[i] = -1;
    advc_put_u64(payload + ADVC_REGISTER_DMABUF_BUFFER_ID_OFFSET,
                 descriptor->buffer_id);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_WIDTH_OFFSET,
                 descriptor->width);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_HEIGHT_OFFSET,
                 descriptor->height);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_FOURCC_OFFSET,
                 descriptor->drm_fourcc);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_FLAGS_OFFSET,
                 descriptor->explicit_flags);
    advc_put_u64(payload + ADVC_REGISTER_DMABUF_MODIFIER_OFFSET,
                 descriptor->drm_modifier);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_CROP_LEFT_OFFSET,
                 descriptor->crop_left);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_CROP_TOP_OFFSET,
                 descriptor->crop_top);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_CROP_WIDTH_OFFSET,
                 descriptor->crop_width);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_CROP_HEIGHT_OFFSET,
                 descriptor->crop_height);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_OBJECT_COUNT_OFFSET,
                 descriptor->object_count);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_PLANE_COUNT_OFFSET,
                 descriptor->plane_count);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_COLOR_PRIMARIES_OFFSET,
                 descriptor->color_primaries);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_COLOR_TRANSFER_OFFSET,
                 descriptor->color_transfer);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_COLOR_MATRIX_OFFSET,
                 descriptor->color_matrix);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_COLOR_RANGE_OFFSET,
                 descriptor->color_range);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_CHROMA_HORIZONTAL_OFFSET,
                 descriptor->chroma_horizontal);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_CHROMA_VERTICAL_OFFSET,
                 descriptor->chroma_vertical);
    for (uint32_t i = 0; i < descriptor->object_count; ++i) {
        uint8_t *record = payload + ADVC_REGISTER_DMABUF_OBJECTS_OFFSET +
                          i * ADVC_REGISTER_DMABUF_OBJECT_STRIDE;
        advc_put_u32(record + ADVC_REGISTER_DMABUF_OBJECT_FD_INDEX_OFFSET, i);
        advc_put_u64(record + ADVC_REGISTER_DMABUF_OBJECT_SIZE_OFFSET,
                     descriptor->objects[i].size);
        fds[i] = descriptor->objects[i].fd;
    }
    for (uint32_t i = 0; i < descriptor->plane_count; ++i) {
        uint8_t *record = payload + ADVC_REGISTER_DMABUF_PLANES_OFFSET +
                          i * ADVC_REGISTER_DMABUF_PLANE_STRIDE;
        advc_put_u32(record + ADVC_REGISTER_DMABUF_PLANE_OBJECT_INDEX_OFFSET,
                     descriptor->planes[i].object_index);
        advc_put_u64(record + ADVC_REGISTER_DMABUF_PLANE_OFFSET_OFFSET,
                     descriptor->planes[i].offset);
        advc_put_u32(record + ADVC_REGISTER_DMABUF_PLANE_PITCH_OFFSET,
                     descriptor->planes[i].pitch);
    }
    *fd_count = (uint16_t)descriptor->object_count;
    return 0;
}

int advc_dmabuf_registration_decode(const uint8_t *payload, size_t payload_size,
                                    const int *fds, uint16_t fd_count,
                                    struct advc_dmabuf_descriptor *d) {
    uint32_t fd_indices = 0;

    if (payload == NULL || d == NULL || payload_size != ADVC_REGISTER_DMABUF_SIZE ||
        fd_count > ADVC_MAX_DMABUF_OBJECTS || (fd_count > 0 && fds == NULL)) {
        errno = EINVAL;
        return -1;
    }
    memset(d, 0, sizeof(*d));
    for (size_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i) d->objects[i].fd = -1;
    d->buffer_id = advc_get_u64(payload + ADVC_REGISTER_DMABUF_BUFFER_ID_OFFSET);
    d->width = advc_get_u32(payload + ADVC_REGISTER_DMABUF_WIDTH_OFFSET);
    d->height = advc_get_u32(payload + ADVC_REGISTER_DMABUF_HEIGHT_OFFSET);
    d->drm_fourcc = advc_get_u32(payload + ADVC_REGISTER_DMABUF_FOURCC_OFFSET);
    d->explicit_flags = advc_get_u32(payload + ADVC_REGISTER_DMABUF_FLAGS_OFFSET);
    d->drm_modifier = advc_get_u64(payload + ADVC_REGISTER_DMABUF_MODIFIER_OFFSET);
    d->crop_left = advc_get_u32(payload + ADVC_REGISTER_DMABUF_CROP_LEFT_OFFSET);
    d->crop_top = advc_get_u32(payload + ADVC_REGISTER_DMABUF_CROP_TOP_OFFSET);
    d->crop_width = advc_get_u32(payload + ADVC_REGISTER_DMABUF_CROP_WIDTH_OFFSET);
    d->crop_height = advc_get_u32(payload + ADVC_REGISTER_DMABUF_CROP_HEIGHT_OFFSET);
    d->object_count = advc_get_u32(payload + ADVC_REGISTER_DMABUF_OBJECT_COUNT_OFFSET);
    d->plane_count = advc_get_u32(payload + ADVC_REGISTER_DMABUF_PLANE_COUNT_OFFSET);
    d->color_primaries = advc_get_u32(payload + ADVC_REGISTER_DMABUF_COLOR_PRIMARIES_OFFSET);
    d->color_transfer = advc_get_u32(payload + ADVC_REGISTER_DMABUF_COLOR_TRANSFER_OFFSET);
    d->color_matrix = advc_get_u32(payload + ADVC_REGISTER_DMABUF_COLOR_MATRIX_OFFSET);
    d->color_range = advc_get_u32(payload + ADVC_REGISTER_DMABUF_COLOR_RANGE_OFFSET);
    d->chroma_horizontal = advc_get_u32(payload + ADVC_REGISTER_DMABUF_CHROMA_HORIZONTAL_OFFSET);
    d->chroma_vertical = advc_get_u32(payload + ADVC_REGISTER_DMABUF_CHROMA_VERTICAL_OFFSET);
    if (!bytes_are_zero(payload + 80, 16) || d->object_count != fd_count ||
        d->object_count == 0 || d->object_count > ADVC_MAX_DMABUF_OBJECTS ||
        d->plane_count == 0 || d->plane_count > ADVC_MAX_DMABUF_PLANES) {
        errno = EINVAL;
        return -1;
    }
    for (uint32_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i) {
        const uint8_t *record = payload + ADVC_REGISTER_DMABUF_OBJECTS_OFFSET +
                                i * ADVC_REGISTER_DMABUF_OBJECT_STRIDE;
        if (i < d->object_count) {
            uint32_t fd_index = advc_get_u32(
                record + ADVC_REGISTER_DMABUF_OBJECT_FD_INDEX_OFFSET);
            if (!bytes_are_zero(record + 4, 4) || fd_index >= fd_count ||
                (fd_indices & (UINT32_C(1) << fd_index)) != 0) {
                errno = EINVAL;
                return -1;
            }
            fd_indices |= UINT32_C(1) << fd_index;
            d->objects[i].fd = fds[fd_index];
            d->objects[i].size = advc_get_u64(
                record + ADVC_REGISTER_DMABUF_OBJECT_SIZE_OFFSET);
        } else if (!bytes_are_zero(record, ADVC_REGISTER_DMABUF_OBJECT_STRIDE)) {
            errno = EINVAL;
            return -1;
        }
    }
    for (uint32_t i = 0; i < ADVC_MAX_DMABUF_PLANES; ++i) {
        const uint8_t *record = payload + ADVC_REGISTER_DMABUF_PLANES_OFFSET +
                                i * ADVC_REGISTER_DMABUF_PLANE_STRIDE;
        if (i < d->plane_count) {
            if (!bytes_are_zero(record + 4, 4) || !bytes_are_zero(record + 20, 4)) {
                errno = EINVAL;
                return -1;
            }
            d->planes[i].object_index = advc_get_u32(
                record + ADVC_REGISTER_DMABUF_PLANE_OBJECT_INDEX_OFFSET);
            d->planes[i].offset = advc_get_u64(
                record + ADVC_REGISTER_DMABUF_PLANE_OFFSET_OFFSET);
            d->planes[i].pitch = advc_get_u32(
                record + ADVC_REGISTER_DMABUF_PLANE_PITCH_OFFSET);
        } else if (!bytes_are_zero(record, ADVC_REGISTER_DMABUF_PLANE_STRIDE)) {
            errno = EINVAL;
            return -1;
        }
    }
    return advc_dmabuf_descriptor_validate(d);
}

int advc_dmabuf_submission_decode(const uint8_t *payload, size_t payload_size,
                                  const int *fds, uint16_t fd_count,
                                  struct advc_dmabuf_submission *submission) {
    uint32_t role;
    if (payload == NULL || submission == NULL ||
        payload_size != ADVC_QUEUE_DMABUF_SIZE || fd_count > 1 ||
        (fd_count == 1 && fds == NULL)) {
        errno = EINVAL;
        return -1;
    }
    role = advc_get_u32(payload + ADVC_QUEUE_DMABUF_FENCE_ROLE_OFFSET);
    if (advc_get_u64(payload + ADVC_QUEUE_DMABUF_BUFFER_ID_OFFSET) == 0 ||
        advc_get_u64(payload + ADVC_QUEUE_DMABUF_PTS_NS_OFFSET) > (uint64_t)INT64_MAX ||
        advc_get_u32(payload + ADVC_QUEUE_DMABUF_FLAGS_OFFSET) != 0 ||
        !bytes_are_zero(payload + 24, 8) ||
        (fd_count == 0 && role != ADVC_FD_NONE) ||
        (fd_count == 1 && role != ADVC_FD_ACQUIRE_FENCE) ||
        (fd_count == 1 && advc_dmabuf_sync_file_validate(fds[0]) < 0)) {
        errno = EINVAL;
        return -1;
    }
    submission->buffer_id = advc_get_u64(payload + ADVC_QUEUE_DMABUF_BUFFER_ID_OFFSET);
    submission->pts_ns = advc_get_u64(payload + ADVC_QUEUE_DMABUF_PTS_NS_OFFSET);
    submission->acquire_fence_fd = fd_count == 1 ? fds[0] : -1;
    return 0;
}

int advc_dmabuf_unregister_decode(const uint8_t *payload, size_t payload_size,
                                  uint16_t fd_count, uint64_t *buffer_id) {
    if (payload == NULL || buffer_id == NULL ||
        payload_size != ADVC_UNREGISTER_DMABUF_SIZE || fd_count != 0) {
        errno = EINVAL;
        return -1;
    }
    *buffer_id = advc_get_u64(payload + ADVC_UNREGISTER_DMABUF_BUFFER_ID_OFFSET);
    if (*buffer_id == 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int advc_dmabuf_completion_validate(const uint8_t *payload, size_t payload_size,
                                    const int *fds, uint16_t fd_count) {
    uint32_t role;
    uint32_t status;
    if (payload == NULL || payload_size != ADVC_COMPLETE_DMABUF_SIZE ||
        fd_count > 1 || (fd_count == 1 && fds == NULL)) {
        errno = EINVAL;
        return -1;
    }
    role = advc_get_u32(payload + ADVC_COMPLETE_DMABUF_FENCE_ROLE_OFFSET);
    status = advc_get_u32(payload + ADVC_COMPLETE_DMABUF_STATUS_OFFSET);
    if (advc_get_u64(payload + ADVC_COMPLETE_DMABUF_BUFFER_ID_OFFSET) == 0 ||
        status > ADVC_STATUS_INTERNAL || !bytes_are_zero(payload + 20, 4) ||
        (fd_count == 0 && role != ADVC_FD_NONE) ||
        (fd_count == 1 && (role != ADVC_FD_RELEASE_FENCE ||
                           status != ADVC_STATUS_OK ||
                           advc_dmabuf_sync_file_validate(fds[0]) < 0))) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static struct advc_dmabuf_entry *find_entry(struct advc_dmabuf_registry *registry,
                                             uint64_t id) {
    for (size_t i = 0; i < ADVC_MAX_REGISTERED_DMABUFS; ++i) {
        if (registry->entries[i].in_use &&
            registry->entries[i].descriptor.buffer_id == id)
            return &registry->entries[i];
    }
    return NULL;
}

static void close_descriptor(struct advc_dmabuf_descriptor *descriptor) {
    for (uint32_t i = 0; i < descriptor->object_count; ++i) {
        if (descriptor->objects[i].fd >= 0) close(descriptor->objects[i].fd);
        descriptor->objects[i].fd = -1;
    }
}

struct advc_dmabuf_registry *advc_dmabuf_registry_create(
    advc_dmabuf_format_allowed_fn allowed, void *userdata) {
    struct advc_dmabuf_registry *registry;
    if (allowed == NULL) {
        errno = EINVAL;
        return NULL;
    }
    registry = calloc(1, sizeof(*registry));
    if (registry == NULL) return NULL;
    registry->allowed = allowed;
    registry->userdata = userdata;
    return registry;
}

void advc_dmabuf_registry_destroy(struct advc_dmabuf_registry *registry) {
    if (registry == NULL) return;
    for (size_t i = 0; i < ADVC_MAX_REGISTERED_DMABUFS; ++i) {
        if (registry->entries[i].in_use)
            close_descriptor(&registry->entries[i].descriptor);
    }
    free(registry);
}

int advc_dmabuf_registry_register(
    struct advc_dmabuf_registry *registry,
    const struct advc_dmabuf_descriptor *descriptor) {
    struct advc_dmabuf_entry *entry = NULL;
    int allowed;
    if (registry == NULL || advc_dmabuf_descriptor_validate(descriptor) < 0)
        return -1;
    if (find_entry(registry, descriptor->buffer_id) != NULL) {
        errno = EEXIST;
        return -1;
    }
    if (registry->registered_count >= ADVC_MAX_REGISTERED_DMABUFS) {
        errno = ENOSPC;
        return -1;
    }
    allowed = registry->allowed(registry->userdata, descriptor);
    if (allowed <= 0) {
        if (allowed == 0) errno = ENOTSUP;
        else if (errno == 0) errno = EIO;
        return -1;
    }
    for (size_t i = 0; i < ADVC_MAX_REGISTERED_DMABUFS; ++i) {
        if (!registry->entries[i].in_use) {
            entry = &registry->entries[i];
            break;
        }
    }
    if (entry == NULL) {
        errno = ENOSPC;
        return -1;
    }
    memset(entry, 0, sizeof(*entry));
    entry->descriptor = *descriptor;
    for (uint32_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        entry->descriptor.objects[i].fd = -1;
    for (uint32_t i = 0; i < descriptor->object_count; ++i) {
        entry->descriptor.objects[i].fd =
            fcntl(descriptor->objects[i].fd, F_DUPFD_CLOEXEC, 0);
        if (entry->descriptor.objects[i].fd < 0) {
            close_descriptor(&entry->descriptor);
            memset(entry, 0, sizeof(*entry));
            return -1;
        }
    }
    entry->in_use = 1;
    ++registry->registered_count;
    return 0;
}

int advc_dmabuf_registry_unregister(struct advc_dmabuf_registry *registry,
                                    uint64_t buffer_id) {
    struct advc_dmabuf_entry *entry;
    if (registry == NULL || buffer_id == 0) {
        errno = EINVAL;
        return -1;
    }
    entry = find_entry(registry, buffer_id);
    if (entry == NULL) {
        errno = ENOENT;
        return -1;
    }
    if (entry->in_flight) {
        errno = EBUSY;
        return -1;
    }
    close_descriptor(&entry->descriptor);
    memset(entry, 0, sizeof(*entry));
    --registry->registered_count;
    return 0;
}

int advc_dmabuf_registry_begin(struct advc_dmabuf_registry *registry,
                               const struct advc_dmabuf_submission *submission,
                               struct advc_dmabuf_job *job) {
    struct advc_dmabuf_entry *entry;
    int fence = -1;
    if (registry == NULL || submission == NULL || job == NULL ||
        submission->buffer_id == 0 || submission->pts_ns > (uint64_t)INT64_MAX ||
        submission->acquire_fence_fd < -1 ||
        (submission->acquire_fence_fd >= 0 &&
         advc_dmabuf_sync_file_validate(submission->acquire_fence_fd) < 0)) {
        errno = EINVAL;
        return -1;
    }
    memset(job, 0, sizeof(*job));
    job->acquire_fence_fd = -1;
    entry = find_entry(registry, submission->buffer_id);
    if (entry == NULL) {
        errno = ENOENT;
        return -1;
    }
    if (entry->in_flight) {
        errno = EBUSY;
        return -1;
    }
    if (registry->inflight_count >= ADVC_MAX_INFLIGHT_DMABUFS) {
        errno = EAGAIN;
        return -1;
    }
    if (submission->acquire_fence_fd >= 0) {
        fence = fcntl(submission->acquire_fence_fd, F_DUPFD_CLOEXEC, 0);
        if (fence < 0) return -1;
    }
    entry->in_flight = 1;
    ++registry->inflight_count;
    job->buffer_id = submission->buffer_id;
    job->pts_ns = submission->pts_ns;
    job->acquire_fence_fd = fence;
    job->descriptor = &entry->descriptor;
    return 0;
}

int advc_dmabuf_registry_finish(struct advc_dmabuf_registry *registry,
                                uint64_t buffer_id) {
    struct advc_dmabuf_entry *entry;
    if (registry == NULL || buffer_id == 0) {
        errno = EINVAL;
        return -1;
    }
    entry = find_entry(registry, buffer_id);
    if (entry == NULL) {
        errno = ENOENT;
        return -1;
    }
    if (!entry->in_flight) {
        errno = EALREADY;
        return -1;
    }
    entry->in_flight = 0;
    --registry->inflight_count;
    return 0;
}

void advc_dmabuf_job_close(struct advc_dmabuf_job *job) {
    if (job == NULL) return;
    if (job->acquire_fence_fd >= 0) close(job->acquire_fence_fd);
    memset(job, 0, sizeof(*job));
    job->acquire_fence_fd = -1;
    job->descriptor = NULL;
}

size_t advc_dmabuf_registry_registered_count(
    const struct advc_dmabuf_registry *registry) {
    return registry == NULL ? 0 : registry->registered_count;
}

size_t advc_dmabuf_registry_inflight_count(
    const struct advc_dmabuf_registry *registry) {
    return registry == NULL ? 0 : registry->inflight_count;
}
