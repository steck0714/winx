// Changes to a Windows that is not running: its SOFTWARE registry hive
// (loaded with reg.exe under HKLM\SWOA_PATCH_SOFTWARE), certificates written
// straight into its certificate stores, and the boot store on the ESP.
using System;
using System.IO;
using System.Security.Cryptography.X509Certificates;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32;
using static SwitchCenter.Lang;

namespace SwitchPatcher
{
    static class Offline
    {
        /// <summary>Loads the offline SOFTWARE hive, runs body on it, and always unloads it.</summary>
        public static Task WithSoftwareHive(string windows, Action<RegistryKey> body, Action<string> log) =>
            WithHive(windows, "SOFTWARE", body, log);

        /// <summary>Loads the offline hive (SOFTWARE, SYSTEM) as HKLM\SWOA_PATCH_name, runs body on it, and always unloads it.</summary>
        public static async Task WithHive(string windows, string name, Action<RegistryKey> body, Action<string> log)
        {
            string mount = "SWOA_PATCH_" + name;
            string hive = Path.Combine(windows, @"Windows\System32\config\" + name);
            if (!File.Exists(hive)) throw new FileNotFoundException(hive);
            if (await Tools.Run(Tools.System32("reg.exe"), $"load HKLM\\{mount} \"{hive}\"", log) != 0)
                throw new InvalidOperationException(T($"レジストリ（{name}）を読み込めませんでした。", $"Could not load the {name} hive."));
            try
            {
                using RegistryKey k = Registry.LocalMachine.OpenSubKey(mount, true);
                body(k);
            }
            finally
            {
                // Every handle into the hive must be closed before reg unload succeeds.
                for (int attempt = 0; ; attempt++)
                {
                    GC.Collect();
                    GC.WaitForPendingFinalizers();
                    if (await Tools.Run(Tools.System32("reg.exe"), $"unload HKLM\\{mount}", log) == 0) break;
                    if (attempt == 4) throw new InvalidOperationException(T($"レジストリ（{name}）を閉じられませんでした。PC を再起動してから SD を外してください。",
                                                                           $"Could not unload the {name} hive. Restart the PC before removing the SD."));
                    Thread.Sleep(1000);
                }
            }
        }

        /// <summary>
        /// Loads the SD host and SD storage drivers at every boot (Start = 0), in every control set.
        /// The Switch UEFI hands Windows the SD as a vendor device path, so winload does not see
        /// an SD boot and skips drivers that only load for one (BootFlags 0x8):
        /// without this a fresh image stops with INACCESSIBLE_BOOT_DEVICE.
        /// Returns "service: old -> 0" lines.
        /// </summary>
        public static System.Collections.Generic.List<string> LoadSdDriversAtBoot(RegistryKey system)
        {
            var changes = new System.Collections.Generic.List<string>();
            foreach (string cs in system.GetSubKeyNames())
            {
                if (!cs.StartsWith("ControlSet", StringComparison.OrdinalIgnoreCase)) continue;
                foreach (string svc in new[] { "sdbus", "sdstor" })
                {
                    using RegistryKey k = system.OpenSubKey($@"{cs}\Services\{svc}", true);
                    if (k == null) throw new InvalidOperationException($@"{cs}\Services\{svc} not found");
                    object old = k.GetValue("Start");
                    k.SetValue("Start", 0, RegistryValueKind.DWord);
                    changes.Add($@"{cs}\{svc}: Start {old ?? "-"} -> 0");
                }
            }
            return changes;
        }

        /// <summary>The build number of the offline Windows (19044 = Windows 10 21H2, 22621 = Windows 11 22H2).</summary>
        public static int WindowsBuild(string windows) =>
            System.Diagnostics.FileVersionInfo.GetVersionInfo(Path.Combine(windows, @"Windows\System32\ntoskrnl.exe")).FileBuildPart;

        /// <summary>
        /// Makes service a boot-start kernel driver (Start = 0, as the boot loader must
        /// load the SD host before Windows can read the SD) in every control set.
        /// Returns "service: old -> 0" lines.
        /// </summary>
        public static System.Collections.Generic.List<string> AddBootDriver(RegistryKey system, string service)
        {
            var changes = new System.Collections.Generic.List<string>();
            foreach (string cs in system.GetSubKeyNames())
            {
                if (!cs.StartsWith("ControlSet", StringComparison.OrdinalIgnoreCase)) continue;
                using RegistryKey k = system.CreateSubKey($@"{cs}\Services\{service}", true);
                object old = k.GetValue("Start");
                k.SetValue("Type", 1, RegistryValueKind.DWord);
                k.SetValue("Start", 0, RegistryValueKind.DWord);
                k.SetValue("ErrorControl", 1, RegistryValueKind.DWord);
                k.SetValue("BootFlags", 8, RegistryValueKind.DWord);
                k.SetValue("ImagePath", $@"\SystemRoot\System32\drivers\{service}.sys", RegistryValueKind.ExpandString);
                k.SetValue("Group", "System Bus Extender", RegistryValueKind.String);
                changes.Add($@"{cs}\{service}: Start {old ?? "-"} -> 0");
            }
            return changes;
        }

        /// <summary>
        /// Copies the driver's .sys to Windows\System32\drivers, replacing one left by an
        /// earlier run (owned by TrustedInstaller after dism, so it is taken over first).
        /// </summary>
        public static async Task CopyDriverFile(string windows, string sys, Action<string> log)
        {
            string dest = Path.Combine(windows, @"Windows\System32\drivers", Path.GetFileName(sys));
            if (File.Exists(dest))
            {
                await Tools.Run(Tools.System32("takeown.exe"), $"/f \"{dest}\" /a", log);
                await Tools.Run(Tools.System32("icacls.exe"), $"\"{dest}\" /grant *S-1-5-32-544:F", log);
                File.SetAttributes(dest, FileAttributes.Normal);
            }
            File.Copy(sys, dest, true);
        }

        /// <summary>Puts the SD loader at EFI\Boot\bootaa64.efi (the file the Switch UEFI starts) and checks the copy.</summary>
        public static void PutLoader(string esp, string loader)
        {
            string dest = Path.Combine(esp, @"EFI\Boot\bootaa64.efi");
            Directory.CreateDirectory(Path.GetDirectoryName(dest));
            if (File.Exists(dest)) File.SetAttributes(dest, FileAttributes.Normal);
            File.Copy(loader, dest, true);
            if (SwitchCenter.ReleaseClient.Sha256(dest) != SwitchCenter.ReleaseClient.Sha256(loader))
                throw new IOException(T("ESP に置いたローダーが元のファイルと合いません。", "The loader on the ESP does not match its source."));
        }

        /// <summary>
        /// Removes the boot debugging settings used while bringing up Windows 11 on the SD
        /// (boot log, driver names on screen, all failures shown, no restart after a stop).
        /// A value that is not set is left alone.
        /// </summary>
        public static async Task ClearBootDebug(string bcd)
        {
            foreach (string v in new[] { "bootlog", "sos", "bootstatuspolicy", "nocrashautoreboot" })
                await Tools.Run(Tools.System32("bcdedit.exe"), $"/store \"{bcd}\" /deletevalue {{default}} {v}", _ => { });
        }

        /// <summary>
        /// The registry "Blob" Windows keeps for a certificate in a store. Made by
        /// adding the certificate to a temporary store of this user and reading
        /// it back, so the format is exactly Windows' own.
        /// </summary>
        public static byte[] CertificateBlob(X509Certificate2 cert)
        {
            const string temp = "SwitchPatcherTemp";
            using (var store = new X509Store(temp, StoreLocation.CurrentUser))
            {
                store.Open(OpenFlags.ReadWrite);
                store.Add(cert);
            }
            try
            {
                using RegistryKey k = Registry.CurrentUser.OpenSubKey($@"Software\Microsoft\SystemCertificates\{temp}\Certificates\{cert.Thumbprint}");
                return k?.GetValue("Blob") as byte[] ?? throw new InvalidOperationException("certificate blob not found");
            }
            finally
            {
                Registry.CurrentUser.DeleteSubKeyTree($@"Software\Microsoft\SystemCertificates\{temp}", false);
            }
        }

        /// <summary>Puts the certificate into the offline machine store (ROOT, TrustedPublisher, ...).</summary>
        public static void AddCertificate(RegistryKey software, string store, string thumbprint, byte[] blob)
        {
            using RegistryKey k = software.CreateSubKey($@"Microsoft\SystemCertificates\{store}\Certificates\{thumbprint}");
            k.SetValue("Blob", blob, RegistryValueKind.Binary);
        }

        /// <summary>Starts a program at every login (HKLM Run).</summary>
        public static void AddRun(RegistryKey software, string name, string command)
        {
            using RegistryKey k = software.CreateSubKey(@"Microsoft\Windows\CurrentVersion\Run");
            k.SetValue(name, command, RegistryValueKind.String);
        }

        /// <summary>Lets the first-boot setup continue without a network (the Switch has no Wi-Fi driver yet).</summary>
        public static void AllowOobeWithoutNetwork(RegistryKey software)
        {
            using RegistryKey k = software.CreateSubKey(@"Microsoft\Windows\CurrentVersion\OOBE");
            k.SetValue("BypassNRO", 1, RegistryValueKind.DWord);
        }

        /// <summary>
        /// First-boot answers: skip the Microsoft account and Wi-Fi pages. No
        /// account or password is set here; the user creates a local account in
        /// the setup screens.
        /// </summary>
        public static void WriteUnattend(string windows)
        {
            string dir = Path.Combine(windows, @"Windows\Panther");
            Directory.CreateDirectory(dir);
            File.WriteAllText(Path.Combine(dir, "unattend.xml"), @"<?xml version=""1.0"" encoding=""utf-8""?>
<unattend xmlns=""urn:schemas-microsoft-com:unattend"">
  <settings pass=""oobeSystem"">
    <component name=""Microsoft-Windows-Shell-Setup"" processorArchitecture=""arm64"" publicKeyToken=""31bf3856ad364e35"" language=""neutral"" versionScope=""nonSxS"" xmlns:wcm=""http://schemas.microsoft.com/WMIConfig/2002/State"">
      <OOBE>
        <HideOnlineAccountScreens>true</HideOnlineAccountScreens>
        <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>
        <ProtectYourPC>3</ProtectYourPC>
      </OOBE>
    </component>
  </settings>
</unattend>
");
        }

        /// <summary>Copies the ESP's Windows boot files to the PC before they are changed. Returns the backup folder.</summary>
        public static string BackupEsp(string esp)
        {
            string backup = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "SwitchPatcher",
                                         "backup-" + DateTime.Now.ToString("yyyyMMdd-HHmmss"));
            foreach (string sub in new[] { @"EFI\Microsoft\Boot", @"EFI\Boot" })
            {
                string src = Path.Combine(esp, sub);
                if (!Directory.Exists(src)) continue;
                foreach (string f in Directory.GetFiles(src, "*", SearchOption.AllDirectories))
                {
                    string dest = Path.Combine(backup, sub, Path.GetRelativePath(src, f));
                    Directory.CreateDirectory(Path.GetDirectoryName(dest));
                    File.Copy(f, dest, true);
                }
            }
            Directory.CreateDirectory(backup);
            return backup;
        }

        /// <summary>
        /// What bcdboot /f UEFI does, by hand: copy Windows\Boot\EFI to the ESP and build a fresh BCD.
        /// Used when the PC's bcdboot refuses another build's ARM64 boot files (seen with 22621 on a 26100 PC: exit 193).
        /// </summary>
        public static async Task MakeBootFiles(string windows, string esp, Action<string> log)
        {
            string w = windows.TrimEnd('\\'), s = esp.TrimEnd('\\');
            string boot = Path.Combine(esp, @"EFI\Microsoft\Boot");
            string mgr = Path.Combine(windows, @"Windows\Boot\EFI\bootmgfw.efi");
            if (!File.Exists(mgr)) throw new FileNotFoundException(mgr);
            CopyTree(Path.Combine(windows, @"Windows\Boot\EFI"), boot);
            CopyTree(Path.Combine(windows, @"Windows\Boot\Fonts"), Path.Combine(boot, "Fonts"));
            Directory.CreateDirectory(Path.Combine(esp, @"EFI\Boot"));
            File.Copy(mgr, Path.Combine(esp, @"EFI\Boot\bootaa64.efi"), true);

            string bcd = Path.Combine(boot, "BCD");
            foreach (string f in new[] { bcd, bcd + ".LOG", bcd + ".LOG1", bcd + ".LOG2" })
                if (File.Exists(f)) { File.SetAttributes(f, FileAttributes.Normal); File.Delete(f); }

            string created = null;
            async Task Bcd(string args)
            {
                if (await Tools.Run(Tools.System32("bcdedit.exe"), $"/store \"{bcd}\" " + args, line =>
                    {
                        log(line);
                        var m = System.Text.RegularExpressions.Regex.Match(line, @"\{[0-9a-fA-F-]{36}\}");
                        if (m.Success) created = m.Value;
                    }) != 0)
                    throw new InvalidOperationException("bcdedit " + args);
            }
            if (await Tools.Run(Tools.System32("bcdedit.exe"), $"/createstore \"{bcd}\"", log) != 0)
                throw new InvalidOperationException("bcdedit /createstore");
            await Bcd("/create {bootmgr} /d \"Windows Boot Manager\"");
            await Bcd($"/set {{bootmgr}} device partition={s}");
            await Bcd(@"/set {bootmgr} path \EFI\Microsoft\Boot\bootmgfw.efi");
            await Bcd("/set {bootmgr} locale en-US");
            await Bcd("/set {bootmgr} timeout 0");
            created = null;
            await Bcd("/create /d \"Windows\" /application osloader");
            string id = created ?? throw new InvalidOperationException("bcdedit did not return the new entry");
            await Bcd($"/set {id} device partition={w}");
            await Bcd($"/set {id} osdevice partition={w}");
            await Bcd($@"/set {id} path \Windows\system32\winload.efi");
            await Bcd($@"/set {id} systemroot \Windows");
            await Bcd($"/set {id} locale en-US");
            await Bcd($"/displayorder {id}");
            await Bcd($"/default {id}");

            // Mark it the system store, as bcdboot does. Without Description\System = 1 the
            // specialize pass of Windows Setup fails ("File is not system store").
            if (await Tools.Run(Tools.System32("reg.exe"), $"load HKLM\\SWOA_PATCH_BCD \"{bcd}\"", log) != 0)
                throw new InvalidOperationException("reg load BCD");
            try
            {
                using RegistryKey k = Registry.LocalMachine.CreateSubKey(@"SWOA_PATCH_BCD\Description");
                k.SetValue("System", 1, RegistryValueKind.DWord);
            }
            finally
            {
                GC.Collect();
                GC.WaitForPendingFinalizers();
                if (await Tools.Run(Tools.System32("reg.exe"), @"unload HKLM\SWOA_PATCH_BCD", log) != 0)
                    throw new InvalidOperationException("reg unload BCD");
            }
        }

        static void CopyTree(string src, string dest)
        {
            if (!Directory.Exists(src)) return;
            foreach (string f in Directory.GetFiles(src, "*", SearchOption.AllDirectories))
            {
                string to = Path.Combine(dest, Path.GetRelativePath(src, f));
                Directory.CreateDirectory(Path.GetDirectoryName(to));
                if (File.Exists(to)) File.SetAttributes(to, FileAttributes.Normal);
                File.Copy(f, to, true);
            }
        }
    }
}
