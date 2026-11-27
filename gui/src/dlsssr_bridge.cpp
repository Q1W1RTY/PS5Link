// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#include <windows.h>
#include <d3d12.h>
#include <nvsdk_ngx_helpers_d3d.h>
#include <filesystem>
#include <memory>

namespace {
struct SuperResolution
{
    HMODULE runtime = nullptr;
    ID3D12Device *device = nullptr;
    NVSDK_NGX_Parameter *parameters = nullptr;
    NVSDK_NGX_Handle *feature = nullptr;
    NVSDK_NGX_Dimensions input{};

    ~SuperResolution()
    {
        if(feature)
            NVSDK_NGX_D3D12_ReleaseFeature(feature);
        if(parameters)
            NVSDK_NGX_D3D12_DestroyParameters(parameters);
        if(device)
            NVSDK_NGX_D3D12_Shutdown1(device);
        if(runtime)
            FreeLibrary(runtime);
    }
};
}

extern "C" __declspec(dllexport) void *Ps5LinkDlssSrCreate(ID3D12Device *device,
    ID3D12GraphicsCommandList *commands, const wchar_t *directory, const wchar_t *dataPath,
    unsigned width, unsigned height, unsigned targetWidth, unsigned targetHeight,
    bool model45, int *status) noexcept
{
    int localStatus = NVSDK_NGX_Result_Fail;
    int &outStatus = status ? *status : localStatus;
    try
    {
        if(!device || !commands || !directory || !dataPath || width == 0 || height == 0 || targetWidth == 0 || targetHeight == 0)
        {
            outStatus = NVSDK_NGX_Result_FAIL_InvalidParameter;
            return nullptr;
        }
        auto sr = std::make_unique<SuperResolution>();
        const auto runtimePath = std::filesystem::path(directory) / L"nvngx_dlss.dll";
        sr->runtime = LoadLibraryExW(runtimePath.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!sr->runtime)
        {
            outStatus = HRESULT_FROM_WIN32(GetLastError());
            return nullptr;
        }
        NVSDK_NGX_FeatureCommonInfo info{};
        info.PathListInfo.Path = &directory;
        info.PathListInfo.Length = 1;
        outStatus = NVSDK_NGX_D3D12_Init_with_ProjectID("a9a3e71b-7b03-4a87-9fdf-91d38bc3d5ae",
            NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", dataPath, device, &info);
        if(outStatus != NVSDK_NGX_Result_Success)
            return nullptr;
        sr->device = device;
        outStatus = NVSDK_NGX_D3D12_AllocateParameters(&sr->parameters);
        if(outStatus != NVSDK_NGX_Result_Success)
            return nullptr;

        const auto preset = model45 ? NVSDK_NGX_DLSS_Hint_Render_Preset_M : NVSDK_NGX_DLSS_Hint_Render_Preset_K;
        for(const char *key : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
            sr->parameters->Set(key, static_cast<unsigned>(preset));
        sr->input = {width, height};
        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature.InWidth = width;
        create.Feature.InHeight = height;
        create.Feature.InTargetWidth = targetWidth;
        create.Feature.InTargetHeight = targetHeight;
        const double scaleRatio = height ? static_cast<double>(targetHeight) / static_cast<double>(height) : 1.0;
        if(scaleRatio <= 1.05)
            create.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
        else if(scaleRatio <= 1.55)
            create.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxQuality;
        else if(scaleRatio <= 1.75)
            create.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_Balanced;
        else
            create.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxPerf;
        create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        outStatus = NGX_D3D12_CREATE_DLSS_EXT(commands, 1, 1, &sr->feature, sr->parameters, &create);
        return outStatus == NVSDK_NGX_Result_Success ? sr.release() : nullptr;
    }
    catch(...)
    {
        outStatus = E_FAIL;
        return nullptr;
    }
}

extern "C" __declspec(dllexport) int Ps5LinkDlssSrEvaluate(void *opaque, ID3D12GraphicsCommandList *commands,
    ID3D12Resource *color, ID3D12Resource *motion, ID3D12Resource *depth, ID3D12Resource *output, bool reset) noexcept
{
    try
    {
        if(!opaque || !commands || !color || !motion || !depth || !output)
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
        auto &sr = *static_cast<SuperResolution *>(opaque);
        NVSDK_NGX_D3D12_DLSS_Eval_Params evaluate{};
        evaluate.Feature.pInColor = color;
        evaluate.Feature.pInOutput = output;
        evaluate.pInDepth = depth;
        evaluate.pInMotionVectors = motion;
        evaluate.InRenderSubrectDimensions = sr.input;
        evaluate.InReset = reset;
        return NGX_D3D12_EVALUATE_DLSS_EXT(commands, sr.feature, sr.parameters, &evaluate);
    }
    catch(...)
    {
        return E_FAIL;
    }
}

extern "C" __declspec(dllexport) void Ps5LinkDlssSrDestroy(void *opaque) noexcept
{
    delete static_cast<SuperResolution *>(opaque);
}
