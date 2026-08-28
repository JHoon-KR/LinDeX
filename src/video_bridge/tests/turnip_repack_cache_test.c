#define _GNU_SOURCE
#include "turnip_repack_cache.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static struct advc_dmabuf_descriptor descriptor(void) {
    struct advc_dmabuf_descriptor value;
    memset(&value, 0, sizeof(value));
    value.buffer_id = 99;
    value.width = 1280;
    value.height = 720;
    value.drm_fourcc = UINT32_C(0x3231564e);
    value.explicit_flags = ADVC_DMABUF_EXPLICIT_ALL;
    value.drm_modifier = UINT64_C(0x0500000000000001);
    value.crop_width = value.width;
    value.crop_height = value.height;
    value.object_count = 1;
    value.plane_count = 2;
    value.objects[0].fd = -1;
    value.objects[0].size = 1382400;
    value.planes[0].pitch = 1280;
    value.planes[1].offset = 921600;
    value.planes[1].pitch = 1280;
    return value;
}

int main(void) {
    struct advc_dmabuf_descriptor value = descriptor();
    struct advc_repack_descriptor_signature signature;
    struct advc_repack_descriptor_signature changed;
    struct advc_repack_fd_identity identity;
    struct advc_repack_fd_identity duplicate_identity;
    struct advc_repack_source_key sources[2];
    struct advc_repack_lease_key leases[2];
    uint64_t next_token = 0;
    uint64_t first;
    uint64_t second;
    uint64_t replacement;
    size_t index;
    int exact;
    int pipe_fds[2];
    int duplicate;

    memset(sources, 0, sizeof(sources));
    memset(leases, 0, sizeof(leases));
    advc_repack_descriptor_signature_make(&value, &signature);
    value.buffer_id = 1234;
    advc_repack_descriptor_signature_make(&value, &changed);
    assert(advc_repack_descriptor_signature_equal(&signature, &changed));
    value.planes[1].pitch += 64;
    advc_repack_descriptor_signature_make(&value, &changed);
    assert(!advc_repack_descriptor_signature_equal(&signature, &changed));

    assert(pipe2(pipe_fds, O_CLOEXEC) == 0);
    duplicate = fcntl(pipe_fds[0], F_DUPFD_CLOEXEC, 3);
    assert(duplicate >= 0);
    assert(advc_repack_fd_identity_from_fd(pipe_fds[0], &identity) == 0);
    assert(advc_repack_fd_identity_from_fd(duplicate, &duplicate_identity) == 0);
    assert(memcmp(&identity, &duplicate_identity, sizeof(identity)) == 0);

    sources[0].occupied = 1;
    sources[0].identity = identity;
    sources[0].signature = signature;
    sources[0].last_use = 5;
    index = advc_repack_source_key_select(sources, 2, &identity, &signature,
                                          &exact);
    assert(index == 0 && exact == 1);
    index = advc_repack_source_key_select(sources, 2, &identity, &changed,
                                          &exact);
    assert(index == 0 && exact == 0); /* Same fd object invalidates first. */
    duplicate_identity.inode += 1;
    index = advc_repack_source_key_select(sources, 2, &duplicate_identity,
                                          &signature, &exact);
    assert(index == 1 && exact == 0); /* Empty precedes LRU eviction. */
    sources[1] = sources[0];
    sources[1].identity = duplicate_identity;
    sources[1].last_use = 9;
    duplicate_identity.inode += 1;
    index = advc_repack_source_key_select(sources, 2, &duplicate_identity,
                                          &signature, &exact);
    assert(index == 0 && exact == 0); /* Bounded cache chooses LRU. */

    assert(advc_repack_lease_acquire(leases, 2, &next_token, &index, &first) == 0);
    assert(index == 0 && first != 0);
    assert(advc_repack_lease_acquire(leases, 2, &next_token, &index, &second) == 0);
    assert(index == 1 && second != 0 && second != first);
    assert(advc_repack_lease_acquire(leases, 2, &next_token, &index,
                                     &replacement) < 0 && errno == ENOSPC);
    assert(advc_repack_lease_find(leases, 2, first, &index) == 0 && index == 0);
    advc_repack_lease_clear(&leases[0]);
    assert(advc_repack_lease_find(leases, 2, first, &index) < 0 &&
           errno == ENOENT);
    assert(advc_repack_lease_acquire(leases, 2, &next_token, &index,
                                     &replacement) == 0);
    assert(index == 0 && replacement != first && replacement != second);

    close(duplicate);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return 0;
}
