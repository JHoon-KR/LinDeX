#define _GNU_SOURCE
#include "turnip_prime_import.h"
#include "turnip_repack_cache.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define ADVC_DRM_FORMAT_NV12 UINT32_C(0x3231564e)
#define ADVC_QCOM_COMPRESSED UINT64_C(0x0500000000000001)
#define ADVC_IMPORT_TIMEOUT_NS UINT64_C(5000000000)
#define ADVC_REPACK_MAX_POOL_SLOTS 32u

struct vulkan_state {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkImage image;
    VkDeviceMemory image_memory;
    VkImage linear_image;
    VkDeviceMemory linear_image_memory;
    VkBuffer readback;
    VkDeviceMemory readback_memory;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;
    VkSemaphore acquire_semaphore;
    VkSemaphore release_semaphore;
    VkFence fence;
    int submitted;
};

struct repack_shared_state {
    pthread_mutex_t mutex;
    int initialized;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties;
    PFN_vkGetMemoryFdKHR get_memory_fd;
    PFN_vkImportSemaphoreFdKHR import_semaphore_fd;
    PFN_vkGetSemaphoreFdKHR get_semaphore_fd;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_image_modifier;
    size_t attached_pools;
    char device_name[256];
};

struct repack_source_slot {
    VkImage image;
    VkDeviceMemory memory;
};

struct repack_destination_slot {
    struct advc_repack_descriptor_signature signature;
    int signature_valid;
    int has_content;
    int pending_release_fence_fd;
    VkImage image;
    VkDeviceMemory memory;
    VkDeviceSize memory_size;
    VkSubresourceLayout layouts[2];
    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;
    VkSemaphore source_acquire_semaphore;
    VkSemaphore destination_acquire_semaphore;
    VkSemaphore release_semaphore;
    VkFence fence;
    int submitted;
};

struct advc_turnip_linear_repack_pool {
    size_t slot_count;
    int shared_attached;
    uint64_t use_clock;
    uint64_t next_token;
    struct repack_source_slot *sources;
    struct advc_repack_source_key *source_keys;
    struct repack_destination_slot *destinations;
    struct advc_repack_lease_key *leases;
};

static struct repack_shared_state repack_shared = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
};

static int vk_errno(VkResult result) {
    switch (result) {
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return ENOMEM;
    case VK_ERROR_EXTENSION_NOT_PRESENT:
    case VK_ERROR_FEATURE_NOT_PRESENT:
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return ENOTSUP;
    case VK_ERROR_DEVICE_LOST:
        return ENODEV;
    default:
        return EIO;
    }
}

static int extension_present(VkPhysicalDevice physical, const char *name) {
    VkExtensionProperties *properties = NULL;
    uint32_t count = 0;
    int found = 0;
    if (vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL) !=
            VK_SUCCESS || count == 0)
        return 0;
    properties = calloc(count, sizeof(*properties));
    if (properties == NULL) return 0;
    if (vkEnumerateDeviceExtensionProperties(physical, NULL, &count,
                                              properties) == VK_SUCCESS) {
        for (uint32_t i = 0; i < count; ++i) {
            if (strcmp(properties[i].extensionName, name) == 0) {
                found = 1;
                break;
            }
        }
    }
    free(properties);
    return found;
}

static int find_queue_family(VkPhysicalDevice physical, uint32_t *family_out) {
    VkQueueFamilyProperties *families = NULL;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    if (count == 0) return -1;
    families = calloc(count, sizeof(*families));
    if (families == NULL) return -1;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    for (uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags &
             (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT)) != 0) {
            *family_out = i;
            free(families);
            return 0;
        }
    }
    free(families);
    errno = ENOTSUP;
    return -1;
}

static uint32_t find_memory_type(
    VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags required,
    VkMemoryPropertyFlags *actual_out) {
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        VkMemoryPropertyFlags actual =
            properties.memoryTypes[i].propertyFlags;
        if ((bits & (UINT32_C(1) << i)) != 0 &&
            (actual & required) == required) {
            if (actual_out != NULL) *actual_out = actual;
            return i;
        }
    }
    return UINT32_MAX;
}

static int modifier_supports(VkPhysicalDevice physical, uint64_t modifier,
                             VkFormatFeatureFlags required) {
    VkDrmFormatModifierPropertiesListEXT list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };
    VkFormatProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &list,
    };
    VkDrmFormatModifierPropertiesEXT *modifiers;
    vkGetPhysicalDeviceFormatProperties2(
        physical, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, &properties);
    if (list.drmFormatModifierCount == 0) return 0;
    modifiers = calloc(list.drmFormatModifierCount, sizeof(*modifiers));
    if (modifiers == NULL) return 0;
    list.pDrmFormatModifierProperties = modifiers;
    vkGetPhysicalDeviceFormatProperties2(
        physical, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, &properties);
    for (uint32_t i = 0; i < list.drmFormatModifierCount; ++i) {
        if (modifiers[i].drmFormatModifier == modifier &&
            modifiers[i].drmFormatModifierPlaneCount == 2 &&
            (modifiers[i].drmFormatModifierTilingFeatures & required) ==
                required) {
            free(modifiers);
            return 1;
        }
    }
    free(modifiers);
    return 0;
}

static int modifier_supported(VkPhysicalDevice physical, uint64_t modifier) {
    return modifier_supports(physical, modifier,
                             VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
}

static int choose_device(struct vulkan_state *state, char name[256]) {
    static const char *required_extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    };
    VkPhysicalDevice *devices = NULL;
    uint32_t count = 0;
    VkResult status = vkEnumeratePhysicalDevices(state->instance, &count, NULL);
    if (status != VK_SUCCESS || count == 0) {
        errno = status == VK_SUCCESS ? ENODEV : vk_errno(status);
        return -1;
    }
    devices = calloc(count, sizeof(*devices));
    if (devices == NULL) return -1;
    status = vkEnumeratePhysicalDevices(state->instance, &count, devices);
    if (status != VK_SUCCESS) {
        free(devices);
        errno = vk_errno(status);
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t family;
        int complete = 1;
        for (size_t extension = 0;
             extension < sizeof(required_extensions) /
                             sizeof(required_extensions[0]);
             ++extension) {
            if (!extension_present(devices[i], required_extensions[extension])) {
                complete = 0;
                break;
            }
        }
        if (!complete || find_queue_family(devices[i], &family) < 0) continue;
        state->physical = devices[i];
        state->queue_family = family;
        {
            VkPhysicalDeviceProperties properties;
            size_t name_length;
            vkGetPhysicalDeviceProperties(devices[i], &properties);
            name_length = strnlen(properties.deviceName, 255);
            memcpy(name, properties.deviceName, name_length);
            name[name_length] = '\0';
        }
        free(devices);
        return 0;
    }
    free(devices);
    errno = ENOTSUP;
    return -1;
}

static void destroy_state(struct vulkan_state *state) {
    if (state->device != VK_NULL_HANDLE && state->submitted)
        (void)vkDeviceWaitIdle(state->device);
    if (state->device != VK_NULL_HANDLE) {
        if (state->fence != VK_NULL_HANDLE)
            vkDestroyFence(state->device, state->fence, NULL);
        if (state->release_semaphore != VK_NULL_HANDLE)
            vkDestroySemaphore(state->device, state->release_semaphore, NULL);
        if (state->acquire_semaphore != VK_NULL_HANDLE)
            vkDestroySemaphore(state->device, state->acquire_semaphore, NULL);
        if (state->command_pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(state->device, state->command_pool, NULL);
        if (state->readback != VK_NULL_HANDLE)
            vkDestroyBuffer(state->device, state->readback, NULL);
        if (state->readback_memory != VK_NULL_HANDLE)
            vkFreeMemory(state->device, state->readback_memory, NULL);
        if (state->image != VK_NULL_HANDLE)
            vkDestroyImage(state->device, state->image, NULL);
        if (state->image_memory != VK_NULL_HANDLE)
            vkFreeMemory(state->device, state->image_memory, NULL);
        if (state->linear_image != VK_NULL_HANDLE)
            vkDestroyImage(state->device, state->linear_image, NULL);
        if (state->linear_image_memory != VK_NULL_HANDLE)
            vkFreeMemory(state->device, state->linear_image_memory, NULL);
        vkDestroyDevice(state->device, NULL);
    }
    if (state->instance != VK_NULL_HANDLE)
        vkDestroyInstance(state->instance, NULL);
}

static int checked_layout(const struct advc_dmabuf_descriptor *descriptor,
                          VkDeviceSize *uv_offset,
                          VkDeviceSize *total_size) {
    uint64_t y_size;
    uint64_t uv_size;
    uint64_t aligned;
    if (descriptor->width > UINT32_MAX / descriptor->height) {
        errno = EOVERFLOW;
        return -1;
    }
    y_size = (uint64_t)descriptor->width * descriptor->height;
    uv_size = y_size / 2u;
    aligned = (y_size + 255u) & ~UINT64_C(255);
    if (aligned > UINT64_MAX - uv_size) {
        errno = EOVERFLOW;
        return -1;
    }
    *uv_offset = (VkDeviceSize)aligned;
    *total_size = (VkDeviceSize)(aligned + uv_size);
    return 0;
}

int advc_turnip_prime_consume(
    const struct advc_dmabuf_descriptor *descriptor, int acquire_fence_fd,
    struct advc_turnip_prime_result *result) {
    static const char *extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    };
    struct vulkan_state state;
    VkSubresourceLayout plane_layouts[2];
    VkDeviceSize uv_offset = 0;
    VkDeviceSize readback_size = 0;
    VkMemoryPropertyFlags readback_properties = 0;
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties;
    PFN_vkImportSemaphoreFdKHR import_semaphore_fd;
    PFN_vkGetSemaphoreFdKHR get_semaphore_fd;
    VkResult status;
    int imported_acquire_fd = -1;
    int saved_errno = 0;
    int rc = -1;

    if (result == NULL || descriptor == NULL || acquire_fence_fd < -1 ||
        advc_dmabuf_descriptor_validate(descriptor) < 0 ||
        descriptor->drm_fourcc != ADVC_DRM_FORMAT_NV12 ||
        (descriptor->drm_modifier != 0 &&
         descriptor->drm_modifier != ADVC_QCOM_COMPRESSED) ||
        descriptor->object_count != 1 || descriptor->plane_count != 2 ||
        descriptor->planes[0].object_index != 0 ||
        descriptor->planes[1].object_index != 0 ||
        (descriptor->width & 1u) != 0 || (descriptor->height & 1u) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (acquire_fence_fd >= 0 &&
        advc_dmabuf_sync_file_validate(acquire_fence_fd) < 0)
        return -1;
    memset(result, 0, sizeof(*result));
    result->release_fence_fd = -1;
    memset(&state, 0, sizeof(state));
    if (checked_layout(descriptor, &uv_offset, &readback_size) < 0)
        return -1;

    {
        VkApplicationInfo application = {
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "lindex-turnip-prime-import",
            .apiVersion = VK_API_VERSION_1_1,
        };
        VkInstanceCreateInfo create = {
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &application,
        };
        status = vkCreateInstance(&create, NULL, &state.instance);
        if (status != VK_SUCCESS) goto vk_fail;
    }
    if (choose_device(&state, result->device_name) < 0) goto fail;
    if (!modifier_supported(state.physical, descriptor->drm_modifier)) {
        errno = ENOTSUP;
        goto fail;
    }
    {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = state.queue_family,
            .queueCount = 1,
            .pQueuePriorities = &priority,
        };
        VkDeviceCreateInfo create = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queue,
            .enabledExtensionCount =
                (uint32_t)(sizeof(extensions) / sizeof(extensions[0])),
            .ppEnabledExtensionNames = extensions,
        };
        status = vkCreateDevice(state.physical, &create, NULL, &state.device);
        if (status != VK_SUCCESS) goto vk_fail;
        vkGetDeviceQueue(state.device, state.queue_family, 0, &state.queue);
    }
    get_memory_fd_properties = (PFN_vkGetMemoryFdPropertiesKHR)
        vkGetDeviceProcAddr(state.device, "vkGetMemoryFdPropertiesKHR");
    import_semaphore_fd = (PFN_vkImportSemaphoreFdKHR)
        vkGetDeviceProcAddr(state.device, "vkImportSemaphoreFdKHR");
    get_semaphore_fd = (PFN_vkGetSemaphoreFdKHR)
        vkGetDeviceProcAddr(state.device, "vkGetSemaphoreFdKHR");
    if (get_memory_fd_properties == NULL || import_semaphore_fd == NULL ||
        get_semaphore_fd == NULL) {
        errno = ENOTSUP;
        goto fail;
    }

    memset(plane_layouts, 0, sizeof(plane_layouts));
    for (uint32_t i = 0; i < 2; ++i) {
        plane_layouts[i].offset = descriptor->planes[i].offset;
        plane_layouts[i].rowPitch = descriptor->planes[i].pitch;
        plane_layouts[i].size = descriptor->objects[0].size -
                                descriptor->planes[i].offset;
    }
    {
        VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_layout = {
            .sType =
                VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
            .drmFormatModifier = descriptor->drm_modifier,
            .drmFormatModifierPlaneCount = 2,
            .pPlaneLayouts = plane_layouts,
        };
        VkExternalMemoryImageCreateInfo external = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .pNext = &explicit_layout,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };
        VkImageCreateInfo create = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &external,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
            .extent = {descriptor->width, descriptor->height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        status = vkCreateImage(state.device, &create, NULL, &state.image);
        if (status != VK_SUCCESS) goto vk_fail;
    }
    {
        VkMemoryRequirements requirements;
        VkMemoryFdPropertiesKHR fd_properties = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
        };
        uint32_t memory_type;
        int imported_fd = fcntl(descriptor->objects[0].fd, F_DUPFD_CLOEXEC, 3);
        if (imported_fd < 0) goto fail;
        status = get_memory_fd_properties(
            state.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
            imported_fd, &fd_properties);
        if (status != VK_SUCCESS) {
            close(imported_fd);
            goto vk_fail;
        }
        vkGetImageMemoryRequirements(state.device, state.image, &requirements);
        memory_type = find_memory_type(
            state.physical,
            requirements.memoryTypeBits & fd_properties.memoryTypeBits, 0,
            NULL);
        if (memory_type == UINT32_MAX) {
            close(imported_fd);
            errno = ENOTSUP;
            goto fail;
        }
        {
            VkMemoryDedicatedAllocateInfo dedicated = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                .image = state.image,
            };
            VkImportMemoryFdInfoKHR import = {
                .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                .pNext = &dedicated,
                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                .fd = imported_fd,
            };
            VkMemoryAllocateInfo allocation = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .pNext = &import,
                .allocationSize = descriptor->objects[0].size,
                .memoryTypeIndex = memory_type,
            };
            status = vkAllocateMemory(state.device, &allocation, NULL,
                                      &state.image_memory);
            if (status != VK_SUCCESS) {
                close(imported_fd); /* Vulkan consumes it only on success. */
                goto vk_fail;
            }
        }
        status = vkBindImageMemory(state.device, state.image,
                                   state.image_memory, 0);
        if (status != VK_SUCCESS) goto vk_fail;
    }
    {
        VkBufferCreateInfo create = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = readback_size,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VkMemoryRequirements requirements;
        uint32_t memory_type;
        status = vkCreateBuffer(state.device, &create, NULL, &state.readback);
        if (status != VK_SUCCESS) goto vk_fail;
        vkGetBufferMemoryRequirements(state.device, state.readback,
                                      &requirements);
        memory_type = find_memory_type(
            state.physical, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &readback_properties);
        if (memory_type == UINT32_MAX) {
            errno = ENOTSUP;
            goto fail;
        }
        {
            VkMemoryAllocateInfo allocation = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = requirements.size,
                .memoryTypeIndex = memory_type,
            };
            status = vkAllocateMemory(state.device, &allocation, NULL,
                                      &state.readback_memory);
            if (status != VK_SUCCESS) goto vk_fail;
        }
        status = vkBindBufferMemory(state.device, state.readback,
                                    state.readback_memory, 0);
        if (status != VK_SUCCESS) goto vk_fail;
    }
    {
        VkCommandPoolCreateInfo pool = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = state.queue_family,
        };
        VkCommandBufferAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = VK_NULL_HANDLE,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        status = vkCreateCommandPool(state.device, &pool, NULL,
                                     &state.command_pool);
        if (status != VK_SUCCESS) goto vk_fail;
        allocation.commandPool = state.command_pool;
        status = vkAllocateCommandBuffers(state.device, &allocation,
                                          &state.command_buffer);
        if (status != VK_SUCCESS) goto vk_fail;
    }
    if (acquire_fence_fd >= 0) {
        VkSemaphoreCreateInfo create = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        imported_acquire_fd = fcntl(acquire_fence_fd, F_DUPFD_CLOEXEC, 3);
        if (imported_acquire_fd < 0) goto fail;
        status = vkCreateSemaphore(state.device, &create, NULL,
                                   &state.acquire_semaphore);
        if (status != VK_SUCCESS) goto vk_fail;
        {
            VkImportSemaphoreFdInfoKHR import = {
                .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
                .semaphore = state.acquire_semaphore,
                .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
                .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
                .fd = imported_acquire_fd,
            };
            status = import_semaphore_fd(state.device, &import);
            if (status != VK_SUCCESS) goto vk_fail;
            imported_acquire_fd = -1; /* Consumed by Vulkan. */
        }
    }
    {
        VkExportSemaphoreCreateInfo export = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        };
        VkSemaphoreCreateInfo create = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &export,
        };
        VkFenceCreateInfo fence = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        };
        status = vkCreateSemaphore(state.device, &create, NULL,
                                   &state.release_semaphore);
        if (status != VK_SUCCESS) goto vk_fail;
        status = vkCreateFence(state.device, &fence, NULL, &state.fence);
        if (status != VK_SUCCESS) goto vk_fail;
    }
    {
        VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        VkImageMemoryBarrier acquire_barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
            .dstQueueFamilyIndex = state.queue_family,
            .image = state.image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        VkBufferImageCopy copies[2];
        VkBufferMemoryBarrier host_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = state.readback,
            .offset = 0,
            .size = readback_size,
        };
        VkImageMemoryBarrier release_barrier = acquire_barrier;
        memset(copies, 0, sizeof(copies));
        copies[0].bufferOffset = 0;
        copies[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
        copies[0].imageSubresource.layerCount = 1;
        copies[0].imageExtent.width = descriptor->width;
        copies[0].imageExtent.height = descriptor->height;
        copies[0].imageExtent.depth = 1;
        copies[1].bufferOffset = uv_offset;
        copies[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
        copies[1].imageSubresource.layerCount = 1;
        copies[1].imageExtent.width = descriptor->width / 2u;
        copies[1].imageExtent.height = descriptor->height / 2u;
        copies[1].imageExtent.depth = 1;
        status = vkBeginCommandBuffer(state.command_buffer, &begin);
        if (status != VK_SUCCESS) goto vk_fail;
        vkCmdPipelineBarrier(
            state.command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
            &acquire_barrier);
        vkCmdCopyImageToBuffer(
            state.command_buffer, state.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, state.readback, 2, copies);
        vkCmdPipelineBarrier(
            state.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &host_barrier, 0, NULL);
        release_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        release_barrier.dstAccessMask = 0;
        release_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        release_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        release_barrier.srcQueueFamilyIndex = state.queue_family;
        release_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        vkCmdPipelineBarrier(
            state.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1,
            &release_barrier);
        status = vkEndCommandBuffer(state.command_buffer);
        if (status != VK_SUCCESS) goto vk_fail;
    }
    {
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = state.acquire_semaphore != VK_NULL_HANDLE ?
                                  1u : 0u,
            .pWaitSemaphores = state.acquire_semaphore != VK_NULL_HANDLE ?
                               &state.acquire_semaphore : NULL,
            .pWaitDstStageMask = state.acquire_semaphore != VK_NULL_HANDLE ?
                                 &wait_stage : NULL,
            .commandBufferCount = 1,
            .pCommandBuffers = &state.command_buffer,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &state.release_semaphore,
        };
        VkSemaphoreGetFdInfoKHR fd_info = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
            .semaphore = state.release_semaphore,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        };
        status = vkQueueSubmit(state.queue, 1, &submit, state.fence);
        if (status != VK_SUCCESS) goto vk_fail;
        state.submitted = 1;
        status = get_semaphore_fd(state.device, &fd_info,
                                  &result->release_fence_fd);
        if (status != VK_SUCCESS || result->release_fence_fd < 0)
            goto vk_fail;
        status = vkWaitForFences(state.device, 1, &state.fence, VK_TRUE,
                                 ADVC_IMPORT_TIMEOUT_NS);
        if (status != VK_SUCCESS) goto vk_fail;
    }
    {
        void *mapping = NULL;
        const uint8_t *bytes;
        uint64_t hash = UINT64_C(1469598103934665603);
        uint8_t seen[256] = {0};
        uint32_t distinct = 0;
        status = vkMapMemory(state.device, state.readback_memory, 0,
                             readback_size, 0, &mapping);
        if (status != VK_SUCCESS || mapping == NULL) goto vk_fail;
        if ((readback_properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
            VkMappedMemoryRange range = {
                .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory = state.readback_memory,
                .offset = 0,
                .size = VK_WHOLE_SIZE,
            };
            status = vkInvalidateMappedMemoryRanges(state.device, 1, &range);
            if (status != VK_SUCCESS) {
                vkUnmapMemory(state.device, state.readback_memory);
                goto vk_fail;
            }
        }
        bytes = (const uint8_t *)mapping;
        for (VkDeviceSize i = 0; i < readback_size; ++i) {
            hash ^= bytes[i];
            hash *= UINT64_C(1099511628211);
            if (!seen[bytes[i]]) {
                seen[bytes[i]] = 1;
                ++distinct;
            }
        }
        vkUnmapMemory(state.device, state.readback_memory);
        result->content_hash = hash;
        result->content_bytes = readback_size;
        result->distinct_sample_values = distinct;
        if (distinct < 2) {
            errno = EBADMSG;
            goto fail;
        }
    }
    rc = 0;
    goto done;

vk_fail:
    errno = vk_errno(status);
fail:
    saved_errno = errno == 0 ? EIO : errno;
done:
    if (imported_acquire_fd >= 0) close(imported_acquire_fd);
    if (rc < 0 && result->release_fence_fd >= 0) {
        close(result->release_fence_fd);
        result->release_fence_fd = -1;
    }
    destroy_state(&state);
    if (rc < 0) errno = saved_errno;
    return rc;
}

static int set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) return -1;
    if ((flags & FD_CLOEXEC) != 0) return 0;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int external_image_supported(VkPhysicalDevice physical,
                                    uint64_t modifier,
                                    VkImageUsageFlags usage,
                                    VkExternalMemoryFeatureFlags required) {
    VkPhysicalDeviceExternalImageFormatInfo external = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .pNext = &external,
        .drmFormatModifier = modifier,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkPhysicalDeviceImageFormatInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &modifier_info,
        .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = usage,
    };
    VkExternalImageFormatProperties external_properties = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
    };
    VkImageFormatProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &external_properties,
    };
    VkResult status = vkGetPhysicalDeviceImageFormatProperties2(
        physical, &info, &properties);
    if (status != VK_SUCCESS) {
        errno = status == VK_ERROR_FORMAT_NOT_SUPPORTED ? ENOTSUP :
                                                        vk_errno(status);
        return -1;
    }
    if ((external_properties.externalMemoryProperties.externalMemoryFeatures &
         required) != required) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

static int initialize_repack_shared(void) {
    static const char *extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    };
    const VkImageUsageFlags source_usage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkImageUsageFlags destination_usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT;
    struct vulkan_state state;
    VkResult status = VK_SUCCESS;
    int saved_errno;

    if (repack_shared.initialized) return 0;
    memset(&state, 0, sizeof(state));
    {
        VkApplicationInfo application = {
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "lindex-turnip-linear-repack",
            .apiVersion = VK_API_VERSION_1_1,
        };
        VkInstanceCreateInfo create = {
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &application,
        };
        status = vkCreateInstance(&create, NULL, &state.instance);
        if (status != VK_SUCCESS) goto vk_fail;
    }
    if (choose_device(&state, repack_shared.device_name) < 0) goto fail;
    if (!modifier_supports(state.physical, ADVC_QCOM_COMPRESSED,
                           VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                               VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
        !modifier_supports(state.physical, 0,
                           VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                               VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                               VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        errno = ENOTSUP;
        goto fail;
    }
    if (external_image_supported(
            state.physical, ADVC_QCOM_COMPRESSED, source_usage,
            VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) < 0 ||
        external_image_supported(
            state.physical, 0, destination_usage,
            VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) < 0)
        goto fail;
    {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = state.queue_family,
            .queueCount = 1,
            .pQueuePriorities = &priority,
        };
        VkDeviceCreateInfo create = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queue,
            .enabledExtensionCount =
                (uint32_t)(sizeof(extensions) / sizeof(extensions[0])),
            .ppEnabledExtensionNames = extensions,
        };
        status = vkCreateDevice(state.physical, &create, NULL, &state.device);
        if (status != VK_SUCCESS) goto vk_fail;
        vkGetDeviceQueue(state.device, state.queue_family, 0, &state.queue);
    }
    repack_shared.get_memory_fd_properties =
        (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
            state.device, "vkGetMemoryFdPropertiesKHR");
    repack_shared.get_memory_fd =
        (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(state.device,
                                                  "vkGetMemoryFdKHR");
    repack_shared.import_semaphore_fd =
        (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(
            state.device, "vkImportSemaphoreFdKHR");
    repack_shared.get_semaphore_fd =
        (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(
            state.device, "vkGetSemaphoreFdKHR");
    repack_shared.get_image_modifier =
        (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(
            state.device, "vkGetImageDrmFormatModifierPropertiesEXT");
    if (repack_shared.get_memory_fd_properties == NULL ||
        repack_shared.get_memory_fd == NULL ||
        repack_shared.import_semaphore_fd == NULL ||
        repack_shared.get_semaphore_fd == NULL ||
        repack_shared.get_image_modifier == NULL) {
        errno = ENOTSUP;
        goto fail;
    }
    repack_shared.instance = state.instance;
    repack_shared.physical = state.physical;
    repack_shared.device = state.device;
    repack_shared.queue = state.queue;
    repack_shared.queue_family = state.queue_family;
    repack_shared.initialized = 1;
    state.instance = VK_NULL_HANDLE;
    state.device = VK_NULL_HANDLE;
    return 0;

vk_fail:
    errno = vk_errno(status);
fail:
    saved_errno = errno == 0 ? EIO : errno;
    destroy_state(&state);
    errno = saved_errno;
    return -1;
}

static void destroy_repack_source_locked(struct repack_source_slot *slot) {
    if (slot->image != VK_NULL_HANDLE)
        vkDestroyImage(repack_shared.device, slot->image, NULL);
    if (slot->memory != VK_NULL_HANDLE)
        vkFreeMemory(repack_shared.device, slot->memory, NULL);
    memset(slot, 0, sizeof(*slot));
}

static void destroy_repack_destination_locked(
    struct repack_destination_slot *slot) {
    if (slot->submitted && slot->fence != VK_NULL_HANDLE &&
        repack_shared.device != VK_NULL_HANDLE)
        (void)vkWaitForFences(repack_shared.device, 1, &slot->fence, VK_TRUE,
                              ADVC_IMPORT_TIMEOUT_NS);
    if (slot->pending_release_fence_fd >= 0)
        close(slot->pending_release_fence_fd);
    if (slot->fence != VK_NULL_HANDLE)
        vkDestroyFence(repack_shared.device, slot->fence, NULL);
    if (slot->release_semaphore != VK_NULL_HANDLE)
        vkDestroySemaphore(repack_shared.device, slot->release_semaphore, NULL);
    if (slot->destination_acquire_semaphore != VK_NULL_HANDLE)
        vkDestroySemaphore(repack_shared.device,
                           slot->destination_acquire_semaphore, NULL);
    if (slot->source_acquire_semaphore != VK_NULL_HANDLE)
        vkDestroySemaphore(repack_shared.device,
                           slot->source_acquire_semaphore, NULL);
    if (slot->command_pool != VK_NULL_HANDLE)
        vkDestroyCommandPool(repack_shared.device, slot->command_pool, NULL);
    if (slot->image != VK_NULL_HANDLE)
        vkDestroyImage(repack_shared.device, slot->image, NULL);
    if (slot->memory != VK_NULL_HANDLE)
        vkFreeMemory(repack_shared.device, slot->memory, NULL);
    memset(slot, 0, sizeof(*slot));
    slot->pending_release_fence_fd = -1;
}

static void destroy_repack_shared_locked(void) {
    if (!repack_shared.initialized) return;
    (void)vkDeviceWaitIdle(repack_shared.device);
    vkDestroyDevice(repack_shared.device, NULL);
    vkDestroyInstance(repack_shared.instance, NULL);
    repack_shared.initialized = 0;
    repack_shared.instance = VK_NULL_HANDLE;
    repack_shared.physical = VK_NULL_HANDLE;
    repack_shared.device = VK_NULL_HANDLE;
    repack_shared.queue = VK_NULL_HANDLE;
    repack_shared.queue_family = 0;
    repack_shared.get_memory_fd_properties = NULL;
    repack_shared.get_memory_fd = NULL;
    repack_shared.import_semaphore_fd = NULL;
    repack_shared.get_semaphore_fd = NULL;
    repack_shared.get_image_modifier = NULL;
    repack_shared.device_name[0] = '\0';
}

static int attach_repack_pool_locked(
    struct advc_turnip_linear_repack_pool *pool) {
    if (pool->shared_attached) return 0;
    if (initialize_repack_shared() < 0) return -1;
    ++repack_shared.attached_pools;
    pool->shared_attached = 1;
    return 0;
}

struct advc_turnip_linear_repack_pool *
advc_turnip_linear_repack_pool_create(size_t max_slots) {
    struct advc_turnip_linear_repack_pool *pool;
    if (max_slots == 0 || max_slots > ADVC_REPACK_MAX_POOL_SLOTS) {
        errno = EINVAL;
        return NULL;
    }
    pool = calloc(1, sizeof(*pool));
    if (pool == NULL) return NULL;
    pool->sources = calloc(max_slots, sizeof(*pool->sources));
    pool->source_keys = calloc(max_slots, sizeof(*pool->source_keys));
    pool->destinations = calloc(max_slots, sizeof(*pool->destinations));
    pool->leases = calloc(max_slots, sizeof(*pool->leases));
    if (pool->sources == NULL || pool->source_keys == NULL ||
        pool->destinations == NULL ||
        pool->leases == NULL) {
        free(pool->leases);
        free(pool->destinations);
        free(pool->source_keys);
        free(pool->sources);
        free(pool);
        return NULL;
    }
    pool->slot_count = max_slots;
    for (size_t i = 0; i < max_slots; ++i)
        pool->destinations[i].pending_release_fence_fd = -1;
    return pool;
}

void advc_turnip_linear_repack_pool_destroy(
    struct advc_turnip_linear_repack_pool *pool) {
    if (pool == NULL) return;
    if (pool->shared_attached) {
        pthread_mutex_lock(&repack_shared.mutex);
        for (size_t i = 0; i < pool->slot_count; ++i) {
            destroy_repack_source_locked(&pool->sources[i]);
            memset(&pool->source_keys[i], 0,
                   sizeof(pool->source_keys[i]));
            destroy_repack_destination_locked(&pool->destinations[i]);
            advc_repack_lease_clear(&pool->leases[i]);
        }
        pool->shared_attached = 0;
        if (repack_shared.attached_pools > 0)
            --repack_shared.attached_pools;
        if (repack_shared.attached_pools == 0)
            destroy_repack_shared_locked();
        pthread_mutex_unlock(&repack_shared.mutex);
    }
    free(pool->leases);
    free(pool->destinations);
    free(pool->source_keys);
    free(pool->sources);
    free(pool);
}

int advc_turnip_linear_repack_pool_release(
    struct advc_turnip_linear_repack_pool *pool, uint64_t lease_token,
    int release_fence_fd) {
    size_t index;
    int duplicate = -1;
    int saved_errno;
    if (pool == NULL || lease_token == 0 || release_fence_fd < -1) {
        errno = EINVAL;
        return -1;
    }
    if (release_fence_fd >= 0) {
        if (advc_dmabuf_sync_file_validate(release_fence_fd) < 0) return -1;
        duplicate = fcntl(release_fence_fd, F_DUPFD_CLOEXEC, 3);
        if (duplicate < 0) return -1;
    }
    pthread_mutex_lock(&repack_shared.mutex);
    if (!pool->shared_attached ||
        advc_repack_lease_find(pool->leases, pool->slot_count, lease_token,
                               &index) < 0) {
        saved_errno = errno == 0 ? ENOENT : errno;
        pthread_mutex_unlock(&repack_shared.mutex);
        if (duplicate >= 0) close(duplicate);
        errno = saved_errno;
        return -1;
    }
    if (pool->destinations[index].pending_release_fence_fd >= 0)
        close(pool->destinations[index].pending_release_fence_fd);
    pool->destinations[index].pending_release_fence_fd = duplicate;
    advc_repack_lease_clear(&pool->leases[index]);
    pthread_mutex_unlock(&repack_shared.mutex);
    return 0;
}

int advc_turnip_linear_repack_pool_discard(
    struct advc_turnip_linear_repack_pool *pool, uint64_t lease_token) {
    size_t index;
    int saved_errno;
    if (pool == NULL || lease_token == 0) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&repack_shared.mutex);
    if (!pool->shared_attached ||
        advc_repack_lease_find(pool->leases, pool->slot_count, lease_token,
                               &index) < 0) {
        saved_errno = errno == 0 ? ENOENT : errno;
        pthread_mutex_unlock(&repack_shared.mutex);
        errno = saved_errno;
        return -1;
    }
    destroy_repack_destination_locked(&pool->destinations[index]);
    advc_repack_lease_clear(&pool->leases[index]);
    pthread_mutex_unlock(&repack_shared.mutex);
    return 0;
}

static int create_repack_source_locked(
    struct repack_source_slot *slot,
    const struct advc_dmabuf_descriptor *source) {
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkSubresourceLayout layouts[2];
    VkResult status;
    int imported_fd = -1;
    int saved_errno;

    memset(layouts, 0, sizeof(layouts));
    for (uint32_t i = 0; i < 2; ++i) {
        layouts[i].offset = source->planes[i].offset;
        layouts[i].rowPitch = source->planes[i].pitch;
        layouts[i].size = source->objects[0].size - source->planes[i].offset;
    }
    {
        VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_layout = {
            .sType =
                VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
            .drmFormatModifier = source->drm_modifier,
            .drmFormatModifierPlaneCount = 2,
            .pPlaneLayouts = layouts,
        };
        VkExternalMemoryImageCreateInfo external = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .pNext = &explicit_layout,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };
        VkImageCreateInfo create = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &external,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
            .extent = {source->width, source->height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        status = vkCreateImage(repack_shared.device, &create, NULL,
                               &slot->image);
        if (status != VK_SUCCESS) goto vk_fail;
    }
    {
        VkMemoryRequirements requirements;
        VkMemoryFdPropertiesKHR fd_properties = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
        };
        uint32_t memory_type;
        imported_fd = fcntl(source->objects[0].fd, F_DUPFD_CLOEXEC, 3);
        if (imported_fd < 0) goto fail;
        status = repack_shared.get_memory_fd_properties(
            repack_shared.device,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, imported_fd,
            &fd_properties);
        if (status != VK_SUCCESS) goto vk_fail;
        vkGetImageMemoryRequirements(repack_shared.device, slot->image,
                                     &requirements);
        memory_type = find_memory_type(
            repack_shared.physical,
            requirements.memoryTypeBits & fd_properties.memoryTypeBits, 0,
            NULL);
        if (memory_type == UINT32_MAX) {
            errno = ENOTSUP;
            goto fail;
        }
        {
            VkMemoryDedicatedAllocateInfo dedicated = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                .image = slot->image,
            };
            VkImportMemoryFdInfoKHR import = {
                .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                .pNext = &dedicated,
                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                .fd = imported_fd,
            };
            VkMemoryAllocateInfo allocation = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .pNext = &import,
                .allocationSize = source->objects[0].size,
                .memoryTypeIndex = memory_type,
            };
            status = vkAllocateMemory(repack_shared.device, &allocation, NULL,
                                      &slot->memory);
            if (status != VK_SUCCESS) goto vk_fail;
            imported_fd = -1; /* Vulkan consumed it. */
        }
        status = vkBindImageMemory(repack_shared.device, slot->image,
                                   slot->memory, 0);
        if (status != VK_SUCCESS) goto vk_fail;
    }
    return 0;

vk_fail:
    errno = vk_errno(status);
fail:
    saved_errno = errno == 0 ? EIO : errno;
    if (imported_fd >= 0) close(imported_fd);
    destroy_repack_source_locked(slot);
    errno = saved_errno;
    return -1;
}

static int create_repack_destination_locked(
    struct repack_destination_slot *slot,
    const struct advc_repack_descriptor_signature *signature) {
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT;
    const uint64_t linear_modifier = 0;
    VkMemoryRequirements requirements;
    VkResult status;
    int saved_errno;

    {
        VkImageDrmFormatModifierListCreateInfoEXT modifier_list = {
            .sType =
                VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
            .drmFormatModifierCount = 1,
            .pDrmFormatModifiers = &linear_modifier,
        };
        VkExternalMemoryImageCreateInfo external = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .pNext = &modifier_list,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };
        VkImageCreateInfo create = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &external,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
            .extent = {signature->width, signature->height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        status = vkCreateImage(repack_shared.device, &create, NULL,
                               &slot->image);
        if (status != VK_SUCCESS) goto vk_fail;
    }
    {
        VkImageDrmFormatModifierPropertiesEXT properties = {
            .sType =
                VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
        };
        uint32_t memory_type;
        status = repack_shared.get_image_modifier(repack_shared.device,
                                                  slot->image, &properties);
        if (status != VK_SUCCESS) goto vk_fail;
        if (properties.drmFormatModifier != 0) {
            errno = EPROTO;
            goto fail;
        }
        vkGetImageMemoryRequirements(repack_shared.device, slot->image,
                                     &requirements);
        if (requirements.size == 0 ||
            requirements.size > ADVC_MAX_DMABUF_OBJECT_BYTES) {
            errno = EOVERFLOW;
            goto fail;
        }
        memory_type = find_memory_type(repack_shared.physical,
                                       requirements.memoryTypeBits, 0, NULL);
        if (memory_type == UINT32_MAX) {
            errno = ENOTSUP;
            goto fail;
        }
        {
            VkMemoryDedicatedAllocateInfo dedicated = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                .image = slot->image,
            };
            VkExportMemoryAllocateInfo export = {
                .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                .pNext = &dedicated,
                .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
            };
            VkMemoryAllocateInfo allocation = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .pNext = &export,
                .allocationSize = requirements.size,
                .memoryTypeIndex = memory_type,
            };
            status = vkAllocateMemory(repack_shared.device, &allocation, NULL,
                                      &slot->memory);
            if (status != VK_SUCCESS) goto vk_fail;
        }
        status = vkBindImageMemory(repack_shared.device, slot->image,
                                   slot->memory, 0);
        if (status != VK_SUCCESS) goto vk_fail;
        slot->memory_size = requirements.size;
    }
    memset(slot->layouts, 0, sizeof(slot->layouts));
    for (uint32_t i = 0; i < 2; ++i) {
        VkImageSubresource subresource = {
            .aspectMask = i == 0 ?
                VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT :
                VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
            .mipLevel = 0,
            .arrayLayer = 0,
        };
        vkGetImageSubresourceLayout(repack_shared.device, slot->image,
                                    &subresource, &slot->layouts[i]);
        if (slot->layouts[i].rowPitch == 0 ||
            slot->layouts[i].rowPitch > UINT32_MAX ||
            slot->layouts[i].offset >= slot->memory_size ||
            slot->layouts[i].rowPitch >
                slot->memory_size - slot->layouts[i].offset) {
            errno = EPROTO;
            goto fail;
        }
    }
    {
        VkCommandPoolCreateInfo create = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = repack_shared.queue_family,
        };
        VkCommandBufferAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        status = vkCreateCommandPool(repack_shared.device, &create, NULL,
                                     &slot->command_pool);
        if (status != VK_SUCCESS) goto vk_fail;
        allocation.commandPool = slot->command_pool;
        status = vkAllocateCommandBuffers(repack_shared.device, &allocation,
                                          &slot->command_buffer);
        if (status != VK_SUCCESS) goto vk_fail;
    }
    {
        VkSemaphoreCreateInfo acquire = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        VkExportSemaphoreCreateInfo export = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        };
        VkSemaphoreCreateInfo release = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &export,
        };
        VkFenceCreateInfo fence = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        };
        status = vkCreateSemaphore(repack_shared.device, &acquire, NULL,
                                   &slot->source_acquire_semaphore);
        if (status != VK_SUCCESS) goto vk_fail;
        status = vkCreateSemaphore(repack_shared.device, &acquire, NULL,
                                   &slot->destination_acquire_semaphore);
        if (status != VK_SUCCESS) goto vk_fail;
        status = vkCreateSemaphore(repack_shared.device, &release, NULL,
                                   &slot->release_semaphore);
        if (status != VK_SUCCESS) goto vk_fail;
        status = vkCreateFence(repack_shared.device, &fence, NULL,
                               &slot->fence);
        if (status != VK_SUCCESS) goto vk_fail;
    }
    slot->signature = *signature;
    slot->signature_valid = 1;
    slot->pending_release_fence_fd = -1;
    return 0;

vk_fail:
    errno = vk_errno(status);
fail:
    saved_errno = errno == 0 ? EIO : errno;
    destroy_repack_destination_locked(slot);
    errno = saved_errno;
    return -1;
}

static int export_repack_destination_locked(
    const struct repack_destination_slot *destination,
    const struct advc_dmabuf_descriptor *source, uint64_t output_buffer_id,
    struct advc_turnip_linear_repack_result *result) {
    VkMemoryGetFdInfoKHR get_fd = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = destination->memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    int memory_fd = -1;
    VkResult status = repack_shared.get_memory_fd(repack_shared.device,
                                                  &get_fd, &memory_fd);
    if (status != VK_SUCCESS || memory_fd < 0) {
        if (memory_fd >= 0) close(memory_fd);
        errno = status == VK_SUCCESS ? EIO : vk_errno(status);
        return -1;
    }
    if (set_cloexec(memory_fd) < 0) {
        int saved_errno = errno;
        close(memory_fd);
        errno = saved_errno;
        return -1;
    }
    result->descriptor.buffer_id = output_buffer_id;
    result->descriptor.width = source->width;
    result->descriptor.height = source->height;
    result->descriptor.drm_fourcc = source->drm_fourcc;
    result->descriptor.explicit_flags = ADVC_DMABUF_EXPLICIT_ALL;
    result->descriptor.drm_modifier = 0;
    result->descriptor.crop_left = source->crop_left;
    result->descriptor.crop_top = source->crop_top;
    result->descriptor.crop_width = source->crop_width;
    result->descriptor.crop_height = source->crop_height;
    result->descriptor.object_count = 1;
    result->descriptor.plane_count = 2;
    result->descriptor.color_primaries = source->color_primaries;
    result->descriptor.color_transfer = source->color_transfer;
    result->descriptor.color_matrix = source->color_matrix;
    result->descriptor.color_range = source->color_range;
    result->descriptor.chroma_horizontal = source->chroma_horizontal;
    result->descriptor.chroma_vertical = source->chroma_vertical;
    result->descriptor.objects[0].fd = memory_fd;
    result->descriptor.objects[0].size = destination->memory_size;
    result->descriptor.planes[0].object_index = 0;
    result->descriptor.planes[0].offset = destination->layouts[0].offset;
    result->descriptor.planes[0].pitch =
        (uint32_t)destination->layouts[0].rowPitch;
    result->descriptor.planes[1].object_index = 0;
    result->descriptor.planes[1].offset = destination->layouts[1].offset;
    result->descriptor.planes[1].pitch =
        (uint32_t)destination->layouts[1].rowPitch;
    if (advc_dmabuf_descriptor_validate(&result->descriptor) < 0) {
        advc_dmabuf_descriptor_close(&result->descriptor);
        return -1;
    }
    return 0;
}

void advc_turnip_linear_repack_close(
    struct advc_turnip_linear_repack_result *result) {
    if (result == NULL) return;
    advc_dmabuf_descriptor_close(&result->descriptor);
    if (result->acquire_fence_fd >= 0) close(result->acquire_fence_fd);
    if (result->source_release_fence_fd >= 0)
        close(result->source_release_fence_fd);
    memset(result, 0, sizeof(*result));
    for (uint32_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        result->descriptor.objects[i].fd = -1;
    result->acquire_fence_fd = -1;
    result->source_release_fence_fd = -1;
}

int advc_turnip_linear_repack_pool_reserve(
    struct advc_turnip_linear_repack_pool *pool, uint32_t width,
    uint32_t height, uint64_t reservation_id,
    struct advc_turnip_linear_repack_result *result) {
    struct advc_dmabuf_descriptor metadata;
    struct advc_dmabuf_descriptor allocation_metadata;
    struct advc_repack_descriptor_signature signature;
    struct repack_destination_slot *destination_slot = NULL;
    size_t destination_index = SIZE_MAX;
    uint64_t lease_token = 0;
    int saved_errno = 0;
    int rc = -1;

    if (result == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(result, 0, sizeof(*result));
    for (uint32_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        result->descriptor.objects[i].fd = -1;
    result->acquire_fence_fd = -1;
    result->source_release_fence_fd = -1;
    if (pool == NULL || reservation_id == 0 || width < 16 || width > 8192 ||
        height < 16 || height > 8192 || (width & 1u) != 0 ||
        (height & 1u) != 0) {
        errno = EINVAL;
        return -1;
    }

    memset(&metadata, 0, sizeof(metadata));
    for (uint32_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        metadata.objects[i].fd = -1;
    metadata.buffer_id = reservation_id;
    metadata.width = width;
    metadata.height = height;
    metadata.drm_fourcc = ADVC_DRM_FORMAT_NV12;
    metadata.explicit_flags = ADVC_DMABUF_EXPLICIT_ALL;
    metadata.drm_modifier = 0;
    metadata.crop_width = width;
    metadata.crop_height = height;
    metadata.object_count = 1;
    metadata.plane_count = 2;

    /*
     * The reserve request carries the visible surface dimensions, while
     * Android AVC output images use macroblock-aligned coded dimensions
     * (for example 1920x1080 is exported by the producer as 1920x1088).
     * Allocate the backing Vulkan image at the coded-size lower bound, but
     * export the originally requested visible descriptor.  This preserves
     * the already returned dma-buf identity and its visible crop while
     * leaving enough image extent for the later GPU-only UBWC copy.
     */
    allocation_metadata = metadata;
    allocation_metadata.width = (width + 15u) & ~15u;
    allocation_metadata.height = (height + 15u) & ~15u;
    allocation_metadata.crop_width = allocation_metadata.width;
    allocation_metadata.crop_height = allocation_metadata.height;
    advc_repack_descriptor_signature_make(&allocation_metadata, &signature);

    pthread_mutex_lock(&repack_shared.mutex);
    if (attach_repack_pool_locked(pool) < 0) goto fail;
    if (advc_repack_lease_acquire(
            pool->leases, pool->slot_count, &pool->next_token,
            &destination_index, &lease_token) < 0)
        goto fail;
    destination_slot = &pool->destinations[destination_index];
    if (!destination_slot->signature_valid ||
        destination_slot->signature.width != width ||
        destination_slot->signature.height != height) {
        destroy_repack_destination_locked(destination_slot);
        if (create_repack_destination_locked(destination_slot, &signature) < 0)
            goto fail;
    }
    if (export_repack_destination_locked(destination_slot, &metadata,
                                         reservation_id, result) < 0)
        goto fail;
    result->lease_token = lease_token;
    memcpy(result->device_name, repack_shared.device_name,
           sizeof(result->device_name));
    rc = 0;
    goto done;

fail:
    saved_errno = errno == 0 ? EIO : errno;
    if (destination_slot != NULL) {
        destroy_repack_destination_locked(destination_slot);
        advc_repack_lease_clear(&pool->leases[destination_index]);
    }
done:
    pthread_mutex_unlock(&repack_shared.mutex);
    if (rc < 0) {
        advc_turnip_linear_repack_close(result);
        errno = saved_errno;
    }
    return rc;
}

static int repack_linear_pooled_internal(
    struct advc_turnip_linear_repack_pool *pool,
    uint64_t requested_lease_token,
    const struct advc_dmabuf_descriptor *source,
    int source_acquire_fence_fd, uint64_t output_buffer_id,
    struct advc_turnip_linear_repack_result *result) {
    struct advc_repack_fd_identity identity;
    struct advc_repack_descriptor_signature signature;
    struct repack_source_slot *source_slot = NULL;
    struct repack_destination_slot *destination_slot = NULL;
    size_t source_index = SIZE_MAX;
    size_t destination_index = SIZE_MAX;
    uint64_t lease_token = 0;
    VkResult status = VK_SUCCESS;
    int imported_source_fd = -1;
    int exported_sync_fd = -1;
    int source_exact = 0;
    int destination_wait_imported = 0;
    int submitted = 0;
    int saved_errno = 0;
    int rc = -1;

    if (result == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(result, 0, sizeof(*result));
    for (uint32_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        result->descriptor.objects[i].fd = -1;
    result->acquire_fence_fd = -1;
    result->source_release_fence_fd = -1;
    if (pool == NULL || source == NULL || output_buffer_id == 0 ||
        source_acquire_fence_fd < -1 ||
        advc_dmabuf_descriptor_validate(source) < 0 ||
        source->drm_fourcc != ADVC_DRM_FORMAT_NV12 ||
        source->drm_modifier != ADVC_QCOM_COMPRESSED ||
        source->object_count != 1 || source->plane_count != 2 ||
        source->planes[0].object_index != 0 ||
        source->planes[1].object_index != 0 ||
        (source->width & 1u) != 0 || (source->height & 1u) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (source_acquire_fence_fd >= 0 &&
        advc_dmabuf_sync_file_validate(source_acquire_fence_fd) < 0)
        return -1;
    if (advc_repack_fd_identity_from_fd(source->objects[0].fd, &identity) < 0)
        return -1;
    advc_repack_descriptor_signature_make(source, &signature);

    pthread_mutex_lock(&repack_shared.mutex);
    if (attach_repack_pool_locked(pool) < 0) goto fail;
    memcpy(result->device_name, repack_shared.device_name,
           sizeof(result->device_name));

    source_index = advc_repack_source_key_select(
        pool->source_keys, pool->slot_count, &identity, &signature,
        &source_exact);
    if (source_index == SIZE_MAX) {
        errno = ENOSPC;
        goto fail;
    }
    source_slot = &pool->sources[source_index];
    if (!source_exact) {
        destroy_repack_source_locked(source_slot);
        memset(&pool->source_keys[source_index], 0,
               sizeof(pool->source_keys[source_index]));
        if (create_repack_source_locked(source_slot, source) < 0)
            goto fail;
        pool->source_keys[source_index].occupied = 1;
        pool->source_keys[source_index].identity = identity;
        pool->source_keys[source_index].signature = signature;
    }
    pool->source_keys[source_index].last_use = ++pool->use_clock;

    if (requested_lease_token != 0) {
        if (advc_repack_lease_find(pool->leases, pool->slot_count,
                                   requested_lease_token,
                                   &destination_index) < 0)
            goto fail;
        lease_token = requested_lease_token;
    } else if (advc_repack_lease_acquire(
                   pool->leases, pool->slot_count, &pool->next_token,
                   &destination_index, &lease_token) < 0) {
        goto fail;
    }
    destination_slot = &pool->destinations[destination_index];
    if (requested_lease_token != 0 &&
        (!destination_slot->signature_valid ||
         destination_slot->signature.width < signature.width ||
         destination_slot->signature.height < signature.height)) {
        errno = EPROTO;
        goto fail;
    }
    if (requested_lease_token == 0 &&
        (!destination_slot->signature_valid ||
         destination_slot->signature.width != signature.width ||
         destination_slot->signature.height != signature.height)) {
        destroy_repack_destination_locked(destination_slot);
        if (create_repack_destination_locked(destination_slot, &signature) < 0)
            goto fail;
    }
    if (export_repack_destination_locked(destination_slot, source,
                                         output_buffer_id, result) < 0)
        goto fail;

    if (destination_slot->submitted) {
        status = vkWaitForFences(repack_shared.device, 1,
                                 &destination_slot->fence, VK_TRUE,
                                 ADVC_IMPORT_TIMEOUT_NS);
        if (status != VK_SUCCESS) goto vk_fail;
        destination_slot->submitted = 0;
    }
    status = vkResetCommandPool(repack_shared.device,
                                destination_slot->command_pool, 0);
    if (status != VK_SUCCESS) goto vk_fail;
    status = vkResetFences(repack_shared.device, 1, &destination_slot->fence);
    if (status != VK_SUCCESS) goto vk_fail;

    if (source_acquire_fence_fd >= 0) {
        imported_source_fd = fcntl(source_acquire_fence_fd,
                                   F_DUPFD_CLOEXEC, 3);
        if (imported_source_fd < 0) goto fail;
        {
            VkImportSemaphoreFdInfoKHR import = {
                .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
                .semaphore = destination_slot->source_acquire_semaphore,
                .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
                .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
                .fd = imported_source_fd,
            };
            status = repack_shared.import_semaphore_fd(repack_shared.device,
                                                       &import);
            if (status != VK_SUCCESS) goto vk_fail;
            imported_source_fd = -1;
        }
    }
    if (destination_slot->pending_release_fence_fd >= 0) {
        VkImportSemaphoreFdInfoKHR import = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
            .semaphore = destination_slot->destination_acquire_semaphore,
            .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
            .fd = destination_slot->pending_release_fence_fd,
        };
        status = repack_shared.import_semaphore_fd(repack_shared.device,
                                                   &import);
        if (status != VK_SUCCESS) goto vk_fail;
        destination_slot->pending_release_fence_fd = -1;
        destination_wait_imported = 1;
    }
    {
        VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        VkImageMemoryBarrier acquire_barriers[2];
        VkImageMemoryBarrier release_barriers[2];
        VkImageCopy copies[2];
        memset(acquire_barriers, 0, sizeof(acquire_barriers));
        memset(release_barriers, 0, sizeof(release_barriers));
        memset(copies, 0, sizeof(copies));
        acquire_barriers[0].sType =
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        acquire_barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        acquire_barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        acquire_barriers[0].newLayout =
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        acquire_barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        acquire_barriers[0].dstQueueFamilyIndex = repack_shared.queue_family;
        acquire_barriers[0].image = source_slot->image;
        acquire_barriers[0].subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        acquire_barriers[0].subresourceRange.levelCount = 1;
        acquire_barriers[0].subresourceRange.layerCount = 1;
        acquire_barriers[1].sType =
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        acquire_barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        acquire_barriers[1].oldLayout = destination_slot->has_content ?
            VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        acquire_barriers[1].newLayout =
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        acquire_barriers[1].srcQueueFamilyIndex =
            destination_slot->has_content ? VK_QUEUE_FAMILY_EXTERNAL :
                                            VK_QUEUE_FAMILY_IGNORED;
        acquire_barriers[1].dstQueueFamilyIndex =
            destination_slot->has_content ? repack_shared.queue_family :
                                            VK_QUEUE_FAMILY_IGNORED;
        acquire_barriers[1].image = destination_slot->image;
        acquire_barriers[1].subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        acquire_barriers[1].subresourceRange.levelCount = 1;
        acquire_barriers[1].subresourceRange.layerCount = 1;

        for (uint32_t i = 0; i < 2; ++i) {
            VkImageAspectFlags aspect = i == 0 ?
                VK_IMAGE_ASPECT_PLANE_0_BIT : VK_IMAGE_ASPECT_PLANE_1_BIT;
            copies[i].srcSubresource.aspectMask = aspect;
            copies[i].srcSubresource.layerCount = 1;
            copies[i].dstSubresource.aspectMask = aspect;
            copies[i].dstSubresource.layerCount = 1;
            copies[i].extent.width = i == 0 ? source->width :
                                                source->width / 2u;
            copies[i].extent.height = i == 0 ? source->height :
                                                 source->height / 2u;
            copies[i].extent.depth = 1;
        }
        release_barriers[0] = acquire_barriers[0];
        release_barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        release_barriers[0].dstAccessMask = 0;
        release_barriers[0].oldLayout =
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        release_barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        release_barriers[0].srcQueueFamilyIndex = repack_shared.queue_family;
        release_barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        release_barriers[1] = acquire_barriers[1];
        release_barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        release_barriers[1].dstAccessMask = 0;
        release_barriers[1].oldLayout =
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        release_barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        release_barriers[1].srcQueueFamilyIndex = repack_shared.queue_family;
        release_barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;

        status = vkBeginCommandBuffer(destination_slot->command_buffer,
                                      &begin);
        if (status != VK_SUCCESS) goto vk_fail;
        vkCmdPipelineBarrier(
            destination_slot->command_buffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2,
            acquire_barriers);
        vkCmdCopyImage(destination_slot->command_buffer, source_slot->image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       destination_slot->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, copies);
        vkCmdPipelineBarrier(
            destination_slot->command_buffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 2,
            release_barriers);
        status = vkEndCommandBuffer(destination_slot->command_buffer);
        if (status != VK_SUCCESS) goto vk_fail;
    }
    {
        VkSemaphore waits[2];
        VkPipelineStageFlags wait_stages[2];
        uint32_t wait_count = 0;
        if (source_acquire_fence_fd >= 0) {
            waits[wait_count] = destination_slot->source_acquire_semaphore;
            wait_stages[wait_count++] = VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
        if (destination_wait_imported) {
            waits[wait_count] =
                destination_slot->destination_acquire_semaphore;
            wait_stages[wait_count++] = VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
        VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = wait_count,
            .pWaitSemaphores = wait_count > 0 ? waits : NULL,
            .pWaitDstStageMask = wait_count > 0 ? wait_stages : NULL,
            .commandBufferCount = 1,
            .pCommandBuffers = &destination_slot->command_buffer,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &destination_slot->release_semaphore,
        };
        VkSemaphoreGetFdInfoKHR get_fd = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
            .semaphore = destination_slot->release_semaphore,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        };
        status = vkQueueSubmit(repack_shared.queue, 1, &submit,
                               destination_slot->fence);
        if (status != VK_SUCCESS) goto vk_fail;
        submitted = 1;
        destination_slot->submitted = 1;
        status = repack_shared.get_semaphore_fd(repack_shared.device, &get_fd,
                                                &exported_sync_fd);
        if (status != VK_SUCCESS || exported_sync_fd < 0) goto vk_fail;
        if (set_cloexec(exported_sync_fd) < 0) goto fail;
        result->acquire_fence_fd = fcntl(exported_sync_fd,
                                         F_DUPFD_CLOEXEC, 3);
        if (result->acquire_fence_fd < 0) goto fail;
        result->source_release_fence_fd = exported_sync_fd;
        exported_sync_fd = -1;
        if (advc_dmabuf_sync_file_validate(result->acquire_fence_fd) < 0 ||
            advc_dmabuf_sync_file_validate(
                result->source_release_fence_fd) < 0)
            goto fail;
        destination_slot->has_content = 1;
    }
    result->lease_token = lease_token;
    rc = 0;
    goto done;

vk_fail:
    errno = vk_errno(status);
fail:
    saved_errno = errno == 0 ? EIO : errno;
done:
    if (imported_source_fd >= 0) close(imported_source_fd);
    if (exported_sync_fd >= 0) close(exported_sync_fd);
    if (rc < 0 && destination_slot != NULL) {
        if (submitted) (void)vkDeviceWaitIdle(repack_shared.device);
        destroy_repack_destination_locked(destination_slot);
        advc_repack_lease_clear(&pool->leases[destination_index]);
    }
    pthread_mutex_unlock(&repack_shared.mutex);
    if (rc < 0) {
        advc_turnip_linear_repack_close(result);
        errno = saved_errno;
    }
    return rc;
}

int advc_turnip_prime_repack_linear_pooled(
    struct advc_turnip_linear_repack_pool *pool,
    const struct advc_dmabuf_descriptor *source,
    int source_acquire_fence_fd, uint64_t output_buffer_id,
    struct advc_turnip_linear_repack_result *result) {
    return repack_linear_pooled_internal(
        pool, 0, source, source_acquire_fence_fd, output_buffer_id, result);
}

int advc_turnip_prime_repack_linear_reserved(
    struct advc_turnip_linear_repack_pool *pool, uint64_t lease_token,
    const struct advc_dmabuf_descriptor *source,
    int source_acquire_fence_fd, uint64_t output_buffer_id,
    struct advc_turnip_linear_repack_result *result) {
    if (lease_token == 0) {
        errno = EINVAL;
        return -1;
    }
    return repack_linear_pooled_internal(
        pool, lease_token, source, source_acquire_fence_fd, output_buffer_id,
        result);
}

int advc_turnip_prime_repack_linear(
    const struct advc_dmabuf_descriptor *source,
    int source_acquire_fence_fd, uint64_t output_buffer_id,
    struct advc_turnip_linear_repack_result *result) {
    struct advc_turnip_linear_repack_pool *pool =
        advc_turnip_linear_repack_pool_create(1);
    int saved_errno;
    int rc;
    if (pool == NULL) return -1;
    rc = advc_turnip_prime_repack_linear_pooled(
        pool, source, source_acquire_fence_fd, output_buffer_id, result);
    saved_errno = errno;
    if (rc == 0) {
        (void)advc_turnip_linear_repack_pool_discard(pool,
                                                     result->lease_token);
        result->lease_token = 0;
    }
    advc_turnip_linear_repack_pool_destroy(pool);
    errno = saved_errno;
    return rc;
}
