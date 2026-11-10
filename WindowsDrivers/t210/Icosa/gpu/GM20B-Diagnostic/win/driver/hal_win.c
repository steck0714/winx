/* SPDX-License-Identifier: MIT */
/*
 * Windows implementation of struct gm20b_hal.
 *  - MMIO: aligned volatile 32-bit accesses through MmMapIoSpaceEx mappings.  Nothing else ever touches device memory.
 *    Every access is followed by a full barrier (dsb sy), so the intent line that precedes a risky access is
 *    guaranteed to be on its way to disk before the access is issued and the access is complete before we go on.
 *  - DMA: MmAllocateContiguousMemorySpecifyCache(MmNonCached), zeroed with 32-bit stores: non-cached memory on ARM64
 *    is Device memory, so wider or unaligned accesses (memset/memcpy!) can fault.
 *  - udelay: KeStallExecutionProcessor in chunks of <= 50 us; msleep: KeDelayExecutionThread (PASSIVE_LEVEL only).
 */
#include "win_priv.h"

static volatile u32 *mmio_ptr(struct win_dev *d, enum gm20b_space sp, u32 off)
{
	u32 why = 0;

	if ((u32)sp >= (u32)GM20B_SP_COUNT)
		why = 1;                                  /* bad space */
	else if (!d->map[sp])
		why = 2;                                  /* not mapped */
	else if (off & 3u)
		why = 3;                                  /* unaligned */
	else if (off > d->map_size[sp] - 4u)
		why = 4;                                  /* out of range */
	if (why) {
		win_bad_access(d, sp, off, why);
		return NULL;
	}
	return (volatile u32 *)((u8 *)d->map[sp] + off);
}

static u32 hal_rd32(void *ctx, enum gm20b_space sp, u32 off)
{
	volatile u32 *p = mmio_ptr((struct win_dev *)ctx, sp, off);
	u32 v;

	if (!p)
		return 0xffffffffu;
	v = *p;
	GM20B_MB();
	return v;
}

static void hal_wr32(void *ctx, enum gm20b_space sp, u32 off, u32 val)
{
	volatile u32 *p = mmio_ptr((struct win_dev *)ctx, sp, off);

	if (!p)
		return;
	*p = val;
	GM20B_MB();
}

static void hal_udelay(void *ctx, u32 us)
{
	(void)ctx;
	while (us) {
		u32 n = us > 50u ? 50u : us;

		KeStallExecutionProcessor(n);
		us -= n;
	}
}

static void hal_msleep(void *ctx, u32 ms)
{
	LARGE_INTEGER t;

	(void)ctx;
	t.QuadPart = -(LONGLONG)ms * 10000;               /* relative, 100 ns units */
	(void)KeDelayExecutionThread(KernelMode, FALSE, &t);
}

static u64 hal_now_ns(void *ctx)
{
	(void)ctx;
	return win_now_ns();
}

static int hal_dma_alloc(void *ctx, struct gm20b_dma *dm, size_t size, size_t align)
{
	PHYSICAL_ADDRESS lo, hi, bound, pa;
	size_t total, adj = 0, i, zlen;
	u8 *base, *cpu;
	u64 phys;

	(void)ctx;
	if (!size)
		return GM20B_E_INVAL;
	if (align < 4096u)
		align = 4096u;                            /* the allocator returns page-aligned memory */
	total = (size + 4095u) & ~(size_t)4095u;
	if (align > 4096u)
		total += align;                           /* over-allocate, align by hand */
	lo.QuadPart = 0;
	bound.QuadPart = 0;
	hi.QuadPart = 0xFFFFFFFFFFll;                     /* 40 bits */
	base = (u8 *)MmAllocateContiguousMemorySpecifyCache(total, lo, hi, bound, MmNonCached);
	if (!base)
		return GM20B_E_NOMEM;
	pa = MmGetPhysicalAddress(base);
	phys = (u64)pa.QuadPart;
	if (align > 4096u)
		adj = (size_t)((align - (phys & (align - 1u))) & (align - 1u));
	cpu = base + adj;
	phys += adj;
	zlen = (size + 3u) & ~(size_t)3u;
	for (i = 0; i < zlen; i += 4u)
		*(volatile u32 *)(cpu + i) = 0;
	GM20B_MB();
	dm->cpu = cpu;
	dm->phys = phys;
	dm->size = size;
	dm->os = base;
	return GM20B_OK;
}

static void hal_dma_free(void *ctx, struct gm20b_dma *dm)
{
	(void)ctx;
	if (dm->os)
		MmFreeContiguousMemory(dm->os);
}

/* S7+ will read firmware from disk; not implemented yet: the core reports GM20B_E_FW. */
static int hal_fw_get(void *ctx, const char *name, struct gm20b_fw *fw)
{
	(void)ctx;
	(void)name;
	(void)fw;
	return GM20B_E_FW;
}

static void hal_fw_put(void *ctx, struct gm20b_fw *fw)
{
	(void)ctx;
	(void)fw;
}

static void hal_journal_flush(void *ctx, const struct gm20b_jentry *e, u32 count)
{
	win_jf_commit((struct win_dev *)ctx, e, count);
}

void win_hal_fill(struct win_dev *d, struct gm20b_hal *h)
{
	gm20b_zero(h, sizeof(*h));
	h->ctx = d;
	h->rd32 = hal_rd32;
	h->wr32 = hal_wr32;
	h->udelay = hal_udelay;
	h->msleep = hal_msleep;
	h->now_ns = hal_now_ns;
	h->dma_alloc = hal_dma_alloc;
	h->dma_free = hal_dma_free;
	h->fw_get = hal_fw_get;
	h->fw_put = hal_fw_put;
	h->journal_flush = hal_journal_flush;
	h->rail_gpu_set_uv = NULL;                        /* the shell never touches the PMIC */
}

int win_map_all(struct win_dev *d)
{
	u32 i;
	int bad = 0;

	for (i = 0; i < (u32)GM20B_SP_COUNT; i++) {
		PHYSICAL_ADDRESS pa;

		pa.QuadPart = (LONGLONG)gm20b_space_pa((enum gm20b_space)i);
		d->map_size[i] = gm20b_space_size((enum gm20b_space)i);
		d->map[i] = MmMapIoSpaceEx(pa, d->map_size[i], PAGE_READWRITE | PAGE_NOCACHE);
		win_rec(d, J_WIN_MAP, i, (u32)(ULONG_PTR)d->map[i]);
		if (!d->map[i])
			bad++;
	}
	d->map_ok = !bad;
	return bad ? GM20B_E_NOMEM : GM20B_OK;
}

void win_unmap_all(struct win_dev *d)
{
	u32 i;

	for (i = 0; i < (u32)GM20B_SP_COUNT; i++) {
		if (d->map[i])
			MmUnmapIoSpace(d->map[i], d->map_size[i]);
		d->map[i] = NULL;
	}
	d->map_ok = 0;
}
