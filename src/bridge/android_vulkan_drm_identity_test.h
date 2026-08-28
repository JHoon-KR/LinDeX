#ifndef ANDROID_VULKAN_DRM_IDENTITY_TEST_H
#define ANDROID_VULKAN_DRM_IDENTITY_TEST_H

#include <stdbool.h>
#include <vulkan/vulkan_core.h>

#ifdef ADVK_TESTING
void advk_test_install_dispatch(
	PFN_vkGetInstanceProcAddr get_instance_proc_addr,
	PFN_vkEnumerateDeviceExtensionProperties enumerate_extensions,
	PFN_vkGetPhysicalDeviceProperties2 get_properties2,
	bool (*node_matches)(const char *, unsigned int, unsigned int, void *),
	void *node_userdata);
#endif

#endif
