using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace PGR4Launcher;

/// <summary>Where things are: the install (build\rel\pgr4.exe, game\) and the user folder (Documents\pgr4).</summary>
public static class Paths
{
    /// <summary>The install folder: the first folder up from the launcher that holds build\rel\pgr4.exe.</summary>
    public static readonly string Root = FindRoot();
    public static string Exe => Path.Combine(Root, "build", "rel", "pgr4.exe");
    public static string GameDir => Path.Combine(Root, "game");
    public static readonly string User = FindUser();
    public static string SettingsFile => Path.Combine(User, "settings.ini");
    public static string Dlc => Path.Combine(User, "dlc");
    public static string DlcDisabled => Path.Combine(User, "dlc-disabled");
    public static string Update => Path.Combine(User, "update");

    /// <summary>Documents\pgr4 in the Documents folder Windows reports (it may be moved off C:), as pgr4.exe picks it
    /// (src/user_dir.cpp): if only the old %USERPROFILE%\Documents\pgr4 exists, that one.</summary>
    static string FindUser()
    {
        var current = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "pgr4");
        var old = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Documents", "pgr4");
        return !Directory.Exists(current) && Directory.Exists(old) ? old : current;
    }

    static string FindRoot()
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
            if (File.Exists(Path.Combine(dir.FullName, "build", "rel", "pgr4.exe")))
                return dir.FullName;
        return Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, ".."));
    }
}

/// <summary>
/// Documents\pgr4\settings.ini, the file pgr4.exe reads (src/settings.cpp). Keys the launcher doesn't know are
/// kept as they are.
/// </summary>
public sealed class GameSettings
{
    public string Profile { get; set; } = "Player";
    public string Resolution { get; set; } = "1280x720";
    public string Aspect { get; set; } = "auto";
    public bool Fullscreen { get; set; }
    public bool Vsync { get; set; } = true;
    public int MaxFps { get; set; } = 60;
    public int Msaa { get; set; } = 1;
    public bool Enhance { get; set; }
    public bool SkipIntro { get; set; }
    public bool LowPerformance { get; set; }
    public int Language { get; set; } = 1;
    public int Anisotropy { get; set; } = -1; // -1 the game's own filtering
    public bool HideShaderOverlay { get; set; } = true; // on unless turned off
    // FSR 1 (upscaler=fsr1): the scene's share of the resolution per axis, one of the game's presets
    // (Options.RenderScale; 100 = native), and its sharpening 0-100 (sharpness 0-1). Off at 100 and 0.
    public int RenderScale { get; set; } = 100;
    public int Sharpening { get; set; }

    static readonly string[] Known = ["profile", "resolution", "aspect", "fullscreen", "vsync", "max_fps", "msaa", "enhance", "skip_intro", "low_performance", "language", "anisotropy", "hide_shader_overlay", "upscaler", "upscale_quality", "sharpness"];
    // upscale_quality's presets (src/settings.cpp SceneScale): native, 1.5x, 1.7x, 2x and 3x per axis.
    static readonly int[] QualityScale = [100, 67, 59, 50, 33];

    public static GameSettings Load()
    {
        var s = new GameSettings();
        string upscaler = "off";
        int quality = 1;
        double sharpness = 0.5;
        foreach (var (k, v) in Read())
            switch (k)
            {
                case "anisotropy": s.Anisotropy = int.TryParse(v, out var an) ? an : -1; break;
                case "hide_shader_overlay": s.HideShaderOverlay = v == "1"; break;
                case "upscaler": upscaler = v; break;
                case "upscale_quality": quality = int.TryParse(v, out var q) ? Math.Clamp(q, 0, 4) : 1; break;
                case "sharpness":
                    sharpness = double.TryParse(v, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var sh) ? sh : 0.5;
                    break;
                case "profile": s.Profile = v; break;
                case "resolution": s.Resolution = v; break;
                case "aspect": s.Aspect = v == "16:9" ? v : "auto"; break; // (ultra-wide comes later)
                case "fullscreen": s.Fullscreen = v == "1"; break;
                case "vsync": s.Vsync = v == "1"; break;
                case "max_fps": s.MaxFps = int.TryParse(v, out var f) ? f : 60; break;
                case "msaa": s.Msaa = int.TryParse(v, out var m) ? m : 1; break;
                case "enhance": break; // (enhanced lighting is off for now)
                case "skip_intro": s.SkipIntro = v == "1"; break;
                case "low_performance": s.LowPerformance = v == "1"; break;
                case "language": s.Language = int.TryParse(v, out var l) && l > 0 ? l : 1; break;
            }
        // FSR 3 and DLSS aren't in this build: anything but fsr1 is off.
        if (upscaler == "fsr1")
        {
            s.RenderScale = QualityScale[quality];
            s.Sharpening = (int)Math.Round(Math.Clamp(sharpness, 0, 1) * 100);
        }
        return s;
    }

    public void Save()
    {
        var keep = Read().Where(kv => !Known.Contains(kv.Key)).ToList();
        var profile = new string(Profile.Trim().Where(c => !char.IsControl(c) && c != '=').Take(15).ToArray());
        var sb = new StringBuilder("# Project Gotham Racing 4 (pgr4) settings; the launcher edits this file.\n");
        sb.Append($"profile={(profile.Length > 0 ? profile : "Player")}\n");
        sb.Append($"resolution={Resolution}\naspect={Aspect}\nfullscreen={B(Fullscreen)}\nvsync={B(Vsync)}\n");
        sb.Append($"max_fps={Math.Clamp(MaxFps, 30, 360)}\nmsaa={Msaa}\nenhance=0\nskip_intro={B(SkipIntro)}\nlow_performance={B(LowPerformance)}\nlanguage={Language}\n");
        sb.Append($"anisotropy={(Anisotropy < 0 ? -1 : Math.Clamp(Anisotropy, 0, 16))}\nhide_shader_overlay={B(HideShaderOverlay)}\n");
        // The nearest preset to the render scale; FSR 1 on below 100% or with sharpening.
        var quality = Array.IndexOf(QualityScale, QualityScale.MinBy(p => Math.Abs(p - Math.Clamp(RenderScale, 33, 100))));
        var sharp = Math.Clamp(Sharpening, 0, 100);
        var upscaler = quality > 0 || sharp > 0 ? "fsr1" : "off";
        sb.Append($"upscaler={upscaler}\nupscale_quality={quality}\nsharpness={(sharp / 100.0).ToString(System.Globalization.CultureInfo.InvariantCulture)}\n");
        foreach (var (k, v) in keep) sb.Append($"{k}={v}\n");
        Directory.CreateDirectory(Paths.User);
        File.WriteAllText(Paths.SettingsFile, sb.ToString());
    }

    static string B(bool b) => b ? "1" : "0";

    static List<KeyValuePair<string, string>> Read()
    {
        var list = new List<KeyValuePair<string, string>>();
        if (!File.Exists(Paths.SettingsFile)) return list;
        foreach (var raw in File.ReadAllLines(Paths.SettingsFile))
        {
            var line = raw.Trim();
            int eq = line.IndexOf('=');
            if (line.StartsWith('#') || eq <= 0) continue;
            list.Add(new(line[..eq].Trim(), line[(eq + 1)..].Trim()));
        }
        return list;
    }
}

/// <summary>Choices the Play page offers.</summary>
public static class Options
{
    public sealed record Language(int Id, string Code, string Name);

    // XGetLanguage values; the game reads game\loc\<code> (cultures.txt lists the codes).
    public static readonly Language[] Languages =
    [
        new(1, "EN", "English"), new(3, "DE", "Deutsch"), new(4, "FR", "Français"), new(5, "ES", "Español"),
        new(6, "IT", "Italiano"), new(2, "JA", "日本語"), new(7, "KO", "한국어"), new(10, "SC", "简体中文"), new(8, "TC", "繁體中文"),
        new(11, "PL", "Polski"), new(12, "RU", "Русский"),
    ];

    /// <summary>The languages whose folder is in this install's game\loc.</summary>
    public static IEnumerable<Language> InstalledLanguages()
    {
        var dir = Path.Combine(Paths.GameDir, "loc");
        if (!Directory.Exists(dir)) return Languages;
        var have = Directory.GetDirectories(dir).Select(f => Path.GetFileName(f).ToUpperInvariant()).ToHashSet();
        return Languages.Where(l => have.Contains(l.Code));
    }

    public static readonly int[] Fps = [30, 60, 120, 144, 165, 240, 360];
    public static readonly (string Value, string Name)[] Aspects = [("auto", "Auto (match resolution)"), ("16:9", "16:9")];
    public static readonly (int Value, string Name)[] Msaa = [(1, "Off"), (2, "2x MSAA"), (4, "4x MSAA"), (8, "8x MSAA")];
    public static readonly (int Value, string Name)[] RenderScale = [(100, "100% (native)"), (67, "67% (Quality)"), (59, "59% (Balanced)"), (50, "50% (Performance)"), (33, "33% (Ultra performance)")];
    public static readonly (int Value, string Name)[] Sharpening = [(0, "Off"), (25, "Low"), (50, "Medium"), (75, "High"), (100, "Maximum")];
    public static readonly (int Value, string Name)[] Anisotropy = [(-1, "Game default"), (0, "Off (trilinear)"), (2, "2x"), (4, "4x"), (8, "8x"), (16, "16x")];

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct DEVMODE
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
        public int dmFields, dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
        public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public short dmLogPixels;
        public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
        public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern bool EnumDisplaySettingsW(string? device, int mode, ref DEVMODE dm);

    /// <summary>The primary display's modes from 1280x720 up, smallest first (as the old launcher listed them).</summary>
    public static List<string> Resolutions()
    {
        var set = new HashSet<(int, int)>();
        var dm = new DEVMODE { dmSize = (short)Marshal.SizeOf<DEVMODE>() };
        for (int i = 0; EnumDisplaySettingsW(null, i, ref dm); i++)
            if (dm.dmPelsWidth >= 1280 && dm.dmPelsHeight >= 720) set.Add((dm.dmPelsWidth, dm.dmPelsHeight));
        if (set.Count == 0) set.Add((1280, 720));
        return set.OrderBy(m => (long)m.Item1 * m.Item2).Select(m => $"{m.Item1}x{m.Item2}").ToList();
    }
}

/// <summary>Starting the game, as play.cmd does: the title update and DLC next to the install copied in first.</summary>
public static class Game
{
    public static bool Running => Process.GetProcessesByName("pgr4").Any(p =>
    {
        try { return string.Equals(p.MainModule?.FileName, Paths.Exe, StringComparison.OrdinalIgnoreCase); }
        catch { return true; } // can't look inside: assume it is ours
    });

    public static void Launch(bool coldStart = false)
    {
        if (!File.Exists(Paths.Exe)) throw new InvalidOperationException($"pgr4.exe isn't at {Paths.Exe}.");
        CopyMissing(Path.Combine(Paths.Root, "update"), Paths.Update);
        CopyMissing(Path.Combine(Paths.Root, "dlc"), Paths.Dlc, Paths.DlcDisabled);
        Directory.CreateDirectory(Path.Combine(Paths.Root, "out"));
        var log = Path.Combine(Paths.Root, "out", "play.log");
        var psi = new ProcessStartInfo("cmd.exe", $"/c \"\"{Paths.Exe}\" game > \"{log}\" 2>&1\"")
        {
            WorkingDirectory = Paths.Root,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        // The title update: Documents\pgr4\update, else a development trees out	u2 (as play.cmd uses).
        var devUpdate = Path.Combine(Paths.Root, "out", "tu2");
        if (!File.Exists(Path.Combine(Paths.Update, "default.xexp")) && File.Exists(Path.Combine(devUpdate, "default.xexp")))
            psi.Environment["PGR4_TITLE_UPDATE_DIR"] = devUpdate;
        // The session's log goes up as soon as the game closes (not only at the launcher's next start)
        var game = Process.Start(psi);
        if (game is not null) _ = Reports.SendAfterExitAsync(game);
    }

    // robocopy /XC /XN /XO in install-content.cmd: what is already there is left alone. A DLC package the game has
    // unpacked is a folder of the same name; one switched off sits in dlc-disabled.
    static void CopyMissing(string from, string to, string? alsoIn = null)
    {
        if (!Directory.Exists(from)) return;
        foreach (var file in Directory.EnumerateFiles(from, "*", SearchOption.AllDirectories))
        {
            var rel = Path.GetRelativePath(from, file);
            var dest = Path.Combine(to, rel);
            var top = rel.Split(Path.DirectorySeparatorChar)[0]; // the package (or the file itself)
            bool Has(string dir) => File.Exists(Path.Combine(dir, top)) || Directory.Exists(Path.Combine(dir, top));
            if (File.Exists(dest) || Directory.Exists(dest)) continue;
            if (top == rel && Has(to)) continue;                         // a package the game unpacked into a folder
            if (alsoIn is not null && Has(alsoIn)) continue;             // switched off
            Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
            File.Copy(file, dest);
        }
    }
}

/// <summary>The shader and pipeline caches in Documents\pgr4 (what play-cold.cmd moves aside).</summary>
public static class ShaderCache
{
    public sealed record Info(bool Exists, int Shaders, long Bytes, DateTime? Built);

    static readonly string[] Dirs = ["shadercache", "shaders"];
    static readonly string[] Files = ["pipelines.bin", "pipelines.manifest", "static_shaders.bin"];

    public static Info Get()
    {
        long bytes = 0;
        int count = 0;
        DateTime? built = null;
        foreach (var d in Dirs.Select(d => Path.Combine(Paths.User, d)).Where(Directory.Exists))
            foreach (var f in new DirectoryInfo(d).EnumerateFiles("*", SearchOption.AllDirectories))
            {
                bytes += f.Length;
                if (f.Extension is ".dxbc" or ".ucode") count++;
            }
        foreach (var f in Files.Select(f => new FileInfo(Path.Combine(Paths.User, f))).Where(f => f.Exists))
        {
            bytes += f.Length;
            if (f.Name == "pipelines.bin") built = f.LastWriteTime;
        }
        return new Info(bytes > 0, count, bytes, built);
    }

    /// <summary>Deletes the caches; the next start rebuilds them (several minutes).</summary>
    public static void Delete()
    {
        if (Game.Running) throw new InvalidOperationException("Close the game first: it is using the shader cache.");
        foreach (var d in Dirs.Select(d => Path.Combine(Paths.User, d)).Where(Directory.Exists)) Directory.Delete(d, true);
        foreach (var f in Files.Select(f => Path.Combine(Paths.User, f)).Where(File.Exists)) File.Delete(f);
    }
}

/// <summary>DLC packages in Documents\pgr4\dlc (disabled ones wait in dlc-disabled). A package is a file or an unpacked
/// folder, with a .title (its name, UTF-16) and a .license beside it.</summary>
public static class Dlc
{
    public sealed record Pack(string Id, string Name, bool Enabled, long Bytes);

    // Mods (backported cars in the same package format) live beside the DLC, marked by a .mod file, listed apart.
    public static List<Pack> List() => ListAll(mods: false);
    public static List<Pack> Mods() => ListAll(mods: true);

    static List<Pack> ListAll(bool mods)
    {
        var packs = new List<Pack>();
        foreach (var (dir, enabled) in new[] { (Paths.Dlc, true), (Paths.DlcDisabled, false) })
        {
            if (!Directory.Exists(dir)) continue;
            foreach (var entry in new DirectoryInfo(dir).EnumerateFileSystemInfos())
            {
                if (entry.Extension is ".title" or ".license" or ".mod") continue;
                if (File.Exists(Path.Combine(dir, entry.Name + ".mod")) != mods) continue;
                var titleFile = Path.Combine(dir, entry.Name + ".title");
                var name = File.Exists(titleFile) ? Encoding.Unicode.GetString(File.ReadAllBytes(titleFile)).TrimEnd('\0').Trim() : entry.Name;
                long bytes = entry is FileInfo fi ? fi.Length
                    : ((DirectoryInfo)entry).EnumerateFiles("*", SearchOption.AllDirectories).Sum(f => f.Length);
                packs.Add(new Pack(entry.Name, name.Length > 0 ? name : entry.Name, enabled, bytes));
            }
        }
        return packs.OrderBy(p => p.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
    }

    public static void SetEnabled(string id, bool enabled)
    {
        if (id.Contains('/') || id.Contains('\\') || id.Contains("..")) throw new ArgumentException("Not a package name.");
        if (Game.Running) throw new InvalidOperationException("Close the game first: DLC is read while it runs.");
        var (from, to) = enabled ? (Paths.DlcDisabled, Paths.Dlc) : (Paths.Dlc, Paths.DlcDisabled);
        Directory.CreateDirectory(to);
        foreach (var name in new[] { id, id + ".title", id + ".license", id + ".mod" })
        {
            var src = Path.Combine(from, name);
            var dst = Path.Combine(to, name);
            if (Directory.Exists(src)) Directory.Move(src, dst);
            else if (File.Exists(src)) File.Move(src, dst, true);
        }
    }

    /// <summary>Installs packages (as downloaded from Xbox Live, or backported mods): unpacked into the DLC folder the
    /// way the game reads them. Before, the package file was only copied there and the game never saw it.</summary>
    public static (int Added, List<string> Errors) Add(IEnumerable<string> files, bool mods)
    {
        if (Game.Running) throw new InvalidOperationException("Close the game first: DLC is read while it runs.");
        Directory.CreateDirectory(Paths.Dlc);
        int added = 0;
        var errors = new List<string>();
        foreach (var f in files.Where(File.Exists))
        {
            try
            {
                if (KelloggsZip.TryInstall(f, Paths.Dlc)) { added++; continue; }
                Stfs.Install(f, Paths.Dlc, mods);
                added++;
            }
            catch (Exception ex) { errors.Add($"{Path.GetFileName(f)}: {ex.Message}"); }
        }
        return (added, errors);
    }

    /// <summary>Deletes an installed pack and its files (the Mods panel's Remove).</summary>
    public static void Remove(string id)
    {
        if (id.Contains('/') || id.Contains('\\') || id.Contains("..")) throw new ArgumentException("Not a package name.");
        if (Game.Running) throw new InvalidOperationException("Close the game first: DLC is read while it runs.");
        foreach (var dir in new[] { Paths.Dlc, Paths.DlcDisabled })
            foreach (var name in new[] { id, id + ".title", id + ".license", id + ".mod" })
            {
                var p = Path.Combine(dir, name);
                if (Directory.Exists(p)) Directory.Delete(p, true);
                else if (File.Exists(p)) File.Delete(p);
            }
    }
}

/// <summary>The Kellogg's Typhon DLC as a zip of its installed files (the 360 package is lost; this was rebuilt from
/// what survives): Add DLC copies it into the dlc folder as it is.</summary>
public static class KelloggsZip
{
    const string Id = "EAB7964FF01A0799094FE8AD59670FF4CFC5CFB44D";
    static readonly string[] Files = [Id + "/toc.txt", Id + "/testoffer.txt", Id + ".license", Id + ".title"];

    public static bool TryInstall(string file, string dlcDir)
    {
        if (!file.EndsWith(".zip", StringComparison.OrdinalIgnoreCase)) return false;
        using var zip = System.IO.Compression.ZipFile.OpenRead(file);
        var entries = zip.Entries.Where(e => e.Length > 0).ToDictionary(e => e.FullName.Replace("\\", "/"), StringComparer.OrdinalIgnoreCase);
        if (!entries.ContainsKey(Id + "/toc.txt")) return false;
        foreach (var name in Files)
        {
            if (!entries.TryGetValue(name, out var e)) throw new InvalidDataException("Not the Kellogg's DLC zip: " + name + " is missing.");
            var dest = Path.Combine(dlcDir, name.Replace('/', Path.DirectorySeparatorChar));
            Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
            System.IO.Compression.ZipFileExtensions.ExtractToFile(e, dest, true);
        }
        return true;
    }
}
