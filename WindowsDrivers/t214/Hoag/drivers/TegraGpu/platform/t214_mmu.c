/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2014-2018 NVIDIA CORPORATION. All rights reserved.
 * Copyright 2026 ruje0504-2
 * Switchroot nvgpu 1ae0167d common/fb/fb_gm20b.c; see NOTICE.
 */
#include "t214_mmu.h"
#include "../core/gm20b_mmu.h"

enum tg_gpu_tlb_result tg_gpu_tlb_invalidate(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, uint64_t root)
{
    uint32_t i, status;
    if (!io || !io->read32 || !io->write32 || !io->delay_us || !power ||
        root >= TG_GPU_VA_LIMIT || (root & 0xfff)) return TG_GPU_TLB_ARGUMENT;
    if (!power->owned || !power->ready) return TG_GPU_TLB_NOT_READY;
    for (i = 0; i < 1000; ++i) {
        status = io->read32(io->context, 0x57100c80);
        if (status == UINT32_MAX) return TG_GPU_TLB_IO_ERROR;
        if (status & 0x00ff0000) break;
        io->delay_us(io->context, 2);
    }
    if (i == 1000) return TG_GPU_TLB_NO_SPACE;
    /* Resolve nvgpu_aperture_mask, not just its sysmem argument: the GM20B
     * platform has honors_aperture=false, so SoC DRAM selects VIDMEM (0).
     */
    io->write32(io->context, 0x57100cb8, (uint32_t)(root >> 8));
    io->write32(io->context, 0x57100cbc, 0x80000001); /* ALL_VA + trigger */
    for (i = 0; i < 1000; ++i) {
        status = io->read32(io->context, 0x57100c80);
        if (status == UINT32_MAX) return TG_GPU_TLB_IO_ERROR;
        if (status & 0x8000) return TG_GPU_TLB_OK;
        io->delay_us(io->context, 2);
    }
    return TG_GPU_TLB_TIMEOUT;
}
