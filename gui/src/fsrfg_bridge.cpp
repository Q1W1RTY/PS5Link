// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Native Direct3D 12 bridge for AMD FidelityFX FSR Frame Generation (SDK v2.4.0).
#include <windows.h>
#include <d3d12.h>
#include <ffx_api.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_framegeneration.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>

namespace {
struct FsrFgGenerator
{
    HMODULE loader = nullptr;
    PfnFfxCreateContext createContext = nullptr;
    PfnFfxDestroyContext destroyContext = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    ffxContext ctx = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t frameId = 0;

    ~FsrFgGenerator()
    {
        if(ctx && destroyContext)
            destroyContext(&ctx, nullptr);
        if(loader)
            FreeLibrary(loader);
    }
};

constexpr auto searchFlags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
}

extern "C" __declspec(dllexport) void *Ps5LinkFsrFgCreate(ID3D12Device *device,
    const wchar_t *directory, unsigned width, unsigned height,
    char *versionOut, size_t versionCap, int *status) noexcept
{
    if(status)
        *status = 0;
    if(versionOut && versionCap > 0)
        versionOut[0] = '\0';
    try
    {
        if(!device || !directory || width == 0 || height == 0)
        {
            if(status) *status = FFX_API_RETURN_ERROR_PARAMETER;
            return nullptr;
        }
        auto gen = std::make_unique<FsrFgGenerator>();
        const std::filesystem::path dir(directory);
        const auto loaderPath = dir / L"amd_fidelityfx_loader_dx12.dll";
        gen->loader = LoadLibraryExW(loaderPath.c_str(), nullptr, searchFlags);
        if(!gen->loader)
        {
            if(status) *status = int(HRESULT_FROM_WIN32(GetLastError()));
            std::fprintf(stderr, "DX12 FSR FG failed to load amd_fidelityfx_loader_dx12.dll (0x%08x).\n",
                unsigned(GetLastError()));
            return nullptr;
        }
        gen->createContext = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(gen->loader, "ffxCreateContext"));
        gen->destroyContext = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(gen->loader, "ffxDestroyContext"));
        gen->configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(gen->loader, "ffxConfigure"));
        gen->query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(gen->loader, "ffxQuery"));
        gen->dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(gen->loader, "ffxDispatch"));
        if(!gen->createContext || !gen->destroyContext || !gen->configure || !gen->dispatch)
        {
            if(status) *status = FFX_API_RETURN_NO_PROVIDER;
            return nullptr;
        }

        ffxCreateBackendDX12Desc backendDesc{};
        backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backendDesc.header.pNext = nullptr;
        backendDesc.device = device;

        ffxCreateContextDescFrameGenerationVersion versionDesc{};
        versionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
        versionDesc.header.pNext = &backendDesc.header;
        versionDesc.version = FFX_FRAMEGENERATION_VERSION;

        ffxCreateContextDescFrameGeneration createDesc{};
        createDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        createDesc.header.pNext = &versionDesc.header;
        createDesc.flags = FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS
            | FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
        createDesc.displaySize = {width, height};
        createDesc.maxRenderSize = {width, height};
        createDesc.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;

        const ffxReturnCode_t rc = gen->createContext(&gen->ctx, &createDesc.header, nullptr);
        if(rc != FFX_API_RETURN_OK || !gen->ctx)
        {
            if(status) *status = int(rc);
            std::fprintf(stderr, "DX12 FSR FG ffxCreateContext returned %u.\n", unsigned(rc));
            return nullptr;
        }

        gen->width = width;
        gen->height = height;

        if(gen->query && versionOut && versionCap > 1)
        {
            ffxQueryGetProviderVersion ver{};
            ver.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
            ver.header.pNext = nullptr;
            if(gen->query(&gen->ctx, &ver.header) == FFX_API_RETURN_OK && ver.versionName)
            {
                std::strncpy(versionOut, ver.versionName, versionCap - 1);
                versionOut[versionCap - 1] = '\0';
            }
        }
        if(status)
            *status = 1;
        return gen.release();
    }
    catch(...)
    {
        if(status) *status = E_FAIL;
        return nullptr;
    }
}

extern "C" __declspec(dllexport) int Ps5LinkFsrFgEvaluateEx(void *opaque, ID3D12GraphicsCommandList *commands,
    bool reset, float frameTimeDeltaMs, ID3D12Resource *current, ID3D12Resource *motion, ID3D12Resource *depth,
    ID3D12Resource *generated) noexcept
{
    try
    {
        if(!opaque || !commands || !current || !motion || !depth || !generated)
            return -int(FFX_API_RETURN_ERROR_PARAMETER);
        auto &gen = *static_cast<FsrFgGenerator *>(opaque);
        const uint64_t fid = ++gen.frameId;

        ffxConfigureDescFrameGeneration cfg{};
        cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        cfg.header.pNext = nullptr;
        cfg.swapChain = nullptr;
        cfg.presentCallback = nullptr;
        cfg.presentCallbackUserContext = nullptr;
        cfg.frameGenerationCallback = nullptr;
        cfg.frameGenerationCallbackUserContext = nullptr;
        cfg.frameGenerationEnabled = true;
        cfg.allowAsyncWorkloads = false;
        cfg.HUDLessColor = FfxApiResource{};
        cfg.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
        cfg.onlyPresentGenerated = false;
        cfg.generationRect = {0, 0, int32_t(gen.width), int32_t(gen.height)};
        cfg.frameID = fid;

        ffxReturnCode_t rc = gen.configure(&gen.ctx, &cfg.header);
        if(rc != FFX_API_RETURN_OK)
        {
            std::fprintf(stderr, "DX12 FSR FG ffxConfigure returned %u.\n", unsigned(rc));
            return -int(rc);
        }

        ffxDispatchDescFrameGenerationPrepareV2 prep{};
        prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        prep.header.pNext = nullptr;
        prep.frameID = fid;
        prep.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
        prep.commandList = commands;
        prep.renderSize = {gen.width, gen.height};
        prep.jitterOffset = {0.0f, 0.0f};
        prep.motionVectorScale = {1.0f, 1.0f};
        prep.frameTimeDelta = (frameTimeDeltaMs >= 4.0f && frameTimeDeltaMs <= 60.0f) ? frameTimeDeltaMs : 16.6667f;
        prep.reset = reset;
        prep.cameraNear = 0.1f;
        prep.cameraFar = 1000.0f;
        prep.cameraFovAngleVertical = 1.0472f;
        prep.viewSpaceToMetersFactor = 1.0f;
        prep.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        prep.motionVectors = ffxApiGetResourceDX12(motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        prep.cameraPosition[0] = 0.0f; prep.cameraPosition[1] = 0.0f; prep.cameraPosition[2] = 0.0f;
        prep.cameraUp[0] = 0.0f;       prep.cameraUp[1] = 1.0f;       prep.cameraUp[2] = 0.0f;
        prep.cameraRight[0] = 1.0f;    prep.cameraRight[1] = 0.0f;    prep.cameraRight[2] = 0.0f;
        prep.cameraForward[0] = 0.0f;  prep.cameraForward[1] = 0.0f;  prep.cameraForward[2] = 1.0f;

        rc = gen.dispatch(&gen.ctx, &prep.header);
        if(rc != FFX_API_RETURN_OK)
        {
            std::fprintf(stderr, "DX12 FSR FG PrepareV2 returned %u.\n", unsigned(rc));
            return -int(rc);
        }

        ffxDispatchDescFrameGeneration disp{};
        disp.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
        disp.header.pNext = nullptr;
        disp.commandList = commands;
        disp.presentColor = ffxApiGetResourceDX12(current, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        disp.outputs[0] = ffxApiGetResourceDX12(generated, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        disp.numGeneratedFrames = reset ? 0u : 1u;
        disp.reset = reset;
        disp.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        disp.minMaxLuminance[0] = 0.0f;
        disp.minMaxLuminance[1] = 100.0f;
        disp.generationRect = {0, 0, int32_t(gen.width), int32_t(gen.height)};
        disp.frameID = fid;

        rc = gen.dispatch(&gen.ctx, &disp.header);
        if(rc != FFX_API_RETURN_OK)
        {
            std::fprintf(stderr, "DX12 FSR FG Dispatch returned %u.\n", unsigned(rc));
            return -int(rc);
        }
        return 1;
    }
    catch(...)
    {
        return int(E_FAIL);
    }
}

extern "C" __declspec(dllexport) int Ps5LinkFsrFgEvaluate(void *opaque, ID3D12GraphicsCommandList *commands,
    bool reset, ID3D12Resource *current, ID3D12Resource *motion, ID3D12Resource *depth,
    ID3D12Resource *generated) noexcept
{
    return Ps5LinkFsrFgEvaluateEx(opaque, commands, reset, 16.6667f, current, motion, depth, generated);
}

extern "C" __declspec(dllexport) void Ps5LinkFsrFgDestroy(void *opaque) noexcept
{
    delete static_cast<FsrFgGenerator *>(opaque);
}
