# Stage 6 — Switch Lite Windows installation

This guide covers the full HDH-001 / T214 installation and subsequent driver updates. The Hekate entry is **Windows 11 Lite**. Windows 11 ARM64 23H2 (22631.2861) has booted on the test Lite; the original installation used Home and the current installation uses a trimmed Pro image. The entry name does not select a Windows edition.

This is a source-based development port. A first installation requires firmware and a boot-storage driver built for the target card, signed driver packages, private board firmware, and your own Windows image. The repository does not contain a complete Windows installation image. Check [porting status](PORTING.md) for hardware results and remaining limitations.

- [Prepare the card and matching builds](#1-prepare-the-card-and-matching-builds)
- [Assemble the complete driver kit](#2-assemble-the-complete-driver-kit)
- [Apply Windows and install drivers](#3-apply-windows-and-install-drivers)
- [Configure Windows boot and Hekate](#4-configure-windows-boot-and-hekate)
- [First boot and everyday settings](#5-first-boot-and-everyday-settings)
- [Update an existing installation](#6-update-an-existing-installation)
- [Troubleshooting](#troubleshooting)
- [Firmware and driver build reference](#firmware-and-driver-build-reference)
- [Build BL31](#build-bl31)
- [Build and package BL33](#build-and-package-bl33)

<a id="firmware-build"></a>

## 1. Prepare the card and matching builds

You need a Switch Lite that already boots Hekate, a card reader, a backup of the existing card, and an ARM64 Windows maintenance environment with DISM, BCDBoot and BCDEdit. Native ARM64 WinPE is used for offline servicing. A Mac can prepare source and copy FAT32 boot files; its normal NTFS mount cannot install Windows drivers.

The Windows drivers are maintained in the [driver repository](https://github.com/ruje0504-2/Windows-for-Switch-Drivers). `WindowsDrivers` is a Git submodule pinned to the revision used by this firmware checkout. Create a build workspace and initialize the submodule:

```sh
mkdir switch-lite-build
cd switch-lite-build
git clone --branch hdh001-hekate-sd --recurse-submodules \
  https://github.com/ruje0504-2/winx.git NintendoSwitchPkg
```

The shell examples below start in `switch-lite-build`, the directory containing `NintendoSwitchPkg`, unless a command explicitly changes directory. Use a workspace path without spaces. For an existing checkout, place it at `NintendoSwitchPkg` in this layout and run `git -C NintendoSwitchPkg submodule update --init WindowsDrivers`. Run that command again after pulling a new WinX revision to select its recorded driver revision. The Lite build selects `drivers/SwitchLiteTouch`, which matches SWOA0502; the original SwitchTouch driver remains available for the upstream hardware.

The current target generator supports this specific layout:

| Item | Required layout |
|---|---|
| Sector size | 512 bytes |
| Partition table | Hybrid MBR and GPT; eleven 128-byte GPT entries starting at LBA 2 |
| Windows partition | GPT slot 11, Microsoft Basic Data, named `WINARM`, exactly 80 GiB |
| Hybrid MBR | Slot 4 describes that same NTFS partition |
| NTFS | 512-byte sectors, 4096-byte clusters; matching partition bounds and a nonzero serial |

Prepare the dedicated Windows space without overwriting existing FAT32, Ubuntu, Android or emuMMC data. Resizing an existing filesystem must precede shrinking its partition. **The generator does not repartition or format a card.** If your layout differs, the target-generation and storage code must be adapted before using this port. Do not copy another card's generated header or boot-storage binary.

After preparing the layout, save a complete card image as `card-backup.img` in the workspace. Generate its private build inputs:

```sh
mkdir -p NintendoSwitchPkg/docs/hdh001-home/generated
python3 NintendoSwitchPkg/WindowsDrivers/drivers/Tegra210Sdhc/winarm/configure-target.py \
  card-backup.img NintendoSwitchPkg/docs/hdh001-home/generated/target
cp NintendoSwitchPkg/docs/hdh001-home/generated/target/winarm-target.h \
  NintendoSwitchPkg/WindowsDrivers/drivers/Tegra210Sdhc/src/winarm-target.h
```

The output directory `generated/target` must not already exist. Build **both BL33 and nvtsdhc from this same header** using the [build reference](#firmware-and-driver-build-reference). BL33 includes it through `Include/LiteSdTarget.h`; a separate firmware copy is not needed. Keep the generated card identity private.

By default, firmware and Windows present only WINARM as an active MBR partition. Hekate still uses the physical FAT32 partition. `--expose-fat32` selects the two-volume profile currently used by the owner. The latest comparison did not establish a clear responsiveness difference between the two views. Changing the presented view does not modify the physical partition table.

## 2. Assemble the complete driver kit

Use the matching ARM64 firmware and signed drivers. Each driver directory must contain its final `.inf`, `.sys` and `.cat`; signing the SYS must precede catalog generation. The installation scripts expect these directory names:

```text
Stage6Kit/
  Install-LiteTouch.cmd
  Install-LiteSystem.cmd
  Stage6-Test.cer
  nvtsdhc/
  tegra210gpio/
  tegra210i2c/
  switchtouch/
  switchfan/
  switchpwrbtn/
  switchbattery/
  switchrtc/
  switchaudio/
  switchbt/
  switchwifi/
  switchsio/
  tegraxusb/
  WifiPrepare.efi
  UsbPrepare.efi
  LiteTouchCheck.exe
  LitePerformanceCheck.exe
  Firmware/
    BCM4356A3.hcd
    brcmfmac4356A3-pcie.bin
    brcmfmac4356A3-pcie.txt
    tegra210b01_xusb_firmware
```

Copy the installers from [LiteTouch](../../LiteTouch/Install-LiteTouch.cmd) and [this directory](Install-LiteSystem.cmd). The kit covers storage, GPIO/I2C/touch, fan/brightness, power and volume buttons, battery/RTC, audio, Bluetooth, Wi-Fi, built-in controls and USB-C host support. Volume buttons use the inbox HID interrupt driver. Windows provides the basic display, Bluetooth class, and USB host class drivers.

Supply the wireless firmware and the target board's calibration from its working Switchroot installation. Bluetooth uses the Switch BCM4356A3 HCD patch. USB uses the **T210B01** Falcon image, not the Erista image. These private binaries are not committed here. See [Wi-Fi packaging](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/drivers/SwitchWifi/README.md), [Bluetooth notes](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/docs/hdh001/README.md#bluetooth-preparation) and [USB firmware extraction](PORTING.md#usb-c-host-on-hdh-001).

The system installer installs the five base system/audio packages and includes Bluetooth, Wi-Fi, SIO and USB when their package directories are present. GPIO/I2C/touch and nvtsdhc are installed separately in the next step. A kit missing an optional directory will not install that function. USB-C remains pending hardware acceptance. The active Lite configuration uses Microsoft Basic Display and CPU software rendering; exclude `winxdisplay` and `LiteLandscape` from this kit.

## 3. Apply Windows and install drivers

For these **Command Prompt** examples:

- `W:` is the prepared, empty WINARM volume for a first installation.
- `D:` holds your ARM64 Windows `sources\install.wim`.
- `K:\Stage6Kit` holds the complete kit above.

Replace these letters with the ones actually assigned in the maintenance environment. Identify WINARM by its label, 80 GiB size and saved card identity. Existing installations should use [the update procedure](#6-update-an-existing-installation); applying an image over an existing Windows directory is not an update.

Use maintenance WinPE with the matching nvtsdhc/firmware partition view. A host that sees the physical GPT instead can encode different partition identifiers in BCD. The Windows volume and boot files must ultimately resolve in the WINARM-only MBR view used on the Lite.

Inspect the available editions, then apply the ARM64 edition that matches your license. Replace `N` with its index:

```bat
dism /Get-WimInfo /WimFile:D:\sources\install.wim
dism /Apply-Image /ImageFile:D:\sources\install.wim /Index:N /ApplyDir:W:\
```

Keep a WIM larger than 4 GiB on NTFS or host storage rather than the FAT32 boot volume. [Microsoft image deployment reference](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/capture-and-apply-windows-using-a-single-wim?view=windows-11)

### Trust the signing certificate in the target image

The supplied installers check for the Stage 6 certificate thumbprint `99C510FC6160C2294603EAD48DA8831210BBC56D` in the target's machine Root and TrustedPublisher stores. Use the public certificate that signed the packages. If you build with your own certificate, update that check in both installers to its thumbprint; the private signing key stays on your build machine.

In maintenance WinPE, import that public certificate into its temporary machine stores, then copy **only its certificate entry** into the offline Windows SOFTWARE hive. Set `thumbprint` to the certificate actually used:

```bat
set "thumbprint=99C510FC6160C2294603EAD48DA8831210BBC56D"
certutil -addstore -f Root K:\Stage6Kit\Stage6-Test.cer
certutil -addstore -f TrustedPublisher K:\Stage6Kit\Stage6-Test.cer
reg load HKLM\Stage6Software W:\Windows\System32\config\SOFTWARE
reg copy "HKLM\SOFTWARE\Microsoft\SystemCertificates\Root\Certificates\%thumbprint%" "HKLM\Stage6Software\Microsoft\SystemCertificates\Root\Certificates\%thumbprint%" /s /f
reg copy "HKLM\SOFTWARE\Microsoft\SystemCertificates\TrustedPublisher\Certificates\%thumbprint%" "HKLM\Stage6Software\Microsoft\SystemCertificates\TrustedPublisher\Certificates\%thumbprint%" /s /f
reg unload HKLM\Stage6Software
```

Stop if a command fails; unload the offline hive before retrying. Some minimal PE images omit CertUtil. Use the ARM64 SDK `certmgr.exe -add -c Stage6-Test.cer -s -r localMachine Root` and the corresponding `TrustedPublisher` command in that case. Importing into PE alone does not establish trust in `W:\Windows`. [Microsoft test-signing requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/test-signing)

### Install the complete kit

Run these from an elevated maintenance prompt, checking each result before continuing:

```bat
cd /d K:\Stage6Kit
dism /Image:W:\ /Add-Driver /Driver:nvtsdhc\nvtsdhc.inf
call Install-LiteTouch.cmd offline W:\
call Install-LiteSystem.cmd offline W:\
copy /y LiteTouchCheck.exe W:\Users\Public\Desktop\LiteTouchCheck.exe
dism /Image:W:\ /Get-Drivers /Format:Table
```

The system installer copies the wireless/USB support files to their runtime locations and copies `LitePerformanceCheck.exe` when supplied. Success is exit code `0`; the system installer also accepts `3010` as installed with a restart required. A staged package still needs to bind successfully on the Lite.

Check the offline SYSTEM hive's `Select` key and use its selected control set. For a fresh image selecting `1`, the boot-storage settings are:

```bat
reg load HKLM\Stage6System W:\Windows\System32\config\SYSTEM
reg query HKLM\Stage6System\Select
reg add HKLM\Stage6System\ControlSet001\Services\nvtsdhc /v Start /t REG_DWORD /d 0 /f
reg add HKLM\Stage6System\ControlSet001\Services\sdbus /v Start /t REG_DWORD /d 0 /f
reg add HKLM\Stage6System\ControlSet001\Services\sdstor /v Start /t REG_DWORD /d 0 /f
reg unload HKLM\Stage6System
```

If `Select` points elsewhere, substitute that control set. Preserve Windows Setup's initial state; no historical setup-state reset is part of a fresh install.

## 4. Configure Windows boot and Hekate

Create the ARM64 UEFI boot files **on WINARM itself**. This port's read-only NTFS UEFI driver loads `\EFI\Microsoft\Boot\bootmgfw.efi` from that volume. Run BCDBoot in the matching partition view described above:

```bat
W:\Windows\System32\bcdboot W:\Windows /s W: /f UEFI
bcdedit /store W:\EFI\Microsoft\Boot\BCD /set {default} testsigning on
bcdedit /store W:\EFI\Microsoft\Boot\BCD /enum {default}
```

The loader must use `\Windows\System32\winload.efi`, with `device` and `osdevice` resolving to WINARM and `testsigning` enabled. Keep Windows boot files on WINARM when FAT32 is hidden. [BCDBoot reference](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/bcdboot-command-line-options-techref-di?view=windows-11)

After servicing finishes, unload any offline registry hives and dismount WINARM. Connect the card to the build computer and back up its current Windows boot files before replacing them. The [firmware build steps](#build-and-package-bl33) produce an `out/sd-card` directory. Copy its contents to the **root of the FAT32 volume**, merging directories, so the files are located at:

```text
bootloader/ini/woa-lite-home.ini
switchroot/woa-lite-home/bl31.bin
switchroot/woa-lite-home/bl33.bin
```

`bl31.bin` comes from the pinned LineageOS ATF source plus this repository's T214 power patches; `bl33.bin` comes from the EDK2 build and BL33 packer below. Use those outputs for this Lite profile. The intermediate `TEGRA210_EFI.fd` still needs its entry stub and must not simply be renamed to `bl33.bin`. Keep one Windows entry, with this baseline configuration:

```ini
[Windows 11 Lite]
l4t=1
boot_prefixes=switchroot/woa-lite-home/
sld_type=1
ram_oc=1600000
```

Merge it with existing Hekate configuration without removing entries for other operating systems. Remove obsolete duplicate Windows test entries. Normal startup must not include temporary `wifi_check` or power-check options. The normal Wi-Fi preparation runs automatically from WINARM before Windows boots. Safely eject the card after copying, then insert it into the fully powered-off Lite.

## 5. First boot and everyday settings

Select **Windows 11 Lite** in Hekate and finish Windows first-run setup. If Wi-Fi is not yet available, the tested 22631 image supports `OOBE\BYPASSNRO` from the setup command prompt (`Shift+F10`); after the requested restart it offers offline setup. This requires keyboard access and a build retaining that script. Completing setup offline does not disable the Wi-Fi adapter.

At the desktop:

- Connect through Windows network settings. The installed Wi-Fi driver must expose a station adapter before a Wi-Fi option appears. Windows can save the network credentials normally.
- Run `osk.exe` for the on-screen keyboard. The Plus mapping invokes it with `Ctrl+Win+O`; the system image must still contain the keyboard and its shortcut support. Full touch-keyboard auto-popup also depends on the Windows components retained by the image.
- Run `LiteTouchCheck.exe` and tap/release its three circles. `TOUCH PASS` confirms those input events. Use `LitePerformanceCheck.exe` for brightness, temperatures and actual CPU/EMC readings.
- In Advanced system settings → Performance → Advanced → Virtual memory, select the Windows volume and set **512 MB initial / 2048 MB maximum** if using the agreed paging configuration. The firmware must include `SWITCH_LITE_BOOT_SD=1` so Windows sees a fixed boot slot.
- Use the built-in controls according to the [mapping table](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/drivers/SwitchSensor/README.md). Capture toggles the desktop mapping; gamepad compatibility is described there.

For the optional clock configuration, use [woa-lite-home-performance.ini](woa-lite-home-performance.ini) in place of the baseline INI. It requests CPU `1996800` kHz and RAM `2100000` kHz, with automatic fan control. The test Lite read back CPU 1996.8 MHz and RAM 1866 MHz after BPMP rounding. Check actual readings rather than treating a requested rate as an achieved rate. GPU overclocking is not enabled. Setting `cpu_oc=0` and `ram_oc=1600000` returns to the normal boot request.

Speaker playback, Wi-Fi, Bluetooth and brightness have hardware results in [porting status](PORTING.md). Normal Windows shutdown and native restart have owner-confirmed physical passes on the tested Lite. Use SwitchBattery **1.0.0.8** or later and the normal R6 firmware, which uses PMC reset for restart and disables diagnostic fan pulses. Wait for complete power removal before taking out the card. USB-C, headphone switching and other unaccepted functions remain listed in porting status. Keep extra test tones disabled.

## 6. Update an existing installation

Back up the current boot files and Windows installation, then assemble the complete matching kit from step 2. Keep the same card-specific storage header and matching partition view in BL33 and the SD miniport. Driver updates do not require formatting, reapplying Windows or resetting OOBE.

From an elevated Command Prompt **inside the installed Windows**, run:

```bat
cd /d C:\Stage6Kit
call Install-LiteTouch.cmd online
call Install-LiteSystem.cmd online
```

For an installation that cannot boot, use ARM64 maintenance WinPE and the actual assigned Windows volume:

```bat
cd /d K:\Stage6Kit
call Install-LiteTouch.cmd offline W:\
call Install-LiteSystem.cmd offline W:\
```

These commands cover the touch/controller and complete system kit. They retain the existing certificate requirement. An nvtsdhc update is a separate boot-critical operation: stage its signed INF offline with DISM and deploy the matching BL33 together, before the next Lite boot. If only unrelated device drivers changed, keep the working storage pair.

When a release requires new firmware, replace the matching files under `switchroot/woa-lite-home/` while the card is connected to the maintenance computer. Keep the existing Hekate name and settings. Check the installed driver version in Device Manager or `pnputil /enum-drivers` after boot; successful DISM staging alone does not prove the active device selected the new driver.

## Troubleshooting

| Symptom | Check |
|---|---|
| `WINARM filesystem missing` or `Volume Corrupt` before Windows | Card identity/header match, NTFS state, the specified read-only NTFS UEFI driver, and files under `WINARM\EFI\Microsoft\Boot`. |
| `INACCESSIBLE_BOOT_DEVICE` / stop 0x7B | BL33 and nvtsdhc must use the same partition view and header. Check boot-start storage services, test-signing and BCD's target. |
| Setup cannot configure Windows or reports an unexpected restart | Preserve `Windows\Panther`, `Windows\Panther\UnattendGC` and the SYSTEM hive before diagnosing the specific failed phase. Old per-machine registry workarounds are not installation steps. |
| No Wi-Fi option | Matching runtime BL33, `SwitchWOA\Wifi\WifiPrepare.efi`, complete board firmware/NVRAM pair, active `switchwifi` binding and the retained Windows WLAN components. [Diagnostics](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/drivers/SwitchWifi/README.md#on-device-status) |
| No Bluetooth toggle | Retained Windows Bluetooth components, BTH0 enumeration, signed `switchbt` binding and the BCM4356A3 patch. Use the [Bluetooth status tool](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/kits/Switch-Bluetooth/Bluetooth.ps1) before changing boot markers. |
| Temporary pagefile warning | Fixed SD slot ACPI profile, requested paging size and enough free space on WINARM. Inspect actual pagefile creation. |
| Fan keeps running after the screen turns black | Shutdown has not completed. Follow the current [power notes](power-performance.md); pre-Windows power-off success does not establish Windows shutdown success. |

## Firmware and driver build reference

### What BL31 and BL33 contain

The boot chain is **Hekate → BL31 → BL33 → Windows Boot Manager → Windows**.

| File | Source and role |
|---|---|
| `bl31.bin` | LineageOS Nintendo ARM Trusted Firmware at `85081cab2050051ea6e93206ef0f80bc0676e613`, with this repository's four ATF patches. Runs at EL3 and provides PSCI, including Lite shutdown and native restart. |
| `bl33.bin` | This WinX branch built with EDK2 `edk2-stable202402`, prefixed with a 4 KiB Hekate entry stub. Provides UEFI, ACPI, display and the card-specific WINARM boot view. |

Use the workspace created in step 1. The build steps add these directories:

```text
switch-lite-build/
  NintendoSwitchPkg/       WinX and its WindowsDrivers submodule
  card-backup.img          private input used in step 1
  atf/                    pinned BL31 source
  edk2/                   pinned BL33 build environment
  out/firmware/           final BL31/BL33 and entry-stub intermediates
  out/sd-card/            FAT32 files ready to copy
  Stage6Kit/              Windows driver kit, including UEFI helper apps
```

### Host dependencies

Use **Bash** for the following commands, including `edksetup.sh`. On macOS, start `bash` in the workspace if the terminal uses another shell. Keep the toolchain exports in that same shell for both firmware builds.

On **macOS**, install Xcode Command Line Tools if missing (`xcode-select --install`), [Homebrew](https://brew.sh/) and [devkitPro pacman](https://devkitpro.org/wiki/devkitPro_pacman). Load the devkitPro environment supplied by its installer so `DEVKITPRO` is set, then:

```sh
brew install acpica nasm python@3.11
sudo dkp-pacman -S devkitA64
export PATH="${DEVKITPRO}/devkitA64/bin:$PATH"
export PYTHON_COMMAND="$(brew --prefix python@3.11)/bin/python3.11"
export GCC5_AARCH64_PREFIX=aarch64-none-elf-
```

The Lite builds used devkitA64 GCC 15.1.0. An existing installation with these commands on `PATH` can be reused.

On **Ubuntu 22.04**, including a WSL2 build environment, use the distribution cross compiler:

```sh
sudo apt update
sudo apt install git build-essential python3 uuid-dev acpica-tools nasm curl \
  gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu
export PYTHON_COMMAND=python3
export GCC5_AARCH64_PREFIX=aarch64-linux-gnu-
```

Both routes need GNU make and the selected prefix's `gcc`, `gcc-ar`, `as`, `ld` and `objcopy`. Check the compiler and ACPI compiler before continuing:

```sh
"${GCC5_AARCH64_PREFIX}gcc" --version
iasl -v
```

`GCC5` is EDK2's toolchain profile name; it does not require GCC version 5. Other compiler versions can produce different binary hashes.

### Build BL31

From the workspace root, fetch the [LineageOS Nintendo ATF source](https://github.com/LineageOS/android_hardware_nintendo_arm-trusted-firmware), select the exact revision and invoke this repository's builder:

```sh
git clone --single-branch --branch lineage-22.2 \
  https://github.com/LineageOS/android_hardware_nintendo_arm-trusted-firmware.git atf
git -C atf checkout 85081cab2050051ea6e93206ef0f80bc0676e613
"$PYTHON_COMMAND" NintendoSwitchPkg/Tools/build-t214-atf.py \
  atf out/firmware --cross-prefix "$GCC5_AARCH64_PREFIX"
```

The builder applies patches `0001` through `0006` from `patches/atf` automatically and writes **`out/firmware/bl31.bin`**. Its default is native PMC/Hekate restart with diagnostic fan pulses disabled. Patches `0005` and `0006` only change behavior with the separate development option `--sc7`; omit that option for the normal build while complete Windows/device context restoration remains unfinished. The builder checks the BL31 entry address against Hekate's `0xFFF00000` load address. `TARGET_SOC=t210` is the upstream shared platform name; the added T214 behavior is selected by the B01 chip check.

For later rebuilds, reuse the patched `atf` checkout and rerun the Python command. The builder recognizes its already-applied patches. The [ATF reference](../../patches/atf/README.md) explains the NX build parameters and optional diagnostic flags.

### Build and package BL33

**1. Prepare EDK2 once.** From the workspace root:

```sh
git clone --branch edk2-stable202402 --depth 1 \
  https://github.com/tianocore/edk2.git edk2
git -C edk2 submodule update --init --recursive
git -C edk2 apply ../NintendoSwitchPkg/patches/edk2/0001-XhciDxe-write-combined-DMA-buffers.patch
```

This patch is applied to EDK2, not to `NintendoSwitchPkg`. Apply it only once to a fresh checkout; retain the patched tree for later builds. The historical numbered WinX patch bundles are already integrated into this branch.

**2. Supply the NTFS UEFI driver and card header.** Download the ARM64 read-only binary from [NTFS-3G UEFI 1.9](https://github.com/pbatard/ntfs-3g/releases/tag/1.9):

```sh
curl -fL -o NintendoSwitchPkg/LiteSdTest/ntfs_aa64_ro.efi \
  https://github.com/pbatard/ntfs-3g/releases/download/1.9/ntfs_aa64_ro.efi
"$PYTHON_COMMAND" - <<'PY'
from pathlib import Path
import hashlib
p = Path("NintendoSwitchPkg/LiteSdTest/ntfs_aa64_ro.efi")
assert hashlib.sha256(p.read_bytes()).hexdigest() == "c04d008d79b4add54a2a770fb904b0df89ae7b1ea63d2d7e5d273145f4106a45"
PY
test -f NintendoSwitchPkg/WindowsDrivers/drivers/Tegra210Sdhc/src/winarm-target.h
```

The header must be the one generated for this card in step 1. It is shared with the Windows nvtsdhc build. The external NTFS binary and card header are deliberately excluded from Git.

**3. Build and load the host tools.** Keep the dependency exports above, then run:

```sh
export WORKSPACE="$PWD/edk2"
export PACKAGES_PATH="$PWD:$WORKSPACE"
export EDK_TOOLS_PATH="$WORKSPACE/BaseTools"
make -C edk2/BaseTools -j8
cd edk2
. edksetup.sh
cd ..
```

The paths are derived from the current workspace. Repeat these exports and source `edksetup.sh` when opening a new shell; BaseTools only needs rebuilding when its source or host tools change.

**4. Compile the Lite profile.** Run from the workspace root, with the same environment:

```sh
build -a AARCH64 -t GCC5 -b RELEASE -n 8 -p NintendoSwitchPkg/NintendoSwitch.dsc \
  -D SWITCH_HEKATE=1 -D SWITCH_FB_ROTATE=2 -D SWITCH_EXCFB=1 \
  -D SWITCH_ACPI_SD_NVTSDHC=1 -D SWITCH_LITE_TOUCH=1 \
  -D SWITCH_LITE_POWER=1 -D SWITCH_LITE_FAN=1 -D SWITCH_FAN_DUTY=76 \
  -D SWITCH_LITE_RESET=1 -D SWITCH_LITE_PERFORMANCE=1 \
  -D SWITCH_LITE_SYSTEM=1 -D SWITCH_LITE_AUDIO=1 \
  -D SWITCH_LITE_BLUETOOTH=1 -D SWITCH_LITE_BACKLIGHT=1 \
  -D SWITCH_LITE_BOOT_SD=1 -D SWITCH_WIFI_RUNTIME=1 \
  -D SWITCH_LITE_XUSB=1 -D SWITCH_LITE_SIO=1
```

For the in-progress sleep/wake candidate, also pass `-D SWITCH_LITE_SLEEP=1`
and stage the matching `switchpwrbtn` package. That option adds the PMIC wake
interrupt, independently of the processor power backend. Do not install
`tegra214pep` in a normal deployment: its boot-start configuration caused a cold-boot
`0xc000000f` failure. The experimental C7/CC6 backend requires a separate explicit
`SWITCH_LITE_PROCESSOR_IDLE=1` development build and is not boot-validated.
Keep the card header identical to the selected hidden
or FAT32-visible SD miniport. See [current work and acceptance](PORTING.md).

S4 hibernation is not supported; the experimental implementation has been
removed. Direct retained-memory sleep and wake remain under development.

This selects the Lite boot/storage, touch, power, audio, Bluetooth, Wi-Fi preparation, SIO and B01 USB profile. It uses Microsoft Basic Display in Windows; the flags do not enable GPU acceleration. The generic `Tools/build.sh` / `build-woa.sh` defaults select a different profile, so use the explicit command above for this guide.

The output is **`edk2/Build/NintendoSwitch-AARCH64/RELEASE_GCC5/FV/TEGRA210_EFI.fd`**, with `FVMAIN_COMPACT.Fv.map` in the same directory. Stop on a build error; continue with that build's matching FD and map only.

**5. Assemble the entry stub and package BL33.** These commands use the selected compiler prefix on both macOS and Linux, rather than relying on the packer's default Linux tool names:

```sh
mkdir -p out/firmware
"${GCC5_AARCH64_PREFIX}as" -o out/firmware/bl33_stub.o \
  NintendoSwitchPkg/Tools/Bl33/bl33_stub.S
"${GCC5_AARCH64_PREFIX}ld" -Ttext=0xAA000000 \
  -o out/firmware/bl33_stub.elf out/firmware/bl33_stub.o
"${GCC5_AARCH64_PREFIX}objcopy" -O binary \
  out/firmware/bl33_stub.elf out/firmware/bl33_stub.bin
"$PYTHON_COMMAND" NintendoSwitchPkg/Tools/make-bl33.py \
  --stub out/firmware/bl33_stub.bin \
  --fd edk2/Build/NintendoSwitch-AARCH64/RELEASE_GCC5/FV/TEGRA210_EFI.fd \
  --map edk2/Build/NintendoSwitch-AARCH64/RELEASE_GCC5/FV/FVMAIN_COMPACT.Fv.map \
  -o out/firmware/bl33.bin
```

The packer checks the 4096-byte stub, Hekate entry branch, FD link address and image size before writing **`out/firmware/bl33.bin`**. These are static build checks, not a hardware boot result.

**6. Collect the installation files.** From the same workspace:

```sh
mkdir -p out/sd-card/switchroot/woa-lite-home out/sd-card/bootloader/ini Stage6Kit
cp out/firmware/bl31.bin out/firmware/bl33.bin out/sd-card/switchroot/woa-lite-home/
cp NintendoSwitchPkg/docs/hdh001-home/woa-lite-home.ini out/sd-card/bootloader/ini/
cp edk2/Build/NintendoSwitch-AARCH64/RELEASE_GCC5/AARCH64/WifiPrepare.efi Stage6Kit/
cp edk2/Build/NintendoSwitch-AARCH64/RELEASE_GCC5/AARCH64/UsbPrepare.efi Stage6Kit/
```

`out/sd-card` is the FAT32 copy tree used in step 4. Keep the UEFI helper apps in `Stage6Kit`; `Install-LiteSystem.cmd` copies them to their WINARM runtime locations alongside the private board firmware. Complete that kit with the Windows driver packages from step 2. An existing installation should retain its Hekate clock settings when merging the new boot files.

### Build the Windows drivers

Build Windows packages with LLVM `clang-cl` / `lld-link` and ARM64 WDK/SDK headers and libraries. Configure `WDK_INCLUDE`, `WDK_LIB`, `WDF_INCLUDE`, `WDF_LIB` and, where required, `WDK_EXTRA_LIB` according to the [driver build guide](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/docs/hdh001/README.md).

| Packages | Build entry |
|---|---|
| nvtsdhc | [build-miniport.sh](../hdh001-lab/build-miniport.sh), `winarm` profile and `winarm/nvtsdhc.inf` |
| GPIO, I2C, touch, power button, fan, battery, RTC, Bluetooth, SIO, USB mailbox | [LiteTouch/build-drivers.sh](../../LiteTouch/build-drivers.sh), with the `WindowsDrivers` tree and an output directory; pair each output with its matching source INF/CDF |
| Audio | [SwitchAudio/build-arm64.sh](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/drivers/SwitchAudio/build-arm64.sh) |
| Wi-Fi | [SwitchWifi build and packaging](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/hdh001-winarm-bounded-write/drivers/SwitchWifi/README.md#build-and-test) using KMDF 1.33 / NetAdapterCx 2.2 / WiFiCx 1.0 |
| Desktop checks | [LiteTouch/build-check.sh](../../LiteTouch/build-check.sh) and [LitePerformanceCheck/build.sh](../../Tools/LitePerformanceCheck/build.sh) |

Sign every final SYS, generate the CAT from the exact INF/SYS bytes with the package's CDF and MakeCat, then sign the CAT. Keep the public certificate with the kit. Build success, package installation and physical acceptance are separate results; consult the linked device notes for the current evidence. Earlier debugging history remains in Git.
