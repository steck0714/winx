// Automatic screen brightness (light sensor) and orientation (accelerometer),
// from switchsensor.sys. Settings in HKCU\Software\SwitchWOA\SwitchClock:
// AutoBrightness, AutoRotate (0/1), RotateUp0..3 (the DMDO_* orientation the
// user taught for each chip axis pointing up, -1 = not taught), RotateReverse
// (0/1, used only until two poses are taught) and RotateBase (0.6.0: the
// orientation for up axis 0).
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Windows.Forms;
using Microsoft.Win32;

namespace SwitchCenter
{
    sealed class AutoScreen : IDisposable
    {
        const string SettingsKey = @"Software\SwitchWOA\SwitchClock";
        const int TickMs = 250;
        const int StableTicks = 4;              // an orientation must hold for 1 s
        const int UpMg = 700, FlatMg = 750;     // in-plane gravity needed / lying flat above this

        readonly Timer timer = new Timer { Interval = TickMs };
        double smoothLux = -1;
        bool fading;
        int candidate = -1, candidateTicks, lastUp = -1;

        /// <summary>The last sensor values (null when the driver is not there).</summary>
        public Sensors.Status Last { get; private set; }
        public string LastError { get; private set; }

        public AutoScreen()
        {
            timer.Tick += (_, _) => Tick();
            Update();
        }

        public bool AutoBrightness
        {
            get => ReadInt("AutoBrightness", 0) != 0;
            set { WriteInt("AutoBrightness", value ? 1 : 0); smoothLux = -1; Update(); }
        }

        public bool AutoRotate
        {
            get => ReadInt("AutoRotate", 0) != 0;
            set
            {
                // The orientation when it is turned on counts as "held normally" if it is a landscape one.
                if (value && !AnyTaught() && Display.Current() is int now && now % 2 == 1)
                    WriteInt("RotateUp0", now);
                WriteInt("AutoRotate", value ? 1 : 0);
                candidate = -1;
                lastUp = -1;
                Update();
            }
        }

        public bool RotateReverse
        {
            get => ReadInt("RotateReverse", 0) != 0;
            set { WriteInt("RotateReverse", value ? 1 : 0); lastUp = -1; }
        }

        /// <summary>
        /// Remember the current orientation as right for how the Switch is held now. Two poses a
        /// quarter turn apart (e.g. landscape and portrait) also settle which way it turns.
        /// False when the Switch is flat or tilted too far to tell.
        /// </summary>
        public bool SetBaseFromNow()
        {
            if (Display.Current() is not int now || Last is not { AccelValid: true } s || UpAxis(s) is not int up) return false;
            // A taught pose that disagrees with this one in direction is from an earlier mistake: forget it.
            for (int i = 0; i < 4; i++)
            {
                int other = Taught(i);
                if (i != up && other >= 0 && Direction(up, now, i, other) == 0) WriteInt("RotateUp" + i, -1);
            }
            WriteInt("RotateUp" + up, now);
            lastUp = up;
            return true;
        }

        /// <summary>Turn the screen a quarter turn by hand (to put it right before "remember").</summary>
        public static bool TurnScreen() => Display.Current() is int now && Display.Set((now + 1) % 4);

        /// <summary>The up axis for the value display: "+Y" etc., or null.</summary>
        public static string UpAxisName(Sensors.Status s) => UpAxis(s) switch { 0 => "+Y", 1 => "+X", 2 => "-Y", 3 => "-X", _ => null };

        /// <summary>Keep sampling while the window shows the values, even with both features off.</summary>
        public bool Watching { get => watching; set { watching = value; Update(); } }
        bool watching;

        void Update()
        {
            if (AutoBrightness || AutoRotate || watching) timer.Start(); else timer.Stop();
        }

        void Tick()
        {
            Sensors.Status s;
            try { s = Sensors.Get(); LastError = null; }
            catch (Win32Exception e) { Last = null; LastError = e.Message; return; }
            Last = s;
            if (AutoBrightness && s.LightValid) Brighten(s.Lux);
            if (AutoRotate && s.AccelValid) Rotate(s);
        }

        // ---------------------------------------------------------- brightness

        /// <summary>Brightness in percent for a light level: 12% in the dark, 100% from about 1000 lux.</summary>
        public static int PercentForLux(double lux) => (int)Math.Clamp(12 + 30 * Math.Log10(1 + lux), 12, 100);

        void Brighten(double lux)
        {
            // Smooth in log space (about 2 s), then fade the backlight a few steps per tick.
            double l = Math.Log10(1 + lux);
            smoothLux = smoothLux < 0 ? l : smoothLux + (l - smoothLux) * 0.12;
            Backlight.Status b;
            try { b = Backlight.Get(); } catch (Win32Exception) { return; }
            if (b.DisplayState == 0) return;
            int target = Backlight.PercentToLevel(PercentForLux(Math.Pow(10, smoothLux) - 1));
            int diff = target - b.Level;
            if (!fading && Math.Abs(diff) < 10) return;     // ignore small changes
            fading = Math.Abs(diff) > 2;
            if (!fading) return;
            int step = Math.Sign(diff) * Math.Max(2, Math.Abs(diff) / 4);
            try { Backlight.Set(b.Level + step); } catch (Win32Exception) { }
        }

        // ------------------------------------------------------------ rotation

        /// <summary>Which chip axis points up: 0 +Y, 1 +X, 2 -Y, 3 -X; null when flat or unclear.</summary>
        static int? UpAxis(Sensors.Status s)
        {
            int ax = Math.Abs(s.X), ay = Math.Abs(s.Y);
            if (Math.Abs(s.Z) > FlatMg || Math.Max(ax, ay) < UpMg || Math.Max(ax, ay) < Math.Min(ax, ay) * 3 / 2) return null;
            return ay >= ax ? (s.Y > 0 ? 0 : 2) : (s.X > 0 ? 1 : 3);
        }

        int Taught(int up)
        {
            int v = ReadInt("RotateUp" + up, -2);
            if (v == -2) v = up == 0 ? ReadInt("RotateBase", -1) : -1;   // never written: the 0.6.0 setting for axis 0
            return v is >= 0 and <= 3 ? v : -1;
        }

        bool AnyTaught() => Taught(0) >= 0 || Taught(1) >= 0 || Taught(2) >= 0 || Taught(3) >= 0;

        /// <summary>+1 if the picture turns with the axis index, -1 if against it, 0 if the two poses contradict.</summary>
        static int Direction(int upA, int orientA, int upB, int orientB)
        {
            int axes = ((upB - upA) % 4 + 4) % 4, turns = ((orientB - orientA) % 4 + 4) % 4;
            if (axes == 0) return turns == 0 ? 1 : 0;
            if (axes == 2) return turns == 2 ? 1 : 0;           // half turn: fits either direction
            return turns == axes ? 1 : turns == 4 - axes ? -1 : 0;
        }

        /// <summary>The orientation for an up axis: taught, or worked out from a taught one.</summary>
        int? OrientationFor(int up)
        {
            if (Taught(up) is int t && t >= 0) return t;
            int known = -1, dir = RotateReverse ? -1 : 1;      // the chip axes advance with the turns unless taught otherwise
            for (int i = 0; i < 4; i++)
            {
                if (Taught(i) < 0) continue;
                if (known >= 0 && (i - known) % 2 != 0) dir = Direction(known, Taught(known), i, Taught(i)) is int d && d != 0 ? d : dir;
                if (known < 0) known = i;
            }
            if (known < 0) return null;
            return ((Taught(known) + dir * (up - known)) % 4 + 8) % 4;
        }

        void Rotate(Sensors.Status s)
        {
            if (UpAxis(s) is not int up) { candidate = -1; return; }
            if (up != candidate) { candidate = up; candidateTicks = 0; return; }
            if (++candidateTicks < StableTicks || up == lastUp) return;
            lastUp = up;
            if (OrientationFor(up) is int target) Display.Set(target);
        }

        // ------------------------------------------------------------ settings

        static int ReadInt(string name, int fallback)
        {
            using RegistryKey k = Registry.CurrentUser.OpenSubKey(SettingsKey);
            return k?.GetValue(name) is int v ? v : fallback;
        }

        static void WriteInt(string name, int value)
        {
            using RegistryKey k = Registry.CurrentUser.CreateSubKey(SettingsKey);
            k.SetValue(name, value);
        }

        public void Dispose() => timer.Dispose();
    }

    /// <summary>The primary display's orientation (DMDO_DEFAULT 0, DMDO_90 1, DMDO_180 2, DMDO_270 3).</summary>
    static class Display
    {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct DevMode
        {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
            public ushort SpecVersion, DriverVersion, Size, DriverExtra;
            public uint Fields;
            public int PositionX, PositionY;
            public uint DisplayOrientation, DisplayFixedOutput;
            public short Color, Duplex, YResolution, TTOption, Collate;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string FormName;
            public ushort LogPixels;
            public uint BitsPerPel, PelsWidth, PelsHeight, DisplayFlags, DisplayFrequency;
            public uint IcmMethod, IcmIntent, MediaType, DitherType, Reserved1, Reserved2, PanningWidth, PanningHeight;
        }

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern bool EnumDisplaySettings(string device, int mode, ref DevMode dm);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern int ChangeDisplaySettingsEx(string device, ref DevMode dm, IntPtr hwnd, uint flags, IntPtr param);

        const int EnumCurrentSettings = -1;
        const uint DmDisplayOrientation = 0x80, DmPelsWidth = 0x80000, DmPelsHeight = 0x100000, CdsUpdateRegistry = 1;

        static bool Read(out DevMode dm)
        {
            dm = new DevMode { Size = (ushort)Marshal.SizeOf<DevMode>() };
            return EnumDisplaySettings(null, EnumCurrentSettings, ref dm);
        }

        public static int? Current() => Read(out DevMode dm) ? (int)dm.DisplayOrientation : null;

        /// <summary>Rotate the primary display; true when it changed.</summary>
        public static bool Set(int orientation)
        {
            if (!Read(out DevMode dm) || dm.DisplayOrientation == orientation) return false;
            if ((dm.DisplayOrientation + orientation) % 2 == 1) (dm.PelsWidth, dm.PelsHeight) = (dm.PelsHeight, dm.PelsWidth);
            dm.DisplayOrientation = (uint)orientation;
            dm.Fields = DmDisplayOrientation | DmPelsWidth | DmPelsHeight;
            return ChangeDisplaySettingsEx(null, ref dm, IntPtr.Zero, CdsUpdateRegistry, IntPtr.Zero) == 0;
        }
    }
}
