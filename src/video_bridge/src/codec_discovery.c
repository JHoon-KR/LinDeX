#include "advc/capabilities.h"

#include <stddef.h>
#include <string.h>

static int starts_with(const char *value, const char *prefix) {
    size_t prefix_length = strlen(prefix);
    return strncmp(value, prefix, prefix_length) == 0;
}

static int ends_with(const char *value, const char *suffix) {
    size_t value_length = strlen(value);
    size_t suffix_length = strlen(suffix);
    return value_length >= suffix_length &&
           strcmp(value + value_length - suffix_length, suffix) == 0;
}

uint8_t advc_classify_codec_component(const char *name) {
    static const char *const software_prefixes[] = {
        "c2.android.", "c2.google.", "c2.ffmpeg.",
        "OMX.google.", "OMX.ffmpeg.",
    };
    static const char *const hardware_prefixes[] = {
        "c2.qti.",      "c2.qcom.",    "c2.mtk.",
        "c2.exynos.",   "c2.samsung.", "c2.unisoc.",
        "c2.rk.",       "c2.amlogic.", "c2.nvidia.",
        "c2.intel.",    "OMX.qcom.",   "OMX.QCOM.",
        "OMX.MTK.",     "OMX.Exynos.", "OMX.SEC.",
        "OMX.Intel.",   "OMX.Nvidia.", "OMX.rk.",
        "OMX.amlogic.",
    };
    size_t i;

    if (name == NULL || name[0] == '\0')
        return ADVC_ACCELERATION_UNKNOWN;
    if (strstr(name, ".secure") != NULL ||
        strstr(name, ".software.") != NULL ||
        strstr(name, ".sw.") != NULL || ends_with(name, ".software") ||
        ends_with(name, ".sw"))
        return ADVC_ACCELERATION_UNKNOWN;
    for (i = 0; i < sizeof(software_prefixes) / sizeof(software_prefixes[0]);
         ++i) {
        if (starts_with(name, software_prefixes[i]))
            return ADVC_ACCELERATION_SOFTWARE;
    }
    for (i = 0; i < sizeof(hardware_prefixes) / sizeof(hardware_prefixes[0]);
         ++i) {
        if (starts_with(name, hardware_prefixes[i]))
            return ADVC_ACCELERATION_HARDWARE;
    }
    return ADVC_ACCELERATION_UNKNOWN;
}
