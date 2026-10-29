using Photino.NET;

namespace FM2Launcher;

/// <summary>
/// A launcher window: Photino showing wwwroot, WebView2 on Windows and WebKitGTK on Linux. Pages come from the
/// launcher's own scheme handler (no server, nothing on the network): https://launcher.fm2/ on Windows, where
/// WebView2 lets the handler take https, fm2://launcher/ on Linux, where WebKitGTK keeps https to itself. Photo Mode
/// pictures are under photos/ on the same host, so the page can draw them to a canvas (PNG copies).
/// </summary>
public static class Web
{
    public static readonly string Base = Platform.Linux ? "fm2://launcher/" : "https://launcher.fm2/";
    const string PhotosPath = "photos/";

    public static PhotinoWindow Create(string title, string page, int width, int height, int minWidth, int minHeight, PhotinoWindow? parent = null)
    {
        var icon = Path.Combine(AppContext.BaseDirectory, Platform.Linux ? "wwwroot/icon.png" : "fm2.ico");
        var w = new PhotinoWindow(parent)
            .SetTitle(title)
            .SetSize(width, height)
            .SetMinSize(minWidth, minHeight)
            .SetUseOsDefaultSize(false)
            .Center()
            .SetLogVerbosity(0)
            .SetContextMenuEnabled(false)
            .SetDevToolsEnabled(System.Diagnostics.Debugger.IsAttached || Environment.GetEnvironmentVariable("FM2_LAUNCHER_DEVTOOLS") == "1")
            .SetJavascriptClipboardAccessEnabled(true)
            .SetTemporaryFilesPath(Path.Combine(Path.GetTempPath(), "FM2Launcher-webview"))
            .RegisterCustomSchemeHandler(Platform.Linux ? "fm2" : "https", Serve);
        if (File.Exists(icon)) w.SetIconFile(icon);
        w.Load(new Uri(Base + page));
        return w;
    }

    /// <summary>A Photo Mode picture's address, from its path under Photos.Root.</summary>
    public static string PhotoUrl(string relative, DateTime written) =>
        Base + PhotosPath + string.Join('/', relative.Split(Path.DirectorySeparatorChar).Select(Uri.EscapeDataString)) + $"?v={written.Ticks}";

    // Only the launcher's host: anything else (a link that wasn't sent to the browser) gets nothing.
    static Stream? Serve(object sender, string scheme, string url, out string contentType)
    {
        contentType = "text/plain";
        if (!url.StartsWith(Base, StringComparison.OrdinalIgnoreCase)) return null;
        var rel = Uri.UnescapeDataString(new Uri(url).AbsolutePath.TrimStart('/'));
        string root;
        if (rel.StartsWith(PhotosPath, StringComparison.OrdinalIgnoreCase))
        {
            root = Photos.Root;
            rel = rel[PhotosPath.Length..];
            contentType = "image/jpeg"; // the game's photos are JPEGs without an extension
        }
        else
        {
            root = Path.Combine(AppContext.BaseDirectory, "wwwroot");
            contentType = Path.GetExtension(rel).ToLowerInvariant() switch
            {
                ".html" => "text/html",
                ".js" => "text/javascript",
                ".css" => "text/css",
                ".png" => "image/png",
                ".svg" => "image/svg+xml",
                ".woff2" => "font/woff2",
                _ => "application/octet-stream",
            };
        }
        var full = Path.GetFullPath(Path.Combine(root, rel));
        if (!full.StartsWith(Path.GetFullPath(root) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) || !File.Exists(full)) return null;
        try { return contentType == "text/html" ? Stamped(full, root) : File.OpenRead(full); } catch { return null; }
    }

    // A page with its scripts and stylesheets named by their file's time ("app.js?v=<ticks>"): WebView2 keeps them in
    // its cache (the handler can't send headers), so an updated launcher went on running the old app.js.
    static Stream Stamped(string page, string root)
    {
        var html = System.Text.RegularExpressions.Regex.Replace(File.ReadAllText(page), @"(src|href)=""([\w.-]+\.(?:js|css))""", m =>
        {
            var file = Path.Combine(root, m.Groups[2].Value);
            return File.Exists(file) ? $"{m.Groups[1].Value}=\"{m.Groups[2].Value}?v={File.GetLastWriteTimeUtc(file).Ticks}\"" : m.Value;
        });
        return new MemoryStream(System.Text.Encoding.UTF8.GetBytes(html));
    }
}
