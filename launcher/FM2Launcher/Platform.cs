using System.Diagnostics;
using System.Runtime.InteropServices;

namespace FM2Launcher;

/// <summary>What differs between Windows and Linux outside the window: opening folders and links, the clipboard,
/// message boxes before there is a window. On Linux the game itself runs through Proton (see Game).</summary>
public static class Platform
{
    public static bool Linux => OperatingSystem.IsLinux();

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
