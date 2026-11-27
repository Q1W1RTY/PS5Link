// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <dx12replay.h>
#include "dx12_shaders.h"
#include <ds5bt.h>
#include <ps5link.h>
#include <ps5linklocale.h>
#include <qmlsettings.h>
#include <streamsession.h>

#include <QElapsedTimer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QScopeGuard>
#include <QSettings>
#include <QLibrary>
#include <QStandardPaths>
#include <QScreen>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QTouchEvent>
#include <QCursor>
#include <QTimer>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <avrt.h>
#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <libavutil/hwcontext_d3d12va.h>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/frame.h>
#include <SDL.h>
}

using Microsoft::WRL::ComPtr;

namespace {
constexpr auto rgbFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr auto highPrecisionFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr auto swapchainFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
enum OverlayAction {
    CloseMenu, OpenFullSettings, Disconnect, SleepConsole, ExitApp, SleepAndExit, Hud, Upscale, Generation,
    Volume, Microphone, PresentMode, AspectMode, AmbientStyle, UpscaleTarget, Sharpness,
    ErrorConcealment, DisableKeyboard, WindowMode, ResetGyro,
    Dither, Deband, HdrPassthrough, HdrPaperWhite, ColorContrast, ColorSaturation, ColorGamma
};

struct RingSamples
{
    static constexpr size_t kCapacity = 16384;
    std::vector<qint64> data;
    size_t writePos = 0;

    RingSamples() { data.reserve(kCapacity); }
    void push(qint64 value)
    {
        if(data.size() < kCapacity)
            data.push_back(value);
        else
        {
            data[writePos] = value;
            writePos = (writePos + 1) % kCapacity;
        }
    }
    bool empty() const { return data.empty(); }
    std::vector<qint64> sortedCopy() const
    {
        auto copy = data;
        std::sort(copy.begin(), copy.end());
        return copy;
    }
};

void check(HRESULT result, const char *operation)
{
    if(FAILED(result))
        throw std::runtime_error(QStringLiteral("%1 failed (0x%2)")
            .arg(QString::fromLatin1(operation)).arg(static_cast<quint32>(result), 8, 16, QChar('0')).toStdString());
}

template<class List>
void transition(List *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to,
    UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, subresource, from, to};
    list->ResourceBarrier(1, &barrier);
}

ComPtr<ID3DBlob> compileShader(const char *source, const char *entry, const char *target)
{
    ComPtr<ID3DBlob> code, errors;
    const HRESULT result = D3DCompile(source, std::char_traits<char>::length(source), "PS5Link DX12 video", nullptr,
        nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if(FAILED(result) && errors)
        std::fprintf(stderr, "%s\n", static_cast<const char *>(errors->GetBufferPointer()));
    check(result, "DX12 shader compilation");
    return code;
}

struct Conversion
{
    float visibleWidth, visibleHeight;
    float maxLumaU, maxLumaV;
    float maxChromaU, maxChromaV;
    float chromaOffsetU, chromaOffsetV;
    float chromaTexW, chromaTexH;
    float invChromaTexW, invChromaTexH;
    float lumaOffset, lumaScale, chromaOffset, chromaScale;
    float kr, kb;
    float hdrMode, hdrPaperWhite;
};

Conversion colorConversion(const AVFrame &frame, const D3D12_RESOURCE_DESC &desc, int hdrPaperWhiteNits, bool hdrPassthrough)
{
    const bool p010 = desc.Format == DXGI_FORMAT_P010;
    const bool full = frame.color_range == AVCOL_RANGE_JPEG;
    const float factor = p010 ? 4.0f : 1.0f;
    const float normalizedMax = p010 ? 65535.0f / 64 : 255.0f;
    const float fullRange = p010 ? 1023.0f : 255.0f;
    float kr = 0.2126f, kb = 0.0722f;
    if(frame.colorspace == AVCOL_SPC_BT470BG || frame.colorspace == AVCOL_SPC_SMPTE170M)
    {
        kr = 0.299f;
        kb = 0.114f;
    }
    else if(frame.colorspace == AVCOL_SPC_BT2020_NCL || frame.color_trc == AVCOL_TRC_SMPTE2084
        || frame.color_trc == AVCOL_TRC_ARIB_STD_B67)
    {
        kr = 0.2627f;
        kb = 0.0593f;
    }

    float hdrMode = 0.0f;
    const float paperWhite = float(std::clamp(hdrPaperWhiteNits, 120, 400));
    if(frame.color_trc == AVCOL_TRC_SMPTE2084)
        hdrMode = hdrPassthrough ? 3.0f : 1.0f;
    else if(frame.color_trc == AVCOL_TRC_ARIB_STD_B67)
        hdrMode = 2.0f;

    const float texW = float(desc.Width);
    const float texH = float(desc.Height);
    const float chromaW = std::max(1.0f, texW * 0.5f);
    const float chromaH = std::max(1.0f, texH * 0.5f);
    const float visChromaW = std::max(1.0f, float((frame.width + 1) / 2));
    const float visChromaH = std::max(1.0f, float((frame.height + 1) / 2));

    // Align 4:2:0 chroma sample position to luma grid (GPU chroma texel 0 center sits at luma 1.0).
    float chromaOffU = 0.0f, chromaOffV = 0.0f;
    switch(frame.chroma_location)
    {
        case AVCHROMA_LOC_TOPLEFT:
            chromaOffU = 0.5f / texW;
            chromaOffV = 0.5f / texH;
            break;
        case AVCHROMA_LOC_BOTTOMLEFT:
            chromaOffU = 0.5f / texW;
            chromaOffV = -0.5f / texH;
            break;
        case AVCHROMA_LOC_CENTER:
            break;
        case AVCHROMA_LOC_LEFT:
            chromaOffU = 0.5f / texW;
            break;
        case AVCHROMA_LOC_UNSPECIFIED:
        default:
            chromaOffU = 0.5f / texW;
            if(frame.color_trc == AVCOL_TRC_SMPTE2084)
                chromaOffV = 0.5f / texH;
            break;
    }

    return {
        float(frame.width) / texW, float(frame.height) / texH,
        (float(frame.width) - 0.5f) / texW, (float(frame.height) - 0.5f) / texH,
        (visChromaW - 0.5f) / chromaW, (visChromaH - 0.5f) / chromaH,
        chromaOffU, chromaOffV,
        chromaW, chromaH,
        1.0f / chromaW, 1.0f / chromaH,
        full ? 0.0f : 16 * factor / normalizedMax, normalizedMax / (full ? fullRange : 219 * factor),
        128 * factor / normalizedMax, normalizedMax / (full ? fullRange : 224 * factor),
        kr, kb,
        hdrMode, paperWhite,
    };
}
}

struct Dx12ReplayWindow::Pipeline
{
    enum class Aspect { Fit, Fill, Stretch, Ambient };
    AVBufferRef *avDevice = nullptr;
    ComPtr<ID3D12Device> device;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGISwapChain3> swapchain;
    HWND swapchainHwnd = nullptr;
    DXGI_COLOR_SPACE_TYPE currentSwapchainColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bool displayHdrCapable = false;
    bool display10Bit = false;
    bool hdrPassthroughActive = false;
    QElapsedTimer hdrCheckTimer;
    ComPtr<ID3D12CommandQueue> graphicsQueue;
    ComPtr<ID3D12CommandAllocator> graphicsAllocator;
    struct FrameSlot
    {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12DescriptorHeap> color, downsample, postProcess, fsr, ambient, baselineAmbient, blit, motion, srMotion;
        std::shared_ptr<AVFrame> decoded;
        uint64_t fence = 0;
    };
    std::array<FrameSlot, 3> frameSlots;
    unsigned nextSlot = 0;
    FrameSlot *currentSlot = nullptr;
    ComPtr<ID3D12GraphicsCommandList> graphicsCommands;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> colorPipeline, colorPipelineFp16;
    ComPtr<ID3D12PipelineState> lumaDownsamplePipeline, motionPipeline, refineMotionPipeline;
    ComPtr<ID3D12PipelineState> interpolationPipeline, interpolationPipelineFp16, dlssMotionPipeline;
    ComPtr<ID3D12DescriptorHeap> views, renderTargetView, interpolationViews;
    ComPtr<ID3D12PipelineState> downsamplePipeline, downsamplePipelineFp16;
    ComPtr<ID3D12PipelineState> postProcessPipeline;
    ComPtr<ID3D12PipelineState> fsrPipeline, fsrPipelineFp16;
    ComPtr<ID3D12PipelineState> ambientPipeline, ambientPipelineFp16;
    ComPtr<ID3D12PipelineState> blitPipeline, blitPipelineFp16;
    ComPtr<ID3D12DescriptorHeap> downsampleViews, postProcessViews, fsrViews, ambientViews, blitViews;
    ComPtr<ID3D12Resource> rgb;
    DXGI_FORMAT rgbSurfaceFormat = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12Resource> postInput, splitBaseline, interpOutput, lastPresented;
    QSize lastPresentedSize;
    bool lastPresentedUsedVsr = false;
    bool lastPresentedUsedFsr = false;
    bool fsrEnabled = false;
    unsigned fsrFrames = 0;
    unsigned concealedFrames = 0;
    bool hdrToneMapped = false;
    float smoothedRenderMs = 0.0f;
    std::array<float, 120> frameTimeHistory{};
    size_t frameTimeHistoryPos = 0;
    size_t frameTimeHistoryCount = 0;
    std::atomic<int> ambientStyle{2}; // 0=Ambient colors, 1=Blurred video, 2=Edge stretch
    std::atomic<bool> errorConcealment{false};
    std::atomic<bool> keyboardDisabled{false};
    std::atomic<int> windowMode{2}; // 0=Fullscreen, 1=Borderless, 2=Windowed
    std::atomic<int> touchpadWidth{100};
    std::atomic<bool> ditherEnabled{true};
    std::atomic<bool> debandEnabled{false};
    std::atomic<bool> hdrPassthroughEnabled{true};
    std::atomic<int> hdrPaperWhite{203};
    std::atomic<int> colorContrast{100};
    std::atomic<int> colorSaturation{100};
    std::atomic<int> colorGamma{100};
    std::atomic<bool> systemVolumeMode{false};
    std::atomic<bool> showLightbarOverlay{false};
    std::atomic<uint32_t> lightbarRgb{0x000040ffu};
    std::atomic<float> hudPacketLossPct{0.0f};
    std::atomic<float> hudBitrateMbps{0.0f};
    std::atomic<float> hudRttMs{0.0f};
    std::atomic<int> hudGamepadBattery{-1};
    std::atomic<bool> hudGamepadCharging{false};
    std::atomic<bool> hudGamepadFull{false};
    std::atomic<bool> hudGamepadConnected{false};
    std::atomic<uint64_t> hudCorruptFrames{0};
    uint64_t prevCorruptFrames = 0;
    QElapsedTimer lastCorruptTime;
    std::atomic<bool> hideOnScreenControls{false};
    std::atomic<bool> showMicBtn{true};
    std::atomic<bool> showAspectBtn{true};
    std::atomic<bool> showVolBtns{true};
    std::atomic<bool> bottomDockHovered{false};
    std::atomic<bool> menuPopupOpen{false};
    std::atomic<float> menuLeftPx{0.0f}, menuTopPx{0.0f}, menuRightPx{0.0f}, menuBottomPx{0.0f};
    float dockAnim = 0.0f;
    int savedVolumeBeforeMute = 100;
    QElapsedTimer volMinusHoldTimer;
    bool volMinusMutedThisHold = false;
    HANDLE fgPaceTimer = nullptr;
    HANDLE balancedPaceTimer = nullptr;
    HANDLE dlssFenceEvent = nullptr;
    double smoothedInputIntervalUs = 16667.0;
    RingSamples fgSpacing;
    ComPtr<ID3D12Fence> graphicsFence;
    HANDLE completedEvent = nullptr;
    HANDLE frameLatency = nullptr;
    UINT swapchainFlags = 0;
    bool tearingSupported = false;
    bool hasPresentPermit = false;
    bool swapchainOccluded = false;
    std::atomic<int> presentMode{1};
    std::atomic<bool> showMenu{false}, showHud{false};
    std::atomic<int> overlayVolume{128};
    std::atomic<bool> overlayMic{false};
    std::atomic<int> sharpnessSetting{-1}; // -1 = Auto (20% for DLSS SR, 0% otherwise), 0..100 = manual
    std::atomic<bool> splitScreen{false};
    std::atomic<float> splitPosition{0.5f};
    std::shared_ptr<const std::string> hudText;
    std::mutex inputMutex;
    QPointF mousePosition;
    float mouseWheel = 0.0f;
    std::vector<bool> mouseClicks;
    struct KeyInput { ImGuiKey key; bool down; };
    std::vector<KeyInput> keyEvents;
    std::function<void(int, int)> overlayAction;
    ImGuiContext *imgui = nullptr;
    ComPtr<ID3D12DescriptorHeap> imguiViews;
    bool imguiFailed = false;
    bool imguiHdrStyled = false;
    int overlayPage = 0;
    QElapsedTimer imguiDeltaTimer;
    float displayedFps = 0;
    QElapsedTimer overlayRate;
    unsigned previousOverlayFrames = 0;
    uint64_t fenceValue = 0;
    QSize outputSize;
    RingSamples submitTime;
    RingSamples presentCalls;
    unsigned presentedFrames = 0;
    unsigned occluded = 0;
    unsigned presentTimeouts = 0;
    unsigned enhancedFrames = 0;
    unsigned supersampledFrames = 0;
    int upscaleHeight = 0;
    QSize upscaleSize;
    unsigned generatedFrames = 0;
    unsigned dlssFrames = 0;
    unsigned dlssFramesAtSelection = 0;
    unsigned fsrFgFrames = 0;
    unsigned fsrFgFramesAtSelection = 0;
    int generationMode = 0;
    bool useSm86 = false;
    bool dlssFailed = false;
    bool fsrFgFailed = false;
    bool historyValid = false;
    Aspect aspect = Aspect::Fit;
    QElapsedTimer inputGap;
    ComPtr<ID3D12Resource> videoHistory[2], videoLumaHistory[2], motionTexture, motionRefinedTexture;
    DXGI_FORMAT videoHistoryFormat = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12Resource> dlssMotionTexture, dlssDepth, dlssGenerated, dlssReal;
    using CreateDlss = void *(*)(ID3D12Device *, ID3D12GraphicsCommandList *, const wchar_t *, bool, unsigned, unsigned, int *);
    using CreateDlssEx = void *(*)(ID3D12Device *, ID3D12GraphicsCommandList *, const wchar_t *, bool, unsigned, unsigned, bool, int *);
    using EvaluateDlss = int (*)(void *, ID3D12GraphicsCommandList *, ID3D12CommandQueue *, ID3D12CommandAllocator *,
        HANDLE, bool, ID3D12Resource *, ID3D12Resource *, ID3D12Resource *, ID3D12Resource *, ID3D12Resource *);
    using DestroyDlss = void (*)(void *);
    using DestroyDlssEx = void (*)(void *, bool);
    QLibrary dlssBridge;
    EvaluateDlss evaluateDlss = nullptr;
    DestroyDlss destroyDlss = nullptr;
    DestroyDlssEx destroyDlssEx = nullptr;
    void *dlss = nullptr;
    QSize dlssSize;
    using CreateFsrFg = void *(*)(ID3D12Device *, const wchar_t *, unsigned, unsigned, char *, size_t, int *);
    using EvaluateFsrFg = int (*)(void *, ID3D12GraphicsCommandList *, bool, ID3D12Resource *, ID3D12Resource *,
        ID3D12Resource *, ID3D12Resource *);
    using EvaluateFsrFgEx = int (*)(void *, ID3D12GraphicsCommandList *, bool, float, ID3D12Resource *, ID3D12Resource *,
        ID3D12Resource *, ID3D12Resource *);
    using DestroyFsrFg = void (*)(void *);
    QLibrary fsrFgBridge;
    EvaluateFsrFg evaluateFsrFg = nullptr;
    EvaluateFsrFgEx evaluateFsrFgEx = nullptr;
    DestroyFsrFg destroyFsrFg = nullptr;
    void *fsrFg = nullptr;
    QSize fsrFgSize;
    std::string fsrFgVersion = "3.1";
    using CreateSr = void *(*)(ID3D12Device *, ID3D12GraphicsCommandList *, const wchar_t *, const wchar_t *,
        unsigned, unsigned, unsigned, unsigned, bool, int *);
    using EvaluateSr = int (*)(void *, ID3D12GraphicsCommandList *, ID3D12Resource *, ID3D12Resource *,
        ID3D12Resource *, ID3D12Resource *, bool);
    using DestroySr = void (*)(void *);
    QLibrary srBridge;
    EvaluateSr evaluateSr = nullptr;
    DestroySr destroySr = nullptr;
    void *sr = nullptr;
    int srModel = 0;
    bool srFailed = false;
    bool srHistoryValid = false;
    unsigned srFrames = 0;
    unsigned srFramesAtSelection = 0;
    ComPtr<ID3D12DescriptorHeap> srViews;
    ComPtr<ID3D12Resource> srInput[2], srLumaHistory[2], srCoarse, srRefined, srMotion, srDepth, srOutput;
    int vsrQuality = 0;
    unsigned vsrError = 0;
    using CreateVsr = void *(*)(ID3D12Device *, ID3D12GraphicsCommandList *, const wchar_t *, int *);
    using CreateVsrEx = void *(*)(ID3D12Device *, ID3D12GraphicsCommandList *, const wchar_t *, bool, int *);
    using EvaluateVsr = int (*)(void *, ID3D12GraphicsCommandList *, ID3D12Resource *, ID3D12Resource *, int);
    using DestroyVsr = void (*)(void *);
    using DestroyVsrEx = void (*)(void *, bool);
    QLibrary bridge;
    CreateVsr createVsr = nullptr;
    CreateVsrEx createVsrEx = nullptr;
    EvaluateVsr evaluateVsr = nullptr;
    DestroyVsr destroyVsr = nullptr;
    DestroyVsrEx destroyVsrEx = nullptr;
    void *vsr = nullptr;
    ComPtr<ID3D12Resource> enhanced;
    QSize convertedSize;
    QSize lastSourceSize;
    QString capturePath;
    unsigned captureFrame = 1;
    QString generatedCapturePath;

    void releaseVsr(bool allowDeviceShutdown)
    {
        if(!vsr)
            return;
        if(destroyVsrEx)
            destroyVsrEx(vsr, allowDeviceShutdown);
        else
            destroyVsr(vsr);
        vsr = nullptr;
    }

    void releaseDlss(bool allowDeviceShutdown)
    {
        if(!dlss)
            return;
        if(destroyDlssEx)
            destroyDlssEx(dlss, allowDeviceShutdown);
        else
            destroyDlss(dlss);
        dlss = nullptr;
    }

    void releaseFsrFg()
    {
        if(!fsrFg)
            return;
        if(destroyFsrFg)
            destroyFsrFg(fsrFg);
        fsrFg = nullptr;
    }

    void releaseGenerationResources()
    {
        waitForFence(fenceValue, 5000);
        releaseDlss(vsr == nullptr);
        releaseFsrFg();
        for(auto &texture : videoHistory)
            texture.Reset();
        for(auto &texture : videoLumaHistory)
            texture.Reset();
        videoHistoryFormat = DXGI_FORMAT_UNKNOWN;
        motionTexture.Reset();
        motionRefinedTexture.Reset();
        interpOutput.Reset();
        dlssMotionTexture.Reset();
        dlssDepth.Reset();
        dlssGenerated.Reset();
        dlssReal.Reset();
        historyValid = false;
    }

    ~Pipeline()
    {
        try { waitForFence(fenceValue, 3000); } catch(const std::exception &) {}
        if(imgui)
        {
            ImGui::SetCurrentContext(imgui);
            ImGui_ImplDX12_Shutdown();
            ImGui::DestroyContext(imgui);
        }
        resetSr();
        releaseFsrFg();
        // Release features without tearing down the shared NGX device while another feature is still alive.
        releaseVsr(dlss == nullptr);
        releaseDlss(true);
        av_buffer_unref(&avDevice);
        if(completedEvent)
            CloseHandle(completedEvent);
        if(dlssFenceEvent)
            CloseHandle(dlssFenceEvent);
        if(fgPaceTimer)
            CloseHandle(fgPaceTimer);
        if(balancedPaceTimer)
            CloseHandle(balancedPaceTimer);
        if(frameLatency)
            CloseHandle(frameLatency);
    }

    void initialize()
    {
        if(av_hwdevice_ctx_create(&avDevice, AV_HWDEVICE_TYPE_D3D12VA, nullptr, nullptr, 0) < 0)
            throw std::runtime_error("FFmpeg could not create a D3D12VA device");
        auto *context = reinterpret_cast<AVHWDeviceContext *>(avDevice->data);
        device = static_cast<AVD3D12VADeviceContext *>(context->hwctx)->device;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
        ComPtr<IDXGIFactory5> factory5;
        BOOL tearing = FALSE;
        tearingSupported = SUCCEEDED(factory.As(&factory5))
            && SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))
            && tearing;
        swapchainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if(tearingSupported)
            swapchainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        ComPtr<IDXGIAdapter1> adapter;
        check(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)), "DXGI decoder adapter");
        DXGI_ADAPTER_DESC1 adapterDescription{};
        check(adapter->GetDesc1(&adapterDescription), "DXGI adapter description");
        const auto adapterName = QString::fromWCharArray(adapterDescription.Description);
        useSm86 = adapterDescription.VendorId == 0x10de
            && !adapterName.contains("Ada", Qt::CaseInsensitive)
            && (adapterName.contains("RTX 20") || adapterName.contains("RTX 30") || adapterName.contains("RTX A"));
        std::printf("DX12 adapter: %s\n", adapterName.toUtf8().constData());

        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&graphicsQueue)), "DX12 graphics queue");
        const D3D12_DESCRIPTOR_HEAP_DESC shaderViews3{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3,
            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        const D3D12_DESCRIPTOR_HEAP_DESC shaderViews12{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 12,
            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        for(auto &slot : frameSlots)
        {
            check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)),
                "DX12 graphics allocator");
            for(auto *heap : {std::addressof(slot.color), std::addressof(slot.downsample),
                    std::addressof(slot.postProcess), std::addressof(slot.fsr),
                    std::addressof(slot.ambient), std::addressof(slot.baselineAmbient), std::addressof(slot.blit)})
                check(device->CreateDescriptorHeap(&shaderViews3, IID_PPV_ARGS(heap->GetAddressOf())),
                    "DX12 frame slot descriptors");
            for(auto *heap : {std::addressof(slot.motion), std::addressof(slot.srMotion)})
                check(device->CreateDescriptorHeap(&shaderViews12, IID_PPV_ARGS(heap->GetAddressOf())),
                    "DX12 motion slot descriptors");
        }
        graphicsAllocator = frameSlots.front().allocator;
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, graphicsAllocator.Get(), nullptr,
            IID_PPV_ARGS(&graphicsCommands)), "DX12 graphics commands");
        check(graphicsCommands->Close(), "DX12 initial graphics close");
        initializeColorPipeline();
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&graphicsFence)), "DX12 graphics fence");
        completedEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if(!completedEvent)
            check(HRESULT_FROM_WIN32(GetLastError()), "DX12 fence event");
        dlssFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if(!dlssFenceEvent)
            check(HRESULT_FROM_WIN32(GetLastError()), "DX12 DLSS fence event");
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
        fgPaceTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if(!fgPaceTimer)
            fgPaceTimer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        balancedPaceTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if(!balancedPaceTimer)
            balancedPaceTimer = CreateWaitableTimerW(nullptr, TRUE, nullptr);

        capturePath = QCoreApplication::instance()->property("ps5linkDx12FramePath").toString();
        captureFrame = qMax(1u, QCoreApplication::instance()->property("ps5linkDx12FrameNumber").toUInt());
        upscaleHeight = QCoreApplication::instance()->property("ps5linkUpscaleHeight").toInt();
        generatedCapturePath = QCoreApplication::instance()->property("ps5linkDx12GeneratedPath").toString();
        presentMode = QCoreApplication::instance()->property("ps5linkDx12PresentMode").isValid()
            ? QCoreApplication::instance()->property("ps5linkDx12PresentMode").toInt() : 1;
        generationMode = QCoreApplication::instance()->property("ps5linkFrameGeneration").toInt();
        fsrEnabled = QCoreApplication::instance()->property("ps5linkFsr").toBool();
        if(QCoreApplication::instance()->property("ps5linkSharpness").isValid())
            sharpnessSetting.store(QCoreApplication::instance()->property("ps5linkSharpness").toInt());
        setVsrQuality(QCoreApplication::instance()->property("ps5linkVsrQuality").toInt());
        setSrModel(QCoreApplication::instance()->property("ps5linkDlssSr").toInt());
    }

    void resetSr()
    {
        if(sr)
        {
            destroySr(sr);
            sr = nullptr;
        }
        for(auto &texture : srInput)
            texture.Reset();
        for(auto &texture : srLumaHistory)
            texture.Reset();
        srCoarse.Reset();
        srRefined.Reset();
        srMotion.Reset();
        srDepth.Reset();
        srOutput.Reset();
        srHistoryValid = false;
    }

    void setSrModel(int model)
    {
        if(srModel == model && !srFailed)
            return;
        waitForFence(fenceValue);
        resetSr();
        srModel = model;
        srFailed = false;
        srFramesAtSelection = srFrames;
        historyValid = false;
        dlssFailed = false;
        dlssFramesAtSelection = dlssFrames;
        fsrFgFailed = false;
        fsrFgFramesAtSelection = fsrFgFrames;
        if(model)
        {
            releaseGenerationResources();
            releaseVsr(dlss == nullptr);
            enhanced.Reset();
        }
    }

    void setVsrQuality(int quality)
    {
        if(vsrQuality == quality)
            return;
        vsrQuality = quality;
        if(quality)
            vsrError = 0;
        else if(vsr || enhanced)
        {
            waitForFence(fenceValue, 5000);
            releaseVsr(dlss == nullptr);
            enhanced.Reset();
        }
        historyValid = false;
        if(vsrQuality && !createVsr)
        {
            bridge.setFileName(QCoreApplication::applicationDirPath() + "/PS5LinkVsr.dll");
            createVsr = reinterpret_cast<CreateVsr>(bridge.resolve("Ps5LinkDx12VsrCreate"));
            createVsrEx = reinterpret_cast<CreateVsrEx>(bridge.resolve("Ps5LinkDx12VsrCreateEx"));
            evaluateVsr = reinterpret_cast<EvaluateVsr>(bridge.resolve("Ps5LinkDx12VsrEvaluate"));
            destroyVsr = reinterpret_cast<DestroyVsr>(bridge.resolve("Ps5LinkDx12VsrDestroy"));
            destroyVsrEx = reinterpret_cast<DestroyVsrEx>(bridge.resolve("Ps5LinkDx12VsrDestroyEx"));
            if(!createVsr || !evaluateVsr || !destroyVsr)
            {
                std::fprintf(stderr, "DX12 RTX VSR bridge unavailable. Using DX12 shader scaling.\n");
                createVsr = nullptr;
                createVsrEx = nullptr;
                evaluateVsr = nullptr;
                destroyVsr = nullptr;
                destroyVsrEx = nullptr;
                vsrError = unsigned(HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND));
                vsrQuality = 0;
            }
        }
    }

    void initializeColorPipeline()
    {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
        D3D12_ROOT_PARAMETER bindings[2]{};
        bindings[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        bindings[0].DescriptorTable = {1, &range};
        bindings[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        bindings[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        bindings[1].Constants = {0, 0, sizeof(Conversion) / sizeof(float)};
        bindings[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MaxAnisotropy = 1;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        const D3D12_ROOT_SIGNATURE_DESC signature{2, bindings, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> serialized;
        check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr),
            "DX12 root signature serialization");
        check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature)), "DX12 root signature");

        const auto vertex = compileShader(dx12_shaders::kColorShader, "vertexMain", "vs_5_0");
        const auto pixel = compileShader(dx12_shaders::kColorShader, "pixelMain", "ps_5_0");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};
        state.pRootSignature = rootSignature.Get();
        state.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
        state.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
        auto &blend = state.BlendState.RenderTarget[0];
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        state.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        state.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        state.RasterizerState.DepthClipEnable = TRUE;
        state.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        state.SampleMask = UINT_MAX;
        state.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        state.NumRenderTargets = 1;
        state.RTVFormats[0] = rgbFormat;
        state.SampleDesc.Count = 1;
        check(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&colorPipeline)), "DX12 color pipeline");
        state.RTVFormats[0] = highPrecisionFormat;
        check(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&colorPipelineFp16)), "DX12 FP16 color pipeline");

        const D3D12_DESCRIPTOR_HEAP_DESC renderViews{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1,
            D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        check(device->CreateDescriptorHeap(&renderViews, IID_PPV_ARGS(&renderTargetView)), "DX12 target view");

        const auto downsample = compileShader(dx12_shaders::kDownsampleShader, "downsample", "ps_5_0");
        state.PS = {downsample->GetBufferPointer(), downsample->GetBufferSize()};
        state.RTVFormats[0] = rgbFormat;
        check(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&downsamplePipeline)), "DX12 downsample pipeline");
        state.RTVFormats[0] = highPrecisionFormat;
        check(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&downsamplePipelineFp16)), "DX12 FP16 downsample pipeline");

        const auto blit = compileShader(dx12_shaders::kBlitShader, "blit", "ps_5_0");
        state.PS = {blit->GetBufferPointer(), blit->GetBufferSize()};
        state.RTVFormats[0] = rgbFormat;
        check(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&blitPipeline)), "DX12 blit pipeline");
        state.RTVFormats[0] = highPrecisionFormat;
        check(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&blitPipelineFp16)), "DX12 FP16 blit pipeline");

        state.RTVFormats[0] = swapchainFormat;
        const QString dataDir = QFileInfo(QSettings(QSettings::IniFormat, QSettings::UserScope,
            QCoreApplication::organizationName(), QCoreApplication::applicationName()).fileName()).absolutePath();
        const QString customShaderPath = QDir(dataDir).filePath(QStringLiteral("Shaders/postprocess.hlsl"));
        if(QFileInfo::exists(customShaderPath))
        {
            QFile shaderFile(customShaderPath);
            if(shaderFile.open(QIODevice::ReadOnly | QIODevice::Text))
            {
                const QByteArray customSrc = shaderFile.readAll();
                try
                {
                    auto customPost = compileShader(customSrc.constData(), "postProcess", "ps_5_0");
                    state.PS = {customPost->GetBufferPointer(), customPost->GetBufferSize()};
                    check(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&postProcessPipeline)),
                        "DX12 custom post-process pipeline");
                    std::printf("[PS5Link] Loaded custom DX12 post-process shader: %s\n",
                        customShaderPath.toUtf8().constData());
                }
                catch(const std::exception &err)
                {
                    postProcessPipeline.Reset();
                    std::fprintf(stderr, "[PS5Link] Custom postprocess.hlsl failed (%s). Using built-in CAS.\n", err.what());
                }
            }
        }
        if(!postProcessPipeline)
        {
            auto post = compileShader(dx12_shaders::kPostProcessShader, "postProcess", "ps_5_0");
            state.PS = {post->GetBufferPointer(), post->GetBufferSize()};
            check(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&postProcessPipeline)), "DX12 post-process pipeline");
        }

        const auto fsr = compileShader(dx12_shaders::kFsrEasuShader, "fsrEasu", "ps_5_0");
        state.PS = {fsr->GetBufferPointer(), fsr->GetBufferSize()};
        state.RTVFormats[0] = rgbFormat;
        check(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&fsrPipeline)), "DX12 FSR EASU pipeline");
        state.RTVFormats[0] = highPrecisionFormat;
        check(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&fsrPipelineFp16)), "DX12 FP16 FSR EASU pipeline");

        const auto ambient = compileShader(dx12_shaders::kAmbientShader, "ambientBg", "ps_5_0");
        state.PS = {ambient->GetBufferPointer(), ambient->GetBufferSize()};
        state.RTVFormats[0] = rgbFormat;
        check(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&ambientPipeline)), "DX12 ambient pipeline");
        state.RTVFormats[0] = highPrecisionFormat;
        check(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&ambientPipelineFp16)), "DX12 FP16 ambient pipeline");
    }

    void initializeInterpolation()
    {
        if(dlssMotionPipeline)
            return;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};
        state.pRootSignature = rootSignature.Get();
        const auto vertex = compileShader(dx12_shaders::kColorShader, "vertexMain", "vs_5_0");
        const auto lumaDown = compileShader(dx12_shaders::kInterpolationShader, "downsampleLuma", "ps_5_0");
        const auto estimate = compileShader(dx12_shaders::kInterpolationShader, "estimateMotionCoarse", "ps_5_0");
        const auto refine = compileShader(dx12_shaders::kInterpolationShader, "refineMotion", "ps_5_0");
        const auto warp = compileShader(dx12_shaders::kInterpolationShader, "interpolate", "ps_5_0");
        state.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
        auto &blend = state.BlendState.RenderTarget[0];
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        state.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        state.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        state.RasterizerState.DepthClipEnable = TRUE;
        state.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        state.SampleMask = UINT_MAX;
        state.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        state.NumRenderTargets = 1;
        state.SampleDesc.Count = 1;
        state.PS = {lumaDown->GetBufferPointer(), lumaDown->GetBufferSize()};
        state.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
        check(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&lumaDownsamplePipeline)), "DX12 luma pyramid pipeline");
        state.PS = {estimate->GetBufferPointer(), estimate->GetBufferSize()};
        state.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        check(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&motionPipeline)), "DX12 motion estimation pipeline");
        state.PS = {refine->GetBufferPointer(), refine->GetBufferSize()};
        check(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&refineMotionPipeline)), "DX12 motion refinement pipeline");
        state.PS = {warp->GetBufferPointer(), warp->GetBufferSize()};
        state.RTVFormats[0] = rgbFormat;
        check(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&interpolationPipeline)), "DX12 interpolation pipeline");
        state.RTVFormats[0] = highPrecisionFormat;
        check(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&interpolationPipelineFp16)), "DX12 FP16 interpolation pipeline");
        const auto vectors = compileShader(dx12_shaders::kInterpolationShader, "dlssMotion", "ps_5_0");
        state.PS = {vectors->GetBufferPointer(), vectors->GetBufferSize()};
        state.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;
        check(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&dlssMotionPipeline)), "DX12 DLSS motion pipeline");
    }

    void detectDisplayCapabilities()
    {
        displayHdrCapable = false;
        display10Bit = false;
        if(!swapchain)
            return;
        if(factory && !factory->IsCurrent())
        {
            ComPtr<IDXGIFactory4> fresh;
            if(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fresh))))
                factory = fresh;
        }
        bool probed = false;
        if(factory && swapchainHwnd)
        {
            const HMONITOR mon = MonitorFromWindow(swapchainHwnd, MONITOR_DEFAULTTONEAREST);
            ComPtr<IDXGIAdapter1> adapter;
            if(mon && SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) && adapter)
            {
                for(UINT idx = 0; ; ++idx)
                {
                    ComPtr<IDXGIOutput> out;
                    if(adapter->EnumOutputs(idx, &out) == DXGI_ERROR_NOT_FOUND)
                        break;
                    ComPtr<IDXGIOutput6> out6;
                    DXGI_OUTPUT_DESC1 desc1{};
                    if(out && SUCCEEDED(out.As(&out6)) && SUCCEEDED(out6->GetDesc1(&desc1)) && desc1.Monitor == mon)
                    {
                        displayHdrCapable = (desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
                        display10Bit = desc1.BitsPerColor >= 10;
                        probed = true;
                        break;
                    }
                }
            }
        }
        if(!probed)
        {
            ComPtr<IDXGIOutput> output;
            ComPtr<IDXGIOutput6> output6;
            if(SUCCEEDED(swapchain->GetContainingOutput(&output)) && output && SUCCEEDED(output.As(&output6)))
            {
                DXGI_OUTPUT_DESC1 desc1{};
                if(SUCCEEDED(output6->GetDesc1(&desc1)))
                {
                    displayHdrCapable = (desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
                    display10Bit = desc1.BitsPerColor >= 10;
                }
            }
        }
        hdrCheckTimer.start();
    }

    void updateSwapchainColorSpace(bool wantHdr10)
    {
        if(!swapchain)
            return;
        const DXGI_COLOR_SPACE_TYPE target = wantHdr10
            ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
            : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        if(target == currentSwapchainColorSpace)
            return;
        UINT support = 0;
        if(SUCCEEDED(swapchain->CheckColorSpaceSupport(target, &support))
            && (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)
            && SUCCEEDED(swapchain->SetColorSpace1(target)))
        {
            currentSwapchainColorSpace = target;
            std::printf("DX12 swapchain color space: %s.\n", wantHdr10 ? "HDR10 (BT.2020 PQ)" : "SDR (BT.709 10-bit)");
        }
    }

    void releaseSwapchain()
    {
        try { waitForFence(fenceValue, 5000); } catch(const std::exception &) {}
        swapchain.Reset();
        swapchainHwnd = nullptr;
        currentSwapchainColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        hasPresentPermit = false;
        swapchainOccluded = false;
        if(frameLatency)
        {
            CloseHandle(frameLatency);
            frameLatency = nullptr;
        }
    }

    void resize(HWND hwnd, QSize size)
    {
        const bool hwndChanged = swapchain && hwnd != swapchainHwnd;
        if(!hwndChanged && swapchain && size == outputSize)
            return;
        const bool sizeChanged = (size != outputSize);
        waitForFence(fenceValue);
        if(hwndChanged)
        {
            // Qt may recreate the native HWND when toggling FramelessWindowHint (Borderless/Windowed).
            releaseSwapchain();
        }
        swapchainOccluded = false;
        if(swapchain)
            check(swapchain->ResizeBuffers(0, size.width(), size.height(), DXGI_FORMAT_UNKNOWN, swapchainFlags), "DX12 resize");
        else
        {
            if(factory && !factory->IsCurrent())
            {
                ComPtr<IDXGIFactory4> fresh;
                if(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fresh))))
                    factory = fresh;
            }
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = size.width();
            desc.Height = size.height();
            desc.Format = swapchainFormat;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = frameSlots.size();
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            desc.Flags = swapchainFlags;
            ComPtr<IDXGISwapChain1> created;
            check(factory->CreateSwapChainForHwnd(graphicsQueue.Get(), hwnd, &desc, nullptr, nullptr,
                &created), "DX12 swapchain");
            check(created.As(&swapchain), "DX12 swapchain interface");
            swapchainHwnd = hwnd;
            check(swapchain->SetMaximumFrameLatency(1), "DX12 maximum frame latency");
            frameLatency = swapchain->GetFrameLatencyWaitableObject();
            if(!frameLatency)
                throw std::runtime_error("DX12 frame latency object unavailable");
            check(factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER), "DXGI window association");
            currentSwapchainColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
            updateSwapchainColorSpace(false);
            detectDisplayCapabilities();
            std::printf("DX12 10-bit waitable swapchain: %zu buffers, max latency 1, tearing %s, display %s%s.\n",
                frameSlots.size(), tearingSupported ? "supported" : "unavailable",
                display10Bit ? "10-bit" : "8-bit", displayHdrCapable ? " HDR" : " SDR");
        }
        detectDisplayCapabilities();

        outputSize = size;
        if(sizeChanged)
        {
            resetSr();
            enhanced.Reset();
            postInput.Reset();
            splitBaseline.Reset();
            interpOutput.Reset();
            lastPresented.Reset();
            lastPresentedSize = QSize();
            for(auto &texture : videoHistory)
                texture.Reset();
            for(auto &texture : videoLumaHistory)
                texture.Reset();
            videoHistoryFormat = DXGI_FORMAT_UNKNOWN;
            motionTexture.Reset();
            motionRefinedTexture.Reset();
            dlssMotionTexture.Reset();
            dlssDepth.Reset();
            dlssGenerated.Reset();
            dlssReal.Reset();
        }
        if(dlss && (hwndChanged || dlssSize != size))
            releaseDlss(vsr == nullptr);
        if(fsrFg && (hwndChanged || fsrFgSize != size))
            releaseFsrFg();
        historyValid = false;
    }

    ComPtr<ID3D12Resource> makeTexture(QSize size, D3D12_RESOURCE_FLAGS flags, DXGI_FORMAT format = rgbFormat)
    {
        D3D12_RESOURCE_DESC image{};
        image.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        image.Width = size.width();
        image.Height = size.height();
        image.DepthOrArraySize = 1;
        image.MipLevels = 1;
        image.Format = format;
        image.SampleDesc.Count = 1;
        image.Flags = flags;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        ComPtr<ID3D12Resource> texture;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &image,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture)), "DX12 RGB video surface");
        return texture;
    }

    void draw(const AVFrame &frame, const AVD3D12VAFrame &decoded, ID3D12Resource *target, QSize targetSize, bool clearTarget = true)
    {
        auto *source = decoded.texture;
        if(!source)
            throw std::runtime_error("D3D12VA returned an empty video texture");
        const auto description = source->GetDesc();
        const bool p010 = description.Format == DXGI_FORMAT_P010;
        if((!p010 && description.Format != DXGI_FORMAT_NV12)
            || (description.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)
            || decoded.subresource_index < 0 || decoded.subresource_index >= description.DepthOrArraySize)
            throw std::runtime_error("DX12 replay requires a shader-readable NV12 or P010 decoder texture");

        auto view = views->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC shaderView{};
        shaderView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        shaderView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        shaderView.Texture2DArray.MipLevels = 1;
        shaderView.Texture2DArray.ArraySize = 1;
        shaderView.Texture2DArray.FirstArraySlice = decoded.subresource_index;
        shaderView.Format = p010 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
        device->CreateShaderResourceView(source, &shaderView, view);
        view.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        shaderView.Texture2DArray.PlaneSlice = 1;
        shaderView.Format = p010 ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
        device->CreateShaderResourceView(source, &shaderView, view);

        const auto rtv = renderTargetView->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(target, nullptr, rtv);
        const bool fp16 = target->GetDesc().Format == highPrecisionFormat;
        const UINT slice = decoded.subresource_index;
        const UINT planeStride = description.MipLevels * description.DepthOrArraySize;
        auto *commands = graphicsCommands.Get();
        for(UINT plane : {slice, slice + planeStride})
            transition(commands, source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, plane);
        transition(commands, target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);

        const QSize sourceSize(frame.width, frame.height);
        const QSize area = aspect == Aspect::Stretch ? targetSize : sourceSize.scaled(targetSize,
            aspect == Aspect::Fill ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio);
        const D3D12_VIEWPORT viewport{(targetSize.width() - area.width()) / 2.0f,
            (targetSize.height() - area.height()) / 2.0f, float(area.width()), float(area.height()), 0, 1};
        const D3D12_RECT scissor{0, 0, targetSize.width(), targetSize.height()};
        const float black[4] = {0, 0, 0, 1};
        commands->SetPipelineState((fp16 ? colorPipelineFp16 : colorPipeline).Get());
        commands->SetGraphicsRootSignature(rootSignature.Get());
        ID3D12DescriptorHeap *heap = views.Get();
        commands->SetDescriptorHeaps(1, &heap);
        commands->SetGraphicsRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
        const auto conversion = colorConversion(frame, description, hdrPaperWhite.load(), hdrPassthroughActive);
        commands->SetGraphicsRoot32BitConstants(1, sizeof(conversion) / sizeof(float), &conversion, 0);
        commands->RSSetScissorRects(1, &scissor);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        if(clearTarget)
            commands->ClearRenderTargetView(rtv, black, 0, nullptr);
        commands->RSSetViewports(1, &viewport);
        commands->DrawInstanced(3, 1, 0, 0);

        for(UINT plane : {slice, slice + planeStride})
            transition(commands, source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON, plane);
        transition(commands, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    }

    void recordFsr(ID3D12Resource *source, ID3D12Resource *target, QSize sourceSize, QSize targetSize)
    {
        auto *commands = graphicsCommands.Get();
        const bool fp16 = target->GetDesc().Format == highPrecisionFormat;
        device->CreateShaderResourceView(source, nullptr, fsrViews->GetCPUDescriptorHandleForHeapStart());
        const auto rtv = renderTargetView->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(target, nullptr, rtv);
        transition(commands, source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(commands, target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const QSize area = aspect == Aspect::Stretch ? targetSize : sourceSize.scaled(targetSize,
            aspect == Aspect::Fill ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio);
        const D3D12_RECT scissor{0, 0, targetSize.width(), targetSize.height()};
        const float black[4] = {0, 0, 0, 1};
        commands->SetGraphicsRootSignature(rootSignature.Get());
        commands->RSSetScissorRects(1, &scissor);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        commands->ClearRenderTargetView(rtv, black, 0, nullptr);
        if(aspect == Aspect::Ambient && area != targetSize)
        {
            device->CreateShaderResourceView(source, nullptr, ambientViews->GetCPUDescriptorHandleForHeapStart());
            auto *ambHeap = ambientViews.Get();
            const D3D12_VIEWPORT fullVp{0, 0, float(targetSize.width()), float(targetSize.height()), 0, 1};
            const float left = (targetSize.width() - area.width()) / (2.0f * targetSize.width());
            const float top = (targetSize.height() - area.height()) / (2.0f * targetSize.height());
            const float ambParams[8] = {
                12.0f / sourceSize.width(), 12.0f / sourceSize.height(),
                float(ambientStyle.load()), 0.0f,
                left, top, 1.0f - left, 1.0f - top
            };
            commands->SetPipelineState((fp16 ? ambientPipelineFp16 : ambientPipeline).Get());
            commands->SetDescriptorHeaps(1, &ambHeap);
            commands->SetGraphicsRootDescriptorTable(0, ambHeap->GetGPUDescriptorHandleForHeapStart());
            commands->SetGraphicsRoot32BitConstants(1, 8, ambParams, 0);
            commands->RSSetViewports(1, &fullVp);
            commands->DrawInstanced(3, 1, 0, 0);
        }
        const float geom[4] = {
            float(sourceSize.width()), float(sourceSize.height()),
            1.0f / sourceSize.width(), 1.0f / sourceSize.height()
        };
        const D3D12_VIEWPORT viewport{(targetSize.width() - area.width()) / 2.0f,
            (targetSize.height() - area.height()) / 2.0f, float(area.width()), float(area.height()), 0, 1};
        auto *heap = fsrViews.Get();
        commands->SetPipelineState((fp16 ? fsrPipelineFp16 : fsrPipeline).Get());
        commands->SetDescriptorHeaps(1, &heap);
        commands->SetGraphicsRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        commands->SetGraphicsRoot32BitConstants(1, 4, geom, 0);
        commands->RSSetViewports(1, &viewport);
        commands->DrawInstanced(3, 1, 0, 0);
        transition(commands, source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        transition(commands, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    }

    void recordAmbientBackdrop(ID3D12Resource *sourceRgb, ID3D12Resource *target,
        QSize sourceSize, QSize targetSize, ID3D12DescriptorHeap *heap)
    {
        auto *commands = graphicsCommands.Get();
        const bool fp16 = target->GetDesc().Format == highPrecisionFormat;
        device->CreateShaderResourceView(sourceRgb, nullptr, heap->GetCPUDescriptorHandleForHeapStart());
        const auto rtv = renderTargetView->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(target, nullptr, rtv);
        transition(commands, sourceRgb, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(commands, target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const QSize area = sourceSize.scaled(targetSize, Qt::KeepAspectRatio);
        const float left = (targetSize.width() - area.width()) / (2.0f * targetSize.width());
        const float top = (targetSize.height() - area.height()) / (2.0f * targetSize.height());
        const D3D12_VIEWPORT fullVp{0, 0, float(targetSize.width()), float(targetSize.height()), 0, 1};
        const D3D12_RECT scissor{0, 0, targetSize.width(), targetSize.height()};
        const float ambParams[8] = {
            12.0f / sourceSize.width(), 12.0f / sourceSize.height(),
            float(ambientStyle.load()), 0.0f,
            left, top, 1.0f - left, 1.0f - top
        };
        commands->SetPipelineState((fp16 ? ambientPipelineFp16 : ambientPipeline).Get());
        commands->SetGraphicsRootSignature(rootSignature.Get());
        commands->SetDescriptorHeaps(1, &heap);
        commands->SetGraphicsRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        commands->SetGraphicsRoot32BitConstants(1, 8, ambParams, 0);
        commands->RSSetViewports(1, &fullVp);
        commands->RSSetScissorRects(1, &scissor);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        commands->DrawInstanced(3, 1, 0, 0);
        transition(commands, sourceRgb, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        transition(commands, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    }

    ComPtr<ID3D12Resource> recordReadback(ID3D12Resource *backBuffer, D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint)
    {
        const auto description = backBuffer->GetDesc();
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "DX12 readback buffer");

        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = backBuffer;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        auto *commands = graphicsCommands.Get();
        transition(commands, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        transition(commands, backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        return readback;
    }

    void saveReadback(ID3D12Resource *resource, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &layout, QSize size, QString &path)
    {
        void *mapped = nullptr;
        check(resource->Map(0, nullptr, &mapped), "DX12 map captured frame");
        const auto unmap = qScopeGuard([&] {
            const D3D12_RANGE noWrites{0, 0};
            resource->Unmap(0, &noWrites);
        });
        const auto *pixels = static_cast<const uchar *>(mapped) + layout.Offset;
        QImage image;
        if(layout.Footprint.Format == DXGI_FORMAT_R10G10B10A2_UNORM)
        {
            image = QImage(size, QImage::Format_RGBA8888);
            for(int y = 0; y < size.height(); ++y)
            {
                const auto *src = reinterpret_cast<const uint32_t *>(pixels + size_t(y) * layout.Footprint.RowPitch);
                auto *dst = image.scanLine(y);
                for(int x = 0; x < size.width(); ++x)
                {
                    const uint32_t p = src[x];
                    dst[x * 4 + 0] = static_cast<uchar>(((p & 0x3ffu) * 255u + 511u) / 1023u);
                    dst[x * 4 + 1] = static_cast<uchar>((((p >> 10) & 0x3ffu) * 255u + 511u) / 1023u);
                    dst[x * 4 + 2] = static_cast<uchar>((((p >> 20) & 0x3ffu) * 255u + 511u) / 1023u);
                    dst[x * 4 + 3] = 255u;
                }
            }
        }
        else
            image = QImage(pixels, size.width(), size.height(), layout.Footprint.RowPitch, QImage::Format_RGBA8888);
        if(!image.save(path, "PNG"))
            throw std::runtime_error("Could not save the DX12 output PNG");
        path.clear();
        std::printf("Saved DX12 output frame from the back buffer.\n");
    }

    void copyTo(ID3D12Resource *source, ID3D12Resource *target)
    {
        auto *commands = graphicsCommands.Get();
        transition(commands, source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(commands, target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        commands->CopyResource(target, source);
        transition(commands, target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        transition(commands, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }

    void outputEnhanced(ID3D12Resource *source, ID3D12Resource *target, QSize videoArea, QSize size)
    {
        const bool sameFormat = source->GetDesc().Format == target->GetDesc().Format;
        if(upscaleSize == size && videoArea == size && sameFormat)
        {
            copyTo(source, target);
            return;
        }
        if(upscaleSize.width() <= videoArea.width() && upscaleSize.height() <= videoArea.height()
            && upscaleSize != videoArea)
        {
            // Stream-size (1:1 DLAA) output -> upscale cleanly to display size using FSR 1.0 EASU.
            recordFsr(source, target, upscaleSize, size);
            return;
        }
        auto *commands = graphicsCommands.Get();
        const bool fp16 = target->GetDesc().Format == highPrecisionFormat;
        const bool needDownsample = upscaleSize.width() > videoArea.width() || upscaleSize.height() > videoArea.height();
        auto *heap = needDownsample ? downsampleViews.Get() : blitViews.Get();
        device->CreateShaderResourceView(source, nullptr, heap->GetCPUDescriptorHandleForHeapStart());
        const auto rtv = renderTargetView->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(target, nullptr, rtv);
        transition(commands, source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(commands, target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_RECT scissor{0, 0, size.width(), size.height()};
        const float black[4] = {0, 0, 0, 1};
        commands->SetGraphicsRootSignature(rootSignature.Get());
        commands->RSSetScissorRects(1, &scissor);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        if(videoArea != size)
        {
            commands->ClearRenderTargetView(rtv, black, 0, nullptr);
            if(aspect == Aspect::Ambient)
            {
                device->CreateShaderResourceView(source, nullptr, ambientViews->GetCPUDescriptorHandleForHeapStart());
                auto *ambHeap = ambientViews.Get();
                const D3D12_VIEWPORT fullVp{0, 0, float(size.width()), float(size.height()), 0, 1};
                const float left = (size.width() - videoArea.width()) / (2.0f * size.width());
                const float top = (size.height() - videoArea.height()) / (2.0f * size.height());
                const float ambParams[8] = {
                    12.0f / upscaleSize.width(), 12.0f / upscaleSize.height(),
                    float(ambientStyle.load()), 0.0f,
                    left, top, 1.0f - left, 1.0f - top
                };
                commands->SetPipelineState((fp16 ? ambientPipelineFp16 : ambientPipeline).Get());
                commands->SetDescriptorHeaps(1, &ambHeap);
                commands->SetGraphicsRootDescriptorTable(0, ambHeap->GetGPUDescriptorHandleForHeapStart());
                commands->SetGraphicsRoot32BitConstants(1, 8, ambParams, 0);
                commands->RSSetViewports(1, &fullVp);
                commands->DrawInstanced(3, 1, 0, 0);
            }
        }
        const D3D12_VIEWPORT viewport{(size.width() - videoArea.width()) / 2.0f,
            (size.height() - videoArea.height()) / 2.0f, float(videoArea.width()), float(videoArea.height()), 0, 1};
        commands->SetPipelineState(needDownsample
            ? (fp16 ? downsamplePipelineFp16 : downsamplePipeline).Get()
            : (fp16 ? blitPipelineFp16 : blitPipeline).Get());
        commands->SetDescriptorHeaps(1, &heap);
        commands->SetGraphicsRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        if(needDownsample)
        {
            const float ratio[] = {float(upscaleSize.width()) / videoArea.width(), float(upscaleSize.height()) / videoArea.height()};
            commands->SetGraphicsRoot32BitConstants(1, 2, ratio, 0);
        }
        commands->RSSetViewports(1, &viewport);
        commands->DrawInstanced(3, 1, 0, 0);
        transition(commands, source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        transition(commands, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    }

    void recordPostProcess(ID3D12Resource *enhancedTex, ID3D12Resource *baselineTex,
        ID3D12Resource *target, QSize size, float sharpness, bool split, float splitX, unsigned frameSeed)
    {
        auto *commands = graphicsCommands.Get();
        const auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto handle = postProcessViews->GetCPUDescriptorHandleForHeapStart();
        device->CreateShaderResourceView(enhancedTex, nullptr, handle);
        handle.ptr += increment;
        device->CreateShaderResourceView(baselineTex ? baselineTex : enhancedTex, nullptr, handle);
        const auto rtv = renderTargetView->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(target, nullptr, rtv);
        transition(commands, enhancedTex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if(baselineTex && baselineTex != enhancedTex)
            transition(commands, baselineTex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(commands, target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float ditherStep = (hdrPassthroughActive || display10Bit) ? (0.75f / 1023.0f) : (0.75f / 255.0f);
        const float params[12] = {
            sharpness, splitX, split ? 1.0f : 0.0f, ditherEnabled.load() ? 1.0f : 0.0f,
            colorContrast.load() / 100.0f, colorSaturation.load() / 100.0f,
            colorGamma.load() / 100.0f, float(frameSeed & 63u),
            debandEnabled.load() ? 1.0f : 0.0f, ditherStep,
            hdrPassthroughActive ? 1.0f : 0.0f, 0.0f
        };
        const D3D12_VIEWPORT viewport{0, 0, float(size.width()), float(size.height()), 0, 1};
        const D3D12_RECT scissor{0, 0, size.width(), size.height()};
        auto *heap = postProcessViews.Get();
        commands->SetPipelineState(postProcessPipeline.Get());
        commands->SetGraphicsRootSignature(rootSignature.Get());
        commands->SetDescriptorHeaps(1, &heap);
        commands->SetGraphicsRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        commands->SetGraphicsRoot32BitConstants(1, 12, params, 0);
        commands->RSSetViewports(1, &viewport);
        commands->RSSetScissorRects(1, &scissor);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        commands->DrawInstanced(3, 1, 0, 0);
        transition(commands, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        if(baselineTex && baselineTex != enhancedTex)
            transition(commands, baselineTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        transition(commands, enhancedTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    }

    void recordMotion(const ComPtr<ID3D12Resource> (&history)[2], ComPtr<ID3D12Resource> (&luma)[2],
        ComPtr<ID3D12Resource> &coarse, ComPtr<ID3D12Resource> &refined,
        ID3D12DescriptorHeap *motionHeap, ID3D12Resource *target, QSize size, bool forDlss)
    {
        const QSize lumaSize(std::max(1, (size.width() + 3) / 4), std::max(1, (size.height() + 3) / 4));
        const QSize coarseSize((size.width() + 15) / 16, (size.height() + 15) / 16);
        const QSize refinedSize((size.width() + 7) / 8, (size.height() + 7) / 8);
        for(auto &tex : luma)
            if(!tex)
                tex = makeTexture(lumaSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, DXGI_FORMAT_R8_UNORM);
        if(!coarse)
            coarse = makeTexture(coarseSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, DXGI_FORMAT_R16G16B16A16_FLOAT);
        if(!refined)
            refined = makeTexture(refinedSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, DXGI_FORMAT_R16G16B16A16_FLOAT);

        const auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu = motionHeap->GetCPUDescriptorHandleForHeapStart();
        // Table 0 (slots 0..2): prev, curr, coarse (Pass 0a luma[0] & Pass 2 refine)
        for(auto *texture : {history[0].Get(), history[1].Get(), coarse.Get()})
        {
            device->CreateShaderResourceView(texture, nullptr, cpu);
            cpu.ptr += increment;
        }
        // Table 1 (slots 3..5): curr, prev, curr (Pass 0b luma[1]; avoids referencing coarse before Pass 1)
        for(auto *texture : {history[1].Get(), history[0].Get(), history[1].Get()})
        {
            device->CreateShaderResourceView(texture, nullptr, cpu);
            cpu.ptr += increment;
        }
        // Table 2 (slots 6..8): luma[0], luma[1], luma[0] (Pass 1 coarse search; avoids SRV/RTV aliasing on coarse)
        for(auto *texture : {luma[0].Get(), luma[1].Get(), luma[0].Get()})
        {
            device->CreateShaderResourceView(texture, nullptr, cpu);
            cpu.ptr += increment;
        }
        // Table 3 (slots 9..11): prev, curr, refined (Pass 3 warp or dlssMotion)
        for(auto *texture : {history[0].Get(), history[1].Get(), refined.Get()})
        {
            device->CreateShaderResourceView(texture, nullptr, cpu);
            cpu.ptr += increment;
        }
        auto gpuTable0 = motionHeap->GetGPUDescriptorHandleForHeapStart();
        auto gpuTable1 = gpuTable0; gpuTable1.ptr += 3 * increment;
        auto gpuTable2 = gpuTable0; gpuTable2.ptr += 6 * increment;
        auto gpuTable3 = gpuTable0; gpuTable3.ptr += 9 * increment;

        auto *commands = graphicsCommands.Get();
        for(const auto &texture : history)
            transition(commands, texture.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        commands->SetGraphicsRootSignature(rootSignature.Get());
        commands->SetDescriptorHeaps(1, &motionHeap);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const auto drawPass = [&](ID3D12PipelineState *state, D3D12_GPU_DESCRIPTOR_HANDLE table,
            const float (&constants)[4], ID3D12Resource *dst, QSize dimensions) {
            const auto rtv = renderTargetView->GetCPUDescriptorHandleForHeapStart();
            device->CreateRenderTargetView(dst, nullptr, rtv);
            const D3D12_VIEWPORT viewport{0, 0, float(dimensions.width()), float(dimensions.height()), 0, 1};
            const D3D12_RECT scissor{0, 0, dimensions.width(), dimensions.height()};
            commands->SetPipelineState(state);
            commands->SetGraphicsRootDescriptorTable(0, table);
            commands->SetGraphicsRoot32BitConstants(1, 4, constants, 0);
            commands->RSSetViewports(1, &viewport);
            commands->RSSetScissorRects(1, &scissor);
            commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            commands->DrawInstanced(3, 1, 0, 0);
        };

        const float fullConstants[4] = {
            1.0f / size.width(), 1.0f / size.height(),
            1.0f / lumaSize.width(), 1.0f / lumaSize.height()
        };
        const float pyramidConstants[4] = {
            1.0f / lumaSize.width(), 1.0f / lumaSize.height(),
            1.0f / coarseSize.width(), 1.0f / coarseSize.height()
        };
        const float coarseConstants[4] = {
            1.0f / size.width(), 1.0f / size.height(),
            1.0f / coarseSize.width(), 1.0f / coarseSize.height()
        };
        const float refinedConstants[4] = {
            1.0f / size.width(), 1.0f / size.height(),
            1.0f / refinedSize.width(), 1.0f / refinedSize.height()
        };

        // Pass 0: anti-aliased 1/4-res luma pyramid for prev & curr
        transition(commands, luma[0].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        drawPass(lumaDownsamplePipeline.Get(), gpuTable0, fullConstants, luma[0].Get(), lumaSize);
        transition(commands, luma[0].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        transition(commands, luma[1].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        drawPass(lumaDownsamplePipeline.Get(), gpuTable1, fullConstants, luma[1].Get(), lumaSize);
        transition(commands, luma[1].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // Pass 1: coarse 1/16 3x3 block search on 1/4-res luma (+/-60 px full-frame motion)
        transition(commands, coarse.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        drawPass(motionPipeline.Get(), gpuTable2, pyramidConstants, coarse.Get(), coarseSize);
        transition(commands, coarse.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // Pass 2: 1/8 spatial propagation + sub-pixel refinement on full-res frames
        transition(commands, refined.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        drawPass(refineMotionPipeline.Get(), gpuTable0, coarseConstants, refined.Get(), refinedSize);
        transition(commands, refined.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // Pass 3: filtered warp or NGX/FFX motion vector output
        const bool targetFp16 = target->GetDesc().Format == highPrecisionFormat;
        transition(commands, target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        drawPass(forDlss ? dlssMotionPipeline.Get() : (targetFp16 ? interpolationPipelineFp16 : interpolationPipeline).Get(),
            gpuTable3, refinedConstants, target, size);
        transition(commands, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);

        transition(commands, refined.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        transition(commands, coarse.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        for(const auto &tex : luma)
            transition(commands, tex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        for(const auto &texture : history)
            transition(commands, texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    }

    void recordInterpolation(ID3D12Resource *dst, QSize size, bool forDlss)
    {
        recordMotion(videoHistory, videoLumaHistory, motionTexture, motionRefinedTexture, interpolationViews.Get(),
            forDlss ? dlssMotionTexture.Get() : dst, size, forDlss);
    }

    void clearTexture(ID3D12Resource *texture, float value)
    {
        const auto rtv = renderTargetView->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(texture, nullptr, rtv);
        auto *commands = graphicsCommands.Get();
        transition(commands, texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float color[] = {value, value, value, value};
        commands->ClearRenderTargetView(rtv, color, 0, nullptr);
        transition(commands, texture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    }

    QSize enhancementSize(QSize sourceSize, QSize videoArea) const
    {
        if(upscaleHeight == 1 && srModel > 0)
            return sourceSize;
        if(upscaleHeight > videoArea.height() && upscaleHeight <= videoArea.height() * 2)
            return QSize(qRound(double(videoArea.width()) * upscaleHeight / videoArea.height()), upscaleHeight);
        return videoArea;
    }

    bool ensureVideoSuperResolution(int &status)
    {
        if(vsr)
        {
            status = 1;
            return true;
        }
        status = 0;
        if(!createVsr && !createVsrEx)
            return false;
        const auto path = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        QDir().mkpath(path);
        const bool shutdownOnFail = (dlss == nullptr);
        beginFrame();
        if(createVsrEx)
            vsr = createVsrEx(device.Get(), graphicsCommands.Get(), reinterpret_cast<const wchar_t *>(path.utf16()),
                shutdownOnFail, &status);
        else
            vsr = createVsr(device.Get(), graphicsCommands.Get(), reinterpret_cast<const wchar_t *>(path.utf16()), &status);
        if(!vsr)
        {
            check(graphicsCommands->Close(), "DX12 discard RTX VSR initialization");
            return false;
        }
        submit(nullptr, false);
        waitForFence(fenceValue, 10000);
        status = 1;
        return true;
    }

    int evaluateVideoSuperResolution(ID3D12Resource *input, ID3D12Resource *output)
    {
        return vsr ? evaluateVsr(vsr, graphicsCommands.Get(), input, output, vsrQuality) : 0;
    }

    void prewarmVsr(QSize input, QSize size)
    {
        if(!vsrQuality || srModel || !createVsr || !evaluateVsr || size.isEmpty())
            return;
        const QSize area = input.scaled(size, Qt::KeepAspectRatio);
        const QSize targetSize = enhancementSize(input, area);
        if(targetSize.width() <= input.width() || targetSize.height() <= input.height())
            return;
        QElapsedTimer time;
        time.start();
        int initStatus = 0;
        if(!ensureVideoSuperResolution(initStatus))
        {
            std::fprintf(stderr, "DX12 RTX VSR warmup unavailable (0x%08x).\n", unsigned(initStatus));
            return;
        }
        waitForFence(fenceValue, 5000);
        rgb = makeTexture(input, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, rgbFormat);
        rgbSurfaceFormat = rgbFormat;
        convertedSize = input;
        auto scratch = makeTexture(targetSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, rgbFormat);
        beginFrame();
        clearTexture(rgb.Get(), 0);
        const int status = evaluateVideoSuperResolution(rgb.Get(), scratch.Get());
        if(status == 1)
        {
            submit(nullptr, false);
            waitForFence(fenceValue, 10000);
            std::printf("DX12 RTX VSR prepared in %lld ms before video.\n", static_cast<long long>(time.elapsed()));
        }
        else
        {
            check(graphicsCommands->Close(), "DX12 discard RTX VSR warmup");
            std::fprintf(stderr, "DX12 RTX VSR warmup unavailable (0x%08x).\n", unsigned(status));
        }
    }

    void initializeSr(QSize input, QSize output)
    {
        if(sr)
            return;
        const auto base = QCoreApplication::applicationDirPath();
        if(!srBridge.isLoaded())
            srBridge.setFileName(base + "/PS5LinkDlssSr.dll");
        auto create = reinterpret_cast<CreateSr>(srBridge.resolve("Ps5LinkDlssSrCreate"));
        evaluateSr = reinterpret_cast<EvaluateSr>(srBridge.resolve("Ps5LinkDlssSrEvaluate"));
        destroySr = reinterpret_cast<DestroySr>(srBridge.resolve("Ps5LinkDlssSrDestroy"));
        if(!create || !evaluateSr || !destroySr)
            throw std::runtime_error("PS5LinkDlssSr.dll is unavailable");
        initializeInterpolation();
        for(auto &texture : srInput)
            texture = makeTexture(input, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, rgbFormat);
        srMotion = makeTexture(input, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, DXGI_FORMAT_R16G16_FLOAT);
        srDepth = makeTexture(input, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, DXGI_FORMAT_R32_FLOAT);
        srOutput = makeTexture(output, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, rgbFormat);
        const auto folder = QDir::toNativeSeparators(base + "/dlsssr");
        const auto data = QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
        QDir().mkpath(data);
        beginFrame();
        int status = 0;
        sr = create(device.Get(), graphicsCommands.Get(), reinterpret_cast<const wchar_t *>(folder.utf16()),
            reinterpret_cast<const wchar_t *>(data.utf16()), input.width(), input.height(), output.width(), output.height(),
            srModel == 2, &status);
        if(!sr)
        {
            check(graphicsCommands->Close(), "DX12 discard DLSS SR initialization");
            throw std::runtime_error(QStringLiteral("NGX initialization failed (0x%1)").arg(uint32_t(status), 8, 16, QChar('0')).toStdString());
        }
        clearTexture(srDepth.Get(), 1.0f);
        submit(nullptr, false);
    }

    int recordSr(QSize input, bool reset)
    {
        if(reset)
            clearTexture(srMotion.Get(), 0.0f);
        else
            recordMotion(srInput, srLumaHistory, srCoarse, srRefined, srViews.Get(), srMotion.Get(), input, true);
        auto *commands = graphicsCommands.Get();
        const auto inputs = {srInput[1].Get(), srMotion.Get(), srDepth.Get()};
        for(auto *texture : inputs)
            transition(commands, texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(commands, srOutput.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const int status = evaluateSr(sr, commands, srInput[1].Get(), srMotion.Get(), srDepth.Get(), srOutput.Get(), reset);
        transition(commands, srOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        for(auto *texture : inputs)
            transition(commands, texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        return status;
    }

    void initializeDlss(QSize size)
    {
        if(dlss)
            return;
        const auto base = QCoreApplication::applicationDirPath();
        if(!dlssBridge.isLoaded())
            dlssBridge.setFileName(base + "/PS5LinkDlssFg.dll");
        auto create = reinterpret_cast<CreateDlss>(dlssBridge.resolve("Ps5LinkDlssFgCreate"));
        auto createEx = reinterpret_cast<CreateDlssEx>(dlssBridge.resolve("Ps5LinkDlssFgCreateEx"));
        evaluateDlss = reinterpret_cast<EvaluateDlss>(dlssBridge.resolve("Ps5LinkDlssFgEvaluate"));
        destroyDlss = reinterpret_cast<DestroyDlss>(dlssBridge.resolve("Ps5LinkDlssFgDestroy"));
        destroyDlssEx = reinterpret_cast<DestroyDlssEx>(dlssBridge.resolve("Ps5LinkDlssFgDestroyEx"));
        if((!create && !createEx) || !evaluateDlss || !destroyDlss)
            throw std::runtime_error("PS5LinkDlssFg.dll is unavailable");
        if(vsr)
        {
            waitForFence(fenceValue, 5000);
            releaseVsr(true);
        }
        else
            waitForFence(fenceValue, 5000);
        dlssMotionTexture = makeTexture(size, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, DXGI_FORMAT_R16G16_FLOAT);
        dlssDepth = makeTexture(size, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, DXGI_FORMAT_R32_FLOAT);
        dlssGenerated = makeTexture(size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, rgbFormat);
        dlssReal = makeTexture(size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, rgbFormat);
        beginFrame();
        const auto folder = QDir::toNativeSeparators(base + "/dlssg");
        int status = 0;
        if(createEx)
            dlss = createEx(device.Get(), graphicsCommands.Get(), reinterpret_cast<const wchar_t *>(folder.utf16()),
                useSm86, size.width(), size.height(), vsr == nullptr, &status);
        else
            dlss = create(device.Get(), graphicsCommands.Get(), reinterpret_cast<const wchar_t *>(folder.utf16()),
                useSm86, size.width(), size.height(), &status);
        if(!dlss)
        {
            check(graphicsCommands->Close(), "DX12 discard DLSS initialization");
            throw std::runtime_error(QStringLiteral("NGX initialization failed (0x%1)").arg(uint32_t(status), 8, 16, QChar('0')).toStdString());
        }
        submit(nullptr, false);
        dlssSize = size;
        std::printf("DX12 DLSS FG initialized via %s.\n", useSm86 ? "SM86" : "NVIDIA");
    }

    void initializeFsrFg(QSize size)
    {
        if(fsrFg)
            return;
        const auto base = QCoreApplication::applicationDirPath();
        if(!fsrFgBridge.isLoaded())
            fsrFgBridge.setFileName(base + "/PS5LinkFsrFg.dll");
        auto create = reinterpret_cast<CreateFsrFg>(fsrFgBridge.resolve("Ps5LinkFsrFgCreate"));
        evaluateFsrFg = reinterpret_cast<EvaluateFsrFg>(fsrFgBridge.resolve("Ps5LinkFsrFgEvaluate"));
        evaluateFsrFgEx = reinterpret_cast<EvaluateFsrFgEx>(fsrFgBridge.resolve("Ps5LinkFsrFgEvaluateEx"));
        destroyFsrFg = reinterpret_cast<DestroyFsrFg>(fsrFgBridge.resolve("Ps5LinkFsrFgDestroy"));
        if(!create || (!evaluateFsrFg && !evaluateFsrFgEx) || !destroyFsrFg)
            throw std::runtime_error("PS5LinkFsrFg.dll is unavailable");
        waitForFence(fenceValue, 5000);
        const auto matchesSize = [&](const ComPtr<ID3D12Resource> &tex) {
            if(!tex)
                return false;
            const auto d = tex->GetDesc();
            return int(d.Width) == size.width() && int(d.Height) == size.height();
        };
        if(!matchesSize(dlssMotionTexture))
            dlssMotionTexture = makeTexture(size, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, DXGI_FORMAT_R16G16_FLOAT);
        if(!matchesSize(dlssDepth))
            dlssDepth = makeTexture(size, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, DXGI_FORMAT_R32_FLOAT);
        if(!matchesSize(dlssGenerated))
            dlssGenerated = makeTexture(size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, rgbFormat);
        const auto folder = QDir::toNativeSeparators(base + "/fsrfg");
        char ver[64]{};
        int status = 0;
        fsrFg = create(device.Get(), reinterpret_cast<const wchar_t *>(folder.utf16()),
            size.width(), size.height(), ver, sizeof(ver), &status);
        if(!fsrFg)
            throw std::runtime_error(QStringLiteral("AMD FidelityFX FG initialization failed (%1)").arg(status).toStdString());
        fsrFgSize = size;
        fsrFgVersion = ver[0] ? std::string(ver) : std::string("3.1");
        std::printf("DX12 AMD FSR %s FG initialized.\n", fsrFgVersion.c_str());
    }

    void prepareEffects(QSize input, QSize output, qreal refreshRate)
    {
        // FG must initialize first on the shared NGX device. A failed FG
        // creation shuts down NGX and would invalidate an existing VSR handle.
        if(!srModel && generationMode == 2 && !dlssFailed && refreshRate >= 119 && !output.isEmpty())
        {
            try
            {
                initializeInterpolation();
                initializeDlss(output);
                waitForFence(fenceValue, 10000);
            }
            catch(const std::exception &failure)
            {
                dlssFailed = true;
                std::fprintf(stderr, "DX12 DLSS FG unavailable: %s\n", failure.what());
            }
        }
        else if(!srModel && generationMode == 3 && !fsrFgFailed && refreshRate >= 119 && !output.isEmpty())
        {
            try
            {
                initializeInterpolation();
                initializeFsrFg(output);
            }
            catch(const std::exception &failure)
            {
                fsrFgFailed = true;
                std::fprintf(stderr, "DX12 FSR FG unavailable: %s\n", failure.what());
            }
        }
        prewarmVsr(input, output);
    }

    int recordDlss(QSize size, bool generate)
    {
        auto *commands = graphicsCommands.Get();
        const auto clear = [&](ID3D12Resource *texture) {
            const auto rtv = renderTargetView->GetCPUDescriptorHandleForHeapStart();
            device->CreateRenderTargetView(texture, nullptr, rtv);
            const float zero[4]{};
            transition(commands, texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            commands->ClearRenderTargetView(rtv, zero, 0, nullptr);
            transition(commands, texture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        };
        if(generate)
            recordInterpolation(nullptr, size, true);
        else
            clear(dlssMotionTexture.Get());
        clear(dlssDepth.Get());
        for(auto *texture : {videoHistory[1].Get(), dlssMotionTexture.Get(), dlssDepth.Get()})
            transition(commands, texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for(auto *texture : {dlssGenerated.Get(), dlssReal.Get()})
            transition(commands, texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const int status = evaluateDlss(dlss, commands, graphicsQueue.Get(), graphicsAllocator.Get(), dlssFenceEvent,
            !generate, videoHistory[1].Get(), dlssMotionTexture.Get(), dlssDepth.Get(), dlssGenerated.Get(), dlssReal.Get());
        if(status != 1)
            return status;
        for(auto *texture : {videoHistory[1].Get(), dlssMotionTexture.Get(), dlssDepth.Get()})
            transition(commands, texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        for(auto *texture : {dlssGenerated.Get(), dlssReal.Get()})
            transition(commands, texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        return status;
    }

    int recordFsrFg(QSize size, bool generate)
    {
        auto *commands = graphicsCommands.Get();
        const auto clear = [&](ID3D12Resource *texture, float value) {
            const auto rtv = renderTargetView->GetCPUDescriptorHandleForHeapStart();
            device->CreateRenderTargetView(texture, nullptr, rtv);
            const float val[4]{value, 0.0f, 0.0f, 0.0f};
            transition(commands, texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            commands->ClearRenderTargetView(rtv, val, 0, nullptr);
            transition(commands, texture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        };
        if(generate)
            recordInterpolation(nullptr, size, true);
        else
            clear(dlssMotionTexture.Get(), 0.0f);
        // Inverted depth [1..0]: use distant flat plane (0.001) rather than 0.5 (0.2m near camera).
        clear(dlssDepth.Get(), 0.001f);
        for(auto *texture : {videoHistory[1].Get(), dlssMotionTexture.Get(), dlssDepth.Get()})
            transition(commands, texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(commands, dlssGenerated.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const float deltaMs = float(std::clamp(smoothedInputIntervalUs / 1000.0, 4.0, 50.0));
        const int status = evaluateFsrFgEx
            ? evaluateFsrFgEx(fsrFg, commands, !generate, deltaMs,
                videoHistory[1].Get(), dlssMotionTexture.Get(), dlssDepth.Get(), dlssGenerated.Get())
            : evaluateFsrFg(fsrFg, commands, !generate,
                videoHistory[1].Get(), dlssMotionTexture.Get(), dlssDepth.Get(), dlssGenerated.Get());
        if(status != 1)
            return status;
        for(auto *texture : {videoHistory[1].Get(), dlssMotionTexture.Get(), dlssDepth.Get()})
            transition(commands, texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        transition(commands, dlssGenerated.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        return status;
    }

    void waitForFence(uint64_t fence, DWORD timeoutMs = 3000)
    {
        if(!fence)
            return;
        const uint64_t completed = graphicsFence->GetCompletedValue();
        if(completed == UINT64_MAX)
            check(device->GetDeviceRemovedReason(), "DX12 device removed during fence wait");
        if(completed >= fence)
            return;
        check(graphicsFence->SetEventOnCompletion(fence, completedEvent), "DX12 arm graphics fence");
        if(WaitForSingleObject(completedEvent, timeoutMs) != WAIT_OBJECT_0)
            throw std::runtime_error("DX12 graphics fence timed out");
    }

    bool checkOccluded()
    {
        if(!swapchainOccluded || !swapchain)
            return false;
        const HRESULT status = swapchain->Present(0, DXGI_PRESENT_TEST);
        if(status == DXGI_STATUS_OCCLUDED)
            return true;
        if(SUCCEEDED(status))
            swapchainOccluded = false;
        return false;
    }

    bool waitForPresentSlot(DWORD timeoutMs = 150)
    {
        if(hasPresentPermit)
            return true;
        const DWORD wait = WaitForSingleObject(frameLatency, timeoutMs);
        if(wait == WAIT_TIMEOUT)
        {
            ++presentTimeouts;
            return false;
        }
        if(wait != WAIT_OBJECT_0)
            throw std::runtime_error("DX12 swapchain latency wait failed");
        hasPresentPermit = true;
        return true;
    }

    float effectiveSharpness(bool usingSr, bool usingFsr) const
    {
        const int setting = sharpnessSetting.load();
        if(setting < 0)
            return usingFsr ? 0.25f : usingSr ? 0.20f : 0.0f;
        return std::clamp(setting, 0, 100) / 100.0f;
    }

    void drawOverlay(ID3D12Resource *target, QSize size, qreal refreshRate, bool usingVsr, bool usingFsr)
    {
        const bool wantQuickBar = !hideOnScreenControls.load() && !showMenu.load() && bottomDockHovered.load();
        const bool showQuickBar = !hideOnScreenControls.load() && !showMenu.load() && (wantQuickBar || dockAnim > 0.005f);
        if(imguiFailed || (!showHud.load() && !showMenu.load() && !showQuickBar))
        {
            dockAnim = 0.0f;
            return;
        }
        const auto ui = [](const char *en, const char *ru) { return ps5LinkUsesRussian() ? ru : en; };
        auto applyDs5Style = [](bool hdrScale)
        {
            ImGui::StyleColorsDark();
            auto &style = ImGui::GetStyle();
            style.WindowRounding = 24.0f;
            style.ChildRounding = 16.0f;
            style.FrameRounding = 12.0f;
            style.PopupRounding = 14.0f;
            style.ScrollbarRounding = 9.0f;
            style.GrabRounding = 12.0f;
            style.GrabMinSize = 16.0f;
            style.TabRounding = 10.0f;
            style.WindowBorderSize = 1.0f;
            style.ChildBorderSize = 1.0f;
            style.PopupBorderSize = 1.0f;
            style.FrameBorderSize = 1.0f;
            style.WindowPadding = ImVec2(14.0f, 11.0f);
            style.FramePadding = ImVec2(10.0f, 5.0f);
            style.ItemSpacing = ImVec2(8.0f, 5.0f);
            style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);

            auto *c = style.Colors;
            c[ImGuiCol_Text]                 = ImVec4(0.941f, 0.945f, 0.953f, 1.00f); // #F0F1F3
            c[ImGuiCol_TextDisabled]         = ImVec4(0.612f, 0.612f, 0.612f, 1.00f); // #9C9C9C
            c[ImGuiCol_WindowBg]             = ImVec4(0.086f, 0.088f, 0.102f, 0.94f); // #16161A
            c[ImGuiCol_ChildBg]              = ImVec4(0.125f, 0.127f, 0.149f, 0.92f); // #202026
            c[ImGuiCol_PopupBg]              = ImVec4(0.122f, 0.124f, 0.145f, 0.98f); // #1F1F25
            c[ImGuiCol_Border]               = ImVec4(0.245f, 0.250f, 0.295f, 0.85f); // #3E404B
            c[ImGuiCol_BorderShadow]         = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
            c[ImGuiCol_FrameBg]              = ImVec4(0.173f, 0.176f, 0.212f, 1.00f); // #2C2D36
            c[ImGuiCol_FrameBgHovered]       = ImVec4(0.235f, 0.239f, 0.282f, 1.00f); // #3C3D48
            c[ImGuiCol_FrameBgActive]        = ImVec4(0.275f, 0.278f, 0.329f, 1.00f);
            c[ImGuiCol_TitleBg]              = ImVec4(0.118f, 0.118f, 0.137f, 1.00f);
            c[ImGuiCol_TitleBgActive]        = ImVec4(0.137f, 0.137f, 0.157f, 1.00f);
            c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.118f, 0.118f, 0.137f, 0.75f);
            c[ImGuiCol_MenuBarBg]            = ImVec4(0.137f, 0.137f, 0.157f, 1.00f);
            c[ImGuiCol_ScrollbarBg]          = ImVec4(0.078f, 0.078f, 0.090f, 0.60f);
            c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.275f, 0.275f, 0.318f, 1.00f);
            c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.353f, 0.353f, 0.408f, 1.00f);
            c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.039f, 0.518f, 1.000f, 1.00f);
            c[ImGuiCol_CheckMark]            = ImVec4(0.188f, 0.820f, 0.345f, 1.00f); // #30D158
            c[ImGuiCol_SliderGrab]           = ImVec4(0.039f, 0.518f, 1.000f, 1.00f); // #0A84FF
            c[ImGuiCol_SliderGrabActive]     = ImVec4(0.270f, 0.620f, 1.000f, 1.00f);
            c[ImGuiCol_Button]               = ImVec4(0.173f, 0.176f, 0.212f, 1.00f);
            c[ImGuiCol_ButtonHovered]        = ImVec4(0.239f, 0.243f, 0.290f, 1.00f);
            c[ImGuiCol_ButtonActive]         = ImVec4(0.039f, 0.518f, 1.000f, 1.00f);
            c[ImGuiCol_Header]               = ImVec4(0.173f, 0.176f, 0.212f, 1.00f);
            c[ImGuiCol_HeaderHovered]        = ImVec4(0.039f, 0.518f, 1.000f, 0.55f);
            c[ImGuiCol_HeaderActive]         = ImVec4(0.039f, 0.518f, 1.000f, 0.85f);
            c[ImGuiCol_Separator]            = ImVec4(0.220f, 0.224f, 0.263f, 0.90f);
            c[ImGuiCol_SeparatorHovered]     = ImVec4(0.039f, 0.518f, 1.000f, 0.78f);
            c[ImGuiCol_SeparatorActive]      = ImVec4(0.039f, 0.518f, 1.000f, 1.00f);
            c[ImGuiCol_PlotLines]            = ImVec4(0.039f, 0.518f, 1.000f, 1.00f);
            c[ImGuiCol_PlotLinesHovered]     = ImVec4(0.188f, 0.820f, 0.345f, 1.00f);

            if(hdrScale)
            {
                // Scale SDR ImGui palette into ~200 nit PQ code space so HUD/menu aren't blinding on HDR10 displays.
                for(int i = 0; i < ImGuiCol_COUNT; ++i)
                {
                    c[i].x *= 0.58f;
                    c[i].y *= 0.58f;
                    c[i].z *= 0.58f;
                }
            }
        };
        if(!imgui)
        {
            const D3D12_DESCRIPTOR_HEAP_DESC descriptors{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
            if(FAILED(device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&imguiViews))))
            {
                imguiFailed = true;
                std::fprintf(stderr, "DX12 in-frame overlay descriptors unavailable.\n");
                return;
            }
            imgui = ImGui::CreateContext();
            auto &ioInit = ImGui::GetIO();
            ioInit.IniFilename = nullptr;
            ioInit.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            QFile fontFile(QStringLiteral(":/ps5link/Geologica-Regular.ttf"));
            if(fontFile.open(QIODevice::ReadOnly))
            {
                auto data = fontFile.readAll();
                static const ImWchar glyphs[] = {0x0020, 0x00ff, 0x0400, 0x052f,
                    0x2013, 0x2014, 0x2022, 0x2022, 0x2026, 0x2026, 0x2212, 0x2212, 0x25cf, 0x25cf, 0};
                ImFontConfig config;
                config.FontDataOwnedByAtlas = false; // The atlas copies borrowed data.
                ioInit.Fonts->AddFontFromMemoryTTF(data.data(), int(data.size()), 15.0f, &config, glyphs);
            }
            if(!ImGui_ImplDX12_Init(device.Get(), int(frameSlots.size()), swapchainFormat, imguiViews.Get(),
                imguiViews->GetCPUDescriptorHandleForHeapStart(), imguiViews->GetGPUDescriptorHandleForHeapStart()))
            {
                ImGui::DestroyContext(imgui);
                imgui = nullptr;
                imguiFailed = true;
                std::fprintf(stderr, "DX12 in-frame overlay unavailable.\n");
                return;
            }
            applyDs5Style(false);
            imguiHdrStyled = false;
        }
        ImGui::SetCurrentContext(imgui);
        if(imguiHdrStyled != hdrPassthroughActive)
        {
            applyDs5Style(hdrPassthroughActive);
            imguiHdrStyled = hdrPassthroughActive;
        }
        auto &io = ImGui::GetIO();
        io.DisplaySize = ImVec2(float(size.width()), float(size.height()));
        if(!imguiDeltaTimer.isValid())
        {
            imguiDeltaTimer.start();
            io.DeltaTime = 1.0f / 60.0f;
        }
        else
        {
            const float dt = float(imguiDeltaTimer.restart()) / 1000.0f;
            io.DeltaTime = std::clamp(dt, 1.0f / 240.0f, 0.1f);
        }
        {
            const std::lock_guard<std::mutex> lock(inputMutex);
            io.AddMousePosEvent(float(mousePosition.x()), float(mousePosition.y()));
            if(mouseWheel != 0.0f)
            {
                io.AddMouseWheelEvent(0.0f, mouseWheel);
                mouseWheel = 0.0f;
            }
            for(bool pressed : mouseClicks)
                io.AddMouseButtonEvent(0, pressed);
            mouseClicks.clear();
            for(const auto &ev : keyEvents)
                io.AddKeyEvent(ev.key, ev.down);
            keyEvents.clear();
        }
        ImGui_ImplDX12_NewFrame();
        ImGui::NewFrame();
        if(!overlayRate.isValid())
            overlayRate.start();
        else if(overlayRate.elapsed() >= 500)
        {
            const unsigned shown = presentedFrames + generatedFrames;
            displayedFps = float((shown - previousOverlayFrames) * 1000.0 / overlayRate.restart());
            previousOverlayFrames = shown;
        }
        const bool usingSr = srModel && !srFailed && sr != nullptr;
        const float activeSharpness = effectiveSharpness(usingSr, usingFsr);
        const float hdrMul = hdrPassthroughActive ? 0.58f : 1.0f;
        const auto tint = [hdrMul](float r, float g, float b, float a = 1.0f) {
            return ImVec4(r * hdrMul, g * hdrMul, b * hdrMul, a);
        };
        const float lossPctLive = hudPacketLossPct.load();
        const float bitrateLive = hudBitrateMbps.load();
        const float rttLive = hudRttMs.load();
        const int padBat = hudGamepadBattery.load();
        const bool padChg = hudGamepadCharging.load();
        const bool padFull = hudGamepadFull.load();
        const bool padConnected = hudGamepadConnected.load();
        const ImVec4 liveDotColor = (smoothedRenderMs > 15.5f || lossPctLive >= 1.0f) ? tint(1.0f, 0.27f, 0.23f)
            : smoothedRenderMs > 12.0f ? tint(1.0f, 0.62f, 0.04f)
            : tint(0.19f, 0.82f, 0.35f);

        auto drawStatusDot = [](const ImVec4 &col, float radius = 4.0f) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float h = ImGui::GetFrameHeight();
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(p.x + radius + 1.0f, p.y + h * 0.5f), radius,
                ImGui::ColorConvertFloat4ToU32(col), 16);
            ImGui::Dummy(ImVec2(radius * 2.0f + 2.0f, h));
        };

        auto drawBatteryBadge = [&](float alphaScale) {
            if(!padConnected)
                return;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const float frameH = ImGui::GetFrameHeight();
            const float bw = 22.0f;
            const float bh = 11.0f;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float by = p.y + (frameH - bh) * 0.5f;
            const ImVec4 outlineCol = tint(0.70f, 0.72f, 0.78f, 0.90f * alphaScale);
            const ImVec4 fillCol = (padChg || padFull) ? tint(0.19f, 0.82f, 0.35f, alphaScale)
                : (padBat >= 0 && padBat <= 15) ? tint(1.0f, 0.27f, 0.23f, alphaScale)
                : (padBat >= 0 && padBat <= 30) ? tint(1.0f, 0.62f, 0.04f, alphaScale)
                : tint(0.19f, 0.82f, 0.35f, alphaScale);
            dl->AddRect(ImVec2(p.x, by), ImVec2(p.x + bw, by + bh), ImGui::ColorConvertFloat4ToU32(outlineCol), 3.0f, 0, 1.3f);
            dl->AddRectFilled(ImVec2(p.x + bw + 1.0f, by + 3.0f), ImVec2(p.x + bw + 3.0f, by + bh - 3.0f),
                ImGui::ColorConvertFloat4ToU32(outlineCol), 1.0f);
            const float frac = padBat >= 0 ? std::clamp(padBat / 100.0f, 0.08f, 1.0f) : 0.65f;
            const float innerW = std::max(2.0f, (bw - 4.0f) * frac);
            dl->AddRectFilled(ImVec2(p.x + 2.0f, by + 2.0f), ImVec2(p.x + 2.0f + innerW, by + bh - 2.0f),
                ImGui::ColorConvertFloat4ToU32(fillCol), 1.5f);
            ImGui::Dummy(ImVec2(bw + 4.0f, frameH));
            ImGui::SameLine(0.0f, 5.0f);
            ImGui::AlignTextToFramePadding();
            if(padBat >= 0)
                ImGui::TextColored(fillCol, padChg && !padFull ? "%d%%+" : "%d%%", padBat);
            else
                ImGui::TextColored(fillCol, padChg ? "USB" : "PAD");
        };

        auto drawMinimalHudStrip = [&](float alphaScale) {
            drawStatusDot(ImVec4(liveDotColor.x, liveDotColor.y, liveDotColor.z, liveDotColor.w * alphaScale), 4.0f);
            ImGui::SameLine(0.0f, 6.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%.0f", displayedFps);
            ImGui::SameLine(0.0f, 3.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("FPS");
            ImGui::SameLine(0.0f, 7.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("·");
            ImGui::SameLine(0.0f, 7.0f);
            ImGui::AlignTextToFramePadding();
            if(bitrateLive > 0.05f)
            {
                ImGui::Text("%.1f", bitrateLive);
                ImGui::SameLine(0.0f, 3.0f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled(ui("Mbps", "Мбит/с"));
            }
            else
                ImGui::TextDisabled(ui("Local", "Лок."));
            if(rttLive > 0.1f)
            {
                ImGui::SameLine(0.0f, 7.0f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("·");
                ImGui::SameLine(0.0f, 7.0f);
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%.0f", rttLive);
                ImGui::SameLine(0.0f, 2.0f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled(ui("ms", "мс"));
            }
            if(padConnected)
            {
                ImGui::SameLine(0.0f, 8.0f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("·");
                ImGui::SameLine(0.0f, 7.0f);
                drawBatteryBadge(alphaScale);
            }
        };

        if(showQuickBar)
        {
            const float targetAnim = wantQuickBar ? 1.0f : 0.0f;
            const float stepAnim = std::min(1.0f, io.DeltaTime * 11.0f);
            dockAnim = dockAnim + (targetAnim - dockAnim) * stepAnim;
            if(std::abs(targetAnim - dockAnim) < 0.005f)
                dockAnim = targetAnim;
        }
        else
            dockAnim = 0.0f;

        if(showHud.load())
        {
            ImGui::SetNextWindowPos(ImVec2(18, 18), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.92f);
            ImGui::Begin("Stream statistics", nullptr, ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
            const float lossPctHeader = hudPacketLossPct.load();
            const ImVec4 dotColor = (smoothedRenderMs > 15.5f || lossPctHeader >= 1.0f) ? ImVec4(1.0f, 0.27f, 0.23f, 1.0f)
                : smoothedRenderMs > 12.0f ? ImVec4(1.0f, 0.62f, 0.04f, 1.0f)
                : ImVec4(0.19f, 0.82f, 0.35f, 1.0f);
            drawStatusDot(dotColor, 4.0f);
            ImGui::SameLine(0.0f, 6.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ImVec4(0.94f, 0.95f, 0.96f, 1.0f), "PS5Link");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.62f, 0.62f, 0.65f, 1.0f), ui("•  Direct3D 12 HUD", "•  Статистика Direct3D 12"));
            ImGui::Separator();
            const char *hdrTag = hdrPassthroughActive ? ui("   HDR10 native", "   HDR10 напрямую")
                : hdrToneMapped ? "   HDR->SDR (BT.2390)"
                : display10Bit ? ui("   10-bit", "   10 бит") : "";
            ImGui::Text(ui("Output %.1f FPS   %d x %d   %.0f Hz%s", "Вывод %.1f кадр/с   %d × %d   %.0f Гц%s"), displayedFps, size.width(), size.height(), refreshRate, hdrTag);
            if(smoothedRenderMs > 0.01f)
            {
                const ImVec4 color = smoothedRenderMs > 15.5f ? ImVec4(1.0f, 0.27f, 0.23f, 1.0f)
                    : smoothedRenderMs > 12.0f ? ImVec4(1.0f, 0.62f, 0.04f, 1.0f)
                    : ImVec4(0.19f, 0.82f, 0.35f, 1.0f);
                ImGui::TextColored(color, ui("Render %.2f ms%s", "Рендер %.2f мс%s"), smoothedRenderMs,
                    smoothedRenderMs > 15.5f ? ui(" (GPU load high)", " (высокая нагрузка GPU)") : "");
                if(frameTimeHistoryCount >= 8)
                {
                    const int count = int(frameTimeHistoryCount);
                    const int offset = frameTimeHistoryCount == frameTimeHistory.size() ? int(frameTimeHistoryPos) : 0;
                    ImGui::PlotLines("##frametime", frameTimeHistory.data(), count, offset,
                        nullptr, 0.0f, 20.0f, ImVec2(236.0f, 30.0f));
                }
            }
            if(usingSr)
                ImGui::Text(ui("DLSS %s SR (preset %c) | %dx%d | CAS %d%%", "DLSS %s SR (пресет %c) | %dx%d | CAS %d%%"),
                    srModel == 2 ? "4.5" : "4", srModel == 2 ? 'M' : 'K',
                    upscaleSize.width(), upscaleSize.height(), int(std::lround(activeSharpness * 100.0f)));
            else if(usingVsr)
                ImGui::Text(ui("RTX VSR active (quality %d) | %dx%d", "RTX VSR активно (качество %d) | %dx%d"), vsrQuality, upscaleSize.width(), upscaleSize.height());
            else if(usingFsr)
                ImGui::Text("AMD FSR 1.0 (EASU) | CAS %d%%", int(std::lround(activeSharpness * 100.0f)));
            else if(vsrError)
                ImGui::Text(ui("RTX VSR failed (0x%08x). Shader scaling active.", "Ошибка RTX VSR (0x%08x). Работает шейдер DX12."), vsrError);
            if(generationMode == 2 && dlssFrames > dlssFramesAtSelection)
                ImGui::Text(ui("DLSS FG 2x active (%s)", "DLSS FG ×2 активно (%s)"), useSm86 ? "SM86" : "NVIDIA");
            else if(generationMode == 3 && fsrFgFrames > fsrFgFramesAtSelection)
                ImGui::Text(ui("AMD FSR %s FG 2x active", "AMD FSR %s FG ×2 активно"), fsrFgVersion.c_str());
            if(concealedFrames > 0)
                ImGui::Text(ui("Error concealment: %u corrupt frame(s) held", "Скрыто повреждённых кадров: %u"), concealedFrames);
            if(const auto text = std::atomic_load(&hudText))
                ImGui::TextUnformatted(text->c_str());
            {
                const float lossPct = hudPacketLossPct.load();
                const uint64_t corruptNow = hudCorruptFrames.load();
                if(corruptNow > prevCorruptFrames)
                {
                    prevCorruptFrames = corruptNow;
                    lastCorruptTime.start();
                }
                const bool recentCorrupt = lastCorruptTime.isValid() && lastCorruptTime.elapsed() < 3000;
                if(smoothedRenderMs > 15.5f)
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), ui("Status: GPU overload (lower upscale/FG)", "Перегрузка GPU (снизьте апскейл или FG)"));
                else if(lossPct >= 1.0f)
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), ui("Status: Network packet loss (%.1f%%)", "Потери пакетов (%.1f%%)"), lossPct);
                else if(recentCorrupt)
                    ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), ui("Status: Decoder frame corruption", "Декодер получил повреждённый кадр"));
                else if(smoothedRenderMs > 12.0f)
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.30f, 1.0f), ui("Status: Elevated GPU load (%.1f ms)", "Повышенная нагрузка GPU (%.1f мс)"), smoothedRenderMs);
                else
                    ImGui::TextColored(ImVec4(0.47f, 0.87f, 0.65f, 1.0f), ui("Status: Smooth (OK)", "Плавное воспроизведение"));
            }
            if(showLightbarOverlay.load())
            {
                const uint32_t rgbVal = lightbarRgb.load();
                const ImVec4 barColor(((rgbVal >> 16) & 0xff) / 255.0f, ((rgbVal >> 8) & 0xff) / 255.0f,
                    (rgbVal & 0xff) / 255.0f, 1.0f);
                ImGui::ColorButton("Lightbar", barColor, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, ImVec2(140, 6));
            }
            ImGui::End();
        }
        if(showQuickBar && dockAnim > 0.005f)
        {
            const float inv = 1.0f - dockAnim;
            const float eased = 1.0f - inv * inv * inv;
            const float slideOffset = (1.0f - eased) * 28.0f;
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, eased);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 22.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 8.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 5.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 0.0f));
            ImGui::SetNextWindowPos(ImVec2(size.width() * 0.5f, size.height() - 14.0f + slideOffset),
                ImGuiCond_Always, ImVec2(0.5f, 1.0f));
            ImGui::SetNextWindowBgAlpha(0.90f * eased);
            ImGui::Begin("BottomDock", nullptr, ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);

            // Minimal Live HUD strip (FPS · Bitrate · RTT · Gamepad Battery)
            drawMinimalHudStrip(eased);

            // Subtle vertical hairline divider
            ImGui::SameLine(0.0f, 10.0f);
            {
                const ImVec2 dp = ImGui::GetCursorScreenPos();
                const float dh = ImGui::GetFrameHeight();
                ImGui::GetWindowDrawList()->AddLine(
                    ImVec2(dp.x, dp.y + 3.0f), ImVec2(dp.x, dp.y + dh - 3.0f),
                    ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
                ImGui::Dummy(ImVec2(2.0f, dh));
            }

            // Quick Controls
            if(showMicBtn.load())
            {
                ImGui::SameLine(0.0f, 8.0f);
                const bool mic = overlayMic.load();
                if(mic)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, tint(0.12f, 0.42f, 0.22f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tint(0.16f, 0.52f, 0.28f));
                }
                if(ImGui::Button(mic ? ui("Mic ON", "Микр. вкл.") : ui("Mic OFF", "Микр. выкл.")))
                    overlayAction(Microphone, mic ? 0 : 1);
                if(mic)
                    ImGui::PopStyleColor(2);
            }
            if(showVolBtns.load())
            {
                ImGui::SameLine(0.0f, 6.0f);
                const int maxVol = systemVolumeMode.load() ? 100 : 128;
                const int step = systemVolumeMode.load() ? 5 : 8;
                const int curVol = overlayVolume.load();
                const int volPct = maxVol > 0 ? int(std::lround(curVol * 100.0 / maxVol)) : 0;
                if(ImGui::Button("−##dockvolminus", ImVec2(26.0f, 0.0f)))
                {
                    if(!volMinusMutedThisHold)
                    {
                        const int nv = std::max(0, curVol - step);
                        if(curVol > 0) savedVolumeBeforeMute = curVol;
                        overlayAction(Volume, nv);
                    }
                    volMinusHoldTimer.invalidate();
                    volMinusMutedThisHold = false;
                }
                if(ImGui::IsItemActive())
                {
                    if(!volMinusHoldTimer.isValid())
                        volMinusHoldTimer.start();
                    else if(!volMinusMutedThisHold && volMinusHoldTimer.elapsed() >= 400)
                    {
                        if(curVol > 0)
                        {
                            savedVolumeBeforeMute = curVol;
                            overlayAction(Volume, 0);
                        }
                        else
                            overlayAction(Volume, std::clamp(savedVolumeBeforeMute, step, maxVol));
                        volMinusMutedThisHold = true;
                    }
                }
                else if(volMinusHoldTimer.isValid())
                {
                    volMinusHoldTimer.invalidate();
                    volMinusMutedThisHold = false;
                }
                ImGui::SameLine(0.0f, 4.0f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%d%%", volPct);
                ImGui::SameLine(0.0f, 4.0f);
                if(ImGui::Button("+##dockvolplus", ImVec2(26.0f, 0.0f)))
                {
                    const int nv = std::min(maxVol, curVol + step);
                    overlayAction(Volume, nv);
                }
            }
            if(showAspectBtn.load())
            {
                ImGui::SameLine(0.0f, 6.0f);
                const char *const kAspectNames[] = {ui("Fit", "Вписать"), ui("Fill", "Заполнить"), ui("Stretch", "Растянуть"), ui("Ambient", "Фон")};
                const int cur = std::clamp(int(aspect), 0, 3);
                if(ImGui::Button(kAspectNames[cur]))
                {
                    const int next = (cur + 1) % 4;
                    aspect = Aspect(next);
                    historyValid = false;
                    srHistoryValid = false;
                    overlayAction(AspectMode, next);
                }
            }
            ImGui::SameLine(0.0f, 6.0f);
            {
                const bool hudOn = showHud.load();
                if(hudOn)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, tint(0.04f, 0.36f, 0.72f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tint(0.08f, 0.45f, 0.85f));
                }
                if(ImGui::Button("HUD"))
                    overlayAction(Hud, hudOn ? 0 : 1);
                if(hudOn)
                    ImGui::PopStyleColor(2);
            }
            ImGui::SameLine(0.0f, 6.0f);
            {
                const int wm = windowMode.load();
                if(ImGui::Button(wm == 2 ? ui("Fullscreen", "Весь экран") : ui("Window", "Окно")))
                {
                    const int next = (wm == 2) ? 0 : 2;
                    windowMode.store(next);
                    overlayAction(WindowMode, next);
                }
            }
            ImGui::SameLine(0.0f, 6.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_SliderGrabActive));
            if(ImGui::Button(ui("Settings", "Настройки")))
                overlayAction(CloseMenu, 0);
            ImGui::PopStyleColor(2);

            ImGui::End();
            ImGui::PopStyleVar(6);
        }
        if(showMenu.load())
        {
            const float panelWidth = std::clamp(io.DisplaySize.x - 36.0f, 340.0f, 900.0f);
            const float panelMaxHeight = std::min(io.DisplaySize.y - 36.0f, 326.0f);
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 14.0f),
                ImGuiCond_Always, ImVec2(0.5f, 1.0f));
            ImGui::SetNextWindowSizeConstraints(ImVec2(panelWidth, 0.0f), ImVec2(panelWidth, panelMaxHeight));
            ImGui::Begin("PS5Link", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);

            const float rowWidth = ImGui::GetContentRegionAvail().x;
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const bool twoCols = rowWidth >= 600.0f;

            // Compact top header: brand + minimal live HUD (FPS · Bitrate · RTT · Battery) + Resume button
            constexpr float resumeWidth = 122.0f;
            const float headerLeftX = ImGui::GetCursorPosX();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("PS5Link");
            ImGui::SameLine(0.0f, 12.0f);
            drawMinimalHudStrip(1.0f);
            if(rowWidth >= 360.0f)
                ImGui::SameLine(headerLeftX + rowWidth - resumeWidth);
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_SliderGrabActive));
            if(ImGui::Button(ui("Resume", "Продолжить"), ImVec2(resumeWidth, 28.0f)))
                overlayAction(CloseMenu, 0);
            ImGui::PopStyleColor(2);

            // Wide single-row tab selector bar
            const char *const pageNames[] = {ui("Display", "Экран"), ui("Video", "Видео"), ui("Color", "Цвет"), ui("Controls", "Управление"), ui("Session", "Сеанс")};
            const bool narrow = rowWidth < 400.0f;
            const int tabCols = narrow ? 3 : 5;
            const float tabHeight = 26.0f;
            const float tabWidth = std::max(1.0f, std::floor((rowWidth - 8.0f - (tabCols - 1) * 4.0f) / tabCols));
            const float tabsHeight = narrow ? tabHeight * 2.0f + 12.0f : tabHeight + 8.0f;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 11.0f);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_ScrollbarBg));
            ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetStyleColorVec4(ImGuiCol_ChildBg));
            if(ImGui::BeginChild("Sections", ImVec2(0, tabsHeight), ImGuiChildFlags_AlwaysUseWindowPadding,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
            {
                for(int i = 0; i < int(std::size(pageNames)); ++i)
                {
                    if(i % tabCols)
                        ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Button, i == overlayPage ?
                        ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive) : ImVec4(0, 0, 0, 0));
                    if(ImGui::Button(pageNames[i], ImVec2(tabWidth, tabHeight)))
                        overlayPage = i;
                    ImGui::PopStyleColor();
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(3);

            const float footerHeight = ImGui::GetFrameHeight() + 6.0f;
            const float bodyLimit = std::max(90.0f, panelMaxHeight - ImGui::GetCursorPosY() -
                ImGui::GetStyle().WindowPadding.y - footerHeight - gap);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 10));
            ImGui::PushID(overlayPage);
            ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, bodyLimit));
            if(ImGui::BeginChild("Settings", ImVec2(0, 0), ImGuiChildFlags_Borders |
                ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize))
            {
                const auto drawLabel = [](const char *label)
                {
                    const float width = ImGui::GetContentRegionAvail().x;
                    if(width >= 260.0f)
                    {
                        ImGui::AlignTextToFramePadding();
                        const float left = ImGui::GetCursorPosX();
                        ImGui::TextUnformatted(label);
                        ImGui::SameLine(0.0f, 0.0f);
                        ImGui::SetCursorPosX(left + width * 0.44f);
                    }
                    else
                        ImGui::TextWrapped("%s", label);
                    ImGui::SetNextItemWidth(-1.0f);
                };
                const auto toggle = [](const char *label, bool &enabled)
                {
                    ImGui::PushID(label);
                    const ImVec2 pos = ImGui::GetCursorScreenPos();
                    const float width = ImGui::GetContentRegionAvail().x;
                    const float height = ImGui::GetFrameHeight();
                    const bool pressed = ImGui::InvisibleButton("Toggle", ImVec2(width, height), ImGuiButtonFlags_EnableNav);
                    if(pressed)
                        enabled = !enabled;
                    ImDrawList *draw = ImGui::GetWindowDrawList();
                    constexpr float trackHeight = 20.0f;
                    constexpr float trackWidth = 36.0f;
                    const float x = pos.x + width - trackWidth;
                    const float y = pos.y + (height - trackHeight) * 0.5f;
                    draw->AddRectFilled(ImVec2(x, y), ImVec2(x + trackWidth, y + trackHeight),
                        ImGui::GetColorU32(enabled ? ImGuiCol_CheckMark :
                            ImGui::IsItemHovered() ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), trackHeight * 0.5f);
                    draw->AddCircleFilled(ImVec2(x + (enabled ? trackWidth - 10.0f : 10.0f), y + 10.0f),
                        7.5f, ImGui::GetColorU32(ImGuiCol_Text));
                    const ImVec4 clip(pos.x, pos.y, x - 6.0f, pos.y + height);
                    draw->AddText(nullptr, 0, ImVec2(pos.x, pos.y + (height - ImGui::GetFontSize()) * 0.5f),
                        ImGui::GetColorU32(ImGuiCol_Text), label, nullptr, 0, &clip);
                    ImGui::PopID();
                    return pressed;
                };

                const auto combo = [](const char *id, int *index, const char *options)
                {
                    const char *preview = "";
                    const char *item = options;
                    for(int i = 0; *item; ++i, item += std::char_traits<char>::length(item) + 1)
                    {
                        if(i == *index)
                        {
                            preview = item;
                            break;
                        }
                    }

                    ImDrawList *draw = ImGui::GetWindowDrawList();
                    const ImVec2 pos = ImGui::GetCursorScreenPos();
                    const float x = pos.x + ImGui::CalcItemWidth() - 16.0f;
                    const float y = pos.y + ImGui::GetFrameHeight() * 0.5f;
                    const ImU32 color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
                    const bool open = ImGui::BeginCombo(id, preview, ImGuiComboFlags_NoArrowButton);
                    if(open || ImGui::IsItemVisible())
                    {
                        draw->AddLine(ImVec2(x - 4, y - 2), ImVec2(x, y + 2), color, 1.5f);
                        draw->AddLine(ImVec2(x, y + 2), ImVec2(x + 4, y - 2), color, 1.5f);
                    }
                    if(!open)
                        return false;

                    bool changed = false;
                    item = options;
                    for(int i = 0; *item; ++i, item += std::char_traits<char>::length(item) + 1)
                    {
                        const bool selected = i == *index;
                        if(ImGui::Selectable(item, selected) && !selected)
                        {
                            *index = i;
                            changed = true;
                        }
                        if(selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                    return changed;
                };

                const auto beginColumns = [&](const char *tableId) -> bool {
                    if(!twoCols)
                        return false;
                    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(10.0f, 2.0f));
                    if(ImGui::BeginTable(tableId, 2, ImGuiTableFlags_SizingStretchSame))
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        return true;
                    }
                    ImGui::PopStyleVar();
                    return false;
                };
                const auto nextColumn = [&](bool inTable) {
                    if(inTable)
                        ImGui::TableSetColumnIndex(1);
                };
                const auto endColumns = [&](bool inTable) {
                    if(inTable)
                    {
                        ImGui::EndTable();
                        ImGui::PopStyleVar();
                    }
                };

                ImGui::PushTextWrapPos(0.0f);
                switch(overlayPage)
                {
                case 0: {
                    const bool tbl = beginColumns("DisplayCols");
                    int pacing = presentMode.load();
                    drawLabel(ui("Output timing", "Синхронизация"));
                    if(combo("##Output timing", &pacing, ui("Low latency\0Balanced 60 FPS\0VSync\0", "Низкая задержка\0Баланс 60 FPS\0VSync\0")))
                    {
                        presentMode.store(pacing);
                        overlayAction(PresentMode, pacing);
                    }
                    if(pacing == 0 && !tearingSupported)
                        ImGui::TextDisabled(ui("Tearing unavailable. VSync active.", "Разрывы недоступны. Включён VSync."));
                    int fit = int(aspect);
                    drawLabel(ui("Aspect", "Формат кадра"));
                    if(combo("##Aspect", &fit, ui("Fit\0Fill\0Stretch\0Ambient\0", "Вписать\0Заполнить\0Растянуть\0Фон по краям\0")))
                    {
                        aspect = Aspect(fit);
                        historyValid = false;
                        srHistoryValid = false;
                        overlayAction(AspectMode, fit);
                    }
                    if(aspect == Aspect::Ambient)
                    {
                        int amb = ambientStyle.load();
                        drawLabel(ui("Ambient", "Фон по краям"));
                        if(combo("##Ambient", &amb, ui("Ambient colors\0Blurred video\0Edge stretch\0", "Цвета кадра\0Размытие\0Растянуть края\0")))
                        {
                            ambientStyle.store(amb);
                            overlayAction(AmbientStyle, amb);
                        }
                    }

                    nextColumn(tbl);
                    int wmode = windowMode.load();
                    drawLabel(ui("Window mode", "Режим окна"));
                    if(combo("##Window mode", &wmode, ui("Fullscreen\0Borderless\0Windowed\0", "Полный экран\0Без рамки\0Окно\0")))
                    {
                        windowMode.store(wmode);
                        overlayAction(WindowMode, wmode);
                    }
                    bool hud = showHud.load();
                    if(toggle(ui("Full performance HUD", "Полная статистика (HUD)"), hud))
                        overlayAction(Hud, hud);
                    ImGui::TextDisabled(ui("%d × %d @ %.0f Hz  ·  Render %.1f ms", "%d × %d @ %.0f Гц  ·  Рендер %.1f мс"),
                        size.width(), size.height(), refreshRate, smoothedRenderMs);
                    endColumns(tbl);
                    break;
                }
                case 1: {
                    const bool tbl = beginColumns("VideoCols");
                    // Upscaling mapping: 0=Shader, 1=FSR 1.0, 2..5=VSR 1..4, 6=DLSS 4 SR, 7=DLSS 4.5 SR
                    int upscale = srModel ? srModel + 5 : vsrQuality ? vsrQuality + 1 : fsrEnabled ? 1 : 0;
                    drawLabel(ui("Upscaling", "Масштабирование"));
                    if(combo("##Upscaling", &upscale,
                        ui("DX12 shader\0AMD FSR 1.0 (EASU)\0RTX VSR Low\0RTX VSR Medium\0RTX VSR High\0RTX VSR Ultra\0DLSS 4 SR\0DLSS 4.5 SR\0", "Шейдер DX12\0AMD FSR 1.0 (EASU)\0RTX VSR Low\0RTX VSR Medium\0RTX VSR High\0RTX VSR Ultra\0DLSS 4 SR\0DLSS 4.5 SR\0")))
                        overlayAction(Upscale, upscale);
                    int targetIndex = upscaleHeight == 2160 ? 3 : upscaleHeight == 1800 ? 2 : upscaleHeight == 1 ? 1 : 0;
                    drawLabel(ui("Resolution", "Разрешение"));
                    if(combo("##Resolution", &targetIndex, ui("Output size\0Stream size (DLSS AA)\0Intermediate 1800p\0Intermediate 2160p\0", "Размер экрана\0Размер потока (DLSS AA)\0Промежуточные 1800p\0Промежуточные 2160p\0")))
                    {
                        upscaleHeight = targetIndex == 3 ? 2160 : targetIndex == 2 ? 1800 : targetIndex == 1 ? 1 : 0;
                        overlayAction(UpscaleTarget, upscaleHeight);
                    }
                    if(usingSr)
                    {
                        drawStatusDot(tint(0.19f, 0.82f, 0.35f), 3.5f);
                        ImGui::SameLine(0.0f, 5.0f);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextColored(tint(0.19f, 0.82f, 0.35f), ui("DLSS %s SR (%c) | %dx%d%s", "DLSS %s SR (%c) | %dx%d%s"), srModel == 2 ? "4.5" : "4",
                            srModel == 2 ? 'M' : 'K', upscaleSize.width(), upscaleSize.height(),
                            upscaleHeight == 1 ? " (DLAA)" : "");
                    }
                    else if(usingVsr)
                    {
                        drawStatusDot(tint(0.19f, 0.82f, 0.35f), 3.5f);
                        ImGui::SameLine(0.0f, 5.0f);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextColored(tint(0.19f, 0.82f, 0.35f), ui("RTX VSR Q%d | %dx%d", "RTX VSR Q%d | %dx%d"), vsrQuality, upscaleSize.width(), upscaleSize.height());
                    }
                    else if(usingFsr)
                    {
                        drawStatusDot(tint(0.19f, 0.82f, 0.35f), 3.5f);
                        ImGui::SameLine(0.0f, 5.0f);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextColored(tint(0.19f, 0.82f, 0.35f), ui("AMD FSR 1.0 (EASU) active", "AMD FSR 1.0 (EASU) активно"));
                    }
                    else if(vsrError)
                        ImGui::TextColored(tint(1.0f, 0.62f, 0.04f), ui("RTX VSR failed (0x%08x).", "Ошибка RTX VSR (0x%08x)."), vsrError);
                    else if(vsrQuality && !srModel)
                        ImGui::TextDisabled(ui("RTX VSR needs larger output size.", "Для RTX VSR нужен больший размер вывода."));

                    nextColumn(tbl);
                    int sharp = sharpnessSetting.load();
                    int sharpUi = sharp < 0 ? int(std::lround(activeSharpness * 100.0f)) : sharp;
                    const char *sharpFmt = sharp < 0 ? ui("Auto (%d%%)", "Авто (%d%%)") : "%d%%";
                    drawLabel(ui("Sharpness (CAS)", "Резкость (CAS)"));
                    if(ImGui::SliderInt("##Sharpness (CAS)", &sharpUi, 0, 100, sharpFmt))
                    {
                        sharpnessSetting.store(sharpUi);
                        overlayAction(Sharpness, sharpUi);
                    }
                    int generation = srModel ? 0 : generationMode;
                    ImGui::BeginDisabled(srModel || refreshRate < 119);
                    drawLabel(ui("Frame generation", "Генерация кадров"));
                    if(combo("##Frame generation", &generation, ui("Off\0Interpolation ×2\0DLSS FG ×2\0AMD FSR 3.1 FG ×2\0", "Выкл.\0Интерполяция ×2\0DLSS FG ×2\0AMD FSR 3.1 FG ×2\0")))
                        overlayAction(Generation, generation);
                    ImGui::EndDisabled();
                    ImGui::BeginDisabled(!usingSr && !usingVsr && !usingFsr);
                    bool split = splitScreen.load();
                    if(toggle(ui("A/B comparison", "Сравнение A/B"), split))
                        splitScreen.store(split);
                    if(split)
                    {
                        float pos = splitPosition.load() * 100.0f;
                        drawLabel(ui("A/B split", "Граница A/B"));
                        if(ImGui::SliderFloat("##A/B split position", &pos, 5.0f, 95.0f, "%.0f%%"))
                            splitPosition.store(pos / 100.0f);
                    }
                    ImGui::EndDisabled();
                    if(srModel && srFailed)
                        ImGui::TextColored(tint(1.0f, 0.62f, 0.04f), ui("DLSS SR unavailable.", "DLSS SR недоступен."));
                    if(generationMode == 2 && dlssFailed)
                        ImGui::TextColored(tint(1.0f, 0.62f, 0.04f), ui("DLSS FG unavailable.", "DLSS FG недоступна."));
                    if(generationMode == 3 && fsrFgFailed)
                        ImGui::TextColored(tint(1.0f, 0.62f, 0.04f), ui("FSR FG unavailable.", "FSR FG недоступна."));
                    endColumns(tbl);
                    break;
                }
                case 2: {
                    const bool tbl = beginColumns("ColorCols");
                    bool dither = ditherEnabled.load();
                    if(toggle(ui("Dithering", "Дизеринг"), dither))
                    {
                        ditherEnabled.store(dither);
                        overlayAction(Dither, dither ? 1 : 0);
                    }
                    bool deband = debandEnabled.load();
                    if(toggle(ui("H.265 deband filter", "Фильтр полос H.265"), deband))
                    {
                        debandEnabled.store(deband);
                        overlayAction(Deband, deband ? 1 : 0);
                    }
                    bool hdrPass = hdrPassthroughEnabled.load();
                    if(toggle(ui("Native HDR10 (Windows HDR)", "HDR10 напрямую (Windows HDR)"), hdrPass))
                    {
                        hdrPassthroughEnabled.store(hdrPass);
                        overlayAction(HdrPassthrough, hdrPass ? 1 : 0);
                    }
                    if(ImGui::Button(ui("Reset color", "Сбросить цвет")))
                    {
                        colorContrast.store(100); overlayAction(ColorContrast, 100);
                        colorSaturation.store(100); overlayAction(ColorSaturation, 100);
                        colorGamma.store(100); overlayAction(ColorGamma, 100);
                        hdrPaperWhite.store(203); overlayAction(HdrPaperWhite, 203);
                    }

                    nextColumn(tbl);
                    int contrast = colorContrast.load();
                    drawLabel(ui("Contrast", "Контраст"));
                    if(ImGui::SliderInt("##Contrast", &contrast, 80, 130, "%d%%"))
                    {
                        colorContrast.store(contrast);
                        overlayAction(ColorContrast, contrast);
                    }
                    int sat = colorSaturation.load();
                    drawLabel(ui("Saturation", "Насыщенность"));
                    if(ImGui::SliderInt("##Saturation", &sat, 50, 150, "%d%%"))
                    {
                        colorSaturation.store(sat);
                        overlayAction(ColorSaturation, sat);
                    }
                    int gam = colorGamma.load();
                    drawLabel(ui("Gamma", "Гамма"));
                    if(ImGui::SliderInt("##Gamma", &gam, 80, 125, "%d%%"))
                    {
                        colorGamma.store(gam);
                        overlayAction(ColorGamma, gam);
                    }
                    int white = hdrPaperWhite.load();
                    drawLabel(ui("HDR paper white", "Белый HDR"));
                    if(ImGui::SliderInt("##HDR paper white", &white, 120, 400, ui("%d nits", "%d нит")))
                    {
                        hdrPaperWhite.store(white);
                        overlayAction(HdrPaperWhite, white);
                    }
                    endColumns(tbl);
                    break;
                }
                case 3: {
                    const bool tbl = beginColumns("ControlCols");
                    int volume = overlayVolume.load();
                    const bool sysVol = systemVolumeMode.load();
                    drawLabel(sysVol ? ui("Windows volume", "Громкость Windows") : ui("Volume", "Громкость"));
                    if(ImGui::SliderInt("##Volume", &volume, 0, sysVol ? 100 : 128,
                        sysVol ? "%d%%" : "%d"))
                        overlayAction(Volume, volume);
                    bool mic = overlayMic.load();
                    if(toggle(ui("Microphone", "Микрофон"), mic))
                        overlayAction(Microphone, mic);
                    if(Ds5BtManager::instance().isConnected())
                    {
                        const int bat = Ds5BtManager::instance().batteryPercent();
                        const bool chg = Ds5BtManager::instance().isCharging();
                        const bool full = Ds5BtManager::instance().isFullyCharged();
                        const bool hp = Ds5BtManager::instance().isHeadphonesPlugged();
                        const bool hsMic = Ds5BtManager::instance().isHeadsetMicPlugged();
                        drawStatusDot(tint(0.19f, 0.82f, 0.35f), 3.5f);
                        ImGui::SameLine(0.0f, 5.0f);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextColored(tint(0.19f, 0.82f, 0.35f),
                            ui("DualSense BT: %d%%%s%s", "DualSense BT: %d%%%s%s"),
                            bat >= 0 ? bat : 0,
                            full ? ui(" (Full)", " (Полный)") : (chg ? ui(" (Charging)", " (Зарядка)") : ""),
                            hp ? (hsMic ? ui(" | Headset", " | Гарнитура") : ui(" | Headphones", " | Наушники")) : "");
                        bool btSpk = Ds5BtManager::instance().isSpeakerAudioEnabled();
                        if(toggle(ui("DualSense BT speaker audio", "Звук на динамик DualSense (BT)"), btSpk))
                            Ds5BtManager::instance().setSpeakerAudioEnabled(btSpk);
                    }

                    nextColumn(tbl);
                    bool conceal = errorConcealment.load();
                    if(toggle(ui("Hold clean frame", "Удерживать чистый кадр"), conceal))
                    {
                        errorConcealment.store(conceal);
                        overlayAction(ErrorConcealment, conceal ? 1 : 0);
                    }
                    bool kbOff = keyboardDisabled.load();
                    if(toggle(ui("Disable keyboard input", "Отключить клавиатуру"), kbOff))
                    {
                        keyboardDisabled.store(kbOff);
                        overlayAction(DisableKeyboard, kbOff ? 1 : 0);
                    }
                    if(ImGui::Button(ui("Reset gyro", "Сбросить гироскоп")))
                        overlayAction(ResetGyro, 0);
                    endColumns(tbl);
                    break;
                }
                case 4: {
                    const bool tbl = beginColumns("SessionCols");
                    if(ImGui::Button(ui("Disconnect", "Отключиться"), ImVec2(-1, 30.0f)))
                        overlayAction(Disconnect, 0);
                    if(ImGui::Button(ui("Put PS5 into rest mode", "Перевести PS5 в режим покоя"), ImVec2(-1, 30.0f)))
                        overlayAction(SleepConsole, 0);

                    nextColumn(tbl);
                    ImGui::PushStyleColor(ImGuiCol_Button, tint(0.34f, 0.14f, 0.16f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tint(0.56f, 0.20f, 0.22f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, tint(0.42f, 0.15f, 0.17f));
                    if(ImGui::Button(ui("Exit PS5Link", "Выйти из PS5Link"), ImVec2(-1, 30.0f)))
                        overlayAction(ExitApp, 0);
                    if(ImGui::Button(ui("Rest mode and exit", "Режим покоя и выход"), ImVec2(-1, 30.0f)))
                        overlayAction(SleepAndExit, 0);
                    ImGui::PopStyleColor(3);
                    endColumns(tbl);
                    break;
                }
                }
                ImGui::PopTextWrapPos();
            }
            ImGui::EndChild();
            ImGui::PopID();
            ImGui::PopStyleVar();

            // Slim footer row: All settings + shortcut hint on left, quick Disconnect on right
            const float footerLeftX = ImGui::GetCursorPosX();
            if(ImGui::Button(ui("All settings", "Все настройки")))
                overlayAction(OpenFullSettings, 0);
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled(ui("Ctrl+O  ·  Esc to close", "Ctrl+O  ·  Esc — закрыть"));
            if(overlayPage != 4)
            {
                const char *discLabel = ui("Disconnect", "Отключиться");
                const float discW = ImGui::CalcTextSize(discLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f;
                if(rowWidth >= 380.0f)
                {
                    ImGui::SameLine(footerLeftX + rowWidth - discW);
                    ImGui::PushStyleColor(ImGuiCol_Button, tint(0.32f, 0.14f, 0.16f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tint(0.52f, 0.19f, 0.21f));
                    if(ImGui::Button(discLabel, ImVec2(discW, 0.0f)))
                        overlayAction(Disconnect, 0);
                    ImGui::PopStyleColor(2);
                }
            }
            const ImVec2 winPos = ImGui::GetWindowPos();
            const ImVec2 winSize = ImGui::GetWindowSize();
            menuLeftPx.store(winPos.x);
            menuTopPx.store(winPos.y);
            menuRightPx.store(winPos.x + winSize.x);
            menuBottomPx.store(winPos.y + winSize.y);
            menuPopupOpen.store(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId));
            ImGui::End();
        }
        else
            menuPopupOpen.store(false);
        ImGui::Render();
        auto *commands = graphicsCommands.Get();
        transition(commands, target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto rtv = renderTargetView->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(target, nullptr, rtv);
        commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        auto *heap = imguiViews.Get();
        commands->SetDescriptorHeaps(1, &heap);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commands);
        transition(commands, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    }

    void beginFrame()
    {
        currentSlot = &frameSlots[nextSlot];
        nextSlot = (nextSlot + 1) % frameSlots.size();
        waitForFence(currentSlot->fence);
        currentSlot->decoded.reset();
        graphicsAllocator = currentSlot->allocator;
        views = currentSlot->color;
        downsampleViews = currentSlot->downsample;
        postProcessViews = currentSlot->postProcess;
        fsrViews = currentSlot->fsr;
        ambientViews = currentSlot->ambient;
        blitViews = currentSlot->blit;
        interpolationViews = currentSlot->motion;
        srViews = currentSlot->srMotion;
        check(graphicsAllocator->Reset(), "DX12 graphics allocator reset");
        check(graphicsCommands->Reset(graphicsAllocator.Get(), nullptr), "DX12 graphics reset");
    }

    HRESULT submit(const AVFrame *frame, bool display = true)
    {
        check(graphicsCommands->Close(), "DX12 graphics close");
        if(frame)
        {
            auto *retained = av_frame_clone(frame);
            if(!retained)
                throw std::bad_alloc();
            currentSlot->decoded.reset(retained, [](AVFrame *value) { av_frame_free(&value); });
            const auto &decoded = *reinterpret_cast<const AVD3D12VAFrame *>(frame->data[0]);
            if(decoded.sync_ctx.fence && decoded.sync_ctx.fence_value)
                check(graphicsQueue->Wait(decoded.sync_ctx.fence, decoded.sync_ctx.fence_value), "DX12 decode fence wait");
        }
        const auto fence = ++fenceValue;
        ID3D12CommandList *graphicsList = graphicsCommands.Get();
        graphicsQueue->ExecuteCommandLists(1, &graphicsList);
        check(graphicsQueue->Signal(graphicsFence.Get(), fence), "DX12 signal graphics fence");
        currentSlot->fence = fence;
        const bool tear = presentMode.load() == 0 && tearingSupported;
        HRESULT presented = S_OK;
        if(display)
        {
            QElapsedTimer call;
            call.start();
            presented = swapchain->Present(tear ? 0 : 1, tear ? DXGI_PRESENT_ALLOW_TEARING : 0);
            if(presented == S_OK)
            {
                presentCalls.push(call.nsecsElapsed());
                hasPresentPermit = false;
            }
            else if(presented == DXGI_STATUS_OCCLUDED)
            {
                swapchainOccluded = true;
                // Do not clear hasPresentPermit: DXGI_STATUS_OCCLUDED does not consume a latency token.
            }
            else
            {
                hasPresentPermit = false;
            }
        }
        check(device->GetDeviceRemovedReason(), "DX12 device status");
        check(presented, "DX12 present");
        return presented;
    }

    void presentOverlayOnly(HWND hwnd, QSize size, qreal refreshRate)
    {
        resize(hwnd, size);
        if(checkOccluded())
            return;
        if(!waitForPresentSlot(50))
            return;
        ComPtr<ID3D12Resource> backBuffer;
        check(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer)), "DX12 overlay back buffer");
        beginFrame();
        if(lastPresented && lastPresentedSize == size)
            copyTo(lastPresented.Get(), backBuffer.Get());
        else
            clearTexture(backBuffer.Get(), 0.08f);
        drawOverlay(backBuffer.Get(), size, refreshRate, lastPresentedUsedVsr, lastPresentedUsedFsr);
        submit(nullptr, true);
    }

    bool present(HWND hwnd, QSize size, const AVFrame &frame, bool continuous, qreal refreshRate)
    {
        if(frame.format != AV_PIX_FMT_D3D12 || !frame.data[0] || frame.width <= 0 || frame.height <= 0)
            throw std::runtime_error("DX12 requires a D3D12VA hardware frame");
        const bool isPqStream = frame.color_trc == AVCOL_TRC_SMPTE2084;
        const bool isHdrStream = isPqStream || frame.color_trc == AVCOL_TRC_ARIB_STD_B67;

        QElapsedTimer time;
        time.start();
        const auto &decoded = *reinterpret_cast<const AVD3D12VAFrame *>(frame.data[0]);
        if(!decoded.texture)
            throw std::runtime_error("D3D12VA returned an empty video texture");
        const auto sourceFormat = decoded.texture->GetDesc().Format;
        if(sourceFormat != DXGI_FORMAT_NV12 && sourceFormat != DXGI_FORMAT_P010)
            throw std::runtime_error("DX12 streaming requires NV12 or P010");
        resize(hwnd, size);
        if(capturePath.isEmpty() && checkOccluded())
        {
            ++occluded;
            historyValid = false;
            return false;
        }
        if(isHdrStream && (!hdrCheckTimer.isValid() || hdrCheckTimer.elapsed() >= 2000))
            detectDisplayCapabilities();
        const QSize sourceSize(frame.width, frame.height);
        if(sourceSize != lastSourceSize)
        {
            waitForFence(fenceValue);
            historyValid = false;
            resetSr();
            lastSourceSize = sourceSize;
        }
        const QSize fitArea = sourceSize.scaled(size, Qt::KeepAspectRatio);
        const QSize videoArea = aspect == Aspect::Stretch ? size : sourceSize.scaled(size,
            aspect == Aspect::Fill ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio);
        // Works on 16:9, 21:9 ultrawide, 16:10 laptops, Ambient bars, and arbitrary Windowed sizes.
        const QSize targetEnhancement = enhancementSize(sourceSize, fitArea);
        const bool canUpscale = targetEnhancement.width() > frame.width && targetEnhancement.height() > frame.height;
        const bool canDlssSr = (upscaleHeight == 1 && srModel > 0) || canUpscale;
        const QSize intermediate = (srModel > 0 ? canDlssSr : canUpscale) ? targetEnhancement : fitArea;
        if(intermediate != upscaleSize)
        {
            waitForFence(fenceValue);
            upscaleSize = intermediate;
            resetSr();
            enhanced.Reset();
            historyValid = false;
        }
        const bool useVsr = !srModel && vsrQuality && canUpscale;
        const bool useFsr = fsrEnabled && !srModel && !vsrQuality
            && (videoArea.width() > frame.width || videoArea.height() > frame.height);
        const bool wantDlss = !srModel && generationMode == 2 && !dlssFailed;
        const bool wantFsrFg = !srModel && generationMode == 3 && !fsrFgFailed;
        bool eligible = !srModel && (generationMode == 1 || wantDlss || wantFsrFg) && refreshRate >= 119.0;
        const qint64 gapUs = inputGap.isValid() ? inputGap.nsecsElapsed() / 1000 : 0;
        const qint64 gap = gapUs / 1000;
        inputGap.start();
        if(continuous && gapUs >= 8000 && gapUs <= 50000)
            smoothedInputIntervalUs = smoothedInputIntervalUs * 0.85 + std::clamp(double(gapUs), 14000.0, 35000.0) * 0.15;
        const bool clean = !(frame.flags & AV_FRAME_FLAG_CORRUPT) && !frame.decode_error_flags;
        if(errorConcealment.load() && !clean && presentedFrames > 0)
        {
            ++concealedFrames;
            historyValid = false;
            srHistoryValid = false;
            return false;
        }
        bool useSr = srModel && !srFailed && canDlssSr && clean;
        // Native HDR10 PQ passthrough when display is in Windows HDR mode and no active SDR-only NGX/FFX pass runs.
        const bool activeDlssFg = wantDlss && refreshRate >= 119.0;
        const bool activeFsrFg = wantFsrFg && refreshRate >= 119.0;
        const bool activeSr = srModel && !srFailed && canDlssSr;
        const bool wantHdr10 = isPqStream && hdrPassthroughEnabled.load() && displayHdrCapable
            && !useVsr && !activeSr && !activeDlssFg && !activeFsrFg;
        updateSwapchainColorSpace(wantHdr10);
        hdrPassthroughActive = wantHdr10 && (currentSwapchainColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        hdrToneMapped = isHdrStream && !hdrPassthroughActive;

        bool justInitializedNgx = false;
        if(useSr && !sr)
        {
            try
            {
                initializeSr(sourceSize, upscaleSize);
                justInitializedNgx = true;
            }
            catch(const std::exception &failure)
            {
                std::fprintf(stderr, "DX12 DLSS SR unavailable: %s\n", failure.what());
                resetSr();
                srFailed = true;
                useSr = false;
            }
        }
        if(eligible)
        {
            try
            {
                initializeInterpolation();
                const DXGI_FORMAT desiredHistoryFormat = (wantDlss || wantFsrFg) ? rgbFormat : highPrecisionFormat;
                if(videoHistoryFormat != desiredHistoryFormat)
                {
                    waitForFence(fenceValue);
                    for(auto &texture : videoHistory)
                        texture.Reset();
                    for(auto &texture : videoLumaHistory)
                        texture.Reset();
                    interpOutput.Reset();
                    videoHistoryFormat = desiredHistoryFormat;
                    historyValid = false;
                }
                for(auto &texture : videoHistory)
                    if(!texture)
                        texture = makeTexture(size, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, videoHistoryFormat);
                if(!wantDlss && !wantFsrFg && !interpOutput)
                    interpOutput = makeTexture(size, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, highPrecisionFormat);
                if(wantDlss)
                {
                    const bool hadDlss = dlss != nullptr;
                    initializeDlss(size);
                    if(!hadDlss && dlss)
                        justInitializedNgx = true;
                }
                else if(wantFsrFg)
                {
                    initializeFsrFg(size);
                }
            }
            catch(const std::exception &failure)
            {
                std::fprintf(stderr, "DX12 %s unavailable: %s\n",
                    wantFsrFg ? "FSR FG" : wantDlss ? "DLSS FG" : "interpolation", failure.what());
                if(wantFsrFg)
                    fsrFgFailed = true;
                else if(wantDlss)
                    dlssFailed = true;
                else
                    generationMode = 0;
                eligible = false;
            }
        }
        const bool generate = eligible && historyValid && clean && continuous && gap < 40;

        if(!waitForPresentSlot(250))
            return false;
        ComPtr<ID3D12Resource> backBuffer;
        check(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer)), "DX12 back buffer");
        const float sharpness = effectiveSharpness(useSr, useFsr);
        const bool splitActive = splitScreen.load() && (useSr || useVsr || useFsr);
        const float splitPos = splitPosition.load();
        if(!eligible && !postInput)
            postInput = makeTexture(size, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, highPrecisionFormat);
        if(splitActive && !splitBaseline)
            splitBaseline = makeTexture(size, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, highPrecisionFormat);
        auto *stageOutput = eligible ? videoHistory[1].Get() : postInput.Get();

        const auto ensureRgb = [&](DXGI_FORMAT fmt) {
            if(convertedSize != sourceSize || rgbSurfaceFormat != fmt || !rgb)
            {
                waitForFence(fenceValue);
                rgb = makeTexture(sourceSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, fmt);
                rgbSurfaceFormat = fmt;
                convertedSize = sourceSize;
            }
        };

        const bool hadVsr = vsr != nullptr;
        if(useVsr && !vsr)
        {
            int initStatus = 0;
            if(!ensureVideoSuperResolution(initStatus))
            {
                std::fprintf(stderr, "DX12 RTX VSR initialization failed (0x%08x). Using DX12 shader scaling.\n",
                    static_cast<unsigned>(initStatus));
                vsrError = unsigned(initStatus);
                setVsrQuality(0);
                return present(hwnd, size, frame, false, refreshRate);
            }
            justInitializedNgx = true;
        }
        beginFrame();
        if(useSr)
        {
            draw(frame, decoded, srInput[1].Get(), sourceSize);
            const int status = recordSr(sourceSize, !srHistoryValid || !continuous || gap >= 40);
            if(status != 1)
            {
                check(graphicsCommands->Close(), "DX12 discard unavailable DLSS SR");
                std::fprintf(stderr, "DX12 DLSS SR evaluation failed (0x%08x). Using DX12 shader scaling.\n", unsigned(status));
                waitForFence(fenceValue, 5000);
                resetSr();
                srFailed = true;
                historyValid = false;
                return present(hwnd, size, frame, false, refreshRate);
            }
            outputEnhanced(srOutput.Get(), stageOutput, videoArea, size);
        }
        else if(useFsr)
        {
            ensureRgb(highPrecisionFormat);
            draw(frame, decoded, rgb.Get(), sourceSize);
            recordFsr(rgb.Get(), stageOutput, sourceSize, size);
        }
        else if(!useVsr)
        {
            if(aspect == Aspect::Ambient && fitArea != size)
            {
                ensureRgb(highPrecisionFormat);
                draw(frame, decoded, rgb.Get(), sourceSize);
                recordAmbientBackdrop(rgb.Get(), stageOutput, sourceSize, size, ambientViews.Get());
                draw(frame, decoded, stageOutput, size, false);
            }
            else
                draw(frame, decoded, stageOutput, size);
        }
        else
        {
            ensureRgb(rgbFormat);
            if(!enhanced)
                enhanced = makeTexture(upscaleSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, rgbFormat);
            draw(frame, decoded, rgb.Get(), sourceSize);
            const int status = evaluateVideoSuperResolution(rgb.Get(), enhanced.Get());
            if(!vsr || status != 1)
            {
                check(graphicsCommands->Close(), "DX12 discard unavailable RTX VSR");
                std::fprintf(stderr, "DX12 RTX VSR failed (0x%08x). Using DX12 shader scaling.\n", static_cast<unsigned>(status));
                vsrError = unsigned(status);
                setVsrQuality(0);
                return present(hwnd, size, frame, false, refreshRate);
            }
            if(!hadVsr && vsr)
                justInitializedNgx = true;
            outputEnhanced(enhanced.Get(), stageOutput, videoArea, size);
        }
        if(splitActive)
        {
            if(aspect == Aspect::Ambient && fitArea != size)
            {
                ID3D12Resource *ambSource = useSr ? srInput[1].Get() : rgb.Get();
                if(ambSource)
                    recordAmbientBackdrop(ambSource, splitBaseline.Get(), sourceSize, size, currentSlot->baselineAmbient.Get());
                draw(frame, decoded, splitBaseline.Get(), size, ambSource == nullptr);
            }
            else
                draw(frame, decoded, splitBaseline.Get(), size);
        }
        if(eligible && wantDlss && clean)
        {
            const int status = recordDlss(size, generate);
            if(status != 1)
            {
                check(graphicsCommands->Close(), "DX12 discard unavailable DLSS FG");
                std::fprintf(stderr, "DX12 DLSS FG evaluation failed (0x%08x).\n", unsigned(status));
                releaseGenerationResources();
                dlssFailed = true;
                historyValid = false;
                return present(hwnd, size, frame, false, refreshRate);
            }
        }
        else if(eligible && wantFsrFg && clean)
        {
            const int status = recordFsrFg(size, generate);
            if(status != 1)
            {
                check(graphicsCommands->Close(), "DX12 discard unavailable FSR FG");
                std::fprintf(stderr, "DX12 FSR FG evaluation failed (%d).\n", status);
                releaseGenerationResources();
                fsrFgFailed = true;
                historyValid = false;
                return present(hwnd, size, frame, false, refreshRate);
            }
        }
        auto genSubmittedAt = std::chrono::steady_clock::time_point{};
        bool genPaced = false;
        qint64 fgPaceWaitNs = 0;
        const unsigned baseSeed = presentedFrames + generatedFrames;
        if(generate)
        {
            ID3D12Resource *genSource = nullptr;
            if(!wantDlss && !wantFsrFg)
            {
                recordInterpolation(interpOutput.Get(), size, false);
                genSource = interpOutput.Get();
            }
            else
                genSource = dlssGenerated.Get();
            // Final post-process (Deband, CAS, Color Grading, True 10/16-bit -> display Dither) AFTER FG!
            recordPostProcess(genSource, splitActive ? splitBaseline.Get() : nullptr,
                backBuffer.Get(), size, sharpness, splitActive, splitPos, baseSeed);
            drawOverlay(backBuffer.Get(), size, refreshRate, useVsr, useFsr);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT generatedLayout{};
            ComPtr<ID3D12Resource> generatedCapture;
            if(!generatedCapturePath.isEmpty())
                generatedCapture = recordReadback(backBuffer.Get(), generatedLayout);
            const HRESULT genPresented = submit(&frame);
            if(generatedCapture)
            {
                waitForFence(fenceValue, 5000);
                saveReadback(generatedCapture.Get(), generatedLayout, size, generatedCapturePath);
            }
            if(genPresented == DXGI_STATUS_OCCLUDED)
            {
                ++occluded;
                historyValid = false;
                return false;
            }
            genSubmittedAt = std::chrono::steady_clock::now();
            genPaced = true;
            ++generatedFrames;
            if(wantDlss && ++dlssFrames == 1)
                std::printf("DX12 DLSS Frame Generation active: 2x.\n");
            else if(wantFsrFg && ++fsrFgFrames == 1)
                std::printf("DX12 FSR Frame Generation active: 2x (%s).\n", fsrFgVersion.c_str());
            backBuffer.Reset();
            if(!waitForPresentSlot(250))
            {
                historyValid = false;
                return false;
            }
            check(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer)), "DX12 real back buffer");
            beginFrame();
        }
        // Final post-process for the real frame directly into the 10-bit swapchain backbuffer.
        recordPostProcess(stageOutput, splitActive ? splitBaseline.Get() : nullptr,
            backBuffer.Get(), size, sharpness, splitActive, splitPos, baseSeed + (generate ? 1u : 0u));
        // Keep a clean copy of the last displayed frame so the ImGui menu remains
        // interactive even if the video stream pauses or stalls.
        if(showMenu.load() || !lastPresented || lastPresentedSize != size)
        {
            if(!lastPresented || lastPresentedSize != size)
            {
                lastPresented = makeTexture(size, D3D12_RESOURCE_FLAG_NONE, swapchainFormat);
                lastPresentedSize = size;
            }
            copyTo(backBuffer.Get(), lastPresented.Get());
            lastPresentedUsedVsr = useVsr;
            lastPresentedUsedFsr = useFsr;
        }
        drawOverlay(backBuffer.Get(), size, refreshRate, useVsr, useFsr);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT capturedLayout{};
        ComPtr<ID3D12Resource> capture;
        const unsigned renderedIndex = presentedFrames + occluded;
        if(!capturePath.isEmpty() && renderedIndex == captureFrame - 1)
            capture = recordReadback(backBuffer.Get(), capturedLayout);
        // Half-frame pacing for 2x Frame Generation:
        // Only apply software sleep when tearing/VRR is active OR display refresh >> 2x stream rate (>=150 Hz);
        // on 120 Hz with VSync ON, Present(1, 0) already spaces frames at exact 8.33 ms intervals.
        const bool tearMode = presentMode.load() == 0 && tearingSupported;
        const bool needSoftwareFgPace = tearMode || refreshRate >= 150.0;
        if(genPaced && needSoftwareFgPace && fgPaceTimer && !showMenu.load() && !capture)
        {
            const auto paceStart = std::chrono::steady_clock::now();
            const double targetHalfUs = std::clamp(smoothedInputIntervalUs * 0.5 - 350.0, 2000.0, 18000.0);
            const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(paceStart - genSubmittedAt).count();
            const double remainUs = targetHalfUs - double(elapsedUs);
            if(remainUs > 300.0)
            {
                LARGE_INTEGER due{};
                due.QuadPart = -static_cast<LONGLONG>(remainUs * 10.0);
                if(SetWaitableTimer(fgPaceTimer, &due, 0, nullptr, nullptr, FALSE))
                    WaitForSingleObject(fgPaceTimer, 20);
                fgPaceWaitNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - paceStart).count();
            }
        }
        const auto presented = submit(generate ? nullptr : &frame);
        if(genPaced && presented != DXGI_STATUS_OCCLUDED)
            fgSpacing.push(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - genSubmittedAt).count());
        if(justInitializedNgx)
            waitForFence(fenceValue, 10000);
        srHistoryValid = useSr && presented != DXGI_STATUS_OCCLUDED;
        if(useSr)
            std::swap(srInput[0], srInput[1]);
        historyValid = eligible && clean && presented != DXGI_STATUS_OCCLUDED;
        if(eligible)
            std::swap(videoHistory[0], videoHistory[1]);
        if(capture)
        {
            waitForFence(fenceValue, 5000);
            saveReadback(capture.Get(), capturedLayout, size, capturePath);
        }
        if(presented == DXGI_STATUS_OCCLUDED)
        {
            ++occluded;
            return false;
        }
        ++presentedFrames;
        if(useSr && ++srFrames == srFramesAtSelection + 1)
            std::printf("DX12 DLSS SR active, requested preset %c, %d x %d to %d x %d.\n",
                srModel == 2 ? 'M' : 'K', frame.width, frame.height, upscaleSize.width(), upscaleSize.height());
        if(useVsr && ++enhancedFrames == 1)
            std::printf("DX12 RTX VSR active, quality %d, %d x %d to %d x %d.\n",
                vsrQuality, frame.width, frame.height, upscaleSize.width(), upscaleSize.height());
        if(useFsr && ++fsrFrames == 1)
            std::printf("DX12 AMD FSR 1.0 (EASU) active, %d x %d to %d x %d.\n",
                frame.width, frame.height, videoArea.width(), videoArea.height());
        if((useSr || useVsr) && upscaleSize != videoArea && ++supersampledFrames == 1)
            std::printf("DX12 intermediate scaling active: %d x %d to %d x %d.\n",
                upscaleSize.width(), upscaleSize.height(), videoArea.width(), videoArea.height());
        if(!capture)
        {
            const qint64 elapsedNs = std::max<qint64>(0, time.nsecsElapsed() - fgPaceWaitNs);
            submitTime.push(elapsedNs);
            const float frameMs = float(elapsedNs) / 1e6f;
            smoothedRenderMs = smoothedRenderMs > 0.01f ? smoothedRenderMs * 0.9f + frameMs * 0.1f : frameMs;
            frameTimeHistory[frameTimeHistoryPos] = frameMs;
            frameTimeHistoryPos = (frameTimeHistoryPos + 1) % frameTimeHistory.size();
            if(frameTimeHistoryCount < frameTimeHistory.size())
                ++frameTimeHistoryCount;
        }
        return true;
    }
};

struct Dx12ReplayWindow::RenderWorker
{
    struct Work
    {
        std::shared_ptr<AVFrame> frame;
        bool continuous;
        std::chrono::steady_clock::time_point queued;
        uint64_t sequence;
    };

    Dx12ReplayWindow *window;
    std::atomic<Work *> pending{nullptr};
    std::atomic<Work *> previous{nullptr};
    std::atomic<bool> stopping{false};
    std::atomic<bool> notified{false};
    std::atomic<unsigned> dropped{0};
    std::atomic<unsigned> closeArrivals{0};
    std::atomic<qint64> lastArrival{0};
    std::atomic<uint64_t> enqueueSequence{0};
    std::atomic<int> requestedVsr{-1}, requestedSr{-1}, requestedFsr{-1}, requestedGeneration{-1};
    std::atomic<int> requestedAspect{-1}, requestedUpscaleHeight{-1};
    std::atomic<unsigned> presented{0}, generated{0}, dlssGenerated{0}, dlssUpscaled{0};
    RingSamples queuedToPresent;
    std::mutex viewportMutex;
    HWND hwnd = nullptr;
    QSize pixels;
    QSize warmupInput, warmupOutput;
    qreal warmupRefresh = 60;
    std::atomic<bool> preparing{false};
    qreal refresh = 60;
    HANDLE wake = nullptr;
    HANDLE pauseAckEvent = nullptr;
    HANDLE resumeEvent = nullptr;
    std::atomic<bool> pauseRequested{false};
    std::atomic<bool> releaseSwapchainRequested{false};
    std::thread thread;

    explicit RenderWorker(Dx12ReplayWindow *owner) : window(owner)
    {
        wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        pauseAckEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        resumeEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        if(!wake || !pauseAckEvent || !resumeEvent)
        {
            if(wake) CloseHandle(wake);
            if(pauseAckEvent) CloseHandle(pauseAckEvent);
            if(resumeEvent) CloseHandle(resumeEvent);
            check(HRESULT_FROM_WIN32(GetLastError()), "DX12 render wake event");
        }
        try
        {
            thread = std::thread([this] { run(); });
        }
        catch(...)
        {
            CloseHandle(wake);
            CloseHandle(pauseAckEvent);
            CloseHandle(resumeEvent);
            throw;
        }
    }

    void pause(bool releaseSwapchain)
    {
        if(!thread.joinable() || stopping.load())
            return;
        ResetEvent(pauseAckEvent);
        ResetEvent(resumeEvent);
        if(releaseSwapchain)
            releaseSwapchainRequested.store(true);
        pauseRequested.store(true);
        SetEvent(wake);
        WaitForSingleObject(pauseAckEvent, 5000);
    }

    void resume()
    {
        pauseRequested.store(false);
        SetEvent(resumeEvent);
        SetEvent(wake);
    }

    void finish()
    {
        stopping.store(true);
        pauseRequested.store(false);
        SetEvent(resumeEvent);
        SetEvent(wake);
        if(thread.joinable())
            thread.join();
    }

    ~RenderWorker()
    {
        finish();
        delete pending.exchange(nullptr);
        delete previous.exchange(nullptr);
        CloseHandle(wake);
        CloseHandle(pauseAckEvent);
        CloseHandle(resumeEvent);
    }

    void applySettings(Pipeline &pipeline)
    {
        if(const int model = requestedSr.exchange(-1); model >= 0)
            pipeline.setSrModel(model);
        if(const int quality = requestedVsr.exchange(-1); quality >= 0)
            pipeline.setVsrQuality(quality);
        if(const int fsr = requestedFsr.exchange(-1); fsr >= 0)
            pipeline.fsrEnabled = fsr != 0;
        if(const int mode = requestedGeneration.exchange(-1); mode >= 0 && pipeline.generationMode != mode)
        {
            const int prevMode = pipeline.generationMode;
            pipeline.generationMode = mode;
            pipeline.dlssFailed = false;
            pipeline.dlssFramesAtSelection = pipeline.dlssFrames;
            pipeline.fsrFgFailed = false;
            pipeline.fsrFgFramesAtSelection = pipeline.fsrFgFrames;
            pipeline.historyValid = false;
            if(mode == 0 && prevMode != 0)
                pipeline.releaseGenerationResources();
            else
            {
                if(mode != 2 && pipeline.dlss)
                {
                    pipeline.waitForFence(pipeline.fenceValue, 5000);
                    pipeline.releaseDlss(pipeline.vsr == nullptr);
                }
                if(mode != 3 && pipeline.fsrFg)
                {
                    pipeline.waitForFence(pipeline.fenceValue, 5000);
                    pipeline.releaseFsrFg();
                }
                if(mode != 2 && mode != 3)
                {
                    pipeline.dlssMotionTexture.Reset();
                    pipeline.dlssDepth.Reset();
                    pipeline.dlssGenerated.Reset();
                    pipeline.dlssReal.Reset();
                }
            }
        }
        if(const int fit = requestedAspect.exchange(-1); fit >= 0 && int(pipeline.aspect) != fit)
        {
            pipeline.aspect = Pipeline::Aspect(std::clamp(fit, 0, 3));
            pipeline.historyValid = false;
            pipeline.srHistoryValid = false;
        }
        if(const int height = requestedUpscaleHeight.exchange(-1); height >= 0 && pipeline.upscaleHeight != height)
            pipeline.upscaleHeight = (height == 1 || height == 1800 || height == 2160) ? height : 0;
    }

    void run()
    {
        DWORD task = 0;
        const HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Games", &task);
        if(mmcss)
            AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
        else
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        const auto revert = qScopeGuard([&] { if(mmcss) AvRevertMmThreadCharacteristics(mmcss); });
        unsigned previousDropCount = 0;
        uint64_t lastSequence = 0;
        bool first = true;
        auto nextBalancedFrame = std::chrono::steady_clock::time_point{};
        constexpr auto frameInterval = std::chrono::microseconds(16667);
        try
        {
            while(true)
            {
                const bool interactiveOverlay = window->pipeline->showMenu.load()
                    || window->pipeline->bottomDockHovered.load()
                    || window->pipeline->dockAnim > 0.005f;
                const DWORD waitTimeout = interactiveOverlay ? 33u : INFINITE;
                const DWORD waitResult = WaitForSingleObject(wake, waitTimeout);
                if(waitResult != WAIT_OBJECT_0 && waitResult != WAIT_TIMEOUT)
                    break;
                if(pauseRequested.load())
                {
                    if(releaseSwapchainRequested.exchange(false))
                        window->pipeline->releaseSwapchain();
                    SetEvent(pauseAckEvent);
                    WaitForSingleObject(resumeEvent, INFINITE);
                    if(stopping.load())
                        break;
                    continue;
                }
                HWND target;
                QSize size;
                QSize warmInput, warmOutput;
                qreal hz;
                qreal warmHz;
                {
                    std::lock_guard<std::mutex> lock(viewportMutex);
                    target = hwnd;
                    size = pixels;
                    hz = refresh;
                    warmInput = std::exchange(warmupInput, QSize());
                    warmOutput = warmupOutput;
                    warmHz = warmupRefresh;
                }
                auto &pipeline = *window->pipeline;
                applySettings(pipeline);
                if(!warmInput.isEmpty())
                {
                    const auto prepared = qScopeGuard([&] { preparing.store(false); });
                    try { pipeline.prepareEffects(warmInput, warmOutput, warmHz); }
                    catch(const std::exception &failure)
                    {
                        std::fprintf(stderr, "DX12 RTX VSR warmup failed: %s\n", failure.what());
                        pipeline.setVsrQuality(0);
                    }
                }
                if(target && !size.isEmpty())
                {
                    const int pacing = pipeline.presentMode.load();
                    if(pacing == 1 && (pending.load() || previous.load()) && !stopping.load() && !pipeline.showMenu.load())
                    {
                        const auto now = std::chrono::steady_clock::now();
                        if(nextBalancedFrame > now)
                        {
                            const auto remainUs = std::chrono::duration_cast<std::chrono::microseconds>(nextBalancedFrame - now).count();
                            if(remainUs > 250 && remainUs <= 20000)
                            {
                                if(pipeline.balancedPaceTimer)
                                {
                                    LARGE_INTEGER due{};
                                    due.QuadPart = -static_cast<LONGLONG>(remainUs * 10);
                                    if(SetWaitableTimer(pipeline.balancedPaceTimer, &due, 0, nullptr, nullptr, FALSE))
                                        WaitForSingleObject(pipeline.balancedPaceTimer, 22);
                                }
                                else
                                    WaitForSingleObject(wake, static_cast<DWORD>(remainUs / 1000));
                            }
                        }
                    }
                    std::unique_ptr<Work> job(previous.exchange(nullptr));
                    if(job && (pacing == 0 || job->sequence <= lastSequence
                            || std::chrono::steady_clock::now() - job->queued > std::chrono::milliseconds(8)))
                    {
                        dropped.fetch_add(1);
                        job.reset();
                    }
                    if(!job)
                        job.reset(pending.exchange(nullptr));
                    if(job)
                    {
                        const auto now = std::chrono::steady_clock::now();
                        nextBalancedFrame = now - nextBalancedFrame > frameInterval
                            ? now + frameInterval : nextBalancedFrame + frameInterval;
                        const auto drops = dropped.load();
                        const bool shown = pipeline.present(target, size, *job->frame,
                            job->continuous && drops == previousDropCount && (lastSequence == 0 || job->sequence == lastSequence + 1), hz);
                        lastSequence = job->sequence;
                        if(shown)
                            queuedToPresent.push(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - job->queued).count());
                        presented.store(pipeline.presentedFrames);
                        generated.store(pipeline.generatedFrames);
                        dlssGenerated.store(pipeline.dlssFrames);
                        dlssUpscaled.store(pipeline.srFrames);
                        previousDropCount = drops;
                        if(shown && first)
                        {
                            first = false;
                            emit window->firstFramePresented();
                        }
                    }
                    else if((pipeline.showMenu.load() || pipeline.bottomDockHovered.load() || pipeline.dockAnim > 0.005f)
                        && !stopping.load())
                    {
                        pipeline.presentOverlayOnly(target, size, hz);
                    }
                    if(pending.load() || previous.load())
                        SetEvent(wake);
                }
                if(stopping.load() && (!target || size.isEmpty() || (!pending.load() && !previous.load())))
                    break;
            }
        }
        catch(const std::exception &error)
        {
            stopping.store(true);
            emit window->renderingFailed(QString::fromUtf8(error.what()));
        }
    }
};

Dx12ReplayWindow::Dx12ReplayWindow()
    : pipeline(std::make_unique<Pipeline>())
{
    setTitle(QStringLiteral("PS5Link — Direct3D 12"));
    resize(1280, 720);
    pipeline->initialize();
    pipeline->overlayAction = [this](int action, int value) { emit overlayAction(action, value); };
    connect(this, &Dx12ReplayWindow::overlayAction, this, [this](int action, int value) {
        switch(action)
        {
            case CloseMenu:
                openSettings();
                break;
            case OpenFullSettings:
            case Disconnect:
            case SleepConsole:
            case ExitApp:
            case SleepAndExit:
                if(pipeline->showMenu.load())
                    openSettings();
                if(action == OpenFullSettings)
                    emit allSettingsRequested();
                if(action == Disconnect)
                    emit disconnectRequested(false, false);
                else if(action == SleepConsole)
                    emit disconnectRequested(true, false);
                else if(action == ExitApp)
                    emit disconnectRequested(false, true);
                else if(action == SleepAndExit)
                    emit disconnectRequested(true, true);
                break;
            case Hud:
                pipeline->showHud.store(value != 0);
                if(settings)
                    settings->setShowStreamStats(value != 0);
                emit statsVisibilityChanged(value != 0);
                break;
            case Upscale: {
                // 0=Shader, 1=FSR 1.0, 2..5=VSR 1..4, 6=DLSS 4 SR, 7=DLSS 4.5 SR
                const int model = value >= 6 ? value - 5 : 0;
                const int vsr = (value >= 2 && value <= 5) ? value - 1 : 0;
                const bool fsr = (value == 1);
                worker->requestedSr.store(model);
                worker->requestedFsr.store(fsr ? 1 : 0);
                setVsrQuality(vsr);
                if(settings)
                {
                    settings->setDx12DlssSr(model);
                    settings->setDx12Fsr(fsr);
                }
                emit vsrQualityChanged(vsr);
                SetEvent(worker->wake);
                break;
            }
            case Generation:
                setFrameGeneration(value);
                if(settings)
                    settings->setDx12FrameGeneration(value);
                break;
            case Volume:
                pipeline->overlayVolume.store(value);
                if(pipeline->systemVolumeMode.load())
                    Ps5LinkSetSystemVolume(value);
                else if(settings)
                    settings->setAudioVolume(value);
                break;
            case ResetGyro:
                if(session)
                    session->ResetGyro();
                break;
            case Microphone:
                if(session)
                    session->SetMuted(!value);
                break;
            case PresentMode:
                if(settings)
                    settings->setDx12PresentMode(value);
                break;
            case AspectMode:
                if(settings)
                    settings->setDx12Aspect(value);
                break;
            case AmbientStyle:
                if(settings)
                    settings->setDx12AmbientStyle(value);
                break;
            case UpscaleTarget:
                if(settings)
                    settings->setDx12UpscaleHeight(value);
                break;
            case Sharpness:
                if(settings)
                    settings->setDx12Sharpness(value);
                break;
            case ErrorConcealment:
                if(settings)
                    settings->setDx12ErrorConcealment(value != 0);
                break;
            case DisableKeyboard:
                if(session)
                    session->SetKeyboardControllerEnabled(value == 0);
                if(settings)
                    settings->setKeyboardEnabled(value == 0);
                break;
            case WindowMode:
                applyWindowMode(value);
                if(settings)
                    settings->setDx12WindowMode(value);
                break;
            case Dither:
                if(settings)
                    settings->setDx12Dither(value != 0);
                break;
            case Deband:
                if(settings)
                    settings->setDx12Deband(value != 0);
                break;
            case HdrPassthrough:
                if(settings)
                    settings->setDx12HdrPassthrough(value != 0);
                break;
            case HdrPaperWhite:
                if(settings)
                    settings->setDx12HdrPaperWhite(value);
                break;
            case ColorContrast:
                if(settings)
                    settings->setDx12Contrast(value);
                break;
            case ColorSaturation:
                if(settings)
                    settings->setDx12Saturation(value);
                break;
            case ColorGamma:
                if(settings)
                    settings->setDx12Gamma(value);
                break;
        }
    }, Qt::QueuedConnection);
    worker = std::make_unique<RenderWorker>(this);
    auto *hudTimer = new QTimer(this);
    connect(hudTimer, &QTimer::timeout, this, &Dx12ReplayWindow::refreshOverlay);
    hudTimer->start(250);
    cursorTimer = new QTimer(this);
    cursorTimer->setSingleShot(true);
    connect(cursorTimer, &QTimer::timeout, this, [this] { updateCursorVisibility(false); });
    dockHideTimer = new QTimer(this);
    dockHideTimer->setSingleShot(true);
    connect(dockHideTimer, &QTimer::timeout, this, [this] {
        if(pipeline->bottomDockHovered.exchange(false))
        {
            if(imguiMouseDown)
            {
                imguiMouseDown = false;
                const std::lock_guard<std::mutex> lock(pipeline->inputMutex);
                if(pipeline->mouseClicks.size() < 64)
                    pipeline->mouseClicks.push_back(false);
            }
            updateCursorVisibility(false);
            if(worker)
                SetEvent(worker->wake);
        }
    });
    connect(this, &QWindow::screenChanged, this, [this] { updateViewport(); });
}

Dx12ReplayWindow::~Dx12ReplayWindow()
{
    worker.reset();
}

void Dx12ReplayWindow::bindSession(StreamSession *stream, QmlSettings *profile, int quality)
{
    session = stream;
    settings = profile;
    if(!QCoreApplication::instance()->property("ps5linkFrameGeneration").isValid()
        && !QCoreApplication::instance()->property("ps5linkPsnTestSeconds").toInt())
        setFrameGeneration(profile->dx12FrameGeneration());
    if(!QCoreApplication::instance()->property("ps5linkVsrQuality").isValid())
        setVsrQuality(quality);
    if(!QCoreApplication::instance()->property("ps5linkDlssSr").isValid())
        worker->requestedSr.store(profile->dx12DlssSr());
    if(!QCoreApplication::instance()->property("ps5linkFsr").isValid())
        worker->requestedFsr.store(profile->dx12Fsr() ? 1 : 0);
    if(!QCoreApplication::instance()->property("ps5linkDx12PresentMode").isValid())
        pipeline->presentMode.store(profile->dx12PresentMode());
    if(!QCoreApplication::instance()->property("ps5linkUpscaleHeight").isValid())
        worker->requestedUpscaleHeight.store(profile->dx12UpscaleHeight());
    worker->requestedAspect.store(profile->dx12Aspect());
    pipeline->ambientStyle.store(profile->dx12AmbientStyle());
    pipeline->errorConcealment.store(profile->dx12ErrorConcealment());
    pipeline->windowMode.store(profile->dx12WindowMode());
    pipeline->touchpadWidth.store(profile->dx12TouchpadWidth());
    pipeline->keyboardDisabled.store(!profile->keyboardEnabled());
    pipeline->systemVolumeMode.store(profile->volumeControlsSystem());
    pipeline->ditherEnabled.store(profile->dx12Dither());
    pipeline->debandEnabled.store(profile->dx12Deband());
    pipeline->hdrPassthroughEnabled.store(profile->dx12HdrPassthrough());
    pipeline->hdrPaperWhite.store(profile->dx12HdrPaperWhite());
    pipeline->colorContrast.store(profile->dx12Contrast());
    pipeline->colorSaturation.store(profile->dx12Saturation());
    pipeline->colorGamma.store(profile->dx12Gamma());
    const int lbMode = profile->lightbarMode();
    pipeline->showLightbarOverlay.store(lbMode == 2 || lbMode == 3);
    if(!QCoreApplication::instance()->property("ps5linkSharpness").isValid())
        pipeline->sharpnessSetting.store(profile->dx12Sharpness());
    SetEvent(worker->wake);
    refreshOverlay();
    updateCursorVisibility(false);
}

void Dx12ReplayWindow::applyWindowMode(int mode)
{
    mode = std::clamp(mode, 0, 2);
    pipeline->windowMode.store(mode);
    const Qt::WindowFlags desiredFlags = (mode == 1)
        ? (flags() | Qt::FramelessWindowHint)
        : (flags() & ~Qt::FramelessWindowHint);
    const bool flagsChanging = (desiredFlags != flags());
    if(worker && flagsChanging)
        worker->pause(true);
    if(mode == 0)
    {
        if(flagsChanging)
            setFlags(desiredFlags);
        showFullScreen();
    }
    else if(mode == 1)
    {
        if(flagsChanging)
            setFlags(desiredFlags);
        if(auto *scr = screen())
            setGeometry(scr->geometry());
        showNormal();
    }
    else
    {
        if(flagsChanging)
            setFlags(desiredFlags);
        showNormal();
        QRect saved = settings ? settings->streamGeometry() : QRect();
        if(!saved.isValid() || saved.width() < 320 || saved.height() < 180)
            saved = QRect(100, 100, 1280, 720);
        setGeometry(saved);
    }
    updateViewport();
    if(worker && flagsChanging)
        worker->resume();
}

void Dx12ReplayWindow::setReplayPreview(int mode, int page)
{
    pipeline->displayedFps = 60.0f;
    pipeline->smoothedRenderMs = 2.8f;
    pipeline->hudBitrateMbps.store(42.5f);
    pipeline->hudRttMs.store(4.0f);
    pipeline->hudGamepadConnected.store(true);
    pipeline->hudGamepadBattery.store(84);
    pipeline->hudGamepadCharging.store(false);
    pipeline->hudGamepadFull.store(false);
    pipeline->overlayMic.store(true);
    pipeline->overlayVolume.store(102);
    pipeline->overlayPage = std::clamp(page, 0, 4);
    pipeline->showHud.store(mode == 3);
    pipeline->bottomDockHovered.store(mode == 1 || mode == 3);
    pipeline->dockAnim = (mode == 1 || mode == 3) ? 1.0f : 0.0f;
    pipeline->showMenu.store(mode == 2);
    if(worker)
        SetEvent(worker->wake);
}

void Dx12ReplayWindow::setVsrQuality(int quality)
{
    worker->requestedVsr.store(quality);
    SetEvent(worker->wake);
}

void Dx12ReplayWindow::setFrameGeneration(int mode)
{
    worker->requestedGeneration.store(mode);
    SetEvent(worker->wake);
}

unsigned Dx12ReplayWindow::dlssGeneratedFrames() const
{
    return worker->dlssGenerated.load();
}

unsigned Dx12ReplayWindow::dlssSrFrames() const
{
    return worker->dlssUpscaled.load();
}

void Dx12ReplayWindow::refreshOverlay()
{
    const bool menuOpen = pipeline->showMenu.load();
    if(settings)
    {
        pipeline->showHud.store(settings->showStreamStats()
            || QCoreApplication::instance()->property("ps5linkPsnTestSeconds").toInt() > 0);
        const bool sysVol = settings->volumeControlsSystem();
        pipeline->systemVolumeMode.store(sysVol);
        if(!menuOpen)
        {
            if(sysVol)
            {
                if(pipeline->bottomDockHovered.load())
                {
                    const int v = Ps5LinkGetSystemVolume();
                    if(v >= 0)
                        pipeline->overlayVolume.store(v);
                }
            }
            else
                pipeline->overlayVolume.store(settings->audioVolume());
            pipeline->ambientStyle.store(settings->dx12AmbientStyle());
            pipeline->errorConcealment.store(settings->dx12ErrorConcealment());
            pipeline->ditherEnabled.store(settings->dx12Dither());
            pipeline->debandEnabled.store(settings->dx12Deband());
            pipeline->hdrPassthroughEnabled.store(settings->dx12HdrPassthrough());
            pipeline->hdrPaperWhite.store(settings->dx12HdrPaperWhite());
            pipeline->colorContrast.store(settings->dx12Contrast());
            pipeline->colorSaturation.store(settings->dx12Saturation());
            pipeline->colorGamma.store(settings->dx12Gamma());
        }
        const int lbMode = settings->lightbarMode();
        pipeline->showLightbarOverlay.store(lbMode == 2 || lbMode == 3);
        pipeline->hideOnScreenControls.store(settings->hideOnScreenControls());
        pipeline->showMicBtn.store(settings->showMicButton());
        pipeline->showAspectBtn.store(settings->showAspectButton());
        pipeline->showVolBtns.store(settings->showVolumeButtons());
        pipeline->touchpadWidth.store(settings->dx12TouchpadWidth());
        pipeline->keyboardDisabled.store(!settings->keyboardEnabled());
        if(session)
        {
            session->SetKeyboardControllerEnabled(settings->keyboardEnabled());
            session->SyncLiveSettings(settings->enableRumble(), settings->enableAdaptiveTriggers(),
                settings->enableGyroReset(), lbMode);
        }
    }
    if(Ds5BtManager::instance().isConnected())
    {
        pipeline->hudGamepadConnected.store(true);
        pipeline->hudGamepadBattery.store(Ds5BtManager::instance().batteryPercent());
        pipeline->hudGamepadCharging.store(Ds5BtManager::instance().isCharging());
        pipeline->hudGamepadFull.store(Ds5BtManager::instance().isFullyCharged());
    }
    else
    {
        bool foundPad = false;
        int batPct = -1;
        bool chg = false;
        bool full = false;
        const int numJoys = SDL_NumJoysticks();
        for(int i = 0; i < numJoys; ++i)
        {
            const SDL_JoystickID jid = SDL_JoystickGetDeviceInstanceID(i);
            if(jid < 0)
                continue;
            if(SDL_Joystick *joy = SDL_JoystickFromInstanceID(jid))
            {
                foundPad = true;
                switch(SDL_JoystickCurrentPowerLevel(joy))
                {
                    case SDL_JOYSTICK_POWER_WIRED: chg = true; batPct = -1; break;
                    case SDL_JOYSTICK_POWER_MAX:
                    case SDL_JOYSTICK_POWER_FULL: batPct = 100; full = true; break;
                    case SDL_JOYSTICK_POWER_MEDIUM: batPct = 65; break;
                    case SDL_JOYSTICK_POWER_LOW: batPct = 25; break;
                    case SDL_JOYSTICK_POWER_EMPTY: batPct = 5; break;
                    default: break;
                }
                break;
            }
        }
        pipeline->hudGamepadConnected.store(foundPad);
        pipeline->hudGamepadBattery.store(batPct);
        pipeline->hudGamepadCharging.store(chg);
        pipeline->hudGamepadFull.store(full);
    }
    if(session)
    {
        pipeline->lightbarRgb.store(session->GetLightbarRgb());
        pipeline->hudPacketLossPct.store(float(session->GetAveragePacketLoss() * 100.0));
        pipeline->hudBitrateMbps.store(float(session->GetMeasuredBitrate()));
        pipeline->hudRttMs.store(float(session->GetRttMs()));
        pipeline->hudCorruptFrames.store(session->GetCorruptDecodedFrames());
        pipeline->overlayMic.store(!session->GetMuted());
        const auto text = tr("Video %1 Mbps | RTT %2 ms | Packet loss %3%\n%4")
            .arg(session->GetMeasuredBitrate(), 0, 'f', 2)
            .arg(session->GetRttMs(), 0, 'f', 1)
            .arg(session->GetAveragePacketLoss() * 100, 0, 'f', 2)
            .arg(session->GetRecoveryStats()).toUtf8();
        std::atomic_store(&pipeline->hudText, std::make_shared<const std::string>(text.constData(), text.size()));
    }
}

namespace {
ImGuiKey mapMenuKey(int qtKey)
{
    switch(qtKey)
    {
        case Qt::Key_Up: return ImGuiKey_UpArrow;
        case Qt::Key_Down: return ImGuiKey_DownArrow;
        case Qt::Key_Left: return ImGuiKey_LeftArrow;
        case Qt::Key_Right: return ImGuiKey_RightArrow;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Space: return ImGuiKey_Space;
        case Qt::Key_Tab: return ImGuiKey_Tab;
        case Qt::Key_Backspace: return ImGuiKey_Backspace;
        default: return ImGuiKey_None;
    }
}
}

void Dx12ReplayWindow::keyPressEvent(QKeyEvent *event)
{
    if(event->key() == Qt::Key_O && event->modifiers() == Qt::ControlModifier)
    {
        if(!event->isAutoRepeat())
            openSettings();
        return;
    }
    if((event->key() == Qt::Key_F11 && event->modifiers() == Qt::NoModifier)
        || ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            && event->modifiers() == Qt::AltModifier))
    {
        if(!event->isAutoRepeat())
        {
            const int nextMode = pipeline->windowMode.load() == 2 ? 0 : 2;
            applyWindowMode(nextMode);
            if(settings)
                settings->setDx12WindowMode(nextMode);
        }
        return;
    }
    if(pipeline->showMenu.load())
    {
        if(event->key() == Qt::Key_Escape)
        {
            openSettings();
            return;
        }
        if(const auto key = mapMenuKey(event->key()); key != ImGuiKey_None)
        {
            const std::lock_guard<std::mutex> lock(pipeline->inputMutex);
            if(pipeline->keyEvents.size() < 64)
                pipeline->keyEvents.push_back({key, true});
            SetEvent(worker->wake);
        }
        return;
    }
    emit gameplayKey(event);
}

void Dx12ReplayWindow::keyReleaseEvent(QKeyEvent *event)
{
    if((event->key() == Qt::Key_F11 && event->modifiers() == Qt::NoModifier)
        || ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            && event->modifiers() == Qt::AltModifier))
        return;
    if(pipeline->showMenu.load())
    {
        if(const auto key = mapMenuKey(event->key()); key != ImGuiKey_None)
        {
            const std::lock_guard<std::mutex> lock(pipeline->inputMutex);
            if(pipeline->keyEvents.size() < 64)
                pipeline->keyEvents.push_back({key, false});
            SetEvent(worker->wake);
        }
        return;
    }
    if(event->key() != Qt::Key_O || event->modifiers() != Qt::ControlModifier)
        emit gameplayKey(event);
}

void Dx12ReplayWindow::openSettings()
{
    const bool visible = !pipeline->showMenu.load();
    if(visible && touchpadMouseActive && session)
    {
        session->HandleNormalizedTouchpad(0, 0.5f, 0.5f, touchpadMouseClickBtn);
        touchpadMouseActive = false;
        touchpadMouseClickBtn = false;
        touchpadMouseButton = Qt::NoButton;
    }
    if(dockHideTimer)
        dockHideTimer->stop();
    pipeline->bottomDockHovered.store(false);
    if(!visible && imguiMouseDown)
    {
        imguiMouseDown = false;
        const std::lock_guard<std::mutex> lock(pipeline->inputMutex);
        if(pipeline->mouseClicks.size() < 64)
            pipeline->mouseClicks.push_back(false);
    }
    pipeline->showMenu.store(visible);
    setProperty("ps5linkDx12MenuOpen", visible);
    if(session)
        session->BlockInput(visible);
    updateCursorVisibility(visible);
    if(worker)
        SetEvent(worker->wake);
}

void Dx12ReplayWindow::updateCursorVisibility(bool userMovedMouse)
{
    const bool allowHide = (!settings || settings->hideCursor())
        && !pipeline->showMenu.load()
        && !pipeline->bottomDockHovered.load()
        && !pipeline->splitScreen.load();
    if(userMovedMouse || !allowHide)
    {
        if(cursorHidden)
        {
            unsetCursor();
            cursorHidden = false;
        }
        if(cursorTimer)
        {
            if(allowHide)
                cursorTimer->start(2000);
            else
                cursorTimer->stop();
        }
    }
    else if(!cursorHidden)
    {
        setCursor(Qt::BlankCursor);
        cursorHidden = true;
    }
}

void Dx12ReplayWindow::feedMouse(QMouseEvent *event, int buttonState)
{
    const bool menuOpen = pipeline->showMenu.load();
    if(menuOpen && buttonState == 1 && event->button() == Qt::LeftButton && !pipeline->menuPopupOpen.load())
    {
        const QPointF phys = event->position() * devicePixelRatio();
        const float ml = pipeline->menuLeftPx.load();
        const float mt = pipeline->menuTopPx.load();
        const float mr = pipeline->menuRightPx.load();
        const float mb = pipeline->menuBottomPx.load();
        if(mr > ml && mb > mt
            && (phys.x() < ml || phys.x() > mr || phys.y() < mt || phys.y() > mb))
        {
            openSettings();
            event->accept();
            return;
        }
    }
    const bool dockAllowed = !menuOpen && !pipeline->hideOnScreenControls.load() && !touchpadMouseActive && height() > 0;
    const double distFromBottom = double(height()) - event->position().y();
    const bool currentlyHovered = pipeline->bottomDockHovered.load();
    const double triggerZone = currentlyHovered ? 116.0 : 60.0;
    const bool inBottomZone = dockAllowed && event->position().y() >= 0.0
        && distFromBottom >= 0.0 && distFromBottom <= triggerZone;
    if(inBottomZone)
    {
        if(dockHideTimer)
            dockHideTimer->stop();
        pipeline->bottomDockHovered.store(true);
    }
    else if(currentlyHovered)
    {
        if(buttonState == 1 || !dockAllowed)
        {
            if(dockHideTimer)
                dockHideTimer->stop();
            pipeline->bottomDockHovered.store(false);
        }
        else if(dockHideTimer && !dockHideTimer->isActive())
        {
            dockHideTimer->start(650);
        }
    }
    const bool overBottomDock = inBottomZone || (pipeline->bottomDockHovered.load() && buttonState != 1);
    updateCursorVisibility(true);
    if(!menuOpen && (!overBottomDock || touchpadMouseActive))
    {
        if(imguiMouseDown && buttonState == 0 && event->button() == Qt::LeftButton)
        {
            imguiMouseDown = false;
            const std::lock_guard<std::mutex> lock(pipeline->inputMutex);
            if(pipeline->mouseClicks.size() < 64)
                pipeline->mouseClicks.push_back(false);
            if(worker)
                SetEvent(worker->wake);
        }
        // Allow interactive A/B split-screen line dragging directly over the stream.
        if(!touchpadMouseActive && pipeline->splitScreen.load() && width() > 0 && (event->buttons() & Qt::LeftButton))
        {
            const float ratio = std::clamp(float(event->position().x() / width()), 0.05f, 0.95f);
            pipeline->splitPosition.store(ratio);
            event->accept();
            return;
        }
        // Use the DX12 stream window as a PlayStation touchpad (centered width fraction).
        if(session && width() > 0 && height() > 0)
        {
            const float frac = std::clamp(pipeline->touchpadWidth.load() / 100.0f, 0.25f, 1.0f);
            const float padWidth = width() * frac;
            const float padLeft = (width() - padWidth) * 0.5f;
            const float x = float(event->position().x());
            const float y = float(event->position().y());
            if(touchpadMouseActive || buttonState == 1)
            {
                const float normX = std::clamp((x - padLeft) / std::max(padWidth, 1.0f), 0.0f, 1.0f);
                const float normY = std::clamp(y / float(height()), 0.0f, 1.0f);
                if(buttonState == 1)
                {
                    if(x >= padLeft && x <= padLeft + padWidth)
                    {
                        const bool clickBtn = (event->button() != Qt::LeftButton);
                        touchpadMouseActive = true;
                        touchpadMouseClickBtn = clickBtn;
                        touchpadMouseButton = event->button();
                        session->HandleNormalizedTouchpad(1, normX, normY, clickBtn);
                    }
                }
                else if(touchpadMouseActive)
                {
                    if(buttonState < 0)
                    {
                        if(!touchpadMouseClickBtn && (event->buttons() & Qt::LeftButton))
                            session->HandleNormalizedTouchpad(-1, normX, normY, false);
                    }
                    else if(buttonState == 0 && (event->button() == touchpadMouseButton || touchpadMouseButton == Qt::NoButton))
                    {
                        session->HandleNormalizedTouchpad(0, normX, normY, touchpadMouseClickBtn);
                        touchpadMouseActive = false;
                        touchpadMouseClickBtn = false;
                        touchpadMouseButton = Qt::NoButton;
                    }
                }
            }
        }
        return;
    }
    const std::lock_guard<std::mutex> lock(pipeline->inputMutex);
    pipeline->mousePosition = event->position() * devicePixelRatio();
    if(buttonState >= 0 && event->button() == Qt::LeftButton && pipeline->mouseClicks.size() < 64)
    {
        imguiMouseDown = (buttonState != 0);
        pipeline->mouseClicks.push_back(imguiMouseDown);
    }
    if(worker)
        SetEvent(worker->wake);
    event->accept();
}

void Dx12ReplayWindow::mouseMoveEvent(QMouseEvent *event) { feedMouse(event, -1); }
void Dx12ReplayWindow::mousePressEvent(QMouseEvent *event) { feedMouse(event, 1); }
void Dx12ReplayWindow::mouseReleaseEvent(QMouseEvent *event) { feedMouse(event, 0); }

void Dx12ReplayWindow::wheelEvent(QWheelEvent *event)
{
    if(!pipeline->showMenu.load())
    {
        QWindow::wheelEvent(event);
        return;
    }
    const std::lock_guard<std::mutex> lock(pipeline->inputMutex);
    pipeline->mousePosition = event->position() * devicePixelRatio();
    pipeline->mouseWheel += float(event->angleDelta().y()) / 120.0f;
    if(worker)
        SetEvent(worker->wake);
    event->accept();
}

void Dx12ReplayWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    if(settings && settings->fullscreenDoubleClick() && !pipeline->showMenu.load()
        && !pipeline->bottomDockHovered.load() && !pipeline->splitScreen.load()
        && event->button() == Qt::LeftButton)
    {
        const int nextMode = pipeline->windowMode.load() == 2 ? 0 : 2;
        applyWindowMode(nextMode);
        settings->setDx12WindowMode(nextMode);
        event->accept();
        return;
    }
    QWindow::mouseDoubleClickEvent(event);
}

bool Dx12ReplayWindow::event(QEvent *ev)
{
    if(ev && ev->type() == QEvent::Leave)
    {
        if(pipeline->bottomDockHovered.load() && dockHideTimer && !dockHideTimer->isActive())
            dockHideTimer->start(650);
    }
    if(ev && ev->type() == QEvent::FocusOut)
    {
        if(touchpadMouseActive && session)
        {
            session->HandleNormalizedTouchpad(0, 0.5f, 0.5f, touchpadMouseClickBtn);
            touchpadMouseActive = false;
            touchpadMouseClickBtn = false;
            touchpadMouseButton = Qt::NoButton;
        }
        if(imguiMouseDown)
        {
            imguiMouseDown = false;
            const std::lock_guard<std::mutex> lock(pipeline->inputMutex);
            if(pipeline->mouseClicks.size() < 64)
                pipeline->mouseClicks.push_back(false);
            if(worker)
                SetEvent(worker->wake);
        }
    }
    if(ev && (ev->type() == QEvent::TouchBegin || ev->type() == QEvent::TouchUpdate
        || ev->type() == QEvent::TouchEnd || ev->type() == QEvent::TouchCancel))
    {
        if(session && !pipeline->showMenu.load() && !pipeline->bottomDockHovered.load()
            && !pipeline->splitScreen.load() && width() > 0 && height() > 0)
        {
            session->HandleTouchEvent(static_cast<QTouchEvent *>(ev), width(), height());
            ev->accept();
            return true;
        }
    }
    return QWindow::event(ev);
}

AVBufferRef *Dx12ReplayWindow::decoderDevice() const
{
    return pipeline->avDevice;
}

bool Dx12ReplayWindow::queueVideo(const AVFrame &frame, bool continuous)
{
    if(worker->stopping.load())
        return false;
    auto *retained = av_frame_clone(&frame);
    if(!retained)
        return false;
    const auto now = std::chrono::steady_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    const auto previous = worker->lastArrival.exchange(ns);
    if(previous && ns - previous < 10000000)
        worker->closeArrivals.fetch_add(1);
    auto *job = new RenderWorker::Work{
        std::shared_ptr<AVFrame>(retained, [](AVFrame *value) { av_frame_free(&value); }),
        continuous, now, worker->enqueueSequence.fetch_add(1) + 1
    };
    if(auto *old = worker->pending.exchange(job))
    {
        if(pipeline->presentMode.load() != 0)
            old = worker->previous.exchange(old);
        if(old)
        {
            worker->dropped.fetch_add(1);
            delete old;
        }
    }
    if(!worker->notified.exchange(true))
        emit videoQueued();
    SetEvent(worker->wake);
    return true;
}

void Dx12ReplayWindow::prepareVideo(QSize input, bool fullscreen)
{
    const std::lock_guard<std::mutex> lock(worker->viewportMutex);
    worker->warmupInput = input;
    worker->warmupOutput = (fullscreen && screen() ? screen()->size() : size()) * devicePixelRatio();
    worker->warmupRefresh = screen() ? screen()->refreshRate() : 60;
    worker->preparing.store(!input.isEmpty());
    SetEvent(worker->wake);
}

bool Dx12ReplayWindow::isPreparingVideo() const
{
    return worker->preparing.load();
}

void Dx12ReplayWindow::updateViewport()
{
    if(!worker)
        return;
    const std::lock_guard<std::mutex> lock(worker->viewportMutex);
    worker->pixels = isExposed() ? size() * devicePixelRatio() : QSize();
    worker->hwnd = reinterpret_cast<HWND>(winId());
    worker->refresh = screen() ? screen()->refreshRate() : 60;
    SetEvent(worker->wake);
}

void Dx12ReplayWindow::exposeEvent(QExposeEvent *event)
{
    QWindow::exposeEvent(event);
    updateViewport();
}

void Dx12ReplayWindow::resizeEvent(QResizeEvent *event)
{
    QWindow::resizeEvent(event);
    if(settings && pipeline && pipeline->windowMode.load() == 2 && isVisible()
        && width() >= 320 && height() >= 180)
        settings->setStreamGeometry(geometry());
    updateViewport();
}

void Dx12ReplayWindow::report()
{
    worker->finish();
    const auto samples = pipeline->submitTime.sortedCopy();
    if(samples.empty())
    {
        std::printf("DX12 present: %u frames, %d x %d, %u occluded, %u latency timeouts, %u RTX VSR.\n",
            pipeline->presentedFrames, pipeline->outputSize.width(), pipeline->outputSize.height(),
            pipeline->occluded, pipeline->presentTimeouts, pipeline->enhancedFrames);
        return;
    }
    if(!samples.empty())
    {
        std::printf("DX12 present: %u frames, %d x %d, %u occluded, %u RTX VSR; render-to-Present return p50 %.2f ms, p95 %.2f ms.\n",
            pipeline->presentedFrames, pipeline->outputSize.width(), pipeline->outputSize.height(), pipeline->occluded, pipeline->enhancedFrames,
            samples[(samples.size() - 1) / 2] / 1e6, samples[(samples.size() - 1) * 95 / 100] / 1e6);
        std::printf("DX12 interpolation: %u generated frames. Display refresh: %.1f Hz.\n",
            pipeline->generatedFrames, screen() ? screen()->refreshRate() : 0);
        if(pipeline->generationMode == 2 || pipeline->dlssFrames)
            std::printf("DX12 DLSS FG: %u generated frames.\n", pipeline->dlssFrames);
        if(pipeline->generationMode == 3 || pipeline->fsrFgFrames)
            std::printf("DX12 FSR FG: %u generated frames.\n", pipeline->fsrFgFrames);
        if(pipeline->srModel || pipeline->srFrames)
            std::printf("DX12 DLSS SR: %u upscaled frames.\n", pipeline->srFrames);
        if(pipeline->fsrEnabled || pipeline->fsrFrames)
            std::printf("DX12 AMD FSR 1.0: %u upscaled frames.\n", pipeline->fsrFrames);
        if(pipeline->concealedFrames)
            std::printf("DX12 error concealment: %u corrupt frame(s) held.\n", pipeline->concealedFrames);
        if(pipeline->upscaleHeight || pipeline->supersampledFrames)
            std::printf("DX12 intermediate scaling: %u downsampled frames.\n", pipeline->supersampledFrames);
        std::printf("DX12 render mailbox: %u replaced frames, %u arrivals less than 10 ms apart.\n",
            worker->dropped.load(), worker->closeArrivals.load());
        const auto reportPercentiles = [](const char *name, const RingSamples &ring) {
            if(ring.empty())
                return;
            const auto times = ring.sortedCopy();
            std::printf("DX12 %s p50 %.2f ms, p95 %.2f ms, p99 %.2f ms.\n", name,
                times[(times.size() - 1) / 2] / 1e6, times[(times.size() - 1) * 95 / 100] / 1e6,
                times[(times.size() - 1) * 99 / 100] / 1e6);
        };
        reportPercentiles("queue to Present return", worker->queuedToPresent);
        reportPercentiles("Present call", pipeline->presentCalls);
        reportPercentiles("FG half-frame spacing", pipeline->fgSpacing);
    }
}


unsigned Dx12ReplayWindow::presentedFrames() const
{
    return worker->presented.load();
}

unsigned Dx12ReplayWindow::generatedFrames() const
{
    return worker->generated.load();
}
