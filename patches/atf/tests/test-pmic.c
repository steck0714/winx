/* Simulated I2C/PMIC contract checks; this does not emulate physical power. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t regs[64], pmic[256], pmc[0x930/4], pointer, reset_bits, clocks;
static unsigned int power_requests, delay_ms, transfers, cleared_irqs;
static bool nack_first, nack_final, mismatch, nack_latch, need_bus_recovery, clear_failed;
static unsigned int recoveries;
#include "pmic-gpio-wire.h"

void udelay(unsigned int amount) { (void)amount; }
void mdelay(unsigned int amount) { delay_ms += amount; }

uint32_t mmio_read_32(uintptr_t address)
{
    if (address >= 0x6000d600U && address < 0x6000d700U)
        return wire_reg_read((unsigned int)(address - 0x6000d600U));
    if (address == 0x60006008U) return reset_bits;
    if (address == 0x60006014U) return clocks;
    if (address >= 0x7000e400U && address < 0x7000ed30U)
        return pmc[(address - 0x7000e400U) / 4];
    assert(address >= 0x7000d000U && address < 0x7000d100U);
    return regs[(address - 0x7000d000U) / 4];
}

void mmio_write_32(uintptr_t address, uint32_t value)
{
    if (address >= 0x6000d600U && address < 0x6000d700U) {
        wire_reg_write((unsigned int)(address - 0x6000d600U), value);
        return;
    }
    if (address == 0x60006308U) { reset_bits |= value; return; }
    if (address == 0x6000630cU) { reset_bits &= ~value; return; }
    if (address == 0x60006328U) { clocks |= value; return; }
    if (address == 0x6000632cU) { clocks &= ~value; return; }
    if (address == 0x60006128U) { assert(value == 6U << 29); return; }
    if (address >= 0x7000e400U && address < 0x7000ed30U) {
        pmc[(address - 0x7000e400U) / 4] = value;
        return;
    }
    assert(address >= 0x7000d000U && address < 0x7000d100U);
    unsigned int reg = (unsigned int)(address - 0x7000d000U);
    if (reg == 0x68) { regs[reg / 4] &= ~value; return; }
    regs[reg / 4] = value;
    if (reg == 0x84 && (value & 7U) == 7U) {
        recoveries++;
        if (!clear_failed) {
            regs[0x68 / 4] |= 1U << 11;
            regs[0x88 / 4] = 1;
            need_bus_recovery = false;
        }
    }
    if (reg == 0x8c) { regs[reg / 4] = 0; if ((regs[0x84/4] & 7U) == 3U) regs[0x68/4] |= 1U << 11; }
    if (reg == 0 && (value & 0x200)) {
        transfers++;
        bool read = (value & 0x40) != 0;
        unsigned int count = ((value >> 1) & 7) + 1;
        uint32_t data = regs[0x0c / 4];
        assert(regs[0x04 / 4] == (0x3cU << 1) + (read ? 1U : 0U));
        regs[0x1c / 4] = 0;
        if (nack_first || need_bus_recovery || (nack_latch && !read && count == 1 && data == 0x0b)) { regs[0x1c / 4] = 1; return; }
        if (read) {
            assert(count == 1 && pointer < sizeof(pmic) / sizeof(pmic[0]));
            regs[0x0c / 4] = pmic[pointer];
            if (pointer == 0x05 || pointer == 0x0b) {
                pmic[pointer] = 0; cleared_irqs++;
            }
        } else if (count == 1) {
            assert(data == 0x42 || data == 0x41 || data == 0x05 || data == 0x0b);
            pointer = data;
        } else {
            assert(count == 2);
            if ((data & 0xff) == 0x42) {
                pmic[0x42] = data >> 8;
                if (mismatch) pmic[0x42] ^= 0x80;
            } else {
                assert((data & 0xff) == 0x41);
                assert((data >> 8) == (pmic[0x41] | 0x80));
                assert(cleared_irqs >= (nack_latch ? 1U : 2U));
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
    memset(pmc, 0, sizeof(pmc));
    memset(pmic, 0, sizeof(pmic));
    memset(&wire, 0, sizeof(wire));
    pmic[0x42] = config;
    pmic[0x41] = 0x4e; /* keep long-press and reserved configuration */
    pmic[0x05] = 0xa0; pmic[0x0b] = 0x03;
    pointer = reset_bits = clocks = power_requests = delay_ms = 0;
    transfers = cleared_irqs = 0;
    nack_first = nack_final = mismatch = nack_latch = need_bus_recovery = clear_failed = false;
    recoveries = 0;
}

static void check_reset(unsigned int alarm1, unsigned int alarm2)
{
    const unsigned int rtc[][2] = {
        {0x13,alarm1}, {0x1a,alarm2}, {0x11,0x77}, {0x18,0x77}, {0x04,1}
    };
    static const unsigned int rails[][2] = {{0x16,0x20}, {0x42,0x99}, {0x41,0x80}};
    reset_mock(0x9f);
    pmc[0] = 0x800500;
    pmc[0x50 / 4] = 0xc0000122;
    assert(tegra_lite_prepare_reset() == 0);
    assert(pmc[0] == 0x810800);
    assert(pmc[0x3c / 4] == 0x6c00 && pmc[0xe0 / 4] == 0x5a8);
    assert(pmc[0x50 / 4] == 0x20000120);
    assert(transfers == 0); /* SD0 must only be written later by BootROM. */
    const uint32_t *b = &pmc[0x908 / 4];
    assert(b[0] == (2 | (10 << 3) | (2 << 8) | (10 << 11)));
    unsigned int cursor = 1;
    for (unsigned int block = 0; block < 2; block++) {
        const unsigned int (*commands)[2] = block ? rails : rtc;
        unsigned int count = block ? 3 : 5, words = 1 + (count + 1) / 2, sum = 0;
        assert((b[cursor] & 0xff) == (block ? 0x3c : 0x68));
        assert(((b[cursor] >> 8) & 0xff) == count);
        assert((b[cursor] >> 24) == 0x80); /* controller reset, byte operands */
        for (unsigned int i = 0; i < words; i++)
            for (unsigned int j = 0; j < 4; j++) sum += (b[cursor+i] >> (j*8)) & 0xff;
        assert((sum & 0xff) == 0);
        for (unsigned int i = 0; i < count; i++) {
            unsigned int pair = (b[cursor+1+i/2] >> ((i%2)*16)) & 0xffff;
            assert((pair & 0xff) == commands[i][0] && (pair >> 8) == commands[i][1]);
        }
        cursor += words;
    }
    assert(cursor == 8);
}

int main(void)
{
    check_reset(2, 0); /* no Hekate handoff */
    const char extra_entry[] = "autoboot=2\nautoboot_list=0\ncpu_oc=1996800\n"
                               "autoboot=7\nautoboot_list=1\nloader_rev=7\n";
    tegra_lite_capture_reboot_target(extra_entry, sizeof(extra_entry));
    check_reset(0x31, 5); /* SELF, additional-INI entry 7 */
    const char main_entry[] = "autoboot=12\nautoboot_list=0\nloader_rev=7\n";
    tegra_lite_capture_reboot_target(main_entry, sizeof(main_entry));
    check_reset(1, 3); /* SELF, main-INI entry 12 */
    const char invalid[] = "autoboot=16\nautoboot_list=1\n";
    tegra_lite_capture_reboot_target(invalid, sizeof(invalid));
    check_reset(2, 0);
    const char missing_list[] = "autoboot=7\n";
    tegra_lite_capture_reboot_target(missing_list, sizeof(missing_list));
    check_reset(2, 0);
    tegra_lite_capture_reboot_target(extra_entry, sizeof(extra_entry) - 1);
    check_reset(2, 0);
    reset_mock(0x9f);
    assert(tegra_lite_pmic_off() == -5);
    assert(pmic[0x42] == 0x1f && power_requests == 2 && delay_ms == 1000);
    reset_mock(0x19); nack_first = true;
    assert(tegra_lite_pmic_off() == -2 && power_requests == 0);
    reset_mock(0x19); mismatch = true;
    assert(tegra_lite_pmic_off() == -3 && power_requests == 0);
    reset_mock(0x19); nack_final = true;
    assert(tegra_lite_pmic_off() == -4 && power_requests == 2 && delay_ms == 1000);
    reset_mock(0x9f); nack_latch = true;
    assert(tegra_lite_pmic_off() == -5 && power_requests == 2);
    reset_mock(0x9f); need_bus_recovery = true;
    assert(tegra_lite_pmic_off() == -5 && recoveries == 1 && power_requests == 1);
    reset_mock(0x9f); need_bus_recovery = clear_failed = true;
    assert(tegra_lite_pmic_off() == -6 && power_requests == 0);
    reset_mock(0x9f); wire.connected = true;
    wire.config = 0x81; wire.oe = 0x54; wire.out = 0xbc;
    gpio_power_off();
    assert(wire.reads == 5 && wire.power_writes == 1 && transfers == 0);
    assert(wire.starts == 12 && wire.stops == 7 && pmic[0x42] == 0x1f);
    assert(pmic[0x05] == 0 && pmic[0x0b] == 0);
    assert(wire.config == 0x81 && wire.oe == 0x54 && wire.out == 0xbc);
    reset_mock(0x9f); wire.connected = true; wire.nack_address = true;
    gpio_power_off();
    assert(wire.reads == 0 && wire.power_writes == 0 && wire.stops == 3);
    reset_mock(0x9f); wire.connected = true; mismatch = true;
    gpio_power_off();
    assert(wire.reads == 4 && wire.power_writes == 0 && pmic[0x42] == 0x9f);
    reset_mock(0x9f); wire.connected = true; wire.force_scl_low = true;
    gpio_power_off();
    assert(!wire.oe && !wire.config && wire.power_writes == 0);
    puts("Switchroot B01 BootROM restart selector, controller recovery and GPIO PMIC shutdown passed (simulated)");
    return 0;
}
