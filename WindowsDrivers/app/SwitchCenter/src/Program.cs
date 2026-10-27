// Switch control center (stage 1: notification-area icon). See
// Windows-Drivers\Design-ControlCenter-ja.md for the plan.
using System;
using System.Threading;
using System.Windows.Forms;

namespace SwitchCenter
{
    static class Program
    {
        /// <param name="args">--open: show the control center window right away.
        /// --install-drivers &lt;tag&gt; &lt;service,...&gt;: the elevated driver installer.
        /// --self-update &lt;folder&gt; &lt;pid&gt;: an unpacked new version replacing the one in folder.</param>
        [STAThread]
        static void Main(string[] args)
        {
            if (args.Length == 3 && args[0] == "--install-drivers")
            {
                // Elevated OTA installer started by the Drivers tab (Updater.RunElevatedAsync).
                Application.EnableVisualStyles();
                Application.SetHighDpiMode(HighDpiMode.SystemAware);
                Lang.Load();
                using var form = new InstallForm(args[1], args[2].Split(','));
                Application.Run(form);
                Environment.ExitCode = form.Result;
                return;
            }
            if (args.Length == 3 && args[0] == "--self-update" && int.TryParse(args[2], out int pid))
            {
                // Unpacked new version replacing the installed one (Updater.StartSelfUpdate).
                Lang.Load();
                Updater.ApplySelfUpdate(args[1], pid);
                return;
            }
            using var single = new Mutex(true, @"Local\SwitchWOA.SwitchCenter", out bool created);
            if (!created) return;
            Application.EnableVisualStyles();
            Application.SetHighDpiMode(HighDpiMode.SystemAware);
            Lang.Load();
            Updater.CleanUpdates();
            Application.Run(new TrayApp(Array.IndexOf(args, "--open") >= 0));
        }
    }
}
