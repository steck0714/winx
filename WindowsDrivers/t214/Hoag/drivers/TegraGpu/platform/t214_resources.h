/* SPDX-License-Identifier: MIT */
/* Shared by the HDH-001 ACPI table and the Windows render backend. */
#ifndef TEGRA_GPU_T214_RESOURCES_H
#define TEGRA_GPU_T214_RESOURCES_H

#define T214_GPU_ACPI_ID          "SWOA0F01"
#define T214_GPU_REG_BASE         0x57000000
#define T214_GPU_REG_SIZE         0x01000000
#define T214_GPU_BAR1_BASE        0x58000000
#define T214_GPU_BAR1_SIZE        0x01000000
#define T214_GPU_AUX_BASE         0x538F0000
#define T214_GPU_AUX_SIZE         0x00001000
/* Tegra GIC SPI 157/158, plus the 32 private interrupts for ACPI GSIs. */
#define T214_GPU_STALL_GSI        189
#define T214_GPU_NONSTALL_GSI     190
/* Optional CPU-side LIC forwarding for SPI 158. Only GPU bit 30 is owned.
 * FIR, FIR_SET/CLR, IER, IER_SET/CLR and IRQ/FIQ class, in LIC bank 4.
 */
#define T214_GPU_LIC_BASE         0x60004414
#define T214_GPU_LIC_SIZE         0x0000001C
#define T214_GPU_LIC_NONSTALL     0x40000000u

/* Narrow GPU power/clock registers, excluding the fan owner's CPU CAR
 * block. PLLP is read through that owner; PMIC access uses its SPB target.
 */
#define T214_GPU_POWER_RESOURCES(X) \
    X(0x60006280, 0x18) \
    X(0x6000667C, 0x04) \
    X(0x7000E6D4, 0x04) \
    X(0x70019970, 0x08) \
    X(0x70019AA8, 0x08)
#define T214_GPU_ID_RESOURCES(X) \
    X(0x70000804, 0x04) \
    X(0x7000F930, 0x04) \
    X(0x7000FB04, 0x04)
#define T214_GPU_EXTRA_RESOURCES(X) T214_GPU_POWER_RESOURCES(X) T214_GPU_ID_RESOURCES(X)
#define T214_GPU_RESOURCE_COUNT 11

#endif
