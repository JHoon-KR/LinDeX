#include "advc_vaapi_slice_layout.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>

static void test_ffmpeg_pair_rebase(void) {
    uint32_t first_offset[] = {0};
    const uint32_t first_size[] = {5};
    uint32_t second_offset[] = {0};
    const uint32_t second_size[] = {7};

    assert(advc_vaapi_rebase_slice_offsets(first_offset, first_size, 1, 0, 5,
                                           1024) == 0);
    assert(first_offset[0] == 0);
    assert(advc_vaapi_rebase_slice_offsets(second_offset, second_size, 1, 5,
                                           7, 1024) == 0);
    assert(second_offset[0] == 5);
}

static void test_many_parameters_one_data_buffer(void) {
    uint32_t offsets[] = {1, 6};
    const uint32_t sizes[] = {4, 3};

    assert(advc_vaapi_rebase_slice_offsets(offsets, sizes, 2, 20, 9, 1024) ==
           0);
    assert(offsets[0] == 21);
    assert(offsets[1] == 26);
}

static void test_failure_is_transactional(void) {
    uint32_t offsets[] = {0, 8};
    const uint32_t sizes[] = {4, 3};

    errno = 0;
    assert(advc_vaapi_rebase_slice_offsets(offsets, sizes, 2, 10, 9, 1024) ==
           -1);
    assert(errno == EINVAL);
    assert(offsets[0] == 0);
    assert(offsets[1] == 8);
}

static void test_aggregate_limit(void) {
    uint32_t offset[] = {0};
    const uint32_t size[] = {2};

    errno = 0;
    assert(advc_vaapi_rebase_slice_offsets(offset, size, 1, 9, 2, 10) == -1);
    assert(errno == EINVAL);
    assert(offset[0] == 0);
}

int main(void) {
    test_ffmpeg_pair_rebase();
    test_many_parameters_one_data_buffer();
    test_failure_is_transactional();
    test_aggregate_limit();
    return 0;
}
