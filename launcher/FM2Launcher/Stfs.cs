using System.Buffers.Binary;
using System.Text;

namespace FM2Launcher;

/// <summary>
/// Xbox 360 STFS packages (LIVE, PIRS, CON): DLC as downloaded, and the backported cars ("mods") that use
/// the same format. A port of tools/stfs.py: a header up to 0xA000, then 0x1000-byte blocks, with a hash
/// table block before every 0xAA data blocks (and higher-level tables every 0xAA*0xAA); CON packages keep two
/// copies of each table. Installed packages are unpacked folders with their name (.title, UTF-16 LE) and
/// license mask (.license) beside them, as the game reads them (src/kernel/imports_xam.cpp).
/// </summary>
public sealed class Stfs : IDisposable
{
    const int Block = 0x1000, L0 = 0xAA, L1 = 0xAA * 0xAA;
    readonly FileStream _f;
    readonly int _sep;
    readonly long _first;
    readonly int _tableBlocks, _tableStart;
    public uint ContentType { get; }
    public uint TitleId { get; }
    public uint LicenseMask { get; }
    public string DisplayName { get; }
    public string TitleName { get; }

    public sealed record Entry(string Path, bool Directory, bool Consecutive, int Blocks, int Start, long Size);

    public Stfs(string path)
    {
        _f = File.OpenRead(path);
        var head = new byte[0x1800];
        if (_f.Read(head, 0, head.Length) < head.Length) throw new InvalidDataException("Too small to be a package.");
        var magic = Encoding.ASCII.GetString(head, 0, 4);
        if (magic is not ("LIVE" or "PIRS" or "CON ")) throw new InvalidDataException("Not an Xbox 360 package (STFS).");
        _sep = magic == "CON " ? 2 : 1;
        ContentType = BinaryPrimitives.ReadUInt32BigEndian(head.AsSpan(0x344));
        TitleId = BinaryPrimitives.ReadUInt32BigEndian(head.AsSpan(0x360));
        var headerSize = BinaryPrimitives.ReadUInt32BigEndian(head.AsSpan(0x340));
        _first = (headerSize + 0xFFF) & ~0xFFFL;
        var vd = head.AsSpan(0x379, 0x24);
        _tableBlocks = BinaryPrimitives.ReadUInt16LittleEndian(vd[3..]);
        _tableStart = vd[5] | vd[6] << 8 | vd[7] << 16;
        for (int i = 0; i < 16; i++)
            LicenseMask |= BinaryPrimitives.ReadUInt32BigEndian(head.AsSpan(0x22C + i * 0x10 + 8));
        DisplayName = Utf16(head.AsSpan(0x411, 0x80));
        TitleName = Utf16(head.AsSpan(0x1691, 0x80));
    }

    static string Utf16(ReadOnlySpan<byte> b) => Encoding.BigEndianUnicode.GetString(b).Split('\0')[0].Trim();

    public void Dispose() => _f.Dispose();

    long Backing(int block)
    {
        long n = block + _sep * (block / L0 + 1);
        if (block >= L0) n += _sep * (block / L1 + 1);
        if (block >= L1) n += _sep;
        return n;
    }

    byte[] ReadBlock(int block)
    {
        var buf = new byte[Block];
        _f.Seek(_first + Backing(block) * Block, SeekOrigin.Begin);
        _f.ReadExactly(buf);
        return buf;
    }

    int NextBlock(int block)
    {
        var first = block / L0 * L0;
        var table = Backing(first) - _sep;
        var e = new byte[0x18];
        _f.Seek(_first + table * Block + block % L0 * 0x18, SeekOrigin.Begin);
        _f.ReadExactly(e);
        return e[0x15] << 16 | e[0x16] << 8 | e[0x17];
    }

    IEnumerable<int> Chain(int start, int count, bool consecutive)
    {
        if (consecutive)
        {
            for (int i = 0; i < count; i++) yield return start + i;
            yield break;
        }
        for (int b = start, n = 0; n < count && b != 0xFFFFFF; n++, b = NextBlock(b)) yield return b;
    }

    public List<Entry> Files()
    {
        var table = Chain(_tableStart, _tableBlocks, false).SelectMany(ReadBlock).ToArray();
        var raw = new List<(string Name, bool Dir, bool Cons, int Blocks, int Start, int Parent, long Size)>();
        for (int i = 0; i + 0x40 <= table.Length; i += 0x40)
        {
            var e = table.AsSpan(i, 0x40);
            int len = e[0x28] & 0x3F;
            if (len == 0) break;
            raw.Add((Encoding.ASCII.GetString(e[..len]), (e[0x28] & 0x80) != 0, (e[0x28] & 0x40) != 0, e[0x2C] | e[0x2D] << 8 | e[0x2E] << 16,
                e[0x2F] | e[0x30] << 8 | e[0x31] << 16, BinaryPrimitives.ReadInt16BigEndian(e[0x32..]), BinaryPrimitives.ReadUInt32BigEndian(e[0x34..])));
        }
        string PathOf(int i)
        {
            var parts = new List<string>();
            for (int guard = 0; i >= 0 && i < raw.Count && guard < 64; guard++, i = raw[i].Parent) parts.Add(raw[i].Name);
            parts.Reverse();
            return string.Join('/', parts);
        }
        return raw.Select((r, i) => new Entry(PathOf(i), r.Dir, r.Cons, r.Blocks, r.Start, r.Size)).ToList();
    }

    /// <summary>Unpacks every file under <paramref name="dest"/> (names checked to stay inside it).</summary>
    public int Extract(string dest)
    {
        var root = Path.GetFullPath(dest) + Path.DirectorySeparatorChar;
        int files = 0;
        foreach (var e in Files())
        {
            var target = Path.GetFullPath(Path.Combine(dest, e.Path.Replace('/', Path.DirectorySeparatorChar)));
            if (!target.StartsWith(root, StringComparison.OrdinalIgnoreCase)) continue;
            if (e.Directory)
            {
                System.IO.Directory.CreateDirectory(target);
                continue;
            }
            System.IO.Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            using var output = File.Create(target);
            long remaining = e.Size;
            foreach (var b in Chain(e.Start, e.Blocks, e.Consecutive))
            {
                if (remaining <= 0) break;
                var chunk = ReadBlock(b);
                output.Write(chunk, 0, (int)Math.Min(remaining, Block));
                remaining -= Block;
            }
            files++;
        }
        return files;
    }

    // The title update this build was made from (installer/setup_game.py REF_TITLE_UPDATE).
    const string TitleUpdateSha256 = "b5fd1860c2fbab6e594baf7dd58cbfe867f51f65e41177e15e0b36da0f61aae5";

    /// <summary>Unpacks the title update into Documents\fm2\update, as the installer does, for installs where that
    /// step didn't happen (a setup that stopped part way, a game folder copied from elsewhere): the game won't start
    /// without it.</summary>
    public static void InstallTitleUpdate(string package, string dest)
    {
        string hash;
        using (var f = File.OpenRead(package))
            hash = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(f)).ToLowerInvariant();
        if (hash != TitleUpdateSha256)
            throw new InvalidDataException($"{Path.GetFileName(package)} isn't the title update this build needs (the file named like TU_16L61VA_...).");
        using var s = new Stfs(package);
        System.IO.Directory.CreateDirectory(dest);
        s.Extract(dest);
        if (!File.Exists(Path.Combine(dest, "default.xexp")))
            throw new InvalidDataException("The title update unpacked without default.xexp.");
    }

    /// <summary>Installs a package as the game reads DLC: unpacked to dir\name, with name.title and name.license.
    /// Returns the installed folder's name. <paramref name="mod"/> marks it as a mod (name.mod) for the launcher.</summary>
    public static string Install(string package, string dir, bool mod)
    {
        using var s = new Stfs(package);
        if (s.TitleId != 0x4D5307EA) throw new InvalidDataException($"{Path.GetFileName(package)} isn't Forza Motorsport 2 content.");
        if (s.ContentType != 2) throw new InvalidDataException($"{Path.GetFileName(package)} isn't downloadable content (type {s.ContentType:X8}).");
        var name = Path.GetFileName(package);
        if (name.Length > 42) name = name[..42];
        var dest = Path.Combine(dir, name);
        if (System.IO.Directory.Exists(dest)) System.IO.Directory.Delete(dest, true);
        s.Extract(dest);
        File.WriteAllBytes(dest + ".title", Encoding.Unicode.GetBytes(s.DisplayName));
        File.WriteAllText(dest + ".license", $"{s.LicenseMask:X8}\r\n");
        if (mod) File.WriteAllText(dest + ".mod", "");
        else if (File.Exists(dest + ".mod")) File.Delete(dest + ".mod");
        return name;
    }
}
