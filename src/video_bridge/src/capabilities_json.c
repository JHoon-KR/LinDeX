#include "advc/capabilities.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int append(char **cursor, size_t *remaining, const char *format, ...) {
    va_list args;
    int result;
    va_start(args, format);
    result = vsnprintf(*cursor, *remaining, format, args);
    va_end(args);
    if (result < 0 || (size_t)result >= *remaining) return -1;
    *cursor += result;
    *remaining -= (size_t)result;
    return 0;
}

static int append_json_string(char **cursor, size_t *remaining, const char *value) {
    if (append(cursor, remaining, "\"") < 0) return -1;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '\\' || *p == '"') {
            if (append(cursor, remaining, "\\%c", *p) < 0) return -1;
        } else if (*p < 0x20) {
            if (append(cursor, remaining, "\\u%04x", *p) < 0) return -1;
        } else {
            if (*remaining <= 1) return -1;
            *(*cursor)++ = (char)*p;
            **cursor = '\0';
            --*remaining;
        }
    }
    return append(cursor, remaining, "\"");
}

int advc_capabilities_write_json(const struct advc_capability_set *caps, char *out, size_t out_size) {
    char *cursor = out;
    size_t remaining = out_size;
    if (caps == NULL || out == NULL || out_size == 0 || caps->count > ADVC_MAX_CAPABILITIES) return -1;
    if (append(&cursor, &remaining,
               "{\"protocol\":\"%u.%u\",\"androidApi\":%u,\"transportFeatures\":%llu,\"codecs\":[",
               ADVC_VERSION_MAJOR, ADVC_VERSION_MINOR, caps->api_level,
               (unsigned long long)caps->transport_features) < 0) return -1;
    for (uint32_t i = 0; i < caps->count; ++i) {
        const struct advc_codec_capability *c = &caps->codecs[i];
        if (i > 0 && append(&cursor, &remaining, ",") < 0) return -1;
        if (append(&cursor, &remaining, "{\"mime\":") < 0 ||
            append_json_string(&cursor, &remaining, c->mime) < 0 ||
            append(&cursor, &remaining, ",\"codec\":") < 0 ||
            append_json_string(&cursor, &remaining, c->codec_name) < 0 ||
            append(&cursor, &remaining,
                   ",\"direction\":%u,\"acceleration\":%u,\"lowLatency\":%s,\"securePlayback\":%s,\"maxWidth\":%u,\"maxHeight\":%u,\"maxFpsMilli\":%u}",
                   c->direction, c->acceleration, c->low_latency ? "true" : "false",
                   c->secure_playback ? "true" : "false", c->max_width, c->max_height,
                   c->max_fps_milli) < 0) return -1;
    }
    if (append(&cursor, &remaining, "]}") < 0) return -1;
    return (int)(cursor - out);
}
