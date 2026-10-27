using Photino.NET;

namespace FM2Launcher;

/// <summary>
/// The Photos window (from the Profiles tab): its own top-level window showing wwwroot/photos.html, the Photo
/// Mode saves of every profile. Its page's calls go through a Bridge like the main window's.
/// </summary>
internal static class PhotosWindow
{
    static PhotinoWindow? _open;

    /// <summary>Shows the window, or brings the open one forward.</summary>
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
            var screen = main.MainMonitor.WorkArea;
            var w = Web.Create("Photos - Forza Motorsport 2 ReGPU", "photos.html",
                Math.Min(1400, screen.Width - 40), Math.Min(860, screen.Height - 60), 800, 500, main);
            _ = new Bridge(w);
            w.RegisterWindowClosingHandler((_, _) => { _open = null; return false; });
            _open = w;
            w.WaitForClose(); // a child window: returns at once, the main window's loop runs it
        });
    }
}
