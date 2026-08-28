#ifndef ANDROID_VULKAN_DRM_IDENTITY_POLICY_H
#define ANDROID_VULKAN_DRM_IDENTITY_POLICY_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

/*
 * Internal policy boundary for the Vulkan preload frontend.  Keeping the real
 * Vulkan calls and device-node lookup injectable lets host tests exercise the
 * exact production decisions without requiring a GPU or creating fake /dev
 * nodes.
 */
struct advk_identity_backend {
	PFN_vkEnumerateDeviceExtensionProperties enumerate_extensions;
	PFN_vkGetPhysicalDeviceProperties2 get_properties2;
	bool (*node_matches)(const char *path, unsigned int expected_major,
	                     unsigned int expected_minor, void *userdata);
	void *node_userdata;
	bool acknowledged;
	bool identity_provided;
	unsigned int primary_major;
	unsigned int primary_minor;
	unsigned int render_major;
	unsigned int render_minor;
	unsigned int kgsl_major;
	unsigned int kgsl_minor;
};

VkResult advk_identity_enumerate(
	const struct advk_identity_backend *backend,
	VkPhysicalDevice physical_device, const char *layer_name,
	uint32_t *property_count, VkExtensionProperties *properties);

void advk_identity_get_properties2(
	const struct advk_identity_backend *backend,
	VkPhysicalDevice physical_device, VkPhysicalDeviceProperties2 *properties);

#endif
