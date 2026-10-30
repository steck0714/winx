// OTA driver updates from the GitHub releases of Windows-for-Switch-Drivers
// (design: docs\Design-DriverRepo-OTA-ja.md). The window only checks and
// compares, as the normal user. Installing restarts SwitchCenter elevated
// with --install-drivers <tag> <services>; that process fetches the same
// release again by itself, into a folder only administrators can write, and
// checks the ZIP's SHA-256, each .sys against the manifest and each .cat's
// signer before it runs pnputil. Nothing from the unelevated process is trusted.
//
// GPIO and I2C carry the other Switch drivers. Replacing them while they run
// stops those drivers, and on the Switch PnP hung restarting I2C1. So they are
// only added to the driver store and their devices marked for reinstall at
// the next boot (CONFIGFLAG_REINSTALL), before anything uses them. The
// manifest says which drivers those are ("installAt": "boot"); releases made
// before that field existed fall back to the list in AtNextBoot.
//
// The devices to look at come from the manifest too, so a driver for a new
// DSDT device installs without a new control center. A device that is present
// in the DSDT proves the UEFI is new enough for it; minUefi only explains why
// a device is missing.
//
// Fetching and checking a release is shared with SwitchPatcher (app\Shared\ReleaseClient.cs).
//
// The control center updates itself as the normal user: the new ZIP (SHA-256
// checked) is unpacked under %LOCALAPPDATA%, and that copy, started with
// --self-update <folder> <pid>, waits for this process to exit, copies itself
// over the installed folder and starts it again.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net.Http;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Cryptography.Pkcs;
using System.Security.Principal;
using System.Text.Json;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Win32;
using static SwitchCenter.Lang;

namespace SwitchCenter
{
    /// <summary>One driver of the release compared with this Switch.</summary>
    sealed class UpdateRow
    {
        public ManifestDriver Item;
        public string Installed;          // lowest installed version over the device's instances, null = no driver
        public bool DevicePresent, UefiTooOld, Update;
        public bool AtNextBoot => Item.AtNextBoot;
        public int Pending;               // instances already marked for reinstall at the next boot
    }

    static class Updater
    {
        static readonly TimeSpan PnputilTimeout = TimeSpan.FromMinutes(2);

        public static Task<Release> FetchAsync(string tag) => ReleaseClient.FetchAsync(tag);
        public static Task<Release> FetchChannelAsync(bool beta) => ReleaseClient.FetchChannelAsync(beta);
        static HttpClient NewClient() => ReleaseClient.NewClient();
        static string Sha256(string path) => ReleaseClient.Sha256(path);

        // ------------------------------------------------------------ compare

        /// <summary>
        /// The UEFI has no version string of its own (SMBIOS only carries the
        /// EDK2 commit), so estimate it from the newest Switch device in the DSDT:
        /// each R3xx below added one. Returns e.g. 377, or 0 when none is present.
        /// </summary>
        public static int EstimateUefi()
        {
            (string Hwid, int Rev)[] added =
            {
                ("SWOA0801", 377), ("SWOA0701", 375), ("SWOA0601", 374), ("SWOA0501", 373),
                ("SWOA0401", 372), ("SWOA0301", 371), ("SWOA0201", 370), ("SWOA0101", 367),
            };
            foreach (var (hwid, rev) in added)
            {
                using RegistryKey k = Registry.LocalMachine.OpenSubKey($@"SYSTEM\CurrentControlSet\Enum\ACPI\{hwid}");
                if (k != null && k.SubKeyCount > 0) return rev;
            }
            return 0;
        }

        static int UefiNumber(string r) => r != null && r.Length > 1 && int.TryParse(r.Substring(1), out int n) ? n : 0;

        public static List<UpdateRow> Compare(Release release, List<DriverRow> installed, int uefi)
        {
            var rows = new List<UpdateRow>();
            foreach (ManifestDriver d in release.Drivers)
            {
                var row = new UpdateRow { Item = d };
                Version lowest = null;
                bool missingDriver = false;
                foreach (DriverRow i in installed.Where(i => i.HardwareId.Equals(d.Hwid, StringComparison.OrdinalIgnoreCase) && i.Instance != ""))
                {
                    row.DevicePresent = true;
                    if (i.ReinstallPending) { row.Pending++; continue; }      // already waiting for the reboot
                    if (System.Version.TryParse(i.Installed, out Version v)) { if (lowest == null || v < lowest) lowest = v; }
                    else missingDriver = true;
                }
                row.Installed = missingDriver ? null : lowest?.ToString();
                // The device being in the DSDT shows the UEFI has it; minUefi only explains a missing device.
                row.UefiTooOld = !row.DevicePresent && UefiNumber(d.MinUefi) > uefi;
                row.Update = row.DevicePresent && (missingDriver || (lowest != null && lowest < System.Version.Parse(d.Version)));
                rows.Add(row);
            }
            return rows;
        }

        public static bool AppIsOlder(Release release) =>
            System.Version.TryParse(release.AppVersion, out Version latest) &&
            System.Version.TryParse(Application.ProductVersion.Split('+')[0], out Version mine) && mine < latest;

        /// <summary>Starts the elevated installer and waits for it. Returns its exit code, or null if UAC was declined.</summary>
        public static async Task<int?> RunElevatedAsync(string tag, IEnumerable<string> services)
        {
            var psi = new ProcessStartInfo(Environment.ProcessPath, $"--install-drivers {tag} {string.Join(",", services)}")
            {
                UseShellExecute = true,
                Verb = "runas",
            };
            Process p;
            try { p = Process.Start(psi); }
            catch (System.ComponentModel.Win32Exception e) when (e.NativeErrorCode == 1223) { return null; }   // ERROR_CANCELLED
            using (p)
            {
                await p.WaitForExitAsync();
                return p.ExitCode;
            }
        }

        // ------------------------------------------------- elevated installer

        public const int ExitOk = 0, ExitReboot = 3010, ExitFailed = 1;

        /// <summary>Runs in the elevated process. log() shows progress; returns the exit code.</summary>
        public static async Task<int> InstallAsync(string tag, string[] services, Action<string> log)
        {
            if (!new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator))
                throw new UnauthorizedAccessException(T("管理者として実行されていません。", "Not running as administrator."));

            log(T($"リリース {tag} を GitHub から読んでいます…", $"Reading release {tag} from GitHub..."));
            Release release = await FetchAsync(tag);
            List<DriverRow> devices = DriverStatus.Read(release);
            var chosen = new List<ManifestDriver>();
            foreach (ManifestDriver d in release.Drivers)               // manifest order = install order (GPIO, I2C, ...)
            {
                if (Array.IndexOf(services, d.Service) < 0) continue;
                if (!devices.Exists(r => r.HardwareId.Equals(d.Hwid, StringComparison.OrdinalIgnoreCase) && r.Instance != ""))
                    throw new InvalidOperationException(T($"{d.Service} のデバイス（{d.Hwid}）がありません。UEFI {d.MinUefi} 以降が必要です。",
                                                          $"There is no {d.Hwid} device for {d.Service}; it needs UEFI {d.MinUefi} or later."));
                chosen.Add(d);
            }
            if (chosen.Count == 0) throw new InvalidOperationException(T("入れるドライバーがありません。", "Nothing to install."));

            string work = ReleaseClient.CreateAdminOnlyFolder();
            bool leaveFiles = false;
            try
            {
                string zip = Path.Combine(work, "drivers.zip");
                log(T($"{release.DriversFile} をダウンロードしています…", $"Downloading {release.DriversFile}..."));
                using (HttpClient http = NewClient())
                using (Stream src = await http.GetStreamAsync(release.DriversUrl))
                using (FileStream dst = File.Create(zip))
                    await src.CopyToAsync(dst);
                string hash = Sha256(zip);
                if (hash != release.DriversSha256)
                    throw new InvalidDataException(T($"ZIP の SHA-256 が manifest と合いません（{hash}）。", $"The ZIP's SHA-256 does not match the manifest ({hash})."));
                log(T("SHA-256 が manifest と合いました。", "SHA-256 matches the manifest."));

                string files = Path.Combine(work, "files");
                ZipFile.ExtractToDirectory(zip, files);                  // rejects entries outside the folder

                foreach (ManifestDriver d in chosen)
                {
                    ReleaseClient.VerifyDriver(files, d);
                    log(T($"{d.Service} {d.Version}: .sys と署名を確かめました。", $"{d.Service} {d.Version}: .sys and signature checked."));
                }

                bool reboot = false;
                for (int n = 0; n < chosen.Count; n++)
                {
                    ManifestDriver d = chosen[n];
                    string inf = Path.Combine(files, d.Inf);
                    if (d.AtNextBoot)
                    {
                        log(T($"{d.Service} を Windows に登録しています（入れ替えは次の起動のとき）…", $"Adding {d.Service} to the driver store (replaced at the next boot)..."));
                        int? code = await Pnputil($"/add-driver \"{inf}\"", log);
                        if (code == null) { leaveFiles = true; return Stopped(chosen, n, log); }
                        if (code != 0 && code != ExitReboot)
                            throw new InvalidOperationException(T($"pnputil が失敗しました（{code}）。", $"pnputil failed ({code})."));
                        int marked = MarkForReinstall(d, release);
                        log(T($"  {marked} 個のデバイスを、次の起動で新しいドライバーにするようにしました。", $"  {marked} device(s) will get the new driver at the next boot."));
                        reboot = true;
                    }
                    else
                    {
                        log(T($"{d.Service} を入れています…", $"Installing {d.Service}..."));
                        int? code = await Pnputil($"/add-driver \"{inf}\" /install", log);
                        if (code == null) { leaveFiles = true; return Stopped(chosen, n, log); }
                        if (code == ExitReboot) reboot = true;
                        else if (code != 0)
                            throw new InvalidOperationException(T($"pnputil が失敗しました（{code}）。", $"pnputil failed ({code})."));
                    }
                }
                return reboot ? ExitReboot : ExitOk;
            }
            finally
            {
                // A pnputil that timed out may still be reading the files.
                if (!leaveFiles)
                    try { Directory.Delete(work, true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
            }
        }

        /// <summary>pnputil did not return in time: say so and leave the rest for after a restart.</summary>
        static int Stopped(List<ManifestDriver> chosen, int index, Action<string> log)
        {
            log(T("pnputil が 2 分たっても終わりません。Windows を再起動すると、入れ替えが終わります。",
                  "pnputil has not finished after 2 minutes. Restart Windows to complete the update."));
            if (index + 1 < chosen.Count)
                log(T("次のドライバーは、まだ入れていません。再起動のあと、もう一度「更新する」を押してください: ",
                      "Not installed yet; press Update again after the restart: ") +
                    string.Join(", ", chosen.Skip(index + 1).Select(c => c.Service)));
            return ExitReboot;
        }

        /// <summary>Runs pnputil and logs its output. Returns its exit code, or null after the timeout (it keeps running).</summary>
        static async Task<int?> Pnputil(string args, Action<string> log)
        {
            var psi = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "pnputil.exe"), args)
            {
                UseShellExecute = false, RedirectStandardOutput = true, CreateNoWindow = true,
            };
            Process p = Process.Start(psi);
            Task<string> output = p.StandardOutput.ReadToEndAsync();
            using (var cts = new CancellationTokenSource(PnputilTimeout))
            {
                try { await p.WaitForExitAsync(cts.Token); }
                catch (OperationCanceledException) { return null; }
            }
            foreach (string line in (await output).Split('\n').Select(l => l.Trim()).Where(l => l.Length > 0)) log("  " + line);
            int code = p.ExitCode;
            p.Dispose();
            return code;
        }

        [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)] static extern int CM_Locate_DevNodeW(out uint devInst, string id, uint flags);
        [DllImport("cfgmgr32.dll")] static extern int CM_Get_DevNode_Registry_PropertyW(uint devInst, uint property, out uint type, ref uint buffer, ref uint length, uint flags);
        [DllImport("cfgmgr32.dll")] static extern int CM_Set_DevNode_Registry_PropertyW(uint devInst, uint property, ref uint buffer, uint length, uint flags);
        const uint CM_DRP_CONFIGFLAGS = 0x0B;
        public const int CONFIGFLAG_REINSTALL = 0x20;

        /// <summary>Marks each instance of the driver's device that has an older driver. Returns how many.</summary>
        static int MarkForReinstall(ManifestDriver d, Release release)
        {
            Version target = System.Version.Parse(d.Version);
            int marked = 0;
            foreach (DriverRow r in DriverStatus.Read(release))
            {
                if (!r.HardwareId.Equals(d.Hwid, StringComparison.OrdinalIgnoreCase) || r.Instance == "" || r.ReinstallPending) continue;
                if (System.Version.TryParse(r.Installed, out Version v) && v >= target) continue;
                if (CM_Locate_DevNodeW(out uint dev, r.Instance, 0) != 0) continue;
                uint flags = 0, length = 4;
                if (CM_Get_DevNode_Registry_PropertyW(dev, CM_DRP_CONFIGFLAGS, out _, ref flags, ref length, 0) != 0) flags = 0;
                flags |= CONFIGFLAG_REINSTALL;
                int cr = CM_Set_DevNode_Registry_PropertyW(dev, CM_DRP_CONFIGFLAGS, ref flags, 4, 0);
                if (cr != 0) throw new InvalidOperationException(T($"{r.Instance} に印を付けられませんでした（CR {cr}）。", $"Could not mark {r.Instance} (CR {cr})."));
                marked++;
            }
            return marked;
        }

        // ------------------------------------------------- control center itself

        static string UpdateRoot => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "SwitchWOA", "SwitchCenter", "update");

        /// <summary>Downloads and checks the new control center; returns the unpacked SwitchCenter.exe.</summary>
        public static async Task<string> DownloadAppAsync(Release release)
        {
            if (release.AppUrl == null) throw new InvalidDataException(release.AppFile + T(" がリリースにありません。", " is missing in the release."));
            string dir = Path.Combine(UpdateRoot, release.AppVersion + "-" + Guid.NewGuid().ToString("N").Substring(0, 8));
            Directory.CreateDirectory(dir);
            try
            {
                string zip = Path.Combine(dir, "app.zip");
                using (HttpClient http = NewClient())
                using (Stream src = await http.GetStreamAsync(release.AppUrl))
                using (FileStream dst = File.Create(zip))
                    await src.CopyToAsync(dst);
                string hash = Sha256(zip);
                if (hash != release.AppSha256)
                    throw new InvalidDataException(T($"ZIP の SHA-256 が manifest と合いません（{hash}）。", $"The ZIP's SHA-256 does not match the manifest ({hash})."));
                ZipFile.ExtractToDirectory(zip, dir);
                File.Delete(zip);
                string exe = Path.Combine(dir, "SwitchCenter", "SwitchCenter.exe");
                if (!File.Exists(exe)) throw new InvalidDataException(T("ZIP に SwitchCenter.exe がありません。", "SwitchCenter.exe is missing in the ZIP."));
                return exe;
            }
            catch
            {
                try { Directory.Delete(dir, true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
                throw;
            }
        }

        /// <summary>Starts the unpacked copy, which replaces this installation once this process has exited.</summary>
        public static void StartSelfUpdate(string newExe)
        {
            string target = AppContext.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar);
            Process.Start(new ProcessStartInfo(newExe, $"--self-update \"{target}\" {Environment.ProcessId}") { UseShellExecute = false })?.Dispose();
        }

        /// <summary>Runs in the unpacked copy: waits for the old process, copies itself over the target folder, starts it.</summary>
        public static void ApplySelfUpdate(string target, int pid)
        {
            try
            {
                if (!File.Exists(Path.Combine(target, "SwitchCenter.exe"))) throw new InvalidDataException(target);
                try { using Process old = Process.GetProcessById(pid); old.WaitForExit(30000); } catch (ArgumentException) { }   // already gone
                string source = AppContext.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar);
                foreach (string file in Directory.GetFiles(source, "*", SearchOption.AllDirectories))
                {
                    string dest = Path.Combine(target, Path.GetRelativePath(source, file));
                    Directory.CreateDirectory(Path.GetDirectoryName(dest));
                    for (int attempt = 0; ; attempt++)
                    {
                        try { File.Copy(file, dest, true); break; }
                        catch (IOException) when (attempt < 10) { Thread.Sleep(1000); }   // the old process may still hold it
                    }
                }
                Process.Start(new ProcessStartInfo(Path.Combine(target, "SwitchCenter.exe"), "--open") { UseShellExecute = false, WorkingDirectory = target })?.Dispose();
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException || e is InvalidDataException)
            {
                MessageBox.Show(T($"コントロールセンターを更新できませんでした。\n{e.Message}\n\nリリースの ZIP を手で展開して、入れ替えてください。",
                                  $"Could not update the control center.\n{e.Message}\n\nUnpack the release ZIP over it by hand."),
                                T("コントロールセンターの更新", "Control center update"), MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        /// <summary>Removes unpacked updates left from earlier runs (best effort).</summary>
        public static void CleanUpdates()
        {
            if (!Directory.Exists(UpdateRoot) || AppContext.BaseDirectory.StartsWith(UpdateRoot, StringComparison.OrdinalIgnoreCase)) return;
            foreach (string dir in Directory.GetDirectories(UpdateRoot))
                try { Directory.Delete(dir, true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
        }

    }

    /// <summary>The elevated process's window: progress log, then the result.</summary>
    sealed class InstallForm : Form
    {
        readonly TextBox logBox = new TextBox { Dock = DockStyle.Fill, Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical };
        readonly Button close = new Button { Dock = DockStyle.Bottom, Height = 48, Enabled = false };
        public int Result { get; private set; } = Updater.ExitFailed;

        public InstallForm(string tag, string[] services)
        {
            Text = T("ドライバーの更新", "Driver update");
            Font = new System.Drawing.Font("Yu Gothic UI", 10f);
            AutoScaleMode = AutoScaleMode.Dpi;
            StartPosition = FormStartPosition.CenterScreen;
            ClientSize = new System.Drawing.Size(560, 520);
            close.Text = T("閉じる", "Close");
            close.Click += (_, _) => Close();
            Controls.Add(logBox);
            Controls.Add(close);
            // Not by the user while pnputil runs, but never block a Windows restart or shutdown.
            FormClosing += (_, e) => { if (!close.Enabled && e.CloseReason == CloseReason.UserClosing) e.Cancel = true; };
            Shown += async (_, _) =>
            {
                try
                {
                    Result = await Updater.InstallAsync(tag, services, Log);
                    Log("");
                    Log(Result == Updater.ExitReboot ? T("完了しました。再起動すると新しいドライバーになります。", "Done. Restart to finish.")
                                                     : T("完了しました。", "Done."));
                }
                catch (Exception e)
                {
                    Result = Updater.ExitFailed;
                    Log("");
                    Log(T("中止しました: ", "Stopped: ") + e.Message);
                }
                close.Enabled = true;
            };
        }

        void Log(string line) => logBox.AppendText(line + Environment.NewLine);
    }
}
