// SPDX-License-Identifier: GPL-2.0-only
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <assert.h>

/* Include the production adapter and the real I2cLib declarations. */
typedef uint8_t UINT8;
typedef uint32_t UINT32;
typedef void VOID;
#include "../../../Library/PlatformBootManagerLib/LiteGpuI2c.h"

static unsigned int packet_calls, writes, rejected_empty_reads;
static UINT32 last_address, last_register;
static UINT8 last_value;
static bool bus_ok = true;

int i2c_xfer_packet(UINT32 idx, UINT32 address, const UINT8 *tx, UINT32 tx_size,
                    UINT8 *rx, UINT32 rx_size)
{
    /* The production I2cLib rejects rx_size == 0 before touching MMIO. */
    if (rx_size == 0) { ++rejected_empty_reads; return 0; }
    assert(idx == I2C_5 && address == 0x33);
    assert(tx && tx_size == 1 && rx && rx_size == 1);
    ++packet_calls;
    if (!bus_ok) return 0;
    *rx = 0x50;
    return 1;
}

UINT32 i2c_send_byte(UINT32 idx, UINT32 address, UINT32 reg, UINT8 value)
{
    assert(idx == I2C_5);
    ++writes;
    last_address = address;
    last_register = reg;
    last_value = value;
    return bus_ok ? 1 : 0;
}

int main(void)
{
    UINT8 value = 0;
    assert(GpuReadPmic(NULL, 0x33, 0x23, &value) && value == 0x50);
    if (!GpuWritePmic(NULL, 0x33, 0x23, 0x6e)) {
        fputs("GPU PMIC adapter used the wrong write transport.\n", stderr);
        return 1;
    }
    assert(writes == 1 && packet_calls == 1 && rejected_empty_reads == 0);
    assert(last_address == 0x33 && last_register == 0x23 && last_value == 0x6e);
    assert(GpuWritePmic(NULL, 0x33, 0x06, 0x41));
    assert(last_register == 0x06 && last_value == 0x41);
    bus_ok = false;
    assert(!GpuWritePmic(NULL, 0x33, 0x23, 0x50));
    assert(!GpuReadPmic(NULL, 0x33, 0x23, &value));
    puts("GPU PMIC adapter contract passed (host; no hardware I/O).");
    return 0;
}
