Patch chain (each applies on the previous state):
  imbushuo/NintendoSwitchPkg 09c3ade
  -> archive/patches/upstream-under-development (387 KB)
  -> archive/patches/upstream-9eba951 (400 KB)
  -> archive/patches/deliver16 (diff vs 9eba951)   -> archive/patches/deliver17-fsdrv (fsdrv; superseded by the later patch series)
  -> archive/patches/incremental-patch-series, diffs vs 9eba951, applied in this order in THIS tree:
       1. winx-bl33-latest.patch   (includes the usb-repower changes; winx-usb-repower.patch is kept for reference only)
       2. sd-write.patch
       3. acpi-select.patch  then acpi-select_v2-incremental.patch   (acpi-select_v2.patch = the same two in one patch)
       4. usb-stdehci.patch
       5. touch-fts4.patch  (made against pristine upstream; its .dsc/.fdf hunks were added by hand)
release/archive/deliver16 and release/archive/deliver17-fsdrv: prebuilt binaries from the matching patch snapshots (untested). None of the later patches are built into them.
Extra files from branch Under-development: winx-display/, bl33/ (wintest A/B), contingency/, uefi/, winpe/, TEST_PLAN.md.

archive/patches/consolidated-latest/winx-bl33.patch = the single consolidated latest patch: everything in archive/patches/incremental-patch-series applied, vs imbushuo/NintendoSwitchPkg 09c3ade
  (source files only: docs, release/, patches/, winx-display/ etc. excluded).
  Re-applied to a clean 09c3ade and compared with this tree. release/archive/edk2-stable202402-fsdrv/bl33.bin is built from exactly this state.
