namespace FM2Launcher;

/// <summary>FM2 Live (Settings): online play through the FM2 Live server, settings.ini's online_server. Never with
/// cheats on the playing profile or Rogue Mode on: their credits, cars and levels would reach the auction house,
/// gifts and leaderboards. The game checks the same when it starts (src/kernel/runtime.cpp).</summary>
static class Online
{
    public const string DefaultServer = "https://fm2-live.fm2-reports.workers.dev";

    /// <summary>Why online play is off whatever the switch says, or null.</summary>
    public static string? Blocked()
    {
        var profile = GameSettings.Load().Profile;
        if (Cheats.Load(Profiles.FolderName(profile)).Any) return $"Off while {profile} has cheats on (Profiles tab).";
        if (CareerMod.Roguelike.IsEnabled) return "Off while Rogue Mode is on.";
        return null;
    }

    public static bool Enabled => GameSettings.Value("online_server").Trim().Length > 0;

    public static void SetEnabled(bool on)
    {
        if (on && Blocked() is { } why) throw new InvalidOperationException(why);
        GameSettings.SetValue("online_server", on ? DefaultServer : "");
    }

    public static object State() => new { enabled = Enabled, blocked = Blocked() };
}
