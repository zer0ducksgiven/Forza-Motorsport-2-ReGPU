using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace FM4Launcher;

/// <summary>Where things are: the install (fm4.exe, game\) and the user folder (Documents\fm4).</summary>
public static class Paths
{
    /// <summary>The install folder: the first folder up from the launcher that holds build\fm4\fm4.exe (a source
    /// checkout) or fm4.exe itself (an install).</summary>
    public static readonly string Root = FindRoot();
    public static string Exe => File.Exists(Path.Combine(Root, "fm4.exe")) ? Path.Combine(Root, "fm4.exe") : Path.Combine(Root, "build", "fm4", "fm4.exe");
    /// <summary>Linux: the Proton prefix the game runs in (as linux/release/play.sh picks it).</summary>
    public static readonly string CompatData = Environment.GetEnvironmentVariable("FM4_COMPATDATA") is { Length: > 0 } c ? c
        : Path.Combine(Environment.GetEnvironmentVariable("FM4_LINUX_HOME") is { Length: > 0 } h ? h
            : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".fm4-linux"), "compatdata");
    public static string GameDir => Path.Combine(Root, "game");
    public static readonly string User = FindUser();
    public static string SettingsFile => Path.Combine(User, "settings.ini");
    public static string Dlc => Path.Combine(User, "dlc");
    public static string DlcDisabled => Path.Combine(User, "dlc-disabled");

    /// <summary>Documents\fm4 in the Documents folder Windows reports (it may be moved off C:), as fm4.exe picks it
    /// (src/user_dir.cpp): if only the old %USERPROFILE%\Documents\fm4 exists, that one.</summary>
    static string FindUser()
    {
        // Linux: fm4.exe's Documents is the Proton prefix's.
        if (Platform.Linux) return Path.Combine(CompatData, "pfx", "drive_c", "users", "steamuser", "Documents", "fm4");
        var current = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "fm4");
        var old = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Documents", "fm4");
        return !Directory.Exists(current) && Directory.Exists(old) ? old : current;
    }

    static string FindRoot()
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
            if (File.Exists(Path.Combine(dir.FullName, "build", "fm4", "fm4.exe")) || File.Exists(Path.Combine(dir.FullName, "fm4.exe")))
                return dir.FullName;
        return Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, ".."));
    }
}

/// <summary>
/// Documents\fm4\settings.ini, the file fm4.exe reads (src/settings.cpp). Keys the launcher doesn't know are
/// kept as they are.
/// </summary>
public sealed class GameSettings
{
    public string Profile { get; set; } = "Player";
    public string Resolution { get; set; } = "1280x720";
    public string Aspect { get; set; } = "auto";
    public bool Fullscreen { get; set; }
    public bool Vsync { get; set; } = true;
    public int MaxFps { get; set; } = 60;
    public int Msaa { get; set; } = 1;
    public bool Enhance { get; set; } // enhanced lighting post pass (experimental)
    public int Language { get; set; } = 1;

    // What fm4.exe reads (src/settings.cpp). FM2's later options (SMAA, anisotropy, render scale, vertex arena, the
    // shader pop-up, music, wheel) have no counterpart in FM4's runtime yet, so they aren't offered.
    static readonly string[] Known = ["profile", "resolution", "aspect", "fullscreen", "vsync", "max_fps", "msaa", "upscaler", "enhance", "language"];

    public static GameSettings Load()
    {
        var s = new GameSettings();
        foreach (var (k, v) in Read())
            switch (k)
            {
                case "profile": s.Profile = v; break;
                case "resolution": s.Resolution = v; break;
                case "aspect": s.Aspect = v; break;
                case "fullscreen": s.Fullscreen = v == "1"; break;
                case "vsync": s.Vsync = v == "1"; break;
                case "max_fps": s.MaxFps = int.TryParse(v, out var f) ? f : 60; break;
                case "msaa": s.Msaa = int.TryParse(v, out var m) ? m : 1; break;
                case "enhance": s.Enhance = v == "1"; break;
                case "language": s.Language = int.TryParse(v, out var l) && l > 0 ? l : 1; break;
            }
        return s;
    }

    public void Save()
    {
        var keep = Read().Where(kv => !Known.Contains(kv.Key)).ToList();
        var profile = new string(Profile.Trim().Where(c => !char.IsControl(c) && c != '=').Take(15).ToArray());
        var sb = new StringBuilder("# Forza Motorsport 4 (fm4) settings; the launcher edits this file.\n");
        sb.Append($"profile={(profile.Length > 0 ? profile : "Player")}\n");
        sb.Append($"resolution={Resolution}\naspect={Aspect}\nfullscreen={B(Fullscreen)}\nvsync={B(Vsync)}\n");
        sb.Append($"max_fps={Math.Clamp(MaxFps, 30, 360)}\nmsaa={Msaa}\nenhance={B(Enhance)}\nlanguage={Language}\n");
        // Upscaling stays off: fm4.exe's own settings window doesn't offer it yet either (src/launcher.cpp).
        sb.Append("upscaler=off\n");
        foreach (var (k, v) in keep) sb.Append($"{k}={v}\n");
        Directory.CreateDirectory(Paths.User);
        File.WriteAllText(Paths.SettingsFile, sb.ToString());
    }

    static string B(bool b) => b ? "1" : "0";

    static List<KeyValuePair<string, string>> Read()
    {
        var list = new List<KeyValuePair<string, string>>();
        if (!File.Exists(Paths.SettingsFile)) return list;
        foreach (var raw in File.ReadAllLines(Paths.SettingsFile))
        {
            var line = raw.Trim();
            int eq = line.IndexOf('=');
            if (line.StartsWith('#') || eq <= 0) continue;
            list.Add(new(line[..eq].Trim(), line[(eq + 1)..].Trim()));
        }
        return list;
    }
}

/// <summary>Choices the Play page offers.</summary>
public static class Options
{
    public sealed record Language(int Id, string Code, string Name);

    // The console's language IDs (XGetLanguage) by the string table each should open; not traced in FM4 yet the way
    // FM2's were. The disc also has CZ, NL and HU tables, which no language ID names, and GB and MX variants of
    // English and Spanish.
    public static readonly Language[] Languages =
    [
        new(1, "EN", "English"), new(3, "DE", "Deutsch"), new(4, "FR", "Français"), new(5, "ES", "Español"),
        new(6, "IT", "Italiano"), new(9, "BR", "Português (Brasil)"), new(11, "PL", "Polski"), new(12, "RU", "Русский"),
        new(2, "JP", "日本語"), new(7, "KO", "한국어"), new(8, "CHT", "繁體中文"),
    ];

    /// <summary>The languages whose string table is in this install's game\Media\StringTables.</summary>
    public static IEnumerable<Language> InstalledLanguages()
    {
        var dir = Path.Combine(Paths.GameDir, "Media", "StringTables");
        if (!Directory.Exists(dir)) return Languages;
        var have = Directory.GetFiles(dir, "*.zip").Select(f => Path.GetFileNameWithoutExtension(f).ToUpperInvariant()).ToHashSet();
        return Languages.Where(l => have.Contains(l.Code));
    }

    public static readonly int[] Fps = [30, 60, 120, 144, 165, 240, 360];
    public static readonly (string Value, string Name)[] Aspects = [("auto", "Auto (match resolution)"), ("16:9", "16:9"), ("21:9", "21:9 (experimental)"), ("32:9", "32:9 (experimental)")];
    public static readonly (int Value, string Name)[] Msaa = [(1, "Off"), (2, "2x MSAA"), (4, "4x MSAA"), (8, "8x MSAA")];

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct DEVMODE
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
        public int dmFields, dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
        public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public short dmLogPixels;
        public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
        public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern bool EnumDisplaySettingsW(string? device, int mode, ref DEVMODE dm);

    /// <summary>The primary display's modes from 1280x720 up, smallest first (as the old launcher listed them).</summary>
    public static List<string> Resolutions()
    {
        var set = new HashSet<(int, int)>();
        if (Platform.Linux)
            foreach (var (w, h) in LinuxModes())
            {
                if (w >= 1280 && h >= 720) set.Add((w, h));
            }
        else
        {
            var dm = new DEVMODE { dmSize = (short)Marshal.SizeOf<DEVMODE>() };
            for (int i = 0; EnumDisplaySettingsW(null, i, ref dm); i++)
                if (dm.dmPelsWidth >= 1280 && dm.dmPelsHeight >= 720) set.Add((dm.dmPelsWidth, dm.dmPelsHeight));
        }
        if (set.Count == 0) set.Add((1280, 720));
        return set.OrderBy(m => (long)m.Item1 * m.Item2).Select(m => $"{m.Item1}x{m.Item2}").ToList();
    }

    // Linux: the connected outputs' modes from xrandr (X11 and XWayland list them alike); without it, the common
    // 16:9 sizes. Wine offers the game the same modes.
    static IEnumerable<(int, int)> LinuxModes()
    {
        var modes = new List<(int, int)>();
        try
        {
            var psi = new ProcessStartInfo("xrandr", "--query") { RedirectStandardOutput = true, UseShellExecute = false };
            using var p = Process.Start(psi)!;
            foreach (var line in p.StandardOutput.ReadToEnd().Split('\n'))
            {
                var m = System.Text.RegularExpressions.Regex.Match(line, @"^\s+(\d+)x(\d+)\s");
                if (m.Success) modes.Add((int.Parse(m.Groups[1].Value), int.Parse(m.Groups[2].Value)));
            }
            p.WaitForExit(2000);
        }
        catch { }
        return modes.Count > 0 ? modes : [(1280, 720), (1600, 900), (1920, 1080), (2560, 1440), (3840, 2160)];
    }
}

/// <summary>Starting the game, as play-fm4.cmd does: the DLC next to the install copied in first.</summary>
public static class Game
{
    public static bool Running => Platform.Linux ? RunningUnderWine() : Process.GetProcessesByName("fm4").Any(p =>
    {
        try { return string.Equals(p.MainModule?.FileName, Paths.Exe, StringComparison.OrdinalIgnoreCase); }
        catch { return true; } // can't look inside: assume it is ours
    });

    // Under Proton, fm4.exe is a wine process whose command line names it (as a Unix or a Z:\ path).
    static bool RunningUnderWine()
    {
        try
        {
            foreach (var dir in Directory.EnumerateDirectories("/proc"))
            {
                if (!int.TryParse(Path.GetFileName(dir), out var pid) || pid == Environment.ProcessId) continue;
                string cmdline;
                try { cmdline = File.ReadAllText(Path.Combine(dir, "cmdline")); } catch { continue; }
                var first = cmdline.Split('\0')[0];
                if (first.EndsWith("fm4.exe", StringComparison.OrdinalIgnoreCase)) return true;
            }
        }
        catch { }
        return false;
    }

    /// <summary>Linux: Proton as play.sh finds it: $PROTON, else Proton Experimental, 11 or 10 in Steam's libraries.</summary>
    public static string? Proton()
    {
        if (Environment.GetEnvironmentVariable("PROTON") is { Length: > 0 } set) return File.Exists(set) ? set : null;
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        foreach (var dir in new[] { ".local/share/Steam/steamapps/common", ".steam/steam/steamapps/common", ".var/app/com.valvesoftware.Steam/data/Steam/steamapps/common" })
            foreach (var name in new[] { "Proton - Experimental", "Proton 11.0", "Proton 10.0" })
            {
                var proton = Path.Combine(home, dir, name, "proton");
                if (File.Exists(proton)) return proton;
            }
        return null;
    }

    static ProcessStartInfo LinuxStart(string log)
    {
        var proton = Proton() ?? throw new InvalidOperationException("No Proton found: install Proton Experimental in Steam (Library, search \"Proton Experimental\"), or set PROTON to its proton script.");
        if (!File.Exists(Path.Combine(Paths.GameDir, "default.xex"))) throw new InvalidOperationException("No game files yet: extract your disc into the game folder first.");
        Directory.CreateDirectory(Paths.CompatData);
        var psi = new ProcessStartInfo("sh") { WorkingDirectory = Paths.Root, UseShellExecute = false };
        foreach (var a in new[] { "-c", "exec \"$0\" run \"$1\" \"$2\" > \"$3\" 2>&1", proton, Paths.Exe, Paths.GameDir, log }) psi.ArgumentList.Add(a);
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        psi.Environment["STEAM_COMPAT_CLIENT_INSTALL_PATH"] = Environment.GetEnvironmentVariable("STEAM_COMPAT_CLIENT_INSTALL_PATH") is { Length: > 0 } s ? s : Path.Combine(home, ".steam", "steam");
        psi.Environment["STEAM_COMPAT_DATA_PATH"] = Paths.CompatData;
        // Wine's own d3dcompiler_47 can't compile the renderer's shaders: the Microsoft DLL beside fm4.exe instead.
        var overrides = Environment.GetEnvironmentVariable("WINEDLLOVERRIDES");
        psi.Environment["WINEDLLOVERRIDES"] = "d3dcompiler_47=n" + (string.IsNullOrEmpty(overrides) ? "" : ";" + overrides);
        return psi;
    }

    public static void Launch(bool coldStart = false)
    {
        if (!File.Exists(Paths.Exe)) throw new InvalidOperationException($"fm4.exe isn't at {Paths.Exe}.");
        CopyMissing(Path.Combine(Paths.Root, "dlc"), Paths.Dlc, Paths.DlcDisabled);
        Directory.CreateDirectory(Path.Combine(Paths.Root, "out"));
        var log = Path.Combine(Paths.Root, "out", "play-fm4.log");
        var psi = Platform.Linux ? LinuxStart(log) : new ProcessStartInfo("cmd.exe", $"/c \"\"{Paths.Exe}\" > \"{log}\" 2>&1\"")
        {
            WorkingDirectory = Paths.Root,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        psi.Environment["FM2_NO_LAUNCHER"] = "1"; // the settings come from settings.ini, saved by this launcher
        if (coldStart) psi.Environment["FM2_PERF"] = "1";
        // As play-fm4.cmd: fences written when recorded, and resolves up to 1024 pixels written back to memory (the
        // road surface and the photo mode tint read them).
        psi.Environment["REGPU_EARLY_FENCES"] = "1";
        psi.Environment["REGPU_RESOLVE_WRITEBACK_MAX"] = "1024";
        var game = Process.Start(psi);
        if (game is null) return;
        // Under Proton the game's stderr never reaches the log (only Proton's own lines do): add its fm4.log.
        if (Platform.Linux)
        {
            game.EnableRaisingEvents = true;
            game.Exited += (_, _) =>
            {
                try { File.AppendAllText(log, File.ReadAllText(Path.Combine(Paths.Root, "fm4.log"))); } catch (IOException) { }
            };
        }
        // The session's log goes up as soon as the game closes (not only at the launcher's next start)
        _ = Reports.SendAfterExitAsync(game);
    }

    // robocopy /XC /XN /XO in install-content.cmd: what is already there is left alone. A DLC package the game has
    // unpacked is a folder of the same name; one switched off sits in dlc-disabled.
    static void CopyMissing(string from, string to, string? alsoIn = null)
    {
        if (!Directory.Exists(from)) return;
        foreach (var file in Directory.EnumerateFiles(from, "*", SearchOption.AllDirectories))
        {
            var rel = Path.GetRelativePath(from, file);
            var dest = Path.Combine(to, rel);
            var top = rel.Split(Path.DirectorySeparatorChar)[0]; // the package (or the file itself)
            bool Has(string dir) => File.Exists(Path.Combine(dir, top)) || Directory.Exists(Path.Combine(dir, top));
            if (File.Exists(dest) || Directory.Exists(dest)) continue;
            if (top == rel && Has(to)) continue;                         // a package the game unpacked into a folder
            if (alsoIn is not null && Has(alsoIn)) continue;             // switched off
            Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
            File.Copy(file, dest);
        }
    }
}

/// <summary>The shader and pipeline caches in Documents\fm4 (what play-cold.cmd moves aside).</summary>
public static class ShaderCache
{
    public sealed record Info(bool Exists, int Shaders, long Bytes, DateTime? Built);

    static readonly string[] Dirs = ["shadercache", "shaders"];
    static readonly string[] Files = ["pipelines.bin", "pipelines.manifest", "static_shaders.bin"];

    public static Info Get()
    {
        long bytes = 0;
        int count = 0;
        DateTime? built = null;
        foreach (var d in Dirs.Select(d => Path.Combine(Paths.User, d)).Where(Directory.Exists))
            foreach (var f in new DirectoryInfo(d).EnumerateFiles("*", SearchOption.AllDirectories))
            {
                bytes += f.Length;
                if (f.Extension is ".dxbc" or ".ucode") count++;
            }
        foreach (var f in Files.Select(f => new FileInfo(Path.Combine(Paths.User, f))).Where(f => f.Exists))
        {
            bytes += f.Length;
            if (f.Name == "pipelines.bin") built = f.LastWriteTime;
        }
        return new Info(bytes > 0, count, bytes, built);
    }

    /// <summary>Deletes the caches; the next start rebuilds them (several minutes).</summary>
    public static void Delete()
    {
        if (Game.Running) throw new InvalidOperationException("Close the game first: it is using the shader cache.");
        foreach (var d in Dirs.Select(d => Path.Combine(Paths.User, d)).Where(Directory.Exists)) Directory.Delete(d, true);
        foreach (var f in Files.Select(f => Path.Combine(Paths.User, f)).Where(File.Exists)) File.Delete(f);
    }
}

/// <summary>DLC packages in Documents\fm4\dlc (disabled ones wait in dlc-disabled). A package is a file or an unpacked
/// folder, with a .title (its name, UTF-16) and a .license beside it.</summary>
public static class Dlc
{
    public sealed record Pack(string Id, string Name, bool Enabled, long Bytes);

    // FM2's mods (marked by a .mod file) share this folder there; FM4 has none, so anything marked so is left out.
    public static List<Pack> List() => ListAll(mods: false);

    static List<Pack> ListAll(bool mods)
    {
        var packs = new List<Pack>();
        foreach (var (dir, enabled) in new[] { (Paths.Dlc, true), (Paths.DlcDisabled, false) })
        {
            if (!Directory.Exists(dir)) continue;
            foreach (var entry in new DirectoryInfo(dir).EnumerateFileSystemInfos())
            {
                if (entry.Extension is ".title" or ".license" or ".mod") continue;
                if (File.Exists(Path.Combine(dir, entry.Name + ".mod")) != mods) continue;
                var titleFile = Path.Combine(dir, entry.Name + ".title");
                var name = File.Exists(titleFile) ? Encoding.Unicode.GetString(File.ReadAllBytes(titleFile)).TrimEnd('\0').Trim() : entry.Name;
                long bytes = entry is FileInfo fi ? fi.Length
                    : ((DirectoryInfo)entry).EnumerateFiles("*", SearchOption.AllDirectories).Sum(f => f.Length);
                packs.Add(new Pack(entry.Name, name.Length > 0 ? name : entry.Name, enabled, bytes));
            }
        }
        return packs.OrderBy(p => p.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
    }

    public static void SetEnabled(string id, bool enabled)
    {
        if (id.Contains('/') || id.Contains('\\') || id.Contains("..")) throw new ArgumentException("Not a package name.");
        if (Game.Running) throw new InvalidOperationException("Close the game first: DLC is read while it runs.");
        var (from, to) = enabled ? (Paths.DlcDisabled, Paths.Dlc) : (Paths.Dlc, Paths.DlcDisabled);
        Directory.CreateDirectory(to);
        foreach (var name in new[] { id, id + ".title", id + ".license", id + ".mod" })
        {
            var src = Path.Combine(from, name);
            var dst = Path.Combine(to, name);
            if (Directory.Exists(src)) Directory.Move(src, dst);
            else if (File.Exists(src)) File.Move(src, dst, true);
        }
    }

    /// <summary>Installs packages (as downloaded from Xbox Live): unpacked into the DLC folder the way the game reads
    /// them.</summary>
    public static (int Added, List<string> Errors) Add(IEnumerable<string> files)
    {
        if (Game.Running) throw new InvalidOperationException("Close the game first: DLC is read while it runs.");
        Directory.CreateDirectory(Paths.Dlc);
        int added = 0;
        var errors = new List<string>();
        foreach (var f in files.Where(File.Exists))
        {
            try
            {
                Stfs.Install(f, Paths.Dlc, false);
                added++;
            }
            catch (Exception ex) { errors.Add($"{Path.GetFileName(f)}: {ex.Message}"); }
        }
        return (added, errors);
    }
}
