using System.Diagnostics;
using System.Text;

namespace FM2Launcher;

/// <summary>
/// The career mods built by tools/modkit/career/random_events.py (shipped beside the launcher in career\). They hold
/// the game's own string tables, so they're built here from the player's game files rather than shipped: the Mods
/// panel's first listing builds each into dlc-disabled (Dlc.InstallBundled; switched on there like the others).
/// <list type="bullet">
/// <item>Random Events: four generated career events that change every two career races, and a random car every
/// fifth Random Events race (fm2.exe, hooks/random_events.cpp). Before each launch Rotate rebuilds an enabled one
/// when the profile's set is due (races / 2 from profiles\&lt;name&gt;\random_events.ini, which fm2.exe keeps).</item>
/// <item>Rogue Mode, Roguelike Career (FM2-roguelike-scope.md): a career run on a fresh profile, parts and cars from
/// wins, strikes that lose cars and no prize money (hooks/roguelike.cpp). Its package also carries the career flows it
/// changes. Settings has its own switch for it (rogue.toggle), so the Mods list leaves it out.</item>
/// </list>
/// They share the career's machinery, so only one is on at a time (Dlc.SetEnabled).
/// </summary>
public sealed class CareerMod
{
    public static readonly CareerMod RandomEvents = new("RandomEvents", "Random Events", "random");
    public static readonly CareerMod Roguelike = new("Roguelike", "Rogue Mode", "roguelike");
    public static readonly CareerMod[] All = [RandomEvents, Roguelike];

    public string Id { get; }
    readonly string _title, _mode;

    CareerMod(string id, string title, string mode) => (Id, _title, _mode) = (id, title, mode);

    /// <summary>The career mod this one can't be on with.</summary>
    public CareerMod Other => this == RandomEvents ? Roguelike : RandomEvents;

    static string Script => Path.Combine(AppContext.BaseDirectory, "career", "random_events.py");
    static string Python => Platform.Linux ? "python3"
        : File.Exists(Path.Combine(Paths.Root, "py", "python.exe")) ? Path.Combine(Paths.Root, "py", "python.exe") : "py";
    static string GameDb => Path.Combine(Paths.GameDir, "Media", "DB", "gamedb.slt");
    static string StringTables => Path.Combine(Paths.GameDir, "Media", "StringTables");

    /// <summary>The installed pack's DLCZips folder (enabled first), or null.</summary>
    string? Installed(out bool enabled)
    {
        foreach (var (dir, on) in new[] { (Paths.Dlc, true), (Paths.DlcDisabled, false) })
        {
            var zips = Path.Combine(dir, Id, "Media", "DLCZips");
            if (File.Exists(Path.Combine(zips, Id + "_pri_73.zip")))
            {
                enabled = on;
                return zips;
            }
        }
        enabled = false;
        return null;
    }

    public bool IsInstalled => Installed(out _) is not null;
    public bool IsEnabled => Installed(out var on) is not null && on;

    public static bool Available => File.Exists(Script) && File.Exists(GameDb);

    /// <summary>Builds the mod (switched off) from the game files, for the Mods panel.</summary>
    public void Install()
    {
        if (Game.Running) throw new InvalidOperationException("Close the game first.");
        if (!Available) throw new InvalidOperationException(_title + " needs the game files (game\\Media\\DB) and the launcher's career folder.");
        var zips = Installed(out _) ?? Path.Combine(Paths.DlcDisabled, Id, "Media", "DLCZips");
        Build(zips, 0, Seed(GameSettings.Load().Profile));
        var dir = Path.GetDirectoryName(Path.GetDirectoryName(zips))!;
        File.WriteAllBytes(dir + ".title", Encoding.Unicode.GetBytes(_title));
        File.WriteAllText(dir + ".license", "FFFFFFFF\r\n");
        File.WriteAllText(dir + ".mod", "");
    }

    /// <summary>Before a launch (Random Events): the profile's set of events, rebuilt if it has moved on (two races
    /// per set).</summary>
    public void Rotate()
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
        var rotation = Path.Combine(zips, Id + "Rotation.txt");
        var have = File.Exists(rotation) && uint.TryParse(File.ReadAllText(rotation).Trim(), out var h) ? h : uint.MaxValue;
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

    void Build(string zips, uint rotation, uint seed)
    {
        var work = Path.Combine(Path.GetTempPath(), "fm2-" + Id.ToLowerInvariant());
        if (Directory.Exists(work)) Directory.Delete(work, true);
        var psi = new ProcessStartInfo(Python) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var a in new[] { "-I", Script, GameDb, StringTables, work, "--rotation", rotation.ToString(), "--seed", seed.ToString(),
                                  "--mode", _mode, "--install", zips })
            psi.ArgumentList.Add(a);
        using var p = Process.Start(psi) ?? throw new InvalidOperationException("Python didn't start.");
        var output = p.StandardOutput.ReadToEndAsync();
        var error = p.StandardError.ReadToEnd();
        p.WaitForExit();
        if (p.ExitCode != 0) throw new InvalidOperationException(error.Trim().Split('\n').LastOrDefault() ?? "the generator failed");
        _ = output.Result;
    }
}
