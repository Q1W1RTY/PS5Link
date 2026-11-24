#include <ps5link.h>
#include <dx12replay.h>

#include <host.h>
#include <qmlmainwindow.h>
#include <settings.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMap>
#include <QScopeGuard>
#include <cstdio>
#include <memory>
#include <QTimer>
#include <qt_windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <wincrypt.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

namespace {
const QString importedProfile = QStringLiteral("chiaki-ng");
const QByteArray secretPrefix("PS5Link-DPAPI:");

QByteArray cryptSecret(QByteArray bytes, bool protect)
{
    if(bytes.isEmpty())
        return {};
    DATA_BLOB input{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE *>(bytes.data())};
    DATA_BLOB output{};
    const BOOL ok = protect
        ? CryptProtectData(&input, L"PS5Link", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)
        : CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
    SecureZeroMemory(input.pbData, input.cbData);
    if(!ok)
        return {};
    QByteArray result(reinterpret_cast<const char *>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}

QByteArray encodeSecret(const QVariant &value)
{
    const QByteArray raw = value.toByteArray();
    if(raw.isEmpty())
        return {};
    const auto encrypted = cryptSecret(raw, true);
    if(encrypted.isEmpty() || cryptSecret(encrypted, false) != raw)
        return {};
    return secretPrefix + encrypted.toBase64();
}

int fail(const char *message)
{
    fprintf(stderr, "%s\n", message);
    return 1;
}

// Dynamic WLAN API definitions so PS5Link works on Ethernet-only Windows systems
// without linking wlanapi.dll statically.
struct WlanInterfaceInfo {
    GUID InterfaceGuid;
    WCHAR strInterfaceDescription[256];
    DWORD isState;
};
struct WlanInterfaceInfoList {
    DWORD dwNumberOfItems;
    DWORD dwIndex;
    WlanInterfaceInfo InterfaceInfo[1];
};
using FnWlanOpenHandle = DWORD(WINAPI *)(DWORD, PVOID, PDWORD, PHANDLE);
using FnWlanCloseHandle = DWORD(WINAPI *)(HANDLE, PVOID);
using FnWlanEnumInterfaces = DWORD(WINAPI *)(HANDLE, PVOID, WlanInterfaceInfoList **);
using FnWlanQueryInterface = DWORD(WINAPI *)(HANDLE, const GUID *, DWORD, PVOID, PDWORD, PVOID *, DWORD *);
using FnWlanSetInterface = DWORD(WINAPI *)(HANDLE, const GUID *, DWORD, DWORD, const PVOID, PVOID);
using FnWlanFreeMemory = VOID(WINAPI *)(PVOID);
constexpr DWORD kWlanIntfOpcodeMediaStreamingMode = 3;
constexpr DWORD kWlanInterfaceStateConnected = 1;

struct StreamBoostState
{
    std::mutex mutex;
    int refCount = 0;
    bool timerRaised = false;
    bool sleepBlocked = false;
    bool priorityRaised = false;
    DWORD previousPriorityClass = NORMAL_PRIORITY_CLASS;
    HMODULE wlanLib = nullptr;
    HANDLE wlanHandle = nullptr;
    std::vector<GUID> boostedWifiInterfaces;
    FnWlanCloseHandle closeHandle = nullptr;
    FnWlanSetInterface setInterface = nullptr;

    void acquire(const Ps5LinkBoostOptions &options)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if(++refCount > 1)
            return;
        if(options.turboMode)
        {
            if(timeBeginPeriod(1) == TIMERR_NOERROR)
                timerRaised = true;
            previousPriorityClass = GetPriorityClass(GetCurrentProcess());
            if(previousPriorityClass != 0 && previousPriorityClass != ABOVE_NORMAL_PRIORITY_CLASS
                && previousPriorityClass != HIGH_PRIORITY_CLASS)
            {
                if(SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS))
                    priorityRaised = true;
            }
        }
        if(options.preventSleep)
        {
            SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
            sleepBlocked = true;
        }
        if(!options.wifiMediaMode)
            return;

        wlanLib = LoadLibraryExW(L"wlanapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!wlanLib)
            return;
        auto openHandle = reinterpret_cast<FnWlanOpenHandle>(GetProcAddress(wlanLib, "WlanOpenHandle"));
        closeHandle = reinterpret_cast<FnWlanCloseHandle>(GetProcAddress(wlanLib, "WlanCloseHandle"));
        auto enumInterfaces = reinterpret_cast<FnWlanEnumInterfaces>(GetProcAddress(wlanLib, "WlanEnumInterfaces"));
        auto queryInterface = reinterpret_cast<FnWlanQueryInterface>(GetProcAddress(wlanLib, "WlanQueryInterface"));
        setInterface = reinterpret_cast<FnWlanSetInterface>(GetProcAddress(wlanLib, "WlanSetInterface"));
        auto freeMemory = reinterpret_cast<FnWlanFreeMemory>(GetProcAddress(wlanLib, "WlanFreeMemory"));
        if(!openHandle || !closeHandle || !enumInterfaces || !queryInterface || !setInterface || !freeMemory)
        {
            FreeLibrary(wlanLib);
            wlanLib = nullptr;
            return;
        }
        DWORD negotiated = 0;
        if(openHandle(2, nullptr, &negotiated, &wlanHandle) != ERROR_SUCCESS)
        {
            wlanHandle = nullptr;
            FreeLibrary(wlanLib);
            wlanLib = nullptr;
            return;
        }
        WlanInterfaceInfoList *list = nullptr;
        if(enumInterfaces(wlanHandle, nullptr, &list) == ERROR_SUCCESS && list)
        {
            for(DWORD i = 0; i < list->dwNumberOfItems; ++i)
            {
                const auto &iface = list->InterfaceInfo[i];
                if(iface.isState != kWlanInterfaceStateConnected)
                    continue;
                DWORD dataSize = 0;
                PVOID data = nullptr;
                BOOL alreadyOn = FALSE;
                if(queryInterface(wlanHandle, &iface.InterfaceGuid, kWlanIntfOpcodeMediaStreamingMode,
                        nullptr, &dataSize, &data, nullptr) == ERROR_SUCCESS && data)
                {
                    if(dataSize >= sizeof(BOOL))
                        alreadyOn = *static_cast<const BOOL *>(data);
                    freeMemory(data);
                }
                if(!alreadyOn)
                {
                    BOOL enable = TRUE;
                    if(setInterface(wlanHandle, &iface.InterfaceGuid, kWlanIntfOpcodeMediaStreamingMode,
                            sizeof(enable), &enable, nullptr) == ERROR_SUCCESS)
                        boostedWifiInterfaces.push_back(iface.InterfaceGuid);
                }
            }
            freeMemory(list);
        }
        if(!boostedWifiInterfaces.empty())
            fprintf(stdout, "[PS5Link] Wi-Fi media streaming mode enabled on %zu interface(s).\n",
                boostedWifiInterfaces.size());
    }

    void release()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if(refCount <= 0 || --refCount > 0)
            return;
        if(wlanHandle && setInterface)
        {
            for(const auto &guid : boostedWifiInterfaces)
            {
                BOOL disable = FALSE;
                setInterface(wlanHandle, &guid, kWlanIntfOpcodeMediaStreamingMode,
                    sizeof(disable), &disable, nullptr);
            }
            boostedWifiInterfaces.clear();
            if(closeHandle)
                closeHandle(wlanHandle, nullptr);
            wlanHandle = nullptr;
        }
        if(wlanLib)
        {
            FreeLibrary(wlanLib);
            wlanLib = nullptr;
        }
        if(sleepBlocked)
        {
            SetThreadExecutionState(ES_CONTINUOUS);
            sleepBlocked = false;
        }
        if(priorityRaised)
        {
            SetPriorityClass(GetCurrentProcess(), previousPriorityClass);
            priorityRaised = false;
        }
        if(timerRaised)
        {
            timeEndPeriod(1);
            timerRaised = false;
        }
    }
};

StreamBoostState &streamBoost()
{
    static StreamBoostState state;
    return state;
}
}

void Ps5LinkBeginStreamBoost(const Ps5LinkBoostOptions &options) { streamBoost().acquire(options); }
void Ps5LinkEndStreamBoost() { streamBoost().release(); }

namespace {
template<typename Fn>
bool withEndpointVolume(Fn &&fn)
{
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = SUCCEEDED(init);
    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice *device = nullptr;
    IAudioEndpointVolume *endpoint = nullptr;
    bool ok = false;
    if(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator))) && enumerator)
    {
        if(SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)) && device)
        {
            if(SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                    reinterpret_cast<void **>(&endpoint))) && endpoint)
            {
                ok = fn(endpoint);
                endpoint->Release();
            }
            device->Release();
        }
        enumerator->Release();
    }
    if(needUninit)
        CoUninitialize();
    return ok;
}
}

int Ps5LinkGetSystemVolume()
{
    float level = 0.0f;
    if(!withEndpointVolume([&](IAudioEndpointVolume *ep) { return SUCCEEDED(ep->GetMasterVolumeLevelScalar(&level)); }))
        return -1;
    return std::clamp(int(std::lround(level * 100.0f)), 0, 100);
}

bool Ps5LinkSetSystemVolume(int percent)
{
    const float scalar = std::clamp(percent, 0, 100) / 100.0f;
    return withEndpointVolume([&](IAudioEndpointVolume *ep) {
        return SUCCEEDED(ep->SetMasterVolumeLevelScalar(scalar, nullptr));
    });
}

bool Ps5LinkOnline()
{
    return QCoreApplication::instance()->property("ps5linkOnline").toBool();
}

bool Ps5LinkIsSecretProtected(const QVariant &value)
{
    return value.toByteArray().startsWith(secretPrefix);
}

QByteArray Ps5LinkEncodeSecret(const QVariant &value)
{
    return encodeSecret(value);
}

bool Ps5LinkSetSecret(QSettings &settings, const QString &key, const QVariant &value)
{
    if(value.toByteArray().isEmpty())
    {
        settings.remove(key);
        return true;
    }
    const auto encoded = encodeSecret(value);
    if(encoded.isEmpty())
    {
        qWarning("PS5Link could not encrypt a credential");
        return false;
    }
    settings.setValue(key, encoded);
    return true;
}

QVariant Ps5LinkGetSecret(const QSettings &settings, const QString &key, QMetaType type)
{
    const auto encoded = settings.value(key).toByteArray();
    if(!encoded.startsWith(secretPrefix))
        return {};
    const auto raw = cryptSecret(QByteArray::fromBase64(encoded.mid(secretPrefix.size())), false);
    if(type == QMetaType::fromType<QString>())
        return QString::fromUtf8(raw);
    return raw;
}

int Ps5LinkImport(const QString &sourceName)
{
    const QFileInfo sourceFile(sourceName);
    const bool fromFile = sourceFile.isFile() && sourceFile.suffix().compare("ini", Qt::CaseInsensitive) == 0;
    if(!fromFile && (sourceName.contains('/') || sourceName.contains('\\') || sourceName.isEmpty()))
        return fail("Invalid source profile name.");

    QSettings sourceDefault(QSettings::NativeFormat, QSettings::UserScope, "Chiaki", "Chiaki");
    sourceDefault.setFallbacksEnabled(false);
    const QString profile = sourceName == "default"
        ? sourceDefault.value("settings/current_profile").toString() : sourceName;
    if(!fromFile && (profile.contains('/') || profile.contains('\\')))
        return fail("Invalid active source profile.");
    std::unique_ptr<QSettings> sourceSettings;
    if(fromFile)
        sourceSettings = std::make_unique<QSettings>(sourceFile.absoluteFilePath(), QSettings::IniFormat);
    else
        sourceSettings = std::make_unique<QSettings>(QSettings::NativeFormat, QSettings::UserScope, "Chiaki",
            profile.isEmpty() ? QString("Chiaki") : "Chiaki-" + profile);
    QSettings &source = *sourceSettings;
    source.setFallbacksEnabled(false);

    QMap<QString, QVariant> values;
    const int count = source.beginReadArray("registered_hosts");
    int imported = 0;
    for(int i = 0; i < count; ++i)
    {
        source.setArrayIndex(i);
        const auto target = static_cast<ChiakiTarget>(source.value("target").toInt());
        if(!chiaki_target_is_ps5(target))
            continue;
        if(source.value("server_mac").toByteArray().size() != 6
            || source.value("rp_regist_key").toByteArray().size() != CHIAKI_SESSION_AUTH_SIZE
            || source.value("rp_key").toByteArray().size() != 16)
            return fail("A PS5 registration is incomplete.");
        const QString prefix = QString("registered_hosts/%1/").arg(++imported);
        for(const QString &key : {"target", "server_nickname", "server_mac", "rp_key_type"})
            values[prefix + key] = source.value(key);
        for(const QString &key : {"rp_regist_key", "rp_key", "console_pin"})
        {
            const auto raw = source.value(key);
            if(raw.toByteArray().isEmpty())
                continue;
            const auto encoded = encodeSecret(raw);
            if(encoded.isEmpty())
                return fail("Windows could not protect the imported registration.");
            values[prefix + key] = encoded;
        }
    }
    source.endArray();
    if(!imported)
        return fail("No PS5 registrations were found in the source profile.");

    values["registered_hosts/size"] = imported;
    for(const QString &key : {"settings/psn_auth_token", "settings/psn_refresh_token", "settings/psn_account_id"})
    {
        const auto raw = source.value(key);
        if(raw.toByteArray().isEmpty())
            continue;
        const auto encoded = encodeSecret(raw);
        if(encoded.isEmpty())
            return fail("Windows could not protect the imported PSN credentials.");
        values[key] = encoded;
    }
    for(const QString &key : source.allKeys())
        if(key.startsWith("controller_mappings/") || key == "settings/psn_auth_token_expiry"
            || key.startsWith("settings/bitrate") || key == "settings/dualsense_enabled")
            values[key] = source.value(key);

    QSettings destination(QSettings::IniFormat, QSettings::UserScope, "PS5Link", "PS5Link-" + importedProfile);
    destination.setFallbacksEnabled(false);
    destination.setAtomicSyncRequired(true);
    destination.remove("registered_hosts");
    destination.remove("controller_mappings");
    destination.remove("settings/psn_auth_token");
    destination.remove("settings/psn_refresh_token");
    destination.remove("settings/psn_account_id");
    destination.remove("settings/psn_auth_token_expiry");
    for(auto it = values.cbegin(); it != values.cend(); ++it)
        destination.setValue(it.key(), it.value());
    destination.sync();
    if(destination.status() != QSettings::NoError)
        return fail("PS5Link could not save the imported profile.");

    Settings settings(importedProfile);
    settings.SetResolutionRemotePS5(CHIAKI_VIDEO_RESOLUTION_PRESET_1080p);
    settings.SetFPSRemotePS5(CHIAKI_VIDEO_FPS_PRESET_60);
    settings.SetCodecRemotePS5(CHIAKI_CODEC_H265);
    settings.SetDiscoveryEnabled(false);
    settings.SetShowStreamStats(true);
    settings.SetCurrentProfile(importedProfile);
    settings.Sync();
    fprintf(stdout, "Imported %d PS5 registration(s) into the chiaki-ng profile.\n", imported);
    if(!values.contains("settings/psn_refresh_token"))
        fprintf(stdout, "No PSN refresh token was present in the source profile.\n");
    return 0;
}

int Ps5LinkReplay(QGuiApplication &app, const QString &fileName, bool fullscreen, bool directX12)
{
    Ps5LinkBeginStreamBoost();
    const auto endBoost = qScopeGuard([] { Ps5LinkEndStreamBoost(); });
    if(!QFileInfo(fileName).isFile())
        return fail("Choose a local HEVC file.");
    AVFormatContext *input = nullptr;
    const auto path = QFileInfo(fileName).absoluteFilePath().toUtf8();
    const auto *format = av_find_input_format("hevc");
    if(!format || avformat_open_input(&input, path.constData(), format, nullptr) < 0)
        return fail("Could not open the HEVC clip.");
    const auto closeInput = qScopeGuard([&] { avformat_close_input(&input); });
    if(avformat_find_stream_info(input, nullptr) < 0)
        return fail("Could not read HEVC stream information.");
    const int stream = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if(stream < 0)
        return fail("No video stream in the clip.");

    Settings settings(QStringLiteral("offline"));
    std::unique_ptr<QmlMainWindow> regular;
    std::unique_ptr<Dx12ReplayWindow> dx12;
    try
    {
        if(directX12)
            dx12 = std::make_unique<Dx12ReplayWindow>();
        else
            regular = std::make_unique<QmlMainWindow>(&settings, false, true);
    }
    catch(const std::exception &error)
    {
        return fail(error.what());
    }
    QWindow &window = dx12 ? static_cast<QWindow &>(*dx12) : *regular;
    ChiakiLog log{};
    chiaki_log_init(&log, CHIAKI_LOG_ALL, chiaki_log_cb_print, nullptr);
    ChiakiFfmpegDecoder decoder{};
    const auto ready = +[](ChiakiFfmpegDecoder *, void *) {};
    auto *device = dx12 ? dx12->decoderDevice() : regular->vulkanHwDeviceCtx();
    const auto *decoderName = dx12 ? "d3d12va" : device ? "vulkan" : "d3d11va";
    auto error = chiaki_ffmpeg_decoder_init(&decoder, &log, CHIAKI_CODEC_H265, 60,
        decoderName, device, ready, nullptr);
    if(error != CHIAKI_ERR_SUCCESS && !dx12)
        error = chiaki_ffmpeg_decoder_init(&decoder, &log, CHIAKI_CODEC_H265, 60, nullptr, nullptr, ready, nullptr);
    if(error != CHIAKI_ERR_SUCCESS)
        return fail("Could not initialize the HEVC decoder.");
    const auto closeDecoder = qScopeGuard([&] { chiaki_ffmpeg_decoder_fini(&decoder); });

    auto *packet = av_packet_alloc();
    if(!packet)
        return fail("Could not allocate a video packet.");
    const auto freePacket = qScopeGuard([&] { av_packet_free(&packet); });
    QTimer timer;
    timer.setSingleShot(true);
    timer.setTimerType(Qt::PreciseTimer);
    QElapsedTimer clock;
    quint64 submitted = 0;
    quint64 decoded = 0;
    quint64 queued = 0;
    if(dx12)
    {
        const auto *parameters = input->streams[stream]->codecpar;
        dx12->prepareVideo(QSize(parameters->width, parameters->height), fullscreen);
        QObject::connect(dx12.get(), &Dx12ReplayWindow::firstFramePresented, &window, [&] {
            if(app.property("ps5linkReplayMenu").toBool())
                dx12->openSettings();
        }, Qt::QueuedConnection);
        QObject::connect(dx12.get(), &Dx12ReplayWindow::renderingFailed, &window, [&](const QString &failure) {
            app.exit(fail(failure.toUtf8().constData()));
        }, Qt::QueuedConnection);
        QObject::connect(dx12.get(), &Dx12ReplayWindow::disconnectRequested, &window, [&](bool, bool) {
            app.quit();
        }, Qt::QueuedConnection);
    }
    QObject::connect(&timer, &QTimer::timeout, &window, [&] {
        if(!window.isExposed() || (dx12 && dx12->isPreparingVideo()))
        {
            timer.start(16);
            return;
        }
        if(!clock.isValid())
            clock.start();
        const int result = av_read_frame(input, packet);
        if(result < 0)
        {
            fprintf(stdout, "Offline replay submitted %llu, decoded %llu, queued %llu frame(s).\n",
                static_cast<unsigned long long>(submitted), static_cast<unsigned long long>(decoded),
                static_cast<unsigned long long>(queued));
            QTimer::singleShot(250, &window, [&] {
                if(dx12)
                    dx12->report();
                const qreal scale = window.devicePixelRatio();
                const bool visible = dx12 ? dx12->presentedFrames() > 0 : regular->hasVideo();
                fprintf(stdout, "Video visible: %s, drawable %dx%d, exposed: %s.\n",
                    visible ? "yes" : "no", qRound(window.width() * scale), qRound(window.height() * scale),
                    window.isExposed() ? "yes" : "no");
                const bool dlssPassed = app.property("ps5linkFrameGeneration").toInt() != 2 ||
                    (dx12 && dx12->dlssGeneratedFrames() > 0);
                const bool srPassed = !app.property("ps5linkDlssSr").toInt() || (dx12 && dx12->dlssSrFrames() > 0);
                app.exit(decoded && visible && dlssPassed && srPassed ? 0 : 1);
            });
            return;
        }
        if(packet->stream_index == stream)
        {
            ++submitted;
            chiaki_ffmpeg_decoder_video_sample_cb(packet->data, packet->size, 0, false, &decoder);
            int32_t lost = 0;
            auto frame = chiaki_ffmpeg_decoder_pull_frame(&decoder, &lost);
            if(frame.frame)
            {
                if(++decoded == 1)
                    fprintf(stdout, "Decoded %dx%d via %s.\n", frame.frame->width, frame.frame->height,
                        av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame.frame->format)));
                if(dx12)
                {
                    const auto releaseFrame = qScopeGuard([&] { av_frame_free(&frame.frame); });
                    try
                    {
                        if(dx12->queueVideo(*frame.frame, lost == 0 && !frame.recovered))
                            ++queued;
                    }
                    catch(const std::exception &error)
                    {
                        av_packet_unref(packet);
                        app.exit(fail(error.what()));
                        return;
                    }
                }
                else if(regular->prepareReplayFrame(frame))
                {
                    ++queued;
                    regular->presentFrame(frame, lost);
                }
                else
                    av_frame_free(&frame.frame);
            }
        }
        av_packet_unref(packet);
        // A late GUI timer tick should not feed consecutive packets in the
        // same event-loop turn and turn the replay into a decode burst.
        const qint64 untilNextFrame = static_cast<qint64>(submitted * 1000 / 60) - clock.elapsed();
        timer.start(static_cast<int>(qMax<qint64>(dx12 ? 4 : 0, untilNextFrame)));
    });
    if(regular)
    {
        regular->show();
        if(app.property("ps5linkReplayMenu").toBool())
        {
            QObject::connect(regular.get(), &QmlMainWindow::loadingTransitionCompleteChanged, regular.get(), [&] {
                if(regular->loadingTransitionComplete())
                    regular->showReplayMenu();
            }, Qt::SingleShotConnection);
        }
    }
    else
        window.show();
    if(fullscreen)
        QTimer::singleShot(0, &window, [&window, &dx12] {
            if(dx12)
                dx12->applyWindowMode(0);
            else
                window.showFullScreen();
            window.raise();
            window.requestActivate();
        });
    timer.start(0);
    return app.exec();
}
