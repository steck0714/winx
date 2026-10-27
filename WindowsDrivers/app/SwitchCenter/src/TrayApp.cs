// Notification-area icon and menu: CPU profile, screen brightness, start at
// login, language, and (stage 2) the control center window. Replaces
// SwitchClock.exe (settings are shared: HKCU\Software\SwitchWOA\SwitchClock).
using System;
using System.ComponentModel;
using System.Drawing;
using System.Drawing.Text;
using System.Runtime.InteropServices;
using System.Windows.Forms;
using Microsoft.Win32;
using static SwitchCenter.Lang;

namespace SwitchCenter
{
    sealed class TrayApp : ApplicationContext
    {
        const string SettingsKey = @"Software\SwitchWOA\SwitchClock";     // kept from SwitchClock: LastProfile
        const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
        const string RunName = "SwitchCenter";
        const string OldRunName = "SwitchClock";
        static readonly int[] BrightnessSteps = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };

        static string AppTitle => T("Switch コントロールセンター", "Switch Control Center");

        static string ProfileName(int index) => index switch
        {
            0 => T("省電力", "Power saver"),
            1 => T("標準", "Standard"),
            2 => T("高め", "Higher"),
            3 => T("高性能", "High performance"),
            4 => T("最大寄り", "Near maximum"),
            5 => T("最大", "Maximum"),
            6 => T("定格最大", "Rated maximum"),
            _ => T("プロファイル ", "Profile ") + index,
        };

        readonly NotifyIcon tray = new NotifyIcon();
        readonly ContextMenuStrip menu = new ContextMenuStrip();
        readonly ToolStripMenuItem header = new ToolStripMenuItem { Enabled = false };
        readonly ToolStripMenuItem brightness = new ToolStripMenuItem();
        readonly ToolStripMenuItem startup = new ToolStripMenuItem();
        readonly ToolStripMenuItem language = new ToolStripMenuItem("言語 / Language");
        readonly ToolStripMenuItem openWindow = new ToolStripMenuItem();
        readonly Timer timer = new Timer { Interval = 2000 };
        readonly SystemUsage usage = new SystemUsage();
        MainForm window;
        ToolStripMenuItem[] profileItems = Array.Empty<ToolStripMenuItem>();
        CpuClock.Profile[] profiles;
        Font boldFont;
        Icon currentIcon;
        string iconKey = "";
        bool restored;

        public TrayApp(bool openWindowAtStart)
        {
            startup.Click += (_, _) => SetStartup(!startup.Checked);
            menu.Opening += (_, _) => { RefreshCpu(); RefreshBrightness(); };
            tray.ContextMenuStrip = menu;
            tray.MouseUp += (_, e) =>
            {
                // NotifyIcon opens the menu only on a right click; reuse its private ShowContextMenu.
                if (e.Button == MouseButtons.Left)
                    typeof(NotifyIcon).GetMethod("ShowContextMenu",
                        System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)?.Invoke(tray, null);
            };
            openWindow.Font = new Font(openWindow.Font, FontStyle.Bold);
            openWindow.Click += (_, _) => OpenWindow();
            tray.DoubleClick += (_, _) => OpenWindow();
            var japanese = new ToolStripMenuItem("日本語");
            var english = new ToolStripMenuItem("English");
            japanese.Click += (_, _) => ChangeLanguage(false);
            english.Click += (_, _) => ChangeLanguage(true);
            language.DropDownItems.Add(japanese);
            language.DropDownItems.Add(english);
            language.DropDownOpening += (_, _) => { japanese.Checked = !English; english.Checked = English; };
            window = new MainForm(ChooseProfile, usage); // created hidden so the chart collects history
            BuildMenu();
            tray.Visible = true;
            timer.Tick += (_, _) => { usage.Sample(); RefreshCpu(); };
            timer.Start();
            // Application.Exit (the control center updating itself) skips ExitThreadCore; do not leave a dead icon.
            Application.ApplicationExit += (_, _) => tray.Visible = false;
            usage.Sample();
            RefreshCpu();
            if (openWindowAtStart) OpenWindow();
        }

        string ProfileLabel(CpuClock.Profile p)
        {
            string label = $"{ProfileName(p.Index)}  {p.Khz / 1000.0:0.0} MHz / {p.Mv} mV";
            if (p.Default) label += T("（起動時）", " (boot default)");
            if (p.Experimental) label += T("（試験的）", " (experimental)");
            return label;
        }

        void OpenWindow()
        {
            window.RefreshAll(profiles, ProfileLabel);
            window.Show();
            if (window.WindowState == FormWindowState.Minimized) window.WindowState = FormWindowState.Normal;
            window.Activate();
        }

        /// <summary>Switch the UI language: rebuild the menu and the window (keeping the chart history).</summary>
        void ChangeLanguage(bool english)
        {
            if (english == English) return;
            Lang.Set(english);
            bool visible = window.Visible;
            var old = window;
            window = new MainForm(ChooseProfile, usage);
            window.CopyHistoryFrom(old);
            old.Dispose();
            BuildMenu();
            iconKey = "";
            RefreshCpu();
            if (visible) OpenWindow();
        }

        void BuildMenu()
        {
            menu.Items.Clear();
            openWindow.Text = T("コントロールセンターを開く", "Open control center");
            brightness.Text = T("画面の明るさ", "Screen brightness");
            startup.Text = T("ログイン時に起動", "Start at login");
            menu.Items.Add(openWindow);
            menu.Items.Add(header);
            menu.Items.Add(new ToolStripSeparator());
            try { profiles = CpuClock.GetProfiles(); }
            catch (Win32Exception) { profiles = null; }
            if (profiles != null)
            {
                profileItems = new ToolStripMenuItem[profiles.Length];
                foreach (CpuClock.Profile p in profiles)
                {
                    var item = new ToolStripMenuItem(ProfileLabel(p));
                    int index = p.Index;
                    item.Click += (_, _) => ChooseProfile(index);
                    profileItems[p.Index] = item;
                    menu.Items.Add(item);
                }
                menu.Items.Add(new ToolStripSeparator());
            }
            brightness.DropDownItems.Clear();
            foreach (int percent in BrightnessSteps)
            {
                var item = new ToolStripMenuItem(percent + "%") { Tag = percent };
                item.Click += (_, _) => SetBrightness(percent);
                brightness.DropDownItems.Add(item);
            }
            menu.Items.Add(brightness);
            menu.Items.Add(new ToolStripSeparator());
            startup.Checked = IsStartup();
            menu.Items.Add(startup);
            menu.Items.Add(language);
            menu.Items.Add(new ToolStripMenuItem(T("終了", "Exit"), null, (_, _) => ExitThread()));
            window?.RefreshAll(profiles, ProfileLabel);
        }

        /// <summary>Switch to a profile (asks first for experimental ones). False if cancelled or failed.</summary>
        bool ChooseProfile(int index)
        {
            CpuClock.Profile p = profiles[index];
            bool ok = true;
            if (p.Experimental)
            {
                string text = T(
                    $"{ProfileName(index)} は試験的なプロファイルです（{p.Khz / 1000.0:0.0} MHz / {p.Mv} mV）。\n\n" +
                    "固まったら、電源ボタンの長押しで電源を切ってください。再起動すると標準に戻ります。\n" +
                    "このプロファイルは、次にログインしたときには自動で戻しません。\n\n切り替えますか？",
                    $"{ProfileName(index)} is an experimental profile ({p.Khz / 1000.0:0.0} MHz / {p.Mv} mV).\n\n" +
                    "If the Switch freezes, hold the power button to turn it off. It boots at the standard clock again.\n" +
                    "This profile is not restored automatically at the next login.\n\nSwitch to it?");
                if (MessageBox.Show(text, AppTitle, MessageBoxButtons.OKCancel, MessageBoxIcon.Warning) != DialogResult.OK)
                    return false;
            }
            try
            {
                CpuClock.SetProfile(index);
                if (!p.Experimental)
                    using (RegistryKey k = Registry.CurrentUser.CreateSubKey(SettingsKey)) k.SetValue("LastProfile", index);
            }
            catch (Win32Exception e)
            {
                MessageBox.Show(T("切り替えに失敗しました: ", "Could not switch: ") + e.Message, AppTitle, MessageBoxButtons.OK, MessageBoxIcon.Error);
                ok = false;
            }
            RefreshCpu();
            window?.RefreshAll();
            return ok;
        }

        void RestoreLastProfile()
        {
            restored = true;
            object v;
            using (RegistryKey k = Registry.CurrentUser.OpenSubKey(SettingsKey)) v = k?.GetValue("LastProfile");
            if (v is not int index || profiles == null) return;
            if (index < 0 || index >= profiles.Length || profiles[index].Experimental) return;
            try { CpuClock.SetProfile(index); } catch (Win32Exception) { }
        }

        void SetBrightness(int percent)
        {
            try { Backlight.Set(Backlight.PercentToLevel(percent)); }
            catch (Win32Exception e)
            {
                MessageBox.Show(T("明るさを変えられませんでした: ", "Could not change the brightness: ") + e.Message, AppTitle,
                                MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            RefreshBrightness();
        }

        void RefreshBrightness()
        {
            Backlight.Status b;
            try { b = Backlight.Get(); }
            catch (Win32Exception) { brightness.Visible = false; return; }
            brightness.Visible = true;
            int nearest = 0;
            foreach (ToolStripMenuItem item in brightness.DropDownItems)
            {
                int p = (int)item.Tag;
                if (Math.Abs(Backlight.PercentToLevel(p) - b.Level) < Math.Abs(Backlight.PercentToLevel(nearest) - b.Level)) nearest = p;
            }
            foreach (ToolStripMenuItem item in brightness.DropDownItems) item.Checked = (int)item.Tag == nearest;
            brightness.Text = T($"画面の明るさ（{Backlight.LevelToPercent(b.Level)}%）", $"Screen brightness ({Backlight.LevelToPercent(b.Level)}%)");
        }

        void RefreshCpu()
        {
            CpuClock.Status s;
            try
            {
                if (profiles == null) BuildMenu();
                if (profiles != null && !restored) RestoreLastProfile();
                s = CpuClock.GetStatus();
            }
            catch (Win32Exception e)
            {
                SetIcon("?", Color.Gray);
                SetTip($"{AppTitle}: " + T("ドライバーに接続できません", "cannot reach the driver") + $" ({e.NativeErrorCode})");
                header.Text = T("CPU のドライバーに接続できません", "Cannot reach the CPU clock driver");
                return;
            }
            window?.AddSample(s);
            string temp = s.TempValid ? $"SoC {s.TempSoC}℃" : T("温度不明", "temperature unknown");
            string status = $"{s.Khz / 1000.0:0.0} MHz / {s.Mv} mV / {temp}";
            SetTip(s.Throttled ? status + T(" / 温度で制限中", " / thermal limit") : status);
            header.Text = s.Throttled ? status + T("（温度で標準に制限中）", " (held at standard: too hot)") : status;
            boldFont ??= new Font(menu.Font, FontStyle.Bold);
            for (int i = 0; i < profileItems.Length; i++)
            {
                profileItems[i].Checked = i == s.Requested;
                profileItems[i].Font = i == s.Active ? boldFont : menu.Font;
            }
            bool experimental = profiles != null && s.Active >= 0 && s.Active < profiles.Length && profiles[s.Active].Experimental;
            SetIcon((s.Khz / 1000000.0).ToString("0.0"), s.Throttled ? Color.OrangeRed : experimental ? Color.Gold : Color.LightGreen);
        }

        void SetTip(string text) => tray.Text = text.Length > 63 ? text.Substring(0, 63) : text;

        [DllImport("user32.dll")]
        static extern bool DestroyIcon(IntPtr handle);

        void SetIcon(string text, Color color)
        {
            string key = text + color.ToArgb();
            if (key == iconKey) return;
            iconKey = key;
            Size size = SystemInformation.SmallIconSize;
            using var bmp = new Bitmap(size.Width, size.Height);
            using Graphics g = Graphics.FromImage(bmp);
            using var font = new Font("Segoe UI", size.Height * 0.55f, FontStyle.Bold, GraphicsUnit.Pixel);
            using var back = new SolidBrush(Color.FromArgb(40, 40, 40));
            using var fore = new SolidBrush(color);
            using var center = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center };
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            g.FillRectangle(back, 0, 0, size.Width, size.Height);
            g.DrawString(text, font, fore, new RectangleF(-2, 0, size.Width + 4, size.Height), center);
            IntPtr handle = bmp.GetHicon();
            var icon = (Icon)Icon.FromHandle(handle).Clone();
            DestroyIcon(handle);
            tray.Icon = icon;
            currentIcon?.Dispose();
            currentIcon = icon;
        }

        static bool IsStartup()
        {
            using RegistryKey k = Registry.CurrentUser.OpenSubKey(RunKey);
            return k?.GetValue(RunName) != null;
        }

        void SetStartup(bool on)
        {
            using (RegistryKey k = Registry.CurrentUser.CreateSubKey(RunKey))
            {
                if (on)
                {
                    k.SetValue(RunName, "\"" + Application.ExecutablePath + "\"");
                    k.DeleteValue(OldRunName, false);          // SwitchClock.exe is replaced by this app
                }
                else
                {
                    k.DeleteValue(RunName, false);
                }
            }
            startup.Checked = IsStartup();
        }

        protected override void ExitThreadCore()
        {
            timer.Stop();
            tray.Visible = false;
            tray.Dispose();
            window.Dispose();
            base.ExitThreadCore();
        }
    }
}
