// Disks the PC sees (never the disk the PC itself runs from), their GPT
// partitions, and temporary drive letters for partitions that have none
// (the ESP never has one).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading.Tasks;
using static SwitchCenter.Lang;

namespace SwitchPatcher
{
    sealed class PartInfo
    {
        public const string EspType = "{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}";
        public const string MsrType = "{e3c9e316-0b5c-4db8-817d-f92df00215ae}";
        public int Number;
        public string Letter;             // "" when it has none
        public long Size;
        public string GptType, FileSystem, Label;
        public bool IsEsp => string.Equals(GptType, EspType, StringComparison.OrdinalIgnoreCase);
        public bool HasWindows => Letter != "" && File.Exists(Letter + @":\Windows\System32\config\SOFTWARE");

        public override string ToString() =>
            $"#{Number}  {(Letter != "" ? Letter + ":" : T("（文字なし）", "(no letter)"))}  {FileSystem}  {Size / 1e9:0.0} GB  {Label}" +
            (HasWindows ? T("  ［Windows あり］", "  [Windows]") : "");
    }

    sealed class DiskInfo
    {
        public int Number;
        public string Name, Bus, Style;
        public long Size;
        public List<PartInfo> Partitions = new List<PartInfo>();
        public PartInfo Esp => Partitions.FirstOrDefault(p => p.IsEsp);
        /// <summary>SD card readers show up as SD, USB or MMC; anything else gets a warning.</summary>
        public bool LooksLikeSd => Bus is "SD" or "USB" or "MMC";

        public override string ToString() => $"{T("ディスク", "Disk")} {Number}: {Name}  {Size / 1e9:0.0} GB  ({Bus}, {Style})";
    }

    static class Disks
    {
        /// <summary>All disks except the ones the PC boots and runs from, and empty card reader slots (0 bytes).</summary>
        public static async Task<List<DiskInfo>> ListAsync()
        {
            using JsonDocument j = await Tools.PowerShellJson(@"
$list = @(Get-Disk | Where-Object { -not $_.IsBoot -and -not $_.IsSystem -and $_.Size -gt 0 } | ForEach-Object {
  $d = $_
  [pscustomobject]@{
    Number = $d.Number; Name = [string]$d.FriendlyName; Size = [long]$d.Size
    Bus = [string]$d.BusType; Style = [string]$d.PartitionStyle
    Partitions = @(Get-Partition -DiskNumber $d.Number -ErrorAction SilentlyContinue | ForEach-Object {
      $v = $_ | Get-Volume -ErrorAction SilentlyContinue
      [pscustomobject]@{
        Number = $_.PartitionNumber; Letter = $(if ($_.DriveLetter -and $_.DriveLetter -ne [char]0) { [string]$_.DriveLetter } else { '' })
        Size = [long]$_.Size; GptType = [string]$_.GptType
        FileSystem = [string]$v.FileSystem; Label = [string]$v.FileSystemLabel
      }
    })
  }
})
ConvertTo-Json -InputObject $list -Depth 5 -Compress");
            var disks = new List<DiskInfo>();
            foreach (JsonElement d in j.RootElement.EnumerateArray())
            {
                var disk = new DiskInfo
                {
                    Number = d.GetProperty("Number").GetInt32(),
                    Name = d.GetProperty("Name").GetString(),
                    Size = d.GetProperty("Size").GetInt64(),
                    Bus = d.GetProperty("Bus").GetString(),
                    Style = d.GetProperty("Style").GetString(),
                };
                foreach (JsonElement p in d.GetProperty("Partitions").EnumerateArray())
                    disk.Partitions.Add(new PartInfo
                    {
                        Number = p.GetProperty("Number").GetInt32(),
                        Letter = p.GetProperty("Letter").GetString(),
                        Size = p.GetProperty("Size").GetInt64(),
                        GptType = p.GetProperty("GptType").GetString(),
                        FileSystem = p.GetProperty("FileSystem").GetString(),
                        Label = p.GetProperty("Label").GetString(),
                    });
                disks.Add(disk);
            }
            return disks;
        }

        /// <summary>Checks again, just before writing, that the disk is still not one the PC runs from.</summary>
        public static async Task CheckNotSystemDisk(int disk)
        {
            string r = (await Tools.PowerShell($"$d = Get-Disk -Number {disk}; [string]($d.IsBoot -or $d.IsSystem)")).Trim();
            if (r != "False") throw new InvalidOperationException(T("このディスクは、PC が起動しているディスクです。中止しました。", "That disk is the one this PC runs from. Stopped."));
        }

        /// <summary>The partition's drive letter, assigning a temporary one if it has none (added to assigned, to remove later).</summary>
        public static async Task<string> EnsureLetter(int disk, PartInfo part, List<(int Disk, int Part, string Letter)> assigned)
        {
            if (part.Letter != "") return part.Letter;
            string letter = (await Tools.PowerShell(
                $"Add-PartitionAccessPath -DiskNumber {disk} -PartitionNumber {part.Number} -AssignDriveLetter; " +
                $"[string](Get-Partition -DiskNumber {disk} -PartitionNumber {part.Number}).DriveLetter")).Trim();
            if (letter.Length != 1 || !char.IsLetter(letter[0])) throw new InvalidOperationException(T("ドライブ文字を付けられませんでした。", "Could not assign a drive letter."));
            assigned.Add((disk, part.Number, letter));
            return letter;
        }

        public static async Task RemoveLetter(int disk, int part, string letter)
        {
            try { await Tools.PowerShell($"Remove-PartitionAccessPath -DiskNumber {disk} -PartitionNumber {part} -AccessPath '{letter}:\\'"); }
            catch (InvalidOperationException) { }
        }

        /// <summary>
        /// Erases the whole disk and makes the Switch SD layout: GPT, an ESP
        /// (FAT32, 260 MB), the Microsoft reserved partition (16 MB, as Windows
        /// Setup makes it) and an NTFS partition for Windows on the rest. Only for
        /// SD / USB card readers (and virtual disks, for testing). The ESP is made
        /// as a data partition first and retyped after formatting, because
        /// Windows does not let Format-Volume open a partition that is already an ESP.
        /// </summary>
        public static async Task InitializeSd(DiskInfo disk, Action<string> log)
        {
            if (!disk.LooksLikeSd && disk.Bus != "File Backed Virtual")
                throw new InvalidOperationException(T($"このディスクの接続は {disk.Bus} です。SD カード以外は初期化しません。", $"This disk is on {disk.Bus}; only SD cards are initialized."));
            await CheckNotSystemDisk(disk.Number);
            int n = disk.Number;
            log(T($"ディスク {n} を消しています…", $"Erasing disk {n}..."));
            await Tools.PowerShell($@"
$d = Get-Disk -Number {n}
if ($d.PartitionStyle -ne 'RAW') {{ Clear-Disk -Number {n} -RemoveData -RemoveOEM -Confirm:$false }}
# A removable SD is often left initialized (MBR) after Clear-Disk instead of RAW.
Update-Disk -Number {n}
$d = Get-Disk -Number {n}
if ($d.PartitionStyle -eq 'RAW') {{ Initialize-Disk -Number {n} -PartitionStyle GPT }}
elseif ($d.PartitionStyle -ne 'GPT') {{ Set-Disk -Number {n} -PartitionStyle GPT }}
Update-Disk -Number {n}
$d = Get-Disk -Number {n}
if ($d.PartitionStyle -ne 'GPT') {{ throw ""disk {n} is $($d.PartitionStyle), not GPT"" }}
if (@(Get-Partition -DiskNumber {n} -ErrorAction SilentlyContinue | Where-Object {{ $_.Type -ne 'Reserved' }}).Count) {{ throw ""disk {n} still has partitions"" }}");
            log(T("ESP（FAT32、260 MB）、MSR（16 MB）と Windows の区画（NTFS）を作っています…", "Creating the ESP (FAT32, 260 MB), the MSR (16 MB) and the Windows partition (NTFS)..."));
            await Tools.PowerShell($@"
$esp = New-Partition -DiskNumber {n} -Size 260MB
$esp | Format-Volume -FileSystem FAT32 -NewFileSystemLabel ESP -Confirm:$false | Out-Null
Set-Partition -DiskNumber {n} -PartitionNumber $esp.PartitionNumber -GptType '{PartInfo.EspType}'
New-Partition -DiskNumber {n} -Size 16MB -GptType '{PartInfo.MsrType}' | Out-Null
New-Partition -DiskNumber {n} -UseMaximumSize | Format-Volume -FileSystem NTFS -NewFileSystemLabel Windows -Confirm:$false | Out-Null");
        }

        /// <summary>Quick NTFS format of the Windows partition only (never the ESP; checked again here).</summary>
        public static async Task FormatWindowsPartition(int disk, PartInfo part, string letter)
        {
            if (part.IsEsp) throw new InvalidOperationException("refusing to format the ESP");
            await Tools.PowerShell($"Format-Volume -DriveLetter {letter} -FileSystem NTFS -NewFileSystemLabel Windows -Force -Confirm:$false | Out-Null");
        }
    }

    /// <summary>One edition inside install.wim / install.esd.</summary>
    sealed class ImageEdition
    {
        public int Index;
        public string Name, Version;
        public int Architecture;          // 12 = ARM64, 9 = x64
        public bool IsArm64 => Architecture == 12;
        public override string ToString() => $"{Index}: {Name}  {Version}  {(IsArm64 ? "ARM64" : Architecture == 9 ? "x64" : "arch " + Architecture)}";
    }

    static class Images
    {
        /// <summary>Mounts an ISO and returns its drive letter.</summary>
        public static async Task<string> MountIso(string iso) =>
            (await Tools.PowerShell($"[string](Mount-DiskImage -ImagePath {Tools.Quote(iso)} -PassThru | Get-Volume).DriveLetter")).Trim();

        public static async Task DismountIso(string iso)
        {
            try { await Tools.PowerShell($"Dismount-DiskImage -ImagePath {Tools.Quote(iso)} | Out-Null"); }
            catch (InvalidOperationException) { }
        }

        /// <summary>install.wim or install.esd under an ISO's (or a folder's) sources.</summary>
        public static string FindInstallImage(string root)
        {
            foreach (string name in new[] { "install.wim", "install.esd" })
            {
                string p = Path.Combine(root, "sources", name);
                if (File.Exists(p)) return p;
            }
            throw new FileNotFoundException(T("ISO の中に sources\\install.wim（.esd）がありません。", "The ISO has no sources\\install.wim (.esd)."));
        }

        public static async Task<List<ImageEdition>> ListEditions(string image)
        {
            using JsonDocument j = await Tools.PowerShellJson($@"
$p = {Tools.Quote(image)}
$list = @(Get-WindowsImage -ImagePath $p | ForEach-Object {{
  $i = Get-WindowsImage -ImagePath $p -Index $_.ImageIndex
  [pscustomobject]@{{ Index = $i.ImageIndex; Name = [string]$i.ImageName; Version = [string]$i.Version; Architecture = [int]$i.Architecture }}
}})
ConvertTo-Json -InputObject $list -Compress");
            return j.RootElement.EnumerateArray().Select(e => new ImageEdition
            {
                Index = e.GetProperty("Index").GetInt32(),
                Name = e.GetProperty("Name").GetString(),
                Version = e.GetProperty("Version").GetString(),
                Architecture = e.GetProperty("Architecture").GetInt32(),
            }).ToList();
        }
    }
}
