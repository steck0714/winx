/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "mock.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOCK_MAX_HOOKS 64
#define MOCK_MAX_VIOL  64
#define MOCK_MAX_DMA   1024
#define MOCK_MAX_FW    32

struct mock_kv {
	u64  key;
	u32  val;
	bool used;
};

struct mock_hook_rec {
	enum gm20b_space sp;
	u32 lo, hi;
	mock_rd_fn rd;
	mock_wr_fn wr;
	void *user;
};

struct mock_dma_rec {
	void  *p;
	u64    phys;
	size_t size;
	bool   live;
};

struct mock_fw_rec {
	char        name[96];
	const void *data;
	size_t      size;
};

struct mock {
	struct mock_kv *tab;
	u32 cap, count;

	struct mock_hook_rec hooks[MOCK_MAX_HOOKS];
	u32 nhooks;

	struct mock_acc *trace;
	u32 ntrace, captrace;

	bool alive[GM20B_SP_COUNT];

	char viol[MOCK_MAX_VIOL][120];
	u32  nviol;       /* kept messages */
	u32  nviol_total; /* all violations */

	u64 now;

	struct mock_dma_rec dma[MOCK_MAX_DMA];
	u32 ndma;
	u64 next_phys;
	int dma_fail_after;

	struct mock_fw_rec fw[MOCK_MAX_FW];
	u32 nfw;

	struct gm20b_jentry *jlog;
	u32 jlen, jcap, jflushes;
};

/* ---- register store ------------------------------------------------------ */
static u32 hsh(u64 k)
{
	k ^= k >> 33;
	k *= 0xff51afd7ed558ccdull;
	k ^= k >> 33;
	return (u32)k;
}

static u64 mkkey(enum gm20b_space sp, u32 off)
{
	return ((u64)sp << 32) | off;
}

static void kv_grow(struct mock *m)
{
	u32 ncap = m->cap ? m->cap * 2 : 1024;
	struct mock_kv *nt = (struct mock_kv *)calloc(ncap, sizeof(*nt));
	u32 i;

	for (i = 0; i < m->cap; i++) {
		if (m->tab[i].used) {
			u32 j = hsh(m->tab[i].key) & (ncap - 1);

			while (nt[j].used)
				j = (j + 1) & (ncap - 1);
			nt[j] = m->tab[i];
		}
	}
	free(m->tab);
	m->tab = nt;
	m->cap = ncap;
}

static struct mock_kv *kv_find(struct mock *m, u64 key, bool create)
{
	u32 i;

	if (create && (m->count + 1) * 2 > m->cap)
		kv_grow(m);
	if (!m->cap)
		return NULL;
	i = hsh(key) & (m->cap - 1);
	for (;;) {
		struct mock_kv *e = &m->tab[i];

		if (!e->used) {
			if (!create)
				return NULL;
			e->used = true;
			e->key = key;
			e->val = 0;
			m->count++;
			return e;
		}
		if (e->key == key)
			return e;
		i = (i + 1) & (m->cap - 1);
	}
}

void mock_set(struct mock *m, enum gm20b_space sp, u32 off, u32 v)
{
	kv_find(m, mkkey(sp, off), true)->val = v;
}

u32 mock_get(struct mock *m, enum gm20b_space sp, u32 off)
{
	struct mock_kv *e = kv_find(m, mkkey(sp, off), false);

	return e ? e->val : 0;
}

/* ---- violations ---------------------------------------------------------- */
void mock_violation(struct mock *m, const char *fmt, ...)
{
	va_list ap;

	m->nviol_total++;
	if (m->nviol >= MOCK_MAX_VIOL)
		return;
	va_start(ap, fmt);
	vsnprintf(m->viol[m->nviol], sizeof(m->viol[0]), fmt, ap);
	va_end(ap);
	m->nviol++;
}

u32 mock_nviolations(const struct mock *m)
{
	return m->nviol_total;
}

const char *mock_violation_msg(const struct mock *m, u32 i)
{
	return i < m->nviol ? m->viol[i] : "";
}

/* ---- trace --------------------------------------------------------------- */
static void trace_add(struct mock *m, enum gm20b_space sp, int is_wr, u32 off, u32 val)
{
	struct mock_acc *a;

	if (m->ntrace == m->captrace) {
		m->captrace = m->captrace ? m->captrace * 2 : 4096;
		m->trace = (struct mock_acc *)realloc(m->trace, m->captrace * sizeof(*m->trace));
	}
	a = &m->trace[m->ntrace];
	memset(a, 0, sizeof(*a));
	a->idx = m->ntrace++;
	a->space = (u8)sp;
	a->is_wr = (u8)is_wr;
	a->off = off;
	a->val = val;
	a->t_ns = m->now;
	a->jlen = m->jlen;
}

u32 mock_ntrace(const struct mock *m)
{
	return m->ntrace;
}

const struct mock_acc *mock_trace(const struct mock *m, u32 i)
{
	return i < m->ntrace ? &m->trace[i] : NULL;
}

int mock_find(const struct mock *m, enum gm20b_space sp, u32 off, int is_wr, u32 from)
{
	u32 i;

	for (i = from; i < m->ntrace; i++)
		if (m->trace[i].space == (u8)sp && m->trace[i].off == off && m->trace[i].is_wr == (u8)!!is_wr)
			return (int)i;
	return -1;
}

int mock_find_val(const struct mock *m, enum gm20b_space sp, u32 off, int is_wr,
		  u32 mask, u32 val, u32 from)
{
	u32 i;

	for (i = from; i < m->ntrace; i++)
		if (m->trace[i].space == (u8)sp && m->trace[i].off == off &&
		    m->trace[i].is_wr == (u8)!!is_wr && (m->trace[i].val & mask) == val)
			return (int)i;
	return -1;
}

void mock_trace_clear(struct mock *m)
{
	m->ntrace = 0;
}

/* ---- hooks / gating ------------------------------------------------------ */
void mock_hook(struct mock *m, enum gm20b_space sp, u32 lo, u32 hi,
	       mock_rd_fn rd, mock_wr_fn wr, void *user)
{
	struct mock_hook_rec *h;

	if (m->nhooks >= MOCK_MAX_HOOKS) {
		fprintf(stderr, "mock: too many hooks\n");
		abort();
	}
	h = &m->hooks[m->nhooks++];
	h->sp = sp;
	h->lo = lo;
	h->hi = hi;
	h->rd = rd;
	h->wr = wr;
	h->user = user;
}

void mock_space_alive(struct mock *m, enum gm20b_space sp, bool alive)
{
	m->alive[sp] = alive;
}

bool mock_is_alive(const struct mock *m, enum gm20b_space sp)
{
	return m->alive[sp];
}

static const struct mock_hook_rec *hook_for(const struct mock *m, enum gm20b_space sp, u32 off)
{
	u32 i;

	for (i = 0; i < m->nhooks; i++)
		if (m->hooks[i].sp == sp && off >= m->hooks[i].lo && off <= m->hooks[i].hi)
			return &m->hooks[i];
	return NULL;
}

/* ---- HAL ----------------------------------------------------------------- */
static bool access_ok(struct mock *m, enum gm20b_space sp, u32 off, const char *what)
{
	if ((unsigned)sp >= GM20B_SP_COUNT) {
		mock_violation(m, "%s: bad space %d", what, (int)sp);
		return false;
	}
	if (off & 3u) {
		mock_violation(m, "%s: UNALIGNED sp=%d off=0x%x", what, (int)sp, off);
		return false;
	}
	if ((u64)off + 4 > gm20b_space_size(sp)) {
		mock_violation(m, "%s: out of range sp=%d off=0x%x", what, (int)sp, off);
		return false;
	}
	if (!m->alive[sp]) {
		mock_violation(m, "%s: access to GATED space sp=%d off=0x%x (would hang the bus)", what, (int)sp, off);
		return false;
	}
	return true;
}

static u32 h_rd32(void *c, enum gm20b_space sp, u32 off)
{
	struct mock *m = (struct mock *)c;
	const struct mock_hook_rec *h;
	u32 v;

	m->now += 50;
	if (!access_ok(m, sp, off, "read")) {
		if ((unsigned)sp < GM20B_SP_COUNT)
			trace_add(m, sp, 0, off, 0xffffffffu);
		return 0xffffffffu;
	}
	v = mock_get(m, sp, off);
	h = hook_for(m, sp, off);
	if (h && h->rd)
		v = h->rd(m, h->user, sp, off, v);
	trace_add(m, sp, 0, off, v);
	return v;
}

static void h_wr32(void *c, enum gm20b_space sp, u32 off, u32 val)
{
	struct mock *m = (struct mock *)c;
	const struct mock_hook_rec *h;
	u32 old;

	m->now += 50;
	if (!access_ok(m, sp, off, "write")) {
		if ((unsigned)sp < GM20B_SP_COUNT)
			trace_add(m, sp, 1, off, val);
		return;
	}
	trace_add(m, sp, 1, off, val);
	old = mock_get(m, sp, off);
	mock_set(m, sp, off, val);
	h = hook_for(m, sp, off);
	if (h && h->wr)
		h->wr(m, h->user, sp, off, val, old);
}

static void h_udelay(void *c, u32 us)
{
	((struct mock *)c)->now += (u64)us * 1000u;
}

static void h_msleep(void *c, u32 ms)
{
	((struct mock *)c)->now += (u64)ms * 1000000u;
}

static u64 h_now(void *c)
{
	return ((struct mock *)c)->now;
}

u64 mock_now(const struct mock *m)
{
	return m->now;
}

void mock_advance(struct mock *m, u64 ns)
{
	m->now += ns;
}

static int h_dma_alloc(void *c, struct gm20b_dma *d, size_t size, size_t align)
{
	struct mock *m = (struct mock *)c;
	struct mock_dma_rec *r;
	void *p = NULL;
	size_t rounded;

	if (m->dma_fail_after == 0)
		return -1;
	if (m->dma_fail_after > 0)
		m->dma_fail_after--;
	if (m->ndma >= MOCK_MAX_DMA)
		return -1;
	if (align < sizeof(void *))
		align = sizeof(void *);
	rounded = (size + align - 1) & ~(align - 1);
	if (posix_memalign(&p, align, rounded))
		return -1;
	memset(p, 0, rounded);
	if (!m->next_phys)
		m->next_phys = 0x80000000ull;
	m->next_phys = (m->next_phys + align - 1) & ~((u64)align - 1);
	r = &m->dma[m->ndma++];
	r->p = p;
	r->phys = m->next_phys;
	r->size = rounded;
	r->live = true;
	m->next_phys += rounded;
	d->cpu = p;
	d->phys = r->phys;
	d->size = size;
	d->os = r;
	return 0;
}

static void h_dma_free(void *c, struct gm20b_dma *d)
{
	struct mock *m = (struct mock *)c;
	struct mock_dma_rec *r = (struct mock_dma_rec *)d->os;

	(void)m;
	if (!r || !r->live) {
		mock_violation(m, "dma_free of unknown/dead buffer");
		return;
	}
	/* keep the memory around (poisoned) so use-after-free in tests is visible, not UB */
	memset(r->p, 0xdd, r->size);
	r->live = false;
}

void *mock_phys_to_ptr(struct mock *m, u64 phys)
{
	u32 i;

	for (i = 0; i < m->ndma; i++)
		if (m->dma[i].live && phys >= m->dma[i].phys && phys < m->dma[i].phys + m->dma[i].size)
			return (u8 *)m->dma[i].p + (phys - m->dma[i].phys);
	return NULL;
}

u32 mock_dma_live(const struct mock *m)
{
	u32 i, n = 0;

	for (i = 0; i < m->ndma; i++)
		if (m->dma[i].live)
			n++;
	return n;
}

void mock_dma_fail_after(struct mock *m, int n)
{
	m->dma_fail_after = n;
}

void mock_fw_add(struct mock *m, const char *name, const void *data, size_t size)
{
	struct mock_fw_rec *f;

	if (m->nfw >= MOCK_MAX_FW) {
		fprintf(stderr, "mock: too many firmware blobs\n");
		abort();
	}
	f = &m->fw[m->nfw++];
	snprintf(f->name, sizeof(f->name), "%s", name);
	f->data = data;
	f->size = size;
}

static int h_fw_get(void *c, const char *name, struct gm20b_fw *fw)
{
	struct mock *m = (struct mock *)c;
	u32 i;

	for (i = 0; i < m->nfw; i++) {
		if (!strcmp(m->fw[i].name, name)) {
			fw->data = m->fw[i].data;
			fw->size = m->fw[i].size;
			fw->os = NULL;
			return 0;
		}
	}
	return -1;
}

static void h_fw_put(void *c, struct gm20b_fw *fw)
{
	(void)c;
	(void)fw;
}

static void h_jflush(void *c, const struct gm20b_jentry *e, u32 count)
{
	struct mock *m = (struct mock *)c;

	if (m->jlen + count > m->jcap) {
		m->jcap = (m->jlen + count) * 2 + 64;
		m->jlog = (struct gm20b_jentry *)realloc(m->jlog, m->jcap * sizeof(*m->jlog));
	}
	memcpy(&m->jlog[m->jlen], e, count * sizeof(*e));
	m->jlen += count;
	m->jflushes++;
}

u32 mock_jflushes(const struct mock *m)
{
	return m->jflushes;
}

u32 mock_jlen(const struct mock *m)
{
	return m->jlen;
}

const struct gm20b_jentry *mock_jlog(const struct mock *m)
{
	return m->jlog;
}

int mock_jfind(const struct mock *m, u32 code, u32 a, u32 from)
{
	u32 i;

	for (i = from; i < m->jlen; i++)
		if (m->jlog[i].code == code && (a == 0xffffffffu || m->jlog[i].a == a))
			return (int)i;
	return -1;
}

/* ---- lifecycle ----------------------------------------------------------- */
struct mock *mock_new(void)
{
	struct mock *m = (struct mock *)calloc(1, sizeof(*m));
	int i;

	for (i = 0; i < GM20B_SP_COUNT; i++)
		m->alive[i] = true;
	m->dma_fail_after = -1;
	return m;
}

void mock_free(struct mock *m)
{
	u32 i;

	if (!m)
		return;
	for (i = 0; i < m->ndma; i++)
		free(m->dma[i].p);
	free(m->tab);
	free(m->trace);
	free(m->jlog);
	free(m);
}

void mock_hal(struct mock *m, struct gm20b_hal *out)
{
	memset(out, 0, sizeof(*out));
	out->ctx = m;
	out->rd32 = h_rd32;
	out->wr32 = h_wr32;
	out->udelay = h_udelay;
	out->msleep = h_msleep;
	out->now_ns = h_now;
	out->dma_alloc = h_dma_alloc;
	out->dma_free = h_dma_free;
	out->fw_get = h_fw_get;
	out->fw_put = h_fw_put;
	out->journal_flush = h_jflush;
	out->rail_gpu_set_uv = NULL;
}
