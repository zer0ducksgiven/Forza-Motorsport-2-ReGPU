using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;

namespace FM2Launcher;

/// <summary>
/// The Photos window (from the Profiles tab): its own top-level window showing wwwroot/photos.html, the Photo
/// Mode saves of every profile. Its page's calls go through a Bridge like the main window's.
/// </summary>
internal sealed class PhotosForm : Form
{
    const string Host = "launcher.fm2";
    static PhotosForm? _open;
    readonly WebView2 _web = new() { Dock = DockStyle.Fill, DefaultBackgroundColor = Color.FromArgb(11, 13, 17) };

    /// <summary>Shows the window, or brings the open one forward.</summary>
    public static void ShowWindow()
    {
        if (_open is { IsDisposed: false })
        {
            if (_open.WindowState == FormWindowState.Minimized) _open.WindowState = FormWindowState.Normal;
            _open.Activate();
            return;
        }
        _open = new PhotosForm();
        _open.Show();
    }

    PhotosForm()
    {
        Text = "Photos - Forza Motorsport 2 ReGPU";
        ClientSize = new Size(Math.Min(1400, Screen.PrimaryScreen!.WorkingArea.Width - 40), Math.Min(860, Screen.PrimaryScreen.WorkingArea.Height - 60));
        MinimumSize = new Size(800, 500);
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = Color.FromArgb(11, 13, 17);
        try { Icon = Icon.ExtractAssociatedIcon(Environment.ProcessPath!); } catch { }
        Controls.Add(_web);
        Load += async (_, _) => await InitAsync();
        FormClosed += (_, _) => _open = null;
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
        core.SetVirtualHostNameToFolderMapping(Photos.Host, Directory.CreateDirectory(Photos.Root).FullName, CoreWebView2HostResourceAccessKind.DenyCors);
        core.WebMessageReceived += new Bridge(core, this).OnMessage;
        core.NavigationStarting += (_, e) =>
        {
            if (!e.Uri.StartsWith($"https://{Host}/", StringComparison.OrdinalIgnoreCase)) e.Cancel = true;
        };
        core.NewWindowRequested += (_, e) => e.Handled = true;
        core.Navigate($"https://{Host}/photos.html");
    }
}
