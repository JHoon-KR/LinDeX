#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vulkan/vk_layer.h>

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
	const VkInstanceCreateInfo *create_info,
	const VkAllocationCallbacks *allocator, VkInstance *instance);
VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(
	VkInstance instance, const VkAllocationCallbacks *allocator);
VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroups(
	VkInstance instance, uint32_t *count,
	VkPhysicalDeviceGroupProperties *groups);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(
	VkPhysicalDevice physical_device, const VkDeviceCreateInfo *create_info,
	const VkAllocationCallbacks *allocator, VkDevice *device);
VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(
	VkDevice device, const VkAllocationCallbacks *allocator);

static unsigned int instance_create_count;
static unsigned int instance_destroy_count;
static unsigned int device_create_count;
static unsigned int device_destroy_count;
static VkPhysicalDevice fake_physical_device =
	(VkPhysicalDevice)(uintptr_t)0x3001;

static VKAPI_ATTR VkResult VKAPI_CALL fake_create_instance(
	const VkInstanceCreateInfo *create_info,
	const VkAllocationCallbacks *allocator, VkInstance *instance)
{
	(void)create_info;
	(void)allocator;
	++instance_create_count;
	*instance = (VkInstance)(uintptr_t)(0x1000 + instance_create_count);
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL fake_destroy_instance(
	VkInstance instance, const VkAllocationCallbacks *allocator)
{
	(void)instance;
	(void)allocator;
	++instance_destroy_count;
}

static VKAPI_ATTR VkResult VKAPI_CALL fake_enumerate_physical_devices(
	VkInstance instance, uint32_t *count, VkPhysicalDevice *physical_devices)
{
	(void)instance;
	if (physical_devices == NULL) {
		*count = 1;
		return VK_SUCCESS;
	}
	if (*count == 0) return VK_INCOMPLETE;
	physical_devices[0] = fake_physical_device;
	*count = 1;
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL fake_enumerate_physical_device_groups(
	VkInstance instance, uint32_t *count,
	VkPhysicalDeviceGroupProperties *groups)
{
	(void)instance;
	if (groups == NULL) {
		*count = 1;
		return VK_SUCCESS;
	}
	if (*count == 0) return VK_INCOMPLETE;
	memset(&groups[0], 0, sizeof(groups[0]));
	groups[0].sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES;
	groups[0].physicalDeviceCount = 1;
	groups[0].physicalDevices[0] = fake_physical_device;
	*count = 1;
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL fake_enumerate_extensions(
	VkPhysicalDevice physical_device, const char *layer_name, uint32_t *count,
	VkExtensionProperties *properties)
{
	(void)physical_device;
	(void)layer_name;
	(void)properties;
	*count = 0;
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL fake_get_properties2(
	VkPhysicalDevice physical_device, VkPhysicalDeviceProperties2 *properties)
{
	(void)physical_device;
	(void)properties;
}

static VKAPI_ATTR VkResult VKAPI_CALL fake_create_device(
	VkPhysicalDevice physical_device, const VkDeviceCreateInfo *create_info,
	const VkAllocationCallbacks *allocator, VkDevice *device)
{
	(void)physical_device;
	(void)create_info;
	(void)allocator;
	++device_create_count;
	*device = (VkDevice)(uintptr_t)(0x4000 + device_create_count);
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL fake_destroy_device(
	VkDevice device, const VkAllocationCallbacks *allocator)
{
	(void)device;
	(void)allocator;
	++device_destroy_count;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_gipa(
	VkInstance instance, const char *name)
{
	(void)instance;
	if (strcmp(name, "vkCreateInstance") == 0)
		return (PFN_vkVoidFunction)fake_create_instance;
	if (strcmp(name, "vkDestroyInstance") == 0)
		return (PFN_vkVoidFunction)fake_destroy_instance;
	if (strcmp(name, "vkEnumeratePhysicalDevices") == 0)
		return (PFN_vkVoidFunction)fake_enumerate_physical_devices;
	if (strcmp(name, "vkEnumeratePhysicalDeviceGroups") == 0 ||
	    strcmp(name, "vkEnumeratePhysicalDeviceGroupsKHR") == 0)
		return (PFN_vkVoidFunction)fake_enumerate_physical_device_groups;
	if (strcmp(name, "vkEnumerateDeviceExtensionProperties") == 0)
		return (PFN_vkVoidFunction)fake_enumerate_extensions;
	if (strcmp(name, "vkGetPhysicalDeviceProperties2") == 0 ||
	    strcmp(name, "vkGetPhysicalDeviceProperties2KHR") == 0)
		return (PFN_vkVoidFunction)fake_get_properties2;
	if (strcmp(name, "vkCreateDevice") == 0)
		return (PFN_vkVoidFunction)fake_create_device;
	if (strcmp(name, "vkDestroyDevice") == 0)
		return (PFN_vkVoidFunction)fake_destroy_device;
	return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_gdpa(
	VkDevice device, const char *name)
{
	(void)device;
	if (strcmp(name, "vkDestroyDevice") == 0)
		return (PFN_vkVoidFunction)fake_destroy_device;
	return NULL;
}

static VkResult create_layer_instance(VkInstance *instance)
{
	VkLayerInstanceLink link = {
		.pfnNextGetInstanceProcAddr = fake_gipa,
		.pfnNextGetPhysicalDeviceProcAddr = NULL,
		.pNext = NULL,
	};
	VkLayerInstanceCreateInfo layer_info = {
		.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO,
		.function = VK_LAYER_LINK_INFO,
		.u.pLayerInfo = &link,
	};
	VkInstanceCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pNext = &layer_info,
	};

	return vkCreateInstance(&create_info, NULL, instance);
}

static VkResult create_layer_device(VkDevice *device)
{
	VkLayerDeviceLink link = {
		.pfnNextGetInstanceProcAddr = fake_gipa,
		.pfnNextGetDeviceProcAddr = fake_gdpa,
		.pNext = NULL,
	};
	VkLayerDeviceCreateInfo layer_info = {
		.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
		.function = VK_LAYER_LINK_INFO,
		.u.pLayerInfo = &link,
	};
	VkDeviceCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &layer_info,
	};

	return vkCreateDevice(fake_physical_device, &create_info, NULL, device);
}

int main(void)
{
	VkInstance first_instance;
	VkInstance rejected_instance;
	VkInstance replacement_instance;
	VkDevice first_device;
	VkDevice rejected_device;
	VkDevice replacement_device;
	VkPhysicalDeviceGroupProperties group = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES,
	};
	uint32_t count = 1;

	assert(create_layer_instance(&first_instance) == VK_SUCCESS);
	assert(create_layer_instance(&rejected_instance) ==
	       VK_ERROR_TOO_MANY_OBJECTS);
	assert(instance_create_count == 2);
	assert(instance_destroy_count == 1);

	assert(vkEnumeratePhysicalDeviceGroups(first_instance, &count, &group) ==
	       VK_SUCCESS);
	assert(count == 1 && group.physicalDeviceCount == 1);
	assert(group.physicalDevices[0] == fake_physical_device);

	assert(create_layer_device(&first_device) == VK_SUCCESS);
	assert(create_layer_device(&rejected_device) == VK_ERROR_TOO_MANY_OBJECTS);
	assert(device_create_count == 2);
	assert(device_destroy_count == 1);

	vkDestroyDevice(first_device, NULL);
	assert(device_destroy_count == 2);
	assert(create_layer_device(&replacement_device) == VK_SUCCESS);
	vkDestroyDevice(replacement_device, NULL);
	assert(device_destroy_count == 3);

	vkDestroyInstance(first_instance, NULL);
	assert(instance_destroy_count == 2);
	assert(create_layer_instance(&replacement_instance) == VK_SUCCESS);
	vkDestroyInstance(replacement_instance, NULL);
	assert(instance_destroy_count == 3);

	puts("Vulkan DRM identity layer lifecycle tests: PASS");
	return 0;
}
