# Firmware artifacts

Older prebuilt firmware is kept under `release/archive/` with descriptive directory names. The files and their SHA-256 manifests were moved without modification.

- `legacy-build-a/` and `legacy-build-b/`: earlier BL33 builds; the original folders did not document their build provenance.
- `deliver16/`: deliver16 firmware, not validated on hardware.
- `deliver17-fsdrv/`: deliver17 filesystem-driver firmware, not validated on hardware.
- `edk2-stable202402-fsdrv/`: the later filesystem-driver build described in its README; it compiles, but was not validated on Switch hardware.

These are historical artifacts. The source currently in this repository has not been rebuilt into a replacement release image yet.
