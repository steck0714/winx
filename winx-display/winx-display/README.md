# winx-display (draft, UNBUILT, UNTESTED)

Non-3D display step for Windows on the Switch: a WDDM **display-only** driver (KMDOD).
Based on Microsoft's `video/KMDOD` sample (Windows-driver-samples, MIT) with these changes:
- `bdd_dmm.cxx`: Rotate180 / Rotate270 allowed (sample had Identity + Rotate90 only). The blt code
  (`bltfuncs.cxx`/`blthw.cxx`) already handles all four rotations.
- `Sample/winxdisplay.inf`: extra hardware ID `ROOT\WINXDISP` (arm64/arm), names set to "winx".
  Existing IDs stay: `PCI\CC_0300/0301`, `ACPI\CLS_0003&SUBCLS_0000/0001`.

Why: the panel is 720x1280 portrait. In its loader mode (mode 1) winx hands Windows that native framebuffer,
so the desktop lies on its side. Microsoft Basic Display already draws on that framebuffer (no driver needed
for a plain picture); this driver adds a way to choose landscape (Settings > Display > orientation).
No 3D / GPU acceleration, software blt only.

Build (on a Windows PC): VS 2022 + WDK, open `Sample/SampleDisplay.vcxproj` (sources already sit one level above `Sample/`, as in the
original sample), platform ARM64, Release.
Test-sign: `bcdedit /set testsigning on` on the target (or load unsigned in WinPE with `drvload` + testsigning in BCD).
Install by hand: `pnputil /add-driver winxdisplay.inf /install`, then `devcon install winxdisplay.inf ROOT\WINXDISP`.

Do this only after plain WinPE boot + BasicDisplay works (Saturday test). Nothing here touches the firmware (bl33).
