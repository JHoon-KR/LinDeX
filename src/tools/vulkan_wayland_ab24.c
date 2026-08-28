// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L
#define VK_USE_PLATFORM_WAYLAND_KHR

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#define ARRAY_LENGTH(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_SWAPCHAIN_IMAGES 8

struct wayland_state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_surface *surface;
    struct xdg_wm_base *wm_base;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    bool configured;
    bool closed;
    int32_t width;
    int32_t height;
};

struct vulkan_state {
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t queue_family;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    uint32_t image_count;
    VkImage images[MAX_SWAPCHAIN_IMAGES];
    VkImageView views[MAX_SWAPCHAIN_IMAGES];
    VkFramebuffer framebuffers[MAX_SWAPCHAIN_IMAGES];
    VkRenderPass render_pass;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffers[MAX_SWAPCHAIN_IMAGES];
    VkSemaphore image_available;
    VkSemaphore render_finished;
    VkFence frame_fence;
};

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static double monotonic_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1000000000.0;
}

static const char *format_name(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
        return "R8G8B8A8_UNORM/AB24";
    case VK_FORMAT_R8G8B8A8_SRGB:
        return "R8G8B8A8_SRGB/AB24";
    case VK_FORMAT_B8G8R8A8_UNORM:
        return "B8G8R8A8_UNORM/XR24";
    case VK_FORMAT_B8G8R8A8_SRGB:
        return "B8G8R8A8_SRGB/XR24";
    default:
        return "other";
    }
}

static int vk_failed(VkResult result, const char *operation)
{
    if (result == VK_SUCCESS)
        return 0;
    fprintf(stderr, "%s failed VkResult=%d\n", operation, result);
    return -1;
}

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base,
                         uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *surface,
                                  uint32_t serial)
{
    struct wayland_state *state = data;
    xdg_surface_ack_configure(surface, serial);
    state->configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height,
                               struct wl_array *states)
{
    struct wayland_state *state = data;
    (void)toplevel;
    (void)states;
    if (width > 0)
        state->width = width;
    if (height > 0)
        state->height = height;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    struct wayland_state *state = data;
    (void)toplevel;
    state->closed = true;
}

static void toplevel_configure_bounds(void *data,
                                      struct xdg_toplevel *toplevel,
                                      int32_t width, int32_t height)
{
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
}

static void toplevel_wm_capabilities(void *data,
                                     struct xdg_toplevel *toplevel,
                                     struct wl_array *capabilities)
{
    (void)data;
    (void)toplevel;
    (void)capabilities;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    struct wayland_state *state = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        uint32_t bind_version = version < 4 ? version : 4;
        state->compositor = wl_registry_bind(registry, name,
                                             &wl_compositor_interface,
                                             bind_version);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        uint32_t bind_version = version < 6 ? version : 6;
        state->wm_base = wl_registry_bind(registry, name,
                                          &xdg_wm_base_interface,
                                          bind_version);
        xdg_wm_base_add_listener(state->wm_base, &wm_base_listener, state);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static int wayland_create(struct wayland_state *state)
{
    memset(state, 0, sizeof(*state));
    state->display = wl_display_connect(NULL);
    if (!state->display) {
        fprintf(stderr, "wl_display_connect failed: %s\n", strerror(errno));
        return -1;
    }
    state->registry = wl_display_get_registry(state->display);
    wl_registry_add_listener(state->registry, &registry_listener, state);
    if (wl_display_roundtrip(state->display) < 0 ||
        !state->compositor || !state->wm_base) {
        fprintf(stderr, "required Wayland globals are unavailable\n");
        return -1;
    }
    state->surface = wl_compositor_create_surface(state->compositor);
    state->xdg_surface = xdg_wm_base_get_xdg_surface(state->wm_base,
                                                     state->surface);
    xdg_surface_add_listener(state->xdg_surface, &xdg_surface_listener, state);
    state->toplevel = xdg_surface_get_toplevel(state->xdg_surface);
    xdg_toplevel_add_listener(state->toplevel, &toplevel_listener, state);
    xdg_toplevel_set_title(state->toplevel, "AB24 UBWC Vulkan direct-scanout probe");
    xdg_toplevel_set_app_id(state->toplevel, "org.drmlease.vulkan-ab24");
    xdg_toplevel_set_fullscreen(state->toplevel, NULL);
    wl_surface_commit(state->surface);

    while (!state->configured && !state->closed) {
        if (wl_display_dispatch(state->display) < 0) {
            fprintf(stderr, "Wayland initial configure failed\n");
            return -1;
        }
    }
    printf("Wayland fullscreen configured size=%dx%d\n",
           state->width, state->height);
    return state->closed ? -1 : 0;
}

static void wayland_destroy(struct wayland_state *state)
{
    if (state->toplevel)
        xdg_toplevel_destroy(state->toplevel);
    if (state->xdg_surface)
        xdg_surface_destroy(state->xdg_surface);
    if (state->surface)
        wl_surface_destroy(state->surface);
    if (state->wm_base)
        xdg_wm_base_destroy(state->wm_base);
    if (state->compositor)
        wl_compositor_destroy(state->compositor);
    if (state->registry)
        wl_registry_destroy(state->registry);
    if (state->display)
        wl_display_disconnect(state->display);
    memset(state, 0, sizeof(*state));
}

static int choose_physical_device(struct vulkan_state *vk)
{
    uint32_t count = 0;
    VkPhysicalDevice devices[8];
    if (vk_failed(vkEnumeratePhysicalDevices(vk->instance, &count, NULL),
                  "vkEnumeratePhysicalDevices(count)") != 0 || count == 0)
        return -1;
    if (count > ARRAY_LENGTH(devices))
        count = ARRAY_LENGTH(devices);
    if (vk_failed(vkEnumeratePhysicalDevices(vk->instance, &count, devices),
                  "vkEnumeratePhysicalDevices(list)") != 0)
        return -1;

    for (uint32_t device_index = 0; device_index < count; ++device_index) {
        uint32_t family_count = 0;
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[device_index], &properties);
        vkGetPhysicalDeviceQueueFamilyProperties(devices[device_index],
                                                 &family_count, NULL);
        VkQueueFamilyProperties *families = calloc(family_count,
                                                    sizeof(*families));
        if (!families)
            return -1;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[device_index],
                                                 &family_count, families);
        for (uint32_t family = 0; family < family_count; ++family) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devices[device_index], family,
                                                 vk->surface, &present);
            if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                vk->physical = devices[device_index];
                vk->queue_family = family;
                printf("Vulkan device=%s driver=0x%x queue_family=%u\n",
                       properties.deviceName, properties.driverVersion, family);
                free(families);
                return 0;
            }
        }
        free(families);
    }
    fprintf(stderr, "no Vulkan graphics+Wayland-present queue found\n");
    return -1;
}

static int choose_surface_format(struct vulkan_state *vk,
                                 VkSurfaceFormatKHR *selected)
{
    static const VkFormat preference[] = {
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_R8G8B8A8_SRGB,
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_B8G8R8A8_SRGB,
    };
    uint32_t count = 0;
    VkSurfaceFormatKHR *formats;
    if (vk_failed(vkGetPhysicalDeviceSurfaceFormatsKHR(vk->physical,
                  vk->surface, &count, NULL), "surface formats(count)") != 0 ||
        count == 0)
        return -1;
    formats = calloc(count, sizeof(*formats));
    if (!formats)
        return -1;
    if (vk_failed(vkGetPhysicalDeviceSurfaceFormatsKHR(vk->physical,
                  vk->surface, &count, formats), "surface formats(list)") != 0) {
        free(formats);
        return -1;
    }
    printf("Wayland Vulkan surface formats=%u\n", count);
    for (uint32_t index = 0; index < count; ++index) {
        printf("  format=%d %s colorspace=%d\n", formats[index].format,
               format_name(formats[index].format), formats[index].colorSpace);
    }
    for (uint32_t preferred = 0; preferred < ARRAY_LENGTH(preference); ++preferred) {
        for (uint32_t index = 0; index < count; ++index) {
            if (formats[index].format == preference[preferred] &&
                formats[index].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                *selected = formats[index];
                free(formats);
                printf("SURFACE FORMAT SELECTED: %s (%d)\n",
                       format_name(selected->format), selected->format);
                return 0;
            }
        }
    }
    *selected = formats[0];
    free(formats);
    printf("SURFACE FORMAT FALLBACK: %s (%d) colorspace=%d\n",
           format_name(selected->format), selected->format,
           selected->colorSpace);
    return 0;
}

static int create_swapchain(struct vulkan_state *vk,
                            struct wayland_state *wayland)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR surface_format;
    uint32_t present_count = 0;
    VkPresentModeKHR present_modes[16];
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (vk_failed(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk->physical,
                  vk->surface, &caps), "surface capabilities") != 0 ||
        choose_surface_format(vk, &surface_format) != 0)
        return -1;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk->physical, vk->surface,
                                              &present_count, NULL);
    if (present_count > ARRAY_LENGTH(present_modes))
        present_count = ARRAY_LENGTH(present_modes);
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk->physical, vk->surface,
                                              &present_count, present_modes);
    for (uint32_t index = 0; index < present_count; ++index) {
        if (present_modes[index] == VK_PRESENT_MODE_FIFO_KHR)
            present_mode = present_modes[index];
    }

    vk->extent = caps.currentExtent;
    if (vk->extent.width == UINT32_MAX) {
        uint32_t width = wayland->width > 0 ? (uint32_t)wayland->width : 1280;
        uint32_t height = wayland->height > 0 ? (uint32_t)wayland->height : 720;
        if (width < caps.minImageExtent.width) width = caps.minImageExtent.width;
        if (width > caps.maxImageExtent.width) width = caps.maxImageExtent.width;
        if (height < caps.minImageExtent.height) height = caps.minImageExtent.height;
        if (height > caps.maxImageExtent.height) height = caps.maxImageExtent.height;
        vk->extent = (VkExtent2D){width, height};
    }
    uint32_t desired_images = caps.minImageCount + 1;
    if (caps.maxImageCount && desired_images > caps.maxImageCount)
        desired_images = caps.maxImageCount;
    if (desired_images > MAX_SWAPCHAIN_IMAGES)
        desired_images = MAX_SWAPCHAIN_IMAGES;

    VkSwapchainCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = vk->surface,
        .minImageCount = desired_images,
        .imageFormat = surface_format.format,
        .imageColorSpace = surface_format.colorSpace,
        .imageExtent = vk->extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = present_mode,
        .clipped = VK_TRUE,
    };
    if (!(caps.supportedCompositeAlpha & info.compositeAlpha)) {
        static const VkCompositeAlphaFlagBitsKHR alpha_modes[] = {
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        };
        for (uint32_t index = 0; index < ARRAY_LENGTH(alpha_modes); ++index) {
            if (caps.supportedCompositeAlpha & alpha_modes[index]) {
                info.compositeAlpha = alpha_modes[index];
                break;
            }
        }
    }
    if (vk_failed(vkCreateSwapchainKHR(vk->device, &info, NULL,
                                       &vk->swapchain),
                  "vkCreateSwapchainKHR") != 0)
        return -1;
    vk->format = surface_format.format;
    vkGetSwapchainImagesKHR(vk->device, vk->swapchain,
                            &vk->image_count, NULL);
    if (vk->image_count > MAX_SWAPCHAIN_IMAGES)
        return -1;
    if (vk_failed(vkGetSwapchainImagesKHR(vk->device, vk->swapchain,
                  &vk->image_count, vk->images), "swapchain images") != 0)
        return -1;
    printf("swapchain=%ux%u images=%u present=FIFO format=%s\n",
           vk->extent.width, vk->extent.height, vk->image_count,
           format_name(vk->format));
    return 0;
}

static int create_render_targets(struct vulkan_state *vk)
{
    VkAttachmentDescription attachment = {
        .format = vk->format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    VkAttachmentReference reference = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &reference,
    };
    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    VkRenderPassCreateInfo pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    if (vk_failed(vkCreateRenderPass(vk->device, &pass_info, NULL,
                                     &vk->render_pass),
                  "vkCreateRenderPass") != 0)
        return -1;

    for (uint32_t index = 0; index < vk->image_count; ++index) {
        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = vk->images[index],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = vk->format,
            .components = {
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
            },
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
        };
        if (vk_failed(vkCreateImageView(vk->device, &view_info, NULL,
                                        &vk->views[index]),
                      "vkCreateImageView") != 0)
            return -1;
        VkFramebufferCreateInfo fb_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = vk->render_pass,
            .attachmentCount = 1,
            .pAttachments = &vk->views[index],
            .width = vk->extent.width,
            .height = vk->extent.height,
            .layers = 1,
        };
        if (vk_failed(vkCreateFramebuffer(vk->device, &fb_info, NULL,
                                          &vk->framebuffers[index]),
                      "vkCreateFramebuffer") != 0)
            return -1;
    }

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = vk->queue_family,
    };
    if (vk_failed(vkCreateCommandPool(vk->device, &pool_info, NULL,
                                      &vk->command_pool),
                  "vkCreateCommandPool") != 0)
        return -1;
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = vk->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = vk->image_count,
    };
    if (vk_failed(vkAllocateCommandBuffers(vk->device, &command_info,
                                           vk->command_buffers),
                  "vkAllocateCommandBuffers") != 0)
        return -1;

    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    if (vk_failed(vkCreateSemaphore(vk->device, &semaphore_info, NULL,
                                    &vk->image_available),
                  "image semaphore") != 0 ||
        vk_failed(vkCreateSemaphore(vk->device, &semaphore_info, NULL,
                                    &vk->render_finished),
                  "render semaphore") != 0 ||
        vk_failed(vkCreateFence(vk->device, &fence_info, NULL,
                                &vk->frame_fence),
                  "frame fence") != 0)
        return -1;
    return 0;
}

static int vulkan_create(struct vulkan_state *vk,
                         struct wayland_state *wayland)
{
    memset(vk, 0, sizeof(*vk));
    static const char *instance_extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
    };
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "vulkan-wayland-ab24",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "none",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
        .enabledExtensionCount = ARRAY_LENGTH(instance_extensions),
        .ppEnabledExtensionNames = instance_extensions,
    };
    if (vk_failed(vkCreateInstance(&instance_info, NULL, &vk->instance),
                  "vkCreateInstance") != 0)
        return -1;
    VkWaylandSurfaceCreateInfoKHR surface_info = {
        .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        .display = wayland->display,
        .surface = wayland->surface,
    };
    if (vk_failed(vkCreateWaylandSurfaceKHR(vk->instance, &surface_info,
                                            NULL, &vk->surface),
                  "vkCreateWaylandSurfaceKHR") != 0 ||
        choose_physical_device(vk) != 0)
        return -1;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = vk->queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const char *device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = ARRAY_LENGTH(device_extensions),
        .ppEnabledExtensionNames = device_extensions,
    };
    if (vk_failed(vkCreateDevice(vk->physical, &device_info, NULL,
                                 &vk->device),
                  "vkCreateDevice") != 0)
        return -1;
    vkGetDeviceQueue(vk->device, vk->queue_family, 0, &vk->queue);
    return create_swapchain(vk, wayland) == 0 &&
           create_render_targets(vk) == 0 ? 0 : -1;
}

static void vulkan_destroy(struct vulkan_state *vk)
{
    if (vk->device)
        vkDeviceWaitIdle(vk->device);
    if (vk->device && vk->frame_fence)
        vkDestroyFence(vk->device, vk->frame_fence, NULL);
    if (vk->device && vk->render_finished)
        vkDestroySemaphore(vk->device, vk->render_finished, NULL);
    if (vk->device && vk->image_available)
        vkDestroySemaphore(vk->device, vk->image_available, NULL);
    if (vk->device && vk->command_pool)
        vkDestroyCommandPool(vk->device, vk->command_pool, NULL);
    if (vk->device) {
        for (uint32_t index = 0; index < vk->image_count; ++index) {
            if (vk->framebuffers[index])
                vkDestroyFramebuffer(vk->device, vk->framebuffers[index], NULL);
            if (vk->views[index])
                vkDestroyImageView(vk->device, vk->views[index], NULL);
        }
    }
    if (vk->device && vk->render_pass)
        vkDestroyRenderPass(vk->device, vk->render_pass, NULL);
    if (vk->device && vk->swapchain)
        vkDestroySwapchainKHR(vk->device, vk->swapchain, NULL);
    if (vk->device)
        vkDestroyDevice(vk->device, NULL);
    if (vk->instance && vk->surface)
        vkDestroySurfaceKHR(vk->instance, vk->surface, NULL);
    if (vk->instance)
        vkDestroyInstance(vk->instance, NULL);
    memset(vk, 0, sizeof(*vk));
}

static int draw_frame(struct vulkan_state *vk, uint64_t frame)
{
    uint32_t image_index;
    VkResult result;
    if (vk_failed(vkWaitForFences(vk->device, 1, &vk->frame_fence,
                                  VK_TRUE, UINT64_MAX),
                  "vkWaitForFences") != 0 ||
        vk_failed(vkResetFences(vk->device, 1, &vk->frame_fence),
                  "vkResetFences") != 0)
        return -1;
    result = vkAcquireNextImageKHR(vk->device, vk->swapchain, UINT64_MAX,
                                   vk->image_available, VK_NULL_HANDLE,
                                   &image_index);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        return vk_failed(result, "vkAcquireNextImageKHR");

    VkCommandBuffer command = vk->command_buffers[image_index];
    vkResetCommandBuffer(command, 0);
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vk_failed(vkBeginCommandBuffer(command, &begin_info),
                  "vkBeginCommandBuffer") != 0)
        return -1;
    float phase = (float)(frame % 360) / 360.0f;
    VkClearValue clear = {.color = {{
        0.08f + 0.75f * phase,
        0.10f + 0.45f * (1.0f - phase),
        0.85f - 0.65f * phase,
        1.0f,
    }}};
    VkRenderPassBeginInfo render_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = vk->render_pass,
        .framebuffer = vk->framebuffers[image_index],
        .renderArea = {{0, 0}, vk->extent},
        .clearValueCount = 1,
        .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(command, &render_info, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(command);
    if (vk_failed(vkEndCommandBuffer(command), "vkEndCommandBuffer") != 0)
        return -1;

    VkPipelineStageFlags wait_stage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &vk->image_available,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &command,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &vk->render_finished,
    };
    if (vk_failed(vkQueueSubmit(vk->queue, 1, &submit, vk->frame_fence),
                  "vkQueueSubmit") != 0)
        return -1;
    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &vk->render_finished,
        .swapchainCount = 1,
        .pSwapchains = &vk->swapchain,
        .pImageIndices = &image_index,
    };
    result = vkQueuePresentKHR(vk->queue, &present);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        return vk_failed(result, "vkQueuePresentKHR");
    return 0;
}

int main(int argc, char **argv)
{
    struct wayland_state wayland;
    struct vulkan_state vk;
    int seconds = 20;
    int result = 1;
    if (argc == 3 && strcmp(argv[1], "--seconds") == 0) {
        char *end = NULL;
        long parsed = strtol(argv[2], &end, 10);
        if (!end || *end || parsed < 1 || parsed > 3600) {
            fprintf(stderr, "invalid --seconds value\n");
            return 2;
        }
        seconds = (int)parsed;
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--seconds 1..3600]\n", argv[0]);
        return 2;
    }
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("START Vulkan Wayland AB24/UBWC fullscreen probe seconds=%d\n",
           seconds);
    if (wayland_create(&wayland) != 0)
        goto done;
    if (vulkan_create(&vk, &wayland) != 0)
        goto wayland_done;

    double started = monotonic_seconds();
    double last_report = started;
    uint64_t frame = 0;
    while (!stop_requested && !wayland.closed &&
           monotonic_seconds() - started < seconds) {
        if (draw_frame(&vk, frame) != 0)
            goto vulkan_done;
        ++frame;
        if (wl_display_dispatch_pending(wayland.display) < 0 ||
            wl_display_flush(wayland.display) < 0)
            goto vulkan_done;
        double now = monotonic_seconds();
        if (now - last_report >= 1.0) {
            printf("frames=%" PRIu64 " elapsed=%.2f average_fps=%.2f\n",
                   frame, now - started, frame / (now - started));
            last_report = now;
        }
    }
    result = 0;
    printf("DONE frames=%" PRIu64 " elapsed=%.2f average_fps=%.2f format=%s\n",
           frame, monotonic_seconds() - started,
           frame / (monotonic_seconds() - started), format_name(vk.format));

vulkan_done:
    vulkan_destroy(&vk);
wayland_done:
    wayland_destroy(&wayland);
done:
    return result;
}
