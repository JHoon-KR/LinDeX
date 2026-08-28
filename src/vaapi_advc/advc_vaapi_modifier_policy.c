#include "advc_vaapi_modifier_policy.h"

#include <errno.h>
#include <string.h>

static int parse_mode(const char *value, enum advc_vaapi_modifier_mode *mode) {
    if (strcmp(value, "auto") == 0) {
        *mode = ADVC_VAAPI_MODIFIER_AUTO;
        return 0;
    }
    if (strcmp(value, "linear") == 0) {
        *mode = ADVC_VAAPI_MODIFIER_LINEAR;
        return 0;
    }
    if (strcmp(value, "qcom") == 0) {
        *mode = ADVC_VAAPI_MODIFIER_QCOM;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

int advc_vaapi_modifier_policy_parse(const char *primary_value,
                                     const char *legacy_value,
                                     struct advc_vaapi_modifier_policy *out) {
    const char *value;
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    out->mode = ADVC_VAAPI_MODIFIER_AUTO;
    out->source = ADVC_VAAPI_MODIFIER_DEFAULT;
    if (primary_value != NULL && primary_value[0] != '\0') {
        value = primary_value;
        out->source = ADVC_VAAPI_MODIFIER_PRIMARY_ENV;
    } else if (legacy_value != NULL && legacy_value[0] != '\0') {
        value = legacy_value;
        out->source = ADVC_VAAPI_MODIFIER_LEGACY_ENV;
    } else {
        return 0;
    }
    if (parse_mode(value, &out->mode) < 0) {
        out->source = ADVC_VAAPI_MODIFIER_DEFAULT;
        return -1;
    }
    return 0;
}

static int qcom_direct_ready(int broker_capable, int importer_validated) {
    return broker_capable && importer_validated;
}

enum advc_vaapi_modifier_route advc_vaapi_modifier_route_decode(
    const struct advc_vaapi_modifier_policy *policy, uint64_t modifier,
    int descriptor_valid, int broker_qcom_capable,
    int consumer_qcom_validated, int gpu_repack_available,
    int async_linear_reservation) {
    if (policy == NULL || !descriptor_valid)
        return ADVC_VAAPI_MODIFIER_ROUTE_UNSUPPORTED;
    if (modifier == 0)
        return policy->mode == ADVC_VAAPI_MODIFIER_QCOM
                   ? ADVC_VAAPI_MODIFIER_ROUTE_UNSUPPORTED
                   : ADVC_VAAPI_MODIFIER_ROUTE_DIRECT_LINEAR;
    if (modifier != ADVC_VAAPI_QCOM_MODIFIER ||
        policy->mode == ADVC_VAAPI_MODIFIER_LINEAR)
        return ADVC_VAAPI_MODIFIER_ROUTE_UNSUPPORTED;
    if (!async_linear_reservation &&
        qcom_direct_ready(broker_qcom_capable,
                          consumer_qcom_validated))
        return ADVC_VAAPI_MODIFIER_ROUTE_DIRECT_QCOM;
    if (policy->mode == ADVC_VAAPI_MODIFIER_QCOM)
        return ADVC_VAAPI_MODIFIER_ROUTE_UNSUPPORTED;
    return gpu_repack_available
               ? ADVC_VAAPI_MODIFIER_ROUTE_GPU_REPACK_LINEAR
               : ADVC_VAAPI_MODIFIER_ROUTE_UNSUPPORTED;
}

enum advc_vaapi_modifier_route advc_vaapi_modifier_route_encode(
    const struct advc_vaapi_modifier_policy *policy, uint64_t modifier,
    int descriptor_valid, int broker_qcom_capable,
    int broker_import_validated, int gpu_repack_available) {
    if (policy == NULL || !descriptor_valid)
        return ADVC_VAAPI_MODIFIER_ROUTE_UNSUPPORTED;
    if (modifier == 0)
        return policy->mode == ADVC_VAAPI_MODIFIER_QCOM
                   ? ADVC_VAAPI_MODIFIER_ROUTE_UNSUPPORTED
                   : ADVC_VAAPI_MODIFIER_ROUTE_DIRECT_LINEAR;
    if (modifier != ADVC_VAAPI_QCOM_MODIFIER ||
        policy->mode == ADVC_VAAPI_MODIFIER_LINEAR)
        return ADVC_VAAPI_MODIFIER_ROUTE_UNSUPPORTED;
    if (qcom_direct_ready(broker_qcom_capable, broker_import_validated))
        return ADVC_VAAPI_MODIFIER_ROUTE_DIRECT_QCOM;
    if (policy->mode == ADVC_VAAPI_MODIFIER_QCOM)
        return ADVC_VAAPI_MODIFIER_ROUTE_UNSUPPORTED;
    return gpu_repack_available
               ? ADVC_VAAPI_MODIFIER_ROUTE_GPU_REPACK_LINEAR
               : ADVC_VAAPI_MODIFIER_ROUTE_UNSUPPORTED;
}

const char *advc_vaapi_modifier_mode_name(enum advc_vaapi_modifier_mode mode) {
    if (mode == ADVC_VAAPI_MODIFIER_LINEAR) return "linear";
    if (mode == ADVC_VAAPI_MODIFIER_QCOM) return "qcom";
    return "auto";
}
