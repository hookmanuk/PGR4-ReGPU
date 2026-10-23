namespace PGR4Launcher;

/// <summary>Photo Mode pictures: Documents\pgr4\profiles\&lt;profile&gt;\content\&lt;user&gt;\&lt;stamp&gt;.p\photo (a JPEG
/// without an extension; the package's name, "Photo: date @ time", is in &lt;stamp&gt;.p.title, UTF-16). The page shows
/// them through the photos.pgr4 virtual host.</summary>
internal static class Photos
{
    public const string Host = "photos.pgr4";
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
            var url = $"https://{Host}/" + string.Join('/', rel.Split(Path.DirectorySeparatorChar).Select(Uri.EscapeDataString)) + $"?v={f.LastWriteTimeUtc.Ticks}";
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

    public static void Copy(string id)
    {
        using var image = Image.FromStream(new MemoryStream(File.ReadAllBytes(PathOf(id))));
        Clipboard.SetImage(image);
    }

    public static bool SaveAs(string id, IWin32Window owner)
    {
        var source = PathOf(id);
        using var dialog = new SaveFileDialog
        {
            Title = "Save photo",
            FileName = Path.GetFileNameWithoutExtension(Path.GetDirectoryName(source)) + ".jpg",
            Filter = "JPEG image|*.jpg|PNG image|*.png",
            InitialDirectory = Environment.GetFolderPath(Environment.SpecialFolder.MyPictures),
        };
        if (dialog.ShowDialog(owner) != DialogResult.OK) return false;
        if (dialog.FilterIndex == 2)
        {
            using var image = Image.FromStream(new MemoryStream(File.ReadAllBytes(source)));
            image.Save(dialog.FileName, System.Drawing.Imaging.ImageFormat.Png);
        }
        else
            File.Copy(source, dialog.FileName, true);
        return true;
    }

    public static void Reveal(string? id)
    {
        if (id is not null)
            System.Diagnostics.Process.Start("explorer.exe", $"/select,\"{PathOf(id)}\"");
        else
            System.Diagnostics.Process.Start("explorer.exe", $"\"{Directory.CreateDirectory(Root).FullName}\"");
    }
}
