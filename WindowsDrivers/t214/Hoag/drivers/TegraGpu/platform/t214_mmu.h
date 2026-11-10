/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_T214_MMU_H
#define TEGRA_GPU_T214_MMU_H
#include "t214_power.h"
enum tg_gpu_tlb_result { TG_GPU_TLB_OK, TG_GPU_TLB_ARGUMENT, TG_GPU_TLB_NOT_READY,
                         TG_GPU_TLB_NO_SPACE, TG_GPU_TLB_TIMEOUT, TG_GPU_TLB_IO_ERROR };
/* Caller owns GPU power, serializes the invalidate FIFO and has made all
 * page-table writes visible. A timeout is a failure; do not recycle pages.
 */
enum tg_gpu_tlb_result tg_gpu_tlb_invalidate(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, uint64_t root_device_address);
#endif
