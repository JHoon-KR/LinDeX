#include "advc_bitstream.h"
#include "advc_media_time.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void test_avc_annex_b(void) {
    static const uint8_t data[] = {
        0, 0, 0, 1, 0x67, 0x64, 0, 0x1f,
        0, 0, 1, 0x68, 0xee, 0x3c, 0x80,
        0, 0, 1, 0x65, 0x88, 0x84,
    };
    struct advc_bitstream_stats stats;
    assert(advc_bitstream_inspect("video/avc", data, sizeof(data), &stats) == 0);
    assert(stats.format == ADVC_BITSTREAM_ANNEX_B);
    assert(stats.nal_units == 3);
    assert(stats.parameter_sets == 2);
    assert(stats.parameter_set_mask == 3);
    assert(stats.vcl_units == 1);
    assert(stats.key_vcl_units == 1);
}

static void test_avc_length_prefixed(void) {
    static const uint8_t data[] = {
        0, 0, 0, 2, 0x67, 0x64,
        0, 0, 0, 2, 0x68, 0xee,
        0, 0, 0, 2, 0x41, 0x9a,
    };
    struct advc_bitstream_stats stats;
    assert(advc_bitstream_inspect("video/avc", data, sizeof(data), &stats) == 0);
    assert(stats.format == ADVC_BITSTREAM_LENGTH_PREFIXED);
    assert(stats.nal_units == 3);
    assert(stats.parameter_sets == 2);
    assert(stats.parameter_set_mask == 3);
    assert(stats.vcl_units == 1);
    assert(stats.key_vcl_units == 0);
}

static void test_avcc(void) {
    static const uint8_t data[] = {
        1, 0x64, 0, 0x1f, 0xff, 0xe1,
        0, 2, 0x67, 0x64,
        1, 0, 2, 0x68, 0xee,
    };
    struct advc_bitstream_stats stats;
    assert(advc_bitstream_inspect("video/avc", data, sizeof(data), &stats) == 0);
    assert(stats.format == ADVC_BITSTREAM_CODEC_CONFIG_RECORD);
    assert(stats.nal_units == 2);
    assert(stats.parameter_sets == 2);
    assert(stats.parameter_set_mask == 3);
}

static void test_hevc_annex_b(void) {
    static const uint8_t data[] = {
        0, 0, 0, 1, 0x40, 1, 0xaa,
        0, 0, 1, 0x42, 1, 0xbb,
        0, 0, 1, 0x44, 1, 0xcc,
        0, 0, 1, 0x26, 1, 0xdd,
    };
    struct advc_bitstream_stats stats;
    assert(advc_bitstream_inspect("video/hevc", data, sizeof(data), &stats) == 0);
    assert(stats.format == ADVC_BITSTREAM_ANNEX_B);
    assert(stats.nal_units == 4);
    assert(stats.parameter_sets == 3);
    assert(stats.parameter_set_mask == 7);
    assert(stats.vcl_units == 1);
    assert(stats.key_vcl_units == 1);
}

static void test_hevc_length_prefixed(void) {
    static const uint8_t data[] = {
        0, 0, 0, 3, 0x40, 1, 0xaa,
        0, 0, 0, 3, 0x42, 1, 0xbb,
        0, 0, 0, 3, 0x44, 1, 0xcc,
        0, 0, 0, 3, 0x02, 1, 0xdd,
    };
    struct advc_bitstream_stats stats;
    assert(advc_bitstream_inspect("video/hevc", data, sizeof(data), &stats) == 0);
    assert(stats.format == ADVC_BITSTREAM_LENGTH_PREFIXED);
    assert(stats.nal_units == 4);
    assert(stats.parameter_set_mask == 7);
    assert(stats.vcl_units == 1);
    assert(stats.key_vcl_units == 0);
}

static void test_hvcc(void) {
    static const uint8_t data[] = {
        1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 3,
        0xa0, 0, 1, 0, 3, 0x40, 1, 0xaa,
        0xa1, 0, 1, 0, 3, 0x42, 1, 0xbb,
        0xa2, 0, 1, 0, 3, 0x44, 1, 0xcc,
    };
    struct advc_bitstream_stats stats;
    assert(advc_bitstream_inspect("video/hevc", data, sizeof(data), &stats) == 0);
    assert(stats.format == ADVC_BITSTREAM_CODEC_CONFIG_RECORD);
    assert(stats.nal_units == 3);
    assert(stats.parameter_sets == 3);
    assert(stats.parameter_set_mask == 7);
}

static void test_malformed(void) {
    static const uint8_t short_length[] = {0, 0, 0, 8, 0x65};
    static const uint8_t empty_nal[] = {0, 0, 0, 1};
    static const uint8_t bad_hevc[] = {0, 0, 0, 1, 0x40};
    static const uint8_t avcc_junk[] = {
        1, 0x42, 0, 0x1f, 0xff, 0xe1, 0, 2, 0x67, 0x42,
        1, 0, 2, 0x68, 0xee, 0xff,
    };
    struct advc_bitstream_stats stats;
    errno = 0;
    assert(advc_bitstream_inspect("video/avc", short_length,
                                  sizeof(short_length), &stats) == -1);
    assert(errno == EPROTO);
    assert(advc_bitstream_inspect("video/avc", empty_nal, sizeof(empty_nal),
                                  &stats) == -1);
    assert(advc_bitstream_inspect("video/hevc", bad_hevc, sizeof(bad_hevc),
                                  &stats) == -1);
    assert(advc_bitstream_inspect("video/avc", avcc_junk, sizeof(avcc_junk),
                                  &stats) == -1);
    errno = 0;
    assert(advc_bitstream_inspect("video/vp9", short_length,
                                  sizeof(short_length), &stats) == -1);
    assert(errno == EINVAL);
}

static void test_media_codec_pts_quantization(void) {
    uint64_t frame_1_30fps = UINT64_C(1000000000000) / UINT64_C(30000);
    uint64_t frame_31_30fps = UINT64_C(31) * UINT64_C(1000000000000) /
                              UINT64_C(30000);
    uint64_t frame_1_25fps = UINT64_C(1000000000000) / UINT64_C(25000);
    assert(frame_1_30fps == UINT64_C(33333333));
    assert(advc_media_codec_roundtrip_pts_ns(frame_1_30fps) ==
           UINT64_C(33333000));
    assert(frame_31_30fps == UINT64_C(1033333333));
    assert(advc_media_codec_roundtrip_pts_ns(frame_31_30fps) ==
           UINT64_C(1033333000));
    assert(advc_media_codec_roundtrip_pts_ns(frame_1_25fps) ==
           UINT64_C(40000000));
}

int main(void) {
    test_avc_annex_b();
    test_avc_length_prefixed();
    test_avcc();
    test_hevc_annex_b();
    test_hevc_length_prefixed();
    test_hvcc();
    test_malformed();
    test_media_codec_pts_quantization();
    puts("bitstream tests passed");
    return 0;
}
