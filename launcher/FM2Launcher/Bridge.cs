using System.Text.Json;
using System.Text.Json.Nodes;
using Photino.NET;

namespace FM2Launcher;

/// <summary>
/// The page's calls: { id, cmd, args } over the window's message channel (wwwroot/host.js), answered with
/// { id, ok, result | error }. Progress and other pushes go out as { event, ... }. Messages tagged with a frame
/// ({ frame, msg }) are the Controls tab's, for BindingsHost.
/// </summary>
public sealed class Bridge
{
    static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);
    readonly PhotinoWindow _window;
    Updater.Release? _latest;
    /// <summary>The Controls tab's bindings (main window only).</summary>
    public BindingsHost? Controls { get; init; }

    public Bridge(PhotinoWindow window)
    {
        _window = window;
        window.RegisterWebMessageReceivedHandler((_, text) => OnMessage(text));
    }

    async void OnMessage(string text)
    {
        JsonNode? msg;
        try { msg = JsonNode.Parse(text); } catch { return; }
        if (msg?["frame"] is not null)
        {
            Controls?.OnMessage(msg["msg"]);
            return;
        }
        var id = msg?["id"]?.GetValue<int>() ?? 0;
        var cmd = msg?["cmd"]?.GetValue<string>() ?? "";
        var args = msg?["args"];
        try
        {
            var result = await Run(cmd, args);
            Post(new { id, ok = true, result });
        }
        catch (Exception ex)
        {
            Post(new { id, ok = false, error = ex.Message });
        }
    }

    // Photino takes messages from any thread (progress comes from Forza Plus 2's copy on a worker).
    void Post(object o) => _window.SendWebMessage(JsonSerializer.Serialize(o, Json));
    void Push(string name, object data) => Post(new { @event = name, data });

    static readonly (string, string[])[] AnyFile = [("Xbox 360 content packages", ["*.*"])];
    static readonly (string, string[])[] ZipFilter = [("Profile zip", ["*.zip"])];

    async Task<object?> Run(string cmd, JsonNode? a)
    {
        switch (cmd)
        {
            case "state": return State();
            case "save":
                a.Deserialize<GameSettings>(Json)!.Save();
                return State();
            case "launch":
                a?["settings"].Deserialize<GameSettings>(Json)?.Save();
                if (Game.Running) throw new InvalidOperationException("The game is already running.");
                Game.Launch();
                return null;
            case "shaders.delete":
                ShaderCache.Delete();
                return State();
            case "shaders.rebuild":
                ShaderCache.Delete();
                Game.Launch();
                return State();
            case "dlc.toggle":
                Dlc.SetEnabled(a!["id"]!.GetValue<string>(), a["enabled"]!.GetValue<bool>());
                return State();
            case "dlc.add":
                {
                    var files = await _window.ShowOpenFileAsync("Choose DLC packages", null, true, AnyFile);
                    if (files is not { Length: > 0 }) return new { added = 0, errors = Array.Empty<string>(), state = State() };
                    var (added, errors) = Dlc.Add(files, false);
                    return new { added, errors, state = State() };
                }
            case "pfp.install":
                {
                    var picked = await _window.ShowOpenFolderAsync("Choose the unzipped Forza Plus 2 folder (the one with \"Main Game Changes\" in it)", null, false);
                    if (picked is not { Length: > 0 }) return null;
                    var folder = picked[0];
                    await Task.Run(() => ForzaPlus2.Install(folder, (p, text) => Push("progress", new { p, text })));
                    return State();
                }
            case "pfp.mode":
                ForzaPlus2.SetMode(Str(a, "mode"), a?["harderAi"]?.GetValue<bool>() ?? false);
                return State();
            case "pfp.toggle":
                ForzaPlus2.SetEnabled(a!.GetValue<bool>());
                return State();
            case "pfp.remove":
                await Task.Run(ForzaPlus2.Remove);
                return State();
            case "controls.active":
                if (Controls is not null) Controls.Active = a!.GetValue<bool>();
                return null;
            case "photos.open":
                PhotosWindow.Show(_window);
                return null;
            case "discord":
                Platform.OpenUrl("https://discord.gg/pVEpmSk2G3");
                return null;
            case "open":
                {
                    var which = a?.GetValue<string>();
                    var path = which switch
                    {
                        "user" => Paths.User,
                        "dlc" => Directory.CreateDirectory(Paths.Dlc).FullName,
                        "shaders" => Paths.User,
                        "install" => Paths.Root,
                        "log" => Path.Combine(Paths.Root, "out"),
                        _ => throw new ArgumentException("Unknown folder."),
                    };
                    Platform.OpenFolder(path);
                    return null;
                }
            case "profiles.list": return ProfilesState();
            case "profiles.use":
                Profiles.SetActive(Str(a, "name"));
                return ProfilesState();
            case "profiles.create":
                Profiles.Create(Str(a, "name"));
                return ProfilesState();
            case "profiles.duplicate":
                Profiles.Duplicate(Str(a, "name"), Str(a, "newName"));
                return ProfilesState();
            case "profiles.rename":
                Profiles.Rename(Str(a, "name"), Str(a, "newName"));
                return ProfilesState();
            case "profiles.delete":
                {
                    var b = Profiles.Delete(Str(a, "name"));
                    return new { backup = b.File, state = ProfilesState() };
                }
            case "profiles.backup":
                {
                    var b = Profiles.MakeBackup(Str(a, "name"));
                    return new { backup = b.File, state = ProfilesState() };
                }
            case "profiles.restore":
                {
                    var name = Profiles.Restore(Str(a, "file"), a?["asName"]?.GetValue<string>());
                    return new { name, state = ProfilesState() };
                }
            case "profiles.import":
                {
                    var files = await _window.ShowOpenFileAsync("Import a profile", null, false, ZipFilter);
                    if (files is not { Length: > 0 }) return null;
                    var name = Profiles.Restore(files[0], a?["asName"]?.GetValue<string>());
                    return new { name, state = ProfilesState() };
                }
            case "profiles.export":
                {
                    var name = Str(a, "name");
                    var file = await _window.ShowSaveFileAsync("Export profile", Profiles.FolderName(name) + ".zip", ZipFilter);
                    if (string.IsNullOrEmpty(file)) return false;
                    Profiles.Export(name, file);
                    return true;
                }
            case "profiles.mergePhotos":
                return new { copied = Profiles.MergePhotos(Str(a, "from"), Str(a, "into")), state = ProfilesState() };
            case "profiles.cheats":
                {
                    if (Game.Running) throw new InvalidOperationException("Close the game first: cheats are read when it starts.");
                    a!["cheats"].Deserialize<Cheats>(Json)!.Save(Profiles.FolderName(Str(a, "name")));
                    return ProfilesState();
                }
            case "profiles.openBackups":
                Platform.OpenFolder(Directory.CreateDirectory(Profiles.Backups).FullName);
                return null;
            case "photos.list": return Photos.List();
            case "photos.copy":
                Platform.CopyPng(Convert.FromBase64String(a!.GetValue<string>()));
                return null;
            case "photos.saveAs":
                {
                    var id = a!.GetValue<string>();
                    var file = await _window.ShowSaveFileAsync("Save photo", Path.Combine(Photos.SaveFolder, Path.GetFileName(Photos.PathOf(id)) + ".jpg"),
                        [("JPEG image", ["*.jpg"]), ("PNG image", ["*.png"])]);
                    return string.IsNullOrEmpty(file) ? null : file;
                }
            case "photos.write":
                {
                    var png = a?["png"]?.GetValue<string>();
                    Photos.Write(Str(a, "id"), Str(a, "file"), png is null ? null : Convert.FromBase64String(png));
                    return null;
                }
            case "photos.reveal":
                Photos.Reveal(a?.GetValue<string>());
                return null;
            case "url":
                {
                    var url = a?.GetValue<string>() ?? "";
                    if (!url.StartsWith("https://github.com/", StringComparison.OrdinalIgnoreCase)) throw new ArgumentException("Only GitHub links open from here.");
                    Platform.OpenUrl(url);
                    return null;
                }
            case "report.state":
                {
                    var o = Reports.Options.Load();
                    return new { shareLogs = o.ShareLogs, noticeShown = o.NoticeShown, installId = o.InstallId, hasLog = Reports.LogPreview() is not null };
                }
            case "report.shareLogs":
                {
                    var o = Reports.Options.Load();
                    o.ShareLogs = a!.GetValue<bool>();
                    // From now on: logs written before the opt-in stay on this PC.
                    o.LastSentLog = DateTime.UtcNow;
                    o.Save();
                    return new { shareLogs = o.ShareLogs };
                }
            case "report.noticeShown":
                {
                    var o = Reports.Options.Load();
                    o.NoticeShown = true;
                    o.Save();
                    return null;
                }
            case "report.preview": return Reports.LogPreview() ?? "";
            case "report.send":
                return new
                {
                    id = await Reports.SendBugAsync(Str(a, "title"), Str(a, "description"), a?["contact"]?.GetValue<string>() ?? "",
                        a?["withLog"]?.GetValue<bool>() ?? false),
                };
            case "update.check":
                _latest = await Updater.LatestAsync();
                return UpdateInfo();
            case "update.apply":
                {
                    // The patch zip is laid out for the Windows install (build/tu, a Windows launcher).
                    if (Platform.Linux) throw new InvalidOperationException("On Linux, download FM2-Linux from the release page and unpack it over this folder.");
                    _latest ??= await Updater.LatestAsync();
                    var applied = await Updater.DownloadAndApplyAsync(_latest, (p, text) => Push("progress", new { p, text }));
                    return new { applied.Files, applied.LauncherChanged, update = UpdateInfo(), state = State() };
                }
            case "restart":
                System.Diagnostics.Process.Start(Environment.ProcessPath!);
                _window.Invoke(_window.Close);
                return null;
            default:
                throw new ArgumentException($"Unknown command {cmd}.");
        }
    }

    static string Str(JsonNode? a, string key) => a?[key]?.GetValue<string>() ?? throw new ArgumentException($"Missing {key}.");

    static object ProfilesState() => new { profiles = Profiles.List(), backups = Profiles.ListBackups(), running = Game.Running };

    object UpdateInfo()
    {
        var installed = Updater.InstalledVersion();
        return new
        {
            installed,
            latest = _latest?.Version,
            name = _latest?.Name,
            notes = _latest?.Notes,
            page = _latest?.Page,
            published = _latest?.Published,
            patchBytes = _latest?.PatchBytes ?? 0,
            hasPatch = _latest?.PatchUrl is not null,
            available = _latest is not null && Updater.IsNewer(_latest.Version, installed),
        };
    }

    static object State()
    {
        var settings = GameSettings.Load();
        var stringTables = Path.Combine(Paths.GameDir, "Media", "StringTables");
        return new
        {
            settings,
            version = Updater.InstalledVersion(),
            paths = new { root = Paths.Root, user = Paths.User },
            checks = new
            {
                exe = File.Exists(Paths.Exe),
                game = File.Exists(Path.Combine(Paths.GameDir, "default.xex")),
                update = File.Exists(Path.Combine(Paths.Update, "default.xexp")),
                running = Game.Running,
            },
            system = SystemCheck.Get(),
            wheel = WheelCheck.Get(),
            options = new
            {
                resolutions = Options.Resolutions(),
                aspects = Options.Aspects.Select(x => new { value = x.Value, name = x.Name }),
                fps = Options.Fps,
                msaa = Options.Msaa.Select(x => new { value = x.Value, name = x.Name }),
                smaa = Options.Smaa.Select(x => new { value = x.Value, name = x.Name }),
                anisotropy = Options.Anisotropy.Select(x => new { value = x.Value, name = x.Name }),
                renderScale = Options.RenderScale.Select(x => new { value = x.Value, name = x.Name }),
                sharpening = Options.Sharpening.Select(x => new { value = x.Value, name = x.Name }),
                languages = Options.InstalledLanguages().Select(l => new { id = l.Id, code = l.Code, name = l.Name }),
                missingLanguages = Directory.Exists(stringTables)
                    ? new[] { "PL", "CZ", "HU" }.Where(c => File.Exists(Path.Combine(stringTables, c + ".zip"))).ToArray() : [],
            },
            shaders = ShaderCache.Get(),
            forzaPlus2 = ForzaPlus2.State(),
            dlc = Dlc.List(),
            mods = Dlc.Mods(),
        };
    }
}
