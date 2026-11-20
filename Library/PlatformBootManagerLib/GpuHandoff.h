// SPDX-License-Identifier: GPL-2.0-only
/* GM20B GPU hand-off from EDK II to Windows (Erista / Lite / OLED).
 *
 * The Windows TegraGpu driver (tg_gpu_power_up in platform/t210_power.c and
 * platform/t214_power.c) requires the GPU to arrive in a known cold state:
 *   - CAR RESET_X bit 24 set   (GPU in reset)
 *   - PMC GPU clamp bit 0 set  (GPU isolated)
 * and returns TG_GPU_POWER_BUSY otherwise, so a UEFI path that left the GPU
 * clocked / unclamped makes the driver refuse to start. The OLED upstream
 * (DanilKhmyrov/windows-switch, patches/...gpu-gputestonly.patch) states the
 * same contract: "The GPU stays in reset and clamped; the driver powers it".
 *
 * This header holds only the register logic, with no EDK II dependency, so it
 * can be compiled and tested on a host (Tools/host-tests/gpu-handoff). It
 * never powers the GPU: it only verifies the contract and, when it does not
 * hold, restores it in the same order as tg_gpu_power_down() (MC drain, clamp,
 * reset, clock gate). It touches no PMIC and no PLL.
 *
 * Board differences are limited to the expected HIDREV chip revision.
 */
#ifndef WINX_GPU_HANDOFF_H
#define WINX_GPU_HANDOFF_H

#include <stdbool.h>
#include <stdint.h>

#define GH_APB_MISC_HIDREV 0x70000804u
#define GH_HIDREV_MASK     0xfff0u
#define GH_HIDREV_T210     0x2110u /* Erista, HAC-001 */
#define GH_HIDREV_T210B01  0x2120u /* Mariko family: Lite HDH-001, OLED HEG-001, Iowa */

#define GH_CAR_BASE        0x60006000u
#define GH_CAR_CLOCK_X     (GH_CAR_BASE + 0x280u)
#define GH_CAR_CLOCK_SET   (GH_CAR_BASE + 0x284u)
#define GH_CAR_CLOCK_CLR   (GH_CAR_BASE + 0x288u)
#define GH_CAR_RESET_X     (GH_CAR_BASE + 0x28cu)
#define GH_CAR_RESET_SET   (GH_CAR_BASE + 0x290u)
#define GH_CAR_RESET_CLR   (GH_CAR_BASE + 0x294u)
#define GH_GPU_GATE        (1u << 24)
#define GH_GPU_REF         (1u << 29)
#define GH_GPU_CLOCKS      (GH_GPU_GATE | GH_GPU_REF)
#define GH_PMC_GPU_CLAMP   0x7000e6d4u
#define GH_MC_FLUSH        0x70019970u
#define GH_MC_FLUSH_STATUS 0x70019974u
#define GH_MC_GPU_BIT      (1u << 2)

struct gpu_handoff_io {
    void *context;
    uint32_t (*read32)(void *context, uint32_t address);
    void (*write32)(void *context, uint32_t address, uint32_t value);
    void (*delay_us)(void *context, uint32_t microseconds);
};

enum gpu_handoff_result {
    GPU_HANDOFF_COLD,          /* already in the contract state; nothing written */
    GPU_HANDOFF_RESTORED,      /* was not cold; isolated again and verified */
    GPU_HANDOFF_BAD_ARGUMENT,
    GPU_HANDOFF_WRONG_SOC,     /* HIDREV is not the expected chip; nothing touched */
    GPU_HANDOFF_BUS_ERROR,     /* a read returned all ones; nothing written */
    GPU_HANDOFF_DRAIN_TIMEOUT, /* MC did not drain; clamp/reset left untouched */
    GPU_HANDOFF_NOT_ISOLATED   /* readback after restore still not cold */
};

struct gpu_handoff_report {
    uint32_t hidrev;
    uint32_t reset_before, clamp_before, clocks_before;
    uint32_t reset_after, clamp_after, clocks_after;
    bool wrote;
};

static inline bool gh_is_cold(uint32_t reset, uint32_t clamp)
{
    return (reset & GH_GPU_GATE) != 0 && (clamp & 1u) != 0;
}

static inline void gh_update32(const struct gpu_handoff_io *io, uint32_t address,
                               uint32_t mask, uint32_t value)
{
    uint32_t old = io->read32(io->context, address);
    io->write32(io->context, address, (old & ~mask) | (value & mask));
    (void)io->read32(io->context, address);
}

/* Same handshake as drain_mc() in the driver: request the flush, then require
 * the status bit to be set and stable for five consecutive reads. */
static inline bool gh_drain_mc(const struct gpu_handoff_io *io)
{
    unsigned int attempt, sample;
    uint32_t request;
    gh_update32(io, GH_MC_FLUSH, GH_MC_GPU_BIT, GH_MC_GPU_BIT);
    request = io->read32(io->context, GH_MC_FLUSH);
    if (request == UINT32_MAX || !(request & GH_MC_GPU_BIT))
        return false;
    for (attempt = 0; attempt < 100000; ++attempt) {
        uint32_t value;
        io->delay_us(io->context, 10);
        value = io->read32(io->context, GH_MC_FLUSH_STATUS);
        if (value == UINT32_MAX)
            return false;
        if (!(value & GH_MC_GPU_BIT))
            continue;
        for (sample = 0; sample < 5; ++sample)
            if (io->read32(io->context, GH_MC_FLUSH_STATUS) != value)
                break;
        if (sample == 5)
            return true;
    }
    return false;
}

/* expected_hidrev: GH_HIDREV_T210 for Erista, GH_HIDREV_T210B01 for Lite / OLED.
 * report may be NULL. Safe to call repeatedly (idempotent on a cold GPU). */
static inline enum gpu_handoff_result
gpu_handoff_prepare(const struct gpu_handoff_io *io, uint32_t expected_hidrev,
                    struct gpu_handoff_report *report)
{
    struct gpu_handoff_report local;
    uint32_t reset, clamp, clocks, hid, mc_before;
    bool flush_was_set;

    if (!io || !io->read32 || !io->write32 || !io->delay_us)
        return GPU_HANDOFF_BAD_ARGUMENT;
    if (!report)
        report = &local;
    report->wrote = false;
    report->reset_after = report->clamp_after = report->clocks_after = 0;

    hid = io->read32(io->context, GH_APB_MISC_HIDREV);
    report->hidrev = hid;
    if (hid == UINT32_MAX)
        return GPU_HANDOFF_BUS_ERROR;
    if ((hid & GH_HIDREV_MASK) != expected_hidrev)
        return GPU_HANDOFF_WRONG_SOC;

    reset = io->read32(io->context, GH_CAR_RESET_X);
    clamp = io->read32(io->context, GH_PMC_GPU_CLAMP);
    clocks = io->read32(io->context, GH_CAR_CLOCK_X);
    report->reset_before = reset;
    report->clamp_before = clamp;
    report->clocks_before = clocks;
    if (reset == UINT32_MAX || clamp == UINT32_MAX || clocks == UINT32_MAX)
        return GPU_HANDOFF_BUS_ERROR;
    if (gh_is_cold(reset, clamp)) {
        report->reset_after = reset;
        report->clamp_after = clamp;
        report->clocks_after = clocks;
        return GPU_HANDOFF_COLD;
    }

    /* Not cold. Quiesce memory traffic first, exactly like tg_gpu_power_down();
     * if the MC will not drain, leave clamp/reset alone (clamping a GPU that is
     * still master on the bus is worse than reporting the failure). */
    report->wrote = true;
    mc_before = io->read32(io->context, GH_MC_FLUSH);
    if (mc_before == UINT32_MAX) {
        report->wrote = false;
        return GPU_HANDOFF_BUS_ERROR;
    }
    flush_was_set = (mc_before & GH_MC_GPU_BIT) != 0;
    if (!gh_drain_mc(io)) {
        if (!flush_was_set)
            gh_update32(io, GH_MC_FLUSH, GH_MC_GPU_BIT, 0);
        return GPU_HANDOFF_DRAIN_TIMEOUT;
    }
    gh_update32(io, GH_PMC_GPU_CLAMP, 1u, 1u);
    io->delay_us(io->context, 10);
    io->write32(io->context, GH_CAR_RESET_SET, GH_GPU_GATE);
    io->delay_us(io->context, 10);
    io->write32(io->context, GH_CAR_CLOCK_CLR, GH_GPU_CLOCKS);
    io->delay_us(io->context, 10);
    /* Release only the flush request we raised; the driver redoes the drain
     * itself when it takes ownership. */
    if (!flush_was_set)
        gh_update32(io, GH_MC_FLUSH, GH_MC_GPU_BIT, 0);

    reset = io->read32(io->context, GH_CAR_RESET_X);
    clamp = io->read32(io->context, GH_PMC_GPU_CLAMP);
    clocks = io->read32(io->context, GH_CAR_CLOCK_X);
    report->reset_after = reset;
    report->clamp_after = clamp;
    report->clocks_after = clocks;
    if (reset == UINT32_MAX || clamp == UINT32_MAX || clocks == UINT32_MAX)
        return GPU_HANDOFF_BUS_ERROR;
    if (!gh_is_cold(reset, clamp) || (clocks & GH_GPU_CLOCKS) != 0)
        return GPU_HANDOFF_NOT_ISOLATED;
    return GPU_HANDOFF_RESTORED;
}

#endif
