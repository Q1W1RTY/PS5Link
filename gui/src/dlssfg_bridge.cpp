// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// NGX frame-generation setup adapted from chiaki-ngx gui/enhancement/worker.cpp
// at 18ff2ed5707cb6e806ed7cc4decdd51587416730 under the same license.
#include <windows.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>

namespace {
class Parameters final : public NVSDK_NGX_Parameter
{
    using Value = std::variant<uint64_t, int64_t, double, void *>;
    std::unordered_map<std::string, Value> values;

    template<class T>
    NVSDK_NGX_Result number(const char *name, T *out) const
    {
        const auto it = values.find(name);
        if(!out)
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
        *out = {};
        if(it == values.end() || std::holds_alternative<void *>(it->second))
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
        *out = std::visit([](auto value) -> T {
            if constexpr (!std::is_pointer_v<decltype(value)>)
                return static_cast<T>(value);
            return {};
        }, it->second);
        return NVSDK_NGX_Result_Success;
    }

    template<class T>
    NVSDK_NGX_Result pointer(const char *name, T **out) const
    {
        const auto it = values.find(name);
        if(!out)
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
        *out = nullptr;
        if(it == values.end())
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
        const auto value = std::get_if<void *>(&it->second);
        if(!value)
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
        *out = static_cast<T *>(*value);
        return NVSDK_NGX_Result_Success;
    }

public:
    void Set(const char *key, unsigned long long value) override { values[key] = uint64_t(value); }
    void Set(const char *key, float value) override { values[key] = double(value); }
    void Set(const char *key, double value) override { values[key] = value; }
    void Set(const char *key, unsigned value) override { values[key] = uint64_t(value); }
    void Set(const char *key, int value) override { values[key] = int64_t(value); }
    void Set(const char *key, ID3D11Resource *value) override { values[key] = static_cast<void *>(value); }
    void Set(const char *key, ID3D12Resource *value) override { values[key] = static_cast<void *>(value); }
    void Set(const char *key, void *value) override { values[key] = value; }
    NVSDK_NGX_Result Get(const char *key, unsigned long long *value) const override { return number(key, value); }
    NVSDK_NGX_Result Get(const char *key, float *value) const override { return number(key, value); }
    NVSDK_NGX_Result Get(const char *key, double *value) const override { return number(key, value); }
    NVSDK_NGX_Result Get(const char *key, unsigned *value) const override { return number(key, value); }
    NVSDK_NGX_Result Get(const char *key, int *value) const override { return number(key, value); }
    NVSDK_NGX_Result Get(const char *key, ID3D11Resource **value) const override { return pointer(key, value); }
    NVSDK_NGX_Result Get(const char *key, ID3D12Resource **value) const override { return pointer(key, value); }
    NVSDK_NGX_Result Get(const char *key, void **value) const override { return pointer(key, value); }
    void Reset() override { values.clear(); }
};

struct Generator
{
    HMODULE module = nullptr;
    ID3D12Device *device = nullptr;
    NVSDK_NGX_Handle *feature = nullptr;
    Parameters parameters;
    unsigned width = 0, height = 0;
    unsigned long long frameId = 0;
    bool shutdownDevice = true;

    ~Generator()
    {
        if(feature)
            NVSDK_NGX_D3D12_ReleaseFeature(feature);
        if(device && shutdownDevice)
            NVSDK_NGX_D3D12_Shutdown1(device);
        if(module)
            FreeLibrary(module);
        // The SM86 proxy installs process-wide hooks. Keep it loaded until process exit.
    }

    void resource(const char *name, ID3D12Resource *texture)
    {
        parameters.Set(name, texture);
        const std::string key(name);
        parameters.Set((key + "SubrectBaseX").c_str(), 0u);
        parameters.Set((key + "SubrectBaseY").c_str(), 0u);
        parameters.Set((key + "SubrectWidth").c_str(), width);
        parameters.Set((key + "SubrectHeight").c_str(), height);
    }
};

constexpr auto searchFlags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
}

extern "C" __declspec(dllexport) void *Ps5LinkDlssFgCreate(ID3D12Device *device,
    ID3D12GraphicsCommandList *commands, const wchar_t *directory, bool useSm86,
    unsigned width, unsigned height, int *status) noexcept
{
    try
    {
        auto generator = std::make_unique<Generator>();
        const std::filesystem::path path(directory);
        if(useSm86)
        {
            const auto proxyPath = path / L"sm86-proxy" / L"version.dll";
            HMODULE proxy = nullptr;
            if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, proxyPath.c_str(), &proxy)
                && !LoadLibraryExW(proxyPath.c_str(), nullptr, searchFlags))
            {
                *status = HRESULT_FROM_WIN32(GetLastError());
                return nullptr;
            }
        }
        generator->module = LoadLibraryExW((path / L"nvngx_dlssg.dll").c_str(), nullptr, searchFlags);
        if(!generator->module)
        {
            *status = HRESULT_FROM_WIN32(GetLastError());
            return nullptr;
        }
        const wchar_t *search = path.c_str();
        NVSDK_NGX_FeatureCommonInfo info{};
        info.PathListInfo.Path = &search;
        info.PathListInfo.Length = 1;
        *status = NVSDK_NGX_D3D12_Init(0x0876232cULL, path.c_str(), device, &info);
        if(*status != NVSDK_NGX_Result_Success)
        {
            std::fprintf(stderr, "DX12 DLSS FG NGX Init returned 0x%08x.\n", unsigned(*status));
            return nullptr;
        }
        generator->device = device;
        generator->width = width;
        generator->height = height;
        auto &p = generator->parameters;
        for(const char *key : {"Width", "OutWidth", "DLSSG.Width", "DLSSG.InputWidth", "DLSSG.OutputWidth",
            "DLSSG.Output.Width", "DLSSG.InternalWidth"})
            p.Set(key, width);
        for(const char *key : {"Height", "OutHeight", "DLSSG.Height", "DLSSG.InputHeight", "DLSSG.OutputHeight",
            "DLSSG.Output.Height", "DLSSG.InternalHeight"})
            p.Set(key, height);
        for(const char *key : {"CreationNodeMask", "VisibilityNodeMask", "DLSSG.EnableInterp", "Enable.OFA", "DLSSG.MultiFrameCount"})
            p.Set(key, 1u);
        p.Set("PerfQualityValue", 1);
        p.Set("DLSSG.ResourceAlwaysProvidedFlags", unsigned(NVSDK_NGX_DLSSG_ResourceFlags_Backbuffer |
            NVSDK_NGX_DLSSG_ResourceFlags_MVecs | NVSDK_NGX_DLSSG_ResourceFlags_Depth |
            NVSDK_NGX_DLSSG_ResourceFlags_HUDLess | NVSDK_NGX_DLSSG_ResourceFlags_OutputInterpolated |
            NVSDK_NGX_DLSSG_ResourceFlags_OutputReal));
        p.Set("DLSSG.ResourceNeverProvidedFlags", unsigned(NVSDK_NGX_DLSSG_ResourceFlags_UI |
            NVSDK_NGX_DLSSG_ResourceFlags_UIAlpha | NVSDK_NGX_DLSSG_ResourceFlags_BidirectionalDistortionField |
            NVSDK_NGX_DLSSG_ResourceFlags_OutputDisableInterpolation));
        for(const char *key : {"DLSSG.UseReflexMatrices", "DLSSG.UserInterfaceRecompositionEnabled",
            "DLSSG.DynamicResolution", "DLSSG.ColorBuffersHDR"})
            p.Set(key, 0u);
        p.Set("DLSSG.BackbufferFormat", unsigned(DXGI_FORMAT_R8G8B8A8_UNORM));
        *status = NVSDK_NGX_D3D12_CreateFeature(commands, NVSDK_NGX_Feature_FrameGeneration, &p, &generator->feature);
        if(*status != NVSDK_NGX_Result_Success)
            std::fprintf(stderr, "DX12 DLSS FG NGX CreateFeature returned 0x%08x.\n", unsigned(*status));
        return *status == NVSDK_NGX_Result_Success ? generator.release() : nullptr;
    }
    catch(...)
    {
        *status = E_FAIL;
        return nullptr;
    }
}

extern "C" __declspec(dllexport) int Ps5LinkDlssFgEvaluate(void *opaque, ID3D12GraphicsCommandList *commands,
    ID3D12CommandQueue *queue, ID3D12CommandAllocator *allocator, HANDLE fenceEvent, bool reset,
    ID3D12Resource *current, ID3D12Resource *motion, ID3D12Resource *depth,
    ID3D12Resource *generated, ID3D12Resource *real) noexcept
{
    try
    {
        if(!opaque)
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
        auto &generator = *static_cast<Generator *>(opaque);
        generator.resource("DLSSG.Backbuffer", current);
        generator.resource("DLSSG.HUDLess", current);
        generator.resource("DLSSG.MVecs", motion);
        generator.resource("DLSSG.Depth", depth);
        generator.resource("DLSSG.OutputInterpolated", generated);
        generator.resource("DLSSG.OutputReal", real);
        auto &p = generator.parameters;
        p.Set("DLSSG.Reset", unsigned(reset));
        p.Set("DLSSG.BackbufferFrameID", ++generator.frameId);
        p.Set("DLSSG.MvecScaleX", 1.0f / generator.width);
        p.Set("DLSSG.MvecScaleY", 1.0f / generator.height);
        p.Set("DLSSG.MvecInvalidValue", FLT_MAX);
        for(const char *key : {"DLSSG.DepthInverted", "DLSSG.MultiFrameCount", "DLSSG.MultiFrameIndex",
            "DLSSG.CameraMotionIncluded", "DLSSG.MvecDilated"})
            p.Set(key, 1u);
        p.Set("DLSSG.MvecJittered", 0u);
        p.Set("DLSSG.CameraNear", 0.1f);
        p.Set("DLSSG.CameraFar", 1000.0f);
        p.Set("DLSSG.CameraFOV", 1.0472f);
        p.Set("DLSSG.CameraAspectRatio", float(generator.width) / generator.height);
        p.Set("DLSSG.InputBackbufferSubrectBaseX", 0u);
        p.Set("DLSSG.InputBackbufferSubrectBaseY", 0u);
        p.Set("DLSSG.InputBackbufferSubrectWidth", generator.width);
        p.Set("DLSSG.InputBackbufferSubrectHeight", generator.height);
        p.Set("DLSSG.LinearizedDepth_Scale", 1.0f);
        p.Set("DLSSG.LinearizedDepth_NearFarPartition", 1.0f);
        static constexpr float identity[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        for(const char *key : {"DLSSG.CameraViewToClip", "DLSSG.ClipToCameraView", "DLSSG.ClipToPrevClip",
            "DLSSG.PrevClipToClip", "DLSSG.ClipToLensClip"})
            p.Set(key, const_cast<void *>(static_cast<const void *>(identity)));
        for(const char *key : {"DLSSG.CameraPosX", "DLSSG.CameraPosY", "DLSSG.CameraPosZ", "DLSSG.CameraRightY",
            "DLSSG.CameraRightZ", "DLSSG.CameraUpX", "DLSSG.CameraUpZ", "DLSSG.CameraFwdX", "DLSSG.CameraFwdY"})
            p.Set(key, 0.0f);
        for(const char *key : {"DLSSG.CameraRightX", "DLSSG.CameraUpY", "DLSSG.CameraFwdZ"})
            p.Set(key, 1.0f);
        for(const char *key : {"DLSSG.NoWarp", "DLSSG.OutputDisableInterpolation", "DLSSG.UI",
            "DLSSG.UIAlpha", "DLSSG.BidirectionalDistortionField"})
            p.Set(key, static_cast<ID3D12Resource *>(nullptr));
        p.Set("DLSSG.CmdQueue", static_cast<void *>(queue));
        p.Set("DLSSG.CmdAlloc", static_cast<void *>(allocator));
        p.Set("DLSSG.FenceEvent", static_cast<void *>(fenceEvent));
        return NVSDK_NGX_D3D12_EvaluateFeature(commands, generator.feature, &p, nullptr);
    }
    catch(...)
    {
        return E_FAIL;
    }
}

extern "C" __declspec(dllexport) void Ps5LinkDlssFgDestroyEx(void *opaque, bool shutdownDevice) noexcept
{
    auto *generator = static_cast<Generator *>(opaque);
    if(!generator)
        return;
    generator->shutdownDevice = shutdownDevice;
    delete generator;
}

extern "C" __declspec(dllexport) void Ps5LinkDlssFgDestroy(void *generator) noexcept
{
    Ps5LinkDlssFgDestroyEx(generator, true);
}
