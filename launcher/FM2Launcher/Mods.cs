namespace FM2Launcher;

/// <summary>
/// Forza Plus 2 ("Project Forza Plus 2", a car and career overhaul made for Xenia), installed as a mod: its files go to
/// Documents\fm2\mods\ForzaPlus2\game\Media, which fm2.exe reads in place of the game's (src/kernel/imports_io.cpp).
/// Switched off, the folder is _ForzaPlus2 (fm2.exe skips folders starting with '_'). Its career modes are different
/// gamedb.slt files: all four are kept in the mod folder so the mode can change without the download.
/// </summary>
public static class ForzaPlus2
{
    const string Name = "ForzaPlus2";
    static string On => Path.Combine(Paths.User, "mods", Name);
    static string Off => Path.Combine(Paths.User, "mods", "_" + Name);
    static string Dir => Directory.Exists(On) ? On : Off;
    static string Settings => Path.Combine(Dir, "forzaplus2.ini");
    public static readonly string[] Modes = ["Normal", "Hard", "Sandbox", "Vanilla"];

    public static object State()
    {
        var installed = File.Exists(Path.Combine(Dir, "game", "Media", "DB", "gamedb.slt"));
        var (mode, harder) = installed ? ReadSettings() : ("Normal", false);
        return new { installed, enabled = installed && Directory.Exists(On), mode, harderAi = harder, modes = Modes };
    }

    /// <summary>Copies the mod from its unzipped folder (the one with "Main Game Changes" in it).</summary>
    public static void Install(string source, Action<double, string> progress)
    {
        var main = Path.Combine(source, "Main Game Changes", "Media");
        var dlc = Path.Combine(source, "DLC Cars and Tracks", "Media");
        var modes = Path.Combine(source, "Game Mode DBs");
        if (!Directory.Exists(main) || !Directory.Exists(modes))
            throw new InvalidOperationException("That isn't the Forza Plus 2 folder: choose the unzipped folder with \"Main Game Changes\" and \"Game Mode DBs\" in it.");
        if (Game.Running) throw new InvalidOperationException("Close the game first.");
        DeleteTree(On);
        DeleteTree(Off);
        var media = Path.Combine(On, "game", "Media");
        // The DLC media first, then the main changes over it (as the mod's instructions copy them).
        var jobs = new List<(string From, string To)>();
        if (Directory.Exists(dlc)) jobs.AddRange(Files(dlc, media));
        jobs.AddRange(Files(main, media));
        jobs.AddRange(Files(modes, Path.Combine(On, "modes")));
        long total = jobs.Sum(j => new FileInfo(j.From).Length), done = 0;
        var shown = DateTime.MinValue;
        foreach (var (from, to) in jobs)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(to)!);
            File.Copy(from, to, true);
            File.SetAttributes(to, FileAttributes.Normal); // the mod's files come read-only (off a disc image)
            done += new FileInfo(from).Length;
            // About ten updates a second: 15,000 files would otherwise queue 15,000 messages for the page.
            if (DateTime.UtcNow - shown > TimeSpan.FromMilliseconds(100))
            {
                shown = DateTime.UtcNow;
                progress(total > 0 ? (double)done / total : 1, $"Copying Forza Plus 2: {done >> 20} of {total >> 20} MB");
            }
        }
        SetMode("Normal", false);
    }

    static IEnumerable<(string, string)> Files(string from, string to) =>
        Directory.EnumerateFiles(from, "*", SearchOption.AllDirectories).Select(f => (f, Path.Combine(to, Path.GetRelativePath(from, f))));

    /// <summary>The career mode: that mode's gamedb.slt (or its Harder AI one) becomes the mod's database.</summary>
    public static void SetMode(string mode, bool harderAi)
    {
        if (!Modes.Contains(mode)) throw new ArgumentException("Unknown mode.");
        if (Game.Running) throw new InvalidOperationException("Close the game first: the mode is read when it starts.");
        var from = Path.Combine(Dir, "modes", mode + " Mode", harderAi ? "Harder AI" : "", "gamedb.slt");
        if (!File.Exists(from)) throw new InvalidOperationException($"The mod's {mode} Mode database is missing: install Forza Plus 2 again.");
        var db = Path.Combine(Dir, "game", "Media", "DB", "gamedb.slt");
        if (File.Exists(db)) File.SetAttributes(db, FileAttributes.Normal);
        File.Copy(from, db, true);
        File.SetAttributes(db, FileAttributes.Normal);
        File.WriteAllText(Settings, $"mode={mode}\nharder_ai={(harderAi ? 1 : 0)}\n");
    }

    public static void SetEnabled(bool enabled)
    {
        if (Game.Running) throw new InvalidOperationException("Close the game first.");
        if (enabled && Directory.Exists(Off) && !Directory.Exists(On)) Directory.Move(Off, On);
        if (!enabled && Directory.Exists(On) && !Directory.Exists(Off)) Directory.Move(On, Off);
    }

    public static void Remove()
    {
        if (Game.Running) throw new InvalidOperationException("Close the game first.");
        DeleteTree(On);
        DeleteTree(Off);
    }

    // Read-only files (copied from the mod as they came, by older launchers) stop Directory.Delete.
    static void DeleteTree(string dir)
    {
        if (!Directory.Exists(dir)) return;
        foreach (var f in Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories))
            File.SetAttributes(f, FileAttributes.Normal);
        Directory.Delete(dir, true);
    }

    static (string Mode, bool Harder) ReadSettings()
    {
        string mode = "Normal";
        var harder = false;
        if (File.Exists(Settings))
            foreach (var line in File.ReadAllLines(Settings))
            {
                var kv = line.Split('=', 2);
                if (kv.Length != 2) continue;
                if (kv[0] == "mode" && Modes.Contains(kv[1].Trim())) mode = kv[1].Trim();
                if (kv[0] == "harder_ai") harder = kv[1].Trim() == "1";
            }
        return (mode, harder);
    }
}
