using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Media;
using System.ComponentModel;
using System.Text.Json;

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

    private sealed record LauncherState(
        bool Dx12 = true,
        int Timing = 0,
        int Generator = 0,
        int Upscale = RtxVsrMedium,
        int Target = 0,
        int Buffer = 1,
        bool Recover = true,
        bool AdaptiveTriggers = true,
        bool Haptics = true,
        bool BtSpeaker = false,
        bool MicUnmuted = false,
        int Lightbar = 0,
        int Polling = 2,
        int Opacity = 94,
        bool GradientTheme = false);

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
        foreach (var toggle in new[] { Dx12, Recover, AdaptiveTriggers, Haptics, BtSpeaker, MicUnmuted })
            toggle.IsCheckedChanged += (_, _) => OnSettingChanged();
        foreach (var input in new[] { Timing, Upscale, Generator, Target, Buffer, Lightbar, Polling })
            input.SelectionChanged += (_, _) => OnSettingChanged();
        Refresh();
    }

    protected override void OnOpened(EventArgs e)
    {
        base.OnOpened(e);
        if (Screens.ScreenFromWindow(this) is { } screen)
            Viewport.MaxHeight = Math.Max(120, screen.WorkingArea.Height / screen.Scaling - 50);
    }

    private void OnSettingChanged()
    {
        if (syncing)
            return;
        Refresh();
        SaveState();
    }

    private void LoadState()
    {
        syncing = true;
        try
        {
            int fallbackOpacity = LoadLegacyOpacity();
            LauncherState state = new(Opacity: fallbackOpacity);
            if (File.Exists(StateFile))
            {
                try
                {
                    if (JsonSerializer.Deserialize<LauncherState>(File.ReadAllText(StateFile)) is { } loaded)
                        state = loaded;
                }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { }
            }

            Dx12.IsChecked = state.Dx12;
            Timing.SelectedIndex = Math.Clamp(state.Timing, 0, 2);
            Generator.SelectedIndex = Math.Clamp(state.Generator, 0, 3);
            Upscale.SelectedIndex = Math.Clamp(state.Upscale, 0, 7);
            Target.SelectedIndex = Math.Clamp(state.Target, 0, 3);
            Buffer.SelectedIndex = Math.Clamp(state.Buffer, 0, 2);
            Recover.IsChecked = state.Recover;
            AdaptiveTriggers.IsChecked = state.AdaptiveTriggers;
            Haptics.IsChecked = state.Haptics;
            BtSpeaker.IsChecked = state.BtSpeaker;
            MicUnmuted.IsChecked = state.MicUnmuted;
            Lightbar.SelectedIndex = Math.Clamp(state.Lightbar, 0, 3);
            Polling.SelectedIndex = Math.Clamp(state.Polling, 0, 3);
            WindowOpacity.Value = Math.Clamp(state.Opacity, 75, 100);
            GradientTheme.IsChecked = state.GradientTheme;
            ApplyGradientBrush(state.GradientTheme);
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
            var state = new LauncherState(
                Dx12: Dx12.IsChecked == true,
                Timing: Math.Clamp(Timing.SelectedIndex, 0, 2),
                Generator: Math.Clamp(Generator.SelectedIndex, 0, 3),
                Upscale: Math.Clamp(Upscale.SelectedIndex, 0, 7),
                Target: Math.Clamp(Target.SelectedIndex, 0, 3),
                Buffer: Math.Clamp(Buffer.SelectedIndex, 0, 2),
                Recover: Recover.IsChecked == true,
                AdaptiveTriggers: AdaptiveTriggers.IsChecked == true,
                Haptics: Haptics.IsChecked == true,
                BtSpeaker: BtSpeaker.IsChecked == true,
                MicUnmuted: MicUnmuted.IsChecked == true,
                Lightbar: Math.Clamp(Lightbar.SelectedIndex, 0, 3),
                Polling: Math.Clamp(Polling.SelectedIndex, 0, 3),
                Opacity: opacity,
                GradientTheme: GradientTheme.IsChecked == true);
            string pending = StateFile + $".{Environment.ProcessId}.tmp";
            File.WriteAllText(pending, JsonSerializer.Serialize(state));
            File.Move(pending, StateFile, overwrite: true);
            File.WriteAllText(OpacityFile, opacity.ToString(System.Globalization.CultureInfo.InvariantCulture));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            System.Diagnostics.Trace.WriteLine($"Could not save launcher state: {error.Message}");
        }
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

    protected override void OnClosed(EventArgs e)
    {
        windowLifetime.Cancel();
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
            Dx12.IsEnabled = Upscale.IsEnabled = !busy;
            RtxPreset.IsEnabled = StandardPreset.IsEnabled = !busy;
            Timing.IsEnabled = !busy && dx12;
            Generator.IsEnabled = !busy && dx12 && !useDlss;
            Target.IsEnabled = !busy && canSelectTarget;
            Buffer.IsEnabled = Recover.IsEnabled = !busy;
            AdaptiveTriggers.IsEnabled = Haptics.IsEnabled = BtSpeaker.IsEnabled = MicUnmuted.IsEnabled = !busy;
            Lightbar.IsEnabled = Polling.IsEnabled = !busy;

            RendererText.Text = dx12 ? "Direct3D 12" : "Vulkan";
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
        if (busy)
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
        SaveState();
    }

    private async void StartClient(object? sender, RoutedEventArgs e) => await RunClientAsync(false);

    private async void OpenAdvanced(object? sender, RoutedEventArgs e) => await RunClientAsync(true);

    private async Task RunClientAsync(bool advancedSettings)
    {
        if (busy || updating)
            return;

        SaveState();
        var folder = AppContext.BaseDirectory;
        var request = new ClientRunner.Request(
            Dx12: Dx12.IsChecked == true,
            Timing: Timing.SelectedIndex,
            Generator: Generator.SelectedIndex,
            Upscale: Upscale.SelectedIndex,
            Target: Target.SelectedIndex,
            Buffer: Buffer.SelectedIndex,
            Recover: Recover.IsChecked == true,
            AdaptiveTriggers: AdaptiveTriggers.IsChecked == true,
            Haptics: Haptics.IsChecked == true,
            BtSpeaker: BtSpeaker.IsChecked == true,
            MicUnmuted: MicUnmuted.IsChecked == true,
            LightbarIndex: Lightbar.SelectedIndex,
            PollingIndex: Polling.SelectedIndex,
            AdvancedSettings: advancedSettings);
        if (!ClientRunner.IsAvailable(folder))
        {
            SetStatus("Запусти из папки PS5Link-Win", "Run from the PS5Link-Win folder", 0xFF453A);
            return;
        }

        busy = true;
        SetStatus("Клиент открыт", "Client open", 0xFF9F0A);
        Refresh();
        try
        {
            int exitCode = await ClientRunner.RunAsync(folder, request);
            if (exitCode == 0)
                SetStatus("Клиент закрыт", "Client closed", 0x30D158);
            else
                SetStatus($"Ошибка запуска ({exitCode})", $"Launch error ({exitCode})", 0xFF453A);
        }
        catch (Win32Exception)
        {
            SetStatus("Не удалось запустить PS5Link", "Could not start PS5Link", 0xFF453A);
        }
        catch (InvalidOperationException)
        {
            SetStatus("Не удалось запустить PS5Link", "Could not start PS5Link", 0xFF453A);
        }
        finally
        {
            busy = false;
            Refresh();
        }
    }

    private async void UpdateApplication(object? sender, RoutedEventArgs e)
    {
        if (busy || updating)
            return;

        updating = true;
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
            if (ReleaseUpdate.OtherInstanceIsRunning(directory))
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
            if (ReleaseUpdate.OtherInstanceIsRunning(directory))
            {
                ShowUpdateFeedback("Закрой другие окна PS5Link и попробуй снова.",
                    "Close other PS5Link windows and try again.");
                return;
            }

            ReleaseUpdate.StartInstaller(installer, directory, russian);
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
            if (!windowLifetime.IsCancellationRequested)
                Refresh();
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
