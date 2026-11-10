/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_PMIC_H
#define TEGRA_GPU_PMIC_H
#include <stdint.h>
/* Kernel-only bridge to SwitchFan's already-open MAX77812 SPB target.
 * No second driver programs I2C5 registers or opens the CPU's connection.
 */
#define TG_GPU_IOCTL_PMIC 0x00222410u
#define TG_GPU_PMIC_QUERY_ADDRESS 0x100u
#define TG_GPU_PMIC_QUERY_PLLP    0x101u
#define TG_GPU_PMIC_QUERY_TEMP    0x102u
struct tg_gpu_pmic_request {
    uint32_t address;
    uint32_t reg;
    uint32_t write;
    uint32_t value;
};
#endif
