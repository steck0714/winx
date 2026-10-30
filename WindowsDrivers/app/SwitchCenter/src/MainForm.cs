// Control center window (stage 2): Overview, Performance and Display tabs.
// Refreshes every 2 s while visible; closing only hides it (the tray icon
// keeps running). Laid out for the Switch's 720 x 1280 portrait screen and
// for touch: big tiles, big radio buttons and a wide slider. Texts follow
// Lang (the tray rebuilds this window when the language changes).
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Linq;
using System.Windows.Forms;
using static SwitchCenter.Lang;

namespace SwitchCenter
{
    sealed class MainForm : Form
    {
        readonly Func<int, bool> chooseProfile;          // TrayApp.ChooseProfile (asks for experimental profiles)
        readonly SystemUsage usage;                      // sampled by the tray every 2 s
        readonly AutoScreen auto;                        // automatic brightness and rotation (owned by the tray)
        readonly Timer timer = new Timer { Interval = 2000 };
        readonly Font tileValueFont = new Font("Segoe UI", 22f, FontStyle.Bold);
        readonly Font tileTitleFont = new Font("Yu Gothic UI", 10f);
        readonly Font bodyFont = new Font("Yu Gothic UI", 11f);
        readonly Font bodyBoldFont = new Font("Yu Gothic UI", 11f, FontStyle.Bold);

        // Overview
        readonly Label cpuValue = new Label(), tempValue = new Label(), batteryValue = new Label(), brightValue = new Label();
        readonly Label loadValue = new Label(), memoryValue = new Label();
        readonly Label details = new Label { AutoSize = false, Dock = DockStyle.Fill };
        readonly HistoryChart chart = new HistoryChart { Dock = DockStyle.Fill };

        // Performance
        readonly FlowLayoutPanel profilePanel = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoScroll = true };
        readonly Label perfStatus = new Label { Dock = DockStyle.Top, Height = 56 };
        readonly List<RadioButton> profileButtons = new List<RadioButton>();
        bool updatingRadios;

        // Display
        readonly TrackBar brightnessBar = new TrackBar { Minimum = 3, Maximum = 100, TickFrequency = 10, LargeChange = 10, SmallChange = 5, Dock = DockStyle.Top, Height = 60 };
        readonly Label brightnessLabel = new Label { Dock = DockStyle.Top, Height = 48 };
        readonly Label displayState = new Label { Dock = DockStyle.Top, Height = 40 };
        readonly Timer brightnessApply = new Timer { Interval = 150 };
        readonly CheckBox autoBright = new CheckBox { Dock = DockStyle.Top, Height = 40 };
        readonly CheckBox autoRotate = new CheckBox { Dock = DockStyle.Top, Height = 40 };
        readonly CheckBox rotateReverse = new CheckBox { Dock = DockStyle.Top, Height = 40 };
        readonly Button rotateBase = new Button { Dock = DockStyle.Top, Height = 44 };
        readonly Button rotateTurn = new Button { Dock = DockStyle.Top, Height = 44 };
        readonly Label sensorValues = new Label { Dock = DockStyle.Top, Height = 72 };
        readonly Timer sensorTimer = new Timer { Interval = 500 };
        bool updatingBar;

        /// <summary>This control center's version, e.g. 0.5.3 (shown in the title and on the Drivers tab).</summary>
        static string AppVersion => Application.ProductVersion.Split('+')[0];

        public MainForm(Func<int, bool> chooseProfile, SystemUsage usage, AutoScreen auto)
        {
            this.chooseProfile = chooseProfile;
            this.usage = usage;
            this.auto = auto;
            Text = T("Switch コントロールセンター", "Switch Control Center") + " " + AppVersion;
            Font = bodyFont;
            AutoScaleMode = AutoScaleMode.Dpi;
            StartPosition = FormStartPosition.CenterScreen;
            ClientSize = new Size(560, 820);
            MinimumSize = new Size(420, 520);

            var tabs = new TabControl { Dock = DockStyle.Fill, ItemSize = new Size(88, 40), SizeMode = TabSizeMode.Fixed };
            tabs.TabPages.Add(BuildOverview());
            tabs.TabPages.Add(BuildPerformance());
            tabs.TabPages.Add(BuildDisplay());
            TabPage power = BuildPower(), drivers = BuildDrivers(), logs = BuildLogs();
            tabs.TabPages.Add(power);
            tabs.TabPages.Add(drivers);
            tabs.TabPages.Add(logs);
            tabs.SelectedIndexChanged += (_, _) =>
            {
                if (tabs.SelectedTab == power) LoadPower();
                else if (tabs.SelectedTab == drivers) LoadDrivers();
                else if (tabs.SelectedTab == logs) LoadReport();
            };
            Controls.Add(tabs);

            timer.Tick += (_, _) => RefreshAll();
            brightnessApply.Tick += (_, _) => { brightnessApply.Stop(); ApplyBrightness(); };
            sensorTimer.Tick += (_, _) => RefreshSensors();
            VisibleChanged += (_, _) =>
            {
                auto.Watching = Visible;
                if (Visible) { RefreshAll(); timer.Start(); sensorTimer.Start(); } else { timer.Stop(); sensorTimer.Stop(); }
            };
            FormClosing += (_, e) =>
            {
                if (e.CloseReason == CloseReason.UserClosing) { e.Cancel = true; Hide(); }
            };
        }

        /// <summary>Called by the tray every 2 s even while hidden, so the chart has history.</summary>
        public void AddSample(CpuClock.Status s) => chart.Add(s.TempValid ? s.TempSoC : (int?)null, s.Khz / 1000, usage.Total);

        /// <summary>Keep the chart when the window is rebuilt for another language.</summary>
        public void CopyHistoryFrom(MainForm other) => chart.CopyFrom(other.chart);

        // ---------------------------------------------------------------- tabs

        TabPage BuildOverview()
        {
            var page = new TabPage(T("概要", "Overview")) { Padding = new Padding(10) };
            var grid = new TableLayoutPanel { Dock = DockStyle.Top, Height = 330, ColumnCount = 2, RowCount = 3 };
            grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
            grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
            for (int i = 0; i < 3; i++) grid.RowStyles.Add(new RowStyle(SizeType.Percent, 33.3f));
            grid.Controls.Add(Tile("CPU", cpuValue), 0, 0);
            grid.Controls.Add(Tile(T("CPU の使用率", "CPU usage"), loadValue), 1, 0);
            grid.Controls.Add(Tile(T("SoC の温度", "SoC temperature"), tempValue), 0, 1);
            grid.Controls.Add(Tile(T("メモリ", "Memory"), memoryValue), 1, 1);
            grid.Controls.Add(Tile(T("バッテリー", "Battery"), batteryValue), 0, 2);
            grid.Controls.Add(Tile(T("画面の明るさ", "Brightness"), brightValue), 1, 2);

            var detailBox = new GroupBox { Text = T("詳しい状態", "Details"), Dock = DockStyle.Top, Height = 215, Padding = new Padding(10) };
            detailBox.Controls.Add(details);
            var chartBox = new GroupBox
            {
                Text = T("直近 5 分（赤: 温度、青: クロック、緑: 使用率 0〜100%）", "Last 5 min (red: temperature, blue: clock, green: usage 0-100%)"),
                Dock = DockStyle.Fill, Padding = new Padding(6),
            };
            chartBox.Controls.Add(chart);

            page.Controls.Add(chartBox);
            page.Controls.Add(detailBox);
            page.Controls.Add(grid);
            return page;
        }

        Control Tile(string title, Label value)
        {
            var panel = new Panel { Dock = DockStyle.Fill, Margin = new Padding(6), BackColor = Color.FromArgb(32, 40, 72) };
            value.Dock = DockStyle.Fill;
            value.Font = tileValueFont;
            value.ForeColor = Color.White;
            value.TextAlign = ContentAlignment.MiddleCenter;
            value.Text = "—";
            var caption = new Label { Text = title, Dock = DockStyle.Top, Height = 28, Font = tileTitleFont, ForeColor = Color.FromArgb(184, 196, 224), TextAlign = ContentAlignment.MiddleCenter };
            panel.Controls.Add(value);
            panel.Controls.Add(caption);
            return panel;
        }

        TabPage BuildPerformance()
        {
            var page = new TabPage(T("パフォーマンス", "Performance")) { Padding = new Padding(10) };
            var note = new Label
            {
                Dock = DockStyle.Bottom, Height = 90,
                Text = T("・SoC が 80℃ 以上になると、自動で標準に下げます（70℃ 未満で戻ります）。\n" +
                         "・ログインしたとき、前回のプロファイルに戻します。ただし、試験的なものは戻しません。\n" +
                         "・タスクマネージャーの「速度」は変わりません。ここの値が実際のクロックです。",
                         "- At 80℃ or more the SoC is held at the standard profile (released below 70℃).\n" +
                         "- At login the last profile is restored, except experimental ones.\n" +
                         "- Task Manager keeps showing the boot speed; the clock shown here is the real one."),
            };
            page.Controls.Add(profilePanel);
            page.Controls.Add(perfStatus);
            page.Controls.Add(note);
            return page;
        }

        void BuildProfileButtons(CpuClock.Profile[] profiles, Func<CpuClock.Profile, string> label)
        {
            profilePanel.Controls.Clear();
            profileButtons.Clear();
            foreach (CpuClock.Profile p in profiles)
            {
                var radio = new RadioButton
                {
                    Text = label(p), Tag = p.Index, AutoSize = false, Width = 500, Height = 48,
                    Appearance = Appearance.Button, TextAlign = ContentAlignment.MiddleLeft, Padding = new Padding(10, 0, 0, 0),
                    Margin = new Padding(0, 0, 0, 6), FlatStyle = FlatStyle.Flat,
                    BackColor = p.Experimental ? Color.FromArgb(255, 246, 214) : SystemColors.Window,
                };
                radio.CheckedChanged += (sender, _) =>
                {
                    var r = (RadioButton)sender;
                    if (updatingRadios || !r.Checked) return;
                    if (!chooseProfile((int)r.Tag)) RefreshAll();     // cancelled or failed: show the real state again
                };
                profileButtons.Add(radio);
                profilePanel.Controls.Add(radio);
            }
        }

        TabPage BuildDisplay()
        {
            var page = new TabPage(T("画面", "Display")) { Padding = new Padding(16) };
            brightnessBar.ValueChanged += (_, _) =>
            {
                brightnessLabel.Text = T("明るさ: ", "Brightness: ") + $"{brightnessBar.Value}%";
                if (!updatingBar) { brightnessApply.Stop(); brightnessApply.Start(); }
            };
            var note = new Label
            {
                Dock = DockStyle.Top, Height = 120,
                Text = T("・Windows が画面を暗くすると 1/3 に、画面を消すとバックライトも消します。\n" +
                         "・選んだ明るさは、次に起動したときにも戻ります。\n" +
                         "・Windows 標準の明るさの設定は、今の画面ドライバーでは使えません。",
                         "- When Windows dims the display the backlight goes to 1/3; when it turns the display off, so does the backlight.\n" +
                         "- The brightness you choose is restored at the next boot.\n" +
                         "- The Windows brightness setting does not work with the current display driver."),
            };
            autoBright.Text = T("明るさを自動で調整する（明るさセンサー）", "Adjust the brightness automatically (light sensor)");
            autoRotate.Text = T("画面を自動で回転する（傾きセンサー）", "Rotate the screen automatically (tilt sensor)");
            rotateReverse.Text = T("回転の向きを逆にする（横と縦の両方を覚えたら不要）", "Reverse the rotation (not needed once landscape and portrait are remembered)");
            rotateTurn.Text = T("画面を 90° 回す（向きを手で直す）", "Turn the screen 90° (put it right by hand)");
            rotateBase.Text = T("今の画面の向きを、今の持ち方の正しい向きとして覚える", "Remember this orientation as right for how the Switch is held now");
            autoBright.Checked = auto.AutoBrightness;
            autoRotate.Checked = auto.AutoRotate;
            rotateReverse.Checked = auto.RotateReverse;
            autoBright.CheckedChanged += (_, _) => { auto.AutoBrightness = autoBright.Checked; brightnessBar.Enabled = !autoBright.Checked; };
            autoRotate.CheckedChanged += (_, _) => auto.AutoRotate = autoRotate.Checked;
            rotateReverse.CheckedChanged += (_, _) => auto.RotateReverse = rotateReverse.Checked;
            rotateBase.Click += (_, _) =>
            {
                if (!auto.SetBaseFromNow())
                    MessageBox.Show(this, T("本体の向きが分かりませんでした。机に置かずに、横か縦にまっすぐ持って、もう一度押してください。",
                                            "Could not tell how the Switch is held. Hold it upright (not flat), landscape or portrait, and press again."),
                                    Text, MessageBoxButtons.OK, MessageBoxIcon.Information);
                RefreshSensors();
            };
            rotateTurn.Click += (_, _) => { AutoScreen.TurnScreen(); RefreshSensors(); };
            page.Controls.Add(sensorValues);
            page.Controls.Add(rotateBase);
            page.Controls.Add(rotateTurn);
            page.Controls.Add(rotateReverse);
            page.Controls.Add(autoRotate);
            page.Controls.Add(note);
            page.Controls.Add(displayState);
            page.Controls.Add(autoBright);
            page.Controls.Add(brightnessBar);
            page.Controls.Add(brightnessLabel);
            return page;
        }

        // ------------------------------------------------------- stage 3 tabs

        readonly ComboBox buttonAction = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 300 };
        readonly ComboBox displayOff = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 300 };
        readonly ComboBox criticalAction = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 300 };
        readonly NumericUpDown criticalLevel = new NumericUpDown { Minimum = 1, Maximum = 50, Width = 80 };
        readonly Label powerResult = new Label { AutoSize = true, ForeColor = Color.DarkGreen };
        readonly Label powerWindowsInfo = new Label { AutoSize = true };
        static readonly uint[] DisplayOffChoices = { 60, 120, 300, 600, 900, 1800, 0 };
        bool loadingPower;

        TabPage BuildPower()
        {
            var page = new TabPage(T("電源", "Power")) { Padding = new Padding(16) };
            buttonAction.Items.AddRange(new object[]
            {
                T("何もしない", "Do nothing"), T("スリープ（未確認）", "Sleep (untested)"), T("休止状態（未確認）", "Hibernate (untested)"),
                T("シャットダウン", "Shut down"), T("画面を消す", "Turn off the display"),
            });
            displayOff.Items.AddRange(new object[]
            {
                T("1 分", "1 minute"), T("2 分", "2 minutes"), T("5 分", "5 minutes"), T("10 分", "10 minutes"),
                T("15 分", "15 minutes"), T("30 分", "30 minutes"), T("消さない", "Never"),
            });
            criticalAction.Items.AddRange(new object[]
            {
                T("何もしない", "Do nothing"), T("スリープ（未確認）", "Sleep (untested)"), T("休止状態（未確認）", "Hibernate (untested)"), T("シャットダウン", "Shut down"),
            });
            buttonAction.SelectionChangeCommitted += (_, _) => WritePower(PowerSettings.SubButtons, PowerSettings.PowerButtonAction, (uint)buttonAction.SelectedIndex);
            displayOff.SelectionChangeCommitted += (_, _) => WritePower(PowerSettings.SubVideo, PowerSettings.DisplayOffSeconds, DisplayOffChoices[displayOff.SelectedIndex]);
            criticalAction.SelectionChangeCommitted += (_, _) => WritePower(PowerSettings.SubBattery, PowerSettings.CriticalAction, (uint)criticalAction.SelectedIndex);
            criticalLevel.ValueChanged += (_, _) => { if (!loadingPower) WritePower(PowerSettings.SubBattery, PowerSettings.CriticalLevel, (uint)criticalLevel.Value); };

            var flow = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoScroll = true };
            flow.Controls.Add(new Label { Text = T("電源ボタンを押したとき", "When the power button is pressed"), AutoSize = true, Margin = new Padding(0, 0, 0, 4) });
            flow.Controls.Add(buttonAction);
            flow.Controls.Add(new Label { Text = T("画面を消すまでの時間（操作しないとき）", "Turn off the display after (when idle)"), AutoSize = true, Margin = new Padding(0, 18, 0, 4) });
            flow.Controls.Add(displayOff);
            flow.Controls.Add(new Label { Text = T("電池が少なくなったとき", "When the battery is critically low"), AutoSize = true, Margin = new Padding(0, 18, 0, 4) });
            flow.Controls.Add(criticalAction);
            var levelRow = new FlowLayoutPanel { AutoSize = true, Margin = new Padding(0, 8, 0, 0) };
            levelRow.Controls.Add(new Label { Text = T("少ないとみなす残量（%）:", "Critical level (%):"), AutoSize = true, Margin = new Padding(0, 6, 8, 0) });
            levelRow.Controls.Add(criticalLevel);
            flow.Controls.Add(levelRow);
            flow.Controls.Add(powerResult);
            powerResult.Margin = new Padding(0, 14, 0, 0);
            flow.Controls.Add(powerWindowsInfo);
            powerWindowsInfo.Margin = new Padding(0, 14, 0, 0);
            flow.Controls.Add(new Label
            {
                AutoSize = true, MaximumSize = new Size(500, 0), Margin = new Padding(0, 14, 0, 0),
                Text = T("・今の電源プランの、AC とバッテリーの両方に設定します。\n" +
                         "・スリープと休止状態は、この Switch ではまだ確かめていません。\n" +
                         "・電池が少ないときの動作は、Windows がバッテリーを認識しているときだけ働きます（下の表示を参照）。",
                         "- Applied to the active power plan, for both AC and battery.\n" +
                         "- Sleep and hibernate are not tested on this Switch yet.\n" +
                         "- The critical-battery action only works when Windows sees the battery (see the line below)."),
            });
            page.Controls.Add(flow);
            return page;
        }

        void LoadPower()
        {
            loadingPower = true;
            uint? action = PowerSettings.Read(PowerSettings.SubButtons, PowerSettings.PowerButtonAction);
            buttonAction.SelectedIndex = action is uint a && a < buttonAction.Items.Count ? (int)a : -1;
            uint? off = PowerSettings.Read(PowerSettings.SubVideo, PowerSettings.DisplayOffSeconds);
            int offIndex = off is uint o ? Array.IndexOf(DisplayOffChoices, o) : -1;
            displayOff.SelectedIndex = offIndex;
            uint? crit = PowerSettings.Read(PowerSettings.SubBattery, PowerSettings.CriticalAction);
            criticalAction.SelectedIndex = crit is uint c && c < criticalAction.Items.Count ? (int)c : -1;
            uint? level = PowerSettings.Read(PowerSettings.SubBattery, PowerSettings.CriticalLevel);
            criticalLevel.Value = level is uint l ? Math.Clamp(l, 1, 50) : 5;
            loadingPower = false;
            powerResult.Text = "";
            PowerStatus ps = SystemInformation.PowerStatus;
            bool noBattery = (ps.BatteryChargeStatus & BatteryChargeStatus.NoSystemBattery) != 0;
            powerWindowsInfo.Text = T("Windows の電源の情報: ", "Windows power status: ") +
                (noBattery ? T("バッテリーを認識していない", "no system battery") : T($"バッテリー {ps.BatteryLifePercent:P0}", $"battery {ps.BatteryLifePercent:P0}")) +
                (ps.PowerLineStatus == PowerLineStatus.Online ? " / AC" : "");
            powerWindowsInfo.ForeColor = noBattery ? Color.DarkOrange : SystemColors.ControlText;
        }

        void WritePower(Guid sub, Guid setting, uint value)
        {
            if (loadingPower) return;
            uint e = PowerSettings.Write(sub, setting, value);
            powerResult.ForeColor = e == 0 ? Color.DarkGreen : Color.Firebrick;
            powerResult.Text = e == 0 ? T("設定しました。", "Saved.")
                : T($"設定できませんでした（エラー {e}）。管理者として実行すると変えられる場合があります。",
                    $"Could not save (error {e}). Running as administrator may help.");
        }

        readonly ListView driverList = new ListView { Dock = DockStyle.Fill, View = View.Details, FullRowSelect = true, HeaderStyle = ColumnHeaderStyle.Nonclickable, ShowItemToolTips = true };
        readonly Button checkButton = new Button { Width = 160, Height = 40 };
        readonly Button installButton = new Button { Width = 200, Height = 40, Enabled = false };
        readonly Button appButton = new Button { Width = 370, Height = 40, Visible = false, Margin = new Padding(3, 8, 3, 3) };
        readonly Label updateStatus = new Label { AutoSize = true, MaximumSize = new Size(520, 0), Margin = new Padding(3, 8, 3, 3) };
        // Kept away from the buttons (a tap meant for them opened it), at the bottom.
        readonly LinkLabel releaseLink = new LinkLabel { AutoSize = true, MaximumSize = new Size(520, 0), Visible = false, Margin = new Padding(3, 24, 3, 3) };
        readonly ComboBox channel = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 330 };
        const string ChannelKey = @"Software\SwitchWOA\SwitchClock";     // DriverChannel: 0 stable, 1 beta
        Release latest;                       // set by "Check for updates"
        List<UpdateRow> updates;

        TabPage BuildDrivers()
        {
            var page = new TabPage(T("ドライバー", "Drivers")) { Padding = new Padding(10) };
            driverList.Columns.Add(T("デバイス", "Device"), 150);
            driverList.Columns.Add(T("状態", "State"), 110);
            driverList.Columns.Add(T("入っている版", "Installed"), 90);
            driverList.Columns.Add(T("動作中", "Running"), 60);
            driverList.Columns.Add(T("最新", "Latest"), 100);
            var bottom = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = 300, FlowDirection = FlowDirection.TopDown, WrapContents = false };
            var reload = new Button { Text = T("再読み込み", "Reload"), Width = 120, Height = 40 };
            reload.Click += (_, _) => LoadDrivers();
            checkButton.Text = T("更新を確認", "Check for updates");
            checkButton.Click += async (_, _) => await CheckUpdates();
            installButton.Text = T("更新する", "Update");
            installButton.Click += async (_, _) => await InstallUpdates();
            appButton.Click += async (_, _) => await UpdateApp();
            releaseLink.Text = T("リリースのページを開く", "Open the release page");
            releaseLink.LinkClicked += (_, _) => { if (latest != null) OpenInBrowser(latest.PageUrl); };
            channel.Items.Add(T("安定版のドライバー", "Stable drivers"));
            channel.Items.Add(T("ベータ版のドライバー（試験中のものを含む）", "Beta drivers (includes ones in testing)"));
            channel.SelectedIndex = ReadChannel();
            channel.SelectionChangeCommitted += async (_, _) => await ChangeChannel();
            var channelRow = new FlowLayoutPanel { AutoSize = true };
            channelRow.Controls.Add(new Label { Text = T("更新の種類:", "Channel:"), AutoSize = true, Margin = new Padding(3, 10, 3, 3) });
            channelRow.Controls.Add(channel);
            bottom.Controls.Add(channelRow);
            var buttons = new FlowLayoutPanel { AutoSize = true };
            buttons.Controls.Add(reload);
            buttons.Controls.Add(checkButton);
            buttons.Controls.Add(installButton);
            bottom.Controls.Add(buttons);
            bottom.Controls.Add(appButton);
            bottom.Controls.Add(updateStatus);
            bottom.Controls.Add(releaseLink);
            updateStatus.Text = T($"コントロールセンター {AppVersion}。「更新を確認」で、GitHub の最新のリリースと比べます。",
                                  $"Control center {AppVersion}. \"Check for updates\" compares with the latest release on GitHub.");
            page.Controls.Add(driverList);
            page.Controls.Add(bottom);
            return page;
        }

        /// <summary>Edge if it is installed: the default browser of this Windows is still IE11, which GitHub no longer supports.</summary>
        static void OpenInBrowser(string url)
        {
            foreach (Environment.SpecialFolder f in new[] { Environment.SpecialFolder.ProgramFilesX86, Environment.SpecialFolder.ProgramFiles })
            {
                string edge = System.IO.Path.Combine(Environment.GetFolderPath(f), @"Microsoft\Edge\Application\msedge.exe");
                if (System.IO.File.Exists(edge))
                {
                    System.Diagnostics.Process.Start(edge, url)?.Dispose();
                    return;
                }
            }
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(url) { UseShellExecute = true })?.Dispose();
        }

        static int ReadChannel()
        {
            using Microsoft.Win32.RegistryKey k = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(ChannelKey);
            return k?.GetValue("DriverChannel") is int v && v == 1 ? 1 : 0;
        }

        bool Beta => channel.SelectedIndex == 1;

        async System.Threading.Tasks.Task ChangeChannel()
        {
            if (Beta && ReadChannel() == 0 &&
                MessageBox.Show(this,
                    T("ベータ版では、GitHub のプレリリースにある試験中のドライバーも、更新の候補になります。\n\n" +
                      "まだ十分に試していないので、動かなかったり、起動に影響したりすることがあります。" +
                      "そのときは、「安定版」に戻して、ドライバーを入れ直してください。\n\nベータ版にしますか？",
                      "With the beta channel, drivers still in testing (GitHub pre-releases) are offered as updates too.\n\n" +
                      "They are not fully tested and may not work or may affect booting. If that happens, switch back to stable and reinstall the drivers.\n\nUse the beta channel?"),
                    T("ベータ版のドライバー", "Beta drivers"), MessageBoxButtons.OKCancel, MessageBoxIcon.Warning) != DialogResult.OK)
            {
                channel.SelectedIndex = 0;
                return;
            }
            using (Microsoft.Win32.RegistryKey k = Microsoft.Win32.Registry.CurrentUser.CreateSubKey(ChannelKey))
                k.SetValue("DriverChannel", channel.SelectedIndex);
            if (latest != null) await CheckUpdates();      // compare with the other channel's release right away
        }

        void LoadDrivers()
        {
            List<DriverRow> rows = DriverStatus.Read(latest);
            if (latest != null) updates = Updater.Compare(latest, rows, Updater.EstimateUefi());
            driverList.BeginUpdate();
            driverList.Items.Clear();
            foreach (DriverRow d in rows)
            {
                UpdateRow u = updates?.Find(x => x.Item.Hwid.Equals(d.HardwareId, StringComparison.OrdinalIgnoreCase));
                // Per instance: of the three I2C controllers only the ones with an older driver are marked.
                bool rowUpdate = u is { Update: true } && !d.ReinstallPending &&
                                 (!Version.TryParse(d.Installed, out Version have) || have < Version.Parse(u.Item.Version));
                string newest = d.ReinstallPending ? T("再起動待ち", "after restart")
                              : u == null ? "" : u.UefiTooOld ? T($"要 UEFI {u.Item.MinUefi}", $"needs {u.Item.MinUefi}")
                              : rowUpdate ? u.Item.Version + " ↑" : u.Item.Version;
                var item = new ListViewItem(new[] { d.Name, d.State, d.Installed, d.Running, newest })
                {
                    ForeColor = !d.Ok ? Color.Firebrick : rowUpdate || d.ReinstallPending ? Color.RoyalBlue : SystemColors.WindowText,
                    ToolTipText = d.Instance,
                };
                driverList.Items.Add(item);
            }
            driverList.EndUpdate();
            int count = updates?.FindAll(x => x.Update).Count ?? 0;
            installButton.Enabled = count > 0;
            installButton.Text = count > 0 ? T($"更新する（{count} 件）", $"Update ({count})") : T("更新する", "Update");
        }

        async System.Threading.Tasks.Task CheckUpdates()
        {
            checkButton.Enabled = false;
            updateStatus.ForeColor = SystemColors.ControlText;
            updateStatus.Text = T("GitHub を確認しています…", "Checking GitHub...");
            try
            {
                latest = await Updater.FetchChannelAsync(Beta);
                LoadDrivers();
                int count = updates.FindAll(x => x.Update).Count;
                int pending = updates.Sum(x => x.Pending);
                int uefi = Updater.EstimateUefi();
                updateStatus.Text =
                    T($"最新のリリース: {latest.Tag}", $"Latest release: {latest.Tag}") +
                    (latest.Prerelease ? T("（ベータ版）", " (beta)") : "") + "\n" +
                    T($"コントロールセンター: 今 {AppVersion} / 最新 {latest.AppVersion}", $"Control center: {AppVersion} installed / {latest.AppVersion} latest") + "\n" +
                    (count > 0 ? T($"更新できるドライバー: {count} 件（青い行）", $"Driver updates: {count} (blue rows)")
                               : T("ドライバーはすべて最新です。", "All drivers are up to date.")) + "\n" +
                    (pending > 0 ? T($"再起動を待っているデバイス: {pending} 個\n", $"Devices waiting for a restart: {pending}\n") : "") +
                    T($"UEFI: R{uefi} 以降（DSDT のデバイスからの推定）", $"UEFI: R{uefi} or later (estimated from the DSDT devices)");
                bool newApp = Updater.AppIsOlder(latest) && latest.AppUrl != null;
                appButton.Visible = newApp;
                appButton.Enabled = true;
                appButton.Text = T($"コントロールセンターを {latest.AppVersion} に更新", $"Update the control center to {latest.AppVersion}");
                releaseLink.Visible = true;
            }
            catch (Exception e) when (e is System.Net.Http.HttpRequestException || e is System.Threading.Tasks.TaskCanceledException ||
                                      e is System.Text.Json.JsonException || e is System.IO.InvalidDataException || e is KeyNotFoundException)
            {
                updateStatus.ForeColor = Color.Firebrick;
                updateStatus.Text = T("確認できませんでした: ", "Could not check: ") + e.Message;
            }
            checkButton.Enabled = true;
        }

        async System.Threading.Tasks.Task InstallUpdates()
        {
            List<UpdateRow> todo = updates?.FindAll(x => x.Update);
            if (latest == null || todo == null || todo.Count == 0) return;
            string list = string.Join("\n", todo.ConvertAll(u => $"・{u.Item.Service}: {u.Installed ?? T("無し", "none")} → {u.Item.Version}" +
                                                               (u.AtNextBoot ? T("（次の起動のとき）", " (at the next boot)") : "")));
            string bootNote = todo.Exists(u => u.AtNextBoot)
                ? T("\n\nGPIO と I2C は、ほかのドライバーが使っているので、今は Windows に登録だけして、次に起動したときに入れ替えます。",
                    "\n\nGPIO and I2C are used by the other drivers, so they are only added now and replaced at the next boot.")
                : "";
            string betaNote = latest.Prerelease
                ? T("\n\nこれはベータ版（プレリリース）です。試験中のドライバーが含まれます。", "\n\nThis is a beta (pre-release) with drivers still in testing.")
                : "";
            if (MessageBox.Show(this,
                    T($"{latest.Tag} から、次のドライバーを入れます。\n\n{list}\n\n管理者の確認が出ます。入れる前に、SHA-256 と署名を確かめます。{bootNote}{betaNote}",
                      $"Install these drivers from {latest.Tag}?\n\n{list}\n\nWindows will ask for administrator approval. SHA-256 and signatures are checked first.{bootNote}{betaNote}"),
                    T("ドライバーの更新", "Driver update"), MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;

            installButton.Enabled = checkButton.Enabled = false;
            int? code = await Updater.RunElevatedAsync(latest.Tag, todo.ConvertAll(u => u.Item.Service));
            updateStatus.ForeColor = code is 0 or Updater.ExitReboot ? Color.DarkGreen : Color.Firebrick;
            updateStatus.Text = code switch
            {
                null => T("管理者の確認がキャンセルされました。", "Administrator approval was cancelled."),
                0 => T("更新しました。", "Updated."),
                Updater.ExitReboot => T("更新しました。再起動すると新しいドライバーになります。", "Updated. Restart to finish."),
                _ => T("更新できませんでした。理由は、更新の画面に出ています。", "The update failed; the update window shows why."),
            };
            checkButton.Enabled = true;
            LoadDrivers();
        }

        async System.Threading.Tasks.Task UpdateApp()
        {
            if (latest == null) return;
            if (MessageBox.Show(this,
                    T($"コントロールセンターを {Application.ProductVersion} から {latest.AppVersion} に更新します。\n\nダウンロードして SHA-256 を確かめたあと、いったん終了して、新しい版で起動し直します。",
                      $"Update the control center from {Application.ProductVersion} to {latest.AppVersion}?\n\nIt is downloaded and its SHA-256 checked; then the control center exits and starts again as the new version."),
                    T("コントロールセンターの更新", "Control center update"), MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;
            appButton.Enabled = checkButton.Enabled = installButton.Enabled = false;
            updateStatus.ForeColor = SystemColors.ControlText;
            updateStatus.Text = T($"{latest.AppFile} をダウンロードしています…", $"Downloading {latest.AppFile}...");
            try
            {
                string exe = await Updater.DownloadAppAsync(latest);
                Updater.StartSelfUpdate(exe);
                Application.Exit();
            }
            catch (Exception e) when (e is System.Net.Http.HttpRequestException || e is System.Threading.Tasks.TaskCanceledException ||
                                      e is System.IO.IOException || e is UnauthorizedAccessException || e is System.ComponentModel.Win32Exception)
            {
                updateStatus.ForeColor = Color.Firebrick;
                updateStatus.Text = T("コントロールセンターを更新できませんでした: ", "Could not update the control center: ") + e.Message;
                appButton.Enabled = checkButton.Enabled = true;
                LoadDrivers();
            }
        }

        readonly TextBox reportBox = new TextBox
        {
            Dock = DockStyle.Fill, Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Both, WordWrap = false,
            Font = new Font("Consolas", 9f),
        };

        TabPage BuildLogs()
        {
            var page = new TabPage(T("記録", "Logs")) { Padding = new Padding(10) };
            var top = new FlowLayoutPanel { Dock = DockStyle.Top, Height = 92, FlowDirection = FlowDirection.TopDown, WrapContents = false };
            var save = new Button { Text = T("困ったときの情報を保存", "Save troubleshooting info"), Width = 260, Height = 40 };
            var saved = new Label { AutoSize = true, MaximumSize = new Size(520, 0) };
            save.Click += (_, _) =>
            {
                try
                {
                    string path = DiagnosticReport.Save(DiagnosticReport.BuildText(usage));
                    saved.Text = T("デスクトップに保存しました: ", "Saved to the desktop: ") + System.IO.Path.GetFileName(path);
                }
                catch (Exception e) when (e is System.IO.IOException || e is UnauthorizedAccessException)
                {
                    saved.Text = T("保存できませんでした: ", "Could not save: ") + e.Message;
                }
            };
            top.Controls.Add(save);
            top.Controls.Add(saved);
            page.Controls.Add(reportBox);
            page.Controls.Add(top);
            return page;
        }

        void LoadReport() => reportBox.Text = DiagnosticReport.BuildText(usage).Replace("\n", Environment.NewLine).Replace("\r\r", "\r");

        void RefreshSensors()
        {
            Sensors.Status s = auto.Last;
            autoBright.Checked = auto.AutoBrightness;     // the tray menu may have changed them
            autoRotate.Checked = auto.AutoRotate;
            if (s == null)
            {
                sensorValues.Text = T("センサーのドライバーに接続できません（UEFI R379 以降と switchsensor が必要）。",
                                      "Cannot reach the sensor driver (needs UEFI R379 or later and switchsensor).") +
                                    (auto.LastError != null ? $" ({auto.LastError})" : "");
                autoBright.Enabled = autoBright.Checked;
                autoRotate.Enabled = autoRotate.Checked;
                return;
            }
            autoBright.Enabled = s.LightValid || autoBright.Checked;
            autoRotate.Enabled = s.AccelValid || autoRotate.Checked;
            string light = s.LightValid
                ? $"{s.Lux:0.#} lux (DATA0 {s.Data0}, DATA1 {s.Data1}, x{s.Gain}) -> {AutoScreen.PercentForLux(s.Lux)}%"
                : T("なし", "none");
            string tilt = s.AccelValid
                ? $"X {s.X}  Y {s.Y}  Z {s.Z} mg  " + T("上の軸: ", "up: ") + (AutoScreen.UpAxisName(s) ?? T("不明（平ら・斜め）", "unclear (flat or tilted)"))
                : T("なし", "none") + $" (ID {s.ImuId:X2})";
            string orientation = Display.Current() is int o ? $"{o * 90}°" : "—";
            sensorValues.Text = T("明るさセンサー: ", "Light sensor: ") + light + "\n" +
                                T("傾きセンサー: ", "Tilt sensor: ") + tilt + "\n" +
                                T("今の画面の向き: ", "Screen orientation now: ") + orientation;
        }

        void ApplyBrightness()
        {
            if (autoBright.Checked) autoBright.Checked = false;     // a manual choice turns the automatic one off
            try { Backlight.Set(Backlight.PercentToLevel(brightnessBar.Value)); }
            catch (Win32Exception e) { brightnessLabel.Text = T("明るさを変えられませんでした: ", "Could not change the brightness: ") + e.Message; }
        }

        // ------------------------------------------------------------- refresh

        public void RefreshAll() => RefreshAll(null, null);

        public void RefreshAll(CpuClock.Profile[] profiles, Func<CpuClock.Profile, string> label)
        {
            if (profiles != null && profileButtons.Count != profiles.Length) BuildProfileButtons(profiles, label);
            if (!Visible) return;

            CpuClock.Status cpu = null;
            try { cpu = CpuClock.GetStatus(); } catch (Win32Exception) { }
            Backlight.Status light = null;
            try { light = Backlight.Get(); } catch (Win32Exception) { }
            BatteryInfo battery = BatteryInfo.Read();
            int? fan = FanInfo.Percent;

            cpuValue.Text = cpu != null ? $"{cpu.Khz / 1000.0:0} MHz" : "—";
            tempValue.Text = cpu is { TempValid: true } ? $"{cpu.TempSoC}℃" : "—";
            tempValue.ForeColor = cpu is { TempValid: true, TempSoC: >= 70 } ? Color.FromArgb(255, 140, 120) : Color.White;
            batteryValue.Text = battery.Percent is int pct ? $"{pct}%" : "—";
            brightValue.Text = light != null ? $"{Backlight.LevelToPercent(light.Level)}%" : "—";
            loadValue.Text = usage.Total is int load ? $"{load}%" : "—";
            memoryValue.Text = usage.MemoryTotalMb > 0 ? $"{usage.MemoryUsedMb / 1024.0:0.0}/{usage.MemoryTotalMb / 1024.0:0.0} GB" : "—";

            string cores = usage.PerCore.Length > 0 ? string.Join("  ", Array.ConvertAll(usage.PerCore, c => c + "%")) : "—";
            string limit = cpu == null ? "—" : cpu.Throttled ? T("制限中（標準に下げています）", "active (held at standard)") : T("なし", "none");
            details.Text =
                T("コアごとの使用率: ", "Usage per core: ") + cores + "\n" +
                T("CPU の電圧: ", "CPU voltage: ") + (cpu != null ? cpu.Mv + " mV" : "—") + "\n" +
                T("基板の温度: ", "Board temperature: ") + (cpu is { TempValid: true } ? cpu.TempBoard + "℃" : "—") +
                T("    ファン: ", "    Fan: ") + (fan is int f ? f + "%" : "—") + "\n" +
                T("電源: ", "Power: ") + battery.PowerText +
                T("    バッテリーの電圧: ", "    Battery voltage: ") + (battery.Millivolts is int mv ? mv + " mV" : "—") + "\n" +
                "USB-C: " + battery.UsbcText + "\n" +
                T("温度による制限: ", "Thermal limit: ") + limit;

            if (cpu != null)
            {
                updatingRadios = true;
                foreach (RadioButton r in profileButtons)
                {
                    r.Checked = (int)r.Tag == cpu.Requested;
                    r.Font = (int)r.Tag == cpu.Active ? bodyBoldFont : bodyFont;
                }
                updatingRadios = false;
                perfStatus.Text = T("今: ", "Now: ") + $"{cpu.Khz / 1000.0:0.0} MHz / {cpu.Mv} mV" +
                                  (cpu.Throttled ? T("（温度で標準に制限中）", " (held at standard: too hot)") : "");
            }
            else
            {
                perfStatus.Text = T("CPU のドライバーに接続できません。", "Cannot reach the CPU clock driver.");
            }

            if (light != null)
            {
                if (!brightnessApply.Enabled && !brightnessBar.Capture)
                {
                    updatingBar = true;
                    brightnessBar.Value = Math.Clamp(Backlight.LevelToPercent(light.Level), brightnessBar.Minimum, brightnessBar.Maximum);
                    brightnessLabel.Text = T("明るさ: ", "Brightness: ") + $"{brightnessBar.Value}%";
                    updatingBar = false;
                }
                displayState.Text = T("画面の状態: ", "Display: ") + light.DisplayState switch
                {
                    0 => T("オフ", "off"),
                    2 => T("暗くしている", "dimmed"),
                    _ => T("オン", "on"),
                };
                brightnessBar.Enabled = !autoBright.Checked;
            }
            else
            {
                brightnessLabel.Text = T("バックライトのドライバーに接続できません（ファンのドライバー v2 と UEFI R376 以降が必要）。",
                                         "Cannot reach the backlight driver (needs the fan driver v2 and UEFI R376 or later).");
                brightnessBar.Enabled = false;
            }
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                timer.Dispose();
                brightnessApply.Dispose();
                sensorTimer.Dispose();
                tileValueFont.Dispose();
                tileTitleFont.Dispose();
                bodyFont.Dispose();
                bodyBoldFont.Dispose();
            }
            base.Dispose(disposing);
        }
    }

    /// <summary>Last 150 samples (5 min at 2 s) of SoC temperature, CPU clock and CPU usage.</summary>
    sealed class HistoryChart : Control
    {
        const int Capacity = 150;
        readonly Queue<(int? temp, int mhz, int? load)> samples = new Queue<(int?, int, int?)>();

        public HistoryChart()
        {
            DoubleBuffered = true;
            BackColor = Color.FromArgb(20, 24, 40);
        }

        public void Add(int? temp, int mhz, int? load)
        {
            samples.Enqueue((temp, mhz, load));
            while (samples.Count > Capacity) samples.Dequeue();
            Invalidate();
        }

        public void CopyFrom(HistoryChart other)
        {
            samples.Clear();
            foreach (var s in other.samples) samples.Enqueue(s);
            Invalidate();
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            var area = new Rectangle(36, 8, Math.Max(10, Width - 80), Math.Max(10, Height - 24));
            using var grid = new Pen(Color.FromArgb(50, 60, 90));
            using var textBrush = new SolidBrush(Color.FromArgb(170, 180, 210));
            using var font = new Font("Segoe UI", 8f);
            for (int i = 0; i <= 4; i++)
            {
                int y = area.Top + area.Height * i / 4;
                g.DrawLine(grid, area.Left, y, area.Right, y);
                g.DrawString($"{90 - i * 15}℃", font, textBrush, 0, y - 7);              // 30..90 C
                g.DrawString($"{2000 - i * 400}", font, textBrush, area.Right + 4, y - 7); // 400..2000 MHz
            }
            if (samples.Count < 2) return;
            var tempPts = new List<PointF>();
            var mhzPts = new List<PointF>();
            var loadPts = new List<PointF>();
            int index = Capacity - samples.Count;
            foreach (var (temp, mhz, load) in samples)
            {
                float x = area.Left + area.Width * index / (float)(Capacity - 1);
                if (temp is int t) tempPts.Add(new PointF(x, area.Bottom - area.Height * (Math.Clamp(t, 30, 90) - 30) / 60f));
                mhzPts.Add(new PointF(x, area.Bottom - area.Height * (Math.Clamp(mhz, 400, 2000) - 400) / 1600f));
                if (load is int l) loadPts.Add(new PointF(x, area.Bottom - area.Height * Math.Clamp(l, 0, 100) / 100f));
                index++;
            }
            using var bluePen = new Pen(Color.FromArgb(90, 176, 255), 2f);
            using var redPen = new Pen(Color.FromArgb(255, 120, 100), 2f);
            using var greenPen = new Pen(Color.FromArgb(120, 220, 140), 1.5f);
            if (loadPts.Count >= 2) g.DrawLines(greenPen, loadPts.ToArray());
            if (mhzPts.Count >= 2) g.DrawLines(bluePen, mhzPts.ToArray());
            if (tempPts.Count >= 2) g.DrawLines(redPen, tempPts.ToArray());
        }
    }
}
