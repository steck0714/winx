/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_T214_CLOCK_H
#define TEGRA_GPU_T214_CLOCK_H

#include "t214_power.h"

/* Fixed bring-up profile, not a governor. GPCCLK = 38.4 MHz * 38 / 2. */
#define TG_GPU_FIXED_KHZ 729600u
#define TG_GPU_OVERCLOCK_KHZ 1228800u
#define TG_GPU_REFERENCE_KHZ 38400u
#define TG_GPU_CLOCK_SAVED_REGS 13u

enum tg_gpu_clock_result {
    TG_GPU_CLOCK_OK, TG_GPU_CLOCK_BAD_STATE, TG_GPU_CLOCK_RAIL_ERROR,
    TG_GPU_CLOCK_CALIBRATION_ERROR, TG_GPU_CLOCK_READBACK_ERROR,
    TG_GPU_CLOCK_COUNTER_ERROR
};

struct tg_gpu_clock {
    bool owned;
    bool running;
    uint32_t saved[TG_GPU_CLOCK_SAVED_REGS];
    uint32_t measured_khz;
    uint32_t target_khz;
    uint32_t rail_uv;
    uint32_t counter;
    uint32_t stage;
    int32_t adc_slope_uv;
    int32_t adc_intercept_uv;
    int32_t external_calibration;
};

/* Requires an idle GPU just acquired by power_up, no live DMA and exclusive
 * register ownership. Zero state first. Uses the C1 NA PLL path, as required
 * by Switchroot on T210B01. Only GPU M1 voltage is set, before PLL startup;
 * power_down restores it after the clock is stopped. CPU/DDR are untouched.
 * Stop channels and drain their writes before calling clock_stop.
 */
enum tg_gpu_clock_result tg_gpu_clock_start(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, uint32_t target_khz, struct tg_gpu_clock *clock);
enum tg_gpu_clock_result tg_gpu_clock_measure(const struct tg_gpu_power_io *io,
    struct tg_gpu_clock *clock);
enum tg_gpu_clock_result tg_gpu_clock_stop(const struct tg_gpu_power_io *io,
    struct tg_gpu_clock *clock);

/* Conservative non-SLT 768/1228.8 MHz CVB envelopes, covering -40..105 C,
 * rounded to the MAX77812 5 mV step. The second profile is explicitly opt-in.
 */
uint32_t tg_gpu_clock_required_uv(uint32_t gpu_speedo, uint32_t target_khz);

#endif
