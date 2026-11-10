// One window: choose the SD and its Windows partition, what to do, then a log.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Forms;
using static SwitchCenter.Lang;

namespace SwitchPatcher
{
    sealed class MainForm : Form
    {
        readonly ComboBox diskBox = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 520 };
        readonly ComboBox partBox = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 520 };
        readonly Label diskNote = new Label { AutoSize = true, MaximumSize = new Size(640, 0), ForeColor = Color.DarkOrange };
        readonly RadioButton patchRadio = new RadioButton { AutoSize = true, Checked = true };
        readonly RadioButton installRadio = new RadioButton { AutoSize = true };
        readonly RadioButton newSdRadio = new RadioButton { AutoSize = true };
        readonly TextBox imagePath = new TextBox { Width = 430, ReadOnly = true };
        readonly Button browse = new Button { Width = 84, Height = 28 };
        readonly ComboBox editionBox = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 520 };
        readonly CheckBox drivers = new CheckBox { AutoSize = true, Checked = true };
        readonly CheckBox testMode = new CheckBox { AutoSize = true, Checked = true };
        readonly CheckBox controlCenter = new CheckBox { AutoSize = true, Checked = true };
        readonly Button start = new Button { Width = 160, Height = 36 };
        readonly Label progressLabel = new Label { AutoSize = true };
        readonly TextBox logBox = new TextBox { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Dock = DockStyle.Fill, Font = new Font("Consolas", 9f) };
        bool running;

        public MainForm()
        {
            Text = "SwitchPatcher " + Application.ProductVersion.Split('+')[0];
            Font = new Font("Yu Gothic UI", 9.5f);
            AutoScaleMode = AutoScaleMode.Dpi;
            StartPosition = FormStartPosition.CenterScreen;
            ClientSize = new Size(700, 760);
            MinimumSize = new Size(600, 600);

            var refresh = new Button { Text = T("再読み込み", "Refresh"), Width = 100, Height = 28 };
            refresh.Click += async (_, _) => await LoadDisks();
            diskBox.SelectedIndexChanged += (_, _) => ShowPartitions();
            patchRadio.Text = T("入っている Windows にパッチを当てる", "Patch the Windows on the SD");
            installRadio.Text = T("Windows のイメージを入れる（Windows の区画を消して入れ直す）", "Install a Windows image (erases the Windows partition)");
            newSdRadio.Text = T("新しい SD に Windows を入れる（SD 全体を消して、区画を作る）", "Install Windows on a new SD (erases the whole SD and makes the partitions)");
            patchRadio.CheckedChanged += (_, _) => UpdateEnabled();
            installRadio.CheckedChanged += (_, _) => UpdateEnabled();
            browse.Text = T("選ぶ…", "Browse...");
            browse.Click += async (_, _) => await ChooseImage();
            drivers.Text = T("Switch のドライバー（最新のリリース）", "Switch drivers (latest release)");
            testMode.Text = T("テストモードとテスト証明書（ドライバーに必要）", "Test signing and the test certificate (needed by the drivers)");
            controlCenter.Text = T("コントロールセンター（ログインしたときに起動）", "Control center (starts at login)");
            start.Text = T("開始", "Start");
            start.Click += async (_, _) => await Run();

            var top = new FlowLayoutPanel { Dock = DockStyle.Top, AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, Padding = new Padding(12) };
            top.Controls.Add(Caption(T("1. SD カード", "1. SD card")));
            var diskRow = new FlowLayoutPanel { AutoSize = true };
            diskRow.Controls.Add(diskBox);
            diskRow.Controls.Add(refresh);
            top.Controls.Add(diskRow);
            top.Controls.Add(diskNote);
            top.Controls.Add(new Label { Text = T("Windows の区画", "Windows partition"), AutoSize = true, Margin = new Padding(3, 8, 3, 0) });
            top.Controls.Add(partBox);
            top.Controls.Add(Caption(T("2. すること", "2. What to do")));
            top.Controls.Add(patchRadio);
            top.Controls.Add(installRadio);
            top.Controls.Add(newSdRadio);
            var imageRow = new FlowLayoutPanel { AutoSize = true, Margin = new Padding(20, 0, 0, 0) };
            imageRow.Controls.Add(imagePath);
            imageRow.Controls.Add(browse);
            top.Controls.Add(imageRow);
            editionBox.Margin = new Padding(23, 3, 3, 3);
            top.Controls.Add(editionBox);
            top.Controls.Add(Caption(T("3. 入れるもの", "3. What to add")));
            top.Controls.Add(drivers);
            top.Controls.Add(testMode);
            top.Controls.Add(controlCenter);
            var runRow = new FlowLayoutPanel { AutoSize = true, Margin = new Padding(3, 14, 3, 3) };
            runRow.Controls.Add(start);
            progressLabel.Margin = new Padding(10, 10, 3, 3);
            runRow.Controls.Add(progressLabel);
            top.Controls.Add(runRow);

            var logPanel = new Panel { Dock = DockStyle.Fill, Padding = new Padding(12, 0, 12, 12) };
            logPanel.Controls.Add(logBox);
            Controls.Add(logPanel);
            Controls.Add(top);
            UpdateEnabled();
            Shown += async (_, _) => await LoadDisks();
            FormClosing += (_, e) =>
            {
                if (running && e.CloseReason == CloseReason.UserClosing &&
                    MessageBox.Show(this, T("作業の途中です。閉じると、SD の Windows が途中の状態で残ります。閉じますか？", "Work is in progress; closing leaves the SD half done. Close anyway?"),
                                    Text, MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes)
                    e.Cancel = true;
            };
        }

        static Label Caption(string text) => new Label { Text = text, AutoSize = true, Font = new Font("Yu Gothic UI", 10.5f, FontStyle.Bold), Margin = new Padding(3, 12, 3, 4) };

        void Log(string line)
        {
            if (InvokeRequired) { BeginInvoke(new Action<string>(Log), line); return; }
            logBox.AppendText(line + Environment.NewLine);
        }

        void Progress(string text)
        {
            if (InvokeRequired) { BeginInvoke(new Action<string>(Progress), text); return; }
            progressLabel.Text = text;
        }

        void UpdateEnabled()
        {
            bool idle = !running;
            imagePath.Enabled = browse.Enabled = editionBox.Enabled = idle && !patchRadio.Checked;
            diskBox.Enabled = patchRadio.Enabled = installRadio.Enabled = newSdRadio.Enabled = idle;
            partBox.Enabled = idle && !newSdRadio.Checked;      // a new SD gets its partitions made
            drivers.Enabled = testMode.Enabled = controlCenter.Enabled = start.Enabled = idle;
        }

        async Task LoadDisks()
        {
            diskBox.Items.Clear();
            partBox.Items.Clear();
            try
            {
                List<DiskInfo> disks = await Disks.ListAsync();
                foreach (DiskInfo d in disks) diskBox.Items.Add(d);
                // Prefer a disk that looks like the Switch SD: an SD/USB disk with an ESP and Windows on it.
                DiskInfo best = disks.FirstOrDefault(d => d.LooksLikeSd && d.Esp != null && d.Partitions.Any(p => p.HasWindows))
                             ?? disks.FirstOrDefault(d => d.LooksLikeSd);
                if (best != null) diskBox.SelectedItem = best;
                else if (disks.Count == 0) diskNote.Text = T("SD カードが見つかりません。挿してから「再読み込み」を押してください。", "No SD card found. Insert it and press Refresh.");
            }
            catch (InvalidOperationException e)
            {
                diskNote.Text = e.Message;
            }
        }

        void ShowPartitions()
        {
            partBox.Items.Clear();
            if (diskBox.SelectedItem is not DiskInfo d) return;
            foreach (PartInfo p in d.Partitions.Where(p => !p.IsEsp && (p.FileSystem == "NTFS" || p.FileSystem == ""))) partBox.Items.Add(p);
            PartInfo best = d.Partitions.FirstOrDefault(p => p.HasWindows) ??
                            d.Partitions.Where(p => !p.IsEsp && p.FileSystem == "NTFS").OrderByDescending(p => p.Size).FirstOrDefault();
            if (best != null) partBox.SelectedItem = best;
            var notes = new List<string>();
            if (!d.LooksLikeSd) notes.Add(T($"注意: このディスクの接続は {d.Bus} です。SD カードか、もう一度確かめてください。", $"Note: this disk is on {d.Bus}. Make sure it is the SD card."));
            if (d.Style != "GPT") notes.Add(T("この SD は GPT ではありません。今の版は GPT の SD だけに対応しています。", "This SD is not GPT; this version supports GPT SD cards only."));
            if (d.Esp == null) notes.Add(T("この SD には ESP（EFI システム区画）がありません。", "This SD has no EFI system partition."));
            if (d.Style != "GPT" || d.Esp == null)
                notes.Add(T("新しい SD なら、「新しい SD に Windows を入れる」を選んでください。", "For a new SD, choose \"Install Windows on a new SD\"."));
            diskNote.Text = string.Join("\n", notes);
        }

        async Task ChooseImage()
        {
            using var dialog = new OpenFileDialog
            {
                Title = T("Windows のイメージ（ARM64）", "Windows image (ARM64)"),
                Filter = T("Windows のイメージ", "Windows image") + " (*.iso;*.wim;*.esd)|*.iso;*.wim;*.esd",
            };
            if (dialog.ShowDialog(this) != DialogResult.OK) return;
            imagePath.Text = dialog.FileName;
            editionBox.Items.Clear();
            Progress(T("エディションを読んでいます…", "Reading the editions..."));
            string iso = null;
            try
            {
                string image = dialog.FileName;
                if (image.EndsWith(".iso", StringComparison.OrdinalIgnoreCase))
                {
                    iso = image;
                    image = Images.FindInstallImage(await Images.MountIso(iso) + @":\");
                }
                foreach (ImageEdition e in await Images.ListEditions(image)) if (e.IsArm64) editionBox.Items.Add(e);
                if (editionBox.Items.Count > 0) editionBox.SelectedIndex = 0;
                Progress(editionBox.Items.Count > 0 ? "" : T("このイメージには ARM64 のエディションがありません。", "This image has no ARM64 edition."));
            }
            catch (Exception e) when (e is InvalidOperationException || e is FileNotFoundException || e is System.Text.Json.JsonException)
            {
                Progress(T("読めませんでした: ", "Could not read it: ") + e.Message);
            }
            finally
            {
                if (iso != null) await Images.DismountIso(iso);
            }
        }

        async Task Run()
        {
            bool newSd = newSdRadio.Checked;
            if (diskBox.SelectedItem is not DiskInfo disk || (!newSd && partBox.SelectedItem is not PartInfo))
            {
                MessageBox.Show(this, T("SD カードと Windows の区画を選んでください。", "Choose the SD card and its Windows partition."), Text);
                return;
            }
            var part = partBox.SelectedItem as PartInfo;
            var o = new PatchOptions
            {
                Disk = disk, Windows = part, InitializeDisk = newSd, InstallImage = !patchRadio.Checked,
                Drivers = drivers.Checked, TestMode = testMode.Checked, ControlCenter = controlCenter.Checked,
            };
            if (o.InstallImage)
            {
                if (editionBox.SelectedItem is not ImageEdition e)
                {
                    MessageBox.Show(this, T("Windows のイメージと、ARM64 のエディションを選んでください。", "Choose a Windows image and an ARM64 edition."), Text);
                    return;
                }
                o.ImagePath = imagePath.Text;
                o.ImageIndex = e.Index;
            }
            if (!newSd && (disk.Esp == null || disk.Style != "GPT"))
            {
                MessageBox.Show(this, diskNote.Text, Text);
                return;
            }

            var steps = new List<string>();
            if (newSd)
            {
                steps.Add(T($"・SD 全体（{disk.Size / 1e9:0.0} GB）を消して、GPT・ESP（260 MB）・Windows の区画（NTFS、残り全部）を作る",
                            $"- Erase the whole SD ({disk.Size / 1e9:0.0} GB) and make GPT, an ESP (260 MB) and a Windows partition (NTFS, the rest)"));
                steps.Add(T($"・次を入れる: {editionBox.SelectedItem}", $"- Install: {editionBox.SelectedItem}"));
            }
            else if (o.InstallImage)
            {
                steps.Add(T($"・Windows の区画（#{part.Number}、{part.Size / 1e9:0.0} GB）を消して、次を入れる: {editionBox.SelectedItem}",
                            $"- Erase the Windows partition (#{part.Number}, {part.Size / 1e9:0.0} GB) and install: {editionBox.SelectedItem}"));
                steps.Add(T("・ESP の起動ファイルを PC に保存してから、起動の設定を作り直す（bcdboot）", "- Save the ESP boot files to the PC, then rebuild the boot files (bcdboot)"));
            }
            if (o.InstallImage) steps.Add(T("・初回起動で Microsoft アカウントとネットワークを求めないようにする", "- First boot without a Microsoft account or network"));
            if (o.Drivers) steps.Add(T("・Switch のドライバーを入れる", "- Add the Switch drivers"));
            if (o.TestMode) steps.Add(T("・テスト証明書を入れて、テストモードを有効にする（BCD は先に PC に保存する）", "- Add the test certificate and turn on test signing (the BCD is saved to the PC first)"));
            if (o.ControlCenter) steps.Add(T("・コントロールセンターを C:\\SwitchWOA\\SwitchCenter に置く", "- Put the control center in C:\\SwitchWOA\\SwitchCenter"));
            if (steps.Count == 0) return;
            if (MessageBox.Show(this, $"{disk}\n\n" + string.Join("\n", steps) + T("\n\n始めますか？", "\n\nStart?"), Text,
                                MessageBoxButtons.OKCancel, o.InstallImage ? MessageBoxIcon.Warning : MessageBoxIcon.Question) != DialogResult.OK) return;
            if (newSd ? !ConfirmErase(disk.Size, T("SD 全体", "the whole SD")) : o.InstallImage && !ConfirmErase(part.Size, T("Windows の区画", "the Windows partition"))) return;

            running = true;
            UpdateEnabled();
            logBox.Clear();
            try
            {
                await Task.Run(() => Patcher.RunAsync(o, Log, Progress));
                Progress(T("完了", "Done"));
            }
            catch (Exception e)
            {
                Log("");
                Log(T("中止しました: ", "Stopped: ") + e.Message);
                Progress(T("中止しました", "Stopped"));
            }
            finally
            {
                running = false;
                UpdateEnabled();
                await LoadDisks();
            }
        }

        /// <summary>Erasing needs the size typed back, so a wrong choice is hard to confirm by accident.</summary>
        bool ConfirmErase(long size, string what)
        {
            string expected = $"{size / 1e9:0.0}";
            using var f = new Form
            {
                Text = Text, FormBorderStyle = FormBorderStyle.FixedDialog, StartPosition = FormStartPosition.CenterParent,
                ClientSize = new Size(460, 150), MinimizeBox = false, MaximizeBox = false, Font = Font,
            };
            var label = new Label
            {
                AutoSize = true, MaximumSize = new Size(430, 0), Location = new Point(12, 12),
                Text = T($"{what}の中身は、すべて消えます。\n消してよければ、大きさ「{expected}」（GB）を入力してください。",
                         $"Everything on {what} will be erased.\nType its size \"{expected}\" (GB) to continue."),
            };
            var box = new TextBox { Location = new Point(12, 70), Width = 160 };
            var ok = new Button { Text = "OK", DialogResult = DialogResult.OK, Location = new Point(260, 104), Width = 90 };
            var cancel = new Button { Text = T("キャンセル", "Cancel"), DialogResult = DialogResult.Cancel, Location = new Point(358, 104), Width = 90 };
            f.Controls.AddRange(new Control[] { label, box, ok, cancel });
            f.AcceptButton = ok;
            f.CancelButton = cancel;
            return f.ShowDialog(this) == DialogResult.OK && box.Text.Trim() == expected;
        }
    }
}
