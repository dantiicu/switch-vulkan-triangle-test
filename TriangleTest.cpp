/**
 * @file TriangleTest.cpp
 * @brief Draw a rotating triangle with the Switch NVK Vulkan path.
 *
 * The example is the first drawable step after the smoke test:
 * 1. Create a Vulkan instance and VI surface.
 * 2. Pick a graphics queue that can present to that surface.
 * 3. Create the device, swapchain, render pass, pipeline, and vertex buffer.
 * 4. Reuse one command buffer to draw and present frames until + is pressed.
 * 5. Destroy Vulkan objects in reverse ownership order.
 *
 * Build with ./vktriangle.sh.
 */

#include <switch.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// devkitA64's C++ headers may hide the POSIX declaration, but libc provides it.
extern "C" int setenv(const char *name, const char *value, int overwrite);

#define VK_USE_PLATFORM_VI_NN 1
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_vi.h>

extern "C" {
/**
 * Request application memory instead of the small applet heap.
 *
 * NVK setup needs more memory than the default applet profile provides.
 */
u32 __nx_applet_type = AppletType_Application;
size_t __nx_heap_size = 0;
}

/** SPIR-V generated from triangle-test/triangle.vert by the build script. */
alignas(4) static const unsigned char vert_spv[] = {
#include "triangle_vert.h"
};

/** SPIR-V generated from triangle-test/triangle.frag by the build script. */
alignas(4) static const unsigned char frag_spv[] = {
#include "triangle_frag.h"
};

/** Vertex layout consumed by triangle.vert: position.xy and color.rgb. */
struct Vertex {
    float pos[2];
    float color[3];
};

/** Three colored vertices used by the sample triangle. */
static const Vertex triangle_vertices[] = {
    {{ 0.0f, -0.5f}, {1.0f, 0.0f, 0.0f}},
    {{ 0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}},
    {{-0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}},
};

static bool g_console_active = false;

/** Vulkan objects owned by the triangle example. */
struct TriangleContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule vert_module = VK_NULL_HANDLE;
    VkShaderModule frag_module = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkBuffer vertex_buffer = VK_NULL_HANDLE;
    VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
    VkSemaphore acquire_semaphore = VK_NULL_HANDLE;
    VkSemaphore render_semaphore = VK_NULL_HANDLE;
    VkFence render_fence = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family_index = UINT32_MAX;
    VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent = {1280, 720};
    std::vector<VkImage> swapchain_images;
    std::vector<VkImageView> image_views;
    std::vector<VkFramebuffer> framebuffers;
};

/** Return a stable short name for VkResult values printed by examples. */
static const char *
vk_result_str(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    default: return "VK_ERROR_(other)";
    }
}

/** Append one line to the SD-card trace file used for crash-side debugging. */
static void
trace(const char *msg)
{
    std::FILE *f = std::fopen("sdmc:/vktriangle-trace.log", "a");
    if (f) {
        std::fprintf(f, "%s\n", msg);
        std::fclose(f);
    }
}

/** Format and append one trace line. */
static void
tracef(const char *fmt, ...)
{
    char buf[192];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    trace(buf);
}

/** Report a setup failure to both stdout and the trace file. */
static bool
fail(const char *msg)
{
    trace(msg);
    if (g_console_active) {
        std::printf("[FAIL] %s\n", msg);
        consoleUpdate(nullptr);
    }
    return false;
}

/** Report a failed Vulkan call and return false. */
static bool
check_vk(VkResult r, const char *call)
{
    if (r == VK_SUCCESS)
        return true;

    char buf[192];
    std::snprintf(buf, sizeof(buf), "%s -> %s (%d)", call,
                  vk_result_str(r), static_cast<int>(r));
    return fail(buf);
}

/** Enable Mesa/NVK logs on the SD card for device-side debugging. */
static void
configure_logging()
{
    setenv("MESA_VK_WSI_SWITCH_PRESENT_TRACE", "1", 1);
    setenv("MESA_LOG_FILE", "sdmc:/vktriangle-mesa.log", 1);
    setenv("MESA_DEBUG", "1", 1);
    setenv("NVK_DEBUG", "trash_memory", 1);
}

/** True if an extension list contains @p name. */
static bool
has_extension(const std::vector<VkExtensionProperties> &props,
              const char *name)
{
    for (const auto &p : props) {
        if (std::strcmp(p.extensionName, name) == 0)
            return true;
    }

    return false;
}

/** Enumerate global instance extensions, returning an empty list on failure. */
static std::vector<VkExtensionProperties>
enumerate_instance_extensions()
{
    uint32_t count = 0;
    VkResult r =
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    if (!check_vk(r, "vkEnumerateInstanceExtensionProperties(count)"))
        return {};

    std::vector<VkExtensionProperties> props(count);
    if (count > 0) {
        r = vkEnumerateInstanceExtensionProperties(nullptr, &count,
                                                   props.data());
        if (!check_vk(r, "vkEnumerateInstanceExtensionProperties(list)"))
            return {};
    }

    props.resize(count);
    return props;
}

/** Create the Vulkan instance with the Switch surface extensions. */
static bool
create_instance(TriangleContext *ctx)
{
    const std::vector<VkExtensionProperties> instance_exts =
        enumerate_instance_extensions();
    const bool have_surface =
        has_extension(instance_exts, VK_KHR_SURFACE_EXTENSION_NAME);
    const bool have_vi_surface =
        has_extension(instance_exts, VK_NN_VI_SURFACE_EXTENSION_NAME);

    if (!have_surface || !have_vi_surface) {
        std::printf("[X] missing required instance extensions:%s%s\n",
                    have_surface ? "" : " VK_KHR_surface",
                    have_vi_surface ? "" : " VK_NN_vi_surface");
        trace("missing required instance extension");
        return false;
    }

    const char *enabled_exts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_NN_VI_SURFACE_EXTENSION_NAME,
    };

    VkApplicationInfo app = {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "nvk-triangle";
    app.applicationVersion = 1;
    app.pEngineName = "none";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = enabled_exts;

    trace("vkCreateInstance");
    if (!check_vk(vkCreateInstance(&ci, nullptr, &ctx->instance),
                  "vkCreateInstance"))
        return false;

    std::printf("[i] vkCreateInstance -> OK\n");
    return true;
}

/** Print physical devices and choose the first one. */
static bool
pick_physical_device(TriangleContext *ctx)
{
    uint32_t count = 0;
    trace("vkEnumeratePhysicalDevices(count)");
    if (!check_vk(vkEnumeratePhysicalDevices(ctx->instance, &count, nullptr),
                  "vkEnumeratePhysicalDevices(count)"))
        return false;

    if (count == 0)
        return fail("vkEnumeratePhysicalDevices -> 0 devices");

    std::vector<VkPhysicalDevice> devices(count);
    trace("vkEnumeratePhysicalDevices(list)");
    if (!check_vk(vkEnumeratePhysicalDevices(ctx->instance, &count,
                                             devices.data()),
                  "vkEnumeratePhysicalDevices(list)"))
        return false;
    devices.resize(count);

    ctx->physical_device = devices[0];
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(devices[i], &props);
        std::printf("    [%u] %s\n", i, props.deviceName);
    }

    VkPhysicalDeviceProperties props = {};
    vkGetPhysicalDeviceProperties(ctx->physical_device, &props);
    std::printf("[i] GPU: %s\n", props.deviceName);
    return true;
}

/** Create the Switch VI surface from the default NWindow. */
static bool
create_surface(TriangleContext *ctx)
{
    NWindow *nw = nwindowGetDefault();
    if (!nw)
        return fail("nwindowGetDefault() -> null");

    VkViSurfaceCreateInfoNN ci = {};
    ci.sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN;
    ci.window = nw;

    trace("vkCreateViSurfaceNN");
    if (!check_vk(vkCreateViSurfaceNN(ctx->instance, &ci, nullptr,
                                      &ctx->surface),
                  "vkCreateViSurfaceNN"))
        return false;

    trace("vkCreateViSurfaceNN OK");
    return true;
}

/** Pick the first graphics queue family. */
static bool
pick_graphics_queue_family(TriangleContext *ctx)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device, &count,
                                             nullptr);
    if (count == 0)
        return fail("vkGetPhysicalDeviceQueueFamilyProperties -> 0 queues");

    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device, &count,
                                             props.data());

    for (uint32_t i = 0; i < count; i++) {
        const bool graphics =
            (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        tracef("qf[%u] flags=0x%x queues=%u -> %s",
               i, props[i].queueFlags, props[i].queueCount,
               graphics ? "graphics" : "no-graphics");
        if (g_console_active) {
            std::printf("    qf[%u] flags=0x%x queues=%u -> %s\n",
                        i, props[i].queueFlags, props[i].queueCount,
                        graphics ? "graphics" : "no-graphics");
        }

        if (graphics) {
            ctx->queue_family_index = i;
            return true;
        }
    }

    return fail("no graphics queue family");
}

/** Verify that the selected graphics queue can present to the VI surface. */
static bool
verify_present_support(TriangleContext *ctx)
{
    VkBool32 present = VK_FALSE;
    VkResult r = vkGetPhysicalDeviceSurfaceSupportKHR(
        ctx->physical_device, ctx->queue_family_index, ctx->surface, &present);
    if (!check_vk(r, "vkGetPhysicalDeviceSurfaceSupportKHR"))
        return false;

    tracef("qf[%u] present=%s", ctx->queue_family_index,
           present ? "yes" : "no");
    if (!present)
        return fail("selected graphics queue cannot present");

    return true;
}

/** True when the selected physical device advertises VK_KHR_swapchain. */
static bool
has_swapchain_extension(VkPhysicalDevice physical_device)
{
    uint32_t count = 0;
    VkResult r = vkEnumerateDeviceExtensionProperties(
        physical_device, nullptr, &count, nullptr);
    if (!check_vk(r, "vkEnumerateDeviceExtensionProperties(count)"))
        return false;

    std::vector<VkExtensionProperties> exts(count);
    if (count > 0) {
        r = vkEnumerateDeviceExtensionProperties(
            physical_device, nullptr, &count, exts.data());
        if (!check_vk(r, "vkEnumerateDeviceExtensionProperties(list)"))
            return false;
    }

    exts.resize(count);
    return has_extension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
}

/** Create the logical device and fetch the graphics queue. */
static bool
create_device(TriangleContext *ctx)
{
    if (!has_swapchain_extension(ctx->physical_device))
        return fail("VK_KHR_swapchain is not advertised");

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_ci = {};
    queue_ci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_ci.queueFamilyIndex = ctx->queue_family_index;
    queue_ci.queueCount = 1;
    queue_ci.pQueuePriorities = &priority;

    const char *device_exts[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    VkDeviceCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &queue_ci;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = device_exts;

    trace("vkCreateDevice");
    if (!check_vk(vkCreateDevice(ctx->physical_device, &ci, nullptr,
                                 &ctx->device),
                  "vkCreateDevice"))
        return false;

    trace("vkCreateDevice OK");
    vkGetDeviceQueue(ctx->device, ctx->queue_family_index, 0, &ctx->queue);
    if (!ctx->queue)
        return fail("vkGetDeviceQueue -> null");

    if (g_console_active)
        std::printf("[i] Device + Queue -> OK\n");
    return true;
}

/** Prefer FIFO because Vulkan requires it and it maps to display vsync. */
static VkPresentModeKHR
choose_present_mode(const std::vector<VkPresentModeKHR> &modes)
{
    for (VkPresentModeKHR mode : modes) {
        if (mode == VK_PRESENT_MODE_FIFO_KHR)
            return mode;
    }

    return modes.empty() ? VK_PRESENT_MODE_FIFO_KHR : modes[0];
}

/** Pick the first supported composite-alpha mode that does not imply blending. */
static VkCompositeAlphaFlagBitsKHR
choose_composite_alpha(VkCompositeAlphaFlagsKHR supported)
{
    if (supported & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (supported & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
        return VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    if (supported & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
        return VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    if (supported & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR)
        return VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

/** Choose the surface format used by the swapchain and render pass. */
static VkSurfaceFormatKHR
choose_surface_format(const std::vector<VkSurfaceFormatKHR> &formats)
{
    if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
        VkSurfaceFormatKHR chosen = {};
        chosen.format = VK_FORMAT_R8G8B8A8_UNORM;
        chosen.colorSpace = formats[0].colorSpace;
        return chosen;
    }

    return formats[0];
}

/** Create the swapchain and store its images. */
static bool
create_swapchain(TriangleContext *ctx)
{
    VkSurfaceCapabilitiesKHR caps = {};
    if (!check_vk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                      ctx->physical_device, ctx->surface, &caps),
                  "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
        return false;

    uint32_t format_count = 0;
    if (!check_vk(vkGetPhysicalDeviceSurfaceFormatsKHR(
                      ctx->physical_device, ctx->surface, &format_count,
                      nullptr),
                  "vkGetPhysicalDeviceSurfaceFormatsKHR(count)"))
        return false;
    if (format_count == 0)
        return fail("vkGetPhysicalDeviceSurfaceFormatsKHR -> 0 formats");

    std::vector<VkSurfaceFormatKHR> formats(format_count);
    if (!check_vk(vkGetPhysicalDeviceSurfaceFormatsKHR(
                      ctx->physical_device, ctx->surface, &format_count,
                      formats.data()),
                  "vkGetPhysicalDeviceSurfaceFormatsKHR(list)"))
        return false;
    formats.resize(format_count);

    uint32_t mode_count = 0;
    if (!check_vk(vkGetPhysicalDeviceSurfacePresentModesKHR(
                      ctx->physical_device, ctx->surface, &mode_count,
                      nullptr),
                  "vkGetPhysicalDeviceSurfacePresentModesKHR(count)"))
        return false;
    if (mode_count == 0)
        return fail("vkGetPhysicalDeviceSurfacePresentModesKHR -> 0 modes");

    std::vector<VkPresentModeKHR> present_modes(mode_count);
    if (!check_vk(vkGetPhysicalDeviceSurfacePresentModesKHR(
                      ctx->physical_device, ctx->surface, &mode_count,
                      present_modes.data()),
                  "vkGetPhysicalDeviceSurfacePresentModesKHR(list)"))
        return false;
    present_modes.resize(mode_count);

    const VkSurfaceFormatKHR surface_format =
        choose_surface_format(formats);
    ctx->color_format = surface_format.format;
    ctx->color_space = surface_format.colorSpace;
    if (caps.currentExtent.width != UINT32_MAX)
        ctx->extent = caps.currentExtent;

    uint32_t image_count = caps.minImageCount < 2 ? 2 : caps.minImageCount;
    if (caps.maxImageCount != 0 && image_count > caps.maxImageCount)
        image_count = caps.maxImageCount;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VkSwapchainCreateInfoKHR ci = {};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = ctx->surface;
    ci.minImageCount = image_count;
    ci.imageFormat = ctx->color_format;
    ci.imageColorSpace = ctx->color_space;
    ci.imageExtent = ctx->extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = usage;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = choose_composite_alpha(caps.supportedCompositeAlpha);
    ci.presentMode = choose_present_mode(present_modes);
    ci.clipped = VK_TRUE;

    trace("vkCreateSwapchainKHR");
    if (!check_vk(vkCreateSwapchainKHR(ctx->device, &ci, nullptr,
                                       &ctx->swapchain),
                  "vkCreateSwapchainKHR"))
        return false;

    uint32_t actual_count = 0;
    if (!check_vk(vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain,
                                          &actual_count, nullptr),
                  "vkGetSwapchainImagesKHR(count)"))
        return false;
    if (actual_count == 0)
        return fail("vkGetSwapchainImagesKHR -> 0 images");

    ctx->swapchain_images.resize(actual_count);
    if (!check_vk(vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain,
                                          &actual_count,
                                          ctx->swapchain_images.data()),
                  "vkGetSwapchainImagesKHR(list)"))
        return false;
    ctx->swapchain_images.resize(actual_count);

    tracef("swapchain %ux%u images=%u",
           ctx->extent.width, ctx->extent.height, actual_count);
    if (g_console_active) {
        std::printf("[i] Swapchain -> %ux%u, images=%u\n",
                    ctx->extent.width, ctx->extent.height, actual_count);
    }
    return true;
}

/** Create the single-subpass render pass used by the triangle pipeline. */
static bool
create_render_pass(TriangleContext *ctx)
{
    VkAttachmentDescription color = {};
    color.format = ctx->color_format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref = {};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkSubpassDependency dep = {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 1;
    ci.pAttachments = &color;
    ci.subpassCount = 1;
    ci.pSubpasses = &subpass;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;

    trace("vkCreateRenderPass");
    return check_vk(vkCreateRenderPass(ctx->device, &ci, nullptr,
                                       &ctx->render_pass),
                    "vkCreateRenderPass");
}

/** Create one image view and framebuffer per swapchain image. */
static bool
create_framebuffers(TriangleContext *ctx)
{
    ctx->image_views.resize(ctx->swapchain_images.size());
    ctx->framebuffers.resize(ctx->swapchain_images.size());

    for (uint32_t i = 0; i < ctx->swapchain_images.size(); i++) {
        VkImageViewCreateInfo iv_ci = {};
        iv_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        iv_ci.image = ctx->swapchain_images[i];
        iv_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv_ci.format = ctx->color_format;
        iv_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        iv_ci.subresourceRange.baseMipLevel = 0;
        iv_ci.subresourceRange.levelCount = 1;
        iv_ci.subresourceRange.baseArrayLayer = 0;
        iv_ci.subresourceRange.layerCount = 1;
        if (!check_vk(vkCreateImageView(ctx->device, &iv_ci, nullptr,
                                        &ctx->image_views[i]),
                      "vkCreateImageView"))
            return false;

        VkFramebufferCreateInfo fb_ci = {};
        fb_ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_ci.renderPass = ctx->render_pass;
        fb_ci.attachmentCount = 1;
        fb_ci.pAttachments = &ctx->image_views[i];
        fb_ci.width = ctx->extent.width;
        fb_ci.height = ctx->extent.height;
        fb_ci.layers = 1;
        if (!check_vk(vkCreateFramebuffer(ctx->device, &fb_ci, nullptr,
                                          &ctx->framebuffers[i]),
                      "vkCreateFramebuffer"))
            return false;
    }

    trace("framebuffers OK");
    return true;
}

/** Create the shader modules from embedded SPIR-V. */
static bool
create_shader_modules(TriangleContext *ctx)
{
    static_assert(sizeof(vert_spv) % sizeof(uint32_t) == 0,
                  "vertex SPIR-V size must be a multiple of 4");
    static_assert(sizeof(frag_spv) % sizeof(uint32_t) == 0,
                  "fragment SPIR-V size must be a multiple of 4");

    VkShaderModuleCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = sizeof(vert_spv);
    ci.pCode = reinterpret_cast<const uint32_t *>(vert_spv);
    if (!check_vk(vkCreateShaderModule(ctx->device, &ci, nullptr,
                                       &ctx->vert_module),
                  "vkCreateShaderModule(vertex)"))
        return false;

    ci.codeSize = sizeof(frag_spv);
    ci.pCode = reinterpret_cast<const uint32_t *>(frag_spv);
    if (!check_vk(vkCreateShaderModule(ctx->device, &ci, nullptr,
                                       &ctx->frag_module),
                  "vkCreateShaderModule(fragment)"))
        return false;

    trace("shaders OK");
    return true;
}

/** Create the pipeline layout and fixed-function graphics pipeline. */
static bool
create_pipeline(TriangleContext *ctx)
{
    VkPushConstantRange push = {};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.offset = 0;
    push.size = sizeof(float);

    VkPipelineLayoutCreateInfo layout_ci = {};
    layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges = &push;
    if (!check_vk(vkCreatePipelineLayout(ctx->device, &layout_ci, nullptr,
                                         &ctx->pipeline_layout),
                  "vkCreatePipelineLayout"))
        return false;

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = ctx->vert_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ctx->frag_module;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding = {};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2] = {};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = offsetof(Vertex, pos);
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset = offsetof(Vertex, color);

    VkPipelineVertexInputStateCreateInfo vertex_input = {};
    vertex_input.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
    input_assembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(ctx->extent.width);
    viewport.height = static_cast<float>(ctx->extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = ctx->extent;

    VkPipelineViewportStateCreateInfo viewport_state = {};
    viewport_state.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo raster = {};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend_attachment = {};
    blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend = {};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    VkGraphicsPipelineCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vertex_input;
    ci.pInputAssemblyState = &input_assembly;
    ci.pViewportState = &viewport_state;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &multisample;
    ci.pColorBlendState = &blend;
    ci.layout = ctx->pipeline_layout;
    ci.renderPass = ctx->render_pass;
    ci.subpass = 0;

    trace("vkCreateGraphicsPipelines");
    if (!check_vk(vkCreateGraphicsPipelines(ctx->device, VK_NULL_HANDLE, 1,
                                            &ci, nullptr, &ctx->pipeline),
                  "vkCreateGraphicsPipelines"))
        return false;

    trace("pipeline OK");
    return true;
}

/** Find a memory type compatible with @p type_filter and @p flags. */
static bool
find_memory_type(VkPhysicalDevice physical_device, uint32_t type_filter,
                 VkMemoryPropertyFlags flags, uint32_t *out)
{
    VkPhysicalDeviceMemoryProperties props = {};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &props);

    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        const bool type_matches = (type_filter & (1u << i)) != 0;
        const bool flags_match =
            (props.memoryTypes[i].propertyFlags & flags) == flags;
        if (type_matches && flags_match) {
            *out = i;
            return true;
        }
    }

    return fail("no compatible host-visible memory type");
}

/** Create and fill the host-visible vertex buffer. */
static bool
create_vertex_buffer(TriangleContext *ctx)
{
    VkBufferCreateInfo buffer_ci = {};
    buffer_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_ci.size = sizeof(triangle_vertices);
    buffer_ci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    buffer_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check_vk(vkCreateBuffer(ctx->device, &buffer_ci, nullptr,
                                 &ctx->vertex_buffer),
                  "vkCreateBuffer"))
        return false;

    VkMemoryRequirements reqs = {};
    vkGetBufferMemoryRequirements(ctx->device, ctx->vertex_buffer, &reqs);

    uint32_t memory_type = 0;
    if (!find_memory_type(ctx->physical_device, reqs.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &memory_type))
        return false;

    VkMemoryAllocateInfo alloc_ci = {};
    alloc_ci.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_ci.allocationSize = reqs.size;
    alloc_ci.memoryTypeIndex = memory_type;
    if (!check_vk(vkAllocateMemory(ctx->device, &alloc_ci, nullptr,
                                   &ctx->vertex_memory),
                  "vkAllocateMemory(vertex)"))
        return false;
    if (!check_vk(vkBindBufferMemory(ctx->device, ctx->vertex_buffer,
                                     ctx->vertex_memory, 0),
                  "vkBindBufferMemory(vertex)"))
        return false;

    void *data = nullptr;
    if (!check_vk(vkMapMemory(ctx->device, ctx->vertex_memory, 0,
                              sizeof(triangle_vertices), 0, &data),
                  "vkMapMemory(vertex)"))
        return false;
    std::memcpy(data, triangle_vertices, sizeof(triangle_vertices));
    vkUnmapMemory(ctx->device, ctx->vertex_memory);

    trace("vertex buffer OK");
    return true;
}

/** Create reusable command and sync objects for the render loop. */
static bool
create_render_loop_objects(TriangleContext *ctx)
{
    VkCommandPoolCreateInfo pool_ci = {};
    pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_ci.queueFamilyIndex = ctx->queue_family_index;
    pool_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (!check_vk(vkCreateCommandPool(ctx->device, &pool_ci, nullptr,
                                      &ctx->command_pool),
                  "vkCreateCommandPool"))
        return false;

    VkCommandBufferAllocateInfo alloc_ci = {};
    alloc_ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_ci.commandPool = ctx->command_pool;
    alloc_ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_ci.commandBufferCount = 1;
    if (!check_vk(vkAllocateCommandBuffers(ctx->device, &alloc_ci,
                                           &ctx->command_buffer),
                  "vkAllocateCommandBuffers"))
        return false;

    VkSemaphoreCreateInfo sem_ci = {};
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (!check_vk(vkCreateSemaphore(ctx->device, &sem_ci, nullptr,
                                    &ctx->acquire_semaphore),
                  "vkCreateSemaphore(acquire)"))
        return false;
    if (!check_vk(vkCreateSemaphore(ctx->device, &sem_ci, nullptr,
                                    &ctx->render_semaphore),
                  "vkCreateSemaphore(render)"))
        return false;

    VkFenceCreateInfo fence_ci = {};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    return check_vk(vkCreateFence(ctx->device, &fence_ci, nullptr,
                                  &ctx->render_fence),
                    "vkCreateFence(render)");
}

/** Record commands that clear, draw, and transition one swapchain image. */
static bool
record_frame(TriangleContext *ctx, uint32_t image_index, float angle)
{
    if (image_index >= ctx->framebuffers.size())
        return fail("acquired image index is out of range");

    if (!check_vk(vkResetCommandBuffer(ctx->command_buffer, 0),
                  "vkResetCommandBuffer"))
        return false;

    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!check_vk(vkBeginCommandBuffer(ctx->command_buffer, &begin),
                  "vkBeginCommandBuffer"))
        return false;

    VkClearValue clear = {};
    clear.color = {{0.05f, 0.35f, 0.90f, 1.0f}};

    VkRenderPassBeginInfo rp = {};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = ctx->render_pass;
    rp.framebuffer = ctx->framebuffers[image_index];
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = ctx->extent;
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;

    vkCmdBeginRenderPass(ctx->command_buffer, &rp,
                         VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(ctx->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      ctx->pipeline);
    vkCmdPushConstants(ctx->command_buffer, ctx->pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float), &angle);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(ctx->command_buffer, 0, 1, &ctx->vertex_buffer,
                           &offset);
    vkCmdDraw(ctx->command_buffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(ctx->command_buffer);

    return check_vk(vkEndCommandBuffer(ctx->command_buffer),
                    "vkEndCommandBuffer");
}

/** Draw and present frames until the user presses + or Vulkan fails. */
static bool
render_loop(TriangleContext *ctx, PadState *pad)
{
    constexpr float two_pi = 6.28318530718f;
    uint32_t frame = 0;
    float angle = 0.0f;

    trace("setup complete, entering render loop");
    while (appletMainLoop()) {
        padUpdate(pad);
        if (padGetButtonsDown(pad) & HidNpadButton_Plus)
            break;

        tracef("frame %u: wait fence", frame);
        if (!check_vk(vkWaitForFences(ctx->device, 1, &ctx->render_fence,
                                      VK_TRUE, UINT64_MAX),
                      "vkWaitForFences(render)"))
            return false;
        if (!check_vk(vkResetFences(ctx->device, 1, &ctx->render_fence),
                      "vkResetFences(render)"))
            return false;

        uint32_t image_index = 0;
        tracef("frame %u: acquire", frame);
        if (!check_vk(vkAcquireNextImageKHR(ctx->device, ctx->swapchain,
                                            UINT64_MAX,
                                            ctx->acquire_semaphore,
                                            VK_NULL_HANDLE, &image_index),
                      "vkAcquireNextImageKHR"))
            return false;

        if (!record_frame(ctx, image_index, angle))
            return false;

        VkPipelineStageFlags wait_stage =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &ctx->acquire_semaphore;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &ctx->command_buffer;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &ctx->render_semaphore;

        tracef("frame %u: submit", frame);
        if (!check_vk(vkQueueSubmit(ctx->queue, 1, &submit,
                                    ctx->render_fence),
                      "vkQueueSubmit"))
            return false;

        VkPresentInfoKHR present = {};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &ctx->render_semaphore;
        present.swapchainCount = 1;
        present.pSwapchains = &ctx->swapchain;
        present.pImageIndices = &image_index;

        tracef("frame %u: present", frame);
        if (!check_vk(vkQueuePresentKHR(ctx->queue, &present),
                      "vkQueuePresentKHR"))
            return false;

        frame++;
        angle += 0.02f;
        if (angle > two_pi)
            angle -= two_pi;
        if (frame == 1)
            trace("first frame rendered");
    }

    if (ctx->queue)
        vkQueueWaitIdle(ctx->queue);
    trace("render loop done");
    return true;
}

/** Destroy all Vulkan objects held by @p ctx. */
static void
destroy_context(TriangleContext *ctx)
{
    trace("cleanup start");
    if (ctx->device)
        vkDeviceWaitIdle(ctx->device);

    if (ctx->render_fence)
        vkDestroyFence(ctx->device, ctx->render_fence, nullptr);
    if (ctx->render_semaphore)
        vkDestroySemaphore(ctx->device, ctx->render_semaphore, nullptr);
    if (ctx->acquire_semaphore)
        vkDestroySemaphore(ctx->device, ctx->acquire_semaphore, nullptr);
    if (ctx->vertex_buffer)
        vkDestroyBuffer(ctx->device, ctx->vertex_buffer, nullptr);
    if (ctx->vertex_memory)
        vkFreeMemory(ctx->device, ctx->vertex_memory, nullptr);
    if (ctx->command_pool)
        vkDestroyCommandPool(ctx->device, ctx->command_pool, nullptr);
    if (ctx->pipeline)
        vkDestroyPipeline(ctx->device, ctx->pipeline, nullptr);
    if (ctx->pipeline_layout)
        vkDestroyPipelineLayout(ctx->device, ctx->pipeline_layout, nullptr);
    if (ctx->vert_module)
        vkDestroyShaderModule(ctx->device, ctx->vert_module, nullptr);
    if (ctx->frag_module)
        vkDestroyShaderModule(ctx->device, ctx->frag_module, nullptr);
    for (VkFramebuffer fb : ctx->framebuffers) {
        if (fb)
            vkDestroyFramebuffer(ctx->device, fb, nullptr);
    }
    for (VkImageView iv : ctx->image_views) {
        if (iv)
            vkDestroyImageView(ctx->device, iv, nullptr);
    }
    if (ctx->render_pass)
        vkDestroyRenderPass(ctx->device, ctx->render_pass, nullptr);
    if (ctx->swapchain)
        vkDestroySwapchainKHR(ctx->device, ctx->swapchain, nullptr);
    if (ctx->surface)
        vkDestroySurfaceKHR(ctx->instance, ctx->surface, nullptr);
    if (ctx->device)
        vkDestroyDevice(ctx->device, nullptr);
    if (ctx->instance)
        vkDestroyInstance(ctx->instance, nullptr);

    trace("cleanup done");
}

/** Keep setup errors visible when the console has not been released yet. */
static void
wait_for_exit(PadState *pad)
{
    std::printf("\nPress + to exit.\n");
    consoleUpdate(nullptr);

    while (appletMainLoop()) {
        padUpdate(pad);
        if (padGetButtonsDown(pad) & HidNpadButton_Plus)
            break;
        svcSleepThread(16000000ULL);
        consoleUpdate(nullptr);
    }
}

/**
 * Run the triangle renderer.
 *
 * @return 0 after a clean exit, 1 when setup or rendering fails.
 */
int
main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    std::remove("sdmc:/vktriangle-trace.log");
    configure_logging();

    consoleInit(nullptr);
    g_console_active = true;

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeAny(&pad);

    std::printf("=== NVK Triangle Test ===\n\n");
    consoleUpdate(nullptr);
    trace("startup");

    TriangleContext ctx;
    bool ok = create_instance(&ctx);
    if (ok)
        ok = pick_physical_device(&ctx);
    if (ok)
        ok = pick_graphics_queue_family(&ctx);
    if (ok)
        ok = create_device(&ctx);

    if (ok) {
        std::printf("[i] Releasing console for triangle rendering...\n");
        consoleUpdate(nullptr);
        consoleExit(nullptr);
        g_console_active = false;
    }

    if (ok)
        ok = create_surface(&ctx);
    if (ok)
        ok = verify_present_support(&ctx);
    if (ok)
        ok = create_swapchain(&ctx);
    if (ok)
        ok = create_render_pass(&ctx);
    if (ok)
        ok = create_framebuffers(&ctx);
    if (ok)
        ok = create_shader_modules(&ctx);
    if (ok)
        ok = create_pipeline(&ctx);
    if (ok)
        ok = create_vertex_buffer(&ctx);
    if (ok)
        ok = create_render_loop_objects(&ctx);
    if (ok)
        ok = render_loop(&ctx, &pad);

    destroy_context(&ctx);
    if (!ok && g_console_active)
        wait_for_exit(&pad);
    if (g_console_active) {
        consoleExit(nullptr);
        g_console_active = false;
    }

    return ok ? 0 : 1;
}
