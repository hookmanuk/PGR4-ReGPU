using Photino.NET;

namespace PGR4Launcher;

/// <summary>
/// The launcher window: Photino showing wwwroot on WebView2 (as the FM2 launcher). Pages come from the launcher's own
/// scheme handler at https://launcher.pgr4/ (no server, nothing on the network). Photo Mode pictures are under photos/
/// on the same host, so the page can draw them to a canvas (copies and PNG saves).
/// </summary>
public static class Web
{
    public const string Base = "https://launcher.pgr4/";
    const string PhotosPath = "photos/";

    public static PhotinoWindow Create(string title, string page, int width, int height, int minWidth, int minHeight)
    {
        var icon = Path.Combine(AppContext.BaseDirectory, "pgr4.ico");
        var w = new PhotinoWindow()
            .SetTitle(title)
            .SetSize(width, height)
            .SetMinSize(minWidth, minHeight)
            .SetUseOsDefaultSize(false)
            .Center()
            .SetLogVerbosity(0)
            .SetContextMenuEnabled(false)
            .SetDevToolsEnabled(System.Diagnostics.Debugger.IsAttached || Environment.GetEnvironmentVariable("PGR4_LAUNCHER_DEVTOOLS") == "1")
            .SetJavascriptClipboardAccessEnabled(true)
            .SetTemporaryFilesPath(Path.Combine(Path.GetTempPath(), "PGR4Launcher-webview"))
            .RegisterCustomSchemeHandler("https", Serve);
        if (File.Exists(icon)) w.SetIconFile(icon);
        w.Load(new Uri(Base + page));
        return w;
    }

    /// <summary>A Photo Mode picture's address, from its path under Photos.Root.</summary>
    public static string PhotoUrl(string relative, DateTime written) =>
        Base + PhotosPath + string.Join('/', relative.Split(Path.DirectorySeparatorChar).Select(Uri.EscapeDataString)) + $"?v={written.Ticks}";

    // Only the launcher's host: anything else gets nothing.
    static Stream? Serve(object sender, string scheme, string url, out string contentType)
    {
        contentType = "text/plain";
        if (!url.StartsWith(Base, StringComparison.OrdinalIgnoreCase)) return null;
        var rel = Uri.UnescapeDataString(new Uri(url).AbsolutePath.TrimStart('/'));
        string root;
        if (rel.StartsWith(PhotosPath, StringComparison.OrdinalIgnoreCase))
        {
            root = Photos.Root;
            rel = rel[PhotosPath.Length..];
            contentType = "image/jpeg"; // the game's photos are JPEGs without an extension
        }
        else
        {
            root = Path.Combine(AppContext.BaseDirectory, "wwwroot");
            contentType = Path.GetExtension(rel).ToLowerInvariant() switch
            {
                ".html" => "text/html",
                ".js" => "text/javascript",
                ".css" => "text/css",
                ".png" => "image/png",
                ".svg" => "image/svg+xml",
                ".woff2" => "font/woff2",
                _ => "application/octet-stream",
            };
        }
        var full = Path.GetFullPath(Path.Combine(root, rel));
        if (!full.StartsWith(Path.GetFullPath(root) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) || !File.Exists(full)) return null;
        try { return contentType == "text/html" ? Stamped(full, root) : File.OpenRead(full); } catch { return null; }
    }

    // Scripts and stylesheets named by their file's time ("app.js?v=<ticks>"): WebView2 caches them and the handler
    // can't send headers, so an updated launcher would otherwise keep running the old app.js.
    static Stream Stamped(string page, string root)
    {
        var html = System.Text.RegularExpressions.Regex.Replace(File.ReadAllText(page), @"(src|href)=""([\w.-]+\.(?:js|css))""", m =>
        {
            var file = Path.Combine(root, m.Groups[2].Value);
            return File.Exists(file) ? $"{m.Groups[1].Value}=\"{m.Groups[2].Value}?v={File.GetLastWriteTimeUtc(file).Ticks}\"" : m.Value;
        });
        return new MemoryStream(System.Text.Encoding.UTF8.GetBytes(html));
    }
}

/// <summary>Opening folders and links, and error boxes before there is a window.</summary>
public static class Platform
{
    public static void OpenFolder(string path) => System.Diagnostics.Process.Start("explorer.exe", $"\"{path}\"");
    public static void Reveal(string file) => System.Diagnostics.Process.Start("explorer.exe", $"/select,\"{file}\"");
    public static void OpenUrl(string url) => System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(url) { UseShellExecute = true });
    public static void Error(string title, string text) => MessageBoxW(IntPtr.Zero, text, title, 0x10);

    [System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
    static extern int MessageBoxW(IntPtr hwnd, string text, string caption, uint type);
}
