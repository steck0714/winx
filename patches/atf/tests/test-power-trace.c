/* Diagnostic contract only; fan response needs the physical Lite. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static uint32_t pwm, ticks, writes[8];
static unsigned int count;
static bool frozen;
uint32_t mmio_read_32(uintptr_t address)
{
    if (address == 0x7000a010U) return pwm;
    assert(address == 0x60005010U);
    if (!frozen) ticks += 100000U;
    return ticks;
}
void mmio_write_32(uintptr_t address, uint32_t value)
{
    assert(address == 0x7000a010U && count < 8);
    writes[count++] = pwm = value;
}
#include POWER_TRACE_SOURCE

int main(void)
{
    const uint32_t saved = 0x80a00000U;
    pwm = saved;
    ticks = UINT32_MAX - 800000U; /* the 32-bit hardware timer wraps */
    tegra_lite_power_trace();
    assert(count == 5 && pwm == saved);
    for (unsigned int i = 0; i < 4; i++)
        assert(writes[i] == ((i & 1) ? 0x80000000U : 0x81000000U));
    assert(writes[4] == saved);
    frozen = true; count = 0;
    tegra_lite_power_trace();
    assert(count == 2 && writes[0] == 0x81000000U && pwm == saved);
    count = 0; pwm = 0;
    tegra_lite_power_trace();
    assert(count == 0);
    puts("PASS: PWM1-only pulses, timer wrap, stopped-timer exit and original-duty restoration");
    return 0;
}
