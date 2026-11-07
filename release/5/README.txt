release/5/bl33.bin : built from this tree (= patches/6/winx-bl33.patch on imbushuo/NintendoSwitchPkg 09c3ade), EDK2 edk2-stable202402,
  aarch64-linux-gnu-gcc 13.3, GCC5 RELEASE. Command: Tools/build-latest.sh
  = SWITCH_HEKATE USB FB_ROTATE=2 EXT RTC NET PERF XUSB WIFIBT EXCFB + FSDRV + BIGFD + ACPI_XHCI + ACPI_EHCI.
Contains: latest SD driver (diag + write gated to FAT partitions), USB power watch, EHC0/XHC0 ACPI selection (UBSL),
  touch panel driver (TouchPanelDxe), NTFS/exFAT/ext/XFS/Btrfs/F2FS/EROFS read-only FS drivers, xusbinit resident daemon.
Not enabled: SWITCH_USB_STDEHCI (A/B test only, breaks the UEFI keyboard), SWITCH_ACPI_EHCI_TXFILL (value unconfirmed), SWITCH_ACPI_EMMC.
Compiles and links; NOT tested on hardware. FVMAIN is 99% full (8 bytes free): the next feature needs a size cut.
