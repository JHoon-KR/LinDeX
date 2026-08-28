#include "advc/ahb_prime_mapper.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct advc_ahb_prime_mapper {
    struct advc_ahb_prime_mapper_ops ops;
    void *userdata;
};

static void initialize_export(struct advc_ahb_prime_export *exported) {
    memset(exported, 0, sizeof(*exported));
    exported->acquire_fence_fd = -1;
    for (size_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        exported->descriptor.objects[i].fd = -1;
}

void advc_ahb_prime_export_close(struct advc_ahb_prime_export *exported) {
    if (exported == NULL) return;
    for (uint32_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i) {
        if (exported->descriptor.objects[i].fd >= 0)
            close(exported->descriptor.objects[i].fd);
    }
    if (exported->acquire_fence_fd >= 0) close(exported->acquire_fence_fd);
    initialize_export(exported);
}

struct advc_ahb_prime_mapper *advc_ahb_prime_mapper_create(
    const struct advc_ahb_prime_mapper_ops *ops, void *userdata) {
    struct advc_ahb_prime_mapper *mapper;
    if (ops == NULL || ops->export_prime == NULL || ops->release == NULL) {
        errno = EINVAL;
        return NULL;
    }
    mapper = (struct advc_ahb_prime_mapper *)calloc(1, sizeof(*mapper));
    if (mapper == NULL) return NULL;
    mapper->ops = *ops;
    mapper->userdata = userdata;
    return mapper;
}

void advc_ahb_prime_mapper_destroy(struct advc_ahb_prime_mapper *mapper) {
    if (mapper != NULL && mapper->ops.destroy != NULL)
        mapper->ops.destroy(mapper->userdata);
    free(mapper);
}

int advc_ahb_prime_mapper_export(
    struct advc_ahb_prime_mapper *mapper, void *hardware_buffer,
    const struct advc_ahb_public_metadata *public_metadata, uint64_t buffer_id,
    int acquire_fence_fd, struct advc_ahb_prime_export *exported) {
    int duplicate = -1;
    if (exported == NULL) {
        errno = EINVAL;
        return -1;
    }
    initialize_export(exported);
    if (mapper == NULL || hardware_buffer == NULL || public_metadata == NULL ||
        buffer_id == 0 || public_metadata->width == 0 ||
        public_metadata->height == 0 || public_metadata->layers != 1 ||
        public_metadata->stride == 0 || public_metadata->crop_width == 0 ||
        public_metadata->crop_height == 0 ||
        (uint64_t)public_metadata->crop_left + public_metadata->crop_width >
            public_metadata->width ||
        (uint64_t)public_metadata->crop_top + public_metadata->crop_height >
            public_metadata->height || acquire_fence_fd < -1 ||
        (acquire_fence_fd >= 0 &&
         advc_dmabuf_sync_file_validate(acquire_fence_fd) < 0)) {
        errno = EINVAL;
        return -1;
    }
    if (mapper->ops.export_prime(mapper->userdata, hardware_buffer,
                                 public_metadata,
                                 &exported->descriptor) < 0)
        goto fail;
    exported->descriptor.buffer_id = buffer_id;
    if (advc_dmabuf_descriptor_validate(&exported->descriptor) < 0 ||
        exported->descriptor.width != public_metadata->width ||
        exported->descriptor.height != public_metadata->height)
        goto fail;
    if (acquire_fence_fd >= 0) {
        duplicate = fcntl(acquire_fence_fd, F_DUPFD_CLOEXEC, 0);
        if (duplicate < 0) goto fail;
    }
    exported->acquire_fence_fd = duplicate;
    return 0;

fail:
    advc_ahb_prime_export_close(exported);
    return -1;
}

int advc_ahb_prime_mapper_release(struct advc_ahb_prime_mapper *mapper,
                                  void *lifetime_token,
                                  int release_fence_fd) {
    if (mapper == NULL || lifetime_token == NULL || release_fence_fd < -1 ||
        (release_fence_fd >= 0 &&
         advc_dmabuf_sync_file_validate(release_fence_fd) < 0)) {
        if (release_fence_fd >= 0) close(release_fence_fd);
        errno = EINVAL;
        return -1;
    }
    return mapper->ops.release(mapper->userdata, lifetime_token,
                               release_fence_fd);
}
