// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
using System.Diagnostics;

namespace PS5Link.Launcher;

internal static class ClientRunner
{
    internal sealed record Request(
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
        bool MicUnmuted,
        int LightbarIndex,
        int PollingIndex,
        bool AdvancedSettings);

    internal static bool IsAvailable(string folder) => File.Exists(Path.Combine(folder, "PS5Link.exe"));

    internal static async Task<int> RunAsync(string folder, Request request)
    {
        var info = new ProcessStartInfo(Path.Combine(folder, "PS5Link.exe"), Arguments(request))
        {
            WorkingDirectory = folder,
            UseShellExecute = false,
            CreateNoWindow = true
        };
        using var process = Process.Start(info) ?? throw new InvalidOperationException("PS5Link did not start.");
        await process.WaitForExitAsync().ConfigureAwait(false);
        return process.ExitCode;
    }

    internal static string Arguments(Request request)
    {
        if (request.AdvancedSettings)
            return "--online --settings";

        var args = new List<string>
        {
            "--online --quick-connect --reorder-window " + (request.Buffer * 8),
            request.Recover ? "--idr-recovery" : "--no-idr-recovery",
            "--adaptive-triggers " + (request.AdaptiveTriggers ? "on" : "off"),
            "--haptics " + (request.Haptics ? "on" : "off"),
            "--bt-speaker " + (request.BtSpeaker ? "on" : "off"),
            "--mic-unmuted " + (request.MicUnmuted ? "on" : "off"),
            "--lightbar " + (request.LightbarIndex switch
            {
                1 => "both",
                2 => "screen",
                3 => "off",
                _ => "pad"
            }),
            "--polling-ms " + (request.PollingIndex switch
            {
                0 => 1,
                1 => 2,
                3 => 8,
                _ => 4
            })
        };
        if (request.Dx12)
        {
            args.Add("--dx12");
            args.Add("--dx12-present " + new[] { "balanced", "low", "vsync" }[request.Timing]);
            args.Add(new[] { "--no-frame-generation", "--frame-interpolation", "--dlss-fg", "--fsr-fg" }[request.Upscale >= 6 ? 0 : request.Generator]);
            string target = request.Target switch
            {
                1 when request.Upscale >= 6 => "stream",
                2 when request.Upscale >= 2 => "1800",
                3 when request.Upscale >= 2 => "2160",
                _ => "output"
            };
            args.Add("--upscale-target " + target);
        }
        else
            args.Add("--standard-renderer");
        if (request.Dx12 && request.Upscale == 1)
            args.Add("--fsr");
        else if (request.Dx12 && request.Upscale >= 6)
            args.Add(request.Upscale == 7 ? "--dlss-sr 4.5" : "--dlss-sr 4");
        else if (request.Upscale >= 2 && request.Upscale <= 5)
            args.Add("--rtx-vsr " + (request.Upscale - 1));
        else
            args.Add("--rtx-vsr 0");
        return string.Join(" ", args);
    }
}
