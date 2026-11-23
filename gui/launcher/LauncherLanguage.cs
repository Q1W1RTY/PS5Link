namespace PS5Link.Launcher;

internal static class LauncherLanguage
{
    internal static readonly (string Key, string Russian, string English)[] Strings =
    [
        ("LangGradientTheme", "Градиентная тема", "Gradient theme"),
        ("LangConnect", "Подключиться", "Connect"),
        ("LangAdvancedSettings", "Дополнительно", "Advanced settings"),
        ("LangAdvancedSettingsTip", "Настройки клиента и подключение консоли", "Client settings and console setup"),
        ("LangVideoCategory", "Видео", "Video"),
        ("LangNetworkCategory", "Сеть", "Network"),
        ("LangAppearanceCategory", "Внешний вид", "Appearance"),
        ("LangRtxPreset", "С RTX", "With RTX"),
        ("LangStandardPreset", "Без RTX", "Without RTX"),
        ("LangRtxPresetTip", "RTX VSR Medium · баланс 60 FPS", "RTX VSR Medium · balanced 60 FPS"),
        ("LangStandardPresetTip", "AMD FSR 1.0 · баланс 60 FPS", "AMD FSR 1.0 · balanced 60 FPS"),
        ("LangMinimize", "Свернуть", "Minimize"),
        ("LangClose", "Закрыть", "Close"),
        ("LangTiming", "Синхронизация", "Output timing"),
        ("LangBalanced", "Баланс 60 FPS", "Balanced 60 FPS"),
        ("LangLowLatency", "Низкая задержка", "Low latency"),
        ("LangFrameGeneration", "Генерация кадров", "Frame generation"),
        ("LangOff", "Выкл.", "Off"),
        ("LangInterpolation", "Интерполяция ×2", "Interpolation ×2"),
        ("LangUpscaling", "Масштабирование", "Upscaling"),
        ("LangDefault", "По умолчанию", "Default"),
        ("LangResolution", "Разрешение", "Resolution"),
        ("LangOutputSize", "Размер экрана", "Output size"),
        ("LangStreamSize", "Размер потока (DLAA)", "Stream size (DLAA)"),
        ("LangTarget1800", "1800p → размер экрана", "1800p → output size"),
        ("LangTarget4K", "4K → размер экрана", "4K → output size"),
        ("LangPacketBuffer", "Буфер пакетов", "Packet buffer"),
        ("LangBuffer0", "0 мс", "0 ms"),
        ("LangBuffer8", "8 мс", "8 ms"),
        ("LangBuffer16", "16 мс", "16 ms"),
        ("LangIdr", "Запрос IDR", "Request IDR"),
        ("LangKeyframe", "Запрос ключевого кадра", "Request keyframe"),
        ("LangGamepadHeader", "DualSense и геймпад", "DualSense & Gamepad"),
        ("LangAdaptiveTriggers", "Адаптивные триггеры", "Adaptive triggers"),
        ("LangHaptics", "Хаптики 3000 Гц и вибрация", "3000 Hz haptics & rumble"),
        ("LangBtSpeaker", "Динамик геймпада (BT)", "Controller speaker (BT)"),
        ("LangMicUnmuted", "Микрофон со старта", "Start with mic on"),
        ("LangLightbar", "Подсветка LED", "LED lightbar"),
        ("LangLightbarPad", "На геймпаде", "Controller only"),
        ("LangLightbarBoth", "Геймпад и экран", "Controller & screen"),
        ("LangLightbarScreen", "Полоса на экране", "On-screen bar"),
        ("LangLightbarOff", "Выкл.", "Off"),
        ("LangPolling", "Опрос геймпада", "Polling rate"),
        ("LangPoll1", "1000 Гц (1 мс)", "1000 Hz (1 ms)"),
        ("LangPoll2", "500 Гц (2 мс)", "500 Hz (2 ms)"),
        ("LangPoll4", "250 Гц (4 мс)", "250 Hz (4 ms)"),
        ("LangPoll8", "125 Гц (8 мс)", "125 Hz (8 ms)"),
        ("LangOpacity", "Непрозрачность окна", "Window opacity"),
        ("LangOverlayHint", "Ctrl+O открывает меню в игре", "Ctrl+O opens the in-game menu"),
        ("LangLanguage", "Язык интерфейса", "Interface language"),
        ("LangTimingHelp",
            "Управляет выводом кадров в Direct3D 12.\nБаланс 60 FPS — ровная подача до 60 кадров/с.\nНизкая задержка — берёт самый свежий кадр; возможны разрывы изображения.\nVSync — синхронизирует вывод с экраном без отдельного лимита 60 FPS.",
            "Controls frame presentation in Direct3D 12.\nBalanced 60 FPS paces output at up to 60 FPS.\nLow latency uses the newest frame and can cause tearing.\nVSync synchronizes output to your display without a separate 60 FPS cap."),
        ("LangFrameGenerationHelp",
            "Добавляет промежуточные кадры для плавности. Ввод по-прежнему обрабатывает сама игра.\nИнтерполяция, DLSS FG и FSR FG работают в Direct3D 12 на экране от 120 Гц и могут повышать нагрузку на GPU. DLSS Super Resolution блокирует генерацию.",
            "Adds intermediate frames for smoother motion. The game still handles input at its own rate.\nInterpolation, DLSS FG, and FSR FG use Direct3D 12 and a 120 Hz or faster display. They can increase GPU load. DLSS Super Resolution disables frame generation."),
        ("LangUpscalingHelp",
            "Увеличивает разрешение изображения из потока.\nПо умолчанию — масштабирование рендерера. AMD FSR 1.0 — лёгкий апскейл Direct3D 12 для карт разных производителей. RTX VSR требует NVIDIA RTX и выход больше потока. DLSS SR требует совместимого RTX и Direct3D 12.",
            "Scales the incoming video to a higher resolution.\nDefault uses the renderer's scaler. AMD FSR 1.0 is a lightweight Direct3D 12 scaler for different GPU vendors. RTX VSR needs an NVIDIA RTX GPU and an output larger than the stream. DLSS SR needs a compatible RTX GPU and Direct3D 12."),
        ("LangResolutionHelp",
            "Задаёт размер кадра для RTX VSR или DLSS SR в Direct3D 12.\nРазмер экрана — прямой вывод. 1800p и 4K — промежуточный этап, затем подгонка под экран. Чем выше разрешение, тем выше нагрузка на GPU. Размер потока (DLAA) доступен только с DLSS SR.",
            "Sets the processing resolution for RTX VSR or DLSS SR in Direct3D 12.\nOutput size scales directly. At 1800p or 4K, the client scales to that intermediate size first, then fits it to the screen. Higher resolutions cost more GPU time. Stream size (DLAA) requires DLSS SR."),
        ("LangPacketBufferHelp",
            "Даёт опоздавшим сетевым пакетам время, чтобы успеть в текущий кадр.\n0 мс — без дополнительного ожидания. 8 и 16 мс помогают при перестановке пакетов, но могут добавить задержку на повреждённых кадрах.",
            "Gives late network packets a chance to complete the current frame.\n0 ms adds no wait. 8 and 16 ms can help with out-of-order packets but may add latency on damaged frames."),
        ("LangIdrHelp",
            "При невосстановимой потере видео просит PS5 прислать новый ключевой кадр. Это помогает быстрее очистить артефакты после сетевого сбоя.",
            "Asks the PS5 for a fresh keyframe when lost video data cannot be recovered. This can clear visual artifacts sooner after a network interruption."),
        ("LangDx12Help",
            "Использует отдельный рендерер Direct3D 12 с потоком H.265. В этом режиме доступны дополнительные варианты синхронизации, масштабирования и генерации кадров. При выключении используется стандартный рендерер клиента.",
            "Uses the Direct3D 12 renderer with an H.265 stream. This mode offers extra display timing, upscaling, and frame generation options. Turn it off to use the client's standard renderer."),
        ("LangAdaptiveTriggersHelp",
            "Передаёт эффекты сопротивления, отдачи и пульсации курков L2/R2 от PS5 на DualSense по Bluetooth и USB без стороннего софта.",
            "Sends PS5 L2/R2 adaptive trigger resistance and feedback effects directly to DualSense over Bluetooth and USB without extra software."),
        ("LangHapticsHelp",
            "Включает настоящую звуковую 3000 Гц хаптик-вибрацию катушек DualSense (по Bluetooth и USB) и эмуляцию моторной вибрации в играх PS4.",
            "Enables true 3000 Hz voice-coil haptic feedback on DualSense (over Bluetooth and USB) as well as legacy rumble emulation."),
        ("LangBtSpeakerHelp",
            "Потоковая передача звука игры по Bluetooth на встроенный динамик DualSense (Opus 160 кбит/с). При подключении проводных наушников в 3.5 мм разъём геймпада звук на них подаётся автоматически.",
            "Streams game audio over Bluetooth to the built-in DualSense speaker (Opus 160 kbps). When 3.5mm headphones are plugged into the controller jack, audio routes to them automatically."),
        ("LangMicUnmutedHelp",
            "Автоматически включает микрофон (встроенный в DualSense по Bluetooth/USB или системный) сразу при подключении к PS5. Кнопка Mute под кнопкой PS переключает микрофон в любой момент.",
            "Automatically unmutes the microphone (DualSense built-in Bluetooth/USB mic or system mic) when connecting to PS5. The hardware Mute button under the PS button toggles mute anytime."),
        ("LangLightbarHelp",
            "Управляет RGB-световой панелью DualSense (по Bluetooth и USB) и индикаторной полосой сверху экрана.",
            "Controls the DualSense RGB lightbar (over Bluetooth and USB) and the on-screen lightbar indicator."),
        ("LangPollingHelp",
            "Частота опроса ввода и гироскопа геймпада. 1000 Гц (1 мс) обеспечивает минимальную задержку отклика.",
            "Controller input and gyroscope polling frequency. 1000 Hz (1 ms) provides the lowest input latency."),
        ("LangOpacityHelp",
            "Управляет прозрачностью всего окна лаунчера. При 75% фон за окном заметнее, при 100% окно непрозрачное. Значение сохраняется после закрытия.",
            "Controls how much of the background shows through the launcher window. At 75% more of it is visible; at 100% the window is opaque. The setting is saved when you close the launcher."),
        ("LangGradientHelp",
            "Включает цветной градиент в фоне лаунчера. Параметры Remote Play от этого не меняются.",
            "Applies a color gradient to the launcher background. This is a visual setting for the launcher."),
    ];

    private static readonly string PreferenceFile = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PS5Link", "ui-language.txt");

    internal static bool LoadRussian()
    {
        try
        {
            string preference = File.ReadAllText(PreferenceFile).Trim();
            if (preference == "ru" || preference == "en")
                return preference == "ru";
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }

        return System.Globalization.CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "ru";
    }

    internal static void Save(bool russian)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(PreferenceFile)!);
        string pending = PreferenceFile + $".{Environment.ProcessId}.tmp";
        File.WriteAllText(pending, russian ? "ru" : "en");
        File.Move(pending, PreferenceFile, overwrite: true);
    }
}
