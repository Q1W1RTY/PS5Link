#pragma once

#include <array>
#include <QLibrary>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>

// All Vulkan resources are local to the display device and used on its render thread.
class RtxVsrPass
{
public:
    explicit RtxVsrPass(pl_vulkan device);
    ~RtxVsrPass();

    bool render(pl_renderer renderer, const pl_frame &source, const pl_frame &target,
        const pl_render_params &params, int quality);

private:
    struct Texture
    {
        pl_tex texture = nullptr;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    using CreateFeature = void *(*)(VkInstance, VkPhysicalDevice, VkDevice, PFN_vkGetInstanceProcAddr,
        PFN_vkGetDeviceProcAddr, VkCommandBuffer, const wchar_t *, int *);
    using EvaluateFeature = int (*)(void *, VkCommandBuffer, VkImage, VkImageView, uint32_t, uint32_t,
        VkImage, VkImageView, uint32_t, uint32_t, int);
    using DestroyFeature = void (*)(void *);

    pl_vulkan device;
    pl_renderer preRenderer = nullptr;
    QLibrary bridge;
    CreateFeature createFeature = nullptr;
    EvaluateFeature evaluateFeature = nullptr;
    DestroyFeature destroyFeature = nullptr;
    void *context = nullptr;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, 3> commandBuffers{};
    std::array<uint64_t, 3> slotValues{};
    size_t nextSlot = 0;
    VkQueue queue = VK_NULL_HANDLE;
    Texture input, output;
    pl_vulkan_sem timeline{};
    uint64_t timelineValue = 0;
    bool failed = false;
    bool active = false;

    bool initialize();
    bool resize(Texture &texture, int width, int height);
    void destroy(Texture &texture);
    bool run(int quality);
    bool fail(const char *stage, int code = 0);
};
