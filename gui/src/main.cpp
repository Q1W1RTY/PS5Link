
// Use the native entry point instead of SDL's WinMain wrapper.
#ifdef CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#endif
#ifdef main
#undef main
#endif

// ugly workaround because Windows does weird things and ENOTIME
int real_main(int argc, char *argv[]);
int main(int argc, char *argv[]) { return real_main(argc, argv); }

#include <streamsession.h>
#include <settings.h>
#include <host.h>
#include <controllermanager.h>
#include <discoverymanager.h>
#include <qmlmainwindow.h>
#include <QApplication>
#include <QFontDatabase>
#include <QtTypes>

#ifdef CHIAKI_ENABLE_CLI
#include <chiaki-cli.h>
#endif

#include <chiaki/session.h>
#include <chiaki/regist.h>
#include <chiaki/base64.h>

#include <stdio.h>
#include <string.h>

#ifdef CHIAKI_HAVE_WEBENGINE
#include <QtWebEngineQuick>
#endif

#include <QCommandLineParser>
#ifdef PS5LINK_BUILD
#ifdef _WIN32
#include <ds5bt.h>
#endif
#include <ps5link.h>
#include <ps5linklocale.h>
#include <qmlbackend.h>
#include <QElapsedTimer>
#include <QTimer>
#endif
#include <QMap>
#include <QSurfaceFormat>

Q_DECLARE_METATYPE(ChiakiLogLevel)
Q_DECLARE_METATYPE(ChiakiRegistEventType)

#if defined(CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE) && defined(Q_OS_LINUX)
#include <QtPlugin>
Q_IMPORT_PLUGIN(SDInputContextPlugin)
#endif

#ifdef CHIAKI_ENABLE_CLI
struct CLICommand
{
	int (*cmd)(ChiakiLog *log, int argc, char *argv[]);
};

static const QMap<QString, CLICommand> cli_commands = {
	{ "discover", { chiaki_cli_cmd_discover } },
	{ "wakeup", { chiaki_cli_cmd_wakeup } }
};
#endif

int RunStream(QGuiApplication &app, const StreamSessionConnectInfo &connect_info);
int RunMain(QGuiApplication &app, Settings *settings, bool exit_app_on_stream_exit);

#ifdef PS5LINK_BUILD
namespace {
constexpr auto readLauncherSettings = "--read-launcher-settings";
constexpr auto writeLauncherSettings = "--write-launcher-settings";
constexpr auto importLauncherProfile = "--import-launcher-profile";
constexpr auto exportLauncherProfile = "--export-launcher-profile";
constexpr auto switchLauncherProfile = "--switch-launcher-profile";
constexpr auto launcherPsnUrl = "--launcher-psn-url";
constexpr auto launcherPsnLogin = "--launcher-psn-login";
constexpr auto launcherRegister = "--launcher-register-console";
constexpr auto launcherDiscover = "--launcher-discover-consoles";
constexpr auto launcherIdle = "--launcher-controller-idle";

bool hasLauncherSettingsCommand(int argc, char *const *argv)
{
    const auto isCommand = [](const char *arg, const char *command) {
        const size_t length = strlen(command);
        return strncmp(arg, command, length) == 0 && (arg[length] == '\0' || arg[length] == '=');
    };
    for(int i = 1; i < argc; ++i)
        if(isCommand(argv[i], readLauncherSettings) || isCommand(argv[i], writeLauncherSettings)
            || isCommand(argv[i], importLauncherProfile) || isCommand(argv[i], exportLauncherProfile)
            || isCommand(argv[i], switchLauncherProfile) || isCommand(argv[i], launcherPsnUrl)
            || isCommand(argv[i], launcherPsnLogin) || isCommand(argv[i], launcherRegister)
            || isCommand(argv[i], launcherDiscover) || isCommand(argv[i], launcherIdle))
            return true;
    return false;
}

int runLauncherSettingsCommand(int &argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    const QCommandLineOption read(readLauncherSettings + 2, "Read the active profile's launcher settings as JSON.");
    const QCommandLineOption write(writeLauncherSettings + 2, "Apply launcher settings from JSON on standard input.");
    const QCommandLineOption importProfile(importLauncherProfile + 2, "Import a backup into a new profile.", "file");
    const QCommandLineOption exportProfile(exportLauncherProfile + 2, "Export the active profile.", "file");
    const QCommandLineOption switchProfile(switchLauncherProfile + 2, "Activate an existing profile.", "profile");
    const QCommandLineOption profileOption("profile", "Select a configuration profile for reading or writing.", "profile");
    const QCommandLineOption psnUrl(launcherPsnUrl + 2, "Get the PSN sign-in URL.");
    const QCommandLineOption psnLogin(launcherPsnLogin + 2, "Finish PSN sign-in using a redirect from standard input.");
    const QCommandLineOption registerConsole(launcherRegister + 2, "Register a console using data from standard input.");
    const QCommandLineOption discoverConsoles(launcherDiscover + 2, "Discover consoles on the local network.");
    const QCommandLineOption idleMonitor(launcherIdle + 2, "Watch Bluetooth controller input for an idle launcher.", "launcher-pid");
    parser.addOptions({read, write, importProfile, exportProfile, switchProfile, profileOption,
        psnUrl, psnLogin, registerConsole, discoverConsoles, idleMonitor});
    if(!parser.parse(app.arguments()) || !parser.positionalArguments().isEmpty()
        || int(parser.isSet(read)) + int(parser.isSet(write)) + int(parser.isSet(importProfile))
            + int(parser.isSet(exportProfile)) + int(parser.isSet(switchProfile)) + int(parser.isSet(psnUrl))
            + int(parser.isSet(psnLogin)) + int(parser.isSet(registerConsole)) + int(parser.isSet(discoverConsoles))
            + int(parser.isSet(idleMonitor)) != 1)
        return 2;
    if(parser.isSet(profileOption) && !(parser.isSet(read) || parser.isSet(write)))
        return 2;

    QSettings defaults(QSettings::IniFormat, QSettings::UserScope, app.organizationName(), app.applicationName());
    defaults.setFallbacksEnabled(false);
    const auto profile = parser.isSet(profileOption)
        ? parser.value(profileOption) : defaults.value("settings/current_profile").toString();
    if(parser.isSet(importProfile))
        return Settings::RunLauncherProfileCommand(profile, "import", parser.value(importProfile));
    if(parser.isSet(exportProfile))
        return Settings::RunLauncherProfileCommand(profile, "export", parser.value(exportProfile));
    if(parser.isSet(switchProfile))
        return Settings::RunLauncherProfileCommand(profile, "switch", parser.value(switchProfile));
    if(parser.isSet(psnUrl))
        return Settings::RunLauncherSetupCommand(profile, "psn-url");
    if(parser.isSet(psnLogin))
        return Settings::RunLauncherSetupCommand(profile, "psn-login");
    if(parser.isSet(registerConsole))
        return Settings::RunLauncherSetupCommand(profile, "register");
    if(parser.isSet(discoverConsoles))
        return Settings::RunLauncherSetupCommand(profile, "discover");
    if(parser.isSet(idleMonitor))
    {
        bool valid = false;
        const auto pid = parser.value(idleMonitor).toULong(&valid);
        return valid && pid ? Settings::RunLauncherIdleMonitor(profile, pid) : 2;
    }
    return Settings::RunLauncherSettingsCommand(profile, parser.isSet(write));
}
}
#endif

int real_main(int argc, char *argv[])
{
	qRegisterMetaType<DiscoveryHost>();
	qRegisterMetaType<RegisteredHost>();
	qRegisterMetaType<HostMAC>();
	qRegisterMetaType<ChiakiQuitReason>();
	qRegisterMetaType<ChiakiRegistEventType>();
	qRegisterMetaType<ChiakiLogLevel>();

#ifdef PS5LINK_BUILD
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QGuiApplication::setOrganizationName("PS5Link");
    QGuiApplication::setApplicationName("PS5Link");
    QGuiApplication::setApplicationDisplayName("PS5Link");
    if(hasLauncherSettingsCommand(argc, argv))
        return runLauncherSettingsCommand(argc, argv);
#else
	QGuiApplication::setOrganizationName("Chiaki");
	QGuiApplication::setApplicationName("Chiaki");
	QGuiApplication::setApplicationDisplayName("chiaki-ng");
#endif
	QGuiApplication::setApplicationVersion(CHIAKI_VERSION);
#if defined(Q_OS_MACOS)
	// Use a plain CAMetalLayer for Vulkan windows. Qt's own layer presents in step with each
	// live resize, but our swapchain presents from the render thread, so every resize step
	// stalled and resizing the window became very slow.
	qputenv("QT_MTL_NO_TRANSACTION", "1");
	// The Vulkan loader also reads Homebrew's driver folder, so with Homebrew's molten-vk installed
	// the app loaded a second MoltenVK beside its own and listed every GPU twice. Use only the
	// bundled one. Setting any of these variables yourself keeps your choice.
	const QString moltenvk_icd = QFileInfo(QString::fromLocal8Bit(argv[0])).canonicalPath()
		+ "/../Resources/vulkan/icd.d/MoltenVK_icd.json";
	if(QFileInfo::exists(moltenvk_icd) && !qEnvironmentVariableIsSet("VK_DRIVER_FILES")
		&& !qEnvironmentVariableIsSet("VK_ICD_FILENAMES") && !qEnvironmentVariableIsSet("VK_LOADER_DRIVERS_SELECT")
		&& !qEnvironmentVariableIsSet("VK_LOADER_DRIVERS_DISABLE"))
		qputenv("VK_DRIVER_FILES", QFileInfo(moltenvk_icd).canonicalFilePath().toUtf8());
#endif
#if defined(Q_OS_LINUX)
	if(qEnvironmentVariableIsSet("FLATPAK_ID"))
		QGuiApplication::setDesktopFileName(qEnvironmentVariable("FLATPAK_ID"));
	else
#endif
		QGuiApplication::setDesktopFileName("chiaki-ng");

	qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--disable-gpu");
#if defined(Q_OS_WIN)
	const size_t cSize = strlen(argv[0])+1;
	wchar_t wc[cSize];
	mbstowcs (wc, argv[0], cSize);
	QString import_path = QFileInfo(QString::fromWCharArray(wc)).dir().absolutePath() + "/qml";
	qputenv("QML_IMPORT_PATH", import_path.toUtf8());
	// Microsoft's OpenCL, OpenGL and Vulkan Compatibility Pack adds the Dozen driver (Vulkan on
	// D3D12) beside the GPU's own, and loading it can crash the app even when it isn't used. Where
	// Dozen is the only Vulkan driver, Vulkan then finds no device and chiaki-ng uses OpenGL,
	// which the pack also provides. Setting either variable yourself keeps your choice.
	if(!qEnvironmentVariableIsSet("VK_LOADER_DRIVERS_DISABLE") && !qEnvironmentVariableIsSet("VK_LOADER_DRIVERS_SELECT"))
		qputenv("VK_LOADER_DRIVERS_DISABLE", "*dzn*");
#endif
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
	qputenv("ANV_VIDEO_DECODE", "1");
	qputenv("RADV_PERFTEST", "video_decode");
#endif
#ifdef CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	if (qEnvironmentVariableIsSet("SteamDeck"))
		qputenv("QT_IM_MODULE", "sdinput");
#endif

	ChiakiErrorCode err = chiaki_lib_init();
	if(err != CHIAKI_ERR_SUCCESS)
	{
		fprintf(stderr, "Chiaki lib init failed: %s\n", chiaki_error_string(err));
		return 1;
	}

    SDL_SetHint(SDL_HINT_APP_NAME, "chiaki-ng");

	if(SDL_Init(SDL_INIT_AUDIO) < 0)
	{
		fprintf(stderr, "SDL Audio init failed: %s\n", SDL_GetError());
		return 1;
	}

	QGuiApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
#ifdef CHIAKI_HAVE_WEBENGINE
	QtWebEngineQuick::initialize();
#endif
	QApplication app(argc, argv);

#ifdef PS5LINK_BUILD
	installPs5LinkLanguage(app);
	const int uiFont = QFontDatabase::addApplicationFont(":/ps5link/Geologica-Regular.ttf");
	QFontDatabase::addApplicationFont(":/ps5link/Geologica-SemiBold.ttf");
	if (uiFont >= 0) {
		QFont font = app.font();
		font.setFamily(QFontDatabase::applicationFontFamilies(uiFont).first());
		app.setFont(font);
	}
	QGuiApplication::setWindowIcon(QIcon(":/ps5link/PS5Link.png"));
#elif defined(Q_OS_MACOS)
	QGuiApplication::setWindowIcon(QIcon(":/icons/chiaking_macos.svg"));
#else
	QGuiApplication::setWindowIcon(QIcon(":/icons/chiaking.svg"));
#endif

	QCommandLineParser parser;
	parser.setOptionsAfterPositionalArgumentsMode(QCommandLineParser::ParseAsPositionalArguments);
	parser.addHelpOption();
	
	QStringList cmds;
	cmds.append("stream");
	cmds.append("list");
#ifdef CHIAKI_ENABLE_CLI
	cmds.append(cli_commands.keys());
#endif

	parser.addPositionalArgument("command", cmds.join(", "));
	parser.addPositionalArgument("nickname", "Needed for stream command to get credentials for connecting. "
			"Use 'list' to get the nickname.");
	parser.addPositionalArgument("host", "Address to connect to (when using the stream command).");

	QCommandLineOption profile_option("profile", "", "profile", "Configuration profile");
	parser.addOption(profile_option);

	QCommandLineOption stream_exit_option("exit-app-on-stream-exit", "Exit the GUI application when the stream session ends.");
	parser.addOption(stream_exit_option);

	QCommandLineOption regist_key_option("registkey", "", "registkey");
	parser.addOption(regist_key_option);

	QCommandLineOption morning_option("morning", "", "morning");
	parser.addOption(morning_option);

	QCommandLineOption fullscreen_option("fullscreen", "Start window in fullscreen mode [maintains aspect ratio, adds black bars to fill unsused parts of screen if applicable] (only for use with stream command).");
	parser.addOption(fullscreen_option);

	QCommandLineOption dualsense_option("dualsense", "Enable DualSense haptics and adaptive triggers (PS5 and DualSense connected via USB only).");
	parser.addOption(dualsense_option);

	QCommandLineOption zoom_option("zoom", "Start window in fullscreen zoomed in to fit screen [maintains aspect ratio, cutting off edges of image to fill screen] (only for use with stream command)");
	parser.addOption(zoom_option);

	QCommandLineOption stretch_option("stretch", "Start window in fullscreen stretched to fit screen [distorts aspect ratio to fill screen] (only for use with stream command).");
	parser.addOption(stretch_option);

	QCommandLineOption passcode_option("passcode", "Automatically send your PlayStation login passcode (only affects users with a login passcode set on their PlayStation console).", "passcode");
	parser.addOption(passcode_option);

	QCommandLineOption opengl_option("opengl", "Switch the renderer to OpenGL from now on, for systems where Vulkan shows a blank window or crashes (switch back under Settings > Video > Renderer Backend).");
	parser.addOption(opengl_option);

#ifdef PS5LINK_BUILD
    const QCommandLineOption online("online", "Enable console connections and PSN.");
    const QCommandLineOption import_profile("import-chiaki", "Import a chiaki-ng profile. Supply default for its active profile, a profile name, or an exported INI file.", "source");
    const QCommandLineOption replay("replay", "Play a local Annex B HEVC 1080p60 clip with networking disabled.", "file");
    const QCommandLineOption reorder_window("reorder-window", "Wait for reordered units in an incomplete frame (0, 8 or 16 ms).", "ms", "0");
    const QCommandLineOption idr_recovery("idr-recovery", "Request a keyframe after unrecoverable video loss.");
    const QCommandLineOption no_idr_recovery("no-idr-recovery", "Disable automatic keyframe requests for this run.");
    const QCommandLineOption rtx_vsr("rtx-vsr", "Enable NVIDIA RTX Video Super Resolution (quality 1-4, 0 = off).", "quality", "0");
    const QCommandLineOption dlss_sr("dlss-sr", "Select experimental Direct3D 12 DLSS Super Resolution (4 or 4.5).", "model");
    const QCommandLineOption fsr("fsr", "Enable AMD FSR 1.0 (EASU) edge-adaptive upscaling in Direct3D 12.");
    const QCommandLineOption sharpness("sharpness", "Set DX12 CAS sharpness: auto or 0-100.", "level");
    const QCommandLineOption upscale_target("upscale-target", "Upscale to an intermediate height before DX12 output (stream, 1800 or 2160).", "height");
    const QCommandLineOption dx12_present("dx12-present", "Choose the DX12 output timing: low, balanced or vsync.", "mode");
    const QCommandLineOption controllers("controllers", "List detected gamepads locally.");
    const QCommandLineOption dx12("dx12", "Use Direct3D 12 for local replay or an online H.265 session.");
    const QCommandLineOption standard_renderer("standard-renderer", "Use the standard renderer for this online run, overriding the profile's DX12 setting.");
    const QCommandLineOption frame_interpolation("frame-interpolation", "Enable experimental 2x motion interpolation in Direct3D 12.");
    const QCommandLineOption dlss_fg("dlss-fg", "Select NVIDIA DLSS Frame Generation 2x in Direct3D 12.");
    const QCommandLineOption fsr_fg("fsr-fg", "Select AMD FSR 3.1 Frame Generation 2x in Direct3D 12.");
    const QCommandLineOption no_fg("no-frame-generation", "Disable DX12 frame generation for this run, overriding the profile.");
    const QCommandLineOption dx12_frame("dx12-frame", "Save the first DX12 output frame to a PNG.", "file");
    const QCommandLineOption dx12_frame_number("dx12-frame-number", "Select the presented DX12 frame to capture (first frame is 1).", "number", "1");
    const QCommandLineOption dx12_generated("dx12-generated-frame", "Save the first generated DX12 frame during local replay.", "file");
    const QCommandLineOption psn_test("psn-test", "Run a muted PSN smoke test with the sole registered PS5.", "seconds");
    const QCommandLineOption replay_menu("replay-menu", "Preview the stream menu during local replay.");
    const QCommandLineOption quick_connect("quick-connect", "Connect directly to the preferred or only registered console.");
    const QCommandLineOption launcher_session("launcher-session", "Return to the launcher when quick connect cannot start.");
    const QCommandLineOption advanced_settings("settings", "Open the advanced settings screen.");
    const QCommandLineOption adaptive_triggers("adaptive-triggers", "Enable or disable DualSense adaptive triggers (on or off).", "state");
    const QCommandLineOption haptics_opt("haptics", "Enable or disable DualSense 3000 Hz haptics and rumble (on or off).", "state");
    const QCommandLineOption bt_speaker("bt-speaker", "Enable or disable DualSense Bluetooth speaker audio output (on or off).", "state");
    const QCommandLineOption mic_unmuted("mic-unmuted", "Start stream session with microphone unmuted (on or off).", "state");
    const QCommandLineOption lightbar_opt("lightbar", "Set DualSense lightbar mode (off, pad, screen, both).", "mode");
    const QCommandLineOption polling_ms("polling-ms", "Set controller polling interval in milliseconds (1, 2, 4 or 8).", "ms");
    parser.addOptions({online, import_profile, replay, reorder_window, idr_recovery, rtx_vsr, dlss_sr, fsr, sharpness, controllers, dx12, standard_renderer,
        dx12_frame, dx12_frame_number, dx12_generated, psn_test, replay_menu, frame_interpolation, dlss_fg, fsr_fg, no_fg,
        upscale_target, no_idr_recovery, dx12_present, advanced_settings, quick_connect, launcher_session,
        adaptive_triggers, haptics_opt, bt_speaker, mic_unmuted, lightbar_opt, polling_ms});
#endif
	parser.process(app);
	QStringList args = parser.positionalArguments();

#ifdef PS5LINK_BUILD
    app.setProperty("ps5linkOpenAdvancedSettings", parser.isSet(advanced_settings));
    app.setProperty("ps5linkQuickConnect", parser.isSet(quick_connect) && parser.isSet(online) && !parser.isSet(advanced_settings));
    app.setProperty("ps5linkLauncherSession", parser.isSet(launcher_session) && app.property("ps5linkQuickConnect").toBool());
    const auto windowMs = parser.value(reorder_window);
    if(windowMs != "0" && windowMs != "8" && windowMs != "16")
    {
        fprintf(stderr, "Expected --reorder-window 0, 8 or 16.\n");
        return 2;
    }
    if(parser.isSet(reorder_window))
        app.setProperty("ps5linkReorderWindow", windowMs.toInt());
    if(parser.isSet(idr_recovery) && parser.isSet(no_idr_recovery))
    {
        fprintf(stderr, "Choose one automatic keyframe setting.\n");
        return 2;
    }
    if(parser.isSet(idr_recovery) || parser.isSet(no_idr_recovery))
        app.setProperty("ps5linkIdrRecovery", parser.isSet(idr_recovery));
    bool vsrValid = false;
    const int vsrQuality = parser.value(rtx_vsr).toInt(&vsrValid);
    if(!vsrValid || vsrQuality < 0 || vsrQuality > 4)
    {
        fprintf(stderr, "Expected --rtx-vsr quality 0 to 4.\n");
        return 2;
    }
    if(parser.isSet(fsr))
    {
        if(!parser.isSet(dx12) || vsrQuality || parser.isSet(dlss_sr))
        {
            fprintf(stderr, "Use --dx12 --fsr with RTX VSR and DLSS SR disabled.\n");
            return 2;
        }
        app.setProperty("ps5linkFsr", true);
        app.setProperty("ps5linkDlssSr", 0);
        app.setProperty("ps5linkVsrQuality", 0);
    }
    if(parser.isSet(dlss_sr))
    {
        const auto model = parser.value(dlss_sr);
        if(!parser.isSet(dx12) || vsrQuality || parser.isSet(dlss_fg) || parser.isSet(fsr_fg) || parser.isSet(frame_interpolation)
            || (model != "4" && model != "4.5"))
        {
            fprintf(stderr, "Use --dx12 --dlss-sr <4|4.5> with RTX VSR and frame generation disabled.\n");
            return 2;
        }
        app.setProperty("ps5linkDlssSr", model == "4.5" ? 2 : 1);
        app.setProperty("ps5linkFsr", false);
    }
    else if(parser.isSet(rtx_vsr))
    {
        app.setProperty("ps5linkDlssSr", 0);
        app.setProperty("ps5linkFsr", false);
    }
    if(parser.isSet(sharpness))
    {
        const auto val = parser.value(sharpness).toLower();
        if(!parser.isSet(dx12))
        {
            fprintf(stderr, "Use --dx12 --sharpness <auto|0..100>.\n");
            return 2;
        }
        if(val == "auto")
            app.setProperty("ps5linkSharpness", -1);
        else
        {
            bool ok = false;
            const int pct = val.toInt(&ok);
            if(!ok || pct < 0 || pct > 100)
            {
                fprintf(stderr, "Expected --sharpness auto or 0 to 100.\n");
                return 2;
            }
            app.setProperty("ps5linkSharpness", pct);
        }
    }
    if(parser.isSet(upscale_target))
    {
        const auto height = parser.value(upscale_target).toLower();
        if(!parser.isSet(dx12) || (height != "output" && height != "stream" && height != "1800" && height != "2160"))
        {
            fprintf(stderr, "Use --dx12 --upscale-target <output|stream|1800|2160>.\n");
            return 2;
        }
        app.setProperty("ps5linkUpscaleHeight", height == "output" ? 0 : height == "stream" ? 1 : height.toInt());
    }
    if(parser.isSet(dx12_present))
    {
        const int mode = QStringList{"low", "balanced", "vsync"}.indexOf(parser.value(dx12_present));
        if(!parser.isSet(dx12) || mode < 0)
        {
            fprintf(stderr, "Use --dx12 --dx12-present <low|balanced|vsync>.\n");
            return 2;
        }
        app.setProperty("ps5linkDx12PresentMode", mode);
    }
    if(parser.isSet(dx12_frame) && (!parser.isSet(dx12) || parser.value(dx12_frame).isEmpty()))
    {
        fprintf(stderr, "--dx12-frame requires --dx12 and a PNG file path.\n");
        return 2;
    }
    if(parser.isSet(dx12_frame))
        app.setProperty("ps5linkDx12FramePath", parser.value(dx12_frame));
    if(parser.isSet(dx12_frame_number))
    {
        bool valid = false;
        unsigned number = parser.value(dx12_frame_number).toUInt(&valid);
        if(!parser.isSet(dx12_frame) || !valid || !number)
        {
            fprintf(stderr, "--dx12-frame-number requires --dx12-frame and a positive frame number.\n");
            return 2;
        }
        app.setProperty("ps5linkDx12FrameNumber", number);
    }
    if(parser.isSet(dx12_generated))
    {
        if(!parser.isSet(replay) || !parser.isSet(dx12) || (!parser.isSet(dlss_fg) && !parser.isSet(fsr_fg) && !parser.isSet(frame_interpolation))
            || parser.value(dx12_generated).isEmpty())
        {
            fprintf(stderr, "--dx12-generated-frame requires local DX12 replay with frame generation.\n");
            return 2;
        }
        app.setProperty("ps5linkDx12GeneratedPath", parser.value(dx12_generated));
    }
    if(parser.isSet(replay_menu))
    {
        if(!parser.isSet(replay))
        {
            fprintf(stderr, "--replay-menu requires --replay.\n");
            return 2;
        }
        app.setProperty("ps5linkReplayMenu", true);
    }
    if(parser.isSet(psn_test))
    {
        bool validSeconds = false;
        const int duration = parser.value(psn_test).toInt(&validSeconds);
        if(!validSeconds || duration < 5 || duration > 120 || !parser.isSet(online)
            || parser.isSet(replay) || parser.isSet(import_profile) || parser.isSet(controllers)
            || !args.isEmpty())
        {
            fprintf(stderr, "Use --online --psn-test with a duration of 5 to 120 seconds.\n");
            return 2;
        }
        app.setProperty("ps5linkPsnTestSeconds", duration);
    }
    if(parser.isSet(dx12) && !parser.isSet(replay) && !parser.isSet(online))
    {
        fprintf(stderr, "--dx12 requires --replay or --online.\n");
        return 2;
    }
    if(parser.isSet(standard_renderer) && (!parser.isSet(online) || parser.isSet(dx12) || parser.isSet(replay)))
    {
        fprintf(stderr, "--standard-renderer requires --online and cannot be combined with --dx12 or --replay.\n");
        return 2;
    }
    const int generationOptions = int(parser.isSet(dlss_fg)) + int(parser.isSet(fsr_fg)) + int(parser.isSet(frame_interpolation)) + int(parser.isSet(no_fg));
    if(generationOptions && !parser.isSet(dx12))
    {
        fprintf(stderr, "Frame generation requires --dx12.\n");
        return 2;
    }
    if(parser.isSet(dx12) || parser.isSet(standard_renderer))
        app.setProperty("ps5linkDx12", parser.isSet(dx12) && parser.isSet(online));
    if(generationOptions > 1)
    {
        fprintf(stderr, "Choose a single frame generation option.\n");
        return 2;
    }
    if(parser.isSet(dlss_sr))
        app.setProperty("ps5linkFrameGeneration", 0);
    else if(generationOptions)
        app.setProperty("ps5linkFrameGeneration", parser.isSet(fsr_fg) ? 3 : parser.isSet(dlss_fg) ? 2 : parser.isSet(frame_interpolation) ? 1 : 0);
    if(parser.isSet(rtx_vsr))
        app.setProperty("ps5linkVsrQuality", vsrQuality);
    app.setProperty("ps5linkOnline", parser.isSet(online) && !parser.isSet(replay) && !parser.isSet(import_profile));
    if(parser.isSet(import_profile))
        return Ps5LinkImport(parser.value(import_profile));
    if(parser.isSet(controllers))
    {
#ifdef _WIN32
        Ds5BtManager::instance().startDiscovery(true);
#endif
        unsigned detected = 0, dualSense = 0;
        auto *manager = ControllerManager::GetInstance();
        for(int id : manager->GetAvailableControllers())
        {
            if(auto *controller = manager->OpenController(id))
            {
                ++detected;
                if(controller->IsDualSense() || controller->IsDualSenseEdge())
                    ++dualSense;
                controller->Unref();
            }
        }
        fprintf(stdout, "Gamepads: %u, DualSense: %u.\n", detected, dualSense);
        return 0;
    }
    if(!args.isEmpty() && args.first() != "list" && !Ps5LinkOnline())
    {
        fprintf(stderr, "Use --online to enable remote sessions.\n");
        return 2;
    }
    if(parser.isSet(replay))
        return Ps5LinkReplay(app, parser.value(replay), parser.isSet(fullscreen_option), parser.isSet(dx12));
#endif

	Settings settings(parser.isSet(profile_option) ? parser.value(profile_option) : QString());
	bool exit_app_on_stream_exit = parser.isSet(stream_exit_option);
	if(parser.isSet(profile_option))
		settings.SetCurrentProfile(parser.value(profile_option));
	Settings alt_settings(parser.isSet(profile_option) ? "" : settings.GetCurrentProfile());
#ifndef PS5LINK_BUILD
	if(!settings.GetCurrentProfile().isEmpty())
		QGuiApplication::setApplicationDisplayName(QString("chiaki-ng:%1").arg(settings.GetCurrentProfile()));
#endif
	bool use_alt_settings = false;
	if(!parser.isSet(profile_option))
		use_alt_settings = true;

	if(parser.isSet(opengl_option))
	{
		Settings *active_settings = use_alt_settings ? &alt_settings : &settings;
		active_settings->SetRenderBackend(RenderBackend::OpenGL);
		active_settings->SetVulkanStartupPending(false);
		active_settings->Sync();
	}

#ifdef PS5LINK_BUILD
	{
		Settings *active_settings = use_alt_settings ? &alt_settings : &settings;
		bool changed_gamepad_settings = false;
		if(parser.isSet(adaptive_triggers))
		{
			const QString v = parser.value(adaptive_triggers).toLower();
			active_settings->SetEnableAdaptiveTriggers(v == "on" || v == "1" || v == "true");
			changed_gamepad_settings = true;
		}
		if(parser.isSet(haptics_opt))
		{
			const QString v = parser.value(haptics_opt).toLower();
			active_settings->SetEnableRumble(v == "on" || v == "1" || v == "true");
			changed_gamepad_settings = true;
		}
		if(parser.isSet(bt_speaker))
		{
			const QString v = parser.value(bt_speaker).toLower();
			const bool on = (v == "on" || v == "1" || v == "true");
			active_settings->SetBtSpeakerAudio(on);
#ifdef _WIN32
			Ds5BtManager::instance().setSpeakerAudioEnabled(on);
#endif
			changed_gamepad_settings = true;
		}
		if(parser.isSet(mic_unmuted))
		{
			const QString v = parser.value(mic_unmuted).toLower();
			active_settings->SetStartMicUnmuted(v == "on" || v == "1" || v == "true");
			changed_gamepad_settings = true;
		}
		if(parser.isSet(lightbar_opt))
		{
			const QString v = parser.value(lightbar_opt).toLower();
			const int mode = (v == "off") ? 0 : (v == "screen") ? 2 : (v == "both") ? 3 : 1;
			active_settings->SetLightbarMode(mode);
			changed_gamepad_settings = true;
		}
		if(parser.isSet(polling_ms))
		{
			const int ms = parser.value(polling_ms).toInt();
			if(ms == 1 || ms == 2 || ms == 4 || ms == 8)
			{
				active_settings->SetControllerPollingMs(ms);
				ControllerManager::GetInstance()->SetPollingIntervalMs(ms);
				changed_gamepad_settings = true;
			}
		}
		if(changed_gamepad_settings)
			active_settings->Sync();
	}
#endif

	if(args.length() == 0)
		return RunMain(app, use_alt_settings ? &alt_settings : &settings, exit_app_on_stream_exit);

	if(args[0] == "list")
	{
		for(const auto &host : (use_alt_settings ? alt_settings : settings).GetRegisteredHosts())
			printf("Host: %s \n", host.GetServerNickname().toLocal8Bit().constData());
		return 0;
	}
	if(args[0] == "stream")
	{
		if(args.length() < 2)
			parser.showHelp(1);

		//QString host = args[sizeof(args) -1]; //the ip is always the last param for stream
		QString host = args[args.size()-1];
		QByteArray morning;
		QByteArray regist_key;
		QString initial_login_passcode;
		ChiakiTarget target = CHIAKI_TARGET_PS4_10;

		if(parser.value(regist_key_option).isEmpty() && parser.value(morning_option).isEmpty())
		{
			if(args.length() < 3)
				parser.showHelp(1);

			bool found = false;
			for(const auto &temphost : settings.GetRegisteredHosts())
			{
				if(temphost.GetServerNickname() == args[1])
				{
					found = true;
					morning = temphost.GetRPKey();
					regist_key = temphost.GetRPRegistKey();
					target = temphost.GetTarget();
					break;
				}
			}
			if(!found)
			{
				printf("No configuration found for '%s'\n", args[1].toLocal8Bit().constData());
				return 1;
			}
		}
		else
		{
			// TODO: explicit option for target
			regist_key = parser.value(regist_key_option).toUtf8();
			if(regist_key.length() > sizeof(ChiakiConnectInfo::regist_key))
			{
				printf("Given regist key is too long (expected size <=%llu, got %" PRIdQSIZETYPE")\n",
					(unsigned long long)sizeof(ChiakiConnectInfo::regist_key),
					regist_key.length());
				return 1;
			}
			regist_key += QByteArray(sizeof(ChiakiConnectInfo::regist_key) - regist_key.length(), 0);
			morning = QByteArray::fromBase64(parser.value(morning_option).toUtf8());
			if(morning.length() != sizeof(ChiakiConnectInfo::morning))
			{
				printf("Given morning has invalid size (expected %llu, got %" PRIdQSIZETYPE")\n",
					(unsigned long long)sizeof(ChiakiConnectInfo::morning),
					morning.length());
				printf("Given morning has invalid size (expected %llu)", (unsigned long long)sizeof(ChiakiConnectInfo::morning));
				return 1;
			}
		}
		if ((parser.isSet(stretch_option) && (parser.isSet(zoom_option) || parser.isSet(fullscreen_option))) || (parser.isSet(zoom_option) && parser.isSet(fullscreen_option)))
		{
			printf("Must choose between fullscreen, zoom or stretch option.");
			return 1;
		}
		if(parser.value(passcode_option).isEmpty())
		{
			//Set to empty if it wasn't given by user.
			initial_login_passcode = QString("");
		}
		else
		{
			initial_login_passcode = parser.value(passcode_option);
			if(initial_login_passcode.length() != 4)
			{
				printf("Login passcode must be 4 digits. You entered %" PRIdQSIZETYPE "digits)\n", initial_login_passcode.length());
				return 1;
			}
		}
		
		StreamSessionConnectInfo connect_info(
				use_alt_settings ? &alt_settings : &settings,
				target,
				std::move(host),
				QString(),
				std::move(regist_key),
				std::move(morning),
				std::move(initial_login_passcode),
				QString(),
				false,
				parser.isSet(fullscreen_option),
				parser.isSet(zoom_option),
				parser.isSet(stretch_option));

		return RunStream(app, connect_info);
	}
#ifdef CHIAKI_ENABLE_CLI
	else if(cli_commands.contains(args[0]))
	{
		ChiakiLog log;
		// TODO: add verbose arg
		chiaki_log_init(&log, CHIAKI_LOG_ALL & ~CHIAKI_LOG_VERBOSE, chiaki_log_cb_print, nullptr);

		const auto &cmd = cli_commands[args[0]];
		int sub_argc = args.count();
		QVector<QByteArray> sub_argv_b(sub_argc);
		QVector<char *> sub_argv(sub_argc);
		for(size_t i=0; i<sub_argc; i++)
		{
			sub_argv_b[i] = args[i].toLocal8Bit();
			sub_argv[i] = sub_argv_b[i].data();
		}
		return cmd.cmd(&log, sub_argc, sub_argv.data());
	}
#endif
	else
	{
		parser.showHelp(1);
	}
}

int RunMain(QGuiApplication &app, Settings *settings, bool exit_app_on_stream_exit)
{
	QmlMainWindow main_window(settings, exit_app_on_stream_exit);
#ifdef PS5LINK_BUILD
    const int seconds = app.property("ps5linkPsnTestSeconds").toInt();
    if(seconds)
    {
        auto *backend = main_window.getBackend();
        backend->setDiscoveryEnabled(false);
        backend->stopAutoConnect();
        const bool dx12 = app.property("ps5linkDx12").toBool();
        QTimer watch;
        watch.setInterval(150);
        QElapsedTimer connection, playback;
        QElapsedTimer fpsInterval;
        QmlBackend::FrameCounts lastCounts;
        bool requested = false;
        QObject::connect(&watch, &QTimer::timeout, &main_window, [&] {
            if(!requested && !backend->psnScanInProgress())
            {
                const auto hosts = backend->hosts();
                int index = -1;
                for(int i = 0; i < hosts.size(); ++i)
                {
                    const auto host = hosts[i].toMap();
                    if(!host.value("registered").toBool() || !host.value("ps5").toBool()
                        || host.value("duid").toString().isEmpty() || host.value("discovered").toBool())
                        continue;
                    if(index != -1)
                    {
                        fprintf(stderr, "[PSN test] More than one registered PS5 matches.\n");
                        app.exit(2);
                        return;
                    }
                    index = i;
                }
                if(index != -1)
                {
                    requested = true;
                    fprintf(stdout, "[PSN test] Connecting to imported PS5 (%s).\n", dx12 ? "DX12" : "standard renderer");
                    fflush(stdout);
                    backend->connectToHost(index);
                }
            }

            auto *session = backend->qmlSession();
            const bool video = dx12 ? backend->dx12PresentedFrames() > 0 : main_window.hasVideo();
            if(session && video && !playback.isValid())
            {
                playback.start();
                fpsInterval.start();
                lastCounts = backend->frameCounts();
                fprintf(stdout, "[PSN test] First video frame shown.\n");
                fflush(stdout);
            }
            if(session && fpsInterval.isValid() && fpsInterval.elapsed() >= 1000)
            {
                const auto counts = backend->frameCounts();
                const double elapsed = fpsInterval.nsecsElapsed() / 1000000000.0;
                fprintf(stdout, "[PSN test] FPS decoded %.1f, output %.1f, generated %.1f.\n",
                    (counts.decoded - lastCounts.decoded) / elapsed,
                    (counts.submitted - lastCounts.submitted) / elapsed,
                    (counts.generated - lastCounts.generated) / elapsed);
                fflush(stdout);
                lastCounts = counts;
                fpsInterval.start();
            }
            if(session && playback.isValid() && playback.elapsed() >= seconds * 1000)
            {
                fprintf(stdout, "[PSN test] Playback %.1f s, bitrate %.2f Mbps, packet loss %.2f%%, lost frames %d, DX12 frames %u.\n",
                    playback.elapsed() / 1000.0, session->GetMeasuredBitrate(), session->GetAveragePacketLoss() * 100.0,
                    session->GetFramesLost(), backend->dx12PresentedFrames());
                watch.stop();
                const bool dlssPassed = app.property("ps5linkFrameGeneration").toInt() != 2 || backend->dx12DlssGeneratedFrames() > 0;
                QObject::connect(backend, &QmlBackend::sessionChanged, &main_window, [&app, dlssPassed](StreamSession *active) {
                    if(!active)
                        app.exit(dlssPassed ? 0 : 1);
                });
                QTimer::singleShot(5000, &main_window, [&app] { app.exit(1); });
                backend->stopSession(false);
                return;
            }
            if(!playback.isValid() && connection.elapsed() > 75000)
            {
                fprintf(stderr, "[PSN test] Timed out before completing playback (state %d).\n",
                    static_cast<int>(backend->connectState()));
                watch.stop();
                if(session)
                    backend->stopSession(false);
                app.exit(1);
            }
        });
        QObject::connect(backend, &QmlBackend::connectStateChanged, &main_window, [&] {
            fprintf(stdout, "[PSN test] Connection state %d.\n", static_cast<int>(backend->connectState()));
            fflush(stdout);
            if(backend->connectState() >= QmlBackend::PsnConnectState::ConnectFailed)
            {
                fprintf(stderr, "[PSN test] PSN connection failed (state %d).\n", static_cast<int>(backend->connectState()));
                app.exit(1);
            }
        });
        QObject::connect(backend, &QmlBackend::psnCredsExpired, &main_window, [&] {
            fprintf(stderr, "[PSN test] PSN rejected the imported credentials.\n");
            app.exit(1);
        });
        QObject::connect(backend, &QmlBackend::sessionPinDialogRequested, &main_window, [&] {
            fprintf(stderr, "[PSN test] The console requires its login PIN.\n");
            backend->stopSession(false);
            app.exit(1);
        });
        QObject::connect(backend, &QmlBackend::sessionError, &main_window, [&] {
            fprintf(stderr, "[PSN test] The remote session ended with an error.\n");
            app.exit(1);
        });
        main_window.show();
        connection.start();
        watch.start();
        return app.exec();
    }
#endif
	main_window.show();
	return app.exec();
}

int RunStream(QGuiApplication &app, const StreamSessionConnectInfo &connect_info)
{
	QmlMainWindow main_window(connect_info);
	main_window.show();
	return app.exec();
}
