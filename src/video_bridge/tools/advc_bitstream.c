#include "advc_bitstream.h"

#include <errno.h>
#include <string.h>

enum codec_kind {
    CODEC_AVC,
    CODEC_HEVC,
};

static uint16_t get_be16(const uint8_t *data) {
    return (uint16_t)((uint16_t)data[0] << 8 | data[1]);
}

static uint32_t get_be32(const uint8_t *data) {
    return (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 |
           (uint32_t)data[2] << 8 | data[3];
}

static int add_nal(enum codec_kind codec, const uint8_t *data, size_t size,
                   struct advc_bitstream_stats *stats) {
    uint8_t type;
    if (data == NULL || stats == NULL || size == 0) return -1;
    if (codec == CODEC_AVC) {
        if ((data[0] & 0x80u) != 0) return -1;
        type = data[0] & 0x1fu;
        if (type == 0 || type >= 24) return -1;
        if (type == 7 || type == 8) {
            ++stats->parameter_sets;
            stats->parameter_set_mask |= type == 7 ? 1u : 2u;
        }
        if (type >= 1 && type <= 5) {
            ++stats->vcl_units;
            if (type == 5) ++stats->key_vcl_units;
        }
    } else {
        if (size < 2 || (data[0] & 0x80u) != 0 || (data[1] & 0x07u) == 0)
            return -1;
        type = (data[0] >> 1) & 0x3fu;
        if (type > 40) return -1;
        if (type == 32 || type == 33 || type == 34) {
            ++stats->parameter_sets;
            stats->parameter_set_mask |= 1u << (type - 32u);
        }
        if (type <= 31) {
            ++stats->vcl_units;
            if (type >= 16 && type <= 23) ++stats->key_vcl_units;
        }
    }
    ++stats->nal_units;
    return 0;
}

static int find_start_code(const uint8_t *data, size_t size, size_t from,
                           size_t *offset, size_t *length) {
    for (size_t i = from; i + 2 < size; ++i) {
        if (data[i] != 0 || data[i + 1] != 0) continue;
        if (data[i + 2] == 1) {
            *offset = i;
            *length = 3;
            return 0;
        }
        if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
            *offset = i;
            *length = 4;
            return 0;
        }
    }
    return -1;
}

static int inspect_annex_b(enum codec_kind codec, const uint8_t *data, size_t size,
                           struct advc_bitstream_stats *stats) {
    size_t start;
    size_t start_length;
    if (find_start_code(data, size, 0, &start, &start_length) < 0) return -1;
    for (size_t i = 0; i < start; ++i) {
        if (data[i] != 0) return -1;
    }
    while (start < size) {
        size_t payload = start + start_length;
        size_t next;
        size_t next_length;
        size_t end = size;
        int have_next = 0;
        if (payload >= size) return -1;
        if (find_start_code(data, size, payload, &next, &next_length) == 0) {
            end = next;
            have_next = 1;
        }
        while (end > payload && data[end - 1] == 0) --end;
        if (end == payload || add_nal(codec, data + payload, end - payload, stats) < 0)
            return -1;
        if (!have_next) break;
        start = next;
        start_length = next_length;
    }
    return stats->nal_units > 0 ? 0 : -1;
}

static int inspect_length_prefixed(enum codec_kind codec, const uint8_t *data,
                                   size_t size, struct advc_bitstream_stats *stats) {
    size_t offset = 0;
    while (offset < size) {
        uint32_t nal_size;
        if (size - offset < 4) return -1;
        nal_size = get_be32(data + offset);
        offset += 4;
        if (nal_size == 0 || nal_size > size - offset ||
            add_nal(codec, data + offset, nal_size, stats) < 0)
            return -1;
        offset += nal_size;
    }
    return stats->nal_units > 0 ? 0 : -1;
}

static int inspect_avcc(const uint8_t *data, size_t size,
                        struct advc_bitstream_stats *stats) {
    size_t offset = 6;
    uint8_t count;
    if (size < 7 || data[0] != 1 || (data[4] & 0xfcu) != 0xfcu ||
        (data[5] & 0xe0u) != 0xe0u)
        return -1;
    count = data[5] & 0x1fu;
    if (count == 0) return -1;
    for (uint8_t i = 0; i < count; ++i) {
        uint16_t nal_size;
        if (size - offset < 2) return -1;
        nal_size = get_be16(data + offset);
        offset += 2;
        if (nal_size == 0 || nal_size > size - offset ||
            add_nal(CODEC_AVC, data + offset, nal_size, stats) < 0 ||
            (data[offset] & 0x1fu) != 7)
            return -1;
        offset += nal_size;
    }
    if (offset >= size) return -1;
    count = data[offset++];
    if (count == 0) return -1;
    for (uint8_t i = 0; i < count; ++i) {
        uint16_t nal_size;
        if (size - offset < 2) return -1;
        nal_size = get_be16(data + offset);
        offset += 2;
        if (nal_size == 0 || nal_size > size - offset ||
            add_nal(CODEC_AVC, data + offset, nal_size, stats) < 0 ||
            (data[offset] & 0x1fu) != 8)
            return -1;
        offset += nal_size;
    }
    if (offset < size) {
        uint8_t extensions;
        int extension_profile = data[1] == 100 || data[1] == 110 ||
                                data[1] == 122 || data[1] == 144 ||
                                data[1] == 83 || data[1] == 86 ||
                                data[1] == 118 || data[1] == 128 ||
                                data[1] == 138 || data[1] == 139 ||
                                data[1] == 134 || data[1] == 135;
        if (!extension_profile || size - offset < 4 ||
            (data[offset] & 0xfcu) != 0xfcu ||
            (data[offset + 1] & 0xf8u) != 0xf8u ||
            (data[offset + 2] & 0xf8u) != 0xf8u)
            return -1;
        extensions = data[offset + 3];
        offset += 4;
        for (uint8_t i = 0; i < extensions; ++i) {
            uint16_t nal_size;
            if (size - offset < 2) return -1;
            nal_size = get_be16(data + offset);
            offset += 2;
            if (nal_size == 0 || nal_size > size - offset ||
                add_nal(CODEC_AVC, data + offset, nal_size, stats) < 0 ||
                (data[offset] & 0x1fu) != 13)
                return -1;
            offset += nal_size;
        }
    }
    return offset == size && (stats->parameter_set_mask & 3u) == 3u ? 0 : -1;
}

static int inspect_hvcc(const uint8_t *data, size_t size,
                        struct advc_bitstream_stats *stats) {
    size_t offset = 23;
    uint8_t arrays;
    if (size < 23 || data[0] != 1) return -1;
    arrays = data[22];
    if (arrays == 0) return -1;
    for (uint8_t i = 0; i < arrays; ++i) {
        uint8_t expected_type;
        uint16_t count;
        if (size - offset < 3) return -1;
        expected_type = data[offset] & 0x3fu;
        count = get_be16(data + offset + 1);
        offset += 3;
        if (count == 0) return -1;
        for (uint16_t j = 0; j < count; ++j) {
            uint16_t nal_size;
            if (size - offset < 2) return -1;
            nal_size = get_be16(data + offset);
            offset += 2;
            if (nal_size < 2 || nal_size > size - offset ||
                ((data[offset] >> 1) & 0x3fu) != expected_type ||
                add_nal(CODEC_HEVC, data + offset, nal_size, stats) < 0)
                return -1;
            offset += nal_size;
        }
    }
    return offset == size && (stats->parameter_set_mask & 7u) == 7u ? 0 : -1;
}

int advc_bitstream_inspect(const char *mime, const uint8_t *data, size_t size,
                           struct advc_bitstream_stats *stats) {
    enum codec_kind codec;
    struct advc_bitstream_stats candidate;
    size_t start;
    size_t start_length;
    if (mime == NULL || data == NULL || stats == NULL || size == 0) {
        errno = EINVAL;
        return -1;
    }
    if (strcmp(mime, "video/avc") == 0) {
        codec = CODEC_AVC;
    } else if (strcmp(mime, "video/hevc") == 0) {
        codec = CODEC_HEVC;
    } else {
        errno = EINVAL;
        return -1;
    }

    memset(&candidate, 0, sizeof(candidate));
    if (find_start_code(data, size, 0, &start, &start_length) == 0 && start <= 3) {
        (void)start_length;
        candidate.format = ADVC_BITSTREAM_ANNEX_B;
        if (inspect_annex_b(codec, data, size, &candidate) == 0) {
            *stats = candidate;
            return 0;
        }
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.format = ADVC_BITSTREAM_LENGTH_PREFIXED;
    if (inspect_length_prefixed(codec, data, size, &candidate) == 0) {
        *stats = candidate;
        return 0;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.format = ADVC_BITSTREAM_CODEC_CONFIG_RECORD;
    if ((codec == CODEC_AVC ? inspect_avcc(data, size, &candidate)
                            : inspect_hvcc(data, size, &candidate)) == 0) {
        *stats = candidate;
        return 0;
    }
    errno = EPROTO;
    return -1;
}

const char *advc_bitstream_format_name(enum advc_bitstream_format format) {
    switch (format) {
    case ADVC_BITSTREAM_ANNEX_B:
        return "annex-b";
    case ADVC_BITSTREAM_LENGTH_PREFIXED:
        return "length-prefixed-4";
    case ADVC_BITSTREAM_CODEC_CONFIG_RECORD:
        return "codec-config-record";
    default:
        return "unknown";
    }
}
