#define _GNU_SOURCE
#include "android_vulkan_drm_identity_policy.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#define ADVK_ACK_VALUE \
	"turnip-qualcomm-card0-renderD128-kgsl3d0-226-0-226-128-466-0-v1"
#define ADVK_EXPORT __attribute__((visibility("default")))

struct advk_dispatch {
	void *loader_handle;
	PFN_vkGetInstanceProcAddr get_instance_proc_addr;
	PFN_vkEnumerateDeviceExtensionProperties enumerate_extensions;
	PFN_vkGetPhysicalDeviceProperties2 get_properties2;
	PFN_vkGetPhysicalDeviceProperties2KHR get_properties2_khr;
#ifdef ADVK_TESTING
	bool (*node_matches)(const char *, unsigned int, unsigned int, void *);
	void *node_userdata;
#endif
};

static pthread_mutex_t dispatch_lock = PTHREAD_MUTEX_INITIALIZER;
static struct advk_dispatch dispatch;

static void assign_symbol(void *symbol, void *function, size_t function_size)
{
	if (function_size == sizeof(symbol)) {
		memcpy(function, &symbol, function_size);
	}
}

static bool exact_node_matches(const char *path, unsigned int expected_major,
	                           unsigned int expected_minor, void *userdata)
{
	struct stat info;

	(void)userdata;
	return lstat(path, &info) == 0 && S_ISCHR(info.st_mode) &&
	       major(info.st_rdev) == expected_major &&
	       minor(info.st_rdev) == expected_minor;
}

static bool acknowledged(void)
{
	const char *enable = getenv("ANDROID_VULKAN_DRM_IDENTITY_ENABLE");
	const char *ack = getenv("ANDROID_VULKAN_DRM_IDENTITY_ACK");

	return enable != NULL && strcmp(enable, "1") == 0 && ack != NULL &&
	       strcmp(ack, ADVK_ACK_VALUE) == 0;
}

static void resolve_direct_symbols_locked(void)
{
	void *symbol;
	void *handle = RTLD_NEXT;

	/*
	 * volk-style applications, including Debian's vulkaninfo and wlroots,
	 * may load libvulkan after this LD_PRELOAD object and keep that handle
	 * local.  In that layout RTLD_NEXT cannot see the loader at all.  Open
	 * the canonical loader explicitly and resolve against that exact object;
	 * this is still the system Vulkan loader, never a guessed ICD entry
	 * point.  Retain RTLD_NEXT as the first choice for normally linked apps.
	 */
	if (dispatch.loader_handle == NULL) {
		dispatch.loader_handle = dlopen("libvulkan.so.1",
		                               RTLD_LAZY | RTLD_LOCAL);
	}

	if (dispatch.get_instance_proc_addr == NULL) {
		symbol = dlsym(RTLD_NEXT, "vkGetInstanceProcAddr");
		if (symbol == NULL && dispatch.loader_handle != NULL) {
			handle = dispatch.loader_handle;
			symbol = dlsym(handle, "vkGetInstanceProcAddr");
		}
		assign_symbol(symbol, &dispatch.get_instance_proc_addr,
		              sizeof(dispatch.get_instance_proc_addr));
	}
	if (dispatch.enumerate_extensions == NULL) {
		symbol = dlsym(RTLD_NEXT,
		               "vkEnumerateDeviceExtensionProperties");
		if (symbol == NULL && dispatch.loader_handle != NULL) {
			symbol = dlsym(dispatch.loader_handle,
			               "vkEnumerateDeviceExtensionProperties");
		}
		assign_symbol(symbol, &dispatch.enumerate_extensions,
		              sizeof(dispatch.enumerate_extensions));
	}
	if (dispatch.get_properties2 == NULL) {
		symbol = dlsym(RTLD_NEXT, "vkGetPhysicalDeviceProperties2");
		if (symbol == NULL && dispatch.loader_handle != NULL) {
			symbol = dlsym(dispatch.loader_handle,
			               "vkGetPhysicalDeviceProperties2");
		}
		assign_symbol(symbol, &dispatch.get_properties2,
		              sizeof(dispatch.get_properties2));
	}
	if (dispatch.get_properties2_khr == NULL) {
		symbol = dlsym(RTLD_NEXT, "vkGetPhysicalDeviceProperties2KHR");
		if (symbol == NULL && dispatch.loader_handle != NULL) {
			symbol = dlsym(dispatch.loader_handle,
			               "vkGetPhysicalDeviceProperties2KHR");
		}
		assign_symbol(symbol, &dispatch.get_properties2_khr,
		              sizeof(dispatch.get_properties2_khr));
	}
}

static struct advk_identity_backend backend_snapshot(bool prefer_khr)
{
	struct advk_identity_backend backend;

	memset(&backend, 0, sizeof(backend));
	pthread_mutex_lock(&dispatch_lock);
	resolve_direct_symbols_locked();
	backend.enumerate_extensions = dispatch.enumerate_extensions;
	if (prefer_khr && dispatch.get_properties2_khr != NULL) {
		backend.get_properties2 = dispatch.get_properties2_khr;
	} else if (dispatch.get_properties2 != NULL) {
		backend.get_properties2 = dispatch.get_properties2;
	} else {
		backend.get_properties2 = dispatch.get_properties2_khr;
	}
#ifdef ADVK_TESTING
	backend.node_matches = dispatch.node_matches != NULL ?
		dispatch.node_matches : exact_node_matches;
	backend.node_userdata = dispatch.node_userdata;
#else
	backend.node_matches = exact_node_matches;
#endif
	pthread_mutex_unlock(&dispatch_lock);
	backend.acknowledged = acknowledged();
	return backend;
}

ADVK_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physical_device,
	const char *layer_name, uint32_t *property_count,
	VkExtensionProperties *properties)
{
	struct advk_identity_backend backend = backend_snapshot(false);

	return advk_identity_enumerate(&backend, physical_device, layer_name,
	                               property_count, properties);
}

ADVK_EXPORT VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceProperties2(VkPhysicalDevice physical_device,
	VkPhysicalDeviceProperties2 *properties)
{
	struct advk_identity_backend backend = backend_snapshot(false);

	advk_identity_get_properties2(&backend, physical_device, properties);
}

ADVK_EXPORT VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice physical_device,
	VkPhysicalDeviceProperties2 *properties)
{
	struct advk_identity_backend backend = backend_snapshot(true);

	advk_identity_get_properties2(&backend, physical_device, properties);
}

ADVK_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
	PFN_vkGetInstanceProcAddr real_gipa;
	PFN_vkVoidFunction native;

	pthread_mutex_lock(&dispatch_lock);
	resolve_direct_symbols_locked();
	real_gipa = dispatch.get_instance_proc_addr;
	pthread_mutex_unlock(&dispatch_lock);
	if (real_gipa == NULL || name == NULL) {
		return NULL;
	}

	native = real_gipa(instance, name);
	if (native == NULL) {
		return NULL;
	}
	pthread_mutex_lock(&dispatch_lock);
	if (strcmp(name, "vkEnumerateDeviceExtensionProperties") == 0) {
		memcpy(&dispatch.enumerate_extensions, &native,
		       sizeof(dispatch.enumerate_extensions));
		native = (PFN_vkVoidFunction)vkEnumerateDeviceExtensionProperties;
	} else if (strcmp(name, "vkGetPhysicalDeviceProperties2") == 0) {
		memcpy(&dispatch.get_properties2, &native,
		       sizeof(dispatch.get_properties2));
		native = (PFN_vkVoidFunction)vkGetPhysicalDeviceProperties2;
	} else if (strcmp(name, "vkGetPhysicalDeviceProperties2KHR") == 0) {
		memcpy(&dispatch.get_properties2_khr, &native,
		       sizeof(dispatch.get_properties2_khr));
		native = (PFN_vkVoidFunction)vkGetPhysicalDeviceProperties2KHR;
	} else if (strcmp(name, "vkGetInstanceProcAddr") == 0) {
		native = (PFN_vkVoidFunction)vkGetInstanceProcAddr;
	}
	pthread_mutex_unlock(&dispatch_lock);
	return native;
}

#ifdef ADVK_TESTING
void advk_test_install_dispatch(
	PFN_vkGetInstanceProcAddr get_instance_proc_addr,
	PFN_vkEnumerateDeviceExtensionProperties enumerate_extensions,
	PFN_vkGetPhysicalDeviceProperties2 get_properties2,
	bool (*node_matches)(const char *, unsigned int, unsigned int, void *),
	void *node_userdata)
{
	pthread_mutex_lock(&dispatch_lock);
	memset(&dispatch, 0, sizeof(dispatch));
	dispatch.get_instance_proc_addr = get_instance_proc_addr;
	dispatch.enumerate_extensions = enumerate_extensions;
	dispatch.get_properties2 = get_properties2;
	dispatch.node_matches = node_matches;
	dispatch.node_userdata = node_userdata;
	pthread_mutex_unlock(&dispatch_lock);
}
#endif
