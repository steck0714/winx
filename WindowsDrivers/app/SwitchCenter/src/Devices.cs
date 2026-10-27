// Access to the Switch drivers. All IOCTL structures are ULONG/LONG fields
// (see SwitchCpuClock\src\switchcpu_ioctl.h and SwitchFan\src\switchfan.c).
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;

namespace SwitchCenter
{
    static class Native
    {
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr sa, uint disposition, uint flags, IntPtr template);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] inBuf, int inLen, byte[] outBuf, int outLen, out int returned, IntPtr ov);

        /// <summary>One IOCTL on \\.\Name. Throws Win32Exception on failure.</summary>
        public static byte[] Ioctl(string device, uint code, byte[] input, int outLen)
        {
            using SafeFileHandle h = CreateFile(device, 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
            if (h.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
            byte[] output = new byte[outLen];
            if (!DeviceIoControl(h, code, input, input?.Length ?? 0, output, outLen, out _, IntPtr.Zero))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            return output;
        }

        public static int I32(byte[] b, int index) => BitConverter.ToInt32(b, index * 4);
    }

    /// <summary>switchcpu.sys (\\.\SwitchCpu): CPU profiles, clock, voltage, thermal guard.</summary>
    static class CpuClock
    {
        const string Device = @"\\.\SwitchCpu";
        const uint GetStatusCode = 0x00226000, GetProfilesCode = 0x00226004, SetProfileCode = 0x0022A008;
        public const int FlagExperimental = 1, FlagDefault = 2;

        public sealed class Status
        {
            public int Khz, Mv, Active, Requested, TempSoC, TempBoard, Switches;
            public bool Throttled, TempValid;
        }

        public sealed class Profile
        {
            public int Index, Khz, Mv, Flags;
            public bool Experimental => (Flags & FlagExperimental) != 0;
            public bool Default => (Flags & FlagDefault) != 0;
        }

        public static Status GetStatus()
        {
            byte[] b = Native.Ioctl(Device, GetStatusCode, null, 48);
            return new Status
            {
                Khz = Native.I32(b, 1), Mv = Native.I32(b, 2), Active = Native.I32(b, 3), Requested = Native.I32(b, 4),
                Throttled = Native.I32(b, 5) != 0, TempSoC = Native.I32(b, 6), TempBoard = Native.I32(b, 7),
                TempValid = Native.I32(b, 8) != 0, Switches = Native.I32(b, 10),
            };
        }

        public static Profile[] GetProfiles()
        {
            byte[] b = Native.Ioctl(Device, GetProfilesCode, null, 4 + 8 * 12);
            int count = Math.Min(Native.I32(b, 0), 8);
            var list = new Profile[count];
            for (int i = 0; i < count; i++)
                list[i] = new Profile { Index = i, Khz = Native.I32(b, 1 + i * 3), Mv = Native.I32(b, 2 + i * 3), Flags = Native.I32(b, 3 + i * 3) };
            return list;
        }

        public static void SetProfile(int index) => Native.Ioctl(Device, SetProfileCode, BitConverter.GetBytes(index), 0);
    }

    /// <summary>switchfan.sys v2 (\\.\SwitchBacklight): brightness 8..255 and the display state.</summary>
    static class Backlight
    {
        const string Device = @"\\.\SwitchBacklight";
        const uint GetCode = 0x00226040, SetCode = 0x0022A044;
        public const int Min = 8, Max = 255;

        public sealed class Status
        {
            public int Level, Duty, DisplayState;      // DisplayState: 0 off, 1 on, 2 dimmed
        }

        public static Status Get()
        {
            byte[] b = Native.Ioctl(Device, GetCode, null, 16);
            return new Status { Level = Native.I32(b, 1), Duty = Native.I32(b, 2), DisplayState = Native.I32(b, 3) };
        }

        public static void Set(int level) => Native.Ioctl(Device, SetCode, BitConverter.GetBytes(Math.Clamp(level, Min, Max)), 0);

        public static int PercentToLevel(int percent) => Math.Max(Min, percent * Max / 100);
        public static int LevelToPercent(int level) => level * 100 / Max;
    }

    /// <summary>The DWORD traces every driver writes under Services\name\Parameters.</summary>
    static class DriverTrace
    {
        public static int? Get(string service, string name)
        {
            using RegistryKey k = Registry.LocalMachine.OpenSubKey($@"SYSTEM\CurrentControlSet\Services\{service}\Parameters");
            return k?.GetValue(name) is int v ? v : null;
        }
    }

    /// <summary>Battery and USB-C state from switchbattery.sys (updated every 10 s / on change).</summary>
    sealed class BatteryInfo
    {
        // BATTERY_STATUS.PowerState bits
        const int OnLine = 1, Discharging = 2, Charging = 4;

        public int? Percent, Millivolts, PowerState, UsbcState;

        public static BatteryInfo Read() => new BatteryInfo
        {
            Percent = DriverTrace.Get("switchbattery", "LastCapacity"),
            Millivolts = DriverTrace.Get("switchbattery", "LastVoltage"),
            PowerState = DriverTrace.Get("switchbattery", "LastPowerState"),
            // UsbcState is written on changes only; before the first change the start state applies.
            UsbcState = DriverTrace.Get("switchbattery", "UsbcState") ?? DriverTrace.Get("switchbattery", "UsbcStartState"),
        };

        public string PowerText => PowerState is not int s ? Lang.T("不明", "unknown")
            : (s & Charging) != 0 ? Lang.T("AC・充電中", "AC, charging")
            : (s & OnLine) != 0 ? Lang.T("AC（充電していない）", "AC, not charging")
            : (s & Discharging) != 0 ? Lang.T("バッテリー", "battery") : Lang.T("不明", "unknown");

        public string UsbcText => UsbcState switch
        {
            0 => Lang.T("何もつながっていない", "nothing attached"),
            1 => Lang.T("機器（本体から 5V）", "device (Switch supplies 5 V)"),
            2 => Lang.T("充電器・ハブ", "charger / hub"),
            _ => Lang.T("不明", "unknown"),
        };
    }

    /// <summary>CPU usage (total and per core) and memory use, from deltas between two Sample() calls.</summary>
    sealed class SystemUsage
    {
        [StructLayout(LayoutKind.Sequential)]
        struct ProcessorTimes          // SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION
        {
            public long Idle, Kernel, User, Dpc, Interrupt;
            public uint InterruptCount;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct MemoryStatus            // MEMORYSTATUSEX
        {
            public uint Length, Load;
            public ulong TotalPhys, AvailPhys, TotalPageFile, AvailPageFile, TotalVirtual, AvailVirtual, AvailExtendedVirtual;
        }

        [DllImport("ntdll.dll")]
        static extern int NtQuerySystemInformation(int infoClass, [Out] ProcessorTimes[] info, int length, out int returned);

        [DllImport("kernel32.dll")]
        static extern bool GlobalMemoryStatusEx(ref MemoryStatus status);

        const int SystemProcessorPerformanceInformation = 8;
        ProcessorTimes[] previous;

        /// <summary>Total CPU usage in percent (null on the first call), per core, and memory.</summary>
        public int? Total { get; private set; }
        public int[] PerCore { get; private set; } = Array.Empty<int>();
        public int MemoryUsedMb { get; private set; }
        public int MemoryTotalMb { get; private set; }

        public void Sample()
        {
            var now = new ProcessorTimes[Environment.ProcessorCount];
            if (NtQuerySystemInformation(SystemProcessorPerformanceInformation, now, now.Length * Marshal.SizeOf<ProcessorTimes>(), out _) == 0)
            {
                if (previous != null)
                {
                    long busySum = 0, allSum = 0;
                    var cores = new int[now.Length];
                    for (int i = 0; i < now.Length; i++)
                    {
                        long all = (now[i].Kernel - previous[i].Kernel) + (now[i].User - previous[i].User);   // kernel includes idle
                        long busy = all - (now[i].Idle - previous[i].Idle);
                        cores[i] = all > 0 ? (int)Math.Clamp(busy * 100 / all, 0, 100) : 0;
                        busySum += busy;
                        allSum += all;
                    }
                    PerCore = cores;
                    Total = allSum > 0 ? (int)Math.Clamp(busySum * 100 / allSum, 0, 100) : 0;
                }
                previous = now;
            }
            var mem = new MemoryStatus { Length = (uint)Marshal.SizeOf<MemoryStatus>() };
            if (GlobalMemoryStatusEx(ref mem))
            {
                MemoryTotalMb = (int)(mem.TotalPhys / (1024 * 1024));
                MemoryUsedMb = (int)((mem.TotalPhys - mem.AvailPhys) / (1024 * 1024));
            }
        }
    }

    /// <summary>Fan duty from switchfan.sys (0..236; 236 = 100 %).</summary>
    static class FanInfo
    {
        public static int? Percent => DriverTrace.Get("switchfan", "Duty") is int d ? d * 100 / 236 : null;
    }
}
