#include "encode_surface_vulkan.h"
#include "encode_surface_vulkan_shaders.h"

#if !defined(__ANDROID__)
#error "encode_surface_vulkan.c is Android-only"
#endif

#include <android/native_window.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#define ADVC_DRM_FORMAT_ABGR8888 UINT32_C(0x34324241) /* AB24 */
#define ADVC_DRM_FORMAT_XBGR8888 UINT32_C(0x34324258) /* XB24 */
#define ADVC_DRM_FORMAT_NV12 UINT32_C(0x3231564e)     /* NV12 */
#define ADVC_QCOM_COMPRESSED UINT64_C(0x0500000000000001)
#define ADVC_VK_MAX_SWAP_IMAGES 32u
#define ADVC_VK_FRAME_SLOT_COUNT ADVC_MAX_INFLIGHT_DMABUFS
#define ADVC_VK_IMPORT_CACHE_SIZE ADVC_MAX_REGISTERED_DMABUFS
#define ADVC_VK_INVALID_CACHE_INDEX UINT32_MAX
#define ADVC_VK_ACQUIRE_TIMEOUT_NS UINT64_C(5000000000)

_Static_assert(ADVC_VK_FRAME_SLOT_COUNT > 1u,
               "Vulkan surface ingress requires bounded pipelining");
_Static_assert(ADVC_VK_IMPORT_CACHE_SIZE >= ADVC_VK_FRAME_SLOT_COUNT,
               "every in-flight frame needs a cache reference");

static uint64_t monotonic_now_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return UINT64_MAX;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

struct advc_vk_imported_image {
    VkImage image;
    VkDeviceMemory memory;
};

struct advc_vk_import_cache_entry {
    struct advc_vk_imported_image imported;
    VkImageView nv12_view;
    struct advc_dmabuf_descriptor descriptor;
    struct stat fd_identity;
    uint64_t last_use_serial;
    uint32_t inflight_refs;
    int occupied;
};

struct advc_vk_frame_slot {
    VkCommandBuffer command_buffer;
    VkSemaphore image_available;
    VkSemaphore producer_ready;
    VkSemaphore release_ready;
    VkFence submit_fence;
    VkDescriptorSet descriptor_set;
    uint32_t cache_index;
    int in_flight;
};

struct advc_vk_surface_producer {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat swap_format;
    VkExtent2D extent;
    VkImage *swap_images;
    VkImageView *swap_views;
    VkFramebuffer *framebuffers;
    uint8_t *swap_initialized;
    VkSemaphore *swap_present_ready;
    uint32_t swap_image_count;
    VkCommandPool command_pool;
    struct advc_vk_frame_slot frames[ADVC_VK_FRAME_SLOT_COUNT];
    struct advc_vk_import_cache_entry
        import_cache[ADVC_VK_IMPORT_CACHE_SIZE];
    uint32_t next_frame_slot;
    uint64_t cache_serial;
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties;
    PFN_vkImportSemaphoreFdKHR import_semaphore_fd;
    PFN_vkGetSemaphoreFdKHR get_semaphore_fd;
    VkSamplerYcbcrConversion ycbcr_conversion;
    VkSampler ycbcr_sampler;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorPool descriptor_pool;
    VkPipelineLayout pipeline_layout;
    VkRenderPass render_pass;
    VkPipeline ycbcr_pipeline;
    int ycbcr_ready;
    uint32_t width;
    uint32_t height;
};

static int vk_extension_present(const VkExtensionProperties *properties,
                                uint32_t count, const char *name) {
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(properties[i].extensionName, name) == 0) return 1;
    }
    return 0;
}

static int descriptor_supported(
    const struct advc_vk_surface_producer *producer,
    const struct advc_dmabuf_descriptor *descriptor) {
    const struct advc_dmabuf_plane *plane0;
    const struct advc_dmabuf_plane *plane1;
    const struct advc_dmabuf_object *object0;
    uint64_t plane0_end;
    uint64_t plane1_end;
    if (producer == NULL || descriptor == NULL ||
        advc_dmabuf_descriptor_validate(descriptor) < 0 ||
        (descriptor->drm_modifier != 0 &&
         descriptor->drm_modifier != ADVC_QCOM_COMPRESSED) ||
        descriptor->object_count != 1 ||
        descriptor->crop_left != 0 || descriptor->crop_top != 0 ||
        descriptor->crop_width != producer->width ||
        descriptor->crop_height != producer->height ||
        descriptor->width != producer->width ||
        descriptor->height != producer->height)
        return 0;

    plane0 = &descriptor->planes[0];
    object0 = &descriptor->objects[plane0->object_index];
    if (descriptor->drm_fourcc == ADVC_DRM_FORMAT_ABGR8888 ||
        descriptor->drm_fourcc == ADVC_DRM_FORMAT_XBGR8888) {
        if (descriptor->plane_count != 1 || plane0->object_index != 0 ||
            plane0->pitch < descriptor->width * 4u ||
            (descriptor->color_matrix != ADVC_COLOR_MATRIX_UNSPECIFIED &&
             descriptor->color_matrix != ADVC_COLOR_MATRIX_RGB) ||
            (descriptor->color_range != ADVC_COLOR_RANGE_UNSPECIFIED &&
             descriptor->color_range != ADVC_COLOR_RANGE_FULL) ||
            descriptor->chroma_horizontal != ADVC_CHROMA_SITING_UNSPECIFIED ||
            descriptor->chroma_vertical != ADVC_CHROMA_SITING_UNSPECIFIED)
            return 0;
        plane0_end = plane0->offset +
                     (uint64_t)plane0->pitch * (descriptor->height - 1u) +
                     (uint64_t)descriptor->width * 4u;
        return plane0_end >= plane0->offset && plane0_end <= object0->size;
    }

    if (!producer->ycbcr_ready ||
        descriptor->drm_fourcc != ADVC_DRM_FORMAT_NV12 ||
        descriptor->plane_count != 2 || (descriptor->width & 1u) != 0 ||
        (descriptor->height & 1u) != 0 ||
        descriptor->color_primaries != ADVC_COLOR_PRIMARIES_BT709 ||
        descriptor->color_transfer != ADVC_COLOR_TRANSFER_BT709 ||
        descriptor->color_matrix != ADVC_COLOR_MATRIX_BT709 ||
        descriptor->color_range != ADVC_COLOR_RANGE_LIMITED ||
        descriptor->chroma_horizontal != ADVC_CHROMA_SITING_MIDPOINT ||
        descriptor->chroma_vertical != ADVC_CHROMA_SITING_MIDPOINT)
        return 0;
    plane1 = &descriptor->planes[1];
    if (plane0->object_index != 0 || plane1->object_index != 0 ||
        plane0->pitch == 0 || plane1->pitch == 0)
        return 0;
    if (descriptor->drm_modifier == ADVC_QCOM_COMPRESSED) {
        const char *validation =
            getenv("ADVC_ENCODE_QCOM_IMPORT_VALIDATION");
        return validation != NULL &&
               strcmp(validation,
                      "validated-turnip-qcom-nv12-surface-v1") == 0 &&
               plane0->offset < object0->size &&
               plane1->offset > plane0->offset &&
               plane1->offset < object0->size;
    }
    if (plane0->pitch < descriptor->width ||
        plane1->pitch < descriptor->width)
        return 0;
    plane0_end = plane0->offset +
                 (uint64_t)plane0->pitch * (descriptor->height - 1u) +
                 descriptor->width;
    plane1_end = plane1->offset +
                 (uint64_t)plane1->pitch * (descriptor->height / 2u - 1u) +
                 descriptor->width;
    return plane0_end >= plane0->offset && plane1_end >= plane1->offset &&
           plane0_end <= object0->size && plane1_end <= object0->size &&
           plane1->offset >= plane0_end;
}

static uint32_t choose_memory_type(struct advc_vk_surface_producer *producer,
                                   uint32_t type_bits) {
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(producer->physical_device, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((type_bits & (UINT32_C(1) << i)) != 0) return i;
    }
    return UINT32_MAX;
}

static int import_image(struct advc_vk_surface_producer *producer,
                        const struct advc_dmabuf_descriptor *descriptor,
                        struct advc_vk_imported_image *imported) {
    VkSubresourceLayout plane_layouts[2];
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info;
    VkExternalMemoryImageCreateInfo external_info;
    VkImageCreateInfo image_info;
    VkMemoryRequirements requirements;
    VkMemoryFdPropertiesKHR fd_properties;
    VkImportMemoryFdInfoKHR import_info;
    VkMemoryAllocateInfo allocation_info;
    uint32_t memory_type;
    int imported_fd = -1;
    const char *stage = "create-image";
    if (imported == NULL || !descriptor_supported(producer, descriptor))
        return -1;
    memset(imported, 0, sizeof(*imported));
    memset(plane_layouts, 0, sizeof(plane_layouts));
    plane_layouts[0].offset = descriptor->planes[0].offset;
    plane_layouts[0].size = descriptor->drm_fourcc == ADVC_DRM_FORMAT_NV12
                                ? descriptor->planes[1].offset -
                                      descriptor->planes[0].offset
                                : descriptor->objects[0].size -
                                      plane_layouts[0].offset;
    plane_layouts[0].rowPitch = descriptor->planes[0].pitch;
    if (descriptor->drm_fourcc == ADVC_DRM_FORMAT_NV12) {
        plane_layouts[1].offset = descriptor->planes[1].offset;
        plane_layouts[1].size = descriptor->objects[0].size -
                                plane_layouts[1].offset;
        plane_layouts[1].rowPitch = descriptor->planes[1].pitch;
    }
    memset(&modifier_info, 0, sizeof(modifier_info));
    modifier_info.sType =
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    modifier_info.drmFormatModifier = descriptor->drm_modifier;
    modifier_info.drmFormatModifierPlaneCount = descriptor->plane_count;
    modifier_info.pPlaneLayouts = plane_layouts;
    memset(&external_info, 0, sizeof(external_info));
    external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external_info.pNext = &modifier_info;
    external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    memset(&image_info, 0, sizeof(image_info));
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext = &external_info;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = descriptor->drm_fourcc == ADVC_DRM_FORMAT_NV12
                            ? VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
                            : VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent.width = descriptor->width;
    image_info.extent.height = descriptor->height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    image_info.usage = descriptor->drm_fourcc == ADVC_DRM_FORMAT_NV12
                           ? VK_IMAGE_USAGE_SAMPLED_BIT
                           : VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(producer->device, &image_info, NULL, &imported->image) !=
        VK_SUCCESS)
        goto fail;
    vkGetImageMemoryRequirements(producer->device, imported->image,
                                 &requirements);
    memset(&fd_properties, 0, sizeof(fd_properties));
    fd_properties.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    stage = "memory-fd-properties";
    if (producer->get_memory_fd_properties(
            producer->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
            descriptor->objects[0].fd, &fd_properties) != VK_SUCCESS)
        goto fail;
    memory_type = choose_memory_type(
        producer, requirements.memoryTypeBits & fd_properties.memoryTypeBits);
    stage = "memory-type-or-size";
    if (memory_type == UINT32_MAX ||
        descriptor->objects[0].size < requirements.size)
        goto fail;
    stage = "dup-dmabuf";
    imported_fd = fcntl(descriptor->objects[0].fd, F_DUPFD_CLOEXEC, 0);
    if (imported_fd < 0) goto fail;
    memset(&import_info, 0, sizeof(import_info));
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import_info.fd = imported_fd;
    memset(&allocation_info, 0, sizeof(allocation_info));
    allocation_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation_info.pNext = &import_info;
    allocation_info.allocationSize = descriptor->objects[0].size;
    allocation_info.memoryTypeIndex = memory_type;
    stage = "allocate-imported-memory";
    if (vkAllocateMemory(producer->device, &allocation_info, NULL,
                         &imported->memory) != VK_SUCCESS)
        goto fail;
    imported_fd = -1; /* Vulkan owns the fd after successful import. */
    stage = "bind-imported-memory";
    if (vkBindImageMemory(producer->device, imported->image,
                          imported->memory, 0) != VK_SUCCESS)
        goto fail;
    return 0;
fail:
    if (getenv("ADVC_DEBUG") != NULL)
        fprintf(stderr, "advc-surface-vulkan: import failed stage=%s\n", stage);
    if (imported_fd >= 0) close(imported_fd);
    if (imported->image != VK_NULL_HANDLE)
        vkDestroyImage(producer->device, imported->image, NULL);
    if (imported->memory != VK_NULL_HANDLE)
        vkFreeMemory(producer->device, imported->memory, NULL);
    memset(imported, 0, sizeof(*imported));
    return -1;
}

static VkImageView create_nv12_view(
    struct advc_vk_surface_producer *producer,
    const struct advc_vk_imported_image *imported) {
    VkSamplerYcbcrConversionInfo conversion_info;
    VkImageViewCreateInfo view_info;
    VkImageView view = VK_NULL_HANDLE;
    if (producer == NULL || imported == NULL || !producer->ycbcr_ready ||
        imported->image == VK_NULL_HANDLE)
        return VK_NULL_HANDLE;
    memset(&conversion_info, 0, sizeof(conversion_info));
    conversion_info.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    conversion_info.conversion = producer->ycbcr_conversion;
    memset(&view_info, 0, sizeof(view_info));
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.pNext = &conversion_info;
    view_info.image = imported->image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(producer->device, &view_info, NULL, &view) !=
        VK_SUCCESS)
        return VK_NULL_HANDLE;
    return view;
}

static void destroy_imported(struct advc_vk_surface_producer *producer,
                             struct advc_vk_imported_image *imported) {
    if (imported->image != VK_NULL_HANDLE)
        vkDestroyImage(producer->device, imported->image, NULL);
    if (imported->memory != VK_NULL_HANDLE)
        vkFreeMemory(producer->device, imported->memory, NULL);
    memset(imported, 0, sizeof(*imported));
}

static int descriptor_metadata_equal(
    const struct advc_dmabuf_descriptor *left,
    const struct advc_dmabuf_descriptor *right) {
    if (left == NULL || right == NULL ||
        left->buffer_id != right->buffer_id || left->width != right->width ||
        left->height != right->height ||
        left->drm_fourcc != right->drm_fourcc ||
        left->explicit_flags != right->explicit_flags ||
        left->drm_modifier != right->drm_modifier ||
        left->crop_left != right->crop_left ||
        left->crop_top != right->crop_top ||
        left->crop_width != right->crop_width ||
        left->crop_height != right->crop_height ||
        left->object_count != right->object_count ||
        left->plane_count != right->plane_count ||
        left->color_primaries != right->color_primaries ||
        left->color_transfer != right->color_transfer ||
        left->color_matrix != right->color_matrix ||
        left->color_range != right->color_range ||
        left->chroma_horizontal != right->chroma_horizontal ||
        left->chroma_vertical != right->chroma_vertical)
        return 0;
    for (uint32_t i = 0; i < left->object_count; ++i) {
        if (left->objects[i].size != right->objects[i].size) return 0;
    }
    for (uint32_t i = 0; i < left->plane_count; ++i) {
        if (left->planes[i].object_index != right->planes[i].object_index ||
            left->planes[i].offset != right->planes[i].offset ||
            left->planes[i].pitch != right->planes[i].pitch)
            return 0;
    }
    return 1;
}

static int fd_identity_equal(const struct stat *left,
                             const struct stat *right) {
    return left != NULL && right != NULL && left->st_dev == right->st_dev &&
           left->st_ino == right->st_ino && left->st_rdev == right->st_rdev &&
           left->st_size == right->st_size;
}

static void copy_descriptor_metadata(
    struct advc_dmabuf_descriptor *destination,
    const struct advc_dmabuf_descriptor *source) {
    *destination = *source;
    for (uint32_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        destination->objects[i].fd = -1;
}

static void destroy_import_cache_entry(
    struct advc_vk_surface_producer *producer,
    struct advc_vk_import_cache_entry *entry) {
    if (producer == NULL || entry == NULL) return;
    if (entry->nv12_view != VK_NULL_HANDLE)
        vkDestroyImageView(producer->device, entry->nv12_view, NULL);
    destroy_imported(producer, &entry->imported);
    memset(entry, 0, sizeof(*entry));
}

static void destroy_import_cache(struct advc_vk_surface_producer *producer) {
    if (producer == NULL || producer->device == VK_NULL_HANDLE) return;
    for (uint32_t i = 0; i < ADVC_VK_IMPORT_CACHE_SIZE; ++i)
        destroy_import_cache_entry(producer, &producer->import_cache[i]);
}

static int get_cached_import(
    struct advc_vk_surface_producer *producer,
    const struct advc_dmabuf_descriptor *descriptor, uint32_t *cache_index) {
    struct advc_vk_imported_image imported;
    struct stat identity;
    VkImageView nv12_view = VK_NULL_HANDLE;
    uint32_t target = ADVC_VK_INVALID_CACHE_INDEX;
    uint64_t oldest_serial = UINT64_MAX;
    if (producer == NULL || descriptor == NULL || cache_index == NULL ||
        !descriptor_supported(producer, descriptor) ||
        fstat(descriptor->objects[0].fd, &identity) < 0)
        return -1;
    *cache_index = ADVC_VK_INVALID_CACHE_INDEX;

    for (uint32_t i = 0; i < ADVC_VK_IMPORT_CACHE_SIZE; ++i) {
        struct advc_vk_import_cache_entry *entry = &producer->import_cache[i];
        if (!entry->occupied || entry->descriptor.buffer_id != descriptor->buffer_id)
            continue;
        if (descriptor_metadata_equal(&entry->descriptor, descriptor) &&
            fd_identity_equal(&entry->fd_identity, &identity)) {
            entry->last_use_serial = ++producer->cache_serial;
            *cache_index = i;
            return 0;
        }
        /* A live buffer ID must never be rebound to different storage. */
        if (entry->inflight_refs != 0) return -1;
        target = i;
        break;
    }
    if (target == ADVC_VK_INVALID_CACHE_INDEX) {
        for (uint32_t i = 0; i < ADVC_VK_IMPORT_CACHE_SIZE; ++i) {
            struct advc_vk_import_cache_entry *entry = &producer->import_cache[i];
            if (!entry->occupied) {
                target = i;
                break;
            }
            if (entry->inflight_refs == 0 && entry->last_use_serial < oldest_serial) {
                target = i;
                oldest_serial = entry->last_use_serial;
            }
        }
    }
    if (target == ADVC_VK_INVALID_CACHE_INDEX) return -1;

    memset(&imported, 0, sizeof(imported));
    if (import_image(producer, descriptor, &imported) < 0) return -1;
    if (descriptor->drm_fourcc == ADVC_DRM_FORMAT_NV12) {
        nv12_view = create_nv12_view(producer, &imported);
        if (nv12_view == VK_NULL_HANDLE) {
            destroy_imported(producer, &imported);
            return -1;
        }
    }
    destroy_import_cache_entry(producer, &producer->import_cache[target]);
    producer->import_cache[target].imported = imported;
    producer->import_cache[target].nv12_view = nv12_view;
    copy_descriptor_metadata(&producer->import_cache[target].descriptor,
                             descriptor);
    producer->import_cache[target].fd_identity = identity;
    producer->import_cache[target].last_use_serial = ++producer->cache_serial;
    producer->import_cache[target].occupied = 1;
    *cache_index = target;
    return 0;
}

static int retire_frame_slot(struct advc_vk_surface_producer *producer,
                             struct advc_vk_frame_slot *slot) {
    uint32_t cache_index;
    if (producer == NULL || slot == NULL) return -1;
    if (!slot->in_flight) return 0;
    /*
     * The submit fence proves that the command buffer, acquire semaphore
     * wait, release semaphore signal, descriptor set, and imported image are
     * no longer used by this slot. The presentation semaphore is deliberately
     * not recycled here: it is owned by the swapchain image and can only be
     * reused after that image is acquired again.
     */
    if (vkWaitForFences(producer->device, 1, &slot->submit_fence, VK_TRUE,
                        ADVC_VK_ACQUIRE_TIMEOUT_NS) != VK_SUCCESS)
        return -1;
    cache_index = slot->cache_index;
    if (cache_index >= ADVC_VK_IMPORT_CACHE_SIZE ||
        producer->import_cache[cache_index].inflight_refs == 0)
        return -1;
    --producer->import_cache[cache_index].inflight_refs;
    slot->cache_index = ADVC_VK_INVALID_CACHE_INDEX;
    slot->in_flight = 0;
    return vkResetFences(producer->device, 1, &slot->submit_fence) == VK_SUCCESS
               ? 0
               : -1;
}

static uint32_t choose_composite_alpha(VkCompositeAlphaFlagsKHR supported) {
    static const VkCompositeAlphaFlagBitsKHR choices[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
    };
    for (size_t i = 0; i < sizeof(choices) / sizeof(choices[0]); ++i) {
        if ((supported & choices[i]) != 0) return choices[i];
    }
    return 0;
}

static void destroy_ycbcr_resources(struct advc_vk_surface_producer *producer) {
    if (producer == NULL || producer->device == VK_NULL_HANDLE) return;
    if (producer->ycbcr_pipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(producer->device, producer->ycbcr_pipeline, NULL);
    if (producer->framebuffers != NULL) {
        for (uint32_t i = 0; i < producer->swap_image_count; ++i) {
            if (producer->framebuffers[i] != VK_NULL_HANDLE)
                vkDestroyFramebuffer(producer->device,
                                     producer->framebuffers[i], NULL);
        }
    }
    if (producer->swap_views != NULL) {
        for (uint32_t i = 0; i < producer->swap_image_count; ++i) {
            if (producer->swap_views[i] != VK_NULL_HANDLE)
                vkDestroyImageView(producer->device,
                                   producer->swap_views[i], NULL);
        }
    }
    if (producer->render_pass != VK_NULL_HANDLE)
        vkDestroyRenderPass(producer->device, producer->render_pass, NULL);
    if (producer->pipeline_layout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(producer->device, producer->pipeline_layout, NULL);
    if (producer->descriptor_pool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(producer->device, producer->descriptor_pool, NULL);
    if (producer->descriptor_set_layout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(producer->device,
                                     producer->descriptor_set_layout, NULL);
    if (producer->ycbcr_sampler != VK_NULL_HANDLE)
        vkDestroySampler(producer->device, producer->ycbcr_sampler, NULL);
    if (producer->ycbcr_conversion != VK_NULL_HANDLE)
        vkDestroySamplerYcbcrConversion(producer->device,
                                        producer->ycbcr_conversion, NULL);
    free(producer->framebuffers);
    free(producer->swap_views);
    producer->framebuffers = NULL;
    producer->swap_views = NULL;
    producer->ycbcr_pipeline = VK_NULL_HANDLE;
    producer->render_pass = VK_NULL_HANDLE;
    producer->pipeline_layout = VK_NULL_HANDLE;
    producer->descriptor_pool = VK_NULL_HANDLE;
    producer->descriptor_set_layout = VK_NULL_HANDLE;
    producer->ycbcr_sampler = VK_NULL_HANDLE;
    producer->ycbcr_conversion = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < ADVC_VK_FRAME_SLOT_COUNT; ++i)
        producer->frames[i].descriptor_set = VK_NULL_HANDLE;
    producer->ycbcr_ready = 0;
}

static int create_ycbcr_resources(struct advc_vk_surface_producer *producer) {
    VkSamplerYcbcrConversionCreateInfo conversion_info;
    VkSamplerYcbcrConversionInfo conversion_link;
    VkSamplerCreateInfo sampler_info;
    VkDescriptorSetLayoutBinding binding;
    VkDescriptorSetLayoutCreateInfo set_layout_info;
    VkDescriptorPoolSize pool_size;
    VkDescriptorPoolCreateInfo pool_info;
    VkDescriptorSetAllocateInfo set_allocate_info;
    VkDescriptorSetLayout set_layouts[ADVC_VK_FRAME_SLOT_COUNT];
    VkDescriptorSet descriptor_sets[ADVC_VK_FRAME_SLOT_COUNT];
    VkPipelineLayoutCreateInfo pipeline_layout_info;
    VkAttachmentDescription attachment;
    VkAttachmentReference attachment_reference;
    VkSubpassDescription subpass;
    VkSubpassDependency dependency;
    VkRenderPassCreateInfo render_pass_info;
    VkImageViewCreateInfo view_info;
    VkFramebufferCreateInfo framebuffer_info;
    VkShaderModuleCreateInfo shader_info;
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vertex_input;
    VkPipelineInputAssemblyStateCreateInfo input_assembly;
    VkViewport viewport;
    VkRect2D scissor;
    VkPipelineViewportStateCreateInfo viewport_state;
    VkPipelineRasterizationStateCreateInfo rasterization;
    VkPipelineMultisampleStateCreateInfo multisample;
    VkPipelineColorBlendAttachmentState blend_attachment;
    VkPipelineColorBlendStateCreateInfo blend;
    VkGraphicsPipelineCreateInfo pipeline_info;
    const char *stage = "ycbcr-conversion";

    memset(&conversion_info, 0, sizeof(conversion_info));
    conversion_info.sType =
        VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
    conversion_info.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    conversion_info.ycbcrModel =
        VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    conversion_info.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
    conversion_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    conversion_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    conversion_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    conversion_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    conversion_info.xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
    conversion_info.yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
    conversion_info.chromaFilter = VK_FILTER_NEAREST;
    if (vkCreateSamplerYcbcrConversion(producer->device, &conversion_info, NULL,
                                       &producer->ycbcr_conversion) != VK_SUCCESS)
        goto fail;

    memset(&conversion_link, 0, sizeof(conversion_link));
    conversion_link.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    conversion_link.conversion = producer->ycbcr_conversion;
    memset(&sampler_info, 0, sizeof(sampler_info));
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.pNext = &conversion_link;
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = 0.0f;
    stage = "ycbcr-sampler";
    if (vkCreateSampler(producer->device, &sampler_info, NULL,
                        &producer->ycbcr_sampler) != VK_SUCCESS)
        goto fail;

    memset(&binding, 0, sizeof(binding));
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binding.pImmutableSamplers = &producer->ycbcr_sampler;
    memset(&set_layout_info, 0, sizeof(set_layout_info));
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 1;
    set_layout_info.pBindings = &binding;
    stage = "descriptor-set-layout";
    if (vkCreateDescriptorSetLayout(producer->device, &set_layout_info, NULL,
                                    &producer->descriptor_set_layout) != VK_SUCCESS)
        goto fail;
    memset(&pool_size, 0, sizeof(pool_size));
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    /* Multi-planar conversions may consume up to four pool descriptors/set. */
    pool_size.descriptorCount = 4 * ADVC_VK_FRAME_SLOT_COUNT;
    memset(&pool_info, 0, sizeof(pool_info));
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = ADVC_VK_FRAME_SLOT_COUNT;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    stage = "descriptor-pool";
    if (vkCreateDescriptorPool(producer->device, &pool_info, NULL,
                               &producer->descriptor_pool) != VK_SUCCESS)
        goto fail;
    memset(&set_allocate_info, 0, sizeof(set_allocate_info));
    set_allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_allocate_info.descriptorPool = producer->descriptor_pool;
    for (uint32_t i = 0; i < ADVC_VK_FRAME_SLOT_COUNT; ++i)
        set_layouts[i] = producer->descriptor_set_layout;
    set_allocate_info.descriptorSetCount = ADVC_VK_FRAME_SLOT_COUNT;
    set_allocate_info.pSetLayouts = set_layouts;
    stage = "descriptor-sets";
    if (vkAllocateDescriptorSets(producer->device, &set_allocate_info,
                                 descriptor_sets) != VK_SUCCESS)
        goto fail;
    for (uint32_t i = 0; i < ADVC_VK_FRAME_SLOT_COUNT; ++i)
        producer->frames[i].descriptor_set = descriptor_sets[i];

    memset(&pipeline_layout_info, 0, sizeof(pipeline_layout_info));
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &producer->descriptor_set_layout;
    stage = "pipeline-layout";
    if (vkCreatePipelineLayout(producer->device, &pipeline_layout_info, NULL,
                               &producer->pipeline_layout) != VK_SUCCESS)
        goto fail;
    memset(&attachment, 0, sizeof(attachment));
    attachment.format = producer->swap_format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachment_reference.attachment = 0;
    attachment_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    memset(&subpass, 0, sizeof(subpass));
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &attachment_reference;
    memset(&dependency, 0, sizeof(dependency));
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    memset(&render_pass_info, 0, sizeof(render_pass_info));
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &attachment;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    render_pass_info.dependencyCount = 1;
    render_pass_info.pDependencies = &dependency;
    stage = "render-pass";
    if (vkCreateRenderPass(producer->device, &render_pass_info, NULL,
                           &producer->render_pass) != VK_SUCCESS)
        goto fail;

    producer->swap_views = calloc(producer->swap_image_count,
                                  sizeof(*producer->swap_views));
    producer->framebuffers = calloc(producer->swap_image_count,
                                    sizeof(*producer->framebuffers));
    if (producer->swap_views == NULL || producer->framebuffers == NULL) {
        stage = "swap-view-allocation";
        goto fail;
    }
    memset(&view_info, 0, sizeof(view_info));
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = producer->swap_format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    memset(&framebuffer_info, 0, sizeof(framebuffer_info));
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = producer->render_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.width = producer->width;
    framebuffer_info.height = producer->height;
    framebuffer_info.layers = 1;
    for (uint32_t i = 0; i < producer->swap_image_count; ++i) {
        view_info.image = producer->swap_images[i];
        stage = "swap-image-view";
        if (vkCreateImageView(producer->device, &view_info, NULL,
                              &producer->swap_views[i]) != VK_SUCCESS)
            goto fail;
        framebuffer_info.pAttachments = &producer->swap_views[i];
        stage = "framebuffer";
        if (vkCreateFramebuffer(producer->device, &framebuffer_info, NULL,
                                &producer->framebuffers[i]) != VK_SUCCESS)
            goto fail;
    }

    memset(&shader_info, 0, sizeof(shader_info));
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = sizeof(advc_nv12_vertex_spv);
    shader_info.pCode = advc_nv12_vertex_spv;
    stage = "vertex-shader";
    if (vkCreateShaderModule(producer->device, &shader_info, NULL, &vertex) !=
        VK_SUCCESS)
        goto fail;
    shader_info.codeSize = sizeof(advc_nv12_fragment_spv);
    shader_info.pCode = advc_nv12_fragment_spv;
    stage = "fragment-shader";
    if (vkCreateShaderModule(producer->device, &shader_info, NULL, &fragment) !=
        VK_SUCCESS)
        goto fail;
    memset(stages, 0, sizeof(stages));
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";
    memset(&vertex_input, 0, sizeof(vertex_input));
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    memset(&input_assembly, 0, sizeof(input_assembly));
    input_assembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)producer->width;
    viewport.height = (float)producer->height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    scissor.offset.x = 0;
    scissor.offset.y = 0;
    scissor.extent = producer->extent;
    memset(&viewport_state, 0, sizeof(viewport_state));
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;
    memset(&rasterization, 0, sizeof(rasterization));
    rasterization.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    memset(&multisample, 0, sizeof(multisample));
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&blend_attachment, 0, sizeof(blend_attachment));
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                      VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT |
                                      VK_COLOR_COMPONENT_A_BIT;
    memset(&blend, 0, sizeof(blend));
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    memset(&pipeline_info, 0, sizeof(pipeline_info));
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.layout = producer->pipeline_layout;
    pipeline_info.renderPass = producer->render_pass;
    stage = "graphics-pipeline";
    if (vkCreateGraphicsPipelines(producer->device, VK_NULL_HANDLE, 1,
                                  &pipeline_info, NULL,
                                  &producer->ycbcr_pipeline) != VK_SUCCESS)
        goto fail;
    vkDestroyShaderModule(producer->device, fragment, NULL);
    vkDestroyShaderModule(producer->device, vertex, NULL);
    producer->ycbcr_ready = 1;
    return 0;
fail:
    if (fragment != VK_NULL_HANDLE)
        vkDestroyShaderModule(producer->device, fragment, NULL);
    if (vertex != VK_NULL_HANDLE)
        vkDestroyShaderModule(producer->device, vertex, NULL);
    if (getenv("ADVC_DEBUG") != NULL)
        fprintf(stderr, "advc-surface-vulkan: ycbcr unavailable stage=%s\n",
                stage);
    destroy_ycbcr_resources(producer);
    return -1;
}

static void destroy_producer(struct advc_vk_surface_producer *producer) {
    if (producer == NULL) return;
    if (producer->device != VK_NULL_HANDLE) {
        (void)vkDeviceWaitIdle(producer->device);
        destroy_import_cache(producer);
        destroy_ycbcr_resources(producer);
        for (uint32_t i = 0; i < ADVC_VK_FRAME_SLOT_COUNT; ++i) {
            struct advc_vk_frame_slot *slot = &producer->frames[i];
            if (slot->submit_fence != VK_NULL_HANDLE)
                vkDestroyFence(producer->device, slot->submit_fence, NULL);
            if (slot->release_ready != VK_NULL_HANDLE)
                vkDestroySemaphore(producer->device, slot->release_ready, NULL);
            if (slot->producer_ready != VK_NULL_HANDLE)
                vkDestroySemaphore(producer->device, slot->producer_ready, NULL);
            if (slot->image_available != VK_NULL_HANDLE)
                vkDestroySemaphore(producer->device, slot->image_available, NULL);
        }
        if (producer->swap_present_ready != NULL) {
            for (uint32_t i = 0; i < producer->swap_image_count; ++i) {
                if (producer->swap_present_ready[i] != VK_NULL_HANDLE)
                    vkDestroySemaphore(producer->device,
                                       producer->swap_present_ready[i], NULL);
            }
        }
        if (producer->command_pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(producer->device, producer->command_pool, NULL);
        if (producer->swapchain != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(producer->device, producer->swapchain, NULL);
        vkDestroyDevice(producer->device, NULL);
    }
    if (producer->surface != VK_NULL_HANDLE && producer->instance != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(producer->instance, producer->surface, NULL);
    if (producer->instance != VK_NULL_HANDLE)
        vkDestroyInstance(producer->instance, NULL);
    free(producer->swap_images);
    free(producer->swap_initialized);
    free(producer->swap_present_ready);
}

int advc_vk_surface_producer_create(
    void *native_window, uint32_t width, uint32_t height,
    struct advc_vk_surface_producer **producer_out) {
    static const char *const instance_extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
    };
    static const char *const device_extensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME,
    };
    struct advc_vk_surface_producer *producer = NULL;
    VkApplicationInfo application_info;
    VkInstanceCreateInfo instance_info;
    VkAndroidSurfaceCreateInfoKHR surface_info;
    VkPhysicalDevice devices[8];
    uint32_t device_count = 8;
    VkQueueFamilyProperties *queue_properties = NULL;
    uint32_t queue_count = 0;
    VkExtensionProperties *extensions = NULL;
    uint32_t extension_count = 0;
    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info;
    VkDeviceCreateInfo device_info;
    VkPhysicalDeviceFeatures2 physical_features;
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr_features;
    VkPhysicalDeviceSamplerYcbcrConversionFeatures enabled_ycbcr;
    VkSurfaceCapabilitiesKHR capabilities;
    VkSurfaceFormatKHR formats[32];
    uint32_t format_count = 32;
    VkSurfaceFormatKHR selected_format;
    VkSwapchainCreateInfoKHR swapchain_info;
    uint32_t composite_alpha;
    VkCommandPoolCreateInfo pool_info;
    VkCommandBufferAllocateInfo command_info;
    VkCommandBuffer command_buffers[ADVC_VK_FRAME_SLOT_COUNT];
    VkSemaphoreCreateInfo semaphore_info;
    VkExportSemaphoreCreateInfo export_info;
    VkFenceCreateInfo fence_info;
    uint32_t image_count;
    int selected = 0;
    int color_attachment_supported = 0;
    int ycbcr_enabled = 0;
    const char *stage = "create-instance";

    if (producer_out == NULL || native_window == NULL || width == 0 || height == 0)
        return -1;
    *producer_out = NULL;
    producer = (struct advc_vk_surface_producer *)calloc(1, sizeof(*producer));
    if (producer == NULL) return -1;
    producer->width = width;
    producer->height = height;
    for (uint32_t i = 0; i < ADVC_VK_FRAME_SLOT_COUNT; ++i)
        producer->frames[i].cache_index = ADVC_VK_INVALID_CACHE_INDEX;
    memset(&application_info, 0, sizeof(application_info));
    application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application_info.pApplicationName = "advc-dmabuf";
    application_info.apiVersion = VK_API_VERSION_1_1;
    memset(&instance_info, 0, sizeof(instance_info));
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application_info;
    instance_info.enabledExtensionCount =
        (uint32_t)(sizeof(instance_extensions) / sizeof(instance_extensions[0]));
    instance_info.ppEnabledExtensionNames = instance_extensions;
    if (vkCreateInstance(&instance_info, NULL, &producer->instance) != VK_SUCCESS)
        goto fail;
    memset(&surface_info, 0, sizeof(surface_info));
    surface_info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    surface_info.window = (ANativeWindow *)native_window;
    stage = "android-surface-and-physical-device";
    if (vkCreateAndroidSurfaceKHR(producer->instance, &surface_info, NULL,
                                  &producer->surface) != VK_SUCCESS ||
        vkEnumeratePhysicalDevices(producer->instance, &device_count, devices) !=
            VK_SUCCESS ||
        device_count == 0)
        goto fail;
    stage = "device-extensions-and-present-queue";
    for (uint32_t device_index = 0; device_index < device_count && !selected;
         ++device_index) {
        vkGetPhysicalDeviceQueueFamilyProperties(devices[device_index],
                                                 &queue_count, NULL);
        queue_properties = (VkQueueFamilyProperties *)calloc(
            queue_count, sizeof(*queue_properties));
        if (queue_properties == NULL) goto fail;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[device_index],
                                                 &queue_count,
                                                 queue_properties);
        if (vkEnumerateDeviceExtensionProperties(devices[device_index], NULL,
                                                  &extension_count, NULL) !=
            VK_SUCCESS)
            goto fail;
        extensions = (VkExtensionProperties *)calloc(extension_count,
                                                       sizeof(*extensions));
        if (extensions == NULL) goto fail;
        if (vkEnumerateDeviceExtensionProperties(devices[device_index], NULL,
                                                  &extension_count,
                                                  extensions) != VK_SUCCESS)
            goto fail;
        for (size_t required = 0;
             required < sizeof(device_extensions) / sizeof(device_extensions[0]);
             ++required) {
            if (!vk_extension_present(extensions, extension_count,
                                      device_extensions[required]))
                goto next_device;
        }
        for (uint32_t family = 0; family < queue_count; ++family) {
            VkBool32 present = VK_FALSE;
            if ((queue_properties[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0 ||
                vkGetPhysicalDeviceSurfaceSupportKHR(
                    devices[device_index], family, producer->surface,
                    &present) != VK_SUCCESS || !present)
                continue;
            producer->physical_device = devices[device_index];
            producer->queue_family = family;
            selected = 1;
            break;
        }
next_device:
        free(extensions);
        extensions = NULL;
        free(queue_properties);
        queue_properties = NULL;
    }
    if (!selected) goto fail;
    memset(&ycbcr_features, 0, sizeof(ycbcr_features));
    ycbcr_features.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    memset(&physical_features, 0, sizeof(physical_features));
    physical_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    physical_features.pNext = &ycbcr_features;
    vkGetPhysicalDeviceFeatures2(producer->physical_device, &physical_features);
    memset(&enabled_ycbcr, 0, sizeof(enabled_ycbcr));
    enabled_ycbcr.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    if (ycbcr_features.samplerYcbcrConversion == VK_TRUE) {
        enabled_ycbcr.samplerYcbcrConversion = VK_TRUE;
        ycbcr_enabled = 1;
    }
    memset(&queue_info, 0, sizeof(queue_info));
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = producer->queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;
    memset(&device_info, 0, sizeof(device_info));
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.pNext = ycbcr_enabled ? &enabled_ycbcr : NULL;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount =
        (uint32_t)(sizeof(device_extensions) / sizeof(device_extensions[0]));
    device_info.ppEnabledExtensionNames = device_extensions;
    stage = "create-device";
    if (vkCreateDevice(producer->physical_device, &device_info, NULL,
                       &producer->device) != VK_SUCCESS)
        goto fail;
    vkGetDeviceQueue(producer->device, producer->queue_family, 0, &producer->queue);
    producer->get_memory_fd_properties = (PFN_vkGetMemoryFdPropertiesKHR)
        vkGetDeviceProcAddr(producer->device, "vkGetMemoryFdPropertiesKHR");
    producer->import_semaphore_fd = (PFN_vkImportSemaphoreFdKHR)
        vkGetDeviceProcAddr(producer->device, "vkImportSemaphoreFdKHR");
    producer->get_semaphore_fd = (PFN_vkGetSemaphoreFdKHR)
        vkGetDeviceProcAddr(producer->device, "vkGetSemaphoreFdKHR");
    stage = "external-fd-entrypoints";
    if (producer->get_memory_fd_properties == NULL ||
        producer->import_semaphore_fd == NULL ||
        producer->get_semaphore_fd == NULL)
        goto fail;
    stage = "surface-capabilities";
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(producer->physical_device,
                                                   producer->surface,
                                                   &capabilities) != VK_SUCCESS ||
        (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0 ||
        vkGetPhysicalDeviceSurfaceFormatsKHR(producer->physical_device,
                                             producer->surface, &format_count,
                                             formats) != VK_SUCCESS ||
        format_count == 0)
        goto fail;
    color_attachment_supported =
        (capabilities.supportedUsageFlags &
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    selected_format = formats[0];
    for (uint32_t i = 0; i < format_count; ++i) {
        if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM ||
            formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            selected_format = formats[i];
            break;
        }
    }
    stage = "surface-format";
    if (selected_format.format != VK_FORMAT_R8G8B8A8_UNORM &&
        selected_format.format != VK_FORMAT_B8G8R8A8_UNORM)
        goto fail;
    producer->extent.width = width;
    producer->extent.height = height;
    if (capabilities.currentExtent.width != UINT32_MAX)
        producer->extent = capabilities.currentExtent;
    stage = "surface-extent";
    if (producer->extent.width != width || producer->extent.height != height)
        goto fail;
    composite_alpha = choose_composite_alpha(capabilities.supportedCompositeAlpha);
    if (composite_alpha == 0) goto fail;
    image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount != 0 && image_count > capabilities.maxImageCount)
        image_count = capabilities.maxImageCount;
    memset(&swapchain_info, 0, sizeof(swapchain_info));
    swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchain_info.surface = producer->surface;
    swapchain_info.minImageCount = image_count;
    swapchain_info.imageFormat = selected_format.format;
    swapchain_info.imageColorSpace = selected_format.colorSpace;
    swapchain_info.imageExtent = producer->extent;
    swapchain_info.imageArrayLayers = 1;
    swapchain_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                (color_attachment_supported
                                     ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                     : 0);
    swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_info.preTransform = capabilities.currentTransform;
    swapchain_info.compositeAlpha = (VkCompositeAlphaFlagBitsKHR)composite_alpha;
    swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchain_info.clipped = VK_TRUE;
    stage = "create-swapchain";
    if (vkCreateSwapchainKHR(producer->device, &swapchain_info, NULL,
                             &producer->swapchain) != VK_SUCCESS)
        goto fail;
    producer->swap_format = selected_format.format;
    producer->swap_image_count = 0;
    stage = "count-swapchain-images";
    if (vkGetSwapchainImagesKHR(producer->device, producer->swapchain,
                                &producer->swap_image_count, NULL) != VK_SUCCESS ||
        producer->swap_image_count == 0 ||
        producer->swap_image_count > ADVC_VK_MAX_SWAP_IMAGES)
        goto fail;
    producer->swap_images = (VkImage *)calloc(producer->swap_image_count,
                                               sizeof(*producer->swap_images));
    producer->swap_initialized = (uint8_t *)calloc(
        producer->swap_image_count, sizeof(*producer->swap_initialized));
    producer->swap_present_ready = (VkSemaphore *)calloc(
        producer->swap_image_count, sizeof(*producer->swap_present_ready));
    if (producer->swap_images == NULL || producer->swap_initialized == NULL ||
        producer->swap_present_ready == NULL)
        goto fail;
    stage = "get-swapchain-images";
    if (vkGetSwapchainImagesKHR(producer->device, producer->swapchain,
                                &producer->swap_image_count,
                                producer->swap_images) != VK_SUCCESS)
        goto fail;
    if (ycbcr_enabled && color_attachment_supported)
        (void)create_ycbcr_resources(producer);
    memset(&pool_info, 0, sizeof(pool_info));
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = producer->queue_family;
    stage = "create-command-pool";
    if (vkCreateCommandPool(producer->device, &pool_info, NULL,
                            &producer->command_pool) != VK_SUCCESS)
        goto fail;
    memset(&command_info, 0, sizeof(command_info));
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_info.commandPool = producer->command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = ADVC_VK_FRAME_SLOT_COUNT;
    stage = "allocate-command-buffers";
    if (vkAllocateCommandBuffers(producer->device, &command_info,
                                 command_buffers) != VK_SUCCESS)
        goto fail;
    for (uint32_t i = 0; i < ADVC_VK_FRAME_SLOT_COUNT; ++i)
        producer->frames[i].command_buffer = command_buffers[i];
    memset(&semaphore_info, 0, sizeof(semaphore_info));
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    stage = "create-present-semaphores";
    for (uint32_t i = 0; i < producer->swap_image_count; ++i) {
        if (vkCreateSemaphore(producer->device, &semaphore_info, NULL,
                              &producer->swap_present_ready[i]) != VK_SUCCESS)
            goto fail;
    }
    memset(&export_info, 0, sizeof(export_info));
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    export_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    memset(&fence_info, 0, sizeof(fence_info));
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    stage = "create-frame-slots";
    for (uint32_t i = 0; i < ADVC_VK_FRAME_SLOT_COUNT; ++i) {
        struct advc_vk_frame_slot *slot = &producer->frames[i];
        semaphore_info.pNext = NULL;
        if (vkCreateSemaphore(producer->device, &semaphore_info, NULL,
                              &slot->image_available) != VK_SUCCESS ||
            vkCreateSemaphore(producer->device, &semaphore_info, NULL,
                              &slot->producer_ready) != VK_SUCCESS)
            goto fail;
        semaphore_info.pNext = &export_info;
        if (vkCreateSemaphore(producer->device, &semaphore_info, NULL,
                              &slot->release_ready) != VK_SUCCESS ||
            vkCreateFence(producer->device, &fence_info, NULL,
                          &slot->submit_fence) != VK_SUCCESS)
            goto fail;
    }
    *producer_out = producer;
    return 0;
fail:
    free(extensions);
    free(queue_properties);
    if (getenv("ADVC_DEBUG") != NULL)
        fprintf(stderr, "advc-surface-vulkan: create failed stage=%s\n", stage);
    destroy_producer(producer);
    free(producer);
    return -1;
}

int advc_vk_surface_producer_validate_dmabuf(
    struct advc_vk_surface_producer *producer,
    const struct advc_dmabuf_descriptor *descriptor) {
    uint32_t cache_index;
    return get_cached_import(producer, descriptor, &cache_index);
}

int advc_vk_surface_producer_render_dmabuf(
    struct advc_vk_surface_producer *producer,
    const struct advc_dmabuf_descriptor *descriptor, uint64_t frame_sequence,
    int64_t presentation_time_ns, int acquire_fence_fd,
    int *release_fence_fd) {
    struct advc_vk_frame_slot *slot = NULL;
    struct advc_vk_import_cache_entry *cache_entry = NULL;
    VkImportSemaphoreFdInfoKHR semaphore_import;
    uint32_t cache_index = ADVC_VK_INVALID_CACHE_INDEX;
    uint32_t image_index = 0;
    VkCommandBufferBeginInfo begin_info;
    VkImageMemoryBarrier barriers[2];
    VkImageBlit blit;
    VkDescriptorImageInfo descriptor_image;
    VkWriteDescriptorSet descriptor_write;
    VkRenderPassBeginInfo render_pass_begin;
    VkSemaphore wait_semaphores[2];
    VkPipelineStageFlags wait_stages[2];
    uint32_t wait_count = 1;
    VkSemaphore signal_semaphores[2];
    VkSubmitInfo submit_info;
    VkSemaphoreGetFdInfoKHR get_fd_info;
    VkPresentTimeGOOGLE present_time;
    VkPresentTimesInfoGOOGLE present_times;
    VkPresentInfoKHR present_info;
    VkResult result;
    uint64_t desired_present_time;
    const char *legacy_present_pts;
    int have_producer_wait = 0;
    int status = -1;
    const char *stage = "arguments";
    if (release_fence_fd != NULL) *release_fence_fd = -1;
    memset(&semaphore_import, 0, sizeof(semaphore_import));
    if (producer == NULL || descriptor == NULL || release_fence_fd == NULL ||
        presentation_time_ns < 0 ||
        acquire_fence_fd < -1 || !descriptor_supported(producer, descriptor))
        goto done;
    slot = &producer->frames[producer->next_frame_slot];
    stage = "retire-frame-slot";
    if (retire_frame_slot(producer, slot) < 0) goto done;
    stage = "cache-import-image";
    if (get_cached_import(producer, descriptor, &cache_index) < 0) goto done;
    cache_entry = &producer->import_cache[cache_index];
    if (descriptor->drm_fourcc == ADVC_DRM_FORMAT_NV12) {
        if (cache_entry->nv12_view == VK_NULL_HANDLE ||
            slot->descriptor_set == VK_NULL_HANDLE)
            goto done;
        memset(&descriptor_image, 0, sizeof(descriptor_image));
        descriptor_image.sampler = producer->ycbcr_sampler;
        descriptor_image.imageView = cache_entry->nv12_view;
        descriptor_image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        memset(&descriptor_write, 0, sizeof(descriptor_write));
        descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptor_write.dstSet = slot->descriptor_set;
        descriptor_write.dstBinding = 0;
        descriptor_write.descriptorCount = 1;
        descriptor_write.descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptor_write.pImageInfo = &descriptor_image;
        vkUpdateDescriptorSets(producer->device, 1, &descriptor_write, 0, NULL);
    }
    stage = "acquire-swapchain-image";
    result = vkAcquireNextImageKHR(producer->device, producer->swapchain,
                                   ADVC_VK_ACQUIRE_TIMEOUT_NS,
                                   slot->image_available,
                                   VK_NULL_HANDLE, &image_index);
    if ((result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) ||
        image_index >= producer->swap_image_count)
        goto done;
    /*
     * Delay the temporary SYNC_FD import until the bounded swapchain acquire
     * succeeds. If acquire times out, producer_ready keeps its permanent
     * payload and the caller still owns (and closes) acquire_fence_fd. Once a
     * temporary payload is imported, every error is terminal for this
     * producer; android_codec_backend discards the whole Vulkan session.
     */
    if (acquire_fence_fd >= 0) {
        memset(&semaphore_import, 0, sizeof(semaphore_import));
        semaphore_import.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
        semaphore_import.semaphore = slot->producer_ready;
        semaphore_import.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
        semaphore_import.handleType =
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        semaphore_import.fd = acquire_fence_fd;
        stage = "import-acquire-fence";
        if (producer->import_semaphore_fd(producer->device,
                                          &semaphore_import) != VK_SUCCESS)
            goto done;
        acquire_fence_fd = -1; /* Vulkan consumed it. */
        have_producer_wait = 1;
    }
    stage = "reset-command-buffer";
    if (vkResetCommandBuffer(slot->command_buffer, 0) != VK_SUCCESS)
        goto done;
    memset(&begin_info, 0, sizeof(begin_info));
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    stage = "begin-command-buffer";
    if (vkBeginCommandBuffer(slot->command_buffer, &begin_info) != VK_SUCCESS)
        goto done;
    memset(barriers, 0, sizeof(barriers));
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].newLayout = descriptor->drm_fourcc == ADVC_DRM_FORMAT_NV12
                                ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    barriers[0].dstQueueFamilyIndex = producer->queue_family;
    barriers[0].dstAccessMask = descriptor->drm_fourcc == ADVC_DRM_FORMAT_NV12
                                    ? VK_ACCESS_SHADER_READ_BIT
                                    : VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].image = cache_entry->imported.image;
    barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[0].subresourceRange.levelCount = 1;
    barriers[0].subresourceRange.layerCount = 1;
    if (descriptor->drm_fourcc == ADVC_DRM_FORMAT_NV12) {
        vkCmdPipelineBarrier(slot->command_buffer,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
                             0, NULL, 1, barriers);
        memset(&render_pass_begin, 0, sizeof(render_pass_begin));
        render_pass_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_pass_begin.renderPass = producer->render_pass;
        render_pass_begin.framebuffer = producer->framebuffers[image_index];
        render_pass_begin.renderArea.extent = producer->extent;
        vkCmdBeginRenderPass(slot->command_buffer, &render_pass_begin,
                             VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(slot->command_buffer,
                          VK_PIPELINE_BIND_POINT_GRAPHICS,
                          producer->ycbcr_pipeline);
        vkCmdBindDescriptorSets(slot->command_buffer,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                producer->pipeline_layout, 0, 1,
                                &slot->descriptor_set, 0, NULL);
        vkCmdDraw(slot->command_buffer, 3, 1, 0, 0);
        vkCmdEndRenderPass(slot->command_buffer);
    } else {
        barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[1].oldLayout = producer->swap_initialized[image_index]
                                    ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                    : VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].image = producer->swap_images[image_index];
        barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[1].subresourceRange.levelCount = 1;
        barriers[1].subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(slot->command_buffer,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                             NULL, 2, barriers);
        memset(&blit, 0, sizeof(blit));
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1].x = (int32_t)producer->width;
        blit.srcOffsets[1].y = (int32_t)producer->height;
        blit.srcOffsets[1].z = 1;
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[1].x = (int32_t)producer->width;
        blit.dstOffsets[1].y = (int32_t)producer->height;
        blit.dstOffsets[1].z = 1;
        vkCmdBlitImage(slot->command_buffer, cache_entry->imported.image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       producer->swap_images[image_index],
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_NEAREST);
    }
    barriers[0].oldLayout = descriptor->drm_fourcc == ADVC_DRM_FORMAT_NV12
                                ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].srcQueueFamilyIndex = producer->queue_family;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    barriers[0].srcAccessMask = descriptor->drm_fourcc == ADVC_DRM_FORMAT_NV12
                                    ? VK_ACCESS_SHADER_READ_BIT
                                    : VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].dstAccessMask = 0;
    if (descriptor->drm_fourcc == ADVC_DRM_FORMAT_NV12) {
        vkCmdPipelineBarrier(slot->command_buffer,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
                             0, NULL, 1, barriers);
    } else {
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].dstAccessMask = 0;
        vkCmdPipelineBarrier(slot->command_buffer,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
                             0, NULL, 2, barriers);
    }
    stage = "end-command-buffer";
    if (vkEndCommandBuffer(slot->command_buffer) != VK_SUCCESS) goto done;
    wait_semaphores[0] = slot->image_available;
    wait_stages[0] = descriptor->drm_fourcc == ADVC_DRM_FORMAT_NV12
                         ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                         : VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (have_producer_wait) {
        wait_semaphores[wait_count] = slot->producer_ready;
        wait_stages[wait_count] = descriptor->drm_fourcc == ADVC_DRM_FORMAT_NV12
                                      ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                      : VK_PIPELINE_STAGE_TRANSFER_BIT;
        ++wait_count;
    }
    signal_semaphores[0] = producer->swap_present_ready[image_index];
    signal_semaphores[1] = slot->release_ready;
    memset(&submit_info, 0, sizeof(submit_info));
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = wait_count;
    submit_info.pWaitSemaphores = wait_semaphores;
    submit_info.pWaitDstStageMask = wait_stages;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &slot->command_buffer;
    submit_info.signalSemaphoreCount = 2;
    submit_info.pSignalSemaphores = signal_semaphores;
    stage = "queue-submit";
    if (vkQueueSubmit(producer->queue, 1, &submit_info, slot->submit_fence) !=
        VK_SUCCESS)
        goto done;
    slot->cache_index = cache_index;
    slot->in_flight = 1;
    ++cache_entry->inflight_refs;
    producer->next_frame_slot =
        (producer->next_frame_slot + 1u) % ADVC_VK_FRAME_SLOT_COUNT;
    memset(&get_fd_info, 0, sizeof(get_fd_info));
    get_fd_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    get_fd_info.semaphore = slot->release_ready;
    get_fd_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    stage = "export-release-fence";
    if (producer->get_semaphore_fd(producer->device, &get_fd_info,
                                   release_fence_fd) != VK_SUCCESS ||
        *release_fence_fd < 0)
        goto done;
    memset(&present_time, 0, sizeof(present_time));
    present_time.presentID =
        (uint32_t)(frame_sequence % UINT32_MAX) + UINT32_C(1);
    legacy_present_pts = getenv("ADVC_VULKAN_LEGACY_PRESENT_PTS");
    if (legacy_present_pts != NULL &&
        strcmp(legacy_present_pts, "diagnostic-only-v1") == 0) {
        /*
         * VK_GOOGLE_display_timing consumes an absolute display-clock time,
         * not a media-stream timestamp. This opt-in exists only to reproduce
         * the pre-fix one-frame stall on a bounded diagnostic session.
         */
        desired_present_time = (uint64_t)presentation_time_ns;
    } else {
        uint64_t now_ns = monotonic_now_ns();
        if (now_ns == UINT64_MAX ||
            now_ns > UINT64_MAX - UINT64_C(1000000))
            goto done;
        desired_present_time = now_ns + UINT64_C(1000000);
    }
    present_time.desiredPresentTime = desired_present_time;
    if (getenv("ADVC_DEBUG") != NULL)
        fprintf(stderr,
                "advc-surface-vulkan: present frame=%" PRIu64
                " logical_pts=%lld "
                "desired=%llu timing=%s\n",
                frame_sequence, (long long)presentation_time_ns,
                (unsigned long long)desired_present_time,
                desired_present_time == (uint64_t)presentation_time_ns
                    ? "legacy-logical-diagnostic"
                    : "absolute-monotonic");
    memset(&present_times, 0, sizeof(present_times));
    present_times.sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE;
    present_times.swapchainCount = 1;
    present_times.pTimes = &present_time;
    memset(&present_info, 0, sizeof(present_info));
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.pNext = &present_times;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores =
        &producer->swap_present_ready[image_index];
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &producer->swapchain;
    present_info.pImageIndices = &image_index;
    stage = "queue-present";
    result = vkQueuePresentKHR(producer->queue, &present_info);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) goto done;
    producer->swap_initialized[image_index] = 1;
    status = 0;
done:
    if (status != 0 && getenv("ADVC_DEBUG") != NULL)
        fprintf(stderr, "advc-surface-vulkan: render failed stage=%s\n", stage);
    if (acquire_fence_fd >= 0) close(acquire_fence_fd);
    if (status != 0 && release_fence_fd != NULL && *release_fence_fd >= 0) {
        close(*release_fence_fd);
        *release_fence_fd = -1;
    }
    return status;
}

void advc_vk_surface_producer_destroy(struct advc_vk_surface_producer *producer) {
    if (producer == NULL) return;
    destroy_producer(producer);
    free(producer);
}
