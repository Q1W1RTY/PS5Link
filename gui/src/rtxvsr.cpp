// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <vulkan/vulkan.h>
#include <rtxvsr.h>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QStandardPaths>
#include <cmath>
#include <cstdio>

namespace {
constexpr VkImageLayout inputLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
constexpr VkImageLayout outputLayout = VK_IMAGE_LAYOUT_GENERAL;

pl_frame rgbFrame(pl_tex texture)
{
    pl_frame frame{};
    frame.num_planes = 1;
    frame.planes[0].texture = texture;
    frame.planes[0].components = 3;
    frame.planes[0].component_mapping[0] = 0;
    frame.planes[0].component_mapping[1] = 1;
    frame.planes[0].component_mapping[2] = 2;
    frame.planes[0].component_mapping[3] = -1;
    frame.repr = pl_color_repr_rgb;
    frame.color = pl_color_space_srgb;
    return frame;
}
}

RtxVsrPass::RtxVsrPass(pl_vulkan device) : device(device) {}

RtxVsrPass::~RtxVsrPass()
{
    pl_gpu_finish(device->gpu);
    if(timeline.sem && timelineValue)
    {
        VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wait.semaphoreCount = 1;
        wait.pSemaphores = &timeline.sem;
        wait.pValues = &timelineValue;
        vkWaitSemaphores(device->device, &wait, 2000000000ULL);
    }
    if(context)
        destroyFeature(context);
    if(preRenderer)
        pl_renderer_destroy(&preRenderer);
    destroy(input);
    destroy(output);
    if(pool)
        vkDestroyCommandPool(device->device, pool, nullptr);
    if(timeline.sem)
        pl_vulkan_sem_destroy(device->gpu, &timeline.sem);
}

bool RtxVsrPass::fail(const char *stage, int code)
{
    failed = true;
    std::fprintf(stderr, "[RTX VSR] Regular scaling fallback at %s (0x%08x).\n",
        stage, static_cast<unsigned>(code));
    return false;
}

bool RtxVsrPass::initialize()
{
    bridge.setFileName(QCoreApplication::applicationDirPath() + "/PS5LinkVsr.dll");
    createFeature = reinterpret_cast<CreateFeature>(bridge.resolve("Ps5LinkVsrCreate"));
    evaluateFeature = reinterpret_cast<EvaluateFeature>(bridge.resolve("Ps5LinkVsrEvaluate"));
    destroyFeature = reinterpret_cast<DestroyFeature>(bridge.resolve("Ps5LinkVsrDestroy"));
    if(!createFeature || !evaluateFeature || !destroyFeature)
        return fail("loading the NVIDIA SDK bridge");

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = device->queue_graphics.index;
    if(vkCreateCommandPool(device->device, &poolInfo, nullptr, &pool) != VK_SUCCESS)
        return fail("allocating a Vulkan command pool");

    VkCommandBufferAllocateInfo commandsInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandsInfo.commandPool = pool;
    commandsInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandsInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());
    if(vkAllocateCommandBuffers(device->device, &commandsInfo, commandBuffers.data()) != VK_SUCCESS)
        return fail("allocating Vulkan command buffers");

    pl_vulkan_sem_params sem{};
    sem.type = VK_SEMAPHORE_TYPE_TIMELINE;
    timeline.sem = pl_vulkan_sem_create(device->gpu, &sem);
    if(!timeline.sem)
        return fail("creating a Vulkan semaphore");
    vkGetDeviceQueue(device->device, device->queue_graphics.index, 0, &queue);
    preRenderer = pl_renderer_create(device->gpu->log, device->gpu);
    return true;
}

bool RtxVsrPass::resize(Texture &tex, int width, int height)
{
    if(tex.texture && tex.texture->params.w == width && tex.texture->params.h == height)
        return true;
    if(tex.texture)
    {
        pl_gpu_finish(device->gpu);
        if(timeline.sem && timelineValue)
        {
            VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wait.semaphoreCount = 1;
            wait.pSemaphores = &timeline.sem;
            wait.pValues = &timelineValue;
            vkWaitSemaphores(device->device, &wait, 2000000000ULL);
        }
    }
    destroy(tex);

    pl_tex_params params{};
    params.w = width;
    params.h = height;
    params.format = pl_find_named_fmt(device->gpu, "rgba8");
    params.renderable = true;
    params.sampleable = true;
    params.storable = true;
    if(!params.format || !(tex.texture = pl_tex_create(device->gpu, &params)))
        return fail("allocating an RGBA texture");

    VkFormat format = VK_FORMAT_UNDEFINED;
    tex.image = pl_vulkan_unwrap(device->gpu, tex.texture, &format, nullptr);
    if(!tex.image || format != VK_FORMAT_R8G8B8A8_UNORM)
        return fail("obtaining an RGBA8 Vulkan image");

    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = tex.image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if(vkCreateImageView(device->device, &info, nullptr, &tex.view) != VK_SUCCESS)
        return fail("creating an RGBA Vulkan image view");
    return true;
}

void RtxVsrPass::destroy(Texture &tex)
{
    if(tex.view)
        vkDestroyImageView(device->device, tex.view, nullptr);
    if(tex.texture)
        pl_tex_destroy(device->gpu, &tex.texture);
    tex = {};
}

bool RtxVsrPass::run(int quality)
{
    auto hold = [this](pl_tex texture, VkImageLayout layout) {
        struct pl_vulkan_hold_params params{};
        params.tex = texture;
        params.layout = layout;
        params.qf = device->queue_graphics.index;
        params.semaphore = {timeline.sem, ++timelineValue};
        return pl_vulkan_hold_ex(device->gpu, &params);
    };

    const bool heldInput = hold(input.texture, inputLayout);
    const bool heldOutput = heldInput && hold(output.texture, outputLayout);
    const uint64_t waitValue = timelineValue;
    pl_gpu_flush(device->gpu);

    // Pure GPU-side timeline semaphore synchronization: wait only on this
    // command buffer slot's own value from N frames ago so the CPU and GPU stay pipelined.
    bool completed = false;
    uint64_t signalValue = 0;
    if(heldOutput)
    {
        const size_t slot = nextSlot;
        nextSlot = (nextSlot + 1) % commandBuffers.size();
        VkCommandBuffer cmd = commandBuffers[slot];
        VkResult result = VK_SUCCESS;
        if(slotValues[slot] != 0)
        {
            VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wait.semaphoreCount = 1;
            wait.pSemaphores = &timeline.sem;
            wait.pValues = &slotValues[slot];
            result = vkWaitSemaphores(device->device, &wait, 2000000000ULL);
        }
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if(result == VK_SUCCESS)
            result = vkResetCommandBuffer(cmd, 0);
        if(result == VK_SUCCESS)
            result = vkBeginCommandBuffer(cmd, &begin);
        if(result == VK_SUCCESS)
        {
            int code = 0;
            const bool creating = !context;
            if(creating)
            {
                QString path = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
                QDir().mkpath(path);
                context = createFeature(device->instance, device->phys_device, device->device,
                    device->get_proc_addr, vkGetDeviceProcAddr, cmd,
                    reinterpret_cast<const wchar_t *>(path.utf16()), &code);
            }

            if(context)
                code = evaluateFeature(context, cmd, input.image, input.view,
                    input.texture->params.w, input.texture->params.h, output.image, output.view,
                    output.texture->params.w, output.texture->params.h, quality);
            result = vkEndCommandBuffer(cmd);
            if(context && code == 1 && result == VK_SUCCESS)
            {
                signalValue = ++timelineValue;
                const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                VkTimelineSemaphoreSubmitInfo timelineSubmit{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
                timelineSubmit.waitSemaphoreValueCount = 1;
                timelineSubmit.pWaitSemaphoreValues = &waitValue;
                timelineSubmit.signalSemaphoreValueCount = 1;
                timelineSubmit.pSignalSemaphoreValues = &signalValue;

                VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                submit.pNext = &timelineSubmit;
                submit.waitSemaphoreCount = 1;
                submit.pWaitSemaphores = &timeline.sem;
                submit.pWaitDstStageMask = &waitStage;
                submit.commandBufferCount = 1;
                submit.pCommandBuffers = &cmd;
                submit.signalSemaphoreCount = 1;
                submit.pSignalSemaphores = &timeline.sem;

                device->lock_queue(device, device->queue_graphics.index, 0);
                result = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
                device->unlock_queue(device, device->queue_graphics.index, 0);
                if(result == VK_SUCCESS)
                {
                    slotValues[slot] = signalValue;
                    completed = true;
                }
            }
            else
                fail("initializing or running the SDK", code);
        }
        if(result != VK_SUCCESS)
            fail("submitting Vulkan work", result);
    }
    else
        fail("acquiring Vulkan images");

    if(!completed)
        pl_gpu_finish(device->gpu);
    auto release = [this, completed, signalValue](pl_tex texture, VkImageLayout layout) {
        struct pl_vulkan_release_params params{};
        params.tex = texture;
        params.layout = layout;
        params.qf = device->queue_graphics.index;
        if(completed && signalValue)
            params.semaphore = {timeline.sem, signalValue};
        pl_vulkan_release_ex(device->gpu, &params);
    };
    if(heldInput)
        release(input.texture, inputLayout);
    if(heldOutput)
        release(output.texture, outputLayout);
    return completed;
}

bool RtxVsrPass::render(pl_renderer renderer, const pl_frame &source, const pl_frame &target,
    const pl_render_params &params, int quality)
{
    if(failed || pl_color_space_is_hdr(&source.color))
        return false;
    const int width = std::lround(std::abs(source.crop.x1 - source.crop.x0));
    const int height = std::lround(std::abs(source.crop.y1 - source.crop.y0));
    const int targetWidth = std::lround(std::abs(target.crop.x1 - target.crop.x0));
    const int targetHeight = std::lround(std::abs(target.crop.y1 - target.crop.y0));
    if(width < 64 || height < 64 || targetWidth <= width || targetHeight <= height)
        return false;
    if(!commandBuffers[0] && !initialize())
        return false;
    if(!resize(input, width, height) || !resize(output, targetWidth, targetHeight))
        return false;

    auto intermediate = rgbFrame(input.texture);
    pl_render_params preParams = pl_render_default_params;
    preParams.dither_params = nullptr;
    if(!pl_render_image(preRenderer ? preRenderer : renderer, &source, &intermediate, &preParams))
        return fail("converting decoded SDR video to RGB");
    if(!run(quality))
        return false;
    auto result = rgbFrame(output.texture);
    auto finalParams = params;
    finalParams.hooks = nullptr;
    finalParams.num_hooks = 0;
    if(!pl_render_image(renderer, &result, &target, &finalParams))
        return fail("compositing the enhanced video");
    if(!active)
    {
        active = true;
        std::fprintf(stdout, "[RTX VSR] Active, quality %d, %dx%d to %dx%d.\n",
            quality, width, height, targetWidth, targetHeight);
        std::fflush(stdout);
    }
    return true;
}
