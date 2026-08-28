#include "advc_vaapi_decode_vp9.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t profile0_key_frame[] = {
    0x82, 0x49, 0x83, 0x42, 0x00, 0x13, 0xf0, 0x0e,
};

static VADecPictureParameterBufferVP9 valid_picture(void) {
    VADecPictureParameterBufferVP9 picture;
    memset(&picture, 0, sizeof(picture));
    picture.frame_width = 320;
    picture.frame_height = 240;
    picture.pic_fields.bits.subsampling_x = 1;
    picture.pic_fields.bits.subsampling_y = 1;
    picture.pic_fields.bits.frame_type = 0;
    picture.pic_fields.bits.show_frame = 1;
    picture.profile = 0;
    picture.bit_depth = 8;
    picture.frame_header_length_in_bytes = 8;
    return picture;
}

static VASliceParameterBufferVP9 valid_slice(void) {
    VASliceParameterBufferVP9 slice;
    memset(&slice, 0, sizeof(slice));
    slice.slice_data_size = sizeof(profile0_key_frame);
    slice.slice_data_offset = 0;
    slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    return slice;
}

static void test_complete_frame_passthrough(void) {
    VADecPictureParameterBufferVP9 picture = valid_picture();
    VASliceParameterBufferVP9 slice = valid_slice();
    uint8_t *access_unit = NULL;
    size_t access_unit_size = 0;
    int key_frame = 0;

    assert(advc_vp9_build_access_unit(
               &picture, &slice, profile0_key_frame,
               sizeof(profile0_key_frame), 320, 240, &access_unit,
               &access_unit_size, &key_frame) == 0);
    assert(access_unit_size == sizeof(profile0_key_frame));
    assert(memcmp(access_unit, profile0_key_frame,
                  sizeof(profile0_key_frame)) == 0);
    assert(key_frame == 1);
    free(access_unit);
}

static void test_complete_inter_frame_passthrough(void) {
    static const uint8_t inter_frame[] = {0x86};
    VADecPictureParameterBufferVP9 picture = valid_picture();
    VASliceParameterBufferVP9 slice = valid_slice();
    uint8_t *access_unit = NULL;
    size_t access_unit_size = 0;
    int key_frame = 1;

    picture.pic_fields.bits.frame_type = 1;
    picture.frame_header_length_in_bytes = sizeof(inter_frame);
    slice.slice_data_size = sizeof(inter_frame);
    assert(advc_vp9_build_access_unit(
               &picture, &slice, inter_frame, sizeof(inter_frame), 320, 240,
               &access_unit, &access_unit_size, &key_frame) == 0);
    assert(access_unit_size == sizeof(inter_frame));
    assert(memcmp(access_unit, inter_frame, sizeof(inter_frame)) == 0);
    assert(key_frame == 0);
    free(access_unit);
}

static void test_frame_prefix_subset(void) {
    static const uint8_t inter_frame[] = {0x86};
    static const uint8_t show_existing[] = {0x88};
    static const uint8_t profile2[] = {0x92};
    struct advc_vp9_frame_info info;

    assert(advc_vp9_parse_frame_prefix(inter_frame, sizeof(inter_frame),
                                       &info) == 0);
    assert(info.profile == 0 && info.key_frame == 0 && info.show_frame == 1 &&
           info.error_resilient_mode == 0);
    errno = 0;
    assert(advc_vp9_parse_frame_prefix(show_existing,
                                       sizeof(show_existing), &info) < 0);
    assert(errno == ENOTSUP);
    assert(advc_vp9_parse_frame_prefix(profile2, sizeof(profile2), &info) < 0);
    assert(errno == ENOTSUP);
}

static void test_fail_closed_framing(void) {
    VADecPictureParameterBufferVP9 picture = valid_picture();
    VASliceParameterBufferVP9 slice = valid_slice();
    uint8_t *access_unit = NULL;
    size_t access_unit_size = 0;
    int key_frame = 0;

    slice.slice_data_offset = picture.frame_header_length_in_bytes;
    errno = 0;
    assert(advc_vp9_build_access_unit(
               &picture, &slice, profile0_key_frame,
               sizeof(profile0_key_frame), 320, 240, &access_unit,
               &access_unit_size, &key_frame) < 0);
    assert(errno == ENOTSUP);
    slice = valid_slice();
    slice.slice_data_flag = VA_SLICE_DATA_FLAG_BEGIN;
    assert(advc_vp9_build_access_unit(
               &picture, &slice, profile0_key_frame,
               sizeof(profile0_key_frame), 320, 240, &access_unit,
               &access_unit_size, &key_frame) < 0);
    slice = valid_slice();
    --slice.slice_data_size;
    assert(advc_vp9_build_access_unit(
               &picture, &slice, profile0_key_frame,
               sizeof(profile0_key_frame), 320, 240, &access_unit,
               &access_unit_size, &key_frame) < 0);
}

static void test_fail_closed_profile_and_header(void) {
    VADecPictureParameterBufferVP9 picture = valid_picture();
    VASliceParameterBufferVP9 slice = valid_slice();
    uint8_t corrupted[sizeof(profile0_key_frame)];
    uint8_t *access_unit = NULL;
    size_t access_unit_size = 0;
    int key_frame = 0;

    picture.profile = 2;
    errno = 0;
    assert(advc_vp9_build_access_unit(
               &picture, &slice, profile0_key_frame,
               sizeof(profile0_key_frame), 320, 240, &access_unit,
               &access_unit_size, &key_frame) < 0);
    assert(errno == ENOTSUP);
    picture = valid_picture();
    picture.bit_depth = 10;
    assert(advc_vp9_build_access_unit(
               &picture, &slice, profile0_key_frame,
               sizeof(profile0_key_frame), 320, 240, &access_unit,
               &access_unit_size, &key_frame) < 0);
    picture = valid_picture();
    memcpy(corrupted, profile0_key_frame, sizeof(corrupted));
    corrupted[2] ^= 1;
    errno = 0;
    assert(advc_vp9_build_access_unit(
               &picture, &slice, corrupted, sizeof(corrupted), 320, 240,
               &access_unit, &access_unit_size, &key_frame) < 0);
    assert(errno == EPROTO);
}

static void test_parameter_mismatch(void) {
    VADecPictureParameterBufferVP9 picture = valid_picture();
    VASliceParameterBufferVP9 slice = valid_slice();
    uint8_t *access_unit = NULL;
    size_t access_unit_size = 0;
    int key_frame = 0;

    picture.pic_fields.bits.frame_type = 1;
    errno = 0;
    assert(advc_vp9_build_access_unit(
               &picture, &slice, profile0_key_frame,
               sizeof(profile0_key_frame), 320, 240, &access_unit,
               &access_unit_size, &key_frame) < 0);
    assert(errno == EPROTO);
    picture = valid_picture();
    picture.frame_width = 321;
    assert(advc_vp9_build_access_unit(
               &picture, &slice, profile0_key_frame,
               sizeof(profile0_key_frame), 320, 240, &access_unit,
               &access_unit_size, &key_frame) < 0);
}

int main(void) {
    test_complete_frame_passthrough();
    test_complete_inter_frame_passthrough();
    test_frame_prefix_subset();
    test_fail_closed_framing();
    test_fail_closed_profile_and_header();
    test_parameter_mismatch();
    puts("ADVC VA-API VP9 Profile 0 translator: PASS");
    return 0;
}
