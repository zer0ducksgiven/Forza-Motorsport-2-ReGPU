namespace FM2Launcher;

/// <summary>The racing wheel the game found the last time it started: wheel.txt in the user folder,
/// written by the game's wheel driver (src/input/wheel.cpp) as the wheel's name, its vendor:product
/// and whether it has force feedback, or empty for none. Null before the game has run with wheel
/// support. Read on every refresh: it changes each time the game starts.</summary>
internal static class WheelCheck
{
    public sealed record Result(bool Found, string Name, string Id, bool ForceFeedback);

    public static Result? Get()
    {
        try
        {
            var file = Path.Combine(Paths.User, "wheel.txt");
            if (!File.Exists(file))
                return null;
            var lines = File.ReadAllLines(file).Select(l => l.Trim()).ToArray();
            if (lines.Length < 2 || lines[0].Length == 0)
                return new(false, "", "", false);
            return new(true, lines[0], lines[1], lines.Length > 2 && lines[2] == "force feedback");
        }
        catch
        {
            return null;
        }
    }
}
