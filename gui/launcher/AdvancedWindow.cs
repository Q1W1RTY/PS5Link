// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform.Storage;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace PS5Link.Launcher;

internal sealed class AdvancedWindow : Window
{
    private sealed record Choice(int Value, string Caption)
    {
        public override string ToString() => Caption;
    }

    private readonly bool russian;
    private readonly string folder = AppContext.BaseDirectory;
    private ClientRunner.ProfileSettings profile;
    private readonly TabControl tabs = new();
    private readonly TextBlock status = new() { TextWrapping = TextWrapping.Wrap, FontSize = 12, Margin = new(0, 2) };
    private readonly Button save = new();
    private readonly Button close = new();
    private readonly Dictionary<string, Func<JsonNode?>> editors = new();
    private bool working;

    internal AdvancedWindow(MainWindow owner, ClientRunner.ProfileSettings profile, bool russian, bool consoleSetup)
    {
        this.profile = profile;
        this.russian = russian;
        Title = T("Настройки PS5Link", "PS5Link settings");
        Width = 620;
        Height = 660;
        MinWidth = 475;
        MinHeight = 370;
        MaxHeight = owner.ViewportHeight;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        Background = owner.TryFindResource("ShellBrush", out var shell) && shell is IBrush brush
            ? brush : Brushes.Black;
        FontFamily = owner.FontFamily;
        Icon = owner.Icon;

        var header = new StackPanel { Spacing = 3, Margin = new(0, 0, 0, 12) };
        header.Children.Add(new TextBlock { Text = Title, FontSize = 23, FontWeight = FontWeight.SemiBold });
        header.Children.Add(status);

        save.Content = T("Сохранить", "Save");
        save.MinWidth = 120;
        save.Classes.Add("accent");
        save.Click += async (_, _) => await RunAsync(async token =>
        {
            await SaveAsync(token);
            CloseAfterAction();
        });
        close.Content = T("Отмена", "Cancel");
        close.Click += (_, _) => CloseAfterAction();
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, HorizontalAlignment = HorizontalAlignment.Right,
            Margin = new(0, 14, 0, 0), Children = { close, save } };
        var layout = new Grid { Margin = new Thickness(20, 16), RowDefinitions = new RowDefinitions("Auto,*,Auto") };
        layout.Children.Add(header);
        Grid.SetRow(tabs, 1);
        layout.Children.Add(tabs);
        Grid.SetRow(actions, 2);
        layout.Children.Add(actions);
        Content = layout;
        BuildTabs();
        if (consoleSetup)
            tabs.SelectedIndex = tabs.ItemCount - 1;
    }

    protected override void OnClosing(WindowClosingEventArgs e)
    {
        if (working)
            e.Cancel = true;
        base.OnClosing(e);
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        if (e.Key == Key.Escape && !working)
        {
            CloseAfterAction();
            e.Handled = true;
        }
        base.OnKeyDown(e);
    }

    private string T(string ru, string en) => russian ? ru : en;
    private string ProfileName(string name) => name.Length == 0 ? T("По умолчанию", "Default") : name;

    private void BuildTabs()
    {
        int selected = Math.Max(tabs.SelectedIndex, 0);
        editors.Clear();

        var stream = new StackPanel { Spacing = 10 };
        AddStream(stream, "Local", T("В домашней сети", "Home network"));
        AddStream(stream, "Remote", T("Через интернет", "Over the internet"));
        stream.Children.Add(Heading(T("Подключение", "Connection")));
        Toggle(stream, "Discovery", T("Искать консоли в локальной сети", "Discover consoles on the local network"));
        Toggle(stream, "WifiMode", T("Режим потоковой передачи Wi-Fi", "Wi-Fi streaming mode"));
        Toggle(stream, "AlwaysIdr", T("Запрашивать ключевой кадр при пропуске", "Request a keyframe on skipped frames"));

        var picture = new StackPanel { Spacing = 10 };
        picture.Children.Add(Heading(T("Окно стрима", "Stream window")));
        Select(picture, "Renderer", T("Стандартный рендерер", "Standard renderer"), (0, "Vulkan"), (1, "OpenGL"));
        Select(picture, "StandardWindow", T("Режим стандартного окна", "Standard window mode"),
            (0, T("По разрешению стрима", "Match stream")), (1, T("Свой размер", "Custom size")),
            (2, T("Изменяемый размер", "Resizable")), (3, T("Полный экран", "Fullscreen")),
            (4, T("Заполнить с обрезкой", "Crop to fill")), (5, T("Растянуть", "Stretch")));
        Integer(picture, "WindowWidth", T("Ширина стандартного окна, px", "Standard window width, px"), 320, 15360, 10);
        Integer(picture, "WindowHeight", T("Высота стандартного окна, px", "Standard window height, px"), 180, 8640, 10);
        Toggle(picture, "ZeroCopy", T("Декодирование без копирования", "Zero-copy decoding"));
        Select(picture, "Dx12Window", T("Режим окна DX12", "DX12 window mode"),
            (0, T("Полный экран", "Fullscreen")), (1, T("Без рамки", "Borderless")), (2, T("Окно", "Windowed")));
        Select(picture, "PictureAspect", T("Заполнение DX12", "DX12 scaling"),
            (0, T("С полями", "Fit")), (1, T("С обрезкой", "Fill")), (2, T("Растянуть", "Stretch")), (3, "Ambient"));
        Select(picture, "AmbientStyle", T("Фон Ambient", "Ambient background"),
            (0, T("Цвета", "Colors")), (1, T("Размытие", "Blur")), (2, T("Растянуть края", "Stretch edges")));

        picture.Children.Add(Heading(T("Обработка DX12", "DX12 processing")));
        Integer(picture, "Sharpness", T("Резкость CAS (−1 — авто)", "CAS sharpness (−1 for auto)"), -1, 100, 1);
        Toggle(picture, "VideoConcealment", T("Скрывать повреждённые кадры", "Conceal corrupt frames"));
        Toggle(picture, "Dither", T("Дизеринг", "Dithering"));
        Toggle(picture, "Deband", T("Сглаживать цветовые полосы", "Reduce color banding"));
        Toggle(picture, "Hdr", T("Выводить HDR10 напрямую", "Output HDR10 directly"));
        Integer(picture, "HdrPaperWhite", T("Белый при HDR → SDR, нит", "HDR-to-SDR paper white, nits"), 120, 400, 5);
        Integer(picture, "Contrast", T("Контраст, %", "Contrast, %"), 80, 130, 1);
        Integer(picture, "Saturation", T("Насыщенность, %", "Saturation, %"), 50, 150, 1);
        Integer(picture, "Gamma", T("Гамма, %", "Gamma, %"), 80, 125, 1);

        var control = new StackPanel { Spacing = 10 };
        control.Children.Add(Heading(T("Во время стрима", "During streaming")));
        Toggle(control, "Stats", T("Показывать статистику стрима", "Show streaming statistics"));
        Toggle(control, "KeepDisplayAwake", T("Не отключать экран во время стрима", "Keep display awake during streaming"));
        Toggle(control, "SystemVolume", T("Управлять системной громкостью", "Control system volume"));
        Select(control, "DisconnectAction", T("При отключении", "On disconnect"),
            (0, T("Оставить консоль включённой", "Leave the console on")),
            (1, T("Перевести консоль в покой", "Put the console in rest mode")), (2, T("Спрашивать", "Ask")));
        Select(control, "SuspendAction", T("Когда ПК засыпает", "When the PC sleeps"),
            (0, T("Оставить консоль включённой", "Leave the console on")), (1, T("Перевести консоль в покой", "Put the console in rest mode")));
        control.Children.Add(Heading(T("Ввод", "Input")));
        Toggle(control, "GyroReset", T("Разрешить сброс гироскопа", "Allow gyro reset"));
        Toggle(control, "GamepadQuit", T("Разрешить выход с геймпада", "Allow gamepad quit shortcut"));
        Toggle(control, "Keyboard", T("Управлять с клавиатуры", "Play with the keyboard"));
        Toggle(control, "MouseTouch", T("Использовать мышь как тачпад", "Use mouse as touchpad"));
        Select(control, "RumbleIntensity", T("Интенсивность вибрации", "Rumble intensity"),
            (0, T("Выкл.", "Off")), (1, T("Очень слабая", "Very weak")), (2, T("Слабая", "Weak")),
            (3, T("Обычная", "Normal")), (4, T("Сильная", "Strong")), (5, T("Очень сильная", "Very strong")));

        tabs.ItemsSource = new[]
        {
            Page(T("Стрим", "Stream"), stream),
            Page(T("Изображение", "Picture"), picture),
            Page(T("Управление", "Controls"), control),
            Page(T("Профили", "Profiles"), BuildProfiles()),
            Page(T("Консоли и PSN", "Consoles & PSN"), BuildConsoles())
        };
        tabs.SelectedIndex = Math.Min(selected, tabs.ItemCount - 1);
        status.Text = T("Активный профиль", "Active profile") + " · " + ProfileName(profile.Profile);
    }

    private StackPanel BuildProfiles()
    {
        var page = new StackPanel { Spacing = 10 };
        page.Children.Add(Heading(T("Профиль", "Profile")));
        var names = profile.Profiles.Append(profile.Profile).Distinct().ToArray();
        var picker = new ComboBox { ItemsSource = names.Select(ProfileName).ToArray(), SelectedIndex = Array.IndexOf(names, profile.Profile),
            HorizontalAlignment = HorizontalAlignment.Stretch };
        AddLabel(page, T("Выбрать профиль", "Select a profile"), picker);
        var activate = Button(T("Активировать", "Activate"), async token =>
        {
            await SaveAsync(token);
            await ClientRunner.RunProfileCommandAsync(folder, "switch", names[picker.SelectedIndex], token);
            await ReloadAsync(token);
        });
        activate.IsEnabled = false;
        picker.SelectionChanged += (_, _) => activate.IsEnabled = picker.SelectedIndex >= 0 && names[picker.SelectedIndex] != profile.Profile;
        page.Children.Add(activate);
        page.Children.Add(Heading(T("Резервные копии", "Backups")));
        page.Children.Add(new TextBlock
        {
            Text = T("Импорт создаст и активирует отдельный профиль. Ключи привязки в копии защищены Windows.",
                "Import creates and activates a separate profile. Windows protects the pairing keys in the backup."),
            TextWrapping = TextWrapping.Wrap
        });
        page.Children.Add(Button(T("Импортировать .ini", "Import .ini"), async token =>
        {
            var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
            {
                Title = T("Импорт профиля", "Import profile"),
                FileTypeFilter = [IniFileType]
            });
            if (files.FirstOrDefault()?.TryGetLocalPath() is not { } path)
                return;
            await SaveAsync(token);
            await ClientRunner.RunProfileCommandAsync(folder, "import", path, token);
            await ReloadAsync(token);
            status.Text = T("Профиль импортирован", "Profile imported") + " · " + ProfileName(profile.Profile);
        }));
        page.Children.Add(Button(T("Экспортировать .ini", "Export .ini"), async token =>
        {
            var file = await StorageProvider.SaveFilePickerAsync(new FilePickerSaveOptions
            {
                Title = T("Экспорт профиля", "Export profile"),
                SuggestedFileName = "PS5Link.ini",
                DefaultExtension = "ini",
                FileTypeChoices = [IniFileType],
                ShowOverwritePrompt = true
            });
            if (file?.TryGetLocalPath() is not { } path)
                return;
            await SaveAsync(token);
            await ClientRunner.RunProfileCommandAsync(folder, "export", path, token);
            status.Text = T("Копия профиля сохранена", "Profile backup saved");
        }));
        return page;
    }

    private StackPanel BuildConsoles()
    {
        var page = new StackPanel { Spacing = 10 };
        page.Children.Add(Heading(T("Вход в PlayStation Network", "PlayStation Network sign-in")));
        page.Children.Add(new TextBlock
        {
            Text = profile.PsnLinked ? T("Учётные данные PSN сохранены", "PSN credentials saved")
                : T("Вход в PSN ещё не настроен", "PSN sign-in is not set up"),
            TextWrapping = TextWrapping.Wrap
        });
        var redirect = new TextBox { PasswordChar = '●', MaxLength = 4096 };
        page.Children.Add(Button(T("Открыть вход в браузере", "Open sign-in in browser"), async token =>
        {
            var url = await ClientRunner.ReadPsnUrlAsync(folder, token);
            using var browser = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(url.AbsoluteUri)
            {
                UseShellExecute = true
            });
            status.Text = T("После входа скопируй адрес из строки браузера и вставь его здесь.",
                "After signing in, copy the browser address and paste it here.");
            redirect.Focus();
        }));
        AddLabel(page, T("Адрес после входа", "Address after sign-in"), redirect);
        page.Children.Add(Button(T("Завершить вход", "Finish sign-in"), async token =>
        {
            var address = redirect.Text?.Trim();
            if (string.IsNullOrEmpty(address))
            {
                status.Text = T("Вставь адрес из браузера после входа в PSN.", "Paste the browser address after PSN sign-in.");
                return;
            }
            await SaveAsync(token);
            status.Text = T("Проверяю вход в PSN…", "Checking PSN sign-in…");
            try
            {
                await ClientRunner.FinishPsnLoginAsync(folder, profile.Profile, address, token);
            }
            finally
            {
                redirect.Clear();
            }
            await ReloadAsync(token);
            status.Text = T("Вход в PSN выполнен", "Signed in to PSN");
        }));

        page.Children.Add(Heading(T("Регистрация в локальной сети", "Local network registration")));
        page.Children.Add(new TextBlock
        {
            Text = T("На консоли открой «Дистанционное воспроизведение» и получи код привязки. Укажи локальный адрес консоли.",
                "On the console, open Remote Play and get the pairing code. Enter the console’s local address."),
            TextWrapping = TextWrapping.Wrap
        });
        var model = new ComboBox { ItemsSource = new[] { "PS5", "PS4" }, SelectedIndex = 0, HorizontalAlignment = HorizontalAlignment.Stretch };
        var host = new TextBox { PlaceholderText = "192.168.1.20", MaxLength = 253 };
        var pin = new TextBox { MaxLength = 8, PasswordChar = '●' };
        var account = new TextBox { MaxLength = 32, PasswordChar = '●' };
        var discovered = new ComboBox { HorizontalAlignment = HorizontalAlignment.Stretch, IsVisible = false };
        AutomationProperties.SetName(discovered, T("Найденные консоли", "Discovered consoles"));
        discovered.SelectionChanged += (_, _) =>
        {
            if (discovered.SelectedItem is ClientRunner.DiscoveredConsole device)
            {
                host.Text = device.Address;
                model.SelectedIndex = device.Ps5 ? 0 : 1;
            }
        };
        page.Children.Add(Button(T("Найти консоли", "Find consoles"), async token =>
        {
            status.Text = T("Ищу консоли в локальной сети…", "Looking for consoles on the local network…");
            var devices = await ClientRunner.DiscoverConsolesAsync(folder, token);
            discovered.ItemsSource = devices;
            discovered.IsVisible = devices.Length > 0;
            discovered.SelectedIndex = devices.Length == 1 ? 0 : -1;
            status.Text = devices.Length > 0 ? T("Выбери найденную консоль.", "Select a discovered console.")
                : T("Консоли не найдены. Можно ввести локальный адрес вручную.", "No consoles found. You can enter the local address manually.");
        }));
        page.Children.Add(discovered);
        AddLabel(page, T("Тип консоли", "Console type"), model);
        AddLabel(page, T("Локальный адрес", "Local address"), host);
        AddLabel(page, T("Код привязки из 8 цифр", "8-digit pairing code"), pin);
        AddLabel(page, T("Account ID в Base64 (если нет входа в PSN)", "Base64 account ID (if not signed in)"), account);
        page.Children.Add(Button(T("Зарегистрировать консоль", "Register console"), async token =>
        {
            var address = host.Text?.Trim() ?? "";
            var code = pin.Text ?? "";
            var accountId = account.Text?.Trim() ?? "";
            if (address.Length == 0 || code.Length != 8 || !code.All(char.IsAsciiDigit)
                || (!profile.PsnLinked && accountId.Length == 0))
            {
                status.Text = T("Проверь адрес, восьмизначный код и вход в PSN или Account ID.",
                    "Check the address, 8-digit code, and PSN sign-in or account ID.");
                return;
            }
            await SaveAsync(token);
            status.Text = T("Отправляю запрос регистрации…", "Sending the pairing request…");
            try
            {
                await ClientRunner.RegisterConsoleAsync(folder, profile.Profile, address, code, accountId, model.SelectedIndex == 0, token);
            }
            finally
            {
                pin.Clear();
                account.Clear();
            }
            await ReloadAsync(token);
            status.Text = T("Консоль зарегистрирована и выбрана для подключения", "Console registered and selected");
        }));
        return page;
    }

    private static FilePickerFileType IniFileType { get; } = new("INI") { Patterns = ["*.ini"] };

    private void AddStream(StackPanel page, string prefix, string heading)
    {
        page.Children.Add(Heading(heading));
        Select(page, prefix + "Resolution", T("Разрешение PS5", "PS5 resolution"),
            (1, "360p"), (2, "540p"), (3, "720p"), (4, "1080p"));
        Select(page, prefix + "Fps", T("Кадры в секунду", "Frames per second"), (30, "30"), (60, "60"));
        Select(page, prefix + "Codec", T("Кодек", "Codec"), (0, "H.264"), (1, "HEVC"), (2, "HEVC HDR"));
        Integer(page, prefix + "Bitrate", T("Битрейт, кбит/с (0 — авто)", "Bitrate, kbps (0 for auto)"), 0, 150000, 1000);
    }

    private static TextBlock Heading(string text) => new()
    {
        Text = text, FontSize = 15, FontWeight = FontWeight.SemiBold, Margin = new(0, 9, 0, 2)
    };

    private static TabItem Page(string label, StackPanel controls)
    {
        controls.Margin = new Thickness(4, 4, 12, 16);
        return new TabItem
        {
            Header = label,
            FontSize = 15,
            Padding = new Thickness(10, 7),
            Content = new ScrollViewer { HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled, Content = controls }
        };
    }

    private static void AddLabel(StackPanel page, string label, Control editor)
    {
        AutomationProperties.SetName(editor, label);
        var row = new Grid { ColumnDefinitions = new ColumnDefinitions("*,210"), ColumnSpacing = 12 };
        row.Children.Add(new TextBlock { Text = label, TextWrapping = TextWrapping.Wrap, VerticalAlignment = VerticalAlignment.Center });
        Grid.SetColumn(editor, 1);
        row.Children.Add(editor);
        page.Children.Add(row);
    }

    private void Toggle(StackPanel page, string key, string label)
    {
        var control = new ToggleSwitch { IsChecked = profile.Advanced[key]?.GetValue<bool>() == true,
            HorizontalAlignment = HorizontalAlignment.Right, OnContent = "", OffContent = "" };
        AddLabel(page, label, control);
        editors.Add(key, () => JsonValue.Create(control.IsChecked == true));
    }

    private void Select(StackPanel page, string key, string label, params (int Value, string Caption)[] options)
    {
        var choices = options.Select(item => new Choice(item.Value, item.Caption)).ToArray();
        int value = profile.Advanced[key]?.GetValue<int>() ?? choices[0].Value;
        var control = new ComboBox
        {
            ItemsSource = choices,
            SelectedIndex = Array.FindIndex(choices, item => item.Value == value),
            HorizontalAlignment = HorizontalAlignment.Stretch
        };
        AddLabel(page, label, control);
        editors.Add(key, () => control.SelectedItem is Choice item ? JsonValue.Create(item.Value) : null);
    }

    private void Integer(StackPanel page, string key, string label, int min, int max, int step)
    {
        int initial = profile.Advanced[key]?.GetValue<int>() ?? min;
        var control = new NumericUpDown
        {
            Minimum = Math.Min(min, initial), Maximum = Math.Max(max, initial), Increment = step, FormatString = "0",
            Value = initial, HorizontalAlignment = HorizontalAlignment.Stretch
        };
        AddLabel(page, label, control);
        editors.Add(key, () =>
        {
            if (control.Value is not { } value)
                return null;
            if (value == initial && (value < min || value > max))
                return null;
            if (value < min || value > max || decimal.Truncate(value) != value)
                throw new IOException($"Invalid value for {label}.");
            return JsonValue.Create((int)value);
        });
    }

    private Button Button(string label, Func<CancellationToken, Task> action)
    {
        var button = new Button { Content = label };
        button.Click += async (_, _) => await RunAsync(action);
        return button;
    }

    private async Task SaveAsync(CancellationToken token)
    {
        var patch = new JsonObject();
        foreach (var (name, read) in editors)
        {
            var value = read();
            if (value is not null && !JsonNode.DeepEquals(value, profile.Advanced[name]))
                patch[name] = value;
        }
        if (patch.Count == 0)
            return;
        await ClientRunner.WriteSettingsAsync(folder, profile.Profile, new JsonObject(), token, advanced: patch);
        foreach (var field in patch)
            profile.Advanced[field.Key] = field.Value?.DeepClone();
    }

    private async Task ReloadAsync(CancellationToken token)
    {
        profile = await ClientRunner.ReadSettingsAsync(folder, token);
        BuildTabs();
    }

    private async Task RunAsync(Func<CancellationToken, Task> action)
    {
        if (working)
            return;
        working = true;
        tabs.IsEnabled = save.IsEnabled = close.IsEnabled = false;
        try
        {
            // File picker interaction can take as long as the user needs.
            await action(CancellationToken.None);
        }
        catch (ClientRunner.ProfileCommandException error) when (error.ExitCode == 4)
        {
            status.Text = T("Копия повреждена или её ключи недоступны в этой учётной записи Windows.",
                "The backup is invalid or its keys are unavailable to this Windows account.");
        }
        catch (ClientRunner.ProfileCommandException error) when (error.ExitCode == 5)
        {
            status.Text = T("Не удалось завершить вход. Открой новый вход в браузере и повтори.",
                "Sign-in failed. Start a new sign-in in the browser and try again.");
        }
        catch (ClientRunner.ProfileCommandException error) when (error.ExitCode == 6)
        {
            status.Text = T("Консоль не ответила или отклонила запрос. Проверь локальный адрес и код.",
                "The console did not respond or rejected the request. Check its local address and code.");
        }
        catch (ClientRunner.ProfileCommandException error) when (error.ExitCode == 2 && error.Message.Contains("--launcher-", StringComparison.Ordinal))
        {
            status.Text = T("Проверь адрес входа или данные регистрации.", "Check the sign-in address or pairing details.");
        }
        catch (Exception error) when (error is IOException or InvalidOperationException or JsonException or OperationCanceledException
            or System.ComponentModel.Win32Exception or UnauthorizedAccessException)
        {
            System.Diagnostics.Trace.WriteLine(error.Message);
            status.Text = T("Операция с профилем не завершилась. Проверь файл или активный профиль.",
                "The profile operation did not finish. Check the file or active profile.");
        }
        finally
        {
            working = false;
            tabs.IsEnabled = save.IsEnabled = close.IsEnabled = true;
        }
    }

    private void CloseAfterAction()
    {
        working = false;
        Close();
    }
}
