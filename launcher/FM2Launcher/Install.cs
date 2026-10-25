using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace FM2Launcher;

/// <summary>Where things are: the install (build\tu\fm2.exe, game\) and the user folder (Documents\fm2).</summary>
public static class Paths
{
    /// <summary>The install folder: the first folder up from the launcher that holds build\tu\fm2.exe.</summary>
    public static readonly string Root = FindRoot();
    public static string Exe => Path.Combine(Root, "build", "tu", "fm2.exe");
    public static string GameDir => Path.Combine(Root, "game");
    public static readonly string User = FindUser();
    public static string SettingsFile => Path.Combine(User, "settings.ini");
    public static string Dlc => Path.Combine(User, "dlc");
    public static string DlcDisabled => Path.Combine(User, "dlc-disabled");
    public static string Update => Path.Combine(User, "update");

    /// <summary>Documents\fm2 in the Documents folder Windows reports (it may be moved off C:), as fm2.exe picks it
    /// (src/user_dir.cpp): if only the old %USERPROFILE%\Documents\fm2 exists, that one.</summary>
    static string FindUser()
    {
        var current = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "fm2");
        var old = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Documents", "fm2");
        return !Directory.Exists(current) && Directory.Exists(old) ? old : current;
    }

    static string FindRoot()
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
            if (File.Exists(Path.Combine(dir.FullName, "build", "tu", "fm2.exe")))
                return dir.FullName;
        return Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, ".."));
    }
}

/// <summary>
/// Documents\fm2\settings.ini, the file fm2.exe reads (src/settings.cpp). Keys the launcher doesn't know are
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
    public int Anisotropy { get; set; } = 16;
    // FSR 1 on the whole frame: render scale (percent per axis) and RCAS sharpening (0 off, 100 max).
    public int RenderScale { get; set; } = 100;
    public int Sharpening { get; set; }
    public bool Enhance { get; set; }
    public bool SkipIntro { get; set; }
    public bool LowPerformance { get; set; }
    public int WheelStrength { get; set; } = 50;
    public int WheelSpring { get; set; } = 50;
    public bool WheelReverse { get; set; }
    public int WheelRotation { get; set; }
    public int Language { get; set; } = 1;

    static readonly string[] Known = ["profile", "resolution", "aspect", "fullscreen", "vsync", "max_fps", "msaa", "anisotropy", "upscaler", "render_scale", "sharpness", "enhance", "skip_intro", "low_performance", "wheel_strength", "wheel_spring", "wheel_reverse", "wheel_rotation", "language"];

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
                case "anisotropy": s.Anisotropy = int.TryParse(v, out var an) ? an : 16; break;
                case "render_scale": s.RenderScale = int.TryParse(v, out var rs) ? rs : 100; break;
                case "sharpness":
                    s.Sharpening = double.TryParse(v, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var sh)
                        ? (int)Math.Round(sh * 100) : 0;
                    break;
                case "enhance": s.Enhance = v == "1"; break;
                case "skip_intro": s.SkipIntro = v == "1"; break;
                case "low_performance": s.LowPerformance = v == "1"; break;
                case "wheel_strength": s.WheelStrength = int.TryParse(v, out var ws) ? ws : 50; break;
                case "wheel_spring": s.WheelSpring = int.TryParse(v, out var wp) ? wp : 50; break;
                case "wheel_reverse": s.WheelReverse = v == "1"; break;
                case "wheel_rotation": s.WheelRotation = int.TryParse(v, out var wr) ? wr : 0; break;
                case "language": s.Language = int.TryParse(v, out var l) && l > 0 ? l : 1; break;
            }
        return s;
    }

    public void Save()
    {
        var keep = Read().Where(kv => !Known.Contains(kv.Key)).ToList();
        var profile = new string(Profile.Trim().Where(c => !char.IsControl(c) && c != '=').Take(15).ToArray());
        var sb = new StringBuilder("# Forza Motorsport 2 (fm2) settings; the launcher edits this file.\n");
        sb.Append($"profile={(profile.Length > 0 ? profile : "Player")}\n");
        sb.Append($"resolution={Resolution}\naspect={Aspect}\nfullscreen={B(Fullscreen)}\nvsync={B(Vsync)}\n");
        sb.Append($"max_fps={Math.Clamp(MaxFps, 30, 360)}\nmsaa={Msaa}\nanisotropy={Anisotropy}\nenhance={B(Enhance)}\nskip_intro={B(SkipIntro)}\nlow_performance={B(LowPerformance)}\nlanguage={Language}\n");
        sb.Append($"wheel_strength={Math.Clamp(WheelStrength, 0, 100)}\nwheel_spring={Math.Clamp(WheelSpring, 0, 100)}\nwheel_reverse={B(WheelReverse)}\nwheel_rotation={Math.Clamp(WheelRotation, 0, 3600)}\n");
        var scale = Math.Clamp(RenderScale, 50, 100);
        var sharp = Math.Clamp(Sharpening, 0, 100);
        // FSR 3 and DLSS are set by hand for testing; the launcher keeps them.
        var current = Read().LastOrDefault(kv => kv.Key == "upscaler").Value;
        var upscaler = current is "fsr3" or "dlss" ? current : scale < 100 || sharp > 0 ? "fsr1" : "off";
        sb.Append($"upscaler={upscaler}\nrender_scale={scale}\nsharpness={(sharp / 100.0).ToString(System.Globalization.CultureInfo.InvariantCulture)}\n");
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

    // XGetLanguage values the game maps to its string tables (checked by tracing the zip it opens). The disc also
    // has PL, CZ and HU tables, which the game doesn't reach through the language ID.
    public static readonly Language[] Languages =
    [
        new(1, "EN", "English"), new(3, "DE", "Deutsch"), new(4, "FR", "Français"), new(5, "ES", "Español"),
        new(6, "IT", "Italiano"), new(2, "JP", "日本語"), new(7, "KO", "한국어"), new(10, "CHS", "简体中文"), new(8, "CHT", "繁體中文"),
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
    public static readonly (int Value, string Name)[] RenderScale = [(100, "100% (native)"), (85, "85%"), (77, "77%"), (67, "67%"), (59, "59%"), (50, "50%")];
    public static readonly (int Value, string Name)[] Sharpening = [(0, "Off"), (25, "Low"), (50, "Medium"), (75, "High"), (100, "Maximum")];
    public static readonly (int Value, string Name)[] Anisotropy = [(0, "Off (trilinear)"), (2, "2x"), (4, "4x"), (8, "8x"), (16, "16x")];

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
        var dm = new DEVMODE { dmSize = (short)Marshal.SizeOf<DEVMODE>() };
        for (int i = 0; EnumDisplaySettingsW(null, i, ref dm); i++)
            if (dm.dmPelsWidth >= 1280 && dm.dmPelsHeight >= 720) set.Add((dm.dmPelsWidth, dm.dmPelsHeight));
        if (set.Count == 0) set.Add((1280, 720));
        return set.OrderBy(m => (long)m.Item1 * m.Item2).Select(m => $"{m.Item1}x{m.Item2}").ToList();
    }
}

/// <summary>Starting the game, as play.cmd does: the title update and DLC next to the install copied in first.</summary>
public static class Game
{
    public static bool Running => Process.GetProcessesByName("fm2").Any(p =>
    {
        try { return string.Equals(p.MainModule?.FileName, Paths.Exe, StringComparison.OrdinalIgnoreCase); }
        catch { return true; } // can't look inside: assume it is ours
    });

    public static void Launch(bool coldStart = false)
    {
        if (!File.Exists(Paths.Exe)) throw new InvalidOperationException($"fm2.exe isn't at {Paths.Exe}.");
        CopyMissing(Path.Combine(Paths.Root, "update"), Paths.Update);
        CopyMissing(Path.Combine(Paths.Root, "dlc"), Paths.Dlc, Paths.DlcDisabled);
        Directory.CreateDirectory(Path.Combine(Paths.Root, "out"));
        var log = Path.Combine(Paths.Root, "out", "play.log");
        var psi = new ProcessStartInfo("cmd.exe", $"/c \"\"{Paths.Exe}\" > \"{log}\" 2>&1\"")
        {
            WorkingDirectory = Paths.Root,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        psi.Environment["FM2_NO_LAUNCHER"] = "1"; // the settings come from settings.ini, saved by this launcher
        if (coldStart) psi.Environment["FM2_PERF"] = "1";
        var game = Process.Start(psi);
        if (game is null) return;
        // The session's play.log goes as soon as the game closes (with sharing on), not only at the next
        // launcher start: most people keep the launcher open while they play.
        game.EnableRaisingEvents = true;
        game.Exited += (_, _) => _ = Reports.SendLastLogAsync();
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

/// <summary>The shader and pipeline caches in Documents\fm2 (what play-cold.cmd moves aside).</summary>
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

/// <summary>DLC packages in Documents\fm2\dlc (disabled ones wait in dlc-disabled). A package is a file or an unpacked
/// folder, with a .title (its name, UTF-16) and a .license beside it.</summary>
public static class Dlc
{
    public sealed record Pack(string Id, string Name, bool Enabled, long Bytes);

    // Mods (backported cars in the same package format) live beside the DLC, marked by a .mod file, listed apart.
    public static List<Pack> List() => ListAll(mods: false);
    public static List<Pack> Mods() => ListAll(mods: true);

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

    /// <summary>Installs packages (as downloaded from Xbox Live, or backported mods): unpacked into the DLC folder the
    /// way the game reads them. Before, the package file was only copied there and the game never saw it.</summary>
    public static (int Added, List<string> Errors) Add(IEnumerable<string> files, bool mods)
    {
        if (Game.Running) throw new InvalidOperationException("Close the game first: DLC is read while it runs.");
        Directory.CreateDirectory(Paths.Dlc);
        int added = 0;
        var errors = new List<string>();
        foreach (var f in files.Where(File.Exists))
        {
            try
            {
                Stfs.Install(f, Paths.Dlc, mods);
                added++;
            }
            catch (Exception ex) { errors.Add($"{Path.GetFileName(f)}: {ex.Message}"); }
        }
        return (added, errors);
    }

    /// <summary>Deletes an installed pack and its files (the Mods panel's Remove).</summary>
    public static void Remove(string id)
    {
        if (id.Contains('/') || id.Contains('\\') || id.Contains("..")) throw new ArgumentException("Not a package name.");
        if (Game.Running) throw new InvalidOperationException("Close the game first: DLC is read while it runs.");
        foreach (var dir in new[] { Paths.Dlc, Paths.DlcDisabled })
            foreach (var name in new[] { id, id + ".title", id + ".license", id + ".mod" })
            {
                var p = Path.Combine(dir, name);
                if (Directory.Exists(p)) Directory.Delete(p, true);
                else if (File.Exists(p)) File.Delete(p);
            }
    }
}
