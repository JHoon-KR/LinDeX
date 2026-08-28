// SPDX-License-Identifier: MIT
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

struct format_name {
    VkFormat format;
    const char *name;
};

static const struct format_name formats[] = {
    {VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM/XR24"},
    {VK_FORMAT_B8G8R8A8_SRGB, "B8G8R8A8_SRGB"},
    {VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM/AB24"},
    {VK_FORMAT_A2R10G10B10_UNORM_PACK32, "A2R10G10B10_UNORM"},
};

static void probe_format(VkPhysicalDevice device, const struct format_name *item)
{
    VkDrmFormatModifierPropertiesListEXT list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };
    VkFormatProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &list,
    };
    vkGetPhysicalDeviceFormatProperties2(device, item->format, &properties);
    printf("format=%s modifier_count=%u linear_features=0x%08x optimal_features=0x%08x\n",
           item->name, list.drmFormatModifierCount,
           properties.formatProperties.linearTilingFeatures,
           properties.formatProperties.optimalTilingFeatures);
    if (!list.drmFormatModifierCount)
        return;
    list.pDrmFormatModifierProperties = calloc(list.drmFormatModifierCount,
        sizeof(*list.pDrmFormatModifierProperties));
    if (!list.pDrmFormatModifierProperties)
        return;
    uint32_t capacity = list.drmFormatModifierCount;
    vkGetPhysicalDeviceFormatProperties2(device, item->format, &properties);
    for (uint32_t index = 0; index < list.drmFormatModifierCount && index < capacity; ++index) {
        const VkDrmFormatModifierPropertiesEXT *modifier =
            &list.pDrmFormatModifierProperties[index];
        printf("  modifier=0x%016" PRIx64 " vendor=0x%02" PRIx64
               " planes=%u features=0x%08x\n",
               modifier->drmFormatModifier,
               modifier->drmFormatModifier >> 56,
               modifier->drmFormatModifierPlaneCount,
               modifier->drmFormatModifierTilingFeatures);
    }
    free(list.pDrmFormatModifierProperties);
}

int main(void)
{
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "vulkan-modifier-probe",
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo create = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
    };
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&create, NULL, &instance);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateInstance failed=%d\n", result);
        return 1;
    }
    uint32_t count = 0;
    result = vkEnumeratePhysicalDevices(instance, &count, NULL);
    if (result != VK_SUCCESS || count == 0) {
        fprintf(stderr, "vkEnumeratePhysicalDevices failed=%d count=%u\n",
                result, count);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices) {
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    result = vkEnumeratePhysicalDevices(instance, &count, devices);
    if (result != VK_SUCCESS) {
        free(devices);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    for (uint32_t device_index = 0; device_index < count; ++device_index) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[device_index], &properties);
        printf("device=%s vendor=0x%04x device=0x%08x\n",
               properties.deviceName, properties.vendorID, properties.deviceID);
        for (size_t index = 0; index < sizeof(formats) / sizeof(formats[0]); ++index)
            probe_format(devices[device_index], &formats[index]);
    }
    free(devices);
    vkDestroyInstance(instance, NULL);
    return 0;
}
