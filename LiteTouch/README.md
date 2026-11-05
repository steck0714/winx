# Switch Lite HDH-001 touchscreen driver R1

GPIO, I²C3, and STM FTM4 touchscreen support for Windows ARM64. The diagnostic image uses WinPE 22621.1. The Home firmware targets this project's Lite, its existing Stage 6 WINARM volume, and its test-signing configuration.

## Historical WinPE test

The original WinPE test is available at the [touch R1 commit](https://github.com/ruje0504-2/winx/tree/082f60d9060426bfa87e3262640be6d26859b58f). Firmware built from the current branch boots Home directly.

1. Extract the historical test package and merge the **contents of its SD folder** into the card root. In Hekate **More Configs**, select **Lite Touch Test R1**.
2. Wait for the GPIO, I2C3, and FTM4 rows. The driver reads the chip ID during startup. Expected results are Started for all three devices, Chip 3670, Stage 4, and Status 00000000.
3. Tap and release each of the three blue circles. The checker validates actual Windows PT_TOUCH down/up events, positions, and driver report counts. Photograph **TOUCH PASS**. If a circle stays gray or a tap is misplaced, capture the full screen, including the PnP problem code, Chip, Stage, Status, IRQs, Events, HID reports, and coordinates. This historical test runs from X: with a read-only SD driver; hold the power button to turn it off.

Stage 1 reads the chip ID; Stage 2 resets the controller and waits for its ready event; Stage 3 enables scanning and drains the FIFO; Stage 4 marks driver initialization complete. If IRQs stay at zero while Events/HID reports increase, polling is active. TOUCH PASS means touch input has reached the Windows application.

## Windows Home

See the [Stage 6 source notes](../docs/hdh001-home/README.md) for the current integration. The Home package contains **SD** and **Windows-Drivers** directories. Merge the contents of SD into the card root to add **Lite Windows 11 Home** in Hekate. It boots the existing Stage 6 installation and is restricted to this device's original WINARM volume. Place Windows-Drivers somewhere accessible to an administrator or the maintenance environment.

- To add the drivers offline, identify WINARM's drive letter in ARM64 maintenance WinPE, open Windows-Drivers, and run `Install-LiteTouch.cmd offline W:\`, replacing W: with the actual letter. The script checks that the target is an offline Windows installation and already trusts the Stage 6 test certificate.
- From a usable Home installation, run `Install-LiteTouch.cmd online` as administrator, then boot **Lite Windows 11 Home**. Run `touch-check.exe` after installation; the Stage 6 desktop copy is named `LiteTouchCheck.exe`. The package includes the matching public test certificate.
- During initial deployment, the same three INF packages can be injected into the offline volume after Stage 6's DISM operation. The historical R1 diagnostic package and Home use separate boot paths.

## Validation

The three driver packages passed ARM64 Windows SignTool SYS-signature and INF/SYS catalog-membership checks using the cold-boot clock, were staged with PnPUtil, and were injected with DISM. The delivered WIM was read back to check the packages, system drivers, both certificate stores, startup script, and checker. The final touch WinPE cold-booted through its private BCD into the graphical checker on a generic AArch64 VM, which correctly reported the absence of the Tegra devices. FIFO regression tests cover fast down/up events, multiple contacts, orientation boundaries, report retries, and reset timeouts.

On a physical HDH-001, touch input can activate buttons in Windows Home setup. The three-point TOUCH PASS result and coordinate validation on the desktop are still pending.

## Source and build

- The [Windows touchscreen driver](../WindowsDrivers/drivers/SwitchTouch/src/switchtouch.c) checks for chip 0x3670, resets it, and enables multitouch scanning whenever self-managed I/O starts. It submits HID reports through VHF. Down and up events in the same FIFO batch are submitted separately; temporary submission failures retain pending reports for retry. GPIO and I²C use the upstream controller implementations.
- [Firmware power initialization](../Drivers/TouchPanelDxe/LiteTouchPower.c) prepares LDO6, PJ7, I²C3, and PX1, leaving the FIFO to Windows. DSDT supplies eight separate GPIO bank interrupts, I²C3 at 0x7000c500, and the touchscreen (SWOA0502, address 0x49, pin PX1). Add `-D SWITCH_LITE_TOUCH=1` to the original read-only WinPE build flags, retaining `SWITCH_HEKATE=1`, `SWITCH_ACPI_SD_NVTSDHC=1`, `SWITCH_FB_ROTATE=2`, and `SWITCH_EXCFB=1`. Home uses the same ACPI and power modules with Stage 6's NTFS boot path and disk-identity checks.
- The [touch checker](touch-check.c) uses SetupAPI, CfgMgr32, and WM_POINTER. The [driver build script](build-drivers.sh) supports Windows ARM64 WDK 26100, KMDF 1.15, and clang-cl/lld-link. From the repository root, run `sh LiteTouch/build-drivers.sh WindowsDrivers /path/to/output`. Set WDK_INCLUDE, WDK_LIB, WDF_INCLUDE, and WDF_LIB to the matching headers and ARM64 libraries. To build the checker with mingw-w64 headers, set MINGW_SYSROOT and run [build-check.sh](build-check.sh). Run the host FIFO tests with `python3 LiteTouch/tests/test-ftm4.py`.
- Board wiring follows the [Switchroot T210B01 device tree](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/cf785c4c176499b301170d79fe57b77f365b73cd/kernel-dts/nx-platforms/tegra210b01-nx-common.dtsi#L830-L844) and [Hekate FTM4 initialization](https://github.com/CTCaer/hekate/blob/v6.5.3/bdk/input/touch.c). Windows controller and HID sources originate from [Windows-for-Switch-Drivers](https://github.com/hayato41200/Windows-for-Switch-Drivers).

