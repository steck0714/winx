Patch chain (each applies on the previous state):
  imbushuo/NintendoSwitchPkg 09c3ade
  -> archive/patches/1 (387 KB, = branch Under-development source)
  -> archive/patches/2 (400 KB, = branch usually 9eba951)
  -> archive/patches/3 (deliver16, diff vs 9eba951)   -> archive/patches/4 (deliver17, fsdrv; superseded by archive/patches/5 latest)
  -> archive/patches/5, diffs vs 9eba951, applied in this order in THIS tree:
       1. winx-bl33-latest.patch   (includes the usb-repower changes; winx-usb-repower.patch is kept for reference only)
       2. sd-write.patch
       3. acpi-select.patch  then acpi-select_v2-incremental.patch   (acpi-select_v2.patch = the same two in one patch)
       4. usb-stdehci.patch
       5. touch-fts4.patch  (made against pristine upstream; its .dsc/.fdf hunks were added by hand)
release/3, release/4: prebuilt binaries from archive/patches/3 and archive/patches/4 (untested). None of the later patches are built into them.
Extra files from branch Under-development: winx-display/, bl33/ (wintest A/B), contingency/, uefi/, winpe/, TEST_PLAN.md.

archive/patches/6/winx-bl33.patch = the single consolidated LATEST patch: everything of archive/patches/5 applied, vs imbushuo/NintendoSwitchPkg 09c3ade
  (same convention as archive/patches/1, archive/patches/2; source files only: docs, release/, patches/, winx-display/ etc. excluded).
  Re-applied to a clean 09c3ade and compared with this tree. release/5/bl33.bin is built from exactly this state.
