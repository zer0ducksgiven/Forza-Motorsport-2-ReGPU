using System.Diagnostics;
using System.Text;

namespace FM2Launcher;

/// <summary>
/// Random Events, the career mod (tools/modkit/career/random_events.py, shipped beside the launcher in career\): four
/// generated career events that change every two career races, and a random car every fifth Random Events race
/// (fm2.exe, hooks/random_events.cpp). The mod holds the game's own string tables, so it's built here from the
/// player's game files rather than shipped: the Mods panel's first listing builds it into dlc-disabled as a mod
/// (Dlc.InstallBundled; switched on there like the others), and before each launch Rotate rebuilds an enabled one when the profile's set is due
/// (races / 2 from profiles\&lt;name&gt;\random_events.ini, which fm2.exe keeps).
/// </summary>
public static class RandomEvents
{
    public const string Id = "RandomEvents";
    const string Folder = Id;

    static string Script => Path.Combine(AppContext.BaseDirectory, "career", "random_events.py");
    static string Python => Platform.Linux ? "python3"
        : File.Exists(Path.Combine(Paths.Root, "py", "python.exe")) ? Path.Combine(Paths.Root, "py", "python.exe") : "py";
    static string GameDb => Path.Combine(Paths.GameDir, "Media", "DB", "gamedb.slt");
    static string StringTables => Path.Combine(Paths.GameDir, "Media", "StringTables");

    /// <summary>The installed pack's DLCZips folder (enabled first), or null.</summary>
    static string? Installed(out bool enabled)
    {
        foreach (var (dir, on) in new[] { (Paths.Dlc, true), (Paths.DlcDisabled, false) })
        {
            var zips = Path.Combine(dir, Folder, "Media", "DLCZips");
            if (File.Exists(Path.Combine(zips, "RandomEvents_pri_73.zip")))
            {
                enabled = on;
                return zips;
            }
        }
        enabled = false;
        return null;
    }

    public static bool IsInstalled => Installed(out _) is not null;

    public static bool Available => File.Exists(Script) && File.Exists(GameDb);

    /// <summary>Builds the mod (switched off) from the game files, for the Mods panel.</summary>
    public static void Install()
    {
        if (Game.Running) throw new InvalidOperationException("Close the game first.");
        if (!Available) throw new InvalidOperationException("Random Events needs the game files (game\\Media\\DB) and the launcher's career folder.");
        var zips = Installed(out _) ?? Path.Combine(Paths.DlcDisabled, Folder, "Media", "DLCZips");
        Build(zips, 0, Seed(GameSettings.Load().Profile));
        var dir = Path.GetDirectoryName(Path.GetDirectoryName(zips))!;
        File.WriteAllBytes(dir + ".title", Encoding.Unicode.GetBytes("Random Events"));
        File.WriteAllText(dir + ".license", "FFFFFFFF\r\n");
        File.WriteAllText(dir + ".mod", "");
    }

    /// <summary>Before a launch: the profile's set of events, rebuilt if it has moved on (two races per set).</summary>
    public static void Rotate()
    {
        var zips = Installed(out var enabled);
        if (zips is null || !enabled || !Available) return;
        var profile = GameSettings.Load().Profile;
        var ini = Path.Combine(Paths.User, "profiles", profile, "random_events.ini");
        uint races = 0;
        if (File.Exists(ini))
            foreach (var line in File.ReadAllLines(ini))
                if (line.StartsWith("races=")) uint.TryParse(line[6..], out races);
        var due = races / 2;
        var have = File.Exists(Path.Combine(zips, "RandomEventsRotation.txt")) && uint.TryParse(File.ReadAllText(Path.Combine(zips, "RandomEventsRotation.txt")).Trim(), out var h) ? h : uint.MaxValue;
        if (have == due) return;
        // A failed rebuild keeps the old set and doesn't stop the game: it's noted for bug reports.
        try { Build(zips, due, Seed(profile)); }
        catch (Exception ex) { File.WriteAllText(Path.Combine(Paths.User, "random_events_error.txt"), $"set {due}: {ex.Message}\n"); }
    }

    // Each profile has its own run of events.
    static uint Seed(string profile)
    {
        uint h = 2166136261;
        foreach (var c in profile.ToLowerInvariant()) h = (h ^ c) * 16777619;
        return h % 1000000;
    }

    static void Build(string zips, uint rotation, uint seed)
    {
        var work = Path.Combine(Path.GetTempPath(), "fm2-random-events");
        if (Directory.Exists(work)) Directory.Delete(work, true);
        var psi = new ProcessStartInfo(Python) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var a in new[] { "-I", Script, GameDb, StringTables, work, "--rotation", rotation.ToString(), "--seed", seed.ToString(), "--install", zips })
            psi.ArgumentList.Add(a);
        using var p = Process.Start(psi) ?? throw new InvalidOperationException("Python didn't start.");
        var output = p.StandardOutput.ReadToEndAsync();
        var error = p.StandardError.ReadToEnd();
        p.WaitForExit();
        if (p.ExitCode != 0) throw new InvalidOperationException(error.Trim().Split('\n').LastOrDefault() ?? "the generator failed");
        _ = output.Result;
    }
}
