/* SPDX-License-Identifier: MIT */
/*
 * Host-side mock of the GM20B + its surroundings, used to test the core.
 *
 * What it gives a stage's tests:
 *  - sparse register files for every gm20b_space (default 0), preset/inspect;
 *  - per-range read/write hooks to model device behaviour (status bits that
 *    follow control bits, write-1-to-clear, PLL lock after N us, ...);
 *  - "gating": a space can be declared dead; any access then records a
 *    VIOLATION and reads 0xffffffff (this is how "touched BAR0 before the GPU
 *    was powered" is caught -- on real hardware that is a bus hang);
 *  - alignment / range checking of every access (violations too);
 *  - a full ordered access trace with virtual timestamps and the number of
 *    journal entries already flushed at that moment, so tests can assert
 *    ORDER ("CAR reset-deassert happened after the clamp was removed and after
 *    the intent was flushed to the journal");
 *  - virtual time (udelay/msleep advance it; nothing really sleeps);
 *  - DMA allocator with fake physical addresses, failure injection, leak count;
 *  - firmware blobs, and a record of every flushed journal entry.
 */
#ifndef SIM_MOCK_H
#define SIM_MOCK_H

#include "gm20b.h"

struct mock;

struct mock_acc {
	u32 idx;
	u8  space;
	u8  is_wr;
	u8  pad[2];
	u32 off;
	u32 val;     /* value written, or value returned by the read */
	u64 t_ns;
	u32 jlen;    /* journal entries already flushed when this access happened */
};

typedef u32  (*mock_rd_fn)(struct mock *m, void *user, enum gm20b_space sp, u32 off, u32 cur);
/* The written value is stored BEFORE the hook runs; 'old' is the previous value.
 * A hook may override the stored value with mock_set(). */
typedef void (*mock_wr_fn)(struct mock *m, void *user, enum gm20b_space sp, u32 off, u32 val, u32 old);

struct mock *mock_new(void);
void         mock_free(struct mock *m);
/* Fill 'out' with a HAL wired to this mock (all functions populated). */
void         mock_hal(struct mock *m, struct gm20b_hal *out);

/* registers */
void mock_set(struct mock *m, enum gm20b_space sp, u32 off, u32 v);
u32  mock_get(struct mock *m, enum gm20b_space sp, u32 off);
/* inclusive [lo, hi]; first matching hook wins; rd/wr may be NULL */
void mock_hook(struct mock *m, enum gm20b_space sp, u32 lo, u32 hi,
	       mock_rd_fn rd, mock_wr_fn wr, void *user);
void mock_space_alive(struct mock *m, enum gm20b_space sp, bool alive);
bool mock_is_alive(const struct mock *m, enum gm20b_space sp);

/* violations (first 64 kept) */
void        mock_violation(struct mock *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
u32         mock_nviolations(const struct mock *m);
const char *mock_violation_msg(const struct mock *m, u32 i);

/* access trace */
u32                   mock_ntrace(const struct mock *m);
const struct mock_acc *mock_trace(const struct mock *m, u32 i);
/* first index >= from matching (space, off, is_wr), or -1 */
int mock_find(const struct mock *m, enum gm20b_space sp, u32 off, int is_wr, u32 from);
/* same, additionally requiring val == (acc.val & mask) */
int mock_find_val(const struct mock *m, enum gm20b_space sp, u32 off, int is_wr,
		  u32 mask, u32 val, u32 from);
void mock_trace_clear(struct mock *m);

/* virtual time */
u64  mock_now(const struct mock *m);
void mock_advance(struct mock *m, u64 ns);

/* DMA */
void *mock_phys_to_ptr(struct mock *m, u64 phys);       /* NULL if unmapped */
u32   mock_dma_live(const struct mock *m);               /* allocations not yet freed */
void  mock_dma_fail_after(struct mock *m, int n);        /* n more succeed, then fail; -1 = never fail */

/* firmware */
void mock_fw_add(struct mock *m, const char *name, const void *data, size_t size);

/* journal sink */
u32                        mock_jflushes(const struct mock *m);
u32                        mock_jlen(const struct mock *m);
const struct gm20b_jentry *mock_jlog(const struct mock *m);
int                        mock_jfind(const struct mock *m, u32 code, u32 a, u32 from); /* a==0xffffffff: any */

#endif /* SIM_MOCK_H */
