using System.Diagnostics;
using System.Runtime.InteropServices;

namespace FM2Launcher;

/// <summary>What differs between Windows and Linux outside the window: opening folders and links, the clipboard,
/// message boxes before there is a window. On Linux the game itself runs through Proton (see Game).</summary>
public static class Platform
{
    public static bool Linux => OperatingSystem.IsLinux();

    /// <summary>Linux: Wayland takes a window's icon from the .desktop file named by the app id, not from the window,
    /// so name the app before GTK starts and keep a .desktop file for it (with the icon) in the user's applications.</summary>
    public static void DesktopEntry()
    {
        const string id = "fm2-regpu-launcher";
        try { g_set_prgname(id); } catch (Exception) { return; }
        try
        {
            var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".local", "share", "applications");
            var exe = Environment.ProcessPath ?? Path.Combine(AppContext.BaseDirectory, "FM2Launcher");
            var text = "[Desktop Entry]\nType=Application\nName=Forza Motorsport 2 ReGPU\n" +
                $"Exec=\"{exe}\"\nIcon={Path.Combine(AppContext.BaseDirectory, "wwwroot", "icon.png")}\n" +
                $"StartupWMClass={id}\nCategories=Game;\nTerminal=false\n";
            var file = Path.Combine(dir, id + ".desktop");
            if (File.Exists(file) && File.ReadAllText(file) == text) return;
            Directory.CreateDirectory(dir);
            File.WriteAllText(file, text);
        }
        catch (Exception) { }
    }

    [System.Runtime.InteropServices.DllImport("libglib-2.0.so.0")]
    static extern void g_set_prgname(string name);

    /// <summary>A folder in Explorer, or the desktop's file manager.</summary>
    public static void OpenFolder(string path)
    {
        if (Linux) Start("xdg-open", path);
        else Process.Start("explorer.exe", $"\"{path}\"");
    }

    /// <summary>A file selected in its folder (Linux: the folder; file managers differ in how to select).</summary>
    public static void Reveal(string file)
    {
        if (Linux) Start("xdg-open", Path.GetDirectoryName(file)!);
        else Process.Start("explorer.exe", $"/select,\"{file}\"");
    }

    public static void OpenUrl(string url)
    {
        if (Linux) Start("xdg-open", url);
        else Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
    }

    /// <summary>A PNG onto the clipboard when the page's own clipboard can't (Linux: wl-copy on Wayland, else xclip).</summary>
    public static void CopyPng(byte[] png)
    {
        if (!Linux) throw new PlatformNotSupportedException("The clipboard isn't available.");
        var wayland = Environment.GetEnvironmentVariable("WAYLAND_DISPLAY") is { Length: > 0 };
        var psi = wayland ? new ProcessStartInfo("wl-copy", "--type image/png") : new ProcessStartInfo("xclip", "-selection clipboard -t image/png");
        psi.RedirectStandardInput = true;
        psi.UseShellExecute = false;
        Process p;
        try { p = Process.Start(psi)!; }
        catch { throw new InvalidOperationException(wayland ? "Copying needs wl-clipboard (wl-copy)." : "Copying needs xclip."); }
        using (var stdin = p.StandardInput.BaseStream) stdin.Write(png);
        // wl-copy and xclip stay running to serve the clipboard: not waited for.
    }

    /// <summary>An error before (or without) a window: --play from a shortcut, no web view.</summary>
    public static void Error(string title, string text)
    {
        if (!Linux) { MessageBoxW(IntPtr.Zero, text, title, 0x10); return; }
        Console.Error.WriteLine($"{title}: {text}");
        try { Start("zenity", "--error", "--title", title, "--text", text)?.WaitForExit(); } catch { }
    }

    static Process? Start(string file, params string[] args)
    {
        var psi = new ProcessStartInfo(file) { UseShellExecute = false };
        foreach (var a in args) psi.ArgumentList.Add(a);
        return Process.Start(psi);
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int MessageBoxW(IntPtr hwnd, string text, string caption, uint type);
}
