#define _GNU_SOURCE
#include "advc/dmabuf_ingress.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

#define DRM_FORMAT_ABGR8888 UINT32_C(0x34324241) /* AB24 */

static int make_object(size_t size) {
    int fd = memfd_create("advc-dmabuf-contract", MFD_CLOEXEC);
    assert(fd >= 0);
    assert(ftruncate(fd, (off_t)size) == 0);
    return fd;
}

static int make_fence(void) {
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    assert(fd >= 0);
    return fd;
}

static void make_registration(uint8_t payload[ADVC_REGISTER_DMABUF_SIZE],
                              uint64_t id, uint64_t modifier, uint64_t object_size) {
    memset(payload, 0, ADVC_REGISTER_DMABUF_SIZE);
    advc_put_u64(payload + ADVC_REGISTER_DMABUF_BUFFER_ID_OFFSET, id);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_WIDTH_OFFSET, 640);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_HEIGHT_OFFSET, 480);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_FOURCC_OFFSET, DRM_FORMAT_ABGR8888);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_FLAGS_OFFSET,
                 ADVC_DMABUF_EXPLICIT_ALL);
    advc_put_u64(payload + ADVC_REGISTER_DMABUF_MODIFIER_OFFSET, modifier);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_CROP_WIDTH_OFFSET, 640);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_CROP_HEIGHT_OFFSET, 480);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_OBJECT_COUNT_OFFSET, 1);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_PLANE_COUNT_OFFSET, 1);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_COLOR_PRIMARIES_OFFSET,
                 ADVC_COLOR_PRIMARIES_BT709);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_COLOR_TRANSFER_OFFSET,
                 ADVC_COLOR_TRANSFER_SRGB);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_COLOR_MATRIX_OFFSET,
                 ADVC_COLOR_MATRIX_RGB);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_COLOR_RANGE_OFFSET,
                 ADVC_COLOR_RANGE_FULL);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_OBJECTS_OFFSET +
                 ADVC_REGISTER_DMABUF_OBJECT_FD_INDEX_OFFSET, 0);
    advc_put_u64(payload + ADVC_REGISTER_DMABUF_OBJECTS_OFFSET +
                 ADVC_REGISTER_DMABUF_OBJECT_SIZE_OFFSET, object_size);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_PLANES_OFFSET +
                 ADVC_REGISTER_DMABUF_PLANE_OBJECT_INDEX_OFFSET, 0);
    advc_put_u64(payload + ADVC_REGISTER_DMABUF_PLANES_OFFSET +
                 ADVC_REGISTER_DMABUF_PLANE_OFFSET_OFFSET, 0);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_PLANES_OFFSET +
                 ADVC_REGISTER_DMABUF_PLANE_PITCH_OFFSET, 640 * 4);
}

static int allow_linear_ab24(void *userdata,
                             const struct advc_dmabuf_descriptor *descriptor) {
    int *calls = userdata;
    ++*calls;
    return descriptor->drm_fourcc == DRM_FORMAT_ABGR8888 &&
           descriptor->drm_modifier == 0;
}

static void test_registration_decode_and_negative_contracts(void) {
    uint8_t payload[ADVC_REGISTER_DMABUF_SIZE];
    uint8_t bad[ADVC_REGISTER_DMABUF_SIZE];
    struct advc_dmabuf_descriptor descriptor;
    int fd = make_object(640u * 480u * 4u);
    int fds[1] = {fd};
    int no_cloexec;

    make_registration(payload, 7, 0, 640u * 480u * 4u);
    assert(advc_dmabuf_registration_decode(payload, sizeof(payload), fds, 1,
                                             &descriptor) == 0);
    assert(descriptor.buffer_id == 7 && descriptor.object_count == 1);
    assert(descriptor.objects[0].fd == fd && descriptor.planes[0].pitch == 2560);
    assert(advc_dmabuf_registration_decode(payload, sizeof(payload) - 1, fds, 1,
                                             &descriptor) == -1);
    assert(advc_dmabuf_registration_decode(payload, sizeof(payload), NULL, 0,
                                             &descriptor) == -1);

    memcpy(bad, payload, sizeof(bad));
    advc_put_u32(bad + ADVC_REGISTER_DMABUF_FLAGS_OFFSET,
                 ADVC_DMABUF_EXPLICIT_FOURCC | ADVC_DMABUF_EXPLICIT_PLANES);
    assert(advc_dmabuf_registration_decode(bad, sizeof(bad), fds, 1,
                                             &descriptor) == -1);
    memcpy(bad, payload, sizeof(bad));
    advc_put_u64(bad + ADVC_REGISTER_DMABUF_MODIFIER_OFFSET, UINT64_MAX);
    assert(advc_dmabuf_registration_decode(bad, sizeof(bad), fds, 1,
                                             &descriptor) == -1);
    memcpy(bad, payload, sizeof(bad));
    bad[80] = 1;
    assert(advc_dmabuf_registration_decode(bad, sizeof(bad), fds, 1,
                                             &descriptor) == -1);
    memcpy(bad, payload, sizeof(bad));
    advc_put_u32(bad + ADVC_REGISTER_DMABUF_CROP_LEFT_OFFSET, 639);
    advc_put_u32(bad + ADVC_REGISTER_DMABUF_CROP_WIDTH_OFFSET, 2);
    assert(advc_dmabuf_registration_decode(bad, sizeof(bad), fds, 1,
                                             &descriptor) == -1);
    memcpy(bad, payload, sizeof(bad));
    advc_put_u64(bad + ADVC_REGISTER_DMABUF_OBJECTS_OFFSET +
                 ADVC_REGISTER_DMABUF_OBJECT_SIZE_OFFSET, 1024);
    assert(advc_dmabuf_registration_decode(bad, sizeof(bad), fds, 1,
                                             &descriptor) == -1);
    memcpy(bad, payload, sizeof(bad));
    advc_put_u32(bad + ADVC_REGISTER_DMABUF_PLANES_OFFSET +
                 ADVC_REGISTER_DMABUF_PLANE_OBJECT_INDEX_OFFSET, 1);
    assert(advc_dmabuf_registration_decode(bad, sizeof(bad), fds, 1,
                                             &descriptor) == -1);
    memcpy(bad, payload, sizeof(bad));
    advc_put_u32(bad + ADVC_REGISTER_DMABUF_COLOR_MATRIX_OFFSET,
                 ADVC_COLOR_MATRIX_RGB);
    advc_put_u32(bad + ADVC_REGISTER_DMABUF_CHROMA_HORIZONTAL_OFFSET,
                 ADVC_CHROMA_SITING_MIDPOINT);
    assert(advc_dmabuf_registration_decode(bad, sizeof(bad), fds, 1,
                                             &descriptor) == -1);
    memcpy(bad, payload, sizeof(bad));
    bad[ADVC_REGISTER_DMABUF_OBJECTS_OFFSET + 4] = 1;
    assert(advc_dmabuf_registration_decode(bad, sizeof(bad), fds, 1,
                                             &descriptor) == -1);

    no_cloexec = dup(fd);
    assert(no_cloexec >= 0 && (fcntl(no_cloexec, F_GETFD) & FD_CLOEXEC) == 0);
    fds[0] = no_cloexec;
    assert(advc_dmabuf_registration_decode(payload, sizeof(payload), fds, 1,
                                             &descriptor) == -1);
    close(no_cloexec);
    close(fd);
}

static void test_submission_and_completion_contracts(void) {
    uint8_t queue[ADVC_QUEUE_DMABUF_SIZE] = {0};
    uint8_t complete[ADVC_COMPLETE_DMABUF_SIZE] = {0};
    uint8_t unregister_payload[ADVC_UNREGISTER_DMABUF_SIZE] = {0};
    struct advc_dmabuf_submission submission;
    uint64_t buffer_id = 0;
    int fence = make_fence();
    int fds[1] = {fence};

    advc_put_u64(queue + ADVC_QUEUE_DMABUF_BUFFER_ID_OFFSET, 8);
    advc_put_u64(queue + ADVC_QUEUE_DMABUF_PTS_NS_OFFSET, 123456789);
    advc_put_u32(queue + ADVC_QUEUE_DMABUF_FENCE_ROLE_OFFSET, ADVC_FD_NONE);
    assert(advc_dmabuf_submission_decode(queue, sizeof(queue), NULL, 0,
                                          &submission) == 0);
    assert(submission.acquire_fence_fd == -1);
    advc_put_u32(queue + ADVC_QUEUE_DMABUF_FENCE_ROLE_OFFSET,
                 ADVC_FD_ACQUIRE_FENCE);
    assert(advc_dmabuf_submission_decode(queue, sizeof(queue), fds, 1,
                                          &submission) == -1); /* eventfd != sync_file */
    advc_put_u32(queue + ADVC_QUEUE_DMABUF_FENCE_ROLE_OFFSET, ADVC_FD_NONE);
    queue[24] = 1;
    assert(advc_dmabuf_submission_decode(queue, sizeof(queue), fds, 1,
                                          &submission) == -1);
    queue[24] = 0;

    advc_put_u64(unregister_payload + ADVC_UNREGISTER_DMABUF_BUFFER_ID_OFFSET, 8);
    assert(advc_dmabuf_unregister_decode(unregister_payload,
                                          sizeof(unregister_payload), 0,
                                          &buffer_id) == 0 && buffer_id == 8);
    assert(advc_dmabuf_unregister_decode(unregister_payload,
                                          sizeof(unregister_payload), 1,
                                          &buffer_id) == -1);
    memset(unregister_payload, 0, sizeof(unregister_payload));
    assert(advc_dmabuf_unregister_decode(unregister_payload,
                                          sizeof(unregister_payload), 0,
                                          &buffer_id) == -1);

    advc_put_u64(complete + ADVC_COMPLETE_DMABUF_BUFFER_ID_OFFSET, 8);
    advc_put_u32(complete + ADVC_COMPLETE_DMABUF_STATUS_OFFSET, ADVC_STATUS_OK);
    advc_put_u32(complete + ADVC_COMPLETE_DMABUF_FENCE_ROLE_OFFSET,
                 ADVC_FD_NONE);
    assert(advc_dmabuf_completion_validate(complete, sizeof(complete), NULL, 0) == 0);
    advc_put_u32(complete + ADVC_COMPLETE_DMABUF_FENCE_ROLE_OFFSET,
                 ADVC_FD_RELEASE_FENCE);
    assert(advc_dmabuf_completion_validate(complete, sizeof(complete), fds, 1) == -1);
    advc_put_u32(complete + ADVC_COMPLETE_DMABUF_STATUS_OFFSET,
                 ADVC_STATUS_CODEC_ERROR);
    assert(advc_dmabuf_completion_validate(complete, sizeof(complete), NULL, 0) == -1);
    advc_put_u32(complete + ADVC_COMPLETE_DMABUF_STATUS_OFFSET, ADVC_STATUS_OK);
    advc_put_u32(complete + ADVC_COMPLETE_DMABUF_FENCE_ROLE_OFFSET,
                 ADVC_FD_NONE);
    complete[20] = 1;
    assert(advc_dmabuf_completion_validate(complete, sizeof(complete), NULL, 0) == -1);
    close(fence);
}

static void test_registry_bounds_and_ownership(void) {
    uint8_t payload[ADVC_REGISTER_DMABUF_SIZE];
    struct advc_dmabuf_descriptor descriptors[ADVC_MAX_REGISTERED_DMABUFS + 1];
    struct advc_dmabuf_registry *registry;
    struct advc_dmabuf_submission submission = {0};
    struct advc_dmabuf_job jobs[ADVC_MAX_INFLIGHT_DMABUFS];
    struct advc_dmabuf_job rejected_job;
    int fd = make_object(640u * 480u * 4u);
    int fds[1] = {fd};
    int calls = 0;

    assert(advc_dmabuf_registry_create(NULL, NULL) == NULL && errno == EINVAL);
    registry = advc_dmabuf_registry_create(allow_linear_ab24, &calls);
    assert(registry != NULL);
    for (uint64_t i = 0; i < ADVC_MAX_REGISTERED_DMABUFS + 1u; ++i) {
        make_registration(payload, 100 + i, 0, 640u * 480u * 4u);
        assert(advc_dmabuf_registration_decode(payload, sizeof(payload), fds, 1,
                                                 &descriptors[i]) == 0);
    }
    assert(advc_dmabuf_registry_register(registry, &descriptors[0]) == 0);
    assert(advc_dmabuf_registry_register(registry, &descriptors[0]) == -1 &&
           errno == EEXIST);
    for (size_t i = 1; i < ADVC_MAX_REGISTERED_DMABUFS; ++i)
        assert(advc_dmabuf_registry_register(registry, &descriptors[i]) == 0);
    assert(advc_dmabuf_registry_registered_count(registry) ==
           ADVC_MAX_REGISTERED_DMABUFS);
    assert(advc_dmabuf_registry_register(
               registry, &descriptors[ADVC_MAX_REGISTERED_DMABUFS]) == -1 &&
           errno == ENOSPC);

    for (size_t i = 0; i < ADVC_MAX_INFLIGHT_DMABUFS; ++i) {
        submission.buffer_id = 100 + i;
        submission.pts_ns = i * 1000;
        submission.acquire_fence_fd = -1;
        assert(advc_dmabuf_registry_begin(registry, &submission, &jobs[i]) == 0);
        assert(jobs[i].acquire_fence_fd == -1);
        assert(jobs[i].descriptor != NULL);
        assert(jobs[i].descriptor->buffer_id == 100 + i);
        assert(jobs[i].descriptor->drm_fourcc == DRM_FORMAT_ABGR8888);
    }
    assert(advc_dmabuf_registry_inflight_count(registry) ==
           ADVC_MAX_INFLIGHT_DMABUFS);
    submission.buffer_id = 104;
    assert(advc_dmabuf_registry_begin(registry, &submission, &rejected_job) == -1 &&
           errno == EAGAIN);
    assert(advc_dmabuf_registry_unregister(registry, 100) == -1 && errno == EBUSY);
    submission.buffer_id = 100;
    assert(advc_dmabuf_registry_begin(registry, &submission, &rejected_job) == -1 &&
           errno == EBUSY);

    for (size_t i = 0; i < ADVC_MAX_INFLIGHT_DMABUFS; ++i) {
        assert(advc_dmabuf_registry_finish(registry, 100 + i) == 0);
        advc_dmabuf_job_close(&jobs[i]);
    }
    assert(advc_dmabuf_registry_finish(registry, 100) == -1 && errno == EALREADY);
    assert(advc_dmabuf_registry_unregister(registry, 100) == 0);
    assert(advc_dmabuf_registry_registered_count(registry) ==
           ADVC_MAX_REGISTERED_DMABUFS - 1u);

    descriptors[ADVC_MAX_REGISTERED_DMABUFS].drm_modifier = UINT64_C(0x0500000000000001);
    assert(advc_dmabuf_registry_register(
               registry, &descriptors[ADVC_MAX_REGISTERED_DMABUFS]) == -1 &&
           errno == ENOTSUP);
    assert(calls == ADVC_MAX_REGISTERED_DMABUFS + 1);
    advc_dmabuf_registry_destroy(registry);
    close(fd);
}

static void test_registration_encode_roundtrip(void) {
    uint8_t payload[ADVC_REGISTER_DMABUF_SIZE];
    struct advc_dmabuf_descriptor source;
    struct advc_dmabuf_descriptor decoded;
    int encoded_fds[ADVC_MAX_DMABUF_OBJECTS];
    uint16_t encoded_fd_count = 0;
    int fd = make_object(128u * 64u * 4u);

    memset(&source, 0, sizeof(source));
    for (size_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        source.objects[i].fd = -1;
    source.buffer_id = 501;
    source.width = 128;
    source.height = 64;
    source.drm_fourcc = DRM_FORMAT_ABGR8888;
    source.explicit_flags = ADVC_DMABUF_EXPLICIT_ALL;
    source.crop_width = 128;
    source.crop_height = 64;
    source.object_count = 1;
    source.plane_count = 1;
    source.color_matrix = ADVC_COLOR_MATRIX_RGB;
    source.color_range = ADVC_COLOR_RANGE_FULL;
    source.objects[0].fd = fd;
    source.objects[0].size = 128u * 64u * 4u;
    source.planes[0].pitch = 128u * 4u;
    assert(advc_dmabuf_registration_encode(payload, &source, encoded_fds,
                                             &encoded_fd_count) == 0);
    assert(encoded_fd_count == 1 && encoded_fds[0] == fd);
    assert(advc_dmabuf_registration_decode(payload, sizeof(payload),
                                             encoded_fds, encoded_fd_count,
                                             &decoded) == 0);
    assert(decoded.buffer_id == source.buffer_id &&
           decoded.drm_fourcc == source.drm_fourcc &&
           decoded.objects[0].fd == fd &&
           decoded.planes[0].pitch == source.planes[0].pitch);
    close(fd);
}

static void test_close_ignores_undeclared_object_slots(void) {
    struct advc_dmabuf_descriptor descriptor;
    int saved_stdin;
    int sentinel;

    saved_stdin = dup(STDIN_FILENO);
    sentinel = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(sentinel >= 0);
    assert(dup2(sentinel, STDIN_FILENO) == STDIN_FILENO);
    if (sentinel != STDIN_FILENO) close(sentinel);

    memset(&descriptor, 0, sizeof(descriptor));
    advc_dmabuf_descriptor_close(&descriptor);
    assert(fcntl(STDIN_FILENO, F_GETFD) >= 0);

    if (saved_stdin >= 0) {
        assert(dup2(saved_stdin, STDIN_FILENO) == STDIN_FILENO);
        close(saved_stdin);
    } else {
        close(STDIN_FILENO);
    }
}

static void test_close_declared_ownership_and_duplicate_rejection(void) {
    struct advc_dmabuf_descriptor descriptor;
    int owned_fd;
    int undeclared_fd;

    memset(&descriptor, 0, sizeof(descriptor));
    for (size_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        descriptor.objects[i].fd = -1;
    owned_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    undeclared_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(owned_fd >= 0 && undeclared_fd >= 0);
    descriptor.object_count = 1;
    descriptor.objects[0].fd = owned_fd;
    descriptor.objects[1].fd = undeclared_fd;
    advc_dmabuf_descriptor_close(&descriptor);
    assert(fcntl(owned_fd, F_GETFD) < 0 && errno == EBADF);
    assert(fcntl(undeclared_fd, F_GETFD) >= 0);
    close(undeclared_fd);
    advc_dmabuf_descriptor_close(&descriptor);

    memset(&descriptor, 0, sizeof(descriptor));
    for (size_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        descriptor.objects[i].fd = -1;
    descriptor.buffer_id = 701;
    descriptor.width = 128;
    descriptor.height = 64;
    descriptor.drm_fourcc = DRM_FORMAT_ABGR8888;
    descriptor.explicit_flags = ADVC_DMABUF_EXPLICIT_ALL;
    descriptor.crop_width = 128;
    descriptor.crop_height = 64;
    descriptor.object_count = 2;
    descriptor.plane_count = 1;
    descriptor.color_matrix = ADVC_COLOR_MATRIX_RGB;
    descriptor.color_range = ADVC_COLOR_RANGE_FULL;
    descriptor.objects[0].fd = make_object(128u * 64u * 4u);
    descriptor.objects[0].size = 128u * 64u * 4u;
    descriptor.objects[1] = descriptor.objects[0];
    descriptor.planes[0].pitch = 128u * 4u;
    assert(advc_dmabuf_descriptor_validate(&descriptor) < 0 && errno == EINVAL);
    advc_dmabuf_descriptor_close(&descriptor);
}

int main(void) {
    test_registration_decode_and_negative_contracts();
    test_submission_and_completion_contracts();
    test_registry_bounds_and_ownership();
    test_registration_encode_roundtrip();
    test_close_ignores_undeclared_object_slots();
    test_close_declared_ownership_and_duplicate_rejection();
    puts("dmabuf_ingress_test: PASS");
    return 0;
}
