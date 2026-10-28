/*
* Copyright (c) 2018 naehrwert
*
* This program is free software; you can redistribute it and/or modify it
* under the terms and conditions of the GNU General Public License,
* version 2, as published by the Free Software Foundation.
*
* This program is distributed in the hope it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
* more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <PiDxe.h>

#include <Library/ArmLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>

#include <Foundation/Types.h>
#include <Library/I2cLib.h>
#include <Library/EarlyTimerLib.h>

static u32 i2c_addrs[] = { 0x7000C000, 0x7000C400, 0x7000C500, 0x7000C700, 0x7000D000, 0x7000D100 };

static void _i2c_wait(vu32 *base)
{
	base[0x23] = 0x25;
	for (u32 i = 0; i < 20; i++)
	{
		sleep(1);
		if (!(base[0x23] & 1))
			break;
	}
}

static int _i2c_send_pkt(u32 idx, u32 x, u8 *buf, u32 size)
{
	if (size > 4)
		return 0;

	u32 tmp = 0;
	memcpy(&tmp, buf, size);

	vu32 *base = (vu32 *)i2c_addrs[idx];
	base[1] = x << 1; //Set x (send mode).
	base[3] = tmp; //Set value.
	base[0] = (2 * size - 2) | 0x2800; //Set size and send mode.
	_i2c_wait(base); //Kick transaction.

	base[0] = (base[0] & 0xFFFFFDFF) | 0x200;
	while (base[7] & 0x100)
		;

	if (base[7] << 28)
		return 0;

	return 1;
}

static int _i2c_recv_pkt(u32 idx, u8 *buf, u32 size, u32 x)
{
	if (size > 4)
		return 0;

	vu32 *base = (vu32 *)i2c_addrs[idx];
	base[1] = (x << 1) | 1; //Set x (recv mode).
	base[0] = (2 * size - 2) | 0x2840; //Set size and recv mode.
	_i2c_wait(base); //Kick transaction.

	base[0] = (base[0] & 0xFFFFFDFF) | 0x200;
	while (base[7] & 0x100)
		;

	if (base[7] << 28)
		return 0;

	u32 tmp = base[3]; //Get value.
	memcpy(buf, &tmp, size);

	return 1;
}

void i2c_init(UINT32 idx)
{
	vu32 *base = (vu32 *)i2c_addrs[idx];

	base[0x1B] = 0x50001;
	base[0x21] = 0x90003;
	_i2c_wait(base);

	for (u32 i = 0; i < 10; i++)
	{
		sleep(20000);
		if (base[0x1A] & 0x800)
			break;
	}

	(void)base[0x22];
	base[0x1A] = base[0x1A];
}

UINT32 i2c_send_buf_small(UINT32 idx, UINT32 x, UINT32 y, UINT8 *buf, UINT32 size)
{
	u8 tmp[4];

	if (size > 3)
		return 0;

	tmp[0] = y;
	memcpy(tmp + 1, buf, size);

	return _i2c_send_pkt(idx, x, tmp, size + 1);
}

int i2c_recv_buf_small(UINT8 *buf, UINT32 size, UINT32 idx, UINT32 x, UINT32 y)
{
	int res = _i2c_send_pkt(idx, x, (u8 *)&y, 1);
	if (res)
		res = _i2c_recv_pkt(idx, buf, size, x);
	return res;
}

UINT32 i2c_send_byte(UINT32 idx, UINT32 x, UINT32 y, UINT8 b)
{
	return i2c_send_buf_small(idx, x, y, &b, 1);
}

UINT8 i2c_recv_byte(UINT32 idx, UINT32 x, UINT32 y)
{
	u8 tmp;
	i2c_recv_buf_small(&tmp, 1, idx, x, y);
	return tmp;
}

/*
 * Packet-mode transfer (ported from hekate bdk/soc/i2c.c, GPL-2.0): write tx_buf, repeated START,
 * read rx_size bytes. Needed by devices that answer with more than 4 bytes (e.g. FTS4 touch).
 * Like the rest of this file: returns 1 on success, 0 on failure.
 */
#define I2CP_CNFG             (0x00 / 4)
#define  I2CP_CMD1_READ       (1 << 6)
#define  I2CP_NORMAL_GO       (1 << 9)
#define  I2CP_PACKET_GO       (1 << 10)
#define  I2CP_NEW_MASTER_FSM  (1 << 11)
#define  I2CP_DEBOUNCE_4T     (2 << 12)
#define I2CP_CMD_ADDR0        (0x04 / 4)
#define I2CP_STATUS           (0x1C / 4)
#define  I2CP_STATUS_NOACK    (0xF << 0)
#define I2CP_TX_FIFO          (0x50 / 4)
#define I2CP_RX_FIFO          (0x54 / 4)
#define I2CP_PKT_STATUS       (0x58 / 4)
#define I2CP_FIFO_CONTROL     (0x5C / 4)
#define  I2CP_RX_FLUSH        (1 << 0)
#define  I2CP_TX_FLUSH        (1 << 1)
#define I2CP_FIFO_STATUS      (0x60 / 4)
#define  I2CP_RX_FULL_CNT     (0xF << 0)
#define I2CP_PROT_I2C         (1 << 4)
#define I2CP_HDR_REP_START    (1 << 16)
#define I2CP_HDR_READ         (1 << 19)
#define I2CP_TIMEOUT_US       200000

int i2c_xfer_packet(UINT32 idx, UINT32 dev_addr, const UINT8 *tx_buf, UINT32 tx_size, UINT8 *rx_buf, UINT32 rx_size)
{
	vu32 *base = (vu32 *)i2c_addrs[idx];
	int ok = 1;
	u32 start, len, word, tx_rem;

	if (tx_size == 0 || tx_size > 20 || rx_size == 0 || rx_size > 32)
		return 0;

	base[I2CP_CMD_ADDR0] = (dev_addr << 1) | 1;
	base[I2CP_CNFG] = I2CP_DEBOUNCE_4T | I2CP_NEW_MASTER_FSM | I2CP_CMD1_READ;
	base[I2CP_FIFO_CONTROL] = I2CP_RX_FLUSH | I2CP_TX_FLUSH;
	_i2c_wait(base);

	base[I2CP_CNFG] = (base[I2CP_CNFG] & ~I2CP_NORMAL_GO) | I2CP_PACKET_GO;

	// Write request header + payload.
	base[I2CP_TX_FIFO] = I2CP_PROT_I2C;
	base[I2CP_TX_FIFO] = tx_size - 1;
	base[I2CP_TX_FIFO] = I2CP_HDR_REP_START | (dev_addr << 1);

	tx_rem = tx_size;
	while (tx_rem)
	{
		len = tx_rem < 4 ? tx_rem : 4;
		word = 0;
		memcpy(&word, tx_buf, len);
		base[I2CP_TX_FIFO] = word;
		tx_buf += len;
		tx_rem -= len;
	}

	start = get_tmr_us();
	while (((base[I2CP_PKT_STATUS] >> 4) & 0xFFF) != (tx_size - 1))
	{
		if (get_tmr_us() - start > I2CP_TIMEOUT_US)
		{
			ok = 0;
			goto out;
		}
	}

	// Read request header.
	base[I2CP_TX_FIFO] = I2CP_PROT_I2C;
	base[I2CP_TX_FIFO] = rx_size - 1;
	base[I2CP_TX_FIFO] = I2CP_HDR_READ | (dev_addr << 1);

	start = get_tmr_us();
	while (rx_size)
	{
		if (base[I2CP_FIFO_STATUS] & I2CP_RX_FULL_CNT)
		{
			len = rx_size < 4 ? rx_size : 4;
			word = base[I2CP_RX_FIFO];
			memcpy(rx_buf, &word, len);
			rx_buf += len;
			rx_size -= len;
		}
		if (get_tmr_us() - start > I2CP_TIMEOUT_US)
		{
			ok = 0;
			break;
		}
	}

out:
	if (base[I2CP_STATUS] & I2CP_STATUS_NOACK)
		ok = 0;

	sleep(20);
	base[I2CP_CNFG] &= ~(I2CP_PACKET_GO | I2CP_NORMAL_GO);
	return ok;
}

int i2c_recv_buf_big(UINT8 *buf, UINT32 size, UINT32 idx, UINT32 dev_addr, UINT32 reg)
{
	UINT8 r = (UINT8)reg;
	return i2c_xfer_packet(idx, dev_addr, &r, 1, buf, size);
}
