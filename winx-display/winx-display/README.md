# WinX display-only driver

This is the shared Microsoft KMDOD sample with WinX's four-way rotation support. It owns the existing UEFI POST framebuffer and copies pixels in software. It does not implement GPU rendering or 3D acceleration. Microsoft sample sources retain their MIT license.

## HDH-001 profile

Build the companion firmware with `SWITCH_LITE_DISPLAY=1`. It exposes ACPI WINX0001 using Hekate's framebuffer reservation. The new `Sample/winxdisplay-lite.inf` profile binds that device; ROOT\WINXDISP is also available for VM development. Use the ACPI device on the physical Lite.

From the firmware repository root, set WDK_INCLUDE, WDK_LIB, WDK_EXTRA_LIB, CLANG_CL, and LLD_LINK, then run:

```sh
sh winx-display/build-arm64.sh /path/to/driver-output
sh winx-display/build-landscape.sh /path/to/app-output
```

The second command also needs MINGW_SYSROOT and LLVM_DLLTOOL. Copy the Lite INF as `winxdisplay.inf`, generate `winxdisplay.cat` from the final signed INF/SYS package, and sign the catalog with the certificate trusted by the target. The target must use test signing for these development packages.

The kernel build follows the upstream ARM64 native-driver linker flags: resident normal sections, discardable INIT, and no TERMINAL_SERVER_AWARE flag. The original `Sample/winxdisplay.inf` remains the general upstream profile.

## Default orientation

Install `LiteLandscape.exe` in ProgramData\SwitchLite and register it at sign-in. For a 720x1280 primary display, it requests 1280x720 with DMDO_90 through ChangeDisplaySettingsEx and saves the setting. A successful first run records DefaultLandscapeApplied under HKCU\Software\SwitchLite\Display. Later runs preserve the user's selection. Already-landscape displays are left in place.

The existing portrait touch coordinates are retained; Windows applies display rotation to input. The native boot framebuffer remains portrait. Physical orientation and touch alignment still require Lite testing.

## Validation

The ARM64 driver and helper build. `display-smoke.c` is a file-only VM integration check that reports the current display mode, rotates it, and restores it. Successful staging or compilation alone does not establish that the miniport started or that physical rotation works.

The ARM64 WinPE VM reports the WinX miniport as Started. Its display API changed 800x600 to 600x800 and restored it successfully. The physical Lite panel, default 1280x720 orientation, and touch alignment remain pending.
