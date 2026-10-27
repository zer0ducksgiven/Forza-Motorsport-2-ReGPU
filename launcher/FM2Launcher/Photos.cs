namespace FM2Launcher;

/// <summary>Photo Mode pictures: Documents\fm2\profiles\&lt;profile&gt;\content\&lt;user&gt;\SavedPhotos\photos\&lt;name&gt;
/// (JPEGs without an extension). The page shows them from the launcher's host (Web.PhotoUrl).</summary>
internal static class Photos
{
    public static readonly string Root = Path.Combine(Paths.User, "profiles");

    public record Photo(string Id, string Name, string Profile, long Bytes, DateTime Taken, string Url);

    public static List<Photo> List()
    {
        var list = new List<Photo>();
        if (!Directory.Exists(Root)) return list;
        foreach (var dir in Directory.EnumerateDirectories(Root, "photos", SearchOption.AllDirectories))
        {
            if (!string.Equals(Path.GetFileName(Path.GetDirectoryName(dir)), "SavedPhotos", StringComparison.OrdinalIgnoreCase)) continue;
            foreach (var f in new DirectoryInfo(dir).EnumerateFiles())
            {
                var rel = Path.GetRelativePath(Root, f.FullName);
                var profile = rel.Split(Path.DirectorySeparatorChar)[0];
                list.Add(new Photo(rel, f.Name, profile, f.Length, f.LastWriteTime, Web.PhotoUrl(rel, f.LastWriteTimeUtc)));
            }
        }
        return list.OrderByDescending(p => p.Taken).ToList();
    }

    /// <summary>The file for an id from List(), refusing anything outside the profiles folder.</summary>
    public static string PathOf(string id)
    {
        var full = Path.GetFullPath(Path.Combine(Root, id));
        if (!full.StartsWith(Path.GetFullPath(Root) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) || !File.Exists(full))
            throw new FileNotFoundException("That photo is gone.");
        return full;
    }

    /// <summary>Where Save as starts: Pictures (on Linux, if the desktop has none, home).</summary>
    public static string SaveFolder
    {
        get
        {
            var pictures = Environment.GetFolderPath(Environment.SpecialFolder.MyPictures);
            return Directory.Exists(pictures) ? pictures : Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        }
    }

    /// <summary>Save as: the JPEG as it is, or the PNG the page drew from it.</summary>
    public static void Write(string id, string file, byte[]? png)
    {
        var source = PathOf(id);
        if (png is not null) File.WriteAllBytes(file, png);
        else File.Copy(source, file, true);
    }

    public static void Reveal(string? id)
    {
        if (id is not null) Platform.Reveal(PathOf(id));
        else Platform.OpenFolder(Directory.CreateDirectory(Root).FullName);
    }
}
