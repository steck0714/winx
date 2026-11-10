/* SPDX-License-Identifier: MIT */
/*
 * winx-gm20b: Tegra X1 (GM20B) GPU bring-up core -- basic types.
 *
 * This core is freestanding C11: it may only include <stdint.h>, <stddef.h>,
 * <stdbool.h> and <stdarg.h>. No libc calls. All hardware access goes through
 * the HAL in gm20b_hal.h so the same code runs in the Windows kernel driver and
 * against the host-side mock in sim/.
 */
#ifndef GM20B_TYPES_H
#define GM20B_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;
typedef int64_t  s64;

/* Status codes: 0 = success, negative = failure. Never positive. */
enum gm20b_status {
	GM20B_OK        =  0,
	GM20B_E_TIMEOUT = -1,  /* a bounded wait expired */
	GM20B_E_IO      = -2,  /* bus-fault pattern: 0xffffffff / 0xbadfXXXX read */
	GM20B_E_NOMEM   = -3,
	GM20B_E_NODEV   = -4,  /* identity mismatch: this is not a GM20B */
	GM20B_E_NOTIMPL = -5,
	GM20B_E_INVAL   = -6,
	GM20B_E_FW      = -7,  /* firmware missing / malformed */
	GM20B_E_GATED   = -8,  /* not run: cfg.max_stage gates it off */
	GM20B_E_STATE   = -9,  /* called in the wrong state */
	GM20B_E_HW      = -10, /* hardware reported an error condition */
};

#define GM20B_BIT(n)            (1u << (n))
#define GM20B_BIT64(n)          (1ull << (n))
#define GM20B_ARRAY_SIZE(a)     (sizeof(a) / sizeof((a)[0]))
#define GM20B_ALIGN_UP(x, a)    (((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))

/* Priv-ring returns 0xbadfXXXX for dead/unpowered units. */
#define GM20B_IS_BADF(v)        (((v) & 0xffff0000u) == 0xbadf0000u)
#define GM20B_IS_DEAD(v)        ((v) == 0xffffffffu || GM20B_IS_BADF(v))

#endif /* GM20B_TYPES_H */
