#ifndef ADVC_VAAPI_MODIFIER_POLICY_H
#define ADVC_VAAPI_MODIFIER_POLICY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADVC_VAAPI_QCOM_MODIFIER UINT64_C(0x0500000000000001)

enum advc_vaapi_modifier_mode {
    ADVC_VAAPI_MODIFIER_AUTO = 0,
    ADVC_VAAPI_MODIFIER_LINEAR = 1,
    ADVC_VAAPI_MODIFIER_QCOM = 2,
};

enum advc_vaapi_modifier_source {
    ADVC_VAAPI_MODIFIER_DEFAULT = 0,
    ADVC_VAAPI_MODIFIER_PRIMARY_ENV = 1,
    ADVC_VAAPI_MODIFIER_LEGACY_ENV = 2,
};

struct advc_vaapi_modifier_policy {
    enum advc_vaapi_modifier_mode mode;
    enum advc_vaapi_modifier_source source;
};

enum advc_vaapi_modifier_route {
    ADVC_VAAPI_MODIFIER_ROUTE_UNSUPPORTED = 0,
    ADVC_VAAPI_MODIFIER_ROUTE_DIRECT_LINEAR = 1,
    ADVC_VAAPI_MODIFIER_ROUTE_DIRECT_QCOM = 2,
    ADVC_VAAPI_MODIFIER_ROUTE_GPU_REPACK_LINEAR = 3,
};

/* Empty values are treated as unset. The direction-specific value wins. */
int advc_vaapi_modifier_policy_parse(const char *primary_value,
                                     const char *legacy_value,
                                     struct advc_vaapi_modifier_policy *out);

/*
 * QCOM direct is fail-closed: both the broker path and the importer must have
 * passed their validation gates. Async decode reservations are LINEAR-only.
 */
enum advc_vaapi_modifier_route advc_vaapi_modifier_route_decode(
    const struct advc_vaapi_modifier_policy *policy, uint64_t modifier,
    int descriptor_valid, int broker_qcom_capable,
    int consumer_qcom_validated, int gpu_repack_available,
    int async_linear_reservation);

/* Encode fallback is GPU-only. There is deliberately no CPU-copy route. */
enum advc_vaapi_modifier_route advc_vaapi_modifier_route_encode(
    const struct advc_vaapi_modifier_policy *policy, uint64_t modifier,
    int descriptor_valid, int broker_qcom_capable,
    int broker_import_validated, int gpu_repack_available);

const char *advc_vaapi_modifier_mode_name(enum advc_vaapi_modifier_mode mode);

#ifdef __cplusplus
}
#endif

#endif
