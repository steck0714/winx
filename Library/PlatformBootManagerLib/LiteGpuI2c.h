// SPDX-License-Identifier: GPL-2.0-only
#ifndef LITE_GPU_I2C_H
#define LITE_GPU_I2C_H

#include <stdbool.h>
#include <stdint.h>
#include <Library/I2cLib.h>

static bool GpuReadPmic (void *Context, uint8_t Address, uint8_t Reg, uint8_t *Value)
{
  (VOID)Context;
  return i2c_xfer_packet (I2C_5, Address, &Reg, 1, Value, 1) != 0;
}

static bool GpuWritePmic (void *Context, uint8_t Address, uint8_t Reg, uint8_t Value)
{
  (VOID)Context;
  // I2cLib packet transfers require a nonempty receive phase. A PMIC register
  // write is a standalone two-byte transaction through the small-write API.
  return i2c_send_byte (I2C_5, Address, Reg, Value) != 0;
}

#endif
