# WinX display-only sources

This is the existing WinX adaptation of Microsoft's KMDOD sample (MIT). It uses CPU framebuffer blits and exposes rotation, including 90/180/270 degrees. It does not implement GPU rendering acceleration. The original source layout and Microsoft license are retained.

## Build

`../build-arm64.sh` cross-builds the shared sources using ARM64 WDK headers, `displib.lib`, and the upstream native-driver linker settings. The optional `Sample/winxdisplay-lite.inf` profile matches ACPI WINX0001 and ROOT\WINXDISP. The sign-in helper source remains at `../landscape.c`; it is not installed by the current Lite Home package.

The ARM64 Windows VM started version 0.1.0.1, rotated 800x600 to 600x800 and restored it. This established VM integration only.

## Lite integration withdrawn

The physical Lite went black during display-device restart. The owner then chose to withdraw CPU-rendered rotation, and the proposed PE trial was cancelled. The active Home profile omits SWITCH_LITE_DISPLAY, removes this driver and the landscape autorun, and retains Microsoft Basic Display with the native framebuffer orientation. The upstream sources remain available for reference; the current installer does not enable them.

The earlier notes required a working plain WinPE/BasicDisplay baseline before experimenting with the display driver. That baseline had worked on Lite, but the new driver itself did not receive physical-panel acceptance.
