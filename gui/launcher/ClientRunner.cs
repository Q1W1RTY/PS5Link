// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace PS5Link.Launcher;

internal static class ClientRunner
{
    internal sealed record Preferences(
        bool Dx12,
        int Timing,
        int Generator,
        int Upscale,
        int Target,
        int Buffer,
        bool Recover,
        bool AdaptiveTriggers,
        bool Haptics,
        bool BtSpeaker,
        bool MutePcAudio,
        bool MicUnmuted,
        int Lightbar,
        int Polling);

    internal sealed record ConsoleEntry(string Id, string Name, bool Ps5);
    internal sealed record DiscoveredConsole(string Name, string Address, bool Ps5,
        string? Id, string? State, string? RunningApp)
    {
        public override string ToString() => $"{(string.IsNullOrWhiteSpace(Name) ? Ps5 ? "PS5" : "PS4" : Name)} · {Address}";
    }

    internal sealed record ProfileSettings(string Profile, Preferences Values, string[] Missing,
        JsonObject Advanced, ConsoleEntry[] Consoles, string PreferredConsole, string[] Profiles, bool PsnLinked);

    internal sealed class ProfileCommandException(string operation, int exitCode)
        : IOException($"PS5Link profile {operation} failed ({exitCode}).")
    {
        internal int ExitCode { get; } = exitCode;
    }

    internal static bool IsAvailable(string folder) => File.Exists(Path.Combine(folder, "PS5Link.exe"));

    internal static Process RunIdleMonitor(string folder) =>
        Start(CreateStartInfo(folder, $"--launcher-controller-idle {Environment.ProcessId}"));

    internal static async Task<int> RunAsync(string folder)
    {
        using var process = Start(CreateStartInfo(folder,
            "--online --quick-connect --launcher-session --exit-app-on-stream-exit"));
        await process.WaitForExitAsync().ConfigureAwait(false);
        return process.ExitCode;
    }

    internal static async Task<ProfileSettings> ReadSettingsAsync(string folder, CancellationToken cancellationToken)
    {
        var json = await RunCommandAsync(folder, "--read-launcher-settings", cancellationToken, readOutput: true).ConfigureAwait(false);
        return JsonSerializer.Deserialize<ProfileSettings>(json)
            ?? throw new JsonException("PS5Link did not return profile settings.");
    }

    internal static Task WriteSettingsAsync(string folder, string profile, JsonObject patch, CancellationToken cancellationToken,
        JsonObject? advanced = null, string? preferredConsole = null)
    {
        var request = new JsonObject { ["Profile"] = profile, ["Values"] = patch };
        if (advanced is not null)
            request["Advanced"] = advanced;
        if (preferredConsole is not null)
            request["PreferredConsole"] = preferredConsole;
        return RunCommandAsync(folder, "--write-launcher-settings", cancellationToken, request);
    }

    internal static Task RunProfileCommandAsync(string folder, string operation, string value, CancellationToken cancellationToken) =>
        RunCommandAsync(folder, $"--{operation}-launcher-profile={value}", cancellationToken);

    internal static async Task<DiscoveredConsole[]> DiscoverConsolesAsync(string folder, CancellationToken cancellationToken)
    {
        string json = await RunCommandAsync(folder, "--launcher-discover-consoles", cancellationToken, readOutput: true).ConfigureAwait(false);
        return JsonSerializer.Deserialize<DiscoveredConsole[]>(json) ?? [];
    }

    internal static async Task<Uri> ReadPsnUrlAsync(string folder, CancellationToken cancellationToken)
    {
        string json = await RunCommandAsync(folder, "--launcher-psn-url", cancellationToken, readOutput: true).ConfigureAwait(false);
        using var document = JsonDocument.Parse(json);
        if (!Uri.TryCreate(document.RootElement.GetProperty("Url").GetString(), UriKind.Absolute, out var url)
            || url.Scheme != Uri.UriSchemeHttps)
            throw new IOException("PS5Link did not return a valid sign-in URL.");
        return url;
    }

    internal static Task FinishPsnLoginAsync(string folder, string profile, string redirect, CancellationToken cancellationToken) =>
        RunCommandAsync(folder, "--launcher-psn-login", cancellationToken,
            new JsonObject { ["Profile"] = profile, ["Redirect"] = redirect });

    internal static Task RegisterConsoleAsync(string folder, string profile, string host, string pin, string account, bool ps5,
        CancellationToken cancellationToken) =>
        RunCommandAsync(folder, "--launcher-register-console", cancellationToken,
            new JsonObject { ["Profile"] = profile, ["Host"] = host, ["Pin"] = pin, ["Account"] = account, ["Ps5"] = ps5 });

    private static async Task<string> RunCommandAsync(string folder, string command, CancellationToken cancellationToken,
        JsonObject? input = null, bool readOutput = false)
    {
        var info = CreateStartInfo(folder, "");
        info.ArgumentList.Add(command);
        info.RedirectStandardOutput = readOutput;
        info.RedirectStandardInput = input is not null;
        if (readOutput)
            info.StandardOutputEncoding = Encoding.UTF8;
        if (input is not null)
            info.StandardInputEncoding = new UTF8Encoding(false);
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        deadline.CancelAfter(TimeSpan.FromSeconds(35));
        using var process = Start(info);
        try
        {
            if (input is not null)
            {
                await process.StandardInput.WriteAsync(input.ToJsonString().AsMemory(), deadline.Token).ConfigureAwait(false);
                process.StandardInput.Close();
            }
            string output = readOutput
                ? await process.StandardOutput.ReadToEndAsync(deadline.Token).ConfigureAwait(false) : "";
            await process.WaitForExitAsync(deadline.Token).ConfigureAwait(false);
            if (process.ExitCode != 0)
                throw new ProfileCommandException(command.Split('=')[0], process.ExitCode);
            return output;
        }
        catch (OperationCanceledException)
        {
            try
            {
                if (!process.HasExited)
                    process.Kill();
            }
            catch (Exception error) when (error is InvalidOperationException or System.ComponentModel.Win32Exception) { }
            throw;
        }
    }

    private static ProcessStartInfo CreateStartInfo(string folder, string arguments) => new(Path.Combine(folder, "PS5Link.exe"), arguments)
    {
        WorkingDirectory = folder,
        UseShellExecute = false,
        CreateNoWindow = true
    };

    private static Process Start(ProcessStartInfo info) =>
        Process.Start(info) ?? throw new InvalidOperationException("PS5Link did not start.");
}
