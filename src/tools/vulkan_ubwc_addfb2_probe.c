// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <drm.h>
#include <drm_fourcc.h>

struct fb_cmd2 {
    uint32_t fb_id, width, height, pixel_format, flags;
    uint32_t handles[4], pitches[4], offsets[4];
    uint64_t modifiers[4];
};

#define IOCTL_MODE_ADDFB2 DRM_IOWR(0xB8, struct fb_cmd2)
#define IOCTL_MODE_RMFB DRM_IOWR(0xAF, uint32_t)

static uint32_t find_memory_type(VkPhysicalDevice physical,
                                 uint32_t bits, VkMemoryPropertyFlags wanted)
{
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if ((bits & (1U << index)) &&
            (properties.memoryTypes[index].propertyFlags & wanted) == wanted)
            return index;
    }
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if (bits & (1U << index))
            return index;
    }
    return UINT32_MAX;
}

int main(void)
{
    const uint32_t width = 1920, height = 1080;
    const uint64_t ubwc = DRM_FORMAT_MOD_QCOM_COMPRESSED;
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    int dmabuf_fd = -1, card_fd = -1;
    uint32_t gem_handle = 0, fb_id = 0;
    int result = 1;

    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "ubwc-addfb2-probe",
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo instance_create = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
    };
    VkResult vk_result = vkCreateInstance(&instance_create, NULL, &instance);
    if (vk_result != VK_SUCCESS) {
        printf("vkCreateInstance failed=%d\n", vk_result);
        goto done;
    }
    uint32_t physical_count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    vk_result = vkEnumeratePhysicalDevices(instance, &physical_count, &physical);
    if (vk_result != VK_SUCCESS || physical_count == 0) {
        printf("no Vulkan physical device result=%d count=%u\n",
               vk_result, physical_count);
        goto done;
    }
    VkPhysicalDeviceProperties physical_properties;
    vkGetPhysicalDeviceProperties(physical, &physical_properties);
    printf("device=%s modifier=0x%016" PRIx64 "\n",
           physical_properties.deviceName, ubwc);

    uint32_t family_count = 0, family_index = UINT32_MAX;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, NULL);
    VkQueueFamilyProperties *families = calloc(family_count, sizeof(*families));
    if (!families)
        goto done;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
    for (uint32_t index = 0; index < family_count; ++index) {
        if (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            family_index = index;
            break;
        }
    }
    free(families);
    if (family_index == UINT32_MAX) {
        puts("no graphics queue family");
        goto done;
    }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_create = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family_index,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const char *extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    };
    VkDeviceCreateInfo device_create = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_create,
        .enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]),
        .ppEnabledExtensionNames = extensions,
    };
    vk_result = vkCreateDevice(physical, &device_create, NULL, &device);
    if (vk_result != VK_SUCCESS) {
        printf("vkCreateDevice failed=%d\n", vk_result);
        goto done;
    }

    VkImageDrmFormatModifierListCreateInfoEXT modifier_list = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .drmFormatModifierCount = 1,
        .pDrmFormatModifiers = &ubwc,
    };
    VkExternalMemoryImageCreateInfo external_image = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modifier_list,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo image_create = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_image,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    vk_result = vkCreateImage(device, &image_create, NULL, &image);
    if (vk_result != VK_SUCCESS) {
        printf("vkCreateImage UBWC failed=%d\n", vk_result);
        goto done;
    }
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image, &requirements);
    uint32_t memory_type = find_memory_type(physical,
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memory_type == UINT32_MAX) {
        puts("no compatible memory type");
        goto done;
    }
    VkExportMemoryAllocateInfo export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export_info,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type,
    };
    vk_result = vkAllocateMemory(device, &allocation, NULL, &memory);
    if (vk_result != VK_SUCCESS ||
        (vk_result = vkBindImageMemory(device, image, memory, 0)) != VK_SUCCESS) {
        printf("allocate/bind failed=%d size=%" PRIu64 "\n",
               vk_result, (uint64_t)requirements.size);
        goto done;
    }

    PFN_vkGetMemoryFdKHR get_memory_fd = (PFN_vkGetMemoryFdKHR)
        vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR");
    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_modifier =
        (PFN_vkGetImageDrmFormatModifierPropertiesEXT)
        vkGetDeviceProcAddr(device, "vkGetImageDrmFormatModifierPropertiesEXT");
    if (!get_memory_fd || !get_modifier) {
        puts("required Vulkan extension entry point missing");
        goto done;
    }
    VkImageDrmFormatModifierPropertiesEXT modifier_properties = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
    };
    vk_result = get_modifier(device, image, &modifier_properties);
    if (vk_result != VK_SUCCESS) {
        printf("vkGetImageDrmFormatModifierPropertiesEXT failed=%d\n", vk_result);
        goto done;
    }
    VkImageSubresource subresource = {
        .aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
        .mipLevel = 0,
        .arrayLayer = 0,
    };
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(device, image, &subresource, &layout);
    VkMemoryGetFdInfoKHR fd_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    vk_result = get_memory_fd(device, &fd_info, &dmabuf_fd);
    if (vk_result != VK_SUCCESS) {
        printf("vkGetMemoryFdKHR failed=%d\n", vk_result);
        goto done;
    }
    printf("Turnip image modifier=0x%016" PRIx64
           " size=%" PRIu64 " rowPitch=%" PRIu64 " offset=%" PRIu64 "\n",
           modifier_properties.drmFormatModifier,
           (uint64_t)requirements.size, (uint64_t)layout.rowPitch,
           (uint64_t)layout.offset);

    card_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (card_fd < 0) {
        printf("open card0 failed errno=%d (%s)\n", errno, strerror(errno));
        goto done;
    }
    struct drm_prime_handle prime = {.fd = dmabuf_fd};
    if (ioctl(card_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) != 0) {
        printf("PRIME_FD_TO_HANDLE failed errno=%d (%s)\n",
               errno, strerror(errno));
        goto done;
    }
    gem_handle = prime.handle;
    struct fb_cmd2 framebuffer = {
        .width = width,
        .height = height,
        .pixel_format = DRM_FORMAT_ABGR8888,
        .flags = DRM_MODE_FB_MODIFIERS,
    };
    framebuffer.handles[0] = gem_handle;
    framebuffer.pitches[0] = (uint32_t)layout.rowPitch;
    framebuffer.offsets[0] = (uint32_t)layout.offset;
    framebuffer.modifiers[0] = modifier_properties.drmFormatModifier;
    if (ioctl(card_fd, IOCTL_MODE_ADDFB2, &framebuffer) != 0) {
        printf("SDE ADDFB2 UBWC failed errno=%d (%s)\n", errno, strerror(errno));
        goto done;
    }
    fb_id = framebuffer.fb_id;
    printf("SDE ADDFB2 UBWC SUCCESS fb=%u format=AB24 modifier=0x%016" PRIx64 "\n",
           fb_id, framebuffer.modifiers[0]);
    result = 0;

done:
    if (fb_id && card_fd >= 0)
        ioctl(card_fd, IOCTL_MODE_RMFB, &fb_id);
    if (gem_handle && card_fd >= 0) {
        struct drm_gem_close close_request = {.handle = gem_handle};
        ioctl(card_fd, DRM_IOCTL_GEM_CLOSE, &close_request);
    }
    if (card_fd >= 0)
        close(card_fd);
    if (dmabuf_fd >= 0)
        close(dmabuf_fd);
    if (device != VK_NULL_HANDLE) {
        if (image != VK_NULL_HANDLE)
            vkDestroyImage(device, image, NULL);
        if (memory != VK_NULL_HANDLE)
            vkFreeMemory(device, memory, NULL);
        vkDestroyDevice(device, NULL);
    }
    if (instance != VK_NULL_HANDLE)
        vkDestroyInstance(instance, NULL);
    printf("DONE exit=%d\n", result);
    return result;
}
