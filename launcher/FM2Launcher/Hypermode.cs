using System.Text;
using Photino.NET;

namespace FM2Launcher;

/// <summary>
/// Hypermode: ircluzar's low-spec performance cuts (the game's gpu/hyper.h), set from their own window (Graphics >
/// Hypermode Options, wwwroot/hyper.html). Kept in Documents\fm2\hypermode.ini, apart from settings.ini (which the
/// game's own Options > Graphics screen writes again): "enabled", the preset's name, the window's choices as ui_*
/// lines, and the hyper_* keys the game reads (settings.cpp: each sets one cut; the game's own presets stay off).
/// </summary>
public static class Hypermode
{
    public static string File => Path.Combine(Paths.User, "hypermode.ini");

    /// <summary>The window's settings and what each is when Hypermode leaves it alone.</summary>
    static readonly (string Key, string Off, string[] Allowed)[] Items =
    [
        ("helpers", "100", ["100", "75", "50", "25"]),
        ("aniso", "16", ["16", "8", "4", "2", "0"]),
        ("resolve1010", "0", ["0", "1"]),
        ("resolve_by_size", "0", ["0", "1"]),
        ("freeze", "0", ["0", "1"]),
        ("freeze_refresh", "3", ["2", "3", "4", "6"]),
        ("cheap_upscale", "0", ["0", "1"]),
        ("cull_pixels", "-1", ["-1", "0", "64", "128", "256"]),
        ("cull_all", "0", ["0", "1"]),
        ("cull_passes", "0", ["0", "1"]),
        ("decimate", "0", ["0", "90", "75", "50", "25", "10"]),
        ("vcache", "0", ["0", "1"]),
        ("shader_opt", "0", ["0", "1"]),
        ("plain_mul", "0", ["0", "1"]),
        ("fetchspec", "0", ["0", "1"]),
        ("tailspec", "0", ["0", "1"]),
        ("swizzle_sel", "0", ["0", "1"]),
        ("lean_lod", "0", ["0", "1"]),
    ];

    public sealed class Settings
    {
        public bool Enabled { get; set; }
        public string Preset { get; set; } = "Custom";
        public Dictionary<string, string> Values { get; set; } = [];
    }

    public static Settings Load()
    {
        var s = new Settings();
        foreach (var (key, off, _) in Items) s.Values[key] = off;
        if (!System.IO.File.Exists(File)) return s;
        foreach (var line in System.IO.File.ReadAllLines(File))
        {
            var eq = line.IndexOf('=');
            if (eq <= 0 || line.StartsWith('#')) continue;
            var (k, v) = (line[..eq].Trim(), line[(eq + 1)..].Trim());
            if (k == "enabled") s.Enabled = v == "1";
            else if (k == "preset") s.Preset = v;
            else if (k.StartsWith("ui_") && Items.Any(i => i.Key == k[3..] && i.Allowed.Contains(v))) s.Values[k[3..]] = v;
        }
        return s;
    }

    public static void Save(Settings s)
    {
        // Only what the window offers is written: anything else falls back to "off".
        var v = Items.ToDictionary(i => i.Key, i => s.Values.TryGetValue(i.Key, out var x) && i.Allowed.Contains(x) ? x : i.Off);
        var sb = new StringBuilder("# Hypermode (the launcher's Graphics > Hypermode Options window edits this file).\n");
        sb.Append($"enabled={(s.Enabled ? 1 : 0)}\n");
        sb.Append($"preset={new string(s.Preset.Where(char.IsLetterOrDigit).Take(24).ToArray())}\n");
        foreach (var (key, _, _) in Items) sb.Append($"ui_{key}={v[key]}\n");
        // What the game reads (settings.cpp, HyperKey): one key per cut that is on.
        sb.Append("hyper=off\n");
        if (v["helpers"] != "100") sb.Append($"hyper_helpers={v["helpers"]}\n");
        if (v["aniso"] != "16") sb.Append($"hyper_aniso={v["aniso"]}\n");
        if (v["resolve1010"] == "1") sb.Append("hyper_resolve1010=1\n");
        if (v["resolve_by_size"] == "1") sb.Append("hyper_resolve_by_size=1\n");
        if (v["freeze"] == "1") sb.Append($"hyper_freeze=120\nhyper_freeze_refresh={v["freeze_refresh"]}\n");
        if (v["cheap_upscale"] == "1") sb.Append("hyper_upscale=1\n");
        if (v["cull_pixels"] != "-1")
        {
            // Culling always carries the invisible re-measure, the hysteresis and the blink fix: without them culled
            // objects flicker (docs/hyper/HYPER-SETTINGS-GUIDE.md).
            sb.Append($"hyper_cull_pixels={v["cull_pixels"]}\nhyper_cull_all={v["cull_all"]}\nhyper_cull_probe=1\nhyper_cull_hyst=200\nhyper_cull_pin=1\n");
            if (v["cull_passes"] == "1") sb.Append("hyper_cull_passes=1\n"); // (the game only looks at whether it is there)
        }
        if (v["decimate"] != "0")
        {
            sb.Append($"hyper_decimate={v["decimate"]}\n");
            if (v["vcache"] == "1") sb.Append("hyper_vcache=2\n"); // (runs inside the simplifier)
        }
        if (v["shader_opt"] == "1") sb.Append("hyper_shader_opt=1\n");
        if (v["plain_mul"] == "1") sb.Append("hyper_plain_mul=5\n");
        foreach (var key in new[] { "fetchspec", "tailspec", "swizzle_sel", "lean_lod" })
            if (v[key] == "1") sb.Append($"hyper_{key}=1\n");
        Directory.CreateDirectory(Paths.User);
        System.IO.File.WriteAllText(File, sb.ToString());
    }

    /// <summary>For the main window's Graphics tab: whether it is on and with which preset.</summary>
    public static object Summary()
    {
        var s = Load();
        return new { enabled = s.Enabled, preset = s.Preset };
    }

    public static object State()
    {
        var s = Load();
        return new
        {
            enabled = s.Enabled,
            preset = s.Preset,
            values = s.Values,
            vulkan = GameSettings.Load().Renderer == "vulkan",
            running = Game.Running,
            integrated = SystemCheck.Get().Integrated,
        };
    }
}

/// <summary>The Hypermode Options window (from the Graphics tab): its own top-level window, like Photos.</summary>
internal static class HypermodeWindow
{
    static PhotinoWindow? _open;
    static PhotinoWindow? _main;

    public static void Show(PhotinoWindow main)
    {
        main.Invoke(() =>
        {
            if (_open is not null)
            {
                _open.SetMinimized(false);
                _open.SetTopMost(true).SetTopMost(false);
                return;
            }
            _main = main;
            var screen = main.MainMonitor.WorkArea;
            var w = Web.Create("Hypermode - Forza Motorsport 2 ReGPU", "hyper.html",
                Math.Min(1120, screen.Width - 40), Math.Min(820, screen.Height - 60), 760, 520, main);
            _ = new Bridge(w);
            w.RegisterWindowClosingHandler((_, _) => { _open = null; return false; });
            _open = w;
            w.WaitForClose(); // a child window: returns at once, the main window's loop runs it
        });
    }

    /// <summary>Tells the main window's page that Hypermode changed (its Graphics tab shows the state).</summary>
    public static void Changed()
    {
        try
        {
            _main?.SendWebMessage(System.Text.Json.JsonSerializer.Serialize(new { @event = "hyper", data = Hypermode.Summary() },
                new System.Text.Json.JsonSerializerOptions(System.Text.Json.JsonSerializerDefaults.Web)));
        }
        catch { } // the main window is closing
    }
}
