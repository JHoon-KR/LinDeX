#define _GNU_SOURCE
#include "android_vulkan_drm_identity_policy.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <vulkan/vk_layer.h>

#define ADVK_LAYER_NAME "VK_LAYER_LINDEX_android_drm_identity"
#define ADVK_ACK_PREFIX \
	"turnip-qualcomm-card0-renderD128-kgsl3d0-"
#define ADVK_EXPORT __attribute__((visibility("default")))
#ifndef MAX_INSTANCES
#define MAX_INSTANCES 16
#endif
#ifndef MAX_PHYSICAL_DEVICES
#define MAX_PHYSICAL_DEVICES 64
#endif
#ifndef MAX_DEVICES
#define MAX_DEVICES 64
#endif

struct instance_dispatch {
	bool used;
	VkInstance instance;
	PFN_vkGetInstanceProcAddr next_gipa;
	PFN_GetPhysicalDeviceProcAddr next_gpdpa;
	PFN_vkDestroyInstance destroy_instance;
	PFN_vkEnumeratePhysicalDevices enumerate_physical_devices;
	PFN_vkEnumeratePhysicalDeviceGroups enumerate_physical_device_groups;
	PFN_vkEnumeratePhysicalDeviceGroupsKHR enumerate_physical_device_groups_khr;
	PFN_vkEnumerateDeviceExtensionProperties enumerate_extensions;
	PFN_vkGetPhysicalDeviceProperties2 get_properties2;
	PFN_vkGetPhysicalDeviceProperties2KHR get_properties2_khr;
};

struct physical_dispatch {
	bool used;
	VkPhysicalDevice physical_device;
	VkInstance instance;
};

struct device_dispatch {
	bool used;
	VkDevice device;
	PFN_vkGetDeviceProcAddr next_gdpa;
	PFN_vkDestroyDevice destroy_device;
};

static pthread_mutex_t dispatch_lock = PTHREAD_MUTEX_INITIALIZER;
static struct instance_dispatch instances[MAX_INSTANCES];
static struct physical_dispatch physical_devices[MAX_PHYSICAL_DEVICES];
static struct device_dispatch devices[MAX_DEVICES];
static PFN_vkGetInstanceProcAddr bootstrap_gipa;
static PFN_GetPhysicalDeviceProcAddr bootstrap_gpdpa;
static PFN_vkGetDeviceProcAddr bootstrap_gdpa;

static void trace_event(const char *format, ...)
{
	va_list arguments;

	if (getenv("ANDROID_VULKAN_DRM_IDENTITY_TRACE") == NULL) return;
	va_start(arguments, format);
	fputs("LinDeX Vulkan layer: ", stderr);
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	fflush(stderr);
	va_end(arguments);
}

ADVK_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name);
ADVK_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *name);
ADVK_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
lindexGetPhysicalDeviceProcAddr(VkInstance instance, const char *name);
ADVK_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkCreateInstance(const VkInstanceCreateInfo *create_info,
	const VkAllocationCallbacks *allocator, VkInstance *instance);
ADVK_EXPORT VKAPI_ATTR void VKAPI_CALL
vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator);
ADVK_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *count,
	VkPhysicalDevice *physical_devices_out);
ADVK_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t *count,
	VkPhysicalDeviceGroupProperties *groups);
ADVK_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumeratePhysicalDeviceGroupsKHR(VkInstance instance, uint32_t *count,
	VkPhysicalDeviceGroupPropertiesKHR *groups);
ADVK_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physical_device,
	const char *layer_name, uint32_t *count, VkExtensionProperties *properties);
ADVK_EXPORT VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceProperties2(VkPhysicalDevice physical_device,
	VkPhysicalDeviceProperties2 *properties);
ADVK_EXPORT VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice physical_device,
	VkPhysicalDeviceProperties2 *properties);
ADVK_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDevice(VkPhysicalDevice physical_device,
	const VkDeviceCreateInfo *create_info,
	const VkAllocationCallbacks *allocator, VkDevice *device);
ADVK_EXPORT VKAPI_ATTR void VKAPI_CALL
vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator);

static bool parse_uint_component(const char **cursor, unsigned int *value)
{
	char *end;
	unsigned long parsed;

	if (cursor == NULL || *cursor == NULL || value == NULL ||
	    **cursor < '0' || **cursor > '9') {
		return false;
	}
	errno = 0;
	parsed = strtoul(*cursor, &end, 10);
	if (errno != 0 || end == *cursor || parsed > UINT_MAX || *end != '-') {
		return false;
	}
	*value = (unsigned int)parsed;
	*cursor = end + 1;
	return true;
}

static bool parse_ack_identity_value(const char *ack,
	struct advk_identity_backend *backend)
{
	const char *cursor;
	size_t prefix_size = sizeof(ADVK_ACK_PREFIX) - 1;

	if (backend == NULL || ack == NULL ||
	    strncmp(ack, ADVK_ACK_PREFIX, prefix_size) != 0) {
		return false;
	}
	cursor = ack + prefix_size;
	if (!parse_uint_component(&cursor, &backend->primary_major) ||
	    !parse_uint_component(&cursor, &backend->primary_minor) ||
	    !parse_uint_component(&cursor, &backend->render_major) ||
	    !parse_uint_component(&cursor, &backend->render_minor) ||
	    !parse_uint_component(&cursor, &backend->kgsl_major) ||
	    !parse_uint_component(&cursor, &backend->kgsl_minor) ||
	    strcmp(cursor, "v2") != 0) {
		return false;
	}
	backend->identity_provided = true;
	return true;
}

static bool owner_process_matches(const char *owner)
{
	char *end;
	unsigned long owner_pid;

	if (owner == NULL || owner[0] < '1' || owner[0] > '9') {
		return false;
	}
	errno = 0;
	owner_pid = strtoul(owner, &end, 10);
	return errno == 0 && end != owner && *end == '\0' &&
		owner_pid <= INT_MAX && (pid_t)owner_pid == getpid();
}

static bool load_ack_identity(struct advk_identity_backend *backend)
{
	const char *enable = getenv("ANDROID_VULKAN_DRM_IDENTITY_ENABLE");
	const char *ack = getenv("ANDROID_VULKAN_DRM_IDENTITY_ACK");
	const char *owner = getenv("ANDROID_VULKAN_DRM_IDENTITY_OWNER_PID");

	return enable != NULL && strcmp(enable, "1") == 0 &&
		owner_process_matches(owner) &&
		parse_ack_identity_value(ack, backend);
}

#ifdef ADVK_LAYER_TESTING
bool advk_layer_test_parse_ack(const char *ack,
	struct advk_identity_backend *backend)
{
	if (backend != NULL) {
		memset(backend, 0, sizeof(*backend));
	}
	return parse_ack_identity_value(ack, backend);
}

bool advk_layer_test_owner_matches(const char *owner)
{
	return owner_process_matches(owner);
}
#endif

static bool exact_node_matches(const char *path, unsigned int expected_major,
	unsigned int expected_minor, void *userdata)
{
	struct stat info;

	(void)userdata;
	return lstat(path, &info) == 0 && S_ISCHR(info.st_mode) &&
		major(info.st_rdev) == expected_major &&
		minor(info.st_rdev) == expected_minor;
}

static bool get_instance_dispatch(VkInstance instance,
	struct instance_dispatch *out)
{
	unsigned int i;
	bool found = false;

	pthread_mutex_lock(&dispatch_lock);
	for (i = 0; i < MAX_INSTANCES; ++i) {
		if (instances[i].used && instances[i].instance == instance) {
			*out = instances[i];
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&dispatch_lock);
	return found;
}

static bool get_physical_dispatch(VkPhysicalDevice physical_device,
	struct instance_dispatch *out)
{
	unsigned int i;
	VkInstance instance = VK_NULL_HANDLE;

	pthread_mutex_lock(&dispatch_lock);
	for (i = 0; i < MAX_PHYSICAL_DEVICES; ++i) {
		if (physical_devices[i].used &&
		    physical_devices[i].physical_device == physical_device) {
			instance = physical_devices[i].instance;
			break;
		}
	}
	if (instance != VK_NULL_HANDLE) {
		for (i = 0; i < MAX_INSTANCES; ++i) {
			if (instances[i].used && instances[i].instance == instance) {
				*out = instances[i];
				pthread_mutex_unlock(&dispatch_lock);
				return true;
			}
		}
	}
	pthread_mutex_unlock(&dispatch_lock);
	return false;
}

static bool remember_physical_devices(VkInstance instance, uint32_t count,
	const VkPhysicalDevice *physical_devices_in)
{
	uint32_t i;
	unsigned int slot;
	bool complete = true;

	if (physical_devices_in == NULL) {
		return true;
	}
	pthread_mutex_lock(&dispatch_lock);
	for (i = 0; i < count; ++i) {
		for (slot = 0; slot < MAX_PHYSICAL_DEVICES; ++slot) {
			if (physical_devices[slot].used &&
			    physical_devices[slot].physical_device ==
				    physical_devices_in[i]) {
				physical_devices[slot].instance = instance;
				break;
			}
		}
		if (slot < MAX_PHYSICAL_DEVICES) {
			continue;
		}
		for (slot = 0; slot < MAX_PHYSICAL_DEVICES; ++slot) {
			if (!physical_devices[slot].used) {
				physical_devices[slot].used = true;
				physical_devices[slot].physical_device =
					physical_devices_in[i];
				physical_devices[slot].instance = instance;
				break;
			}
		}
		if (slot == MAX_PHYSICAL_DEVICES) {
			complete = false;
		}
	}
	pthread_mutex_unlock(&dispatch_lock);
	return complete;
}

static VkLayerInstanceCreateInfo *instance_link_info(
	const VkInstanceCreateInfo *create_info)
{
	VkLayerInstanceCreateInfo *chain;

	for (chain = (VkLayerInstanceCreateInfo *)create_info->pNext;
	     chain != NULL;
	     chain = (VkLayerInstanceCreateInfo *)chain->pNext) {
		if (chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
		    chain->function == VK_LAYER_LINK_INFO) {
			return chain;
		}
	}
	return NULL;
}

static VkLayerDeviceCreateInfo *device_link_info(
	const VkDeviceCreateInfo *create_info)
{
	VkLayerDeviceCreateInfo *chain;

	for (chain = (VkLayerDeviceCreateInfo *)create_info->pNext;
	     chain != NULL;
	     chain = (VkLayerDeviceCreateInfo *)chain->pNext) {
		if (chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
		    chain->function == VK_LAYER_LINK_INFO) {
			return chain;
		}
	}
	return NULL;
}

static VkResult next_enumerate_extensions(VkPhysicalDevice physical_device,
	const char *layer_name, uint32_t *count, VkExtensionProperties *properties)
{
	struct instance_dispatch dispatch;

	if (!get_physical_dispatch(physical_device, &dispatch) ||
	    dispatch.enumerate_extensions == NULL) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	return dispatch.enumerate_extensions(physical_device, layer_name, count,
		properties);
}

static void next_get_properties2(VkPhysicalDevice physical_device,
	VkPhysicalDeviceProperties2 *properties)
{
	struct instance_dispatch dispatch;

	if (!get_physical_dispatch(physical_device, &dispatch)) {
		return;
	}
	if (dispatch.get_properties2 != NULL) {
		dispatch.get_properties2(physical_device, properties);
	} else if (dispatch.get_properties2_khr != NULL) {
		dispatch.get_properties2_khr(physical_device, properties);
	}
}

static struct advk_identity_backend identity_backend(void)
{
	struct advk_identity_backend backend = {
		.enumerate_extensions = next_enumerate_extensions,
		.get_properties2 = next_get_properties2,
		.node_matches = exact_node_matches,
	};
	backend.acknowledged = load_ack_identity(&backend);

	return backend;
}

ADVK_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkCreateInstance(const VkInstanceCreateInfo *create_info,
	const VkAllocationCallbacks *allocator, VkInstance *instance)
{
	VkLayerInstanceCreateInfo *chain;
	VkLayerInstanceLink *link;
	PFN_vkGetInstanceProcAddr next_gipa;
	PFN_vkCreateInstance next_create;
	struct instance_dispatch created = {0};
	VkResult result;
	unsigned int i;

	if (create_info == NULL || instance == NULL ||
	    (chain = instance_link_info(create_info)) == NULL ||
	    (link = chain->u.pLayerInfo) == NULL ||
	    link->pfnNextGetInstanceProcAddr == NULL) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	next_gipa = link->pfnNextGetInstanceProcAddr;
	next_create = (PFN_vkCreateInstance)next_gipa(VK_NULL_HANDLE,
		"vkCreateInstance");
	if (next_create == NULL) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	chain->u.pLayerInfo = link->pNext;
	trace_event("vkCreateInstance forwarding");
	result = next_create(create_info, allocator, instance);
	trace_event("vkCreateInstance returned result=%d", result);
	if (result != VK_SUCCESS) {
		return result;
	}

	created.used = true;
	created.instance = *instance;
	created.next_gipa = next_gipa;
	created.next_gpdpa = link->pfnNextGetPhysicalDeviceProcAddr;
	created.destroy_instance = (PFN_vkDestroyInstance)next_gipa(*instance,
		"vkDestroyInstance");
	created.enumerate_physical_devices =
		(PFN_vkEnumeratePhysicalDevices)next_gipa(*instance,
			"vkEnumeratePhysicalDevices");
	created.enumerate_physical_device_groups =
		(PFN_vkEnumeratePhysicalDeviceGroups)next_gipa(*instance,
			"vkEnumeratePhysicalDeviceGroups");
	created.enumerate_physical_device_groups_khr =
		(PFN_vkEnumeratePhysicalDeviceGroupsKHR)next_gipa(*instance,
			"vkEnumeratePhysicalDeviceGroupsKHR");
	created.enumerate_extensions =
		(PFN_vkEnumerateDeviceExtensionProperties)next_gipa(*instance,
			"vkEnumerateDeviceExtensionProperties");
	created.get_properties2 = (PFN_vkGetPhysicalDeviceProperties2)next_gipa(
		*instance, "vkGetPhysicalDeviceProperties2");
	created.get_properties2_khr =
		(PFN_vkGetPhysicalDeviceProperties2KHR)next_gipa(*instance,
			"vkGetPhysicalDeviceProperties2KHR");

	if (created.destroy_instance == NULL) {
		created.destroy_instance = (PFN_vkDestroyInstance)next_gipa(
			VK_NULL_HANDLE, "vkDestroyInstance");
	}
	if (created.destroy_instance == NULL ||
	    created.enumerate_physical_devices == NULL ||
	    created.enumerate_extensions == NULL ||
	    (created.get_properties2 == NULL &&
	     created.get_properties2_khr == NULL)) {
		if (created.destroy_instance != NULL) {
			created.destroy_instance(*instance, allocator);
		}
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	pthread_mutex_lock(&dispatch_lock);
	for (i = 0; i < MAX_INSTANCES; ++i) {
		if (!instances[i].used) {
			instances[i] = created;
			bootstrap_gipa = next_gipa;
			bootstrap_gpdpa = link->pfnNextGetPhysicalDeviceProcAddr;
			break;
		}
	}
	pthread_mutex_unlock(&dispatch_lock);
	if (i == MAX_INSTANCES) {
		created.destroy_instance(*instance, allocator);
		return VK_ERROR_TOO_MANY_OBJECTS;
	}
	return VK_SUCCESS;
}

ADVK_EXPORT VKAPI_ATTR void VKAPI_CALL
vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator)
{
	struct instance_dispatch dispatch;
	PFN_vkGetInstanceProcAddr next_gipa = NULL;
	PFN_vkDestroyInstance destroy = NULL;
	unsigned int i;

	if (!get_instance_dispatch(instance, &dispatch)) {
		pthread_mutex_lock(&dispatch_lock);
		next_gipa = bootstrap_gipa;
		pthread_mutex_unlock(&dispatch_lock);
		if (next_gipa != NULL) {
			destroy = (PFN_vkDestroyInstance)next_gipa(instance,
				"vkDestroyInstance");
		}
		if (destroy != NULL && destroy != vkDestroyInstance) {
			destroy(instance, allocator);
		}
		return;
	}
	pthread_mutex_lock(&dispatch_lock);
	for (i = 0; i < MAX_PHYSICAL_DEVICES; ++i) {
		if (physical_devices[i].used &&
		    physical_devices[i].instance == instance) {
			memset(&physical_devices[i], 0, sizeof(physical_devices[i]));
		}
	}
	for (i = 0; i < MAX_INSTANCES; ++i) {
		if (instances[i].used && instances[i].instance == instance) {
			memset(&instances[i], 0, sizeof(instances[i]));
			break;
		}
	}
	pthread_mutex_unlock(&dispatch_lock);
	dispatch.destroy_instance(instance, allocator);
}

ADVK_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *count,
	VkPhysicalDevice *physical_devices_out)
{
	struct instance_dispatch dispatch;
	VkResult result;
	uint32_t returned;

	if (!get_instance_dispatch(instance, &dispatch) || count == NULL) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	returned = physical_devices_out != NULL ? *count : 0;
	result = dispatch.enumerate_physical_devices(instance, count,
		physical_devices_out);
	if ((result == VK_SUCCESS || result == VK_INCOMPLETE) &&
	    physical_devices_out != NULL) {
		if (returned > *count) {
			returned = *count;
		}
		if (!remember_physical_devices(instance, returned,
		                              physical_devices_out)) {
			return VK_ERROR_TOO_MANY_OBJECTS;
		}
		trace_event("remembered %u physical device(s)", returned);
	}
	return result;
}

static VkResult enumerate_physical_device_groups(VkInstance instance,
	uint32_t *count, VkPhysicalDeviceGroupProperties *groups, bool prefer_khr)
{
	struct instance_dispatch dispatch;
	PFN_vkEnumeratePhysicalDeviceGroups enumerate = NULL;
	VkResult result;
	uint32_t capacity;
	uint32_t returned;
	uint32_t i;

	if (!get_instance_dispatch(instance, &dispatch) || count == NULL) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	if (prefer_khr && dispatch.enumerate_physical_device_groups_khr != NULL) {
		enumerate = (PFN_vkEnumeratePhysicalDeviceGroups)
			dispatch.enumerate_physical_device_groups_khr;
	} else if (dispatch.enumerate_physical_device_groups != NULL) {
		enumerate = dispatch.enumerate_physical_device_groups;
	} else if (dispatch.enumerate_physical_device_groups_khr != NULL) {
		enumerate = (PFN_vkEnumeratePhysicalDeviceGroups)
			dispatch.enumerate_physical_device_groups_khr;
	}
	if (enumerate == NULL) {
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	}
	capacity = groups != NULL ? *count : 0;
	result = enumerate(instance, count, groups);
	if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && groups != NULL) {
		returned = capacity < *count ? capacity : *count;
		for (i = 0; i < returned; ++i) {
			if (!remember_physical_devices(instance,
			        groups[i].physicalDeviceCount,
			        groups[i].physicalDevices)) {
				return VK_ERROR_TOO_MANY_OBJECTS;
			}
		}
		trace_event("remembered %u physical device group(s)", returned);
	}
	return result;
}

ADVK_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t *count,
	VkPhysicalDeviceGroupProperties *groups)
{
	return enumerate_physical_device_groups(instance, count, groups, false);
}

ADVK_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumeratePhysicalDeviceGroupsKHR(VkInstance instance, uint32_t *count,
	VkPhysicalDeviceGroupPropertiesKHR *groups)
{
	return enumerate_physical_device_groups(instance, count,
		(VkPhysicalDeviceGroupProperties *)groups, true);
}

ADVK_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physical_device,
	const char *layer_name, uint32_t *count, VkExtensionProperties *properties)
{
	struct advk_identity_backend backend = identity_backend();
	VkResult result = advk_identity_enumerate(&backend, physical_device, layer_name,
		count, properties);
	trace_event("vkEnumerateDeviceExtensionProperties result=%d count=%u",
		result, count != NULL ? *count : 0);
	return result;
}

ADVK_EXPORT VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceProperties2(VkPhysicalDevice physical_device,
	VkPhysicalDeviceProperties2 *properties)
{
	struct advk_identity_backend backend = identity_backend();

	advk_identity_get_properties2(&backend, physical_device, properties);
}

ADVK_EXPORT VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice physical_device,
	VkPhysicalDeviceProperties2 *properties)
{
	vkGetPhysicalDeviceProperties2(physical_device, properties);
}

ADVK_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDevice(VkPhysicalDevice physical_device,
	const VkDeviceCreateInfo *create_info,
	const VkAllocationCallbacks *allocator, VkDevice *device)
{
	struct instance_dispatch instance_dispatch;
	VkLayerDeviceCreateInfo *chain;
	VkLayerDeviceLink *link;
	PFN_vkCreateDevice next_create;
	PFN_vkGetDeviceProcAddr next_gdpa;
	PFN_vkDestroyDevice destroy_device;
	struct device_dispatch created = {0};
	VkResult result;
	unsigned int i;

	if (!get_physical_dispatch(physical_device, &instance_dispatch) ||
	    create_info == NULL || device == NULL ||
	    (chain = device_link_info(create_info)) == NULL ||
	    (link = chain->u.pLayerInfo) == NULL ||
	    link->pfnNextGetInstanceProcAddr == NULL ||
	    link->pfnNextGetDeviceProcAddr == NULL) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	trace_event("vkCreateDevice entered");
	next_create = (PFN_vkCreateDevice)link->pfnNextGetInstanceProcAddr(
		instance_dispatch.instance, "vkCreateDevice");
	next_gdpa = link->pfnNextGetDeviceProcAddr;
	if (next_create == NULL) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	chain->u.pLayerInfo = link->pNext;
	trace_event("vkCreateDevice forwarding");
	result = next_create(physical_device, create_info, allocator, device);
	trace_event("vkCreateDevice returned result=%d", result);
	if (result != VK_SUCCESS) {
		return result;
	}
	destroy_device = (PFN_vkDestroyDevice)next_gdpa(*device,
		"vkDestroyDevice");
	if (destroy_device == NULL) {
		destroy_device = (PFN_vkDestroyDevice)
			instance_dispatch.next_gipa(instance_dispatch.instance,
				"vkDestroyDevice");
	}
	if (destroy_device == NULL) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	created.used = true;
	created.device = *device;
	created.next_gdpa = next_gdpa;
	created.destroy_device = destroy_device;
	pthread_mutex_lock(&dispatch_lock);
	for (i = 0; i < MAX_DEVICES; ++i) {
		if (!devices[i].used) {
			devices[i] = created;
			bootstrap_gdpa = next_gdpa;
			break;
		}
	}
	pthread_mutex_unlock(&dispatch_lock);
	trace_event("vkCreateDevice mapped slot=%u", i);
	if (i == MAX_DEVICES) {
		destroy_device(*device, allocator);
		return VK_ERROR_TOO_MANY_OBJECTS;
	}
	return VK_SUCCESS;
}

ADVK_EXPORT VKAPI_ATTR void VKAPI_CALL
vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator)
{
	PFN_vkDestroyDevice destroy = NULL;
	PFN_vkGetDeviceProcAddr next = NULL;
	unsigned int i;

	pthread_mutex_lock(&dispatch_lock);
	for (i = 0; i < MAX_DEVICES; ++i) {
		if (devices[i].used && devices[i].device == device) {
			destroy = devices[i].destroy_device;
			memset(&devices[i], 0, sizeof(devices[i]));
			break;
		}
	}
	pthread_mutex_unlock(&dispatch_lock);
	if (destroy == NULL) {
		pthread_mutex_lock(&dispatch_lock);
		next = bootstrap_gdpa;
		pthread_mutex_unlock(&dispatch_lock);
		if (next != NULL) {
			destroy = (PFN_vkDestroyDevice)next(device,
				"vkDestroyDevice");
		}
	}
	if (destroy != NULL) {
		if (destroy != vkDestroyDevice) {
			destroy(device, allocator);
		}
	}
}

static PFN_vkVoidFunction intercept(const char *name)
{
	if (name == NULL) return NULL;
	if (strcmp(name, "vkGetInstanceProcAddr") == 0)
		return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
	if (strcmp(name, "vkGetDeviceProcAddr") == 0)
		return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
	if (strcmp(name, "vkCreateInstance") == 0)
		return (PFN_vkVoidFunction)vkCreateInstance;
	if (strcmp(name, "vkDestroyInstance") == 0)
		return (PFN_vkVoidFunction)vkDestroyInstance;
	if (strcmp(name, "vkEnumeratePhysicalDevices") == 0)
		return (PFN_vkVoidFunction)vkEnumeratePhysicalDevices;
	if (strcmp(name, "vkEnumeratePhysicalDeviceGroups") == 0)
		return (PFN_vkVoidFunction)vkEnumeratePhysicalDeviceGroups;
	if (strcmp(name, "vkEnumeratePhysicalDeviceGroupsKHR") == 0)
		return (PFN_vkVoidFunction)vkEnumeratePhysicalDeviceGroupsKHR;
	if (strcmp(name, "vkEnumerateDeviceExtensionProperties") == 0)
		return (PFN_vkVoidFunction)vkEnumerateDeviceExtensionProperties;
	if (strcmp(name, "vkGetPhysicalDeviceProperties2") == 0)
		return (PFN_vkVoidFunction)vkGetPhysicalDeviceProperties2;
	if (strcmp(name, "vkGetPhysicalDeviceProperties2KHR") == 0)
		return (PFN_vkVoidFunction)vkGetPhysicalDeviceProperties2KHR;
	if (strcmp(name, "vkCreateDevice") == 0)
		return (PFN_vkVoidFunction)vkCreateDevice;
	if (strcmp(name, "vkDestroyDevice") == 0)
		return (PFN_vkVoidFunction)vkDestroyDevice;
	return NULL;
}

ADVK_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
	struct instance_dispatch dispatch;
	PFN_vkVoidFunction function = intercept(name);

	if (function != NULL) return function;
	if (get_instance_dispatch(instance, &dispatch) &&
	    dispatch.next_gipa != NULL) {
		return dispatch.next_gipa(instance, name);
	}
	return bootstrap_gipa != NULL ? bootstrap_gipa(instance, name) : NULL;
}

ADVK_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
lindexGetPhysicalDeviceProcAddr(VkInstance instance, const char *name)
{
	struct instance_dispatch dispatch;
	PFN_vkVoidFunction function = intercept(name);

	if (function != NULL) return function;
	if (get_instance_dispatch(instance, &dispatch) &&
	    dispatch.next_gpdpa != NULL) {
		return dispatch.next_gpdpa(instance, name);
	}
	return bootstrap_gpdpa != NULL ? bootstrap_gpdpa(instance, name) : NULL;
}

ADVK_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *name)
{
	PFN_vkGetDeviceProcAddr next = NULL;
	unsigned int i;

	if (name != NULL && strcmp(name, "vkGetDeviceProcAddr") == 0)
		return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
	if (name != NULL && strcmp(name, "vkDestroyDevice") == 0)
		return (PFN_vkVoidFunction)vkDestroyDevice;
	pthread_mutex_lock(&dispatch_lock);
	for (i = 0; i < MAX_DEVICES; ++i) {
		if (devices[i].used && devices[i].device == device) {
			next = devices[i].next_gdpa;
			break;
		}
	}
	if (next == NULL) next = bootstrap_gdpa;
	pthread_mutex_unlock(&dispatch_lock);
	return next != NULL ? next(device, name) : NULL;
}

ADVK_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceLayerProperties(uint32_t *count, VkLayerProperties *properties)
{
	VkLayerProperties layer = {0};

	if (count == NULL) return VK_ERROR_INITIALIZATION_FAILED;
	if (properties == NULL) {
		*count = 1;
		return VK_SUCCESS;
	}
	if (*count == 0) return VK_INCOMPLETE;
	strncpy(layer.layerName, ADVK_LAYER_NAME, sizeof(layer.layerName) - 1);
	layer.specVersion = VK_API_VERSION_1_1;
	layer.implementationVersion = 1;
	strncpy(layer.description, "LinDeX exact-device Android DRM identity",
		sizeof(layer.description) - 1);
	properties[0] = layer;
	*count = 1;
	return VK_SUCCESS;
}

ADVK_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceExtensionProperties(const char *layer_name, uint32_t *count,
	VkExtensionProperties *properties)
{
	(void)properties;
	if (count == NULL) return VK_ERROR_INITIALIZATION_FAILED;
	if (layer_name != NULL && strcmp(layer_name, ADVK_LAYER_NAME) != 0)
		return VK_ERROR_LAYER_NOT_PRESENT;
	*count = 0;
	return VK_SUCCESS;
}

ADVK_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *version)
{
	if (version == NULL || version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
		return VK_ERROR_INITIALIZATION_FAILED;
	if (version->loaderLayerInterfaceVersion > 2)
		version->loaderLayerInterfaceVersion = 2;
	version->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
	version->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
	version->pfnGetPhysicalDeviceProcAddr = lindexGetPhysicalDeviceProcAddr;
	return VK_SUCCESS;
}
