#include "advc_vaapi_decode_av1.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t raw_key_frame[] = {
    0x0a, 0x01, 0x00,             /* sequence header: Main, non-still */
    0x32, 0x03, 0x10, 0xaa, 0xbb, /* frame OBU: key, show_frame */
};

static const uint8_t annex_b_key_frame[] = {
    0x09,                         /* temporal_unit_size */
    0x08,                         /* frame_unit_size */
    0x02, 0x08, 0x00,             /* obu_length, sequence header */
    0x04, 0x30, 0x10, 0xaa, 0xbb, /* obu_length, frame OBU */
};

static VADecPictureParameterBufferAV1 valid_picture(void) {
    VADecPictureParameterBufferAV1 picture;
    memset(&picture, 0, sizeof(picture));
    picture.profile = 0;
    picture.bit_depth_idx = 0;
    picture.seq_info_fields.fields.subsampling_x = 1;
    picture.seq_info_fields.fields.subsampling_y = 1;
    picture.frame_width_minus1 = 63;
    picture.frame_height_minus1 = 63;
    picture.tile_cols = 1;
    picture.tile_rows = 1;
    picture.pic_info_fields.bits.frame_type = 0;
    picture.pic_info_fields.bits.show_frame = 1;
    return picture;
}

static VASliceParameterBufferAV1 raw_tile(void) {
    VASliceParameterBufferAV1 tile;
    memset(&tile, 0, sizeof(tile));
    tile.slice_data_size = 2;
    tile.slice_data_offset = 6;
    tile.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    return tile;
}

static VASliceParameterBufferAV1 annex_b_tile(void) {
    VASliceParameterBufferAV1 tile = raw_tile();
    tile.slice_data_offset = 8;
    return tile;
}

static void test_raw_obu_exact_copy(void) {
    VADecPictureParameterBufferAV1 picture = valid_picture();
    VASliceParameterBufferAV1 tile = raw_tile();
    struct advc_av1_access_unit_info info;
    uint8_t *output = NULL;
    size_t output_size = 0;
    assert(advc_av1_build_access_unit(
               &picture, &tile, 1, raw_key_frame, sizeof(raw_key_frame),
               64, 64, &output, &output_size, &info) == 0);
    assert(output_size == sizeof(raw_key_frame));
    assert(memcmp(output, raw_key_frame, sizeof(raw_key_frame)) == 0);
    assert(info.input_format == ADVC_AV1_INPUT_RAW_OBU);
    assert(info.key_frame == 1 && info.frame_type == 0);
    assert(info.contains_sequence_header == 1 && info.obu_count == 2);
    free(output);
}

static void test_annex_b_exact_unwrap(void) {
    VADecPictureParameterBufferAV1 picture = valid_picture();
    VASliceParameterBufferAV1 tile = annex_b_tile();
    struct advc_av1_access_unit_info info;
    uint8_t *output = NULL;
    size_t output_size = 0;
    assert(advc_av1_build_access_unit(
               &picture, &tile, 1, annex_b_key_frame,
               sizeof(annex_b_key_frame), 64, 64, &output, &output_size,
               &info) == 0);
    assert(output_size == sizeof(raw_key_frame));
    assert(memcmp(output, raw_key_frame, sizeof(raw_key_frame)) == 0);
    assert(info.input_format == ADVC_AV1_INPUT_ANNEX_B);
    assert(info.key_frame == 1 && info.contains_sequence_header == 1);
    free(output);
}

static void test_fail_closed_profile_and_format(void) {
    VADecPictureParameterBufferAV1 picture = valid_picture();
    VASliceParameterBufferAV1 tile = raw_tile();
    struct advc_av1_access_unit_info info;
    uint8_t stripped_tile_data[] = {0xaa, 0xbb};
    uint8_t *output = NULL;
    size_t output_size = 0;

    picture.profile = 1;
    errno = 0;
    assert(advc_av1_build_access_unit(
               &picture, &tile, 1, raw_key_frame, sizeof(raw_key_frame),
               64, 64, &output, &output_size, &info) < 0);
    assert(errno == ENOTSUP);
    picture = valid_picture();
    picture.bit_depth_idx = 1;
    assert(advc_av1_build_access_unit(
               &picture, &tile, 1, raw_key_frame, sizeof(raw_key_frame),
               64, 64, &output, &output_size, &info) < 0);
    picture = valid_picture();
    picture.seq_info_fields.fields.subsampling_y = 0;
    assert(advc_av1_build_access_unit(
               &picture, &tile, 1, raw_key_frame, sizeof(raw_key_frame),
               64, 64, &output, &output_size, &info) < 0);
    picture = valid_picture();
    tile.slice_data_offset = 0;
    errno = 0;
    assert(advc_av1_build_access_unit(
               &picture, &tile, 1, stripped_tile_data,
               sizeof(stripped_tile_data), 64, 64, &output, &output_size,
               &info) < 0);
    assert(errno == ENOTSUP || errno == EPROTO);
}

static void test_fail_closed_fragments_and_ranges(void) {
    VADecPictureParameterBufferAV1 picture = valid_picture();
    VASliceParameterBufferAV1 tile = raw_tile();
    struct advc_av1_access_unit_info info;
    uint8_t *output = NULL;
    size_t output_size = 0;

    tile.slice_data_flag = VA_SLICE_DATA_FLAG_BEGIN;
    errno = 0;
    assert(advc_av1_build_access_unit(
               &picture, &tile, 1, raw_key_frame, sizeof(raw_key_frame),
               64, 64, &output, &output_size, &info) < 0);
    assert(errno == ENOTSUP);
    tile = raw_tile();
    tile.slice_data_offset = 5;
    tile.slice_data_size = 4;
    assert(advc_av1_build_access_unit(
               &picture, &tile, 1, raw_key_frame, sizeof(raw_key_frame),
               64, 64, &output, &output_size, &info) < 0);
    tile = raw_tile();
    assert(advc_av1_build_access_unit(
               &picture, &tile, 0, raw_key_frame, sizeof(raw_key_frame),
               64, 64, &output, &output_size, &info) < 0);
}

static void test_fail_closed_incomplete_and_mismatch(void) {
    VADecPictureParameterBufferAV1 picture = valid_picture();
    VASliceParameterBufferAV1 tile = raw_tile();
    struct advc_av1_access_unit_info info;
    uint8_t frame_header_only[] = {0x1a, 0x01, 0x10};
    uint8_t corrupt_size[sizeof(raw_key_frame)];
    uint8_t *output = NULL;
    size_t output_size = 0;

    errno = 0;
    assert(advc_av1_build_access_unit(
               &picture, &tile, 1, frame_header_only,
               sizeof(frame_header_only), 64, 64, &output, &output_size,
               &info) < 0);
    assert(errno == ENOTSUP || errno == EPROTO);
    memcpy(corrupt_size, raw_key_frame, sizeof(corrupt_size));
    corrupt_size[4] = 0x7f;
    assert(advc_av1_build_access_unit(
               &picture, &tile, 1, corrupt_size, sizeof(corrupt_size),
               64, 64, &output, &output_size, &info) < 0);
    assert(advc_av1_build_access_unit(
               &picture, &tile, 1, raw_key_frame, sizeof(raw_key_frame),
               65, 64, &output, &output_size, &info) < 0);
    picture = valid_picture();
    picture.pic_info_fields.bits.frame_type = 1;
    errno = 0;
    assert(advc_av1_build_access_unit(
               &picture, &tile, 1, raw_key_frame, sizeof(raw_key_frame),
               64, 64, &output, &output_size, &info) < 0);
    assert(errno == EPROTO || errno == ENOTSUP);
}

static void test_bounded_mutations(void) {
    VADecPictureParameterBufferAV1 picture = valid_picture();
    VASliceParameterBufferAV1 raw = raw_tile();
    VASliceParameterBufferAV1 annex = annex_b_tile();
    uint8_t mutated[sizeof(annex_b_key_frame)];
    size_t limit;
    size_t byte;
    unsigned int bit;

    for (limit = 1; limit <= sizeof(raw_key_frame); ++limit) {
        struct advc_av1_access_unit_info info;
        uint8_t *output = NULL;
        size_t output_size = 0;
        int result = advc_av1_build_access_unit(
            &picture, &raw, 1, raw_key_frame, limit, 64, 64, &output,
            &output_size, &info);
        if (result == 0) free(output);
    }
    for (byte = 0; byte < sizeof(annex_b_key_frame); ++byte) {
        for (bit = 0; bit < 8; ++bit) {
            struct advc_av1_access_unit_info info;
            uint8_t *output = NULL;
            size_t output_size = 0;
            int result;
            memcpy(mutated, annex_b_key_frame, sizeof(mutated));
            mutated[byte] ^= (uint8_t)(1u << bit);
            result = advc_av1_build_access_unit(
                &picture, &annex, 1, mutated, sizeof(mutated), 64, 64,
                &output, &output_size, &info);
            if (result == 0) free(output);
        }
    }
}

static void test_official_real_vector(void) {
    const char *path = getenv("ADVC_AV1_REAL_VECTOR");
    VADecPictureParameterBufferAV1 picture;
    VASliceParameterBufferAV1 tile;
    struct advc_av1_access_unit_info info;
    uint8_t input[292];
    uint8_t *output = NULL;
    size_t output_size = 0;
    FILE *file;
    if (path == NULL || path[0] == '\0') return;
    file = fopen(path, "rb");
    assert(file != NULL);
    assert(fread(input, 1, sizeof(input), file) == sizeof(input));
    assert(fgetc(file) == EOF);
    assert(fclose(file) == 0);
    assert(input[0] == 0x12 && input[1] == 0x00);
    assert(input[2] == 0x0a && input[3] == 0x0a);
    assert(input[14] == 0x32 && input[15] == 0x93 && input[16] == 0x02);

    picture = valid_picture();
    picture.frame_width_minus1 = 31;
    picture.frame_height_minus1 = 31;
    tile = raw_tile();
    tile.slice_data_offset = 17;
    tile.slice_data_size = 275;
    assert(advc_av1_build_access_unit(
               &picture, &tile, 1, input, sizeof(input), 32, 32, &output,
               &output_size, &info) == 0);
    assert(output_size == sizeof(input));
    assert(memcmp(output, input, sizeof(input)) == 0);
    assert(info.input_format == ADVC_AV1_INPUT_RAW_OBU);
    assert(info.key_frame == 1 && info.contains_sequence_header == 1);
    assert(info.obu_count == 3);
    free(output);
}

int main(void) {
    test_raw_obu_exact_copy();
    test_annex_b_exact_unwrap();
    test_fail_closed_profile_and_format();
    test_fail_closed_fragments_and_ranges();
    test_fail_closed_incomplete_and_mismatch();
    test_bounded_mutations();
    test_official_real_vector();
    puts("ADVC VA-API AV1 Main raw OBU/Annex-B translator: PASS");
    return 0;
}
