using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Media;
using System.ComponentModel;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace PS5Link.Launcher;

internal sealed partial class MainWindow : Window
{
    private const int RtxVsrMedium = 3;
    private const int FsrEasu = 1;
    private static readonly string[] ScalingNames =
        ["", "AMD FSR 1.0", "RTX VSR Q1", "RTX VSR Q2", "RTX VSR Q3", "RTX VSR Q4", "DLSS 4 SR", "DLSS 4.5 SR"];
    private static readonly string[] GeneratorNames = ["", " · Интерп. ×2", " · DLSS FG", " · FSR FG"];
    private static readonly string StateFile = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PS5Link", "launcher-state.json");
    private static readonly string OpacityFile = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PS5Link", "launcher-opacity.txt");

    private JsonObject? legacyState;
    private ClientRunner.Preferences? savedPreferences;
    private ClientRunner.ProfileSettings? profileSnapshot;
    private System.Diagnostics.Process? idleMonitor;
    private (string Profile, int Minutes, bool KeepCharging)? idleMonitorPolicy;
    private ClientRunner.ConsoleEntry[] consoles = [];
    private string savedConsole = "";
    internal double ViewportHeight => double.IsFinite(Viewport.MaxHeight) ? Math.Max(370, Viewport.MaxHeight) : 760;
    private string loadedProfile = "";
    private bool closeAfterSaving;
    private bool busy;
    private bool updating;
    private readonly CancellationTokenSource windowLifetime = new();
    private (string Russian, string English) updateFeedback;
    private bool syncing;
    private bool russian;
    private (string Russian, string English) status = ("Готово к запуску", "Ready");
    private int? generatorBeforeDlss;
    private int? targetBeforeBasicScaler;
    private IPointer? dragPointer;
    private PixelPoint dragPointerStart;
    private PixelPoint dragWindowStart;

    public MainWindow()
    {
        InitializeComponent();
        VersionText.Text = $"v{ReleaseUpdate.CurrentVersion}";
        russian = LauncherLanguage.LoadRussian();
        LoadState();
        ApplyLanguage();
        ApplyOpacity();

        WindowOpacity.ValueChanged += (_, _) =>
        {
            ApplyOpacity();
            SaveState();
        };
        foreach (var toggle in new[] { Dx12, Recover, AdaptiveTriggers, Haptics, BtSpeaker, MutePcAudio, MicUnmuted })
            toggle.IsCheckedChanged += (_, _) => OnSettingChanged();
        foreach (var input in new[] { Timing, Upscale, Generator, Target, Buffer, Lightbar, Polling })
            input.SelectionChanged += (_, _) => OnSettingChanged();
        Refresh();
    }

    protected override async void OnOpened(EventArgs e)
    {
        base.OnOpened(e);
        ReleaseUpdate.CleanupStaleInstaller();
        if (Screens.ScreenFromWindow(this) is { } screen)
            Viewport.MaxHeight = Math.Max(120, screen.WorkingArea.Height / screen.Scaling - 50);
        if (!ClientRunner.IsAvailable(AppContext.BaseDirectory))
            return;

        busy = true;
        Refresh();
        try
        {
            await LoadClientSettingsAsync();
        }
        catch (Exception error) when (IsSettingsError(error))
        {
            ReportSettingsError(error);
        }
        finally
        {
            busy = false;
            Refresh();
        }
    }

    private void OnSettingChanged()
    {
        if (syncing)
            return;
        Refresh();
    }

    private void LoadState()
    {
        syncing = true;
        try
        {
            int opacity = LoadLegacyOpacity();
            bool gradient = false;
            if (File.Exists(StateFile))
            {
                try
                {
                    var state = JsonNode.Parse(File.ReadAllText(StateFile))?.AsObject();
                    if (state is not null)
                    {
                        opacity = state["Opacity"]?.GetValue<int>() ?? opacity;
                        gradient = state["GradientTheme"]?.GetValue<bool>() ?? false;
                        legacyState = state;
                    }
                }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException or FormatException) { }
            }
            else
            {
                legacyState = JsonSerializer.SerializeToNode(GetPreferences())!.AsObject();
            }
            WindowOpacity.Value = Math.Clamp(opacity, 75, 100);
            GradientTheme.IsChecked = gradient;
            ApplyGradientBrush(gradient);
        }
        finally
        {
            syncing = false;
        }
    }

    private void SaveState()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(StateFile)!);
            int opacity = Math.Clamp((int)WindowOpacity.Value, 75, 100);
            var state = legacyState ?? new JsonObject();
            state["Opacity"] = opacity;
            state["GradientTheme"] = GradientTheme.IsChecked == true;
            string pending = StateFile + $".{Environment.ProcessId}.tmp";
            File.WriteAllText(pending, state.ToJsonString());
            File.Move(pending, StateFile, overwrite: true);
            File.WriteAllText(OpacityFile, opacity.ToString(System.Globalization.CultureInfo.InvariantCulture));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            System.Diagnostics.Trace.WriteLine($"Could not save launcher state: {error.Message}");
        }
    }

    private ClientRunner.Preferences GetPreferences() => new(
        Dx12.IsChecked == true,
        Timing.SelectedIndex,
        Generator.SelectedIndex,
        Upscale.SelectedIndex,
        Target.SelectedIndex,
        Buffer.SelectedIndex,
        Recover.IsChecked == true,
        AdaptiveTriggers.IsChecked == true,
        Haptics.IsChecked == true,
        BtSpeaker.IsChecked == true,
        MutePcAudio.IsChecked == true,
        MicUnmuted.IsChecked == true,
        Lightbar.SelectedIndex,
        Polling.SelectedIndex);

    private async Task LoadClientSettingsAsync()
    {
        string folder = AppContext.BaseDirectory;
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(windowLifetime.Token);
        deadline.CancelAfter(TimeSpan.FromSeconds(10));
        var profile = await ClientRunner.ReadSettingsAsync(folder, deadline.Token);
        if (legacyState is { } legacy)
        {
            var current = JsonSerializer.SerializeToNode(profile.Values)!.AsObject();
            var migration = new JsonObject();
            foreach (string key in profile.Missing)
            {
                if (legacy[key] is JsonValue old && current[key] is JsonValue value
                    && (value.TryGetValue<bool>(out _) ? old.TryGetValue<bool>(out _) : old.TryGetValue<int>(out _)))
                {
                    int maximum = key switch
                    {
                        "Upscale" => 7,
                        "Generator" or "Target" or "Lightbar" or "Polling" => 3,
                        _ => 2
                    };
                    migration[key] = old.TryGetValue<int>(out int number) ? JsonValue.Create(Math.Clamp(number, 0, maximum)) : old.DeepClone();
                }
            }
            if (migration.Count > 0)
            {
                await ClientRunner.WriteSettingsAsync(folder, profile.Profile, migration, deadline.Token);
                profile = await ClientRunner.ReadSettingsAsync(folder, deadline.Token);
            }
            legacyState = null;
            SaveState();
        }

        var prefs = profile.Values;
        profileSnapshot = profile;
        consoles = profile.Consoles;
        savedConsole = consoles.Any(console => console.Id == profile.PreferredConsole) ? profile.PreferredConsole : "";
        syncing = true;
        try
        {
            generatorBeforeDlss = null;
            targetBeforeBasicScaler = null;
            RefreshConsolePicker(savedConsole);
            Dx12.IsChecked = prefs.Dx12;
            Timing.SelectedIndex = prefs.Timing;
            Generator.SelectedIndex = prefs.Generator;
            Upscale.SelectedIndex = prefs.Upscale;
            Target.SelectedIndex = prefs.Target;
            Buffer.SelectedIndex = prefs.Buffer;
            Recover.IsChecked = prefs.Recover;
            AdaptiveTriggers.IsChecked = prefs.AdaptiveTriggers;
            Haptics.IsChecked = prefs.Haptics;
            BtSpeaker.IsChecked = prefs.BtSpeaker;
            MutePcAudio.IsChecked = prefs.MutePcAudio;
            MicUnmuted.IsChecked = prefs.MicUnmuted;
            Lightbar.SelectedIndex = prefs.Lightbar;
            Polling.SelectedIndex = prefs.Polling;
        }
        finally
        {
            syncing = false;
        }
        Refresh();
        savedPreferences = GetPreferences();
        loadedProfile = profile.Profile;
        RefreshIdleMonitor();
    }

    private void RefreshIdleMonitor()
    {
        if (windowLifetime.IsCancellationRequested)
            return;
        var profile = profileSnapshot;
        int minutes = profile?.Advanced["GamepadIdleEnabled"]?.GetValue<bool>() == true
            ? profile.Advanced["GamepadIdleDesktop"]?.GetValue<int>() ?? 0 : 0;
        if (minutes == 0)
        {
            StopIdleMonitor();
            return;
        }

        var policy = (profile!.Profile, minutes, profile.Advanced["GamepadIdleKeepCharging"]?.GetValue<bool>() == true);
        if (idleMonitor is { HasExited: false } && idleMonitorPolicy == policy)
            return;
        StopIdleMonitor();
        try
        {
            idleMonitor = ClientRunner.RunIdleMonitor(AppContext.BaseDirectory);
            idleMonitorPolicy = policy;
        }
        catch (Exception error) when (error is Win32Exception or InvalidOperationException)
        {
            System.Diagnostics.Trace.WriteLine(error.Message);
        }
    }

    private int? GetIdleMonitorProcessId()
    {
        try
        {
            return idleMonitor is { HasExited: false } process ? process.Id : null;
        }
        catch (Exception error) when (error is InvalidOperationException or Win32Exception)
        {
            return null;
        }
    }

    private void StopIdleMonitor()
    {
        if (idleMonitor is not { } process)
            return;
        try
        {
            if (!process.HasExited)
            {
                process.Kill();
                process.WaitForExit(1500);
            }
        }
        catch (Exception error) when (error is InvalidOperationException or Win32Exception)
        {
            // The monitor can exit between checking its state and stopping it.
        }
        finally
        {
            process.Dispose();
            idleMonitor = null;
            idleMonitorPolicy = null;
        }
    }

    private string SelectedConsole => ConsolePicker.SelectedIndex is > 0 && ConsolePicker.SelectedIndex <= consoles.Length
        ? consoles[ConsolePicker.SelectedIndex - 1].Id : "";

    private void RefreshConsolePicker(string selection)
    {
        var labels = consoles.Select(console =>
        {
            string kind = console.Ps5 ? "PS5" : "PS4";
            string caption = string.IsNullOrWhiteSpace(console.Name) ? kind : $"{console.Name} ({kind})";
            return consoles.Count(other => other.Name == console.Name && other.Ps5 == console.Ps5) > 1
                ? $"{caption} · {console.Id[^4..]}" : caption;
        });
        string prompt = consoles.Length > 1 ? Localize("Выбрать консоль…", "Select a console…")
            : Localize("Автовыбор", "Automatic");
        ConsolePicker.ItemsSource = labels.Prepend(prompt).ToArray();
        ConsolePicker.SelectedIndex = Array.FindIndex(consoles, console => console.Id == selection) + 1;
        ConsoleRow.IsVisible = consoles.Length > 0;
    }

    private async Task SaveClientSettingsAsync()
    {
        var preferences = GetPreferences();
        string selectedConsole = SelectedConsole;
        if (savedPreferences is null || (preferences == savedPreferences && selectedConsole == savedConsole))
            return;

        var previous = JsonSerializer.SerializeToNode(savedPreferences)!.AsObject();
        var current = JsonSerializer.SerializeToNode(preferences)!.AsObject();
        var patch = new JsonObject();
        foreach (var field in current)
            if (!JsonNode.DeepEquals(previous[field.Key], field.Value))
                patch[field.Key] = field.Value?.DeepClone();

        if (preferences.Dx12 != savedPreferences.Dx12)
            patch["Upscale"] = preferences.Upscale;

        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(windowLifetime.Token);
        deadline.CancelAfter(TimeSpan.FromSeconds(10));
        await ClientRunner.WriteSettingsAsync(AppContext.BaseDirectory, loadedProfile, patch, deadline.Token,
            preferredConsole: selectedConsole == savedConsole ? null : selectedConsole);
        savedPreferences = preferences;
        savedConsole = selectedConsole;
    }

    private static bool IsSettingsError(Exception error) =>
        error is Win32Exception or InvalidOperationException or IOException or JsonException or OperationCanceledException;

    private void ReportSettingsError(Exception error)
    {
        if (windowLifetime.IsCancellationRequested)
            return;
        System.Diagnostics.Trace.WriteLine(error.Message);
        SetStatus("Не удалось синхронизировать настройки PS5Link", "Could not sync PS5Link settings", 0xFF453A);
    }

    private static int LoadLegacyOpacity()
    {
        try
        {
            return int.TryParse(File.ReadAllText(OpacityFile), out int value) ? Math.Clamp(value, 75, 100) : 94;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return 94;
        }
    }

    private void ApplyOpacity()
    {
        Opacity = WindowOpacity.Value / 100;
        OpacityLabel.Text = $"{WindowOpacity.Value:0}%";
    }

    private string Localize(string ru, string en) => russian ? ru : en;

    private void ApplyLanguage()
    {
        foreach (var entry in LauncherLanguage.Strings)
            Resources[entry.Key] = Localize(entry.Russian, entry.English);
        // Avalonia copies the selected item's content into the closed combo box.
        // Recreate that copy after the localized resource values have changed.
        syncing = true;
        try
        {
            foreach (var combo in new[] { Timing, Generator, Upscale, Target, Buffer, Lightbar, Polling })
            {
                int selected = combo.SelectedIndex;
                combo.SelectedIndex = -1;
                combo.SelectedIndex = selected;
            }
        }
        finally
        {
            syncing = false;
        }
        LanguageButton.Content = russian ? "RU" : "EN";
        StatusText.Text = Localize(status.Russian, status.English);
        ToolTip.SetTip(StatusText, StatusText.Text);
        if (UpdateFeedback.IsVisible)
            UpdateFeedback.Text = Localize(updateFeedback.Russian, updateFeedback.English);
        if (profileSnapshot is not null)
            RefreshConsolePicker(SelectedConsole);
    }

    private void ToggleLanguage(object? sender, RoutedEventArgs e)
    {
        try
        {
            LauncherLanguage.Save(!russian);
            russian = !russian;
            ApplyLanguage();
            Refresh();
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            SetStatus("Не удалось сохранить язык", "Could not save language", 0xFF453A);
        }
    }

    protected override async void OnClosing(WindowClosingEventArgs e)
    {
        base.OnClosing(e);
        if (e.Cancel || closeAfterSaving || savedPreferences is null
            || e.CloseReason is WindowCloseReason.OSShutdown or WindowCloseReason.ApplicationShutdown
            || (savedPreferences == GetPreferences() && savedConsole == SelectedConsole))
            return;

        e.Cancel = true;
        if (busy || updating)
            return;

        busy = true;
        Refresh();
        try
        {
            await SaveClientSettingsAsync();
            closeAfterSaving = true;
            Close();
        }
        catch (Exception error) when (IsSettingsError(error))
        {
            ReportSettingsError(error);
            // Allow the next close attempt instead of trapping the window forever.
            closeAfterSaving = true;
        }
        finally
        {
            busy = false;
            if (!closeAfterSaving)
                Refresh();
        }
    }

    protected override void OnClosed(EventArgs e)
    {
        windowLifetime.Cancel();
        StopIdleMonitor();
        SaveState();
        base.OnClosed(e);
    }

    private void Refresh()
    {
        if (syncing)
            return;

        syncing = true;
        try
        {
            bool dx12 = Dx12.IsChecked == true;
            if (!dx12 && (Upscale.SelectedIndex == 1 || Upscale.SelectedIndex >= 6))
                Upscale.SelectedIndex = 0;

            bool useDlss = dx12 && Upscale.SelectedIndex >= 6;
            if (useDlss && generatorBeforeDlss == null)
            {
                generatorBeforeDlss = Generator.SelectedIndex;
                Generator.SelectedIndex = 0;
            }
            else if (!useDlss && generatorBeforeDlss is { } previous)
            {
                Generator.SelectedIndex = previous;
                generatorBeforeDlss = null;
            }

            bool canSelectTarget = dx12 && Upscale.SelectedIndex >= 2;
            if (!canSelectTarget)
            {
                if (Target.SelectedIndex != 0)
                {
                    targetBeforeBasicScaler = Target.SelectedIndex;
                    Target.SelectedIndex = 0;
                }
            }
            else if (targetBeforeBasicScaler is { } prevTarget)
            {
                if (prevTarget != 1 || useDlss)
                    Target.SelectedIndex = prevTarget;
                targetBeforeBasicScaler = null;
            }

            if (!useDlss && Target.SelectedIndex == 1)
                Target.SelectedIndex = 0;

            Start.IsEnabled = Advanced.IsEnabled = UpdateButton.IsEnabled = !busy && !updating;
            bool editable = !busy && !updating && savedPreferences is not null;
            Dx12.IsEnabled = Upscale.IsEnabled = ConsolePicker.IsEnabled = editable;
            RtxPreset.IsEnabled = StandardPreset.IsEnabled = editable;
            Timing.IsEnabled = editable && dx12;
            Generator.IsEnabled = editable && dx12 && !useDlss;
            Target.IsEnabled = editable && canSelectTarget;
            Buffer.IsEnabled = Recover.IsEnabled = editable;
            AdaptiveTriggers.IsEnabled = Haptics.IsEnabled = BtSpeaker.IsEnabled = MutePcAudio.IsEnabled = MicUnmuted.IsEnabled = editable;
            Lightbar.IsEnabled = Polling.IsEnabled = editable;

            RendererText.Text = dx12 ? "Direct3D 12"
                : (profileSnapshot?.Advanced["Renderer"]?.GetValue<int>() == 1 ? "OpenGL" : "Vulkan");
            string scaling = Upscale.SelectedIndex <= 0 ? Localize("Шейдер", "Shader") : ScalingNames[Upscale.SelectedIndex];
            string targetBadge = canSelectTarget
                ? Target.SelectedIndex switch
                {
                    1 when useDlss => " · DLAA",
                    2 => " · 1800p",
                    3 => " · 4K",
                    _ => ""
                }
                : "";
            string generator = Generator.SelectedIndex == 1 && !russian
                ? " · Interp. ×2" : GeneratorNames[Math.Max(0, Generator.SelectedIndex)];
            ScalingText.Text = scaling + targetBadge + (dx12 && !useDlss ? generator : "");

            bool presetSettings = dx12 && Timing.SelectedIndex == 0 &&
                Generator.SelectedIndex == 0 && Target.SelectedIndex == 0;
            RtxPreset.Classes.Set("active", presetSettings && Upscale.SelectedIndex == RtxVsrMedium);
            StandardPreset.Classes.Set("active", presetSettings && Upscale.SelectedIndex == FsrEasu);
        }
        finally
        {
            syncing = false;
        }
    }

    private void SelectRtxPreset(object? sender, RoutedEventArgs e) => ApplyPreset(RtxVsrMedium);
    private void SelectStandardPreset(object? sender, RoutedEventArgs e) => ApplyPreset(FsrEasu);

    private void ApplyPreset(int upscale)
    {
        if (busy || updating)
            return;

        syncing = true;
        try
        {
            generatorBeforeDlss = null;
            targetBeforeBasicScaler = null;
            Dx12.IsChecked = true;
            Timing.SelectedIndex = 0;
            Upscale.SelectedIndex = upscale;
            Generator.SelectedIndex = 0;
            Target.SelectedIndex = 0;
        }
        finally
        {
            syncing = false;
        }
        Refresh();
    }

    private async void StartClient(object? sender, RoutedEventArgs e)
    {
        if (profileSnapshot is not null && consoles.Length == 0)
        {
            await OpenAdvancedAsync(consoleSetup: true);
            return;
        }
        if (consoles.Length > 1 && SelectedConsole.Length == 0)
        {
            SetStatus("Выбери консоль для подключения", "Select a console to connect", 0xFF9F0A);
            ConsolePicker.Focus();
            return;
        }
        await RunClientAsync();
    }

    private async void OpenAdvanced(object? sender, RoutedEventArgs e) => await OpenAdvancedAsync(consoleSetup: false);

    private async Task OpenAdvancedAsync(bool consoleSetup)
    {
        if (busy || updating)
            return;
        if (!ClientRunner.IsAvailable(AppContext.BaseDirectory))
        {
            SetStatus("Запусти из папки PS5Link-Win", "Run from the PS5Link-Win folder", 0xFF453A);
            return;
        }

        busy = true;
        Refresh();
        try
        {
            if (profileSnapshot is null)
                await LoadClientSettingsAsync();
            await SaveClientSettingsAsync();
            await new AdvancedWindow(this, profileSnapshot!, russian, consoleSetup).ShowDialog(this);
            await LoadClientSettingsAsync();
        }
        catch (Exception error) when (IsSettingsError(error))
        {
            ReportSettingsError(error);
        }
        finally
        {
            busy = false;
            Refresh();
        }

    }

    private async Task RunClientAsync()
    {
        if (busy || updating)
            return;

        SaveState();
        var folder = AppContext.BaseDirectory;
        if (!ClientRunner.IsAvailable(folder))
        {
            SetStatus("Запусти из папки PS5Link-Win", "Run from the PS5Link-Win folder", 0xFF453A);
            return;
        }

        busy = true;
        SetStatus("Подключение к консоли", "Connecting to console", 0xFF9F0A);
        Refresh();
        try
        {
            if (savedPreferences is null)
            {
                await LoadClientSettingsAsync();
                if (consoles.Length == 0)
                {
                    busy = false;
                    Refresh();
                    await OpenAdvancedAsync(consoleSetup: true);
                    return;
                }
                if (consoles.Length > 1 && SelectedConsole.Length == 0)
                {
                    SetStatus("Выбери консоль для подключения", "Select a console to connect", 0xFF9F0A);
                    ConsolePicker.Focus();
                    return;
                }
            }
            await SaveClientSettingsAsync();
            StopIdleMonitor();
            int exitCode = await ClientRunner.RunAsync(folder);
            if (!windowLifetime.IsCancellationRequested)
                await LoadClientSettingsAsync();
            if (exitCode == 0)
                SetStatus("Стрим завершён", "Stream ended", 0x30D158);
            else if (exitCode == 7)
                SetStatus("Консоль не найдена. Проверь сеть или вход в PSN.", "Console unavailable. Check the network or PSN sign-in.", 0xFF453A);
            else
                SetStatus($"Ошибка запуска ({exitCode})", $"Launch error ({exitCode})", 0xFF453A);
        }
        catch (Exception error) when (IsSettingsError(error))
        {
            ReportSettingsError(error);
        }
        finally
        {
            busy = false;
            RefreshIdleMonitor();
            Refresh();
        }
    }

    private async void UpdateApplication(object? sender, RoutedEventArgs e)
    {
        if (busy || updating)
            return;

        updating = true;
        bool installerStarted = false;
        Refresh();
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(windowLifetime.Token);
        deadline.CancelAfter(TimeSpan.FromMinutes(6));
        try
        {
            string? directory = ReleaseUpdate.GetInstallDirectory();
            if (directory is null)
            {
                ShowUpdateFeedback("Обновление доступно в версии, установленной через Setup.",
                    "Updates are available from the copy installed by Setup.");
                return;
            }
            if (ReleaseUpdate.OtherInstanceIsRunning(directory, GetIdleMonitorProcessId()))
            {
                ShowUpdateFeedback("Закрой другие окна PS5Link и попробуй снова.",
                    "Close other PS5Link windows and try again.");
                return;
            }

            ShowUpdateFeedback("Проверяем релизы GitHub…", "Checking GitHub releases…");
            var release = await ReleaseUpdate.CheckAsync(deadline.Token);
            if (release is null)
            {
                ShowUpdateFeedback("Установлена последняя версия.", "You have the latest version.");
                return;
            }

            ShowUpdateFeedback($"Скачиваем версию {release.Version}…", $"Downloading version {release.Version}…");
            string installer = await ReleaseUpdate.DownloadAsync(release, deadline.Token);
            deadline.Token.ThrowIfCancellationRequested();
            if (savedPreferences is not null
                && (savedPreferences != GetPreferences() || savedConsole != SelectedConsole))
            {
                await SaveClientSettingsAsync();
            }
            StopIdleMonitor();
            if (ReleaseUpdate.OtherInstanceIsRunning(directory))
            {
                ShowUpdateFeedback("Закрой другие окна PS5Link и попробуй снова.",
                    "Close other PS5Link windows and try again.");
                return;
            }

            ReleaseUpdate.StartInstaller(installer, directory, russian);
            installerStarted = true;
            closeAfterSaving = true;
            Close();
        }
        catch (OperationCanceledException) when (windowLifetime.IsCancellationRequested) { }
        catch (Exception error) when (error is HttpRequestException or IOException or InvalidDataException or
            JsonException or FormatException or InvalidOperationException or ArgumentException or
            UnauthorizedAccessException or Win32Exception or OperationCanceledException or KeyNotFoundException)
        {
            System.Diagnostics.Trace.WriteLine($"PS5Link update: {error.Message}");
            ShowUpdateFeedback("Не удалось скачать или установить обновление.",
                "Could not download or start the update.");
        }
        finally
        {
            updating = false;
            if (!installerStarted && !windowLifetime.IsCancellationRequested)
            {
                RefreshIdleMonitor();
                Refresh();
            }
        }
    }

    private void ShowUpdateFeedback(string ru, string en)
    {
        updateFeedback = (ru, en);
        UpdateFeedback.Text = Localize(ru, en);
        UpdateFeedback.IsVisible = true;
    }

    private void SetStatus(string ru, string en, uint rgb)
    {
        status = (ru, en);
        StatusText.Text = Localize(ru, en);
        StatusDot.Fill = new SolidColorBrush(Color.FromUInt32(0xFF000000 | rgb));
        ToolTip.SetTip(StatusText, StatusText.Text);
    }

    private void DragTitle(object? sender, PointerPressedEventArgs e)
    {
        if (e.GetCurrentPoint(this).Properties.PointerUpdateKind != PointerUpdateKind.LeftButtonPressed)
            return;

        dragPointerStart = this.PointToScreen(e.GetPosition(this));
        dragWindowStart = Position;
        dragPointer = e.Pointer;
        e.Pointer.Capture(this);
        e.Handled = true;
    }

    protected override void OnPointerMoved(PointerEventArgs e)
    {
        base.OnPointerMoved(e);
        if (e.Pointer != dragPointer)
            return;

        var pointer = this.PointToScreen(e.GetPosition(this));
        Position = new PixelPoint(dragWindowStart.X + pointer.X - dragPointerStart.X,
            dragWindowStart.Y + pointer.Y - dragPointerStart.Y);
        e.Handled = true;
    }

    protected override void OnPointerReleased(PointerReleasedEventArgs e)
    {
        base.OnPointerReleased(e);
        if (e.Pointer != dragPointer || e.GetCurrentPoint(this).Properties.IsLeftButtonPressed)
            return;

        dragPointer = null;
        e.Pointer.Capture(null);
        e.Handled = true;
    }

    protected override void OnPointerCaptureLost(PointerCaptureLostEventArgs e)
    {
        base.OnPointerCaptureLost(e);
        if (e.Pointer == dragPointer)
            dragPointer = null;
    }

    private void MinimizeWindow(object? sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
    private void CloseWindow(object? sender, RoutedEventArgs e) => Close();

    private void ApplyGradientBrush(bool enabled)
    {
        Resources["ShellBrush"] = enabled
            ? new LinearGradientBrush
            {
                StartPoint = new RelativePoint(0, 0, RelativeUnit.Relative),
                EndPoint = new RelativePoint(1, 1, RelativeUnit.Relative),
                GradientStops =
                [
                    new GradientStop(Color.Parse("#23454C"), 0),
                    new GradientStop(Color.Parse("#293751"), 0.52),
                    new GradientStop(Color.Parse("#503750"), 1)
                ]
            }
            : new SolidColorBrush(Color.Parse("#18181B"));
    }

    private void ChangeTheme(object? sender, RoutedEventArgs e)
    {
        ApplyGradientBrush(sender is ToggleSwitch { IsChecked: true });
        if (!syncing)
            SaveState();
    }
}
