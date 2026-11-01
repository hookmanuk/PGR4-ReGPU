using System.IO.Compression;
using System.Text;

namespace PGR4Launcher;

/// <summary>
/// Game profiles: Documents\pgr4\profiles\&lt;name&gt;, one per profile name in settings.ini. The saves inside are
/// encrypted and signed for the profile's XUID, which pgr4.exe keeps in the folder's xuid file (src/kernel/
/// imports_xam.cpp), so a copied, renamed or restored folder keeps working. A folder from before that file
/// existed gets the XUID pgr4.exe derives from the name, written here before the folder is copied or renamed.
/// Backups are zips of the folder in Documents\pgr4\backups.
/// </summary>
internal static class Profiles
{
    public static readonly string Root = Path.Combine(Paths.User, "profiles");
    public static readonly string Backups = Path.Combine(Paths.User, "backups");

    public record Info(string Name, string Xuid, long Bytes, DateTime? LastPlayed, int Photos, bool Active);
    public record Backup(string File, string Profile, long Bytes, DateTime Made);

    public static List<Info> List()
    {
        var active = GameSettings.Load().Profile;
        var list = new List<Info>();
        // The game plays the active profile (and shows its name) whether or not it has a folder yet, so list it.
        Directory.CreateDirectory(Path.Combine(Root, FolderName(active)));
        foreach (var dir in new DirectoryInfo(Root).EnumerateDirectories().OrderBy(d => d.Name, StringComparer.OrdinalIgnoreCase))
        {
            var content = Path.Combine(dir.FullName, "content");
            var files = Directory.Exists(content) ? new DirectoryInfo(content).EnumerateFiles("*", SearchOption.AllDirectories).ToList() : [];
            DateTime? last = files.Count > 0 ? files.Max(f => f.LastWriteTime) : null;
            int photos = files.Count(Photos.IsPhoto);
            list.Add(new Info(dir.Name, XuidOf(dir.Name), files.Sum(f => f.Length), last, photos,
                string.Equals(FolderName(active), dir.Name, StringComparison.OrdinalIgnoreCase)));
        }
        return list;
    }

    /// <summary>The folder pgr4.exe uses for a profile name (runtime.cpp: characters Windows forbids become '_').</summary>
    public static string FolderName(string name) =>
        new(name.Select(c => "<>:\"/\\|?*".Contains(c) || c < 32 ? '_' : c).ToArray());

    static string Dir(string name)
    {
        var dir = Path.Combine(Root, FolderName(name));
        if (!Directory.Exists(dir)) throw new DirectoryNotFoundException($"There is no profile called {name}.");
        return dir;
    }

    /// <summary>Checks a new profile name: what settings.ini and the game accept (15 characters, not empty, not taken).</summary>
    static string NewName(string name)
    {
        name = new string((name ?? "").Trim().Where(c => !char.IsControl(c) && c != '=').Take(15).ToArray());
        if (name.Length == 0) throw new ArgumentException("Give the profile a name.");
        if (Directory.Exists(Path.Combine(Root, FolderName(name)))) throw new ArgumentException($"A profile called {name} already exists.");
        return name;
    }

    /// <summary>The profile's XUID: its xuid file, else the one pgr4.exe derives from the name (FNV-1a of the name's bytes).</summary>
    public static string XuidOf(string folder)
    {
        var file = Path.Combine(Root, folder, "xuid");
        try
        {
            var text = File.ReadAllText(file).Trim();
            if (text.Length == 16 && text[0] is 'E' or 'e') return text.ToUpperInvariant();
        }
        catch { }
        ulong h = 0xCBF29CE484222325;
        foreach (var b in Encoding.UTF8.GetBytes(folder))
            h = (h ^ b) * 0x100000001B3;
        return (0xE000000000000000 | (h & 0x0000FFFFFFFFFFFF)).ToString("X16");
    }

    /// <summary>Writes the xuid file if it's missing, so the saves stay valid under another folder name.</summary>
    static void PinXuid(string dir)
    {
        var file = Path.Combine(dir, "xuid");
        if (!File.Exists(file)) File.WriteAllText(file, XuidOf(Path.GetFileName(dir)) + "\n");
    }

    static void NotRunning()
    {
        if (Game.Running) throw new InvalidOperationException("Close the game first: it keeps the profile open.");
    }

    public static void SetActive(string name)
    {
        Dir(name);
        var s = GameSettings.Load();
        s.Profile = name;
        s.Save();
    }

    public static string Create(string name)
    {
        name = NewName(name);
        Directory.CreateDirectory(Path.Combine(Root, FolderName(name)));
        return name;
    }

    public static string Duplicate(string name, string newName)
    {
        NotRunning();
        var from = Dir(name);
        newName = NewName(newName);
        PinXuid(from);
        CopyDir(from, Path.Combine(Root, FolderName(newName)));
        return newName;
    }

    public static string Rename(string name, string newName)
    {
        NotRunning();
        var from = Dir(name);
        newName = NewName(newName);
        PinXuid(from);
        Directory.Move(from, Path.Combine(Root, FolderName(newName)));
        var s = GameSettings.Load();
        if (string.Equals(FolderName(s.Profile), Path.GetFileName(from), StringComparison.OrdinalIgnoreCase))
        {
            s.Profile = newName;
            s.Save();
        }
        return newName;
    }

    /// <summary>Deletes a profile, backing it up first (the backup is the way back).</summary>
    public static Backup Delete(string name)
    {
        NotRunning();
        var dir = Dir(name);
        var backup = MakeBackup(name, "deleted");
        Directory.Delete(dir, true);
        // Deleting the active profile: play another one, so the game doesn't quietly recreate it empty.
        var s = GameSettings.Load();
        if (string.Equals(FolderName(s.Profile), Path.GetFileName(dir), StringComparison.OrdinalIgnoreCase))
        {
            s.Profile = new DirectoryInfo(Root).EnumerateDirectories().Select(d => d.Name)
                .OrderBy(n => n, StringComparer.OrdinalIgnoreCase).FirstOrDefault() ?? "Player";
            s.Save();
        }
        return backup;
    }

    public static Backup MakeBackup(string name, string? tag = null)
    {
        var dir = Dir(name);
        PinXuid(dir);
        Directory.CreateDirectory(Backups);
        var stamp = DateTime.Now.ToString("yyyy-MM-dd HHmmss");
        var file = Path.Combine(Backups, $"{Path.GetFileName(dir)} {stamp}{(tag is null ? "" : " " + tag)}.zip");
        Zip(dir, file);
        var fi = new FileInfo(file);
        return new Backup(fi.FullName, Path.GetFileName(dir), fi.Length, fi.LastWriteTime);
    }

    public static List<Backup> ListBackups()
    {
        if (!Directory.Exists(Backups)) return [];
        return new DirectoryInfo(Backups).EnumerateFiles("*.zip").OrderByDescending(f => f.LastWriteTime)
            .Select(f => new Backup(f.FullName, ProfileInZip(f.FullName) ?? "?", f.Length, f.LastWriteTime)).ToList();
    }

    /// <summary>Restores a backup (or an exported zip) as a profile: over the profile it came from (backed up first),
    /// or as a new profile when a name is given.</summary>
    public static string Restore(string zip, string? asName)
    {
        NotRunning();
        var source = ProfileInZip(zip) ?? throw new InvalidDataException("That zip doesn't hold a PGR4 profile.");
        string name;
        if (string.IsNullOrWhiteSpace(asName))
        {
            name = source;
            if (Directory.Exists(Path.Combine(Root, FolderName(name))))
            {
                MakeBackup(name, "before restore");
                Directory.Delete(Path.Combine(Root, FolderName(name)), true);
            }
        }
        else
            name = NewName(asName);
        var target = Path.Combine(Root, FolderName(name));
        Directory.CreateDirectory(target);
        using var archive = ZipFile.OpenRead(zip);
        var prefix = source + "/";
        foreach (var e in archive.Entries)
        {
            var rel = e.FullName.Replace('\\', '/');
            if (!rel.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) || rel.EndsWith('/')) continue;
            var dest = Path.GetFullPath(Path.Combine(target, rel[prefix.Length..]));
            if (!dest.StartsWith(Path.GetFullPath(target) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) continue;
            Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
            e.ExtractToFile(dest, true);
        }
        // a zip made before the xuid file existed: the saves belong to the name it was made under
        var xuid = Path.Combine(target, "xuid");
        if (!File.Exists(xuid)) File.WriteAllText(xuid, XuidOf(source) + "\n");
        return name;
    }

    /// <summary>Imports a save copied off an Xbox 360 (PGR4's save package, from the console's
    /// Content\&lt;profile&gt;\4D5307F9\00000001 folder), as the FM2 launcher does. The save stays signed for the console
    /// profile, so the new profile takes that profile's XUID from the package header; it's named after the gamertag
    /// unless a name is given. The game keeps it as content\00000001\pgr4.save (savegame, buffer.save).</summary>
    public static string Import360(string package, string? asName)
    {
        NotRunning();
        using var stfs = new Stfs(package);
        if (stfs.TitleId != 0x4D5307F9 || stfs.ContentType != 1)
            throw new InvalidDataException("That isn't a Project Gotham Racing 4 save from an Xbox 360.");
        if (!stfs.Files().Any(e => !e.Directory && e.Path == "savegame"))
            throw new InvalidDataException("That save package has no savegame in it.");
        var head = new byte[8];
        using (var f = File.OpenRead(package)) { f.Position = 0x371; f.ReadExactly(head); }
        var xuid = Convert.ToHexString(head);
        if (xuid[0] != 'E') throw new InvalidDataException("The save doesn't say which Xbox profile it belongs to.");

        string name;
        if (!string.IsNullOrWhiteSpace(asName)) name = NewName(asName);
        else
        {
            var tag = string.IsNullOrWhiteSpace(stfs.DisplayName) ? "Xbox 360" : stfs.DisplayName;
            tag = new string(tag.Trim().Where(c => !char.IsControl(c) && c != '=').Take(15).ToArray());
            name = tag;
            for (int i = 2; Directory.Exists(Path.Combine(Root, FolderName(name))); i++)
                name = tag[..Math.Min(tag.Length, 12)] + " " + i;
            name = NewName(name);
        }
        var target = Path.Combine(Root, FolderName(name));
        var save = Path.Combine(target, "content", "00000001", "pgr4.save");
        Directory.CreateDirectory(save);
        try
        {
            stfs.Extract(save);
            File.WriteAllText(save + ".title", "PGR4 Save Game", new UnicodeEncoding(false, false));
            File.WriteAllText(Path.Combine(target, "xuid"), xuid + "\n");
        }
        catch
        {
            Directory.Delete(target, true);
            throw;
        }
        return name;
    }

    public static void Export(string name, string file)
    {
        var dir = Dir(name);
        PinXuid(dir);
        Zip(dir, file);
    }

    /// <summary>Merging what can be merged: saved photos (each a &lt;stamp&gt;.p package with its .title) from one
    /// profile into another, renamed where a name is taken. The career is one save, so it can't be combined.</summary>
    public static int MergePhotos(string from, string into)
    {
        NotRunning();
        int copied = 0;
        foreach (var photo in Directory.GetFiles(Path.Combine(Dir(from), "content"), "photo", SearchOption.AllDirectories))
        {
            var package = Path.GetDirectoryName(photo)!;
            if (!Photos.IsPhoto(new FileInfo(photo))) continue;
            // the same user folder (content\<user>) under the other profile
            var user = Path.GetFileName(Path.GetDirectoryName(package)!);
            var dstUser = Path.Combine(Dir(into), "content", user);
            var name = Path.GetFileNameWithoutExtension(package);
            var unique = name;
            for (int n = 2; Directory.Exists(Path.Combine(dstUser, unique + ".p")); n++) unique = $"{name}_{n}";
            CopyDir(package, Path.Combine(dstUser, unique + ".p"));
            if (File.Exists(package + ".title")) File.Copy(package + ".title", Path.Combine(dstUser, unique + ".p.title"), true);
            copied++;
        }
        return copied;
    }

    static string? ProfileInZip(string zip)
    {
        try
        {
            using var archive = ZipFile.OpenRead(zip);
            var first = archive.Entries.Select(e => e.FullName.Replace('\\', '/')).FirstOrDefault(n => n.Contains("/content/"));
            return first?[..first.IndexOf('/')];
        }
        catch { return null; }
    }

    static void Zip(string dir, string file)
    {
        var tmp = file + ".tmp";
        if (File.Exists(tmp)) File.Delete(tmp);
        ZipFile.CreateFromDirectory(dir, tmp, CompressionLevel.Optimal, includeBaseDirectory: true);
        File.Move(tmp, file, true);
    }

    static void CopyDir(string from, string to)
    {
        foreach (var d in Directory.GetDirectories(from, "*", SearchOption.AllDirectories))
            Directory.CreateDirectory(Path.Combine(to, Path.GetRelativePath(from, d)));
        Directory.CreateDirectory(to);
        foreach (var f in Directory.GetFiles(from, "*", SearchOption.AllDirectories))
            File.Copy(f, Path.Combine(to, Path.GetRelativePath(from, f)));
    }
}

