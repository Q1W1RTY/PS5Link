using System.Diagnostics;
using System.Net.Http.Json;
using System.Security.Cryptography;
using System.Text.Json;
using Microsoft.Win32;

namespace PS5Link.Launcher;

internal static class ReleaseUpdate
{
    private const string Repository = "Q1W1RTY/PS5Link";
    private const string TagPrefix = "ps5link-v";
    private const long MaxInstallerBytes = 512L * 1024 * 1024;
    private const string UninstallKey = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{9AA0F444-6856-499A-B2BE-8BD7D47238E6}_is1";
    private static readonly HttpClient Client = CreateClient();

    internal sealed record Release(Version Version, long Bytes, byte[] Digest)
    {
        internal Uri DownloadUrl => new($"https://github.com/{Repository}/releases/download/{TagPrefix}{Version}/PS5Link-{Version}-win-x64-setup.exe");
    }

    internal static Version CurrentVersion { get; } =
        Version.Parse(typeof(ReleaseUpdate).Assembly.GetName().Version!.ToString(3));

    private static HttpClient CreateClient()
    {
        var client = new HttpClient();
        client.DefaultRequestHeaders.UserAgent.ParseAdd("PS5Link-Updater");
        return client;
    }

    internal static string? GetInstallDirectory()
    {
        if (!OperatingSystem.IsWindows())
            return null;

        using var key = Registry.CurrentUser.OpenSubKey(UninstallKey);
        if (key?.GetValue("InstallLocation") is not string location || !Path.IsPathFullyQualified(location))
            return null;

        string installed = Path.TrimEndingDirectorySeparator(Path.GetFullPath(location));
        string current = Path.TrimEndingDirectorySeparator(AppContext.BaseDirectory);
        return string.Equals(installed, current, StringComparison.OrdinalIgnoreCase) ? installed : null;
    }

    internal static bool OtherInstanceIsRunning(string installDirectory)
    {
        foreach (var process in Process.GetProcessesByName("PS5Link").Concat(Process.GetProcessesByName("PS5LinkLauncher")))
        {
            using (process)
            {
                if (process.Id == Environment.ProcessId)
                    continue;
                try
                {
                    string? directory = Path.GetDirectoryName(process.MainModule?.FileName);
                    if (string.Equals(directory, installDirectory, StringComparison.OrdinalIgnoreCase))
                        return true;
                }
                catch (InvalidOperationException) { }
                catch (System.ComponentModel.Win32Exception) { }
            }
        }
        return false;
    }

    internal static async Task<Release?> CheckAsync(CancellationToken token)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get,
            $"https://api.github.com/repos/{Repository}/releases/latest");
        request.Headers.Accept.ParseAdd("application/vnd.github+json");
        using var response = await Client.SendAsync(request, token);
        response.EnsureSuccessStatusCode();
        using var document = await response.Content.ReadFromJsonAsync<JsonDocument>(token)
            ?? throw new InvalidDataException("GitHub returned an empty release.");
        return ReadRelease(document.RootElement, CurrentVersion);
    }

    internal static Release? ReadRelease(JsonElement release, Version currentVersion)
    {
        if (release.GetProperty("draft").GetBoolean() || release.GetProperty("prerelease").GetBoolean())
            return null;

        string? tag = release.GetProperty("tag_name").GetString();
        if (tag is null || !tag.StartsWith(TagPrefix, StringComparison.Ordinal) ||
            !Version.TryParse(tag[TagPrefix.Length..], out var version) ||
            version.Build < 0 || version.Revision >= 0 || tag != $"{TagPrefix}{version}")
            throw new InvalidDataException("Unrecognized PS5Link release tag.");
        if (version <= currentVersion)
            return null;

        string name = $"PS5Link-{version}-win-x64-setup.exe";
        foreach (var asset in release.GetProperty("assets").EnumerateArray())
        {
            if (asset.GetProperty("name").GetString() != name)
                continue;

            long size = asset.GetProperty("size").GetInt64();
            string? digest = asset.TryGetProperty("digest", out var field) ? field.GetString() : null;
            if (asset.GetProperty("state").GetString() != "uploaded" || size <= 0 ||
                size > MaxInstallerBytes || digest is null || !digest.StartsWith("sha256:", StringComparison.Ordinal) ||
                digest.Length != 71)
                break;
            return new Release(version, size, Convert.FromHexString(digest.AsSpan(7)));
        }
        throw new InvalidDataException("Verified Windows installer is missing from the release.");
    }

    internal static async Task<string> DownloadAsync(Release release, CancellationToken token)
    {
        string cache = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PS5Link", "Updates");
        Directory.CreateDirectory(cache);
        string installer = Path.Combine(cache, "PS5Link-update.exe");
        string temporary = Path.Combine(cache, $"{Guid.NewGuid():N}.part");
        try
        {
            using var response = await Client.GetAsync(release.DownloadUrl, HttpCompletionOption.ResponseHeadersRead, token);
            response.EnsureSuccessStatusCode();
            if (response.Content.Headers.ContentLength is long size && size != release.Bytes)
                throw new InvalidDataException("The download size does not match the GitHub release.");

            await using (var output = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None, 128 * 1024))
            await using (var source = await response.Content.ReadAsStreamAsync(token))
            using (var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256))
            {
                byte[] buffer = new byte[128 * 1024];
                long total = 0;
                int read;
                while ((read = await source.ReadAsync(buffer, token)) > 0)
                {
                    total += read;
                    if (total > release.Bytes)
                        throw new InvalidDataException("The update exceeded its advertised size.");
                    await output.WriteAsync(buffer.AsMemory(0, read), token);
                    hash.AppendData(buffer, 0, read);
                }
                if (total != release.Bytes || !CryptographicOperations.FixedTimeEquals(hash.GetHashAndReset(), release.Digest))
                    throw new InvalidDataException("The update could not be verified.");
            }
            File.Move(temporary, installer, overwrite: true);
            return installer;
        }
        finally
        {
            try { File.Delete(temporary); }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }
    }

    internal static void StartInstaller(string installer, string installDirectory, bool russian)
    {
        var start = new ProcessStartInfo(installer) { UseShellExecute = false };
        foreach (string argument in new[]
        {
            "/SP-", "/SILENT", "/NORESTART", "/NOCLOSEAPPLICATIONS",
            $"/LANG={(russian ? "russian" : "english")}", $"/DIR={installDirectory}",
            $"/PS5LINKUPDATEPID={Environment.ProcessId}"
        })
            start.ArgumentList.Add(argument);
        using var process = Process.Start(start) ?? throw new IOException("Could not start PS5Link setup.");
    }
}
