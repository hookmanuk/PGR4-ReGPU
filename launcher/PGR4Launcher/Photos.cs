namespace PGR4Launcher;

/// <summary>Photo Mode pictures: Documents\pgr4\profiles\&lt;profile&gt;\content\&lt;user&gt;\&lt;stamp&gt;.p\photo (a JPEG
/// without an extension; the package's name, "Photo: date @ time", is in &lt;stamp&gt;.p.title, UTF-16). The page shows
/// them from photos/ on the launcher's host (Window.cs).</summary>
internal static class Photos
{
    public static readonly string Root = Path.Combine(Paths.User, "profiles");

    public record Photo(string Id, string Name, string Profile, long Bytes, DateTime Taken, string Url);

    public static List<Photo> List()
    {
        var list = new List<Photo>();
        if (!Directory.Exists(Root)) return list;
        foreach (var f in new DirectoryInfo(Root).EnumerateFiles("photo", SearchOption.AllDirectories).Where(IsPhoto))
        {
            var rel = Path.GetRelativePath(Root, f.FullName);
            var profile = rel.Split(Path.DirectorySeparatorChar)[0];
            var url = Web.PhotoUrl(rel, f.LastWriteTimeUtc);
            list.Add(new Photo(rel, TitleOf(f), profile, f.Length, f.LastWriteTime, url));
        }
        return list.OrderByDescending(p => p.Taken).ToList();
    }

    /// <summary>A saved photo: the "photo" file of a .p package.</summary>
    public static bool IsPhoto(FileInfo f) => f.Name == "photo" && f.Directory?.Extension == ".p";

    static string TitleOf(FileInfo f)
    {
        try
        {
            var title = System.Text.Encoding.Unicode.GetString(File.ReadAllBytes(f.Directory!.FullName + ".title")).TrimEnd('\0').Trim();
            if (title.Length > 0) return title;
        }
        catch { }
        return f.Directory!.Name;
    }

    /// <summary>The file for an id from List(), refusing anything outside the profiles folder.</summary>
    public static string PathOf(string id)
    {
        var full = Path.GetFullPath(Path.Combine(Root, id));
        if (!full.StartsWith(Path.GetFullPath(Root) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) || !File.Exists(full))
            throw new FileNotFoundException("That photo is gone.");
        return full;
    }

    /// <summary>A photo saved where the player chose: the JPEG as it is, or the PNG the page converted it to.</summary>
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
