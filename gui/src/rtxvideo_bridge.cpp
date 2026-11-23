// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Build with MSVC and the locally installed NVIDIA RTX Video SDK. The C exports
// keep the SDK's MSVC C++ ABI out of the MinGW application.

#include <windows.h>
#include <d3d12.h>
#include <vulkan/vulkan.h>
#include <nvsdk_ngx_helpers_vsr.h>
#include <nvsdk_ngx_helpers_vsr_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <cstdio>
#include <filesystem>
#include <new>
#include <string>

namespace {
std::wstring bridgeDirectory()
{
    HMODULE self = nullptr;
    if(!GetModuleHandleExW(
           GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
           reinterpret_cast<LPCWSTR>(&bridgeDirectory), &self) || !self)
        return {};
    wchar_t path[MAX_PATH * 4]{};
    const DWORD len = GetModuleFileNameW(self, path, static_cast<DWORD>(std::size(path)));
    if(len == 0 || len >= std::size(path))
        return {};
    return std::filesystem::path(path).parent_path().wstring();
}
}

struct VsrContext
{
    VkDevice device;
    NVSDK_NGX_Parameter *parameters = nullptr;
    NVSDK_NGX_Handle *feature = nullptr;
};

extern "C" __declspec(dllexport) void Ps5LinkVsrDestroy(VsrContext *context) noexcept
{
    try
    {
        if(!context)
            return;
        if(context->feature)
            NVSDK_NGX_VULKAN_ReleaseFeature(context->feature);
        if(context->parameters)
            NVSDK_NGX_VULKAN_DestroyParameters(context->parameters);
        NVSDK_NGX_VULKAN_Shutdown1(context->device);
        delete context;
    }
    catch(...) {}
}

extern "C" __declspec(dllexport) VsrContext *Ps5LinkVsrCreate(VkInstance instance, VkPhysicalDevice physical,
    VkDevice device, PFN_vkGetInstanceProcAddr instanceProc, PFN_vkGetDeviceProcAddr deviceProc,
    VkCommandBuffer commands, const wchar_t *dataPath, int *status) noexcept
{
    int localStatus = NVSDK_NGX_Result_Fail;
    int &outStatus = status ? *status : localStatus;
    try
    {
        if(!instance || !physical || !device || !commands || !dataPath)
        {
            outStatus = NVSDK_NGX_Result_FAIL_InvalidParameter;
            return nullptr;
        }
        const std::wstring dir = bridgeDirectory();
        const wchar_t *searchPath = dir.empty() ? nullptr : dir.c_str();
        NVSDK_NGX_FeatureCommonInfo commonInfo{};
        if(searchPath)
        {
            commonInfo.PathListInfo.Path = &searchPath;
            commonInfo.PathListInfo.Length = 1;
        }
        outStatus = NVSDK_NGX_VULKAN_Init_with_ProjectID("a941c36e-0a63-4d80-b463-e3f16823f935",
            NVSDK_NGX_ENGINE_TYPE_CUSTOM, "PS5Link", dataPath, instance, physical, device, instanceProc, deviceProc,
            searchPath ? &commonInfo : nullptr);
        if(NVSDK_NGX_FAILED(outStatus))
        {
            std::fprintf(stderr, "[RTX VSR] SDK initialization failed (0x%08x).\n", static_cast<unsigned>(outStatus));
            return nullptr;
        }

        auto *context = new(std::nothrow) VsrContext{device};
        if(!context)
        {
            NVSDK_NGX_VULKAN_Shutdown1(device);
            outStatus = NVSDK_NGX_Result_FAIL_OutOfGPUMemory;
            return nullptr;
        }
        outStatus = NVSDK_NGX_VULKAN_GetCapabilityParameters(&context->parameters);
        int available = 0;
        if(NVSDK_NGX_SUCCEED(outStatus))
            outStatus = context->parameters->Get(NVSDK_NGX_Parameter_VSR_Available, &available);
        if(NVSDK_NGX_SUCCEED(outStatus) && !available)
            outStatus = NVSDK_NGX_Result_FAIL_FeatureNotSupported;
        if(NVSDK_NGX_SUCCEED(outStatus))
        {
            NVSDK_NGX_Feature_Create_Params params{};
            outStatus = NGX_VULKAN_CREATE_VSR_EXT1(device, commands, 1, 1, &context->feature, context->parameters, &params);
        }
        if(NVSDK_NGX_FAILED(outStatus))
        {
            std::fprintf(stderr, "[RTX VSR] Feature creation failed (0x%08x).\n", static_cast<unsigned>(outStatus));
            Ps5LinkVsrDestroy(context);
            return nullptr;
        }
        return context;
    }
    catch(...)
    {
        outStatus = E_FAIL;
        return nullptr;
    }
}

extern "C" __declspec(dllexport) int Ps5LinkVsrEvaluate(VsrContext *context, VkCommandBuffer commands,
    VkImage source, VkImageView sourceView, uint32_t sourceWidth, uint32_t sourceHeight,
    VkImage target, VkImageView targetView, uint32_t targetWidth, uint32_t targetHeight, int quality) noexcept
{
    try
    {
        if(!context || !context->feature || !context->parameters || !commands || !source || !target)
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        auto input = NVSDK_NGX_Create_ImageView_Resource_VK(sourceView, source, range,
            VK_FORMAT_R8G8B8A8_UNORM, sourceWidth, sourceHeight, false);
        auto output = NVSDK_NGX_Create_ImageView_Resource_VK(targetView, target, range,
            VK_FORMAT_R8G8B8A8_UNORM, targetWidth, targetHeight, true);

        NVSDK_NGX_VK_VSR_Eval_Params params{};
        params.pInput = &input;
        params.pOutput = &output;
        params.InputSubrectSize.Width = sourceWidth;
        params.InputSubrectSize.Height = sourceHeight;
        params.OutputSubrectSize.Width = targetWidth;
        params.OutputSubrectSize.Height = targetHeight;
        params.QualityLevel = static_cast<NVSDK_NGX_VSR_QualityLevel>(quality);
        return NGX_VULKAN_EVALUATE_VSR_EXT(commands, context->feature, context->parameters, &params);
    }
    catch(...)
    {
        return E_FAIL;
    }
}


struct Dx12VsrContext
{
    ID3D12Device *device;
    NVSDK_NGX_Parameter *parameters = nullptr;
    NVSDK_NGX_Handle *feature = nullptr;
};

extern "C" __declspec(dllexport) void Ps5LinkDx12VsrDestroyEx(Dx12VsrContext *context, bool shutdownDevice) noexcept
{
    try
    {
        if(!context)
            return;
        if(context->feature)
            NVSDK_NGX_D3D12_ReleaseFeature(context->feature);
        if(context->parameters)
            NVSDK_NGX_D3D12_DestroyParameters(context->parameters);
        if(shutdownDevice && context->device)
            NVSDK_NGX_D3D12_Shutdown1(context->device);
        delete context;
    }
    catch(...) {}
}

extern "C" __declspec(dllexport) void Ps5LinkDx12VsrDestroy(Dx12VsrContext *context) noexcept
{
    Ps5LinkDx12VsrDestroyEx(context, true);
}

extern "C" __declspec(dllexport) Dx12VsrContext *Ps5LinkDx12VsrCreateEx(ID3D12Device *device,
    ID3D12GraphicsCommandList *commands, const wchar_t *dataPath, bool shutdownDeviceOnFail, int *status) noexcept
{
    int localStatus = NVSDK_NGX_Result_Fail;
    int &outStatus = status ? *status : localStatus;
    try
    {
        if(!device || !commands || !dataPath)
        {
            outStatus = NVSDK_NGX_Result_FAIL_InvalidParameter;
            return nullptr;
        }
        const std::wstring dir = bridgeDirectory();
        const wchar_t *searchPath = dir.empty() ? nullptr : dir.c_str();
        NVSDK_NGX_FeatureCommonInfo commonInfo{};
        if(searchPath)
        {
            commonInfo.PathListInfo.Path = &searchPath;
            commonInfo.PathListInfo.Length = 1;
        }
        outStatus = NVSDK_NGX_D3D12_Init_with_ProjectID("a941c36e-0a63-4d80-b463-e3f16823f935",
            NVSDK_NGX_ENGINE_TYPE_CUSTOM, "PS5Link", dataPath, device,
            searchPath ? &commonInfo : nullptr);
        if(NVSDK_NGX_FAILED(outStatus))
            return nullptr;

        auto *context = new(std::nothrow) Dx12VsrContext{device};
        if(!context)
        {
            if(shutdownDeviceOnFail)
                NVSDK_NGX_D3D12_Shutdown1(device);
            outStatus = NVSDK_NGX_Result_FAIL_OutOfGPUMemory;
            return nullptr;
        }
        outStatus = NVSDK_NGX_D3D12_GetCapabilityParameters(&context->parameters);
        int available = 0;
        if(NVSDK_NGX_SUCCEED(outStatus))
            outStatus = context->parameters->Get(NVSDK_NGX_Parameter_VSR_Available, &available);
        if(NVSDK_NGX_SUCCEED(outStatus) && !available)
            outStatus = NVSDK_NGX_Result_FAIL_FeatureNotSupported;
        if(NVSDK_NGX_SUCCEED(outStatus))
        {
            NVSDK_NGX_Feature_Create_Params params{};
            outStatus = NGX_D3D12_CREATE_VSR_EXT(commands, 1, 1, &context->feature, context->parameters, &params);
        }
        if(NVSDK_NGX_FAILED(outStatus))
        {
            Ps5LinkDx12VsrDestroyEx(context, shutdownDeviceOnFail);
            return nullptr;
        }
        return context;
    }
    catch(...)
    {
        outStatus = E_FAIL;
        return nullptr;
    }
}

extern "C" __declspec(dllexport) Dx12VsrContext *Ps5LinkDx12VsrCreate(ID3D12Device *device,
    ID3D12GraphicsCommandList *commands, const wchar_t *dataPath, int *status) noexcept
{
    return Ps5LinkDx12VsrCreateEx(device, commands, dataPath, true, status);
}

extern "C" __declspec(dllexport) int Ps5LinkDx12VsrEvaluate(Dx12VsrContext *context,
    ID3D12GraphicsCommandList *commands, ID3D12Resource *input, ID3D12Resource *output, int quality) noexcept
{
    try
    {
        if(!context || !context->feature || !context->parameters || !commands || !input || !output)
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
        const auto source = input->GetDesc();
        const auto target = output->GetDesc();
        NVSDK_NGX_D3D12_VSR_Eval_Params params{};
        params.pInput = input;
        params.pOutput = output;
        params.InputSubrectSize.Width = static_cast<unsigned>(source.Width);
        params.InputSubrectSize.Height = source.Height;
        params.OutputSubrectSize.Width = static_cast<unsigned>(target.Width);
        params.OutputSubrectSize.Height = target.Height;
        params.QualityLevel = static_cast<NVSDK_NGX_VSR_QualityLevel>(quality);
        return NGX_D3D12_EVALUATE_VSR_EXT(commands, context->feature, context->parameters, &params);
    }
    catch(...)
    {
        return E_FAIL;
    }
}
