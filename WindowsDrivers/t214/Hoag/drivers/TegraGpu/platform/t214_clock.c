/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2014-2018 NVIDIA CORPORATION. All rights reserved.
 * Copyright 2026 ruje0504-2
 * C1 NA PLL sequence and counter follow Switchroot nvgpu clk_gm20b.c
 * at 1ae0167d. See NOTICE. No live frequency scaling is implemented.
 */
#include "t214_clock.h"
#include "../core/identity.h"

#define GPU 0x57000000u
#define CFG 0x137000u
#define COEFF 0x137004u
#define CFG2 0x13700cu
#define CFG3 0x137018u
#define DVFS0 0x137010u
#define DVFS1 0x137014u
#define DVFS2 0x132820u
#define VCO 0x137100u
#define OUT 0x137250u
#define BYPASS 0x137340u
#define SLOW 0x020160u
#define THERM 0x020798u
#define SVOP 0x021944u
#define SVOP_OVERRIDE 0x021948u
#define COUNT_CFG 0x134124u
#define COUNT_VAL 0x134128u
#define ENABLE 1u
#define IDDQ 2u
#define SYNC 4u

static const uint32_t clock_regs[TG_GPU_CLOCK_SAVED_REGS] = {
    CFG, COEFF, CFG2, CFG3, DVFS0, DVFS1, DVFS2, VCO, OUT, BYPASS,
    SVOP, SVOP_OVERRIDE, SLOW
};
/* Ignore read-only lock/calibration/testout bits when restoring control. */
static const uint32_t clock_masks[TG_GPU_CLOCK_SAVED_REGS] = {
    0x17, 0x3fffff, 0xff0000ff, 0x00ff01ff, 0x00007f7f,
    0x7fff0000, 0x0001007f, 1, 0x80003f3f, 1, 3, 1, 0x003f0000
};

static uint32_t clk_read(const struct tg_gpu_power_io *io, uint32_t reg)
{ return io->read32(io->context, GPU + reg); }

static void clk_write(const struct tg_gpu_power_io *io, uint32_t reg, uint32_t val)
{
    io->write32(io->context, GPU + reg, val);
    (void)clk_read(io, reg);
}

static void clk_update(const struct tg_gpu_power_io *io, uint32_t reg,
                       uint32_t mask, uint32_t val)
{ clk_write(io, reg, (clk_read(io, reg) & ~mask) | (val & mask)); }

static void clk_select(const struct tg_gpu_power_io *io, uint32_t vco)
{
    /* Switchroot masks throttling only across the mux update. */
    uint32_t saved = clk_read(io, THERM);
    clk_write(io, THERM, 0);
    clk_update(io, VCO, 1, vco);
    clk_write(io, THERM, saved);
}

static int32_t div_nearest(int32_t n, int32_t d)
{ return n < 0 ? -((-n + d / 2) / d) : (n + d / 2) / d; }

uint32_t tg_gpu_clock_required_uv(uint32_t speedo, uint32_t target)
{
    int32_t base, peak = 0, t, c0, c1, c2, c4, c5;
    if (!speedo || speedo > 5000 ||
        (target != TG_GPU_FIXED_KHZ && target != TG_GPU_OVERCLOCK_KHZ))
        return UINT32_MAX;
    /* GPUB01_NA_CVB_TABLE at 768000: c0=891575,c1=-4409,c2=-584,
     * c3=0,c4=-2849,c5=39, speedo_scale=100, thermal_scale=10.
     * 729.6 MHz uses the next higher characterized frequency envelope.
     */
    if (target == TG_GPU_OVERCLOCK_KHZ) {
        c0 = 1277303; c1 = -11675; c2 = -859; c4 = 3722; c5 = 313;
    } else {
        c0 = 891575; c1 = -4409; c2 = -584; c4 = -2849; c5 = 39;
    }
    base = div_nearest(c2 * (int32_t)speedo, 100) + c1;
    base = div_nearest(base * (int32_t)speedo, 100) + c0;
    for (t = -40; t <= 105; ++t) {
        int32_t uv = base + div_nearest((div_nearest(c5 * t, 10) + c4) * t, 10);
        if (uv > peak) peak = uv;
    }
    if (peak < 800000) peak = 800000; /* Switchroot default VTS floor. */
    return ((uint32_t)peak + 4999u) / 5000u * 5000u;
}

enum tg_gpu_clock_result tg_gpu_clock_start(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, uint32_t target, struct tg_gpu_clock *c)
{
    uint32_t i, fuse, coeff, value, required_uv, mv;
    uint8_t rail, enabled;
    struct tg_gpu_identity id;
    if (!io || !power || !c || !io->read32 || !io->write32 ||
        !io->read_pmic || !io->write_pmic || !io->delay_us || !power->ready || c->owned ||
        (target != TG_GPU_FIXED_KHZ && target != TG_GPU_OVERCLOCK_KHZ) ||
        !tg_gpu_identify(clk_read(io, 0), &id) || id.chipset != TG_GPU_CHIP_GM20B_B)
        return TG_GPU_CLOCK_BAD_STATE;
    c->stage = 0;
    /* Freshly reset GPU only. Existing clock owners are never displaced. */
    if ((clk_read(io, CFG) & ENABLE) || (clk_read(io, VCO) & 1))
        return TG_GPU_CLOCK_BAD_STATE;
    required_uv = tg_gpu_clock_required_uv(io->read32(io->context, 0x7000f930u), target);
    /* Conservative B01 table limit. Do not overvolt a part whose calculated
     * envelope would need more than 1050 mV. Only the GPU M1 rail changes.
     */
    if (required_uv > 1050000u) return TG_GPU_CLOCK_RAIL_ERROR;
    if (!io->read_pmic(io->context, power->pmic_address, 0x23, &rail) ||
        !io->read_pmic(io->context, power->pmic_address, 0x06, &enabled) ||
        !(enabled & 1))
        return TG_GPU_CLOCK_RAIL_ERROR;
    /* PLL is still disabled; program voltage before frequency. power_down
     * restores the pre-acquisition voltage after clocks/reset are stopped.
     */
    rail = (uint8_t)((required_uv - 250000u) / 5000u);
    if (!io->write_pmic(io->context, power->pmic_address, 0x23, rail) ||
        !io->read_pmic(io->context, power->pmic_address, 0x23, &enabled) || enabled != rail)
        return TG_GPU_CLOCK_RAIL_ERROR;
    io->delay_us(io->context, 5000);
    c->rail_uv = required_uv;
    c->target_khz = target;
    mv = required_uv / 1000u;
    for (i = 0; i < TG_GPU_CLOCK_SAVED_REGS; ++i)
        c->saved[i] = clk_read(io, clock_regs[i]);
    c->owned = true;
    c->running = false;
    c->stage = 1;
    clk_select(io, 0);
    clk_update(io, OUT, 0x80003f3f, 0x80000000);
    clk_update(io, BYPASS, 1, 0);
    if (!(clk_read(io, SVOP) & 3)) {
        clk_update(io, SVOP, 3, 2);
        clk_update(io, SVOP_OVERRIDE, 1, 1);
    }
    /* Switchroot disables idle slowdown during clock setup, not only while
     * measuring. Keep the fixed profile through command execution and restore
     * this field with the other controls when relinquishing the GPU.
     */
    clk_update(io, SLOW, 0x003f0000, 0);
    if (clk_read(io, SLOW) & 0x003f0000)
        return TG_GPU_CLOCK_READBACK_ERROR;

    /* NA mode is required on B01. Use C1 VCO_CTRL/DFS_CTRL, not B1. */
    clk_update(io, DVFS1, 0x2fff0000, 0x2c000000);
    clk_update(io, CFG3, 0x1ff, 0x0f);
    fuse = io->read32(io->context, 0x7000fb04u); /* FUSE + 0x100 + 0x204 */
    c->adc_slope_uv = (int32_t)(((fuse >> 24) & 0x3f) * 1000 + ((fuse >> 14) & 0x3ff));
    c->adc_intercept_uv = (int32_t)(((fuse >> 4) & 0x3ff) * 1000 + (fuse & 0xf) * 100);
    if (!(fuse >> 30) || !c->adc_slope_uv || !c->adc_intercept_uv) {
        c->adc_slope_uv = 0;
        c->adc_intercept_uv = 0;
    } else {
        clk_update(io, DVFS1, 1u << 30, 1u << 30);
    }
    clk_update(io, CFG, IDDQ, 0);
    io->delay_us(io->context, 5);
    clk_update(io, CFG2, 0xff000000, 0x04000000);
    clk_update(io, CFG3, 0x00ff0000, 0x00050000);
    c->stage = 2;
    if (!c->adc_slope_uv) {
        clk_update(io, DVFS1, 1u << 30, 1u << 30);
        clk_update(io, CFG, ENABLE, ENABLE);
        for (i = 0; i < 5; ++i) {
            if (clk_read(io, DVFS1) & (1u << 31)) break;
            io->delay_us(io->context, 1);
        }
        value = (clk_read(io, CFG3) >> 24) & 0x7f;
        clk_update(io, CFG, ENABLE, 0);
        if (i == 5) return TG_GPU_CLOCK_CALIBRATION_ERROR;
        c->adc_slope_uv = 10000;
        c->adc_intercept_uv = (int32_t)required_uv - (int32_t)value * 10000;
    }
    c->external_calibration = div_nearest((int32_t)required_uv - c->adc_intercept_uv, c->adc_slope_uv);
    if (c->external_calibration <= -64 || c->external_calibration >= 64)
        return TG_GPU_CLOCK_CALIBRATION_ERROR;

    c->stage = 3;
    clk_update(io, CFG, SYNC, 0);
    clk_update(io, CFG, ENABLE, 0);
    /* C1 coefficient round((-172550 * 0.8 + 195374) / 1000) = 57.
     * DET_MAX=0 preserves the upper frequency bound if the rail rises.
     */
    clk_update(io, DVFS2, 1u << 16, 1u << 16);
    value = (uint32_t)div_nearest(div_nearest(-172550 * (int32_t)mv, 1000) + 195374, 1000);
    if (value > 127) return TG_GPU_CLOCK_CALIBRATION_ERROR;
    clk_update(io, DVFS0, 0x7f7f, value);
    io->delay_us(io->context, 1);
    clk_update(io, DVFS2, 1u << 16, 0);
    clk_update(io, DVFS2, 0x7f, (uint32_t)c->external_calibration);
    io->delay_us(io->context, 1);
    clk_update(io, DVFS1, 0x0fff0000, 0x0e010000);
    /* Effective N=38 or 64, SDM=-4096 (top byte f0), M=1, P=1. */
    clk_update(io, CFG2, 0xff, 0xf0);
    coeff = 0x00010001 | ((target / 19200u) << 8);
    clk_update(io, COEFF, 0x3fffff, coeff);
    clk_update(io, CFG, ENABLE, ENABLE);
    io->delay_us(io->context, 40); /* NA mode has no usable LOCK bit. */
    clk_update(io, CFG, SYNC, SYNC);
    clk_select(io, 1);
    if ((clk_read(io, COEFF) & 0x3fffff) != coeff ||
        (clk_read(io, CFG) & (ENABLE | SYNC | IDDQ)) != (ENABLE | SYNC) ||
        !(clk_read(io, VCO) & 1) || (clk_read(io, CFG2) & 0xff) != 0xf0)
        return TG_GPU_CLOCK_READBACK_ERROR;
    c->stage = 4;
    c->running = true;
    return tg_gpu_clock_measure(io, c);
}

enum tg_gpu_clock_result tg_gpu_clock_measure(const struct tg_gpu_power_io *io,
    struct tg_gpu_clock *c)
{
    uint32_t slow, counter_cfg, first, second;
    if (!c->owned || !c->running) return TG_GPU_CLOCK_BAD_STATE;
    slow = clk_read(io, SLOW);
    counter_cfg = clk_read(io, COUNT_CFG);
    clk_update(io, SLOW, 0x3f0000, 0);
    clk_write(io, COUNT_CFG, 0x01000000);
    clk_write(io, COUNT_CFG, 0x00110000 | 800);
    io->delay_us(io->context, 200);
    first = clk_read(io, COUNT_VAL) & 0xfffff;
    io->delay_us(io->context, 100);
    second = clk_read(io, COUNT_VAL) & 0xfffff;
    clk_write(io, COUNT_CFG, counter_cfg);
    clk_write(io, SLOW, slow);
    c->counter = second;
    c->measured_khz = (uint32_t)((uint64_t)second * TG_GPU_REFERENCE_KHZ / 800);
    /* Counter's resolution is 48 kHz; tolerate 0.5% including oscillator
     * error. This is a live counter check, not a report of programmed N.
     */
    if (!first || first != second || c->measured_khz < c->target_khz * 995u / 1000u ||
        c->measured_khz > c->target_khz * 1005u / 1000u)
        return TG_GPU_CLOCK_COUNTER_ERROR;
    c->stage = 5;
    return TG_GPU_CLOCK_OK;
}

enum tg_gpu_clock_result tg_gpu_clock_stop(const struct tg_gpu_power_io *io,
    struct tg_gpu_clock *c)
{
    uint32_t i;
    bool ok = true;
    if (!c->owned) return TG_GPU_CLOCK_OK;
    clk_select(io, 0);
    clk_update(io, CFG, SYNC, 0);
    clk_update(io, CFG, ENABLE, 0);
    c->running = false;
    /* Restore CFG last, so the original IDDQ state follows other registers. */
    for (i = TG_GPU_CLOCK_SAVED_REGS; i-- > 0; ) {
        clk_update(io, clock_regs[i], clock_masks[i], c->saved[i]);
        if ((clk_read(io, clock_regs[i]) & clock_masks[i]) !=
            (c->saved[i] & clock_masks[i])) ok = false;
    }
    if (!ok) return TG_GPU_CLOCK_READBACK_ERROR;
    c->owned = false;
    return TG_GPU_CLOCK_OK;
}
