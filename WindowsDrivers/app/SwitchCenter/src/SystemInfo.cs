// Stage 3 data: power plan settings (PowrProf), the state of each Switch
// driver (cfgmgr32 + the Enum / Class registry, no WMI) and the diagnostic
// report.
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;
using Microsoft.Win32;
using static SwitchCenter.Lang;

namespace SwitchCenter
{
    /// <summary>Settings of the active power plan, for AC and battery together.</summary>
    static class PowerSettings
    {
        [DllImport("powrprof.dll")] static extern uint PowerGetActiveScheme(IntPtr root, out IntPtr scheme);
        [DllImport("powrprof.dll")] static extern uint PowerReadACValueIndex(IntPtr root, ref Guid scheme, ref Guid sub, ref Guid setting, out uint value);
        [DllImport("powrprof.dll")] static extern uint PowerWriteACValueIndex(IntPtr root, ref Guid scheme, ref Guid sub, ref Guid setting, uint value);
        [DllImport("powrprof.dll")] static extern uint PowerWriteDCValueIndex(IntPtr root, ref Guid scheme, ref Guid sub, ref Guid setting, uint value);
        [DllImport("powrprof.dll")] static extern uint PowerSetActiveScheme(IntPtr root, ref Guid scheme);
        [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr mem);

        public static readonly Guid SubButtons = new Guid("4f971e89-eebd-4455-a8de-9e59040e7347");
        public static readonly Guid PowerButtonAction = new Guid("7648efa3-dd9c-4e3e-b566-50f929386280");   // 0 nothing 1 sleep 2 hibernate 3 shut down 4 display off
        public static readonly Guid SubVideo = new Guid("7516b95f-f776-4464-8c53-06167f40cc99");
        public static readonly Guid DisplayOffSeconds = new Guid("3c0bc021-c8a8-4e07-a973-6b14cbcb2b7e");   // 0 = never
        public static readonly Guid SubBattery = new Guid("e73a048d-bf27-4f12-9731-8b2076e8891f");
        public static readonly Guid CriticalAction = new Guid("637ea02f-bbcb-4015-8e2c-a1c7b9c0b546");      // 0 nothing 1 sleep 2 hibernate 3 shut down
        public static readonly Guid CriticalLevel = new Guid("9a66d8d7-4ff7-4ef9-b5a2-5a326ca2a469");       // percent

        static Guid ActiveScheme()
        {
            if (PowerGetActiveScheme(IntPtr.Zero, out IntPtr p) != 0) throw new InvalidOperationException("PowerGetActiveScheme");
            try { return Marshal.PtrToStructure<Guid>(p); }
            finally { LocalFree(p); }
        }

        /// <summary>The AC value (the Switch is mostly on a charger); null if it cannot be read.</summary>
        public static uint? Read(Guid sub, Guid setting)
        {
            try
            {
                Guid scheme = ActiveScheme();
                return PowerReadACValueIndex(IntPtr.Zero, ref scheme, ref sub, ref setting, out uint v) == 0 ? v : null;
            }
            catch (InvalidOperationException) { return null; }
        }

        /// <summary>Write AC and DC and re-apply the plan. Returns a Win32 error code, 0 on success.</summary>
        public static uint Write(Guid sub, Guid setting, uint value)
        {
            Guid scheme;
            try { scheme = ActiveScheme(); } catch (InvalidOperationException) { return 1; }
            uint e = PowerWriteACValueIndex(IntPtr.Zero, ref scheme, ref sub, ref setting, value);
            if (e == 0) e = PowerWriteDCValueIndex(IntPtr.Zero, ref scheme, ref sub, ref setting, value);
            if (e == 0) e = PowerSetActiveScheme(IntPtr.Zero, ref scheme);
            return e;
        }
    }

    /// <summary>One Switch device (ACPI\SWOAxxxx instance) and its driver.</summary>
    sealed class DriverRow
    {
        public string Name, HardwareId, Instance, Service, State, Installed, Running;
        public bool Ok, ReinstallPending;     // ReinstallPending: marked to get a new driver at the next boot
    }

    static class DriverStatus
    {
        [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)] static extern int CM_Locate_DevNodeW(out uint devInst, string id, uint flags);
        [DllImport("cfgmgr32.dll")] static extern int CM_Get_DevNode_Status(out uint status, out uint problem, uint devInst, uint flags);
        const uint DN_STARTED = 0x8, DN_HAS_PROBLEM = 0x400;

        /// <summary>The Switch devices this project adds to the DSDT, with their drivers.</summary>
        static readonly (string Hwid, string Service, string Ja, string En)[] Known =
        {
            ("SWOA0101", "tegra210gpio",  "GPIO コントローラー", "GPIO controller"),
            ("SWOA0201", "tegra210i2c",   "I2C コントローラー",  "I2C controller"),
            ("SWOA0301", "switchbattery", "バッテリー・USB-C",   "Battery, USB-C"),
            ("SWOA0401", "switchfan",     "ファン・バックライト", "Fan, backlight"),
            ("SWOA0501", "switchtouch",   "タッチパネル",        "Touch screen"),
            ("SWOA0601", "switchpwrbtn",  "電源ボタン",          "Power button"),
            ("SWOA0701", "switchcpu",     "CPU のクロック",      "CPU clock"),
            ("SWOA0801", "switchrtc",     "時計（RTC）",         "Clock (RTC)"),
        };

        /// <summary>The known devices, plus any other device the release has a driver for (named from the manifest).</summary>
        public static List<DriverRow> Read(Release release = null)
        {
            var devices = new List<(string Hwid, string Service, string Ja, string En)>(Known);
            if (release != null)
                foreach (ManifestDriver d in release.Drivers)
                {
                    string hwid = d.Hwid.Substring(d.Hwid.IndexOf('\\') + 1);
                    if (!devices.Exists(x => x.Hwid.Equals(hwid, StringComparison.OrdinalIgnoreCase)))
                        devices.Add((hwid, d.Service, d.NameJa ?? d.Service, d.NameEn ?? d.Service));
                }
            var rows = new List<DriverRow>();
            foreach (var k in devices)
            {
                string name = T(k.Ja, k.En);
                using RegistryKey enumKey = Registry.LocalMachine.OpenSubKey($@"SYSTEM\CurrentControlSet\Enum\ACPI\{k.Hwid}");
                string[] instances = enumKey?.GetSubKeyNames() ?? Array.Empty<string>();
                if (instances.Length == 0)
                {
                    rows.Add(new DriverRow
                    {
                        Name = name, HardwareId = "ACPI\\" + k.Hwid, Service = k.Service, Instance = "",
                        State = T("デバイスが無い（UEFI が古い？）", "no device (older UEFI?)"), Installed = "—", Running = "—",
                    });
                    continue;
                }
                foreach (string inst in instances)
                {
                    string id = $@"ACPI\{k.Hwid}\{inst}";
                    var row = new DriverRow { Name = instances.Length > 1 ? $"{name} #{inst}" : name, HardwareId = "ACPI\\" + k.Hwid, Instance = id, Service = k.Service };
                    if (CM_Locate_DevNodeW(out uint dev, id, 0) != 0)
                    {
                        row.State = T("今は見えていない", "not present now");
                    }
                    else if (CM_Get_DevNode_Status(out uint status, out uint problem, dev, 0) == 0)
                    {
                        row.Ok = (status & DN_STARTED) != 0 && (status & DN_HAS_PROBLEM) == 0;
                        row.State = row.Ok ? T("正常", "OK")
                                  : (status & DN_HAS_PROBLEM) != 0 ? T($"エラー（コード {problem}）", $"error (code {problem})")
                                  : T("停止中", "stopped");
                    }
                    else
                    {
                        row.State = T("不明", "unknown");
                    }
                    using (RegistryKey dk = Registry.LocalMachine.OpenSubKey($@"SYSTEM\CurrentControlSet\Enum\{id}"))
                    {
                        row.ReinstallPending = dk?.GetValue("ConfigFlags") is int flags && (flags & Updater.CONFIGFLAG_REINSTALL) != 0;
                        if (dk?.GetValue("Driver") is string cls)
                        {
                            using RegistryKey ck = Registry.LocalMachine.OpenSubKey($@"SYSTEM\CurrentControlSet\Control\Class\{cls}");
                            row.Installed = ck?.GetValue("DriverVersion") as string ?? "—";
                        }
                        else
                        {
                            row.Installed = T("ドライバー無し", "no driver");
                        }
                    }
                    // The I2C driver keeps one trace for all controllers, with I2C_ names.
                    row.Running = (DriverTrace.Get(k.Service, "DriverVersion") ?? DriverTrace.Get(k.Service, "I2C_0000_DriverVersion")) is int v ? "v" + v : "—";
                    rows.Add(row);
                }
            }
            return rows;
        }
    }

    /// <summary>"Save information for troubleshooting": one ZIP with a text report.</summary>
    static class DiagnosticReport
    {
        static readonly string[] Services =
            { "tegra210gpio", "tegra210i2c", "switchbattery", "switchfan", "switchtouch", "switchpwrbtn", "switchcpu", "switchrtc" };

        public static string BuildText(SystemUsage usage)
        {
            var r = new StringBuilder();
            r.AppendLine($"Switch Control Center {Application.ProductVersion}  {DateTime.Now:yyyy-MM-dd HH:mm:ss}");
            r.AppendLine($"OS: {RuntimeInformation.OSDescription} ({RuntimeInformation.OSArchitecture}), runtime {RuntimeInformation.FrameworkDescription}");
            r.AppendLine($"CPU usage: {usage.Total}%  per core: {string.Join(" ", usage.PerCore)}  memory: {usage.MemoryUsedMb}/{usage.MemoryTotalMb} MB");
            PowerStatus ps = SystemInformation.PowerStatus;
            r.AppendLine($"Windows power status: {ps.PowerLineStatus}, {ps.BatteryChargeStatus}, {ps.BatteryLifePercent:P0}");
            r.AppendLine();
            r.AppendLine("== Devices");
            foreach (DriverRow d in DriverStatus.Read())
                r.AppendLine($"{d.Instance,-22} {d.Service,-14} state={d.State} installed={d.Installed} running={d.Running}");
            foreach (string s in Services)
            {
                r.AppendLine();
                r.AppendLine($"== {s} trace");
                using RegistryKey k = Registry.LocalMachine.OpenSubKey($@"SYSTEM\CurrentControlSet\Services\{s}\Parameters");
                if (k == null) { r.AppendLine("(none)"); continue; }
                foreach (string name in k.GetValueNames())
                {
                    object v = k.GetValue(name);
                    r.AppendLine(v is int i ? $"{name} = 0x{i:X8} ({i})" : v is long l ? $"{name} = {l}" : $"{name} = {v}");
                }
            }
            return r.ToString();
        }

        /// <summary>Writes SwitchCenter-report-*.zip to the desktop and returns its path.</summary>
        public static string Save(string text)
        {
            string folder = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
            string path = Path.Combine(folder, $"SwitchCenter-report-{DateTime.Now:yyyyMMdd-HHmmss}.zip");
            using ZipArchive zip = ZipFile.Open(path, ZipArchiveMode.Create);
            ZipArchiveEntry entry = zip.CreateEntry("report.txt");
            using (var w = new StreamWriter(entry.Open(), new UTF8Encoding(true))) w.Write(text);
            foreach (string log in new[] { @"C:\SwitchWOA-Battery.txt", @"C:\SwitchWOA-Touch.txt", @"C:\SwitchWOA-CpuClock.txt", @"C:\SwitchWOA-Rtc.txt", @"C:\SwitchWOA-UsbDiag.txt" })
                if (File.Exists(log)) zip.CreateEntryFromFile(log, Path.GetFileName(log));
            return path;
        }
    }
}
