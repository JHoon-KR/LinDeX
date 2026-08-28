#include "android_vulkan_drm_identity_policy.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

bool advk_layer_test_parse_ack(const char *ack,
	struct advk_identity_backend *backend);
bool advk_layer_test_owner_matches(const char *owner);

static void test_valid_dynamic_ack(void)
{
	struct advk_identity_backend backend;

	assert(advk_layer_test_parse_ack(
		"turnip-qualcomm-card0-renderD128-kgsl3d0-226-0-226-128-462-0-v2",
		&backend));
	assert(backend.identity_provided);
	assert(backend.primary_major == 226 && backend.primary_minor == 0);
	assert(backend.render_major == 226 && backend.render_minor == 128);
	assert(backend.kgsl_major == 462 && backend.kgsl_minor == 0);
}

static void test_invalid_ack_is_rejected(void)
{
	struct advk_identity_backend backend;
	const char *const invalid[] = {
		NULL,
		"turnip-qualcomm-card0-renderD128-kgsl3d0-226-0-226-128-462-0-v1",
		"turnip-qualcomm-card0-renderD128-kgsl3d0-226-0-226-128-462-v2",
		"turnip-qualcomm-card0-renderD128-kgsl3d0-226-0-226-128--1-0-v2",
		"turnip-qualcomm-card0-renderD128-kgsl3d0-226-0-226-128-4294967296-0-v2",
		"turnip-qualcomm-card0-renderD128-kgsl3d0-226-0-226-128-462-0-v2-tail",
	};
	unsigned int i;

	for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
		assert(!advk_layer_test_parse_ack(invalid[i], &backend));
		assert(!backend.identity_provided);
	}
}

static void test_compositor_owner_pid(void)
{
	char owner[32];

	assert(snprintf(owner, sizeof(owner), "%ld", (long)getpid()) > 0);
	assert(advk_layer_test_owner_matches(owner));
	assert(!advk_layer_test_owner_matches(NULL));
	assert(!advk_layer_test_owner_matches("0"));
	assert(!advk_layer_test_owner_matches("+1"));
	assert(!advk_layer_test_owner_matches("1-tail"));
	assert(!advk_layer_test_owner_matches("4294967296"));
}

int main(void)
{
	test_valid_dynamic_ack();
	test_invalid_ack_is_rejected();
	test_compositor_owner_pid();
	puts("Vulkan DRM identity layer ACK tests: PASS");
	return 0;
}
