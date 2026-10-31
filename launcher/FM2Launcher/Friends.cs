using System.Text;

namespace FM2Launcher;

/// <summary>The friends list on the top bar: the playing profile's friends.txt ("&lt;xuid hex&gt; &lt;name&gt;" a line), the same
/// list Gift a Car sends to in the game (src/kernel/friends.cpp). Friends are added by profile name, looked up on the
/// FM2 Live server (online_server); there are no requests to accept. Online is "signed in to FM2 Live now".</summary>
static class Friends
{
    static readonly HttpClient Http = new() { Timeout = TimeSpan.FromSeconds(5) };

    /// <summary>The FM2 Live server's base URL from settings.ini, or "" when online play is off.</summary>
    public static string Server()
    {
        var server = GameSettings.Value("online_server").Trim().TrimEnd('/');
        if (server.Length > 0 && !server.Contains("://")) server = "https://" + server;
        return server;
    }

    /// <summary>A POST to the server: its reply, or null when it can't be reached.</summary>
    public static string? Post(string path, string body)
    {
        var server = Server();
        if (server.Length == 0) return null;
        try
        {
            var r = Http.PostAsync(server + path, new StringContent(body)).GetAwaiter().GetResult();
            return r.IsSuccessStatusCode ? r.Content.ReadAsStringAsync().GetAwaiter().GetResult() : null;
        }
        catch { return null; }
    }

    static string FilePath() => Path.Combine(Profiles.Root, Profiles.FolderName(GameSettings.Load().Profile), "friends.txt");

    static List<(string Xuid, string Name)> Read()
    {
        var list = new List<(string, string)>();
        try
        {
            foreach (var line in File.ReadAllLines(FilePath()))
            {
                var space = line.IndexOf(' ');
                if (space == 16 && line.Length > 17) list.Add((line[..16].ToUpperInvariant(), line[17..].Trim()));
            }
        }
        catch (IOException) { }
        return list;
    }

    static void Write(List<(string Xuid, string Name)> list)
    {
        var text = new StringBuilder();
        foreach (var (xuid, name) in list) text.Append(xuid).Append(' ').Append(name).Append('\n');
        File.WriteAllText(FilePath(), text.ToString());
    }

    /// <summary>The friends with who's online (online is false for everyone when the server can't be reached).</summary>
    public static object State()
    {
        var list = Read();
        var reply = list.Count > 0 ? Post("/players/online", string.Join("\n", list.Select(f => f.Xuid))) : "";
        var on = (reply ?? "").Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).ToHashSet(StringComparer.OrdinalIgnoreCase);
        return new
        {
            profile = GameSettings.Load().Profile,
            server = Server().Length > 0,
            reachable = reply != null,
            friends = list.Select(f => new { name = f.Name, online = on.Contains(f.Xuid) })
                .OrderBy(f => !f.online).ThenBy(f => f.name, StringComparer.OrdinalIgnoreCase),
        };
    }

    /// <summary>Adds the player with that profile name (any case); the server knows each name's one player.</summary>
    public static void Add(string name)
    {
        name = name.Trim();
        if (name.Length == 0) throw new ArgumentException("Type a profile name.");
        if (Server().Length == 0) throw new InvalidOperationException("Online play is off: set the FM2 Live server first.");
        if (string.Equals(name, GameSettings.Load().Profile, StringComparison.OrdinalIgnoreCase))
            throw new ArgumentException("That's you. Type another player's profile name.");
        // "player <xuid hex> <last seen> <name>" or "none"
        var reply = Post("/players/find", name) ?? throw new InvalidOperationException("Can't reach the FM2 Live server.");
        var parts = reply.Trim().Split(' ', 4);
        if (parts.Length < 4 || parts[0] != "player" || parts[1].Length != 16)
            throw new ArgumentException($"No player called {name} has been on FM2 Live.");
        var list = Read();
        if (list.Any(f => f.Xuid == parts[1].ToUpperInvariant())) throw new ArgumentException($"{parts[3]} is already your friend.");
        list.Add((parts[1].ToUpperInvariant(), parts[3]));
        Write(list);
    }

    public static void Remove(string name)
    {
        var list = Read();
        list.RemoveAll(f => string.Equals(f.Name, name, StringComparison.OrdinalIgnoreCase));
        Write(list);
    }
}
