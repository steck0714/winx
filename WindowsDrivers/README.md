# Windows for Switch — Drivers

This is the upstream driver snapshot bundled with WinX, with the shared HDH-001 changes applied. See the [Stage 6 build and integration notes](../docs/hdh001-home/README.md) for the Lite profile. A physical Lite has reached Windows 11 Home 23H2 OOBE and accepts touch input; desktop validation and the latest fan/clock/restart results are pending.

KMDF drivers and SwitchCenter, a tray/control-center application, for Windows on ARM (ARM64) on the Nintendo Switch (Tegra210 / Erista).

The upstream UEFI and DSDT are maintained in [Windows-for-Switch](https://github.com/hayato41200/Windows-for-Switch). These drivers bind to devices exposed by that firmware, starting with R367. Install upstream packages from release ZIPs, or build them with `release/Make-Release.ps1`.

## Upstream drivers

| Driver | Device | Required UEFI | Function |
|---|---|---|---|
| `tegra210gpio` | ACPI\SWOA0101 | R367 | GPIO controller (GpioClx) |
| `tegra210i2c` | ACPI\SWOA0201 | R370 | I²C controller (SpbCx): I2C1, I2C3, and I2C5 |
| `switchbattery` | ACPI\SWOA0301 | R371; R378 for USB-C | Battery class support and VBUS/charging transitions on USB-C attachment and removal |
| `switchfan` | ACPI\SWOA0401 | R372; R376 for brightness | Temperature-based fan control, display brightness, and backlight power-off |
| `switchtouch` | ACPI\SWOA0501 | R373 | Multitouch HID through VHF; the Lite FTM4 profile uses SWOA0502 |
| `switchpwrbtn` | ACPI\SWOA0601 | R374 | Power-button HID through VHF |
| `switchcpu` | ACPI\SWOA0701 | R375 | Erista CPU clock profiles (614–1766 MHz) and thermal limits |
| `switchrtc` | ACPI\SWOA0801 | R377 | MAX77620 RTC with a persistent Windows time offset |
| `swoadiag` | Loaded on demand | — | Read-only USB (XUSB) register diagnostics |

## Tested operating systems

- Upstream: Windows 11 ARM64 22H2 (build 22621).
- HDH-001 branch: Windows 11 Home ARM64 23H2 (22631.2861) has reached OOBE; see the Lite-specific status above.

Supply your own Windows image and license. Windows is not included in this repository.

## Upstream installation

For Lite, use the HDH-001 build notes rather than installing every Erista package below.

1. Download `SwitchDrivers-<version>.zip` and `SwitchCenter-<version>-arm64.zip` from [Releases](https://github.com/hayato41200/Windows-for-Switch-Drivers/releases).
2. Prepare test signing once: follow Switch-Stage0 to enable test-signing mode, then import the included `SwitchWOA-Test.cer` into Trusted Root Certification Authorities and Trusted Publishers on the Switch.
3. Extract the ZIP and use each Switch-* kit's installation script. Follow its README. Install GPIO (Stage1), I²C (Stage3), battery, fan, touch, power button, CPU, and RTC in that order.
4. Extract `SwitchCenter-<version>-arm64.zip` and run `SwitchCenter.exe`. Its tray menu provides CPU profiles, brightness, status, power settings, and the driver list.

## SwitchPatcher

SwitchPatcher runs on a PC with the Switch SD card inserted. It installs drivers, test-signing settings, and SwitchCenter into Windows on the card, and can redeploy an ARM64 Windows image.

- Download: `SwitchPatcher-<version>-x64.zip` from releases.
- Usage: [SwitchPatcher kit](kits/Switch-Patcher/README-ja.md) (upstream Japanese documentation).
- Design: [SwitchPatcher design notes](docs/Design-SwitchPatcher-ja.md) (Japanese).
- Upstream status: This bundled snapshot documents virtual-disk testing only; physical SD-card testing was still pending.

## Build on a PC

Requirements:

- LLVM (`clang-cl` and `lld-link`).
- WDK headers and libraries (10.0.19041 and 10.0.26100).
- A .NET SDK that can target .NET 8.
- A test-signing certificate in the certificate store, named `CN=SwitchWOA Test Driver Signing`. The private key is not included.

```powershell
.\release\Make-Release.ps1 -Version 2026.10.26
```

Build output is written to `out\<version>\`. This snapshot documents these release artifacts:

- `SwitchDrivers-<version>.zip`
- `SwitchCenter-<version>-arm64.zip`
- `manifest.json`

Create a GitHub release tagged `drivers-<version>` and attach the generated artifacts.

With GitHub CLI already authenticated (`gh auth login`), `-Publish` creates the release and uploads the files. Use `-PublishOnly` to publish files built earlier.

## Updates through SwitchCenter

See the [OTA design notes](docs/Design-DriverRepo-OTA-ja.md) (Japanese). SwitchCenter reads the latest release's `manifest.json`, compares it with installed driver versions, and checks HTTPS transport, SHA-256, and the test-signing certificate before installation.

Installation requests administrator elevation. The elevated process independently downloads the same release, repeats those checks, and installs it with `pnputil`.

In SwitchCenter v0.5.0 or later, open the Drivers tab, check for updates, review the blue rows, and install. GPIO and I²C are staged for the next boot rather than replaced while dependent drivers are running (v0.5.1 or later).

SwitchCenter can also update itself from the same tab (v0.5.1 or later). It verifies the ZIP's SHA-256, replaces its files, and restarts.

Since v0.5.2, new device drivers can be distributed without changing SwitchCenter. Each manifest entry supplies:

- `hwid`: the device to query.
- `nameJa` and `nameEn`: display names.
- `installAt`: `boot` for next-boot activation, or `live` for immediate installation.
- `minUefi`: the firmware revision to display when the device is absent.

To add a driver, add an entry to the table in `release/Make-Release.ps1`.

## Layout

| Path | Contents |
|---|---|
| `drivers/` | Driver sources and INF files |
| `kits/` | Installation scripts and documentation; binaries are distributed in release ZIPs |
| `app/SwitchCenter/` | Control center: .NET 8, WinForms, win-arm64 |
| `app/SwitchPatcher/` | PC-side patcher: .NET 8, WinForms, win-x64 |
| `app/Shared/` | Shared release loading and validation |
| `tools/` | `Build-Driver.ps1` for clang-cl/lld-link KMDF builds and test signing; public test certificate |
| `release/` | `Make-Release.ps1` |
| `docs/` | Design and porting notes |

## Credits

Thanks to Claude for assistance with the upstream GitHub repository and README.

## License

GPL-2.0, as in Windows-for-Switch. Some register sequences follow Hekate and Linux, both GPL-2.0 projects.
