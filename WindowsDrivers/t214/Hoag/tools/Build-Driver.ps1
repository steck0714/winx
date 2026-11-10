<#
.SYNOPSIS
  Build (and test-sign) an ARM64 KMDF kernel driver with LLVM clang-cl + lld-link
  against the installed WDK. No Visual Studio needed.

.EXAMPLE
  .\Build-Driver.ps1 -Name SwoaHello -Sources ..\SwoaHello\src\hello.c -OutDir ..\SwoaHello\build
  .\Build-Driver.ps1 -Name tegra210gpio -Sources a.c,b.c -Libs msgpioclxstub.lib -OutDir ...
#>
param(
  [Parameter(Mandatory = $true)][string]$Name,
  [Parameter(Mandatory = $true)][string[]]$Sources,
  [Parameter(Mandatory = $true)][string]$OutDir,
  [string[]]$Libs = @(),
  [string[]]$Defines = @(),
  [string]$Inf,
  [string]$KmdfVersion = '1.15',
  [string[]]$ExtraIncludes = @(),   # searched after the WDK 10.0.19041 headers (e.g. km\spb\1.0)
  [switch]$NoSign,
  [switch]$Wdm                      # plain WDM driver (DriverEntry, no KMDF), e.g. an sdport miniport
)

$ErrorActionPreference = 'Stop'
$Llvm = 'C:\Program Files\LLVM\bin'
$Kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
$Wdk = '10.0.19041.0'
$Inc = "$Kits\Include\$Wdk"
$KmLib = "$Kits\Lib\$Wdk\km\arm64"
$KmdfInc = "$Kits\Include\wdf\kmdf\$KmdfVersion"
$KmdfLib = "$Kits\Lib\wdf\kmdf\arm64\$KmdfVersion"
$SignTool = "$Kits\bin\10.0.26100.0\x64\signtool.exe"
$Inf2Cat = "$Kits\bin\10.0.26100.0\x86\Inf2Cat.exe"
$CertSubject = 'CN=SwitchWOA Test Driver Signing'
$CertDir = Join-Path $PSScriptRoot 'cert'

foreach ($p in "$Llvm\clang-cl.exe", "$Llvm\lld-link.exe", "$Inc\km\wdm.h", "$KmdfInc\wdf.h", "$KmdfLib\wdfldr.lib") {
  if (-not (Test-Path $p)) { throw "Missing: $p" }
}
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path    # .NET calls below need an absolute path
$KmdfMinor = $KmdfVersion.Split('.')[1]

$cflags = @(
  '--target=aarch64-pc-windows-msvc', '/c', '/kernel', '/GS-', '/Gy', '/Zl', '/O2', '/W4', '/WX',
  '-Wno-unused-parameter', '-Wno-microsoft-anon-tag', '-Wno-ignored-pragma-intrinsic',
  '-Wno-pragma-pack', '-Wno-invalid-token-paste', '-Wno-unknown-pragmas', '-Wno-ignored-attributes', '-Wno-nonportable-include-path',
  '/D_ARM64_', '/DARM64', '/D_WIN64', '/DNTDDI_VERSION=0x0A000008', '/D_WIN32_WINNT=0x0A00', '/DWINVER=0x0A00',
  '/DKMDF_VERSION_MAJOR=1', "/DKMDF_VERSION_MINOR=$KmdfMinor", '/DPOOL_NX_OPTIN=1',
  # WDK headers as system headers: their warnings are not ours (/W4 /WX applies to our sources only).
  "/imsvc$Inc\km\crt", "/imsvc$Inc\km", "/imsvc$Inc\shared", "/imsvc$KmdfInc"
) + ($ExtraIncludes | ForEach-Object { "/imsvc$($_ -replace '^\$Kits', $Kits)" }) + ($Defines | ForEach-Object { "/D$_" })

$objs = @()
foreach ($s in @($Sources) + (Join-Path $PSScriptRoot 'kmdf_support.c')) {
  $src = (Resolve-Path $s).Path
  $obj = Join-Path $OutDir ([IO.Path]::GetFileNameWithoutExtension($src) + '.obj')
  & "$Llvm\clang-cl.exe" @cflags $src "/Fo$obj"
  if ($LASTEXITCODE -ne 0) { throw "compile failed: $src" }
  $objs += $obj
}

$sys = Join-Path $OutDir "$Name.sys"
$kmdfLibs = if ($Wdm) { @() } else { @("$KmdfLib\WdfDriverEntry.lib", "$KmdfLib\WdfLdr.lib") }
$libPaths = $kmdfLibs + @("$KmLib\ntoskrnl.lib", "$KmLib\hal.lib",
              "$KmLib\ntstrsafe.lib", "$KmLib\bufferoverflowfastfailk.lib") +($Libs | ForEach-Object { if (Test-Path $_) { $_ } else { Get-ChildItem $KmLib -Recurse -Filter $_ | Select-Object -First 1 -ExpandProperty FullName } })
# Match what MSVC link /driver produces: resident (non-paged, "P") sections,
# discardable INIT ("D"), separate .rdata, no TERMINAL_SERVER_AWARE. SwoaHello
# built without these failed to start with error 487 on the Switch.
& "$Llvm\lld-link.exe" '/nologo' '/driver' '/subsystem:native,10.0' '/machine:arm64' $(if ($Wdm) { '/entry:DriverEntry' } else { '/entry:FxDriverEntry' }) `
  '/nodefaultlib' '/release' '/opt:ref' '/opt:icf' '/tsaware:no' `
  '/section:.text,ERP' '/section:.rdata,RP' '/section:.data,RWP' '/section:.pdata,RP' '/section:INIT,ERD' `
  "/out:$sys" @objs @libPaths
if ($LASTEXITCODE -ne 0) { throw 'link failed' }
$b = [IO.File]::ReadAllBytes($sys)
if ($b[0] -ne 0x4D -or $b[1] -ne 0x5A) { throw 'output is not a PE file' }

if ($Inf) {
  Copy-Item $Inf (Join-Path $OutDir (Split-Path $Inf -Leaf)) -Force
}
if ($NoSign) { Write-Host "Built (unsigned): $sys"; return }

# Test certificate (created once; the .cer is installed on the Switch).
New-Item -ItemType Directory -Force $CertDir | Out-Null
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $CertSubject -and $_.NotAfter -gt (Get-Date) } | Select-Object -First 1
if (-not $cert) {
  $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject -CertStoreLocation Cert:\CurrentUser\My `
          -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(10)
}
Export-Certificate -Cert $cert -FilePath (Join-Path $CertDir 'SwitchWOA-Test.cer') -Force | Out-Null

& $SignTool sign /q /fd sha256 /sha1 $cert.Thumbprint /s My $sys
if ($LASTEXITCODE -ne 0) { throw 'signtool failed (sys)' }
if ($Inf) {
  & $Inf2Cat "/driver:$OutDir" '/os:10_VB_ARM64' '/uselocaltime' | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'Inf2Cat failed' }
  Get-ChildItem $OutDir -Filter *.cat | ForEach-Object {
    & $SignTool sign /q /fd sha256 /sha1 $cert.Thumbprint /s My $_.FullName
    if ($LASTEXITCODE -ne 0) { throw "signtool failed ($($_.Name))" }
  }
}
Write-Host "Built and test-signed: $sys"
Write-Host "Certificate: $(Join-Path $CertDir 'SwitchWOA-Test.cer') thumbprint=$($cert.Thumbprint)"
