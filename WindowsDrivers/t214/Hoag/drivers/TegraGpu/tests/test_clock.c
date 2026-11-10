/* SPDX-License-Identifier: MIT */
#include "../platform/t214_clock.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t registers[0x140000 / 4];
static bool counter_bad, calibration_timeout, slowdown_stuck;
static uint32_t adc_fuse;
static uint32_t writes;
static uint8_t rail;
static uint32_t target_khz;
static uint32_t read_reg(void *ctx, uint32_t address)
{
    (void)ctx;
    if (address == 0x7000f930) return 1400;
    if (address == 0x7000fb04) return adc_fuse;
    assert(address >= 0x57000000 && address < 0x57140000);
    return registers[(address - 0x57000000) / 4];
}
static void write_reg(void *ctx, uint32_t address, uint32_t value)
{
    (void)ctx;
    assert(address >= 0x57000000 && address < 0x57140000);
    ++writes;
    if (address == 0x57020160 && slowdown_stuck && !(value & 0x003f0000)) return;
    registers[(address - 0x57000000) / 4] = value;
    if (address == 0x57137000 && (value & 1) && !calibration_timeout) {
        registers[0x137014 / 4] |= 1u << 31;
        registers[0x137018 / 4] |= 20u << 24;
    }
    if (address == 0x57134124 && value == 0x00110320) {
        /* Counter observes GPCCLK (half the PLL), not GPC2CLK. */
        assert(!(registers[0x20160 / 4] & 0x003f0000));
        assert((registers[0x137004 / 4] & 0x3fffff) == (0x10001u | ((target_khz / 19200u) << 8)));
        assert((registers[0x13700c / 4] & 0xff) == 0xf0);
        assert((registers[0x137014 / 4] & 0x2fff0000) == 0x2e010000);
        assert((registers[0x132820 / 4] & 0x7f) == 20);
        registers[0x134128 / 4] = counter_bad ? 3200 : target_khz / 48;
    }
}
static bool read_pmic(void *ctx, uint8_t address, uint8_t reg, uint8_t *value)
{
    (void)ctx;
    assert(address == 0x33);
    *value = reg == 0x23 ? rail : 0x41;
    return true;
}
static bool write_pmic(void *ctx, uint8_t addr, uint8_t reg, uint8_t value)
{ (void)ctx; assert(addr == 0x33 && reg == 0x23); assert(!(registers[0x137000 / 4] & 1)); rail = value; return true; }
static void delay(void *ctx, uint32_t us) { (void)ctx; (void)us; }
static void reset(void)
{
    memset(registers, 0, sizeof(registers));
    registers[0] = 0x12e000b1;
    registers[0x137000 / 4] = 2;
    registers[0x20798 / 4] = 0x105;
    registers[0x20160 / 4] = 0x00220000;
    adc_fuse = 0;
    rail = 110;
    target_khz = TG_GPU_FIXED_KHZ;
    counter_bad = calibration_timeout = slowdown_stuck = false;
    writes = 0;
}
int main(void)
{
    struct tg_gpu_power p = {.owned = true, .ready = true, .pmic_address = 0x33};
    struct tg_gpu_power_io io = {NULL, read_reg, write_reg, read_pmic, write_pmic, delay};
    struct tg_gpu_clock c = {0};
    assert(tg_gpu_clock_required_uv(0, TG_GPU_FIXED_KHZ) == UINT32_MAX);
    assert(tg_gpu_clock_required_uv(5001, TG_GPU_FIXED_KHZ) == UINT32_MAX);
    assert(tg_gpu_clock_required_uv(1400, TG_GPU_FIXED_KHZ) == 800000);
    assert(tg_gpu_clock_required_uv(100, TG_GPU_FIXED_KHZ) > 800000);
    reset();
    assert(tg_gpu_clock_start(&io, &p, target_khz, &c) == TG_GPU_CLOCK_OK);
    assert(c.measured_khz == TG_GPU_FIXED_KHZ && c.counter == 15200);
    assert(c.external_calibration == 20 && c.stage == 5);
    assert(registers[0x20798 / 4] == 0x105);
    assert(registers[0x20160 / 4] == 0); /* persists beyond the counter sample */
    assert(tg_gpu_clock_stop(&io, &c) == TG_GPU_CLOCK_OK);
    assert(!c.owned && !c.running && registers[0x137000 / 4] == 2);
    assert(registers[0x20160 / 4] == 0x00220000);
    assert((registers[0x132820 / 4] & 0x1007f) == 0);
    reset(); memset(&c, 0, sizeof(c));
    /* Fused slope 10mV, intercept 600mV gives the same 800mV calibration. */
    adc_fuse = (1u << 30) | (10u << 24) | (600u << 4);
    assert(tg_gpu_clock_start(&io, &p, target_khz, &c) == TG_GPU_CLOCK_OK);
    assert(tg_gpu_clock_stop(&io, &c) == TG_GPU_CLOCK_OK);
    assert(registers[0x20160 / 4] == 0x00220000);
    reset(); memset(&c, 0, sizeof(c)); slowdown_stuck=true;
    assert(tg_gpu_clock_start(&io, &p, target_khz, &c) == TG_GPU_CLOCK_READBACK_ERROR);
    assert(c.owned && !c.running);
    assert(tg_gpu_clock_stop(&io, &c) == TG_GPU_CLOCK_OK);
    assert(registers[0x20160 / 4] == 0x00220000);
    reset(); memset(&c, 0, sizeof(c)); counter_bad = true;
    assert(tg_gpu_clock_start(&io, &p, target_khz, &c) == TG_GPU_CLOCK_COUNTER_ERROR);
    assert(c.owned);
    assert(tg_gpu_clock_stop(&io, &c) == TG_GPU_CLOCK_OK);
    reset(); memset(&c, 0, sizeof(c)); calibration_timeout = true;
    assert(tg_gpu_clock_start(&io, &p, target_khz, &c) == TG_GPU_CLOCK_CALIBRATION_ERROR);
    assert(tg_gpu_clock_stop(&io, &c) == TG_GPU_CLOCK_OK);
    reset(); memset(&c, 0, sizeof(c)); registers[0x137000 / 4] = 1;
    assert(tg_gpu_clock_start(&io, &p, target_khz, &c) == TG_GPU_CLOCK_BAD_STATE);
    assert(!writes && !c.owned);
    reset(); memset(&c, 0, sizeof(c)); target_khz = TG_GPU_OVERCLOCK_KHZ;
    assert(tg_gpu_clock_start(&io, &p, target_khz, &c) == TG_GPU_CLOCK_OK);
    assert(c.target_khz == 1228800 && c.measured_khz == 1228800);
    assert(c.rail_uv > 800000 && c.rail_uv <= 1050000);
    assert(rail == (c.rail_uv - 250000) / 5000);
    assert(tg_gpu_clock_stop(&io, &c) == TG_GPU_CLOCK_OK);
    assert(tg_gpu_clock_start(&io, &p, 1234567, &c) == TG_GPU_CLOCK_BAD_STATE);
    puts("T214 C1 NA clock ordering, counter and unwind tests passed (host model).");
    return 0;
}
