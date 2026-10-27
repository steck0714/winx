<#
.SYNOPSIS
  Build every driver and the control center, then make the files for one
  GitHub release:
    out\<Version>\SwitchDrivers-<Version>.zip      kits\Switch-* with the built .sys/.cat/.inf
    out\<Version>\SwitchCenter-<app>-arm64.zip     the control center (self-contained, win-arm64)
    out\<Version>\manifest.json                    versions, hardware IDs, minimum UEFI, SHA-256
  Upload the three files to a release tagged drivers-<Version>, or pass -Publish
  to have the GitHub CLI (gh, logged in with `gh auth login`) create the release.
  -PublishOnly publishes files that an earlier run already made.

  Needs (on the PC): LLVM, the WDK 10.0.19041 + 10.0.26100 headers, the .NET SDK,
  and the test-signing certificate in the certificate store (see tools\Build-Driver.ps1).

.EXAMPLE
  .\release\Make-Release.ps1 -Version 2026.10.26 -Publish
#>
param(
  [Parameter(Mandatory = $true)][ValidatePattern('^\d{4}\.\d{1,2}\.\d{1,2}(\.\d+)?$')][string]$Version,
  [switch]$Publish,
  [switch]$PublishOnly
)

$Repo = 'hayato41200/Windows-for-Switch-Drivers'

function Publish-Release([string]$dir) {
  $gh = (Get-Command gh -ErrorAction SilentlyContinue).Source
  if (-not $gh) { $gh = Join-Path $env:ProgramFiles 'GitHub CLI\gh.exe' }
  if (-not (Test-Path $gh)) { throw 'GitHub CLI (gh) not found. Install it with: winget install GitHub.cli' }
  & $gh auth status 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'gh is not logged in. Run: gh auth login' }
  $m = Get-Content (Join-Path $dir 'manifest.json') -Raw | ConvertFrom-Json
  $notes = @("Drivers $Version", '', '| Driver | Version | Device | Needs UEFI |', '|---|---|---|---|')
  $notes += $m.drivers.items | ForEach-Object { "| $($_.service) | $($_.version) | $($_.hwid) | $($_.minUefi) |" }
  $notes += '', "Control center: $($m.app.version) ($($m.app.file))", '', 'SHA-256 values are in manifest.json.'
  $notesFile = Join-Path $dir 'release-notes.md'
  $notes -join "`n" | Set-Content $notesFile -Encoding utf8
  $files = Get-ChildItem $dir -File | Where-Object { $_.Name -ne 'release-notes.md' } | ForEach-Object FullName
  & $gh release create "drivers-$Version" @files --repo $Repo --title "Drivers $Version" --notes-file $notesFile
  if ($LASTEXITCODE -ne 0) { throw 'gh release create failed' }
}

if ($PublishOnly) {
  Publish-Release (Join-Path (Split-Path -Parent $PSScriptRoot) "out\$Version")
  return
}

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Out = Join-Path $Root "out\$Version"
$Stage = Join-Path $Out 'stage'
if (Test-Path $Out) { throw "$Out already exists; use a new version." }
New-Item -ItemType Directory -Force $Stage | Out-Null

$Spb = '$Kits\Include\10.0.19041.0\km\spb\1.0', '$Kits\Include\10.0.26100.0\shared'
# Dir, driver name, source, libraries, extra includes, kit folder, hardware ID, minimum UEFI,
# display names, and Boot = replaced at the next boot by the control center (other drivers sit on it)
$Drivers = @(
  @{ Dir = 'Tegra210Gpio';      Name = 'tegra210gpio';  Src = 'tegra210gpio.c';  Libs = @('msgpioclxstub.lib');          Inc = @();  Kit = 'Switch-Stage1';      Hwid = 'ACPI\SWOA0101'; MinUefi = 'R367'; Ja = 'GPIO コントローラー'; En = 'GPIO controller'; Boot = $true }
  @{ Dir = 'Tegra210I2c';       Name = 'tegra210i2c';   Src = 'tegra210i2c.c';   Libs = @('spb\1.0\spbcxstubs.lib');     Inc = $Spb; Kit = 'Switch-Stage3';      Hwid = 'ACPI\SWOA0201'; MinUefi = 'R370'; Ja = 'I2C コントローラー'; En = 'I2C controller'; Boot = $true }
  @{ Dir = 'SwitchBattery';     Name = 'switchbattery'; Src = 'switchbattery.c'; Libs = @('battc.lib');                  Inc = $Spb; Kit = 'Switch-Battery';     Hwid = 'ACPI\SWOA0301'; MinUefi = 'R371'; Ja = 'バッテリー・USB-C'; En = 'Battery, USB-C'; Boot = $false }
  @{ Dir = 'SwitchFan';         Name = 'switchfan';     Src = 'switchfan.c';     Libs = @();                             Inc = $Spb; Kit = 'Switch-Fan';         Hwid = 'ACPI\SWOA0401'; MinUefi = 'R372'; Ja = 'ファン・バックライト'; En = 'Fan, backlight'; Boot = $false }
  @{ Dir = 'SwitchTouch';       Name = 'switchtouch';   Src = 'switchtouch.c';   Libs = @('vhfkm.lib', 'libcntpr.lib');  Inc = $Spb; Kit = 'Switch-Touch';       Hwid = 'ACPI\SWOA0501'; MinUefi = 'R373'; Ja = 'タッチパネル'; En = 'Touch screen'; Boot = $false }
  @{ Dir = 'SwitchPowerButton'; Name = 'switchpwrbtn';  Src = 'switchpwrbtn.c';  Libs = @('vhfkm.lib', 'libcntpr.lib');  Inc = $Spb; Kit = 'Switch-PowerButton'; Hwid = 'ACPI\SWOA0601'; MinUefi = 'R374'; Ja = '電源ボタン'; En = 'Power button'; Boot = $false }
  @{ Dir = 'SwitchCpuClock';    Name = 'switchcpu';     Src = 'switchcpu.c';     Libs = @();                             Inc = $Spb; Kit = 'Switch-CpuClock';    Hwid = 'ACPI\SWOA0701'; MinUefi = 'R375'; Ja = 'CPU のクロック'; En = 'CPU clock'; Boot = $false }
  @{ Dir = 'SwitchRtc';         Name = 'switchrtc';     Src = 'switchrtc.c';     Libs = @();                             Inc = $Spb; Kit = 'Switch-Rtc';         Hwid = 'ACPI\SWOA0801'; MinUefi = 'R377'; Ja = '時計（RTC）'; En = 'Clock (RTC)'; Boot = $false }
  @{ Dir = 'SwoaDiag';          Name = 'swoadiag';      Src = 'swoadiag.c';      Libs = @();                             Inc = @();  Kit = 'Switch-UsbDiag';     Hwid = '';              MinUefi = '' }
  @{ Dir = 'SwoaHello';         Name = 'SwoaHello';     Src = 'hello.c';         Libs = @();                             Inc = @();  Kit = 'Switch-Stage0';      Hwid = '';              MinUefi = '' }
)

function Sha256([string]$path) { (Get-FileHash -Algorithm SHA256 $path).Hash.ToLowerInvariant() }

# ---------------------------------------------------------------- drivers
$DriverStage = Join-Path $Stage 'SwitchDrivers'
$manifestDrivers = @()
foreach ($d in $Drivers) {
  $dir = Join-Path $Root "drivers\$($d.Dir)"
  $build = Join-Path $Root "build\$($d.Dir)"
  $inf = Join-Path $dir "$($d.Name).inf"
  $args = @{ Name = $d.Name; Sources = (Join-Path $dir "src\$($d.Src)"); OutDir = $build }
  if (Test-Path $inf) { $args.Inf = $inf }
  if ($d.Libs.Count) { $args.Libs = $d.Libs }
  if ($d.Inc.Count) { $args.ExtraIncludes = $d.Inc }
  Write-Host "=== $($d.Name)"
  & (Join-Path $Root 'tools\Build-Driver.ps1') @args | Out-Host

  $kit = Join-Path $DriverStage $d.Kit
  New-Item -ItemType Directory -Force $kit | Out-Null
  Copy-Item (Join-Path $Root "kits\$($d.Kit)\*") $kit -Recurse
  foreach ($ext in 'sys', 'cat', 'inf') {
    $f = Join-Path $build "$($d.Name).$ext"
    if (Test-Path $f) { Copy-Item $f $kit }
  }
  if ($d.Hwid) {
    $ver = (Select-String -Path $inf -Pattern '^\s*DriverVer\s*=\s*[^,]+,\s*(\S+)').Matches[0].Groups[1].Value
    $manifestDrivers += [ordered]@{
      service = $d.Name; kit = $d.Kit; inf = "$($d.Kit)/$($d.Name).inf"; hwid = $d.Hwid
      version = $ver; minUefi = $d.MinUefi; sysSha256 = (Sha256 (Join-Path $build "$($d.Name).sys"))
      nameJa = $d.Ja; nameEn = $d.En; installAt = $(if ($d.Boot) { 'boot' } else { 'live' })
    }
  }
}
Copy-Item (Join-Path $Root 'tools\cert\SwitchWOA-Test.cer') $DriverStage
$driversZip = Join-Path $Out "SwitchDrivers-$Version.zip"
Compress-Archive -Path (Join-Path $DriverStage '*') -DestinationPath $driversZip -CompressionLevel Optimal

# ------------------------------------------------------- control center
$csproj = Join-Path $Root 'app\SwitchCenter\src\SwitchCenter.csproj'
$appVersion = ([xml](Get-Content $csproj)).Project.PropertyGroup.Version | Where-Object { $_ } | Select-Object -First 1
$appStage = Join-Path $Stage 'SwitchCenter'
Write-Host "=== SwitchCenter $appVersion"
& dotnet publish $csproj -c Release -o $appStage | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'dotnet publish failed' }
Copy-Item (Join-Path $Root 'kits\Switch-Center\README-ja.md') $appStage
$appZip = Join-Path $Out "SwitchCenter-$appVersion-arm64.zip"
Compress-Archive -Path $appStage -DestinationPath $appZip -CompressionLevel Optimal

# ------------------------------------------------------------ manifest
$manifest = [ordered]@{
  release   = "drivers-$Version"
  created   = (Get-Date).ToString('yyyy-MM-ddTHH:mm:ssK')
  signer    = 'A9E1F054DFCC96F43010918892C3C30D5A522712'
  app       = [ordered]@{ version = $appVersion; file = (Split-Path $appZip -Leaf); sha256 = (Sha256 $appZip) }
  drivers   = [ordered]@{ file = (Split-Path $driversZip -Leaf); sha256 = (Sha256 $driversZip); items = $manifestDrivers }
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $Out 'manifest.json') -Encoding utf8
Remove-Item $Stage -Recurse -Force

Write-Host ''
Write-Host "Release files in ${Out}:"
Get-ChildItem $Out | ForEach-Object { Write-Host ('  {0}  {1:N1} MB' -f $_.Name, ($_.Length / 1MB)) }
if ($Publish) {
  Publish-Release $Out
} else {
  Write-Host "Create a GitHub release tagged drivers-$Version and attach these three files (or rerun with -PublishOnly)."
}
