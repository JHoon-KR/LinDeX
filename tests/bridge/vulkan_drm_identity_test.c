#include "android_vulkan_drm_identity_test.h"
#include "android_vulkan_drm_identity_policy.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ACK_VALUE \
	"turnip-qualcomm-card0-renderD128-kgsl3d0-226-0-226-128-466-0-v1"

struct fake_state {
	bool native_drm;
	bool missing_modifier;
	bool wrong_vendor;
	bool wrong_driver;
	bool nodes_match;
	unsigned int primary_major;
	unsigned int primary_minor;
	unsigned int render_major;
	unsigned int render_minor;
	unsigned int kgsl_major;
	unsigned int kgsl_minor;
	unsigned int native_properties_calls;
};

static struct fake_state state;
static VkPhysicalDevice fake_physical_device = (VkPhysicalDevice)(uintptr_t)7;

static const char *extension_at(uint32_t index)
{
	static const char *const required[] = {
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
		VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
		VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
	};

	if (index < 3) {
		return required[index];
	}
	return VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME;
}

static VKAPI_ATTR VkResult VKAPI_CALL fake_enumerate(
	VkPhysicalDevice physical_device, const char *layer_name,
	uint32_t *property_count, VkExtensionProperties *properties)
{
	uint32_t available = state.native_drm ? 4 : 3;
	uint32_t written;
	uint32_t i;

	assert(physical_device == fake_physical_device);
	if (layer_name != NULL) {
		if (property_count != NULL) {
			*property_count = 23;
		}
		return VK_ERROR_LAYER_NOT_PRESENT;
	}
	assert(property_count != NULL);
	if (properties == NULL) {
		*property_count = available;
		return VK_SUCCESS;
	}
	written = *property_count < available ? *property_count : available;
	for (i = 0; i < written; ++i) {
		memset(&properties[i], 0, sizeof(properties[i]));
		strncpy(properties[i].extensionName, extension_at(i),
		        sizeof(properties[i].extensionName) - 1);
		properties[i].specVersion = 99 + i;
	}
	if (state.missing_modifier && written > 2) {
		strncpy(properties[2].extensionName, "VK_FAKE_not_a_modifier",
		        sizeof(properties[2].extensionName) - 1);
	}
	*property_count = written;
	return written < available ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL fake_properties2(
	VkPhysicalDevice physical_device, VkPhysicalDeviceProperties2 *properties)
{
	VkBaseOutStructure *next;

	assert(physical_device == fake_physical_device);
	assert(properties != NULL);
	++state.native_properties_calls;
	properties->properties.vendorID = state.wrong_vendor ? UINT32_C(0x1002) :
		UINT32_C(0x5143);
	for (next = properties->pNext; next != NULL; next = next->pNext) {
		if (next->sType ==
		    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES) {
			VkPhysicalDeviceDriverProperties *driver =
				(VkPhysicalDeviceDriverProperties *)next;
			driver->driverID = state.wrong_driver ?
				VK_DRIVER_ID_MESA_RADV : VK_DRIVER_ID_MESA_TURNIP;
			strncpy(driver->driverName,
			        state.wrong_driver ? "radv" : "turnip Mesa driver",
			        sizeof(driver->driverName) - 1);
		} else if (next->sType ==
		           VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT &&
		           state.native_drm) {
			VkPhysicalDeviceDrmPropertiesEXT *drm =
				(VkPhysicalDeviceDrmPropertiesEXT *)next;
			drm->hasPrimary = VK_FALSE;
			drm->hasRender = VK_TRUE;
			drm->primaryMajor = 91;
			drm->primaryMinor = 92;
			drm->renderMajor = 93;
			drm->renderMinor = 94;
		}
	}
}

static bool fake_node_matches(const char *path, unsigned int device_major,
	                          unsigned int device_minor, void *userdata)
{
	struct fake_state *fake = userdata;

	if (!fake->nodes_match) {
		return false;
	}
	if (strcmp(path, "/dev/dri/card0") == 0) {
		return device_major == fake->primary_major &&
			device_minor == fake->primary_minor;
	}
	if (strcmp(path, "/dev/dri/renderD128") == 0) {
		return device_major == fake->render_major &&
			device_minor == fake->render_minor;
	}
	if (strcmp(path, "/dev/kgsl-3d0") == 0) {
		return device_major == fake->kgsl_major &&
			device_minor == fake->kgsl_minor;
	}
	return false;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_gipa(
	VkInstance instance, const char *name)
{
	(void)instance;
	if (strcmp(name, "vkEnumerateDeviceExtensionProperties") == 0) {
		return (PFN_vkVoidFunction)fake_enumerate;
	}
	if (strcmp(name, "vkGetPhysicalDeviceProperties2") == 0 ||
	    strcmp(name, "vkGetPhysicalDeviceProperties2KHR") == 0) {
		return (PFN_vkVoidFunction)fake_properties2;
	}
	if (strcmp(name, "vkGetInstanceProcAddr") == 0) {
		return (PFN_vkVoidFunction)fake_gipa;
	}
	return NULL;
}

static void reset_state(void)
{
	memset(&state, 0, sizeof(state));
	state.nodes_match = true;
	state.primary_major = 226;
	state.primary_minor = 0;
	state.render_major = 226;
	state.render_minor = 128;
	state.kgsl_major = 466;
	state.kgsl_minor = 0;
	assert(setenv("ANDROID_VULKAN_DRM_IDENTITY_ENABLE", "1", 1) == 0);
	assert(setenv("ANDROID_VULKAN_DRM_IDENTITY_ACK", ACK_VALUE, 1) == 0);
	advk_test_install_dispatch(fake_gipa, fake_enumerate, fake_properties2,
	                           fake_node_matches, &state);
}

static void test_runtime_device_identity(void)
{
	struct advk_identity_backend backend;
	VkPhysicalDeviceDrmPropertiesEXT drm = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
	};
	VkPhysicalDeviceProperties2 properties = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		.pNext = &drm,
	};
	uint32_t count = 0;

	reset_state();
	state.kgsl_major = 462;
	memset(&backend, 0, sizeof(backend));
	backend.enumerate_extensions = fake_enumerate;
	backend.get_properties2 = fake_properties2;
	backend.node_matches = fake_node_matches;
	backend.node_userdata = &state;
	backend.acknowledged = true;
	backend.identity_provided = true;
	backend.primary_major = 226;
	backend.primary_minor = 0;
	backend.render_major = 226;
	backend.render_minor = 128;
	backend.kgsl_major = 462;
	backend.kgsl_minor = 0;

	assert(advk_identity_enumerate(&backend, fake_physical_device, NULL,
	                               &count, NULL) == VK_SUCCESS);
	assert(count == 4);
	advk_identity_get_properties2(&backend, fake_physical_device, &properties);
	assert(drm.hasPrimary == VK_TRUE && drm.hasRender == VK_TRUE);
	assert(drm.primaryMajor == 226 && drm.primaryMinor == 0);
	assert(drm.renderMajor == 226 && drm.renderMinor == 128);

	backend.kgsl_major = 466;
	count = 0;
	assert(advk_identity_enumerate(&backend, fake_physical_device, NULL,
	                               &count, NULL) == VK_SUCCESS);
	assert(count == 3);
}

static PFN_vkEnumerateDeviceExtensionProperties intercepted_enumerator(void)
{
	PFN_vkVoidFunction generic;
	PFN_vkEnumerateDeviceExtensionProperties enumerate;

	generic = vkGetInstanceProcAddr((VkInstance)(uintptr_t)5,
	                                "vkEnumerateDeviceExtensionProperties");
	assert(generic != NULL);
	memcpy(&enumerate, &generic, sizeof(enumerate));
	assert(enumerate != fake_enumerate);
	return enumerate;
}

static PFN_vkGetPhysicalDeviceProperties2 intercepted_properties2(
	const char *name)
{
	PFN_vkVoidFunction generic;
	PFN_vkGetPhysicalDeviceProperties2 properties2;

	generic = vkGetInstanceProcAddr((VkInstance)(uintptr_t)5, name);
	assert(generic != NULL);
	memcpy(&properties2, &generic, sizeof(properties2));
	assert(properties2 != fake_properties2);
	return properties2;
}

static void test_exact_ack_and_extension_enumeration(void)
{
	PFN_vkEnumerateDeviceExtensionProperties enumerate;
	VkExtensionProperties properties[4];
	uint32_t count;
	VkResult result;

	reset_state();
	enumerate = intercepted_enumerator();
	count = 0;
	result = enumerate(fake_physical_device, NULL, &count, NULL);
	assert(result == VK_SUCCESS);
	assert(count == 4);

	memset(properties, 0, sizeof(properties));
	count = 4;
	result = enumerate(fake_physical_device, NULL, &count, properties);
	assert(result == VK_SUCCESS);
	assert(count == 4);
	assert(strcmp(properties[3].extensionName,
	              VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME) == 0);
	assert(properties[3].specVersion ==
	       VK_EXT_PHYSICAL_DEVICE_DRM_SPEC_VERSION);

	assert(setenv("ANDROID_VULKAN_DRM_IDENTITY_ACK", "wrong", 1) == 0);
	count = 0;
	assert(enumerate(fake_physical_device, NULL, &count, NULL) == VK_SUCCESS);
	assert(count == 3);
	assert(setenv("ANDROID_VULKAN_DRM_IDENTITY_ACK", ACK_VALUE, 1) == 0);
	assert(unsetenv("ANDROID_VULKAN_DRM_IDENTITY_ENABLE") == 0);
	count = 0;
	assert(enumerate(fake_physical_device, NULL, &count, NULL) == VK_SUCCESS);
	assert(count == 3);
}

static void test_fail_closed_gates_and_vulkan_semantics(void)
{
	PFN_vkEnumerateDeviceExtensionProperties enumerate;
	VkExtensionProperties properties[4];
	uint32_t count;

	reset_state();
	enumerate = intercepted_enumerator();
	state.missing_modifier = true;
	count = 0;
	assert(enumerate(fake_physical_device, NULL, &count, NULL) == VK_SUCCESS);
	assert(count == 3);
	state.missing_modifier = false;
	state.wrong_vendor = true;
	count = 0;
	assert(enumerate(fake_physical_device, NULL, &count, NULL) == VK_SUCCESS);
	assert(count == 3);
	state.wrong_vendor = false;
	state.wrong_driver = true;
	count = 0;
	assert(enumerate(fake_physical_device, NULL, &count, NULL) == VK_SUCCESS);
	assert(count == 3);
	state.wrong_driver = false;
	state.nodes_match = false;
	count = 0;
	assert(enumerate(fake_physical_device, NULL, &count, NULL) == VK_SUCCESS);
	assert(count == 3);
	state.nodes_match = true;

	count = 3;
	assert(enumerate(fake_physical_device, NULL, &count, properties) ==
	       VK_INCOMPLETE);
	assert(count == 2);
	count = 17;
	assert(enumerate(fake_physical_device, "fake-layer", &count,
	                 properties) == VK_ERROR_LAYER_NOT_PRESENT);
	assert(count == 23);
}

static void test_properties2_and_native_passthrough(void)
{
	PFN_vkEnumerateDeviceExtensionProperties enumerate;
	PFN_vkGetPhysicalDeviceProperties2 properties2;
	VkPhysicalDeviceDrmPropertiesEXT drm = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
	};
	VkPhysicalDeviceProperties2 properties = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		.pNext = &drm,
	};
	VkExtensionProperties extension_properties[4];
	uint32_t count;

	reset_state();
	properties2 = intercepted_properties2("vkGetPhysicalDeviceProperties2");
	properties2(fake_physical_device, &properties);
	assert(drm.hasPrimary == VK_TRUE && drm.hasRender == VK_TRUE);
	assert(drm.primaryMajor == 226 && drm.primaryMinor == 0);
	assert(drm.renderMajor == 226 && drm.renderMinor == 128);

	reset_state();
	state.native_drm = true;
	properties2 = intercepted_properties2("vkGetPhysicalDeviceProperties2KHR");
	properties2(fake_physical_device, &properties);
	assert(drm.hasPrimary == VK_FALSE && drm.hasRender == VK_TRUE);
	assert(drm.primaryMajor == 91 && drm.primaryMinor == 92);
	assert(drm.renderMajor == 93 && drm.renderMinor == 94);

	enumerate = intercepted_enumerator();
	count = 4;
	memset(extension_properties, 0, sizeof(extension_properties));
	assert(enumerate(fake_physical_device, NULL, &count,
	                 extension_properties) == VK_SUCCESS);
	assert(count == 4);
	assert(extension_properties[3].specVersion == 102);
	assert(strcmp(extension_properties[3].extensionName,
	              VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME) == 0);
}

int main(void)
{
	test_exact_ack_and_extension_enumeration();
	test_fail_closed_gates_and_vulkan_semantics();
	test_properties2_and_native_passthrough();
	test_runtime_device_identity();
	puts("Vulkan DRM identity bridge tests: PASS");
	return 0;
}
