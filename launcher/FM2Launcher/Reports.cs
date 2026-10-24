using System.IO.Compression;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace FM2Launcher;

/// <summary>
/// Bug reports and opted-in play.logs, sent to the reports server (server\reports: a Cloudflare Worker
/// that files them in a private GitHub repo). Everything sent goes through Redact first, and the
/// Report tab shows the redacted text before it goes.
/// </summary>
public static class Reports
{
    // FM2_REPORTS_URL points the launcher at a local Worker (wrangler dev) for testing.
    static readonly string Endpoint = Environment.GetEnvironmentVariable("FM2_REPORTS_URL") ?? "https://fm2-reports.fm2-reports.workers.dev/v1/report";
    const int MaxLogBytes = 4 << 20; // the tail of a longer log

    static readonly HttpClient Http = MakeClient();

    static HttpClient MakeClient()
    {
        var h = new HttpClient { Timeout = TimeSpan.FromSeconds(60) };
        h.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("FM2Launcher", Updater.InstalledVersion()));
        return h;
    }

    static string LogFile => Path.Combine(Paths.Root, "out", "play.log");

    /// <summary>The launcher's own reporting choices (not the game's settings.ini).</summary>
    public sealed class Options
    {
        public bool ShareLogs { get; set; }
        public string InstallId { get; set; } = "";
        public DateTime LastSentLog { get; set; }

        static string File => Path.Combine(Paths.User, "reporting.json");

        public static Options Load()
        {
            Options o;
            try { o = JsonSerializer.Deserialize<Options>(System.IO.File.ReadAllText(File)) ?? new(); }
            catch { o = new(); }
            if (o.InstallId.Length == 0)
            {
                // Random, so reports from one PC can be grouped without saying who it is.
                o.InstallId = Guid.NewGuid().ToString("N")[..12];
                o.Save();
            }
            return o;
        }

        public void Save()
        {
            Directory.CreateDirectory(Paths.User);
            System.IO.File.WriteAllText(File, JsonSerializer.Serialize(this));
        }
    }

    /// <summary>
    /// The Windows user and PC names out of a log: paths (C:\Users\name, Wine's Z:\home\name and
    /// /home/name), and the names anywhere else they appear.
    /// </summary>
    public static string Redact(string text)
    {
        text = Regex.Replace(text, @"(?i)([A-Z]:[\\/](?:Users|Documents and Settings)[\\/])[^\\/\r\n""]+", "$1<user>");
        text = Regex.Replace(text, @"(?i)((?:[A-Z]:)?[\\/]home[\\/])[^\\/\r\n""]+", "$1<user>");
        foreach (var (name, mark) in new[] { (Environment.UserName, "<user>"), (Environment.MachineName, "<pc>") })
            if (name.Length >= 3) // shorter names would hit ordinary words
                text = Regex.Replace(text, @"(?i)(?<![A-Za-z0-9])" + Regex.Escape(name) + @"(?![A-Za-z0-9])", mark);
        return text;
    }

    /// <summary>The play.log as it would be sent (redacted, the tail when long); null when there is none.</summary>
    public static string? LogPreview()
    {
        if (!System.IO.File.Exists(LogFile)) return null;
        using var f = new FileStream(LogFile, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        if (f.Length > MaxLogBytes) f.Seek(-MaxLogBytes, SeekOrigin.End);
        using var r = new StreamReader(f, Encoding.UTF8);
        return Redact(r.ReadToEnd());
    }

    static string Gzip64(string text)
    {
        using var ms = new MemoryStream();
        using (var gz = new GZipStream(ms, CompressionLevel.SmallestSize))
            gz.Write(Encoding.UTF8.GetBytes(text));
        return Convert.ToBase64String(ms.ToArray());
    }

    static object SystemInfo() => SystemCheck.Get();

    static async Task<string> SendAsync(object payload)
    {
        using var res = await Http.PostAsJsonAsync(Endpoint, payload);
        var body = await res.Content.ReadAsStringAsync();
        if (!res.IsSuccessStatusCode)
        {
            string? error = null;
            try { error = JsonDocument.Parse(body).RootElement.GetProperty("error").GetString(); } catch { }
            throw new InvalidOperationException(error ?? $"The reports server answered {(int)res.StatusCode}.");
        }
        return JsonDocument.Parse(body).RootElement.GetProperty("id").GetString() ?? "";
    }

    /// <summary>Sends a bug report; returns its id on the server.</summary>
    public static Task<string> SendBugAsync(string title, string description, string contact, bool withLog)
    {
        if (string.IsNullOrWhiteSpace(title)) throw new ArgumentException("Give the report a short title.");
        if (string.IsNullOrWhiteSpace(description)) throw new ArgumentException("Say what happened.");
        var log = withLog ? LogPreview() : null;
        return SendAsync(new
        {
            kind = "bug",
            title = Redact(title.Trim()),
            description = Redact(description.Trim()),
            contact = contact.Trim(),
            version = Updater.InstalledVersion(),
            installId = Options.Load().InstallId,
            system = SystemInfo(),
            log = log is null ? null : Gzip64(log),
        });
    }

    /// <summary>
    /// With log sharing on, sends the last session's play.log once (at launcher start: the game has
    /// closed by then). Never throws: a failed send just tries again next time.
    /// </summary>
    public static async Task SendLastLogAsync()
    {
        try
        {
            var o = Options.Load();
            if (!o.ShareLogs || Game.Running || !System.IO.File.Exists(LogFile)) return;
            var written = System.IO.File.GetLastWriteTimeUtc(LogFile);
            if (written <= o.LastSentLog) return;
            var log = LogPreview();
            if (log is null) return;
            await SendAsync(new
            {
                kind = "log",
                version = Updater.InstalledVersion(),
                installId = o.InstallId,
                system = SystemInfo(),
                log = Gzip64(log),
            });
            o.LastSentLog = written;
            o.Save();
        }
        catch { }
    }
}
