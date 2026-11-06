/* Simulated I2C/PMIC contract checks; this does not emulate physical power. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t regs[64], pmic_config, pointer, reset_bits, clocks;
static unsigned int power_requests, delay_ms;
static bool nack_first, nack_final, mismatch;

void udelay(unsigned int amount) { (void)amount; }
void mdelay(unsigned int amount) { delay_ms += amount; }

uint32_t mmio_read_32(uintptr_t address)
{
    if (address == 0x60006008U) return reset_bits;
    if (address == 0x60006014U) return clocks;
    assert(address >= 0x7000d000U && address < 0x7000d100U);
    return regs[(address - 0x7000d000U) / 4];
}

void mmio_write_32(uintptr_t address, uint32_t value)
{
    if (address == 0x60006308U) { reset_bits |= value; return; }
    if (address == 0x6000630cU) { reset_bits &= ~value; return; }
    if (address == 0x60006328U) { clocks |= value; return; }
    if (address == 0x6000632cU) { clocks &= ~value; return; }
    if (address == 0x60006128U) { assert(value == 6U << 29); return; }
    assert(address >= 0x7000d000U && address < 0x7000d100U);
    unsigned int reg = (unsigned int)(address - 0x7000d000U);
    regs[reg / 4] = value;
    if (reg == 0x8c) { regs[reg / 4] = 0; regs[0x68 / 4] = 1U << 11; }
    if (reg == 0 && (value & 0x200)) {
        bool read = (value & 0x40) != 0;
        unsigned int count = ((value >> 1) & 7) + 1;
        uint32_t data = regs[0x0c / 4];
        assert(regs[0x04 / 4] == (0x3cU << 1) + (read ? 1U : 0U));
        regs[0x1c / 4] = 0;
        if (nack_first) { regs[0x1c / 4] = 1; return; }
        if (read) {
            assert(count == 1 && pointer == 0x42);
            regs[0x0c / 4] = pmic_config;
        } else if (count == 1) {
            assert(data == 0x42);
            pointer = data;
        } else {
            assert(count == 2);
            if ((data & 0xff) == 0x42) {
                pmic_config = data >> 8;
                if (mismatch) pmic_config ^= 0x80;
            } else {
                assert(data == 0x8041); // the only other write is soft reset
                power_requests++;
                if (nack_final) regs[0x1c / 4] = 1;
            }
        }
    }
}

#include POWER_SOURCE

static void reset_mock(uint32_t config)
{
    memset(regs, 0, sizeof(regs));
    pmic_config = config;
    pointer = reset_bits = clocks = power_requests = delay_ms = 0;
    nack_first = nack_final = mismatch = false;
}

int main(void)
{
    reset_mock(0x9f);
    assert(tegra_lite_pmic_power(false) == -5);
    assert(pmic_config == 0x19 && power_requests == 1 && delay_ms == 500);
    reset_mock(0x19);
    assert(tegra_lite_pmic_power(true) == -5);
    assert(pmic_config == 0x99 && power_requests == 1 && delay_ms == 500);
    reset_mock(0x19); nack_first = true;
    assert(tegra_lite_pmic_power(true) == -2 && power_requests == 0);
    reset_mock(0x19); mismatch = true;
    assert(tegra_lite_pmic_power(true) == -3 && power_requests == 0);
    reset_mock(0x19); nack_final = true;
    assert(tegra_lite_pmic_power(true) == -4 && power_requests == 1 && delay_ms == 500);
    puts("PMIC off/restart wake policy and I2C failure paths passed (simulated)");
    return 0;
}
