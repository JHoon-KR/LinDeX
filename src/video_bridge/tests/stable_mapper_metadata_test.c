#include "stable_mapper_metadata.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct writer {
    uint8_t data[1024];
    size_t size;
};

static void put(struct writer *writer, const void *data, size_t size) {
    assert(writer->size + size <= sizeof(writer->data));
    memcpy(writer->data + writer->size, data, size);
    writer->size += size;
}

static void put_i64(struct writer *writer, int64_t value) {
    put(writer, &value, sizeof(value));
}

static void put_i32(struct writer *writer, int32_t value) {
    put(writer, &value, sizeof(value));
}

static void put_string(struct writer *writer, const char *value) {
    int64_t length = (int64_t)strlen(value);
    put_i64(writer, length);
    put(writer, value, (size_t)length);
}

static void put_header(struct writer *writer, int64_t type) {
    put_string(writer,
               "android.hardware.graphics.common.StandardMetadataType");
    put_i64(writer, type);
}

static void scalar_test(void) {
    struct writer writer = {0};
    uint64_t value = UINT64_C(0x1122334455667788);
    uint64_t decoded = 0;
    put_header(&writer, ADVC_STANDARD_METADATA_PIXEL_FORMAT_MODIFIER);
    put(&writer, &value, sizeof(value));
    assert(advc_mapper_metadata_decode_u64(
               writer.data, writer.size,
               ADVC_STANDARD_METADATA_PIXEL_FORMAT_MODIFIER, &decoded) == 0);
    assert(decoded == value);
    writer.data[8] ^= 1u;
    assert(advc_mapper_metadata_decode_u64(
               writer.data, writer.size,
               ADVC_STANDARD_METADATA_PIXEL_FORMAT_MODIFIER, &decoded) < 0);
}

static void layouts_test(void) {
    struct writer writer = {0};
    struct advc_mapper_plane_layouts layouts;
    put_header(&writer, ADVC_STANDARD_METADATA_PLANE_LAYOUTS);
    put_i64(&writer, 2);
    for (int plane = 0; plane < 2; ++plane) {
        put_i64(&writer, 1);
        put_string(&writer,
                   "android.hardware.graphics.common.PlaneLayoutComponentType");
        put_i64(&writer, plane == 0 ? 1 : 2);
        put_i64(&writer, 0);
        put_i64(&writer, 8);
        put_i64(&writer, plane == 0 ? 0 : 4096);
        put_i64(&writer, plane == 0 ? 8 : 16);
        put_i64(&writer, 128);
        put_i64(&writer, plane == 0 ? 128 : 64);
        put_i64(&writer, plane == 0 ? 64 : 32);
        put_i64(&writer, plane == 0 ? 8192 : 4096);
        put_i64(&writer, plane == 0 ? 1 : 2);
        put_i64(&writer, plane == 0 ? 1 : 2);
    }
    assert(advc_mapper_metadata_decode_plane_layouts(
               writer.data, writer.size, &layouts) == 0);
    assert(layouts.count == 2);
    assert(layouts.planes[0].component_count == 1);
    assert(strcmp(layouts.planes[0].components[0].type_name,
                  "android.hardware.graphics.common.PlaneLayoutComponentType") ==
           0);
    assert(layouts.planes[0].components[0].type_value == 1);
    assert(layouts.planes[0].components[0].offset_bits == 0);
    assert(layouts.planes[0].components[0].size_bits == 8);
    assert(layouts.planes[0].offset_bytes == 0);
    assert(layouts.planes[0].stride_bytes == 128);
    assert(layouts.planes[1].offset_bytes == 4096);
    assert(layouts.planes[1].horizontal_subsampling == 2);
    writer.data[writer.size++] = 0;
    assert(advc_mapper_metadata_decode_plane_layouts(
               writer.data, writer.size, &layouts) < 0);
}

static void crop_test(void) {
    struct writer writer = {0};
    struct advc_mapper_crops crops;
    put_header(&writer, ADVC_STANDARD_METADATA_CROP);
    put_i64(&writer, 1);
    put_i32(&writer, 4);
    put_i32(&writer, 2);
    put_i32(&writer, 124);
    put_i32(&writer, 62);
    assert(advc_mapper_metadata_decode_crops(
               writer.data, writer.size, &crops) == 0);
    assert(crops.count == 1 && crops.crops[0].left == 4 &&
           crops.crops[0].top == 2 && crops.crops[0].right == 124 &&
           crops.crops[0].bottom == 62);
    memcpy(writer.data + writer.size - sizeof(int32_t), &crops.crops[0].top,
           sizeof(crops.crops[0].top));
    assert(advc_mapper_metadata_decode_crops(
               writer.data, writer.size, &crops) < 0);
    assert(errno == EPROTO);
}

static void prime_layout_gate_test(void) {
    assert(advc_mapper_prime_layout_gate(1, 4, 4) ==
           ADVC_PRIME_LAYOUT_GATE_PASS);
    assert(advc_mapper_prime_layout_gate(2, 4, 1) ==
           ADVC_PRIME_LAYOUT_GATE_CROP_PLANE_COUNT_MISMATCH);
    assert(advc_mapper_prime_layout_gate(2, 4, 4) ==
           ADVC_PRIME_LAYOUT_GATE_AMBIGUOUS_MULTI_FD);
}

static void set_component(struct advc_mapper_plane_component *component,
                          const char *name, int64_t value, int64_t offset_bits,
                          int64_t size_bits) {
    strncpy(component->type_name, name, sizeof(component->type_name) - 1u);
    component->type_value = value;
    component->offset_bits = offset_bits;
    component->size_bits = size_bits;
}

static void set_layout(struct advc_mapper_plane_layout *layout,
                       uint64_t offset, uint64_t stride, uint64_t width,
                       uint64_t height, uint64_t total, uint64_t hsub,
                       uint64_t vsub) {
    layout->offset_bytes = offset;
    layout->sample_increment_bits = 8;
    layout->stride_bytes = stride;
    layout->width_samples = width;
    layout->height_samples = height;
    layout->total_size_bytes = total;
    layout->horizontal_subsampling = hsub;
    layout->vertical_subsampling = vsub;
}

static void qti_nv12_normalizer_test(void) {
    static const char standard[] =
        "android.hardware.graphics.common.PlaneLayoutComponentType";
    struct advc_mapper_plane_layouts layouts = {0};
    struct advc_mapper_crops crops = {0};
    struct advc_mapper_qti_nv12_layout normalized;
    uint64_t fd_sizes[ADVC_MAX_DMABUF_OBJECTS] = {163840, 73728, 0, 0};

    layouts.count = 4;
    layouts.planes[0].component_count = 1;
    set_component(&layouts.planes[0].components[0], standard, 1, 0, 8);
    set_layout(&layouts.planes[0], 4096, 384, 320, 240, 98304, 1, 1);
    layouts.planes[1].component_count = 2;
    set_component(&layouts.planes[1].components[0], standard, 2, 0, 8);
    set_component(&layouts.planes[1].components[1], standard, 4, 8, 8);
    set_layout(&layouts.planes[1], 106496, 384, 160, 120, 49152, 2, 2);
    layouts.planes[2].component_count = 2;
    set_component(&layouts.planes[2].components[0], standard, 1, 0, 0);
    set_component(&layouts.planes[2].components[1], "QTI", INT32_MIN, 0, 0);
    set_layout(&layouts.planes[2], 0, 64, 320, 240, 4096, 1, 1);
    layouts.planes[3].component_count = 3;
    set_component(&layouts.planes[3].components[0], standard, 2, 0, 0);
    set_component(&layouts.planes[3].components[1], standard, 4, 0, 0);
    set_component(&layouts.planes[3].components[2], "QTI", INT32_MIN, 0, 0);
    set_layout(&layouts.planes[3], 102400, 64, 320, 240, 4096, 1, 1);
    crops.count = 1;
    crops.crops[0].right = 384;
    crops.crops[0].bottom = 256;
    assert(advc_mapper_qti_nv12_normalize(
               UINT32_C(0x3231564e), UINT64_C(0x0500000000000001), 163840,
               320, 240, 384, 2, fd_sizes, 0, 1, 163840, &layouts, &crops,
               &normalized) == 0);
    assert(normalized.kind == ADVC_MAPPER_QTI_NV12_UBWC);
    assert(normalized.image_transport_index == 0);
    assert(normalized.plane_count == 2);
    assert(normalized.plane_offsets[0] == 0);
    assert(normalized.plane_offsets[1] == 102400);
    assert(normalized.plane_strides[0] == 384);
    assert(normalized.plane_strides[1] == 384);
    assert(advc_mapper_qti_nv12_normalize(
               UINT32_C(0x3231564e), UINT64_C(0x0500000000000001), 163840,
               320, 240, 384, 2, fd_sizes, 1, 1, 73728, &layouts, &crops,
               &normalized) < 0);
    layouts.planes[3].components[2].type_value = 0;
    assert(advc_mapper_qti_nv12_normalize(
               UINT32_C(0x3231564e), UINT64_C(0x0500000000000001), 163840,
               320, 240, 384, 2, fd_sizes, 0, 1, 163840, &layouts, &crops,
               &normalized) < 0);

    memset(&layouts, 0, sizeof(layouts));
    layouts.count = 2;
    layouts.planes[0].component_count = 1;
    set_component(&layouts.planes[0].components[0], standard, 1, 0, 8);
    set_layout(&layouts.planes[0], 0, 384, 320, 240, 92160, 1, 1);
    layouts.planes[1].component_count = 2;
    set_component(&layouts.planes[1].components[0], standard, 2, 0, 8);
    set_component(&layouts.planes[1].components[1], standard, 4, 8, 8);
    set_layout(&layouts.planes[1], 92160, 384, 160, 120, 46080, 2, 2);
    crops.crops[0].right = 320;
    crops.crops[0].bottom = 240;
    fd_sizes[0] = 138240;
    assert(advc_mapper_qti_nv12_normalize(
               UINT32_C(0x3231564e), 0, 138240, 320, 240, 384, 2,
               fd_sizes, 0, 1, 138240, &layouts, &crops, &normalized) == 0);
    assert(normalized.kind == ADVC_MAPPER_QTI_NV12_LINEAR);
    assert(normalized.plane_offsets[0] == 0);
    assert(normalized.plane_offsets[1] == 92160);
}

int main(void) {
    scalar_test();
    layouts_test();
    crop_test();
    prime_layout_gate_test();
    qti_nv12_normalizer_test();
    puts("stable_mapper_metadata_test: PASS");
    return 0;
}
