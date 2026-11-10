/* SPDX-License-Identifier: MIT */
/*
 * Platform abstraction (HAL). The Windows driver and the host mock each fill in
 * one of these. The core never touches memory-mapped hardware any other way.
 *
 * Rules the HAL implementation must honour:
 *  - rd32/wr32: exactly one aligned 32-bit device access per call (no
 *    memcpy, no wider/narrower accesses). `off` is always 4-byte aligned.
 *  - Device accesses are ordered; wr32 is followed by a barrier that makes it
 *    visible to the device before the next access to ANY space.
 *  - udelay: busy-wait, <= 1000 us, callable at any IRQL the core runs at.
 *  - msleep: may block (PASSIVE_LEVEL only).
 *  - dma_alloc: zeroed, physically contiguous, UNCACHED (or otherwise coherent
 *    with the GPU), 'align' is a power of two. The GPU is assumed to use the
 *    CPU physical address directly (SMMU bypass) -- see docs/DESIGN.md.
 *  - journal_flush: must persist the entries durably before returning (the
 *    machine may hang on the very next MMIO access).
 */
#ifndef GM20B_HAL_H
#define GM20B_HAL_H

#include "gm20b_types.h"

/* Register spaces. Physical bases are SoC constants (tegra210.dtsi / TRM). */
enum gm20b_space {
	GM20B_SP_BAR0  = 0, /* GPU registers      0x57000000  16 MiB */
	GM20B_SP_BAR1  = 1, /* GPU BAR1 aperture  0x58000000  16 MiB */
	GM20B_SP_CAR   = 2, /* Clock & Reset      0x60006000   4 KiB (SHARED with ACPI SWOA0701 'CPUC') */
	GM20B_SP_PMC   = 3, /* APBDEV_PMC         0x7000e400   1 KiB */
	GM20B_SP_MC    = 4, /* Memory controller  0x70019000   4 KiB (diagnostics: read only!) */
	GM20B_SP_COUNT
};

#define GM20B_PA_BAR0   0x57000000ull
#define GM20B_SZ_BAR0   0x01000000u
#define GM20B_PA_BAR1   0x58000000ull
#define GM20B_SZ_BAR1   0x01000000u
#define GM20B_PA_CAR    0x60006000ull
#define GM20B_SZ_CAR    0x00001000u
#define GM20B_PA_PMC    0x7000e400ull
#define GM20B_SZ_PMC    0x00000400u
#define GM20B_PA_MC     0x70019000ull
#define GM20B_SZ_MC     0x00001000u

/* GIC SPI numbers from tegra210.dtsi; Windows/ACPI GSIV = SPI + 32. */
#define GM20B_SPI_STALL     157u
#define GM20B_SPI_NONSTALL  158u

static inline u64 gm20b_space_pa(enum gm20b_space sp)
{
	switch (sp) {
	case GM20B_SP_BAR0: return GM20B_PA_BAR0;
	case GM20B_SP_BAR1: return GM20B_PA_BAR1;
	case GM20B_SP_CAR:  return GM20B_PA_CAR;
	case GM20B_SP_PMC:  return GM20B_PA_PMC;
	case GM20B_SP_MC:   return GM20B_PA_MC;
	default:            return 0;
	}
}

static inline u32 gm20b_space_size(enum gm20b_space sp)
{
	switch (sp) {
	case GM20B_SP_BAR0: return GM20B_SZ_BAR0;
	case GM20B_SP_BAR1: return GM20B_SZ_BAR1;
	case GM20B_SP_CAR:  return GM20B_SZ_CAR;
	case GM20B_SP_PMC:  return GM20B_SZ_PMC;
	case GM20B_SP_MC:   return GM20B_SZ_MC;
	default:            return 0;
	}
}

/* One journal record (24 bytes). */
struct gm20b_jentry {
	u32 seq;
	u32 code;   /* enum gm20b_jcode */
	u32 a;
	u32 b;
	u64 t_ns;   /* hal.now_ns() at record time */
};

/* A physically contiguous, GPU-visible buffer. */
struct gm20b_dma {
	void  *cpu;     /* kernel virtual address (uncached) */
	u64    phys;    /* physical address == GPU-visible address (SMMU bypass) */
	size_t size;
	void  *os;      /* platform-private handle */
};

/* A firmware blob provided by the platform (read from disk by the shell). */
struct gm20b_fw {
	const void *data;
	size_t      size;
	void       *os;
};

struct gm20b_hal {
	void *ctx;

	u32  (*rd32)(void *ctx, enum gm20b_space sp, u32 off);
	void (*wr32)(void *ctx, enum gm20b_space sp, u32 off, u32 val);

	void (*udelay)(void *ctx, u32 us);
	void (*msleep)(void *ctx, u32 ms);
	u64  (*now_ns)(void *ctx);

	int  (*dma_alloc)(void *ctx, struct gm20b_dma *d, size_t size, size_t align);
	void (*dma_free)(void *ctx, struct gm20b_dma *d);

	/* name is e.g. "nvidia/gm20b/acr_ucode.bin" (linux-firmware layout). */
	int  (*fw_get)(void *ctx, const char *name, struct gm20b_fw *fw);
	void (*fw_put)(void *ctx, struct gm20b_fw *fw);

	/* Persist entries before returning (see header comment). May be NULL. */
	void (*journal_flush)(void *ctx, const struct gm20b_jentry *e, u32 count);

	/*
	 * OPTIONAL platform service: set the GPU rail (MAX77621) in microvolts.
	 * NULL in the default build. Only called when GM20B_CF_ALLOW_RAIL is set.
	 */
	int  (*rail_gpu_set_uv)(void *ctx, u32 uv);
};

#endif /* GM20B_HAL_H */
