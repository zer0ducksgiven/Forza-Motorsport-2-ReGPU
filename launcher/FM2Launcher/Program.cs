using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;

namespace FM2Launcher;

internal static class Program
{
    [STAThread]
    static int Main(string[] args)
    {
        // --apply <patch.zip>: install a downloaded patch (as the Updates tab does) and report how it went
        int apply = Array.IndexOf(args, "--apply");
        if (apply >= 0 && apply + 1 < args.Length)
        {
            var log = Path.Combine(Path.GetTempPath(), "FM2Launcher-apply.log");
            try
            {
                if (Game.Running) throw new InvalidOperationException("Close the game first.");
                var r = Updater.Apply(Path.GetFullPath(args[apply + 1]), deleteZip: false);
                File.WriteAllText(log, $"ok: {r.Files} files, launcher changed: {r.LauncherChanged}");
                return 0;
            }
            catch (Exception ex) { File.WriteAllText(log, "error: " + ex.Message); return 1; }
        }
        // --play: start the game with the saved settings, without the window (shortcuts, tests)
        if (args.Contains("--play"))
        {
            try { Game.Launch(); return 0; }
            catch (Exception ex) { MessageBox.Show(ex.Message, "Forza Motorsport 2 ReGPU", MessageBoxButtons.OK, MessageBoxIcon.Error); return 1; }
        }
        // one window: starting the launcher again brings the open one forward
        using var single = new Mutex(true, "FM2ReGPU.Launcher", out bool first);
        if (!first)
        {
            foreach (var p in System.Diagnostics.Process.GetProcessesByName("FM2Launcher"))
                if (p.Id != Environment.ProcessId && p.MainWindowHandle != IntPtr.Zero) SetForegroundWindow(p.MainWindowHandle);
            return 0;
        }
        Updater.CleanUp();
        ApplicationConfiguration.Initialize();
        Application.Run(new MainForm());
        return 0;
    }

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    static extern bool SetForegroundWindow(IntPtr hWnd);
}

/// <summary>The window: WebView2 showing wwwroot through a virtual host (no server, nothing on the network).</summary>
internal sealed class MainForm : Form
{
    const string Host = "launcher.fm2";
    readonly WebView2 _web = new() { Dock = DockStyle.Fill, DefaultBackgroundColor = Color.FromArgb(11, 13, 17) };

    public MainForm()
    {
        Text = "Forza Motorsport 2 ReGPU";
        ClientSize = new Size(1360, 820);
        MinimumSize = new Size(1000, 700);
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = Color.FromArgb(11, 13, 17);
        try { Icon = Icon.ExtractAssociatedIcon(Environment.ProcessPath!); } catch { }
        Controls.Add(_web);
        Load += async (_, _) => await InitAsync();
    }

    async Task InitAsync()
    {
        CoreWebView2Environment env;
        try
        {
            env = await CoreWebView2Environment.CreateAsync(null, Path.Combine(Path.GetTempPath(), "FM2Launcher-webview"));
            await _web.EnsureCoreWebView2Async(env);
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, "The launcher needs Microsoft Edge WebView2 (part of Windows 11; a free download for Windows 10).\n\n" + ex.Message,
                Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
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
        core.SetVirtualHostNameToFolderMapping(Photos.Host, Directory.CreateDirectory(Photos.Root).FullName, CoreWebView2HostResourceAccessKind.DenyCors);
        var bridge = new Bridge(core, this) { Controls = new BindingsHost(core) };
        core.WebMessageReceived += bridge.OnMessage;
        _ = Reports.SendLastLogAsync(); // opted in: the last session's play.log
        // only its own page; links go to the browser
        core.NavigationStarting += (_, e) =>
        {
            if (!e.Uri.StartsWith($"https://{Host}/", StringComparison.OrdinalIgnoreCase)) e.Cancel = true;
        };
        core.NewWindowRequested += (_, e) => e.Handled = true;
        core.Navigate($"https://{Host}/index.html");
    }
}
