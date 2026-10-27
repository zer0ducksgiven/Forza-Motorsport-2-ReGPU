using System.Diagnostics;
using System.IO.Compression;
using System.Net.Http.Headers;
using System.Text.Json;

namespace FM2Launcher;

/// <summary>
/// Updates from GitHub releases: the latest release's FM2-x.y.z-patch.zip is downloaded and unpacked over the
/// install. Files of the running launcher are renamed to *.old first (Windows lets a running exe be renamed, not
/// overwritten); the next start deletes them.
/// </summary>
public static class Updater
{
    public const string Repo = "zer0ducksgiven/Forza-Motorsport-2-ReGPU";
    public sealed record Release(string Version, string Name, string Notes, string Page, string? PatchUrl, long PatchBytes, DateTime Published);

    static readonly HttpClient Http = MakeClient();

    static HttpClient MakeClient()
    {
        var h = new HttpClient { Timeout = TimeSpan.FromMinutes(10) };
        h.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("FM2Launcher", InstalledVersion()));
        h.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
        return h;
    }

    /// <summary>The installed game's version: fm2.exe's ProductVersion, else version.txt (written by the updater), else 0.0.0.</summary>
    public static string InstalledVersion()
    {
        try
        {
            var v = File.Exists(Paths.Exe) ? FileVersionInfo.GetVersionInfo(Paths.Exe).ProductVersion : null;
            if (!string.IsNullOrWhiteSpace(v)) return v.Trim();
            // fm2.exe from before 0.2.1 has no version resource: what the updater last installed
            var file = Path.Combine(Paths.Root, "version.txt");
            return File.Exists(file) ? File.ReadAllText(file).Trim() : "0.0.0";
        }
        catch { return "0.0.0"; }
    }

    public static async Task<Release> LatestAsync()
    {
        using var doc = JsonDocument.Parse(await Http.GetStringAsync($"https://api.github.com/repos/{Repo}/releases/latest"));
        var r = doc.RootElement;
        string tag = r.GetProperty("tag_name").GetString() ?? "";
        string? url = null;
        long bytes = 0;
        foreach (var a in r.GetProperty("assets").EnumerateArray())
        {
            var name = a.GetProperty("name").GetString() ?? "";
            if (name.EndsWith("-patch.zip", StringComparison.OrdinalIgnoreCase))
            {
                url = a.GetProperty("browser_download_url").GetString();
                bytes = a.GetProperty("size").GetInt64();
            }
        }
        return new Release(tag.TrimStart('v', 'V'), r.GetProperty("name").GetString() ?? tag, r.GetProperty("body").GetString() ?? "",
            r.GetProperty("html_url").GetString() ?? $"https://github.com/{Repo}/releases", url, bytes,
            r.TryGetProperty("published_at", out var p) && p.ValueKind == JsonValueKind.String ? p.GetDateTime() : DateTime.MinValue);
    }

    public static bool IsNewer(string latest, string installed) =>
        Version.TryParse(Normalise(latest), out var a) && Version.TryParse(Normalise(installed), out var b) && a > b;

    static string Normalise(string v)
    {
        var core = new string(v.TakeWhile(c => char.IsDigit(c) || c == '.').ToArray());
        return core.Count(c => c == '.') == 0 ? core + ".0" : core;
    }

    public sealed record Applied(int Files, bool LauncherChanged);

    /// <summary>Downloads the patch and unpacks it over the install. progress: 0..1 while downloading.</summary>
    public static async Task<Applied> DownloadAndApplyAsync(Release release, Action<double, string> progress)
    {
        if (release.PatchUrl is null) throw new InvalidOperationException("This release has no patch zip. Use the installer from the release page.");
        if (Game.Running) throw new InvalidOperationException("Close the game first: its files are about to be replaced.");
        var zipPath = Path.Combine(Path.GetTempPath(), $"FM2-{release.Version}-patch.zip");
        using (var response = await Http.GetAsync(release.PatchUrl, HttpCompletionOption.ResponseHeadersRead))
        {
            response.EnsureSuccessStatusCode();
            long total = response.Content.Headers.ContentLength ?? release.PatchBytes;
            await using var input = await response.Content.ReadAsStreamAsync();
            await using var output = File.Create(zipPath);
            var buffer = new byte[1 << 16];
            long done = 0;
            int n;
            while ((n = await input.ReadAsync(buffer)) > 0)
            {
                await output.WriteAsync(buffer.AsMemory(0, n));
                done += n;
                if (total > 0) progress(Math.Min(1, (double)done / total), $"Downloading {done / 1048576.0:0.0} of {total / 1048576.0:0.0} MB");
            }
        }
        progress(1, "Installing");
        var applied = Apply(zipPath);
        File.WriteAllText(Path.Combine(Paths.Root, "version.txt"), release.Version);
        return applied;
    }

    public static Applied Apply(string zipPath, bool deleteZip = true)
    {
        var root = Path.GetFullPath(Paths.Root) + Path.DirectorySeparatorChar;
        var self = Path.GetFullPath(AppContext.BaseDirectory);
        int files = 0;
        bool launcher = false;
        using var zip = ZipFile.OpenRead(zipPath);
        // check every entry before touching anything: names must stay inside the install
        var entries = new List<(ZipArchiveEntry Entry, string Target)>();
        foreach (var e in zip.Entries)
        {
            if (e.FullName.EndsWith('/') || e.FullName.EndsWith('\\')) continue;
            var name = e.FullName.Replace('\\', '/');
            if (name.Any(char.IsControl)) throw new InvalidDataException($"The patch has a broken file name ({Uri.EscapeDataString(name)}).");
            var target = Path.GetFullPath(Path.Combine(root, name));
            if (!target.StartsWith(root, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException($"The patch writes outside the install ({name}).");
            entries.Add((e, target));
        }
        foreach (var (e, target) in entries)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            bool ours = target.StartsWith(self, StringComparison.OrdinalIgnoreCase);
            if (File.Exists(target))
            {
                // Renamed, then the new one written: the running launcher's files, and any other file in use
                // (the launcher loads build\tu\SDL3.dll for controllers; overwriting it failed the 0.9.0 update).
                var old = target + ".old";
                if (File.Exists(old)) File.Delete(old);
                File.Move(target, old);
                if (ours) launcher = true;
            }
            var temp = target + ".new";
            e.ExtractToFile(temp, true);
            File.Move(temp, target, true);
            files++;
        }
        if (deleteZip) try { File.Delete(zipPath); } catch { }
        return new Applied(files, launcher);
    }

    /// <summary>Leftovers of an update (files renamed while in use): the launcher's, build\tu's and the install's own.</summary>
    public static void CleanUp()
    {
        foreach (var (dir, depth) in new[] { (AppContext.BaseDirectory, SearchOption.AllDirectories),
                                             (Path.Combine(Paths.Root, "build", "tu"), SearchOption.AllDirectories),
                                             (Paths.Root, SearchOption.TopDirectoryOnly) })
            try
            {
                foreach (var f in Directory.EnumerateFiles(dir, "*.old", depth))
                    try { File.Delete(f); } catch { }
                foreach (var f in Directory.EnumerateFiles(dir, "*.new", depth))
                    try { File.Delete(f); } catch { } // a write that failed (the 0.9.0 update's SDL3.dll.new)
            }
            catch { }
    }
}
