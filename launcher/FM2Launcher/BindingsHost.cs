using System.Text.Json;
using System.Text.Json.Nodes;
using Photino.NET;

namespace FM2Launcher;

/// <summary>
/// The Controls tab: wwwroot/bindings.html in a frame of the main window, its messages tagged with the frame's name
/// (wwwroot/host.js) and handed here by the Bridge. It lists the wheels, pedals, shifters and button boxes SDL sees,
/// streams their axes, buttons and hats to the page about 30 times a second while the tab is shown, and saves
/// wheel_bindings.ini for the game.
/// </summary>
public sealed class BindingsHost
{
    const string Frame = "controls";
    static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);
    readonly PhotinoWindow _window;
    readonly Timer _poll;
    int _rescan;

    public BindingsHost(PhotinoWindow window)
    {
        _window = window;
        // SDL is read on the window's thread, as it was under WinForms' timer.
        _poll = new Timer(_ => _window.Invoke(Push));
    }

    /// <summary>The tab is shown (live input streams) or hidden (it stops).</summary>
    public bool Active
    {
        set
        {
            _rescan = 0;
            _poll.Change(value ? 0 : Timeout.Infinite, value ? 33 : Timeout.Infinite);
        }
    }

    void Post(object o) => _window.SendWebMessage(JsonSerializer.Serialize(new { frame = Frame, msg = o }, Json));

    // Live input; the device list is checked again every couple of seconds (plugged in or out).
    void Push()
    {
        if (_rescan++ % 60 == 0) Post(new { @event = "devices", data = Devices() });
        Post(new { @event = "state", data = Joysticks.Read() });
    }

    static object Devices() => Joysticks.List().Select(d => new { d.Guid, d.Name, d.Kind, d.Axes, d.Buttons, d.Hats, d.Rest });

    public void OnMessage(JsonNode? msg)
    {
        var id = msg?["id"]?.GetValue<int>() ?? 0;
        var cmd = msg?["cmd"]?.GetValue<string>() ?? "";
        try
        {
            object? result = cmd switch
            {
                "init" => Init(),
                "save" => Save(msg?["args"]),
                "ffb" => SetForce(msg?["args"]),
                _ => throw new ArgumentException($"Unknown command {cmd}."),
            };
            Post(new { id, ok = true, result });
        }
        catch (Exception ex)
        {
            Post(new { id, ok = false, error = ex.Message });
        }
    }

    static object Init()
    {
        var (map, names) = WheelBindings.Load();
        return new { devices = Devices(), bindings = map, names, running = Game.Running, ffb = Force(GameSettings.Load()) };
    }

    static object Force(GameSettings s) => new { s.WheelStrength, s.WheelSpring, s.WheelRotation, s.WheelReverse };

    // Force feedback settings live in settings.ini with the rest; the main page reloads them when it's shown.
    static object SetForce(JsonNode? a)
    {
        var s = GameSettings.Load();
        if (a?["wheelStrength"] is { } st) s.WheelStrength = st.GetValue<int>();
        if (a?["wheelSpring"] is { } sp) s.WheelSpring = sp.GetValue<int>();
        if (a?["wheelRotation"] is { } ro) s.WheelRotation = ro.GetValue<int>();
        if (a?["wheelReverse"] is { } re) s.WheelReverse = re.GetValue<bool>();
        s.Save();
        return new { ffb = Force(s), running = Game.Running };
    }

    static object Save(JsonNode? a)
    {
        var map = a?["bindings"].Deserialize<Dictionary<string, List<string>>>(Json) ?? [];
        var names = a?["names"].Deserialize<Dictionary<string, string>>(Json) ?? [];
        WheelBindings.Save(map, names);
        return new { running = Game.Running };
    }
}
