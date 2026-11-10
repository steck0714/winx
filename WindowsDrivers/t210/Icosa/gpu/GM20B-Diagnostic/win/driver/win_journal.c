/* SPDX-License-Identifier: MIT */
/*
 * Journal persistence for the Windows shell.
 *
 * Why two mechanisms: the first bad MMIO access may hang the whole machine.  Whatever has not reached the SD card /
 * eMMC by then is lost.  (1) every flush appends to \SystemRoot\gm20b-journal.bin (write-through) and calls
 * ZwFlushBuffersFile; (2) the newest record is also mirrored into the registry value LastEntry of the service key and
 * flushed with ZwFlushKey.  After a hang, either or both of them name the operation that was in flight.
 *
 * Shell records (J_WIN_*) are queued in pend[] and written together with the next flush, which keeps the file in
 * chronological order: win_rec() first flushes the core's older records, then queues the new one.
 */
#include "win_priv.h"

static WCHAR jf_path_w[] = GM20B_JF_PATH_NT;
static UNICODE_STRING jf_path = NTK_USTR_INIT(jf_path_w);
static WCHAR bc_name_w[] = L"LastEntry";
static UNICODE_STRING bc_name = NTK_USTR_INIT(bc_name_w);

u64 win_now_ns(void)
{
	LARGE_INTEGER freq, c;
	u64 f, n;

	freq.QuadPart = 0;
	c = KeQueryPerformanceCounter(&freq);
	f = (u64)freq.QuadPart;
	n = (u64)c.QuadPart;
	if (f == 0)
		return 0;
	return (n / f) * 1000000000ull + (n % f) * 1000000000ull / f;   /* no overflow for any uptime */
}

static NTSTATUS jf_write(struct win_dev *d, const void *buf, ULONG len)
{
	IO_STATUS_BLOCK iosb;
	LARGE_INTEGER off;
	NTSTATUS st;

	if (!d->jf || !len)
		return STATUS_UNSUCCESSFUL;
	if (d->jf_bytes + len > WIN_JF_SESSION_CAP) {
		d->jf_dropped++;
		return STATUS_UNSUCCESSFUL;
	}
	off.QuadPart = -1;                           /* FILE_WRITE_TO_END_OF_FILE */
	iosb.Status = 0;
	iosb.Information = 0;
	st = ZwWriteFile(d->jf, NULL, NULL, NULL, &iosb, (PVOID)buf, len, &off, NULL);
	if (NT_SUCCESS(st)) {
		d->jf_bytes += len;
	} else {
		d->jf_err_count++;
		d->jf_last_err = st;
	}
	return st;
}

static void breadcrumb(struct win_dev *d, const struct gm20b_jentry *e)
{
	NTSTATUS st;

	if (!d->key)
		return;
	st = ZwSetValueKey(d->key, &bc_name, 0, REG_BINARY, (PVOID)e, (ULONG)sizeof(*e));
	if (NT_SUCCESS(st))
		st = ZwFlushKey(d->key);
	if (!NT_SUCCESS(st)) {
		d->bc_err_count++;
		d->bc_last_err = st;
	}
}

/* Write the queued shell records, then e[0..n), flush the file, mirror the newest record into the registry. */
void win_jf_commit(struct win_dev *d, const struct gm20b_jentry *e, u32 n)
{
	struct gm20b_jentry last;
	IO_STATUS_BLOCK iosb;
	int have = 0;

	gm20b_zero(&last, sizeof(last));
	if (d->npend) {
		(void)jf_write(d, d->pend, d->npend * (ULONG)sizeof(d->pend[0]));
		last = d->pend[d->npend - 1u];
		have = 1;
		d->npend = 0;
	}
	if (e && n) {
		(void)jf_write(d, e, n * (ULONG)sizeof(*e));
		last = e[n - 1u];
		have = 1;
	}
	if (!have)
		return;
	if (d->jf) {
		iosb.Status = 0;
		iosb.Information = 0;
		(void)ZwFlushBuffersFile(d->jf, &iosb);
	}
	breadcrumb(d, &last);
}

static void queue(struct win_dev *d, u32 code, u32 a, u32 b)
{
	struct gm20b_jentry *e;

	if (d->npend >= WIN_PEND_MAX)
		win_jf_commit(d, NULL, 0);
	e = &d->pend[d->npend++];
	e->seq = GM20B_JF_SEQ_WIN + d->rec_seq++;
	e->code = code;
	e->a = a;
	e->b = b;
	e->t_ns = win_now_ns();
}

void win_rec(struct win_dev *d, u32 code, u32 a, u32 b)
{
	if (d->g_valid)
		gm20b_jflush(&d->g);        /* older core records go to the file first */
	queue(d, code, a, b);
}

void win_step(struct win_dev *d, u32 code, u32 a, u32 b)
{
	win_rec(d, code, a, b);
	win_jf_commit(d, NULL, 0);
}

void win_sync(struct win_dev *d)
{
	if (d->g_valid)
		gm20b_jflush(&d->g);
	win_jf_commit(d, NULL, 0);
}

void win_bad_access(struct win_dev *d, enum gm20b_space sp, u32 off, u32 reason)
{
	d->bad_access++;
	if (d->bad_access <= 16u) {         /* a runaway loop must not flood the file */
		win_rec(d, J_WIN_BAD_ACCESS, GM20B_JADDR((u32)sp & 0xfu, off), reason);   /* keeps file order */
		win_jf_commit(d, NULL, 0);
	}
}

void win_jf_open(struct win_dev *d)
{
	OBJECT_ATTRIBUTES oa;
	IO_STATUS_BLOCK iosb;
	struct gm20b_jf_header h;
	HANDLE hnd = NULL;
	NTSTATUS st;

	InitializeObjectAttributes(&oa, &jf_path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
	iosb.Status = 0;
	iosb.Information = 0;
	st = ZwCreateFile(&hnd, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
			  FILE_SHARE_READ, FILE_OPEN_IF,
			  FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH, NULL, 0);
	d->jf_open_status = st;
	d->jf_created = 0;
	if (!NT_SUCCESS(st)) {
		d->jf = NULL;
		return;
	}
	d->jf = hnd;
	if (iosb.Information == FILE_CREATED) {
		d->jf_created = 1;
		gm20b_zero(&h, sizeof(h));
		h.magic = GM20B_JF_MAGIC;
		h.version = GM20B_JF_VERSION;
		h.entry_size = GM20B_JF_ENTRY_SIZE;
		h.header_size = GM20B_JF_HDR_SIZE;
		(void)jf_write(d, &h, (ULONG)sizeof(h));
		d->jf_bytes = 0;                /* the header does not count against the cap */
	}
}

void win_jf_close(struct win_dev *d)
{
	if (d->jf)
		(void)ZwClose(d->jf);
	if (d->key)
		(void)ZwClose(d->key);
	d->jf = NULL;
	d->key = NULL;
}
