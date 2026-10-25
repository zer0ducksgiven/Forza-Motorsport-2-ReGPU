using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;

namespace FM2Launcher;

/// <summary>
/// The wheel Bindings window: its own top-level window (it can sit beside or outside the launcher, on
/// another screen), showing wwwroot/bindings.html. It lists the wheels, pedals, shifters and button boxes
/// SDL sees, streams their axes, buttons and hats to the page about 30 times a second, and saves
/// wheel_bindings.ini for the game.
/// </summary>
internal sealed class BindingsForm : Form
{
    const string Host = "launcher.fm2";
    static BindingsForm? _open;
    static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);
    readonly WebView2 _web = new() { Dock = DockStyle.Fill, DefaultBackgroundColor = Color.FromArgb(11, 13, 17) };
    readonly System.Windows.Forms.Timer _poll = new() { Interval = 33 };
    int _rescan;

    /// <summary>Shows the window, or brings the open one forward.</summary>
    public static void ShowWindow()
    {
        if (_open is { IsDisposed: false })
        {
            if (_open.WindowState == FormWindowState.Minimized) _open.WindowState = FormWindowState.Normal;
            _open.Activate();
            return;
        }
        _open = new BindingsForm();
        _open.Show();
    }

    BindingsForm()
    {
        Text = "Wheel bindings - Forza Motorsport 2 ReGPU";
        ClientSize = new Size(Math.Min(1680, Screen.PrimaryScreen!.WorkingArea.Width - 40), Math.Min(965, Screen.PrimaryScreen.WorkingArea.Height - 60));
        MinimumSize = new Size(900, 600);
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = Color.FromArgb(11, 13, 17);
        try { Icon = Icon.ExtractAssociatedIcon(Environment.ProcessPath!); } catch { }
        Controls.Add(_web);
        Load += async (_, _) => await InitAsync();
        _poll.Tick += (_, _) => Push();
        FormClosed += (_, _) => { _poll.Stop(); _open = null; };
    }

    async Task InitAsync()
    {
        try
        {
            var env = await CoreWebView2Environment.CreateAsync(null, Path.Combine(Path.GetTempPath(), "FM2Launcher-webview"));
            await _web.EnsureCoreWebView2Async(env);
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, ex.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
            Close();
            return;
        }
        var core = _web.CoreWebView2;
        core.Settings.AreDevToolsEnabled = System.Diagnostics.Debugger.IsAttached;
        core.Settings.AreDefaultContextMenusEnabled = false;
        core.Settings.IsStatusBarEnabled = false;
        core.Settings.IsZoomControlEnabled = false;
        core.Settings.AreBrowserAcceleratorKeysEnabled = false;
        core.SetVirtualHostNameToFolderMapping(Host, Path.Combine(AppContext.BaseDirectory, "wwwroot"), CoreWebView2HostResourceAccessKind.DenyCors);
        core.WebMessageReceived += OnMessage;
        core.NavigationStarting += (_, e) =>
        {
            if (!e.Uri.StartsWith($"https://{Host}/", StringComparison.OrdinalIgnoreCase)) e.Cancel = true;
        };
        core.NewWindowRequested += (_, e) => e.Handled = true;
        core.Navigate($"https://{Host}/bindings.html");
        _poll.Start();
    }

    void Post(object o)
    {
        if (_web.CoreWebView2 != null) _web.CoreWebView2.PostWebMessageAsJson(JsonSerializer.Serialize(o, Json));
    }

    // Live input; the device list is checked again every couple of seconds (plugged in or out).
    void Push()
    {
        if (++_rescan % 60 == 1) Post(new { @event = "devices", data = Devices() });
        Post(new { @event = "state", data = Joysticks.Read() });
    }

    static object Devices() => Joysticks.List().Select(d => new { d.Guid, d.Name, d.Kind, d.Axes, d.Buttons, d.Hats, d.Rest });

    void OnMessage(object? sender, CoreWebView2WebMessageReceivedEventArgs e)
    {
        JsonNode? msg;
        try { msg = JsonNode.Parse(e.WebMessageAsJson); } catch { return; }
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

    // Force feedback settings live in settings.ini with the rest; the main window reloads them when focused.
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
