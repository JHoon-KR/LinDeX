#include "advc_vaapi_modifier_policy.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

static struct advc_vaapi_modifier_policy parse(const char *primary,
                                                const char *legacy) {
    struct advc_vaapi_modifier_policy policy;
    assert(advc_vaapi_modifier_policy_parse(primary, legacy, &policy) == 0);
    return policy;
}

static void test_parser_and_priority(void) {
    struct advc_vaapi_modifier_policy policy = parse(NULL, NULL);
    assert(policy.mode == ADVC_VAAPI_MODIFIER_AUTO);
    assert(policy.source == ADVC_VAAPI_MODIFIER_DEFAULT);
    policy = parse("linear", "qcom");
    assert(policy.mode == ADVC_VAAPI_MODIFIER_LINEAR);
    assert(policy.source == ADVC_VAAPI_MODIFIER_PRIMARY_ENV);
    policy = parse("", "qcom");
    assert(policy.mode == ADVC_VAAPI_MODIFIER_QCOM);
    assert(policy.source == ADVC_VAAPI_MODIFIER_LEGACY_ENV);
    errno = 0;
    assert(advc_vaapi_modifier_policy_parse("QCOM", "linear", &policy) < 0);
    assert(errno == EINVAL);
}

static void test_decode_routes(void) {
    struct advc_vaapi_modifier_policy policy = parse("auto", NULL);
    assert(advc_vaapi_modifier_route_decode(
               &policy, 0, 1, 0, 0, 1, 0) ==
           ADVC_VAAPI_MODIFIER_ROUTE_DIRECT_LINEAR);
    assert(advc_vaapi_modifier_route_decode(
               &policy, ADVC_VAAPI_QCOM_MODIFIER, 1, 1, 1, 1, 0) ==
           ADVC_VAAPI_MODIFIER_ROUTE_DIRECT_QCOM);
    assert(advc_vaapi_modifier_route_decode(
               &policy, ADVC_VAAPI_QCOM_MODIFIER, 1, 0, 1, 1, 0) ==
           ADVC_VAAPI_MODIFIER_ROUTE_GPU_REPACK_LINEAR);
    assert(advc_vaapi_modifier_route_decode(
               &policy, ADVC_VAAPI_QCOM_MODIFIER, 1, 1, 1, 1, 1) ==
           ADVC_VAAPI_MODIFIER_ROUTE_GPU_REPACK_LINEAR);
    policy = parse("qcom", NULL);
    assert(advc_vaapi_modifier_route_decode(
               &policy, ADVC_VAAPI_QCOM_MODIFIER, 1, 0, 1, 1, 0) ==
           ADVC_VAAPI_MODIFIER_ROUTE_UNSUPPORTED);
    policy = parse("linear", NULL);
    assert(advc_vaapi_modifier_route_decode(
               &policy, ADVC_VAAPI_QCOM_MODIFIER, 1, 1, 1, 1, 0) ==
           ADVC_VAAPI_MODIFIER_ROUTE_UNSUPPORTED);
}

static void test_encode_routes(void) {
    struct advc_vaapi_modifier_policy policy = parse("auto", NULL);
    assert(advc_vaapi_modifier_route_encode(
               &policy, ADVC_VAAPI_QCOM_MODIFIER, 1, 1, 1, 1) ==
           ADVC_VAAPI_MODIFIER_ROUTE_DIRECT_QCOM);
    assert(advc_vaapi_modifier_route_encode(
               &policy, ADVC_VAAPI_QCOM_MODIFIER, 1, 0, 1, 1) ==
           ADVC_VAAPI_MODIFIER_ROUTE_GPU_REPACK_LINEAR);
    policy = parse("qcom", NULL);
    assert(advc_vaapi_modifier_route_encode(
               &policy, ADVC_VAAPI_QCOM_MODIFIER, 1, 0, 1, 1) ==
           ADVC_VAAPI_MODIFIER_ROUTE_UNSUPPORTED);
    assert(advc_vaapi_modifier_route_encode(
               &policy, 0, 1, 1, 1, 1) ==
           ADVC_VAAPI_MODIFIER_ROUTE_UNSUPPORTED);
    policy = parse("linear", NULL);
    assert(advc_vaapi_modifier_route_encode(
               &policy, 0, 1, 0, 0, 0) ==
           ADVC_VAAPI_MODIFIER_ROUTE_DIRECT_LINEAR);
}

int main(void) {
    test_parser_and_priority();
    test_decode_routes();
    test_encode_routes();
    puts("advc VA-API modifier policy: PASS");
    return 0;
}
