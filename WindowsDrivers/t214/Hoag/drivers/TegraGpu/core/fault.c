/* SPDX-License-Identifier: MIT
 * Copyright (c) 2014-2018, NVIDIA CORPORATION. All rights reserved.
 * Follows Switchroot nvgpu 1ae0167d gm20b_fifo_init_pbdma_intr_descs,
 * gk20a_fifo_handle_pbdma_intr and gk20a_fifo_isr. See ../NOTICE.
 */
#include "fault.h"
#include "../windows/control.h"
#define GPU 0x57000000u
#define ENGINE_EVENT 0x01000000u
#define PBDMA_SUMMARY 0x20000000u
bool tg_gpu_fault_pending(const struct tg_gpu_fault_status *s)
{
    uint32_t fifo = s->fifo, pbdma = s->pbdma;
    if (pbdma == ENGINE_EVENT && !s->pbdma1 && !s->mmu) {
        fifo &= ~PBDMA_SUMMARY;
        pbdma = 0;
    }
    return tg_gpu_fifo_has_error(fifo) || s->mmu || pbdma || s->pbdma1;
}
static void read_status(const struct tg_gpu_power_io *io, struct tg_gpu_fault_status *s)
{
    s->fifo = io->read32(io->context, GPU + 0x2100);
    s->mmu = io->read32(io->context, GPU + 0x259c);
    s->pbdma = io->read32(io->context, GPU + 0x40108);
    s->pbdma1 = io->read32(io->context, GPU + 0x40148);
}
bool tg_gpu_fault_poll(const struct tg_gpu_power_io *io, struct tg_gpu_fault_status *s)
{
    read_status(io, s);
    /* ENGINE (bit 24) is absent from GM20B's fatal/restartable masks. The
     * upstream ISR returns NO_RC for that lone bit, then W1C-acks it and the
     * FIFO PBDMA summary. Do not clear or mask concurrent real/unknown faults.
     * Re-read for concurrent faults, but do not invent channel recovery for a
     * reasserted ENGINE notification: the upstream handler returns NO_RC.
     * The caller's bounded fence wait still requires actual GPU completion.
     */
    if (s->pbdma == ENGINE_EVENT && !s->pbdma1 && !s->mmu &&
        !tg_gpu_fifo_has_error(s->fifo & ~PBDMA_SUMMARY)) {
        io->write32(io->context, GPU + 0x40108, ENGINE_EVENT);
        (void)io->read32(io->context, GPU + 0x40108);
        io->write32(io->context, GPU + 0x2100, PBDMA_SUMMARY);
        (void)io->read32(io->context, GPU + 0x2100);
        read_status(io, s);
    }
    return tg_gpu_fault_pending(s);
}
