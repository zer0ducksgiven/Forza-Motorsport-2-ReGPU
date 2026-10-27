using System.Text;

namespace FM2Launcher;

/// <summary>
/// Documents\fm2\wheel_bindings.ini, read by the game when its wheel thread starts (src/input/wheel.cpp):
/// for each pad input, the device inputs that drive it as "&lt;SDL GUID&gt;/&lt;source&gt;" joined by commas.
/// A source is an axis (a5, a0- reversed), a button (b12) or a hat direction (h0u, h0d, h0l, h0r); keys are
/// "keyboard/k&lt;virtual key&gt;" (the game reads those itself, src/kernel/imports_xam.cpp), steering by key as steerleft/steerright.
/// Pad inputs left out keep the game's defaults.
/// </summary>
public static class WheelBindings
{
    public static readonly string[] Targets =
        ["steer", "steerleft", "steerright", "throttle", "brake", "a", "b", "x", "y", "lb", "rb", "back", "start", "ls", "rs", "up", "down", "left", "right"];

    static string File => Path.Combine(Paths.User, "wheel_bindings.ini");

    /// <summary>Target to its sources, and the device names remembered for the GUIDs.</summary>
    public static (Dictionary<string, List<string>> Map, Dictionary<string, string> Names) Load()
    {
        var map = Targets.ToDictionary(t => t, _ => new List<string>());
        var names = new Dictionary<string, string>();
        if (!System.IO.File.Exists(File)) return (map, names);
        foreach (var raw in System.IO.File.ReadAllLines(File))
        {
            var line = raw.Trim();
            // "# device <guid> <name>": the name a GUID had when bound (the window shows it for unplugged devices)
            if (line.StartsWith("# device "))
            {
                var parts = line[9..].Split(' ', 2);
                if (parts.Length == 2) names[parts[0]] = parts[1];
                continue;
            }
            if (line.Length == 0 || line[0] is '#' or ';') continue;
            int eq = line.IndexOf('=');
            if (eq < 0) continue;
            var key = line[..eq].Trim();
            if (!map.TryGetValue(key, out var list)) continue;
            list.AddRange(line[(eq + 1)..].Split(',').Select(s => s.Trim()).Where(Valid));
        }
        return (map, names);
    }

    static bool Valid(string s)
    {
        int slash = s.IndexOf('/');
        if (slash <= 0 || slash + 2 > s.Length) return false;
        return s[slash + 1] switch
        {
            'a' => int.TryParse(s[(slash + 2)..].TrimEnd('-'), out _),
            'b' => int.TryParse(s[(slash + 2)..], out _),
            'k' => s[..slash] == "keyboard" && int.TryParse(s[(slash + 2)..], out var vk) && vk is > 0 and < 256,
            'h' => s.Length >= slash + 4 && "udlr".Contains(s[^1]) && int.TryParse(s[(slash + 2)..^1], out _),
            _ => false,
        };
    }

    public static void Save(Dictionary<string, List<string>> map, Dictionary<string, string> names)
    {
        var sb = new StringBuilder();
        sb.Append("# Forza Motorsport 2 wheel bindings: the launcher's Bindings window edits this file.\n");
        sb.Append("# <pad input> = <SDL GUID>/<source>, ... (a5 axis, a0- reversed, b12 button, h0u hat up). Missing: the game's default.\n");
        var used = map.Values.SelectMany(l => l).Select(s => s[..s.IndexOf('/')]).Distinct();
        foreach (var g in used)
            if (names.TryGetValue(g, out var n)) sb.Append($"# device {g} {n}\n");
        foreach (var t in Targets)
            if (map.TryGetValue(t, out var list) && list.Count > 0)
                sb.Append($"{t}={string.Join(",", list.Where(Valid))}\n");
        Directory.CreateDirectory(Paths.User);
        System.IO.File.WriteAllText(File, sb.ToString());
    }
}
