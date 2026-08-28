#include "android_vulkan_drm_identity_policy.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define ADVK_QUALCOMM_VENDOR_ID UINT32_C(0x5143)
#define ADVK_MAX_DEVICE_EXTENSIONS UINT32_C(4096)

enum advk_device_status {
	ADVK_DEVICE_REJECTED,
	ADVK_DEVICE_NATIVE_DRM,
	ADVK_DEVICE_SYNTHETIC_DRM,
};

struct advk_node_identity {
	unsigned int primary_major;
	unsigned int primary_minor;
	unsigned int render_major;
	unsigned int render_minor;
	unsigned int kgsl_major;
	unsigned int kgsl_minor;
};

static struct advk_node_identity node_identity(
	const struct advk_identity_backend *backend)
{
	/* Preserve the legacy preload contract while the Vulkan layer uses v2. */
	if (backend->identity_provided) {
		return (struct advk_node_identity) {
			.primary_major = backend->primary_major,
			.primary_minor = backend->primary_minor,
			.render_major = backend->render_major,
			.render_minor = backend->render_minor,
			.kgsl_major = backend->kgsl_major,
			.kgsl_minor = backend->kgsl_minor,
		};
	}
	return (struct advk_node_identity) {
		.primary_major = 226,
		.primary_minor = 0,
		.render_major = 226,
		.render_minor = 128,
		.kgsl_major = 466,
		.kgsl_minor = 0,
	};
}

static bool name_is(const VkExtensionProperties *property, const char *name)
{
	return strncmp(property->extensionName, name,
	               VK_MAX_EXTENSION_NAME_SIZE) == 0;
}

static bool ascii_contains_case(const char *value, size_t value_size,
	                             const char *needle)
{
	size_t needle_size = strlen(needle);
	size_t i;
	size_t j;

	if (needle_size == 0 || needle_size > value_size) {
		return false;
	}
	for (i = 0; i + needle_size <= value_size && value[i] != '\0'; ++i) {
		for (j = 0; j < needle_size; ++j) {
			unsigned char left = (unsigned char)value[i + j];
			unsigned char right = (unsigned char)needle[j];

			if (left == '\0' || tolower(left) != tolower(right)) {
				break;
			}
		}
		if (j == needle_size) {
			return true;
		}
	}
	return false;
}

static enum advk_device_status classify_device(
	const struct advk_identity_backend *backend,
	VkPhysicalDevice physical_device)
{
	VkPhysicalDeviceDriverProperties driver = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
	};
	VkPhysicalDeviceProperties2 device = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		.pNext = &driver,
	};
	VkExtensionProperties *extensions = NULL;
	uint32_t extension_count = 0;
	bool has_external_fd = false;
	bool has_dma_buf = false;
	bool has_modifier = false;
	bool has_native_drm = false;
	VkResult result;
	uint32_t i;
	struct advk_node_identity identity;

	if (backend == NULL || !backend->acknowledged ||
	    backend->enumerate_extensions == NULL ||
	    backend->get_properties2 == NULL || backend->node_matches == NULL) {
		return ADVK_DEVICE_REJECTED;
	}

	result = backend->enumerate_extensions(physical_device, NULL,
	                                      &extension_count, NULL);
	if (result != VK_SUCCESS || extension_count == 0 ||
	    extension_count > ADVK_MAX_DEVICE_EXTENSIONS) {
		return ADVK_DEVICE_REJECTED;
	}
	extensions = calloc(extension_count, sizeof(*extensions));
	if (extensions == NULL) {
		return ADVK_DEVICE_REJECTED;
	}
	result = backend->enumerate_extensions(physical_device, NULL,
	                                      &extension_count, extensions);
	if (result != VK_SUCCESS) {
		free(extensions);
		return ADVK_DEVICE_REJECTED;
	}
	for (i = 0; i < extension_count; ++i) {
		has_external_fd |= name_is(&extensions[i],
		                           VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
		has_dma_buf |= name_is(&extensions[i],
		                       VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
		has_modifier |= name_is(&extensions[i],
		                       VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
		has_native_drm |= name_is(&extensions[i],
		                         VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
	}
	free(extensions);

	/* A driver-provided implementation always wins, byte for byte. */
	if (has_native_drm) {
		return ADVK_DEVICE_NATIVE_DRM;
	}
	if (!has_external_fd || !has_dma_buf || !has_modifier) {
		return ADVK_DEVICE_REJECTED;
	}

	backend->get_properties2(physical_device, &device);
	if (device.properties.vendorID != ADVK_QUALCOMM_VENDOR_ID ||
	    driver.driverID != VK_DRIVER_ID_MESA_TURNIP ||
	    !ascii_contains_case(driver.driverName, sizeof(driver.driverName),
	                         "turnip")) {
		return ADVK_DEVICE_REJECTED;
	}

	/*
	 * These are the measured reference-device identities, not defaults.  All
	 * three paths must be real character devices with the exact rdev tuple.
	 * Never derive or guess an arbitrary DRM node from directory contents.
	 */
	identity = node_identity(backend);
	if (!backend->node_matches("/dev/dri/card0",
	                           identity.primary_major,
	                           identity.primary_minor,
	                           backend->node_userdata) ||
	    !backend->node_matches("/dev/dri/renderD128",
	                           identity.render_major,
	                           identity.render_minor,
	                           backend->node_userdata) ||
	    !backend->node_matches("/dev/kgsl-3d0",
	                           identity.kgsl_major,
	                           identity.kgsl_minor,
	                           backend->node_userdata)) {
		return ADVK_DEVICE_REJECTED;
	}

	return ADVK_DEVICE_SYNTHETIC_DRM;
}

VkResult advk_identity_enumerate(
	const struct advk_identity_backend *backend,
	VkPhysicalDevice physical_device, const char *layer_name,
	uint32_t *property_count, VkExtensionProperties *properties)
{
	enum advk_device_status status;
	VkResult result;
	uint32_t capacity;

	if (backend == NULL || backend->enumerate_extensions == NULL) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	if (!backend->acknowledged || layer_name != NULL || property_count == NULL) {
		return backend->enumerate_extensions(physical_device, layer_name,
		                                     property_count, properties);
	}
	status = classify_device(backend, physical_device);
	if (status != ADVK_DEVICE_SYNTHETIC_DRM) {
		return backend->enumerate_extensions(physical_device, layer_name,
		                                     property_count, properties);
	}

	if (properties == NULL) {
		result = backend->enumerate_extensions(physical_device, NULL,
		                                      property_count, NULL);
		if (result == VK_SUCCESS && *property_count < UINT32_MAX) {
			++*property_count;
		}
		return result;
	}

	capacity = *property_count;
	if (capacity == 0) {
		return backend->enumerate_extensions(physical_device, NULL,
		                                     property_count, properties);
	}
	*property_count = capacity - 1;
	result = backend->enumerate_extensions(physical_device, NULL,
	                                      property_count, properties);
	if (result != VK_SUCCESS) {
		return result;
	}
	if (*property_count >= capacity) {
		return VK_INCOMPLETE;
	}

	memset(&properties[*property_count], 0, sizeof(properties[*property_count]));
	memcpy(properties[*property_count].extensionName,
	       VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME,
	       sizeof(VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME));
	properties[*property_count].specVersion =
		VK_EXT_PHYSICAL_DEVICE_DRM_SPEC_VERSION;
	++*property_count;
	return VK_SUCCESS;
}

void advk_identity_get_properties2(
	const struct advk_identity_backend *backend,
	VkPhysicalDevice physical_device, VkPhysicalDeviceProperties2 *properties)
{
	VkBaseOutStructure *next;
	unsigned int depth;
	struct advk_node_identity identity;

	if (backend == NULL || backend->get_properties2 == NULL ||
	    properties == NULL) {
		return;
	}
	backend->get_properties2(physical_device, properties);
	if (!backend->acknowledged ||
	    classify_device(backend, physical_device) !=
		ADVK_DEVICE_SYNTHETIC_DRM) {
		return;
	}
	identity = node_identity(backend);

	next = properties->pNext;
	for (depth = 0; next != NULL && depth < 64; ++depth) {
		if (next->sType ==
		    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT) {
			VkPhysicalDeviceDrmPropertiesEXT *drm =
				(VkPhysicalDeviceDrmPropertiesEXT *)next;

			drm->hasPrimary = VK_TRUE;
			drm->hasRender = VK_TRUE;
			drm->primaryMajor = identity.primary_major;
			drm->primaryMinor = identity.primary_minor;
			drm->renderMajor = identity.render_major;
			drm->renderMinor = identity.render_minor;
			return;
		}
		next = next->pNext;
	}
}
