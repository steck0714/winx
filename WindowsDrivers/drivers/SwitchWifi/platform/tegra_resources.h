/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared C/ASL resource contract. Tegra210 and T210B01 use the same host
 * apertures and legacy interrupt; only PHY/power preparation differs. */
#ifndef SWIFI_TEGRA_RESOURCES_H
#define SWIFI_TEGRA_RESOURCES_H

#define SWIFI_ACPI_HID       "SWOA0D01"
#define SWIFI_HOST_BASE      0x01000000
#define SWIFI_HOST_LENGTH    0x00004000
#define SWIFI_AFI_OFFSET     0x00003800
#define SWIFI_AFI_LENGTH     0x00000800
#define SWIFI_ROOT_OFFSET    0x00001000
#define SWIFI_CONFIG_BASE    0x02000000
#define SWIFI_CONFIG_LENGTH  0x00001000
#define SWIFI_APERTURE_BASE  0x13000000
#define SWIFI_APERTURE_SIZE  0x0D000000
/* GIC SPI 98 + 32, level high. MSI SPI 99 is not used in this profile. */
#define SWIFI_LEGACY_GSI     0x00000082
/* Legacy controller bank 3 forwards SPI 98 (bit 2) to the CPU/GIC. */
#define SWIFI_LIC_BASE       0x60004300
#define SWIFI_LIC_MASK       0x00000004

#endif
