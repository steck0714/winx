# XUSB firmware (not included)

`NintendoSwitch.fdf` embeds the Tegra210 XUSB (USB host) firmware into the UEFI
image so USB works without an SD card. The binary is NVIDIA's and is not stored
in this repository.

1. Get `nvidia/tegra210/xusb.bin` from
   [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git),
   or `tegra21x_xusb_firmware` from an L4T / Switchroot image.
2. Save it here as `Firmware/tegra21x_xusb_firmware`. The file is 124,416 bytes.
   - The build that was tested used SHA-256 `81aa8787…5d40`.
3. Build as described in the top-level README.

The file name is listed in `.gitignore`. Follow the license that ships with the
firmware if you redistribute a built image.
