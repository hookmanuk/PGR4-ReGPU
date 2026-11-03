namespace PGR4Launcher;

internal static class Program
{
    [STAThread]
    static int Main(string[] args)
    {
        // --apply <patch.zip>: install a downloaded patch (as the Updates tab does) and report how it went
        int apply = Array.IndexOf(args, "--apply");
        if (apply >= 0 && apply + 1 < args.Length)
        {
            var log = Path.Combine(Path.GetTempPath(), "PGR4Launcher-apply.log");
            try
            {
                if (Game.Running) throw new InvalidOperationException("Close the game first.");
                var r = Updater.Apply(Path.GetFullPath(args[apply + 1]), deleteZip: false);
                File.WriteAllText(log, $"ok: {r.Files} files, launcher changed: {r.LauncherChanged}");
                return 0;
            }
            catch (Exception ex) { File.WriteAllText(log, "error: " + ex.Message); return 1; }
        }
        // --play: start the game with the saved settings, without the window (shortcuts, tests)
        if (args.Contains("--play"))
        {
            try
            {
                Game.Launch();
                // no window to keep the launcher alive: wait for the game here so its log is sent when it closes
                for (int i = 0; i < 60 && !Game.Running; i++) Thread.Sleep(500);
                while (Game.Running) Thread.Sleep(2000);
                Reports.SendLastLogAsync().Wait();
                return 0;
            }
            catch (Exception ex) { Platform.Error("Project Gotham Racing 4 ReGPU", ex.Message); return 1; }
        }
        // one window: starting the launcher again brings the open one forward
        using var single = new Mutex(true, "PGR4ReGPU.Launcher", out bool first);
        // After an update the old launcher starts this one and only then closes: wait for it a moment.
        if (!first)
            try { first = single.WaitOne(TimeSpan.FromSeconds(5)); }
            catch (AbandonedMutexException) { first = true; }
        if (!first)
        {
            foreach (var p in System.Diagnostics.Process.GetProcessesByName("PGR4Launcher"))
                if (p.Id != Environment.ProcessId && p.MainWindowHandle != IntPtr.Zero) SetForegroundWindow(p.MainWindowHandle);
            return 0;
        }
        Updater.CleanUp();
        Photino.NET.PhotinoWindow window;
        try { window = Web.Create("Project Gotham Racing 4 ReGPU", "index.html", 1680, 960, 1000, 700); }
        catch (Exception ex)
        {
            Platform.Error("Project Gotham Racing 4 ReGPU",
                "The launcher needs Microsoft Edge WebView2 (part of Windows 11; a free download for Windows 10).\n\n" + ex.Message);
            return 1;
        }
        _ = new Bridge(window) { Controls = new BindingsHost(window) };
        _ = Reports.SendLastLogAsync(); // opted in: the last session's play.log
        window.WaitForClose();
        return 0;
    }

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    static extern bool SetForegroundWindow(IntPtr hWnd);
}
