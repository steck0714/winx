/* SPDX-License-Identifier: MIT */
/*
 * Host test of the Windows shell logic (the C files in win/driver) against a FAKE ntoskrnl.
 *
 * What this proves: the shell's own logic -- that every journal record is durable BEFORE the hardware access it
 * announces, registry handling, IOCTL buffer rules, the raw-MMIO policy, failure paths, unload.  The fake NT layer
 * also checks the arguments the driver passes to the kernel against what the WDK documents.
 * What it can NOT prove: that Windows loads the driver or that the real kernel behaves like this fake.
 *
 * The device registers are memfd-backed memory that is mapped twice: a PROT_NONE view handed to the driver (every
 * first access to a page traps, and the trap handler snapshots how much of the journal file is durable at that
 * very moment) and a read/write view for the test.  Build: make -C win check-host
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "win_priv.h"
#include "tegra_car_pmc.h"

#define ST_NOT_FOUND ((NTSTATUS)0xC0000034L)
#define SERVICE_PATH "\\REGISTRY\\MACHINE\\SYSTEM\\CurrentControlSet\\Services\\gm20b"
#define H_FILE   ((HANDLE)(uintptr_t)0x1000)
#define H_KEY    ((HANDLE)(uintptr_t)0x2000)
#define H_PARAMS ((HANDLE)(uintptr_t)0x2001)
#define BASE_COUNTER 3000000000000000ll          /* ~1.8 years of 19.2 MHz ticks: counter * 1e9 overflows 64 bits */
#define QPC_FREQ 19200000ll

/* ---- check macros ---------------------------------------------------------------------------------------- */
static int nfail, nchk;
#define CHECK(c) do { nchk++; if (!(c)) { nfail++; printf("  FAIL %d: %s\n", __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); nchk++; \
	if (a_ != b_) { nfail++; printf("  FAIL %d: %s == %s   (%lld != %lld)\n", __LINE__, #a, #b, a_, b_); } } while (0)
#define SCENARIO(name) printf("== %s\n", name)
static void violation(const char *msg)
{
	nfail++;
	printf("  FAIL (NT API contract): %s\n", msg);
}

/* ---- the fake world -------------------------------------------------------------------------------------- */
static struct {
	u64 now_ns;
	unsigned stalls, sleeps, max_stall;
	NTSTATUS create_status, write_status;
	int exists;
	unsigned char *fb;
	size_t flen, fcap, fflushed;
	unsigned nflush, file_closed, key_closed, params_closed;
	int key_ok, params_ok;
	struct { const char *name; u32 val; u32 type; } vals[8];
	int nvals;
	unsigned char crumb[24];
	int have_crumb;
	unsigned char crumb_dur[24];    /* the value as of the last ZwFlushKey: what survives a hang */
	int have_crumb_dur;
	int fail_map_space;
	int mapped[GM20B_SP_COUNT];
	unsigned unmapped, dma_allocs, dma_frees;
	void *last_alloc;
	int dev_created, link_created, dev_deleted, link_deleted;
	char devname[64], linkname[64];
	unsigned completed;
} F;

static void ustr_ascii(const UNICODE_STRING *u, char *out, size_t n)
{
	size_t i, len = u->Length / 2u;

	for (i = 0; i < len && i + 1 < n; i++)
		out[i] = (char)u->Buffer[i];
	out[i] = 0;
}

/* device memory: two views of the same pages */
static struct {
	u8 *rw, *trap;
	size_t len;
	u64 pa;
	u32 size;
} SP[GM20B_SP_COUNT];

static struct {
	int n;
	struct { int space; u32 off; size_t flushed, written; unsigned char crumb[24]; int have_crumb; } ev[32];
} TR;

static void on_segv(int sig, siginfo_t *si, void *uc)
{
	uintptr_t a = (uintptr_t)si->si_addr;
	int s;

	(void)sig;
	(void)uc;
	for (s = 0; s < GM20B_SP_COUNT; s++) {
		uintptr_t b = (uintptr_t)SP[s].trap;

		if (a >= b && a < b + SP[s].len) {
			if (TR.n < 32) {
				TR.ev[TR.n].space = s;
				TR.ev[TR.n].off = (u32)(a - b - (SP[s].pa & 0xfffu));
				TR.ev[TR.n].flushed = F.fflushed;
				TR.ev[TR.n].written = F.flen;
				memcpy(TR.ev[TR.n].crumb, F.crumb_dur, 24);
				TR.ev[TR.n].have_crumb = F.have_crumb_dur;
				TR.n++;
			}
			mprotect((void *)(a & ~(uintptr_t)0xfff), 4096, PROT_READ | PROT_WRITE);
			return;
		}
	}
	{
		char msg[200];
		int n = snprintf(msg, sizeof(msg), "unexpected SIGSEGV at %p; windows:", (void *)a);
		for (s = 0; s < GM20B_SP_COUNT && n < 150; s++)
			n += snprintf(msg + n, sizeof(msg) - (size_t)n, " [%p..+%zx]", (void *)SP[s].trap, SP[s].len);
		msg[n++] = '\n';
		(void)!write(2, msg, (size_t)n);
	}
	_exit(99);
}

static void spaces_init(void)
{
	struct sigaction sa;
	int s;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_segv;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
	for (s = 0; s < GM20B_SP_COUNT; s++) {
		int fd = memfd_create("space", 0);

		SP[s].pa = gm20b_space_pa((enum gm20b_space)s);
		SP[s].size = gm20b_space_size((enum gm20b_space)s);
		SP[s].len = ((SP[s].pa & 0xfffu) + SP[s].size + 0xfffu) & ~(size_t)0xfff;
		if (fd < 0 || ftruncate(fd, (off_t)SP[s].len)) {
			perror("memfd");
			exit(2);
		}
		SP[s].rw = mmap(NULL, SP[s].len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		SP[s].trap = mmap(NULL, SP[s].len, PROT_NONE, MAP_SHARED, fd, 0);
		close(fd);
		if (SP[s].rw == MAP_FAILED || SP[s].trap == MAP_FAILED) {
			perror("mmap");
			exit(2);
		}
	}
}

static u32 mem_rd(int s, u32 off)
{
	u32 v;

	memcpy(&v, SP[s].rw + (SP[s].pa & 0xfffu) + off, 4);
	return v;
}

static void mem_wr(int s, u32 off, u32 v)
{
	memcpy(SP[s].rw + (SP[s].pa & 0xfffu) + off, &v, 4);
}

/* ---- fake ntoskrnl --------------------------------------------------------------------------------------- */
NTSTATUS ZwCreateFile(PHANDLE h, ACCESS_MASK acc, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK iosb, PLARGE_INTEGER alloc,
		      ULONG attr, ULONG share, ULONG disp, ULONG opts, PVOID ea, ULONG ealen)
{
	char nm[128];

	if (F.create_status)
		return F.create_status;
	ustr_ascii(oa->ObjectName, nm, sizeof(nm));
	if (strcmp(nm, "\\SystemRoot\\gm20b-journal.bin"))
		violation("journal path");
	if (acc != (FILE_APPEND_DATA | SYNCHRONIZE))
		violation("access must be exactly FILE_APPEND_DATA|SYNCHRONIZE (FILE_WRITE_DATA would break append semantics)");
	if (disp != FILE_OPEN_IF)
		violation("disposition must be FILE_OPEN_IF");
	if ((opts & (FILE_WRITE_THROUGH | FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE)) !=
	    (FILE_WRITE_THROUGH | FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE))
		violation("create options need WRITE_THROUGH | SYNCHRONOUS_IO_NONALERT | NON_DIRECTORY_FILE");
	if (!(oa->Attributes & OBJ_KERNEL_HANDLE))
		violation("ZwCreateFile without OBJ_KERNEL_HANDLE");
	if (share != FILE_SHARE_READ)
		violation("share mode");
	if (oa->Length != sizeof(OBJECT_ATTRIBUTES))
		violation("OBJECT_ATTRIBUTES.Length");
	iosb->Information = F.exists ? 1 : FILE_CREATED;
	F.exists = 1;
	*h = H_FILE;
	return STATUS_SUCCESS;
}

NTSTATUS ZwWriteFile(HANDLE h, HANDLE ev, PVOID apc, PVOID ctx, PIO_STATUS_BLOCK iosb, PVOID buf, ULONG len,
		     PLARGE_INTEGER off, PULONG key)
{
	if (h != H_FILE)
		violation("ZwWriteFile on a handle that is not the journal");
	if (!iosb)
		violation("ZwWriteFile needs an IO_STATUS_BLOCK");
	if (!off || off->QuadPart != -1)
		violation("append must pass ByteOffset = FILE_WRITE_TO_END_OF_FILE (-1)");
	if (F.write_status)
		return F.write_status;
	if (F.flen + len > F.fcap) {
		F.fcap = (F.flen + len) * 2 + 4096;
		F.fb = realloc(F.fb, F.fcap);
	}
	memcpy(F.fb + F.flen, buf, len);
	F.flen += len;
	iosb->Information = len;
	return STATUS_SUCCESS;
}

NTSTATUS ZwFlushBuffersFile(HANDLE h, PIO_STATUS_BLOCK iosb)
{
	if (h != H_FILE || !iosb)
		violation("ZwFlushBuffersFile arguments");
	F.fflushed = F.flen;
	F.nflush++;
	return STATUS_SUCCESS;
}

NTSTATUS ZwClose(HANDLE h)
{
	if (h == H_FILE)
		F.file_closed++;
	else if (h == H_KEY)
		F.key_closed++;
	else if (h == H_PARAMS)
		F.params_closed++;
	else
		violation("ZwClose of an unknown handle");
	return STATUS_SUCCESS;
}

NTSTATUS ZwOpenKey(PHANDLE h, ACCESS_MASK acc, POBJECT_ATTRIBUTES oa)
{
	char nm[256];

	ustr_ascii(oa->ObjectName, nm, sizeof(nm));
	if (!(oa->Attributes & OBJ_KERNEL_HANDLE))
		violation("ZwOpenKey without OBJ_KERNEL_HANDLE");
	if (!oa->RootDirectory) {
		if (strcmp(nm, SERVICE_PATH) || !F.key_ok)
			return ST_NOT_FOUND;
		if (!(acc & KEY_SET_VALUE))
			violation("service key must be opened with KEY_SET_VALUE (the breadcrumb needs it)");
		*h = H_KEY;
		return STATUS_SUCCESS;
	}
	if (oa->RootDirectory == H_KEY && !strcmp(nm, "Parameters") && F.params_ok) {
		*h = H_PARAMS;
		return STATUS_SUCCESS;
	}
	return ST_NOT_FOUND;
}

NTSTATUS ZwQueryValueKey(HANDLE h, PUNICODE_STRING name, KEY_VALUE_INFORMATION_CLASS cls, PVOID buf, ULONG len,
			 PULONG rlen)
{
	char nm[64];
	int i;

	ustr_ascii(name, nm, sizeof(nm));
	if (h != H_PARAMS)
		violation("ZwQueryValueKey on an unexpected handle");
	if (cls != KeyValuePartialInformation)
		violation("information class must be KeyValuePartialInformation");
	for (i = 0; i < F.nvals; i++) {
		if (!strcmp(nm, F.vals[i].name)) {
			KEY_VALUE_PARTIAL_INFORMATION *p = buf;

			if (len < 16)
				return STATUS_BUFFER_TOO_SMALL;
			p->TitleIndex = 0;
			p->Type = F.vals[i].type;
			p->DataLength = 4;
			memcpy(p->Data, &F.vals[i].val, 4);
			*rlen = 16;
			return STATUS_SUCCESS;
		}
	}
	return ST_NOT_FOUND;
}

NTSTATUS ZwSetValueKey(HANDLE h, PUNICODE_STRING name, ULONG title, ULONG type, PVOID data, ULONG size)
{
	char nm[64];

	ustr_ascii(name, nm, sizeof(nm));
	if (h != H_KEY || strcmp(nm, "LastEntry") || type != REG_BINARY || size != 24)
		violation("ZwSetValueKey must write the 24-byte REG_BINARY value LastEntry into the service key");
	memcpy(F.crumb, data, 24);
	F.have_crumb = 1;
	return STATUS_SUCCESS;
}

NTSTATUS ZwFlushKey(HANDLE h)
{
	if (h != H_KEY)
		violation("ZwFlushKey handle");
	memcpy(F.crumb_dur, F.crumb, 24);
	F.have_crumb_dur = F.have_crumb;
	return STATUS_SUCCESS;
}

PVOID MmMapIoSpaceEx(PHYSICAL_ADDRESS pa, SIZE_T n, ULONG prot)
{
	int s;

	if (prot != (PAGE_READWRITE | PAGE_NOCACHE))
		violation("MmMapIoSpaceEx protection must be PAGE_READWRITE|PAGE_NOCACHE");
	for (s = 0; s < GM20B_SP_COUNT; s++) {
		if ((u64)pa.QuadPart == SP[s].pa) {
			if (n != SP[s].size)
				violation("MmMapIoSpaceEx size");
			if (s == F.fail_map_space)
				return NULL;
			F.mapped[s]++;
			return SP[s].trap + (SP[s].pa & 0xfffu);
		}
	}
	violation("MmMapIoSpaceEx of an unexpected physical address");
	return NULL;
}

void MmUnmapIoSpace(PVOID va, SIZE_T n)
{
	int s;

	for (s = 0; s < GM20B_SP_COUNT; s++) {
		if ((u8 *)va == SP[s].trap + (SP[s].pa & 0xfffu)) {
			if (n != SP[s].size)
				violation("MmUnmapIoSpace size must equal the mapped size");
			F.unmapped++;
			return;
		}
	}
	violation("MmUnmapIoSpace of something that was never mapped");
}

PVOID MmAllocateContiguousMemorySpecifyCache(SIZE_T n, PHYSICAL_ADDRESS lo, PHYSICAL_ADDRESS hi,
					     PHYSICAL_ADDRESS bound, MEMORY_CACHING_TYPE type)
{
	void *p;

	if (type != MmNonCached)
		violation("DMA memory must be MmNonCached");
	if (hi.QuadPart < 0xFFFFFFFFll || lo.QuadPart != 0)
		violation("DMA address window");
	p = aligned_alloc(4096, n);
	memset(p, 0xAA, n);                               /* garbage: the driver must zero it itself */
	F.last_alloc = p;
	F.dma_allocs++;
	return p;
}

void MmFreeContiguousMemory(PVOID p)
{
	if (p != F.last_alloc)
		violation("MmFreeContiguousMemory must get the ORIGINAL allocation pointer");
	F.dma_frees++;
	free(p);
	F.last_alloc = NULL;
}

PHYSICAL_ADDRESS MmGetPhysicalAddress(PVOID p)
{
	PHYSICAL_ADDRESS pa;

	pa.QuadPart = (LONGLONG)((uintptr_t)p + 0x1000);   /* physical != virtual: forces the alignment code to work */
	return pa;
}

void KeStallExecutionProcessor(ULONG us)
{
	if (us > 50)
		violation("KeStallExecutionProcessor must not be asked for more than 50 us");
	if (us > F.max_stall)
		F.max_stall = us;
	F.now_ns += (u64)us * 1000u;
	F.stalls++;
}

NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE mode, BOOLEAN alertable, PLARGE_INTEGER iv)
{
	if (mode != KernelMode || iv->QuadPart >= 0)
		violation("KeDelayExecutionThread needs KernelMode and a RELATIVE (negative) interval");
	F.now_ns += (u64)(-iv->QuadPart) * 100u;
	F.sleeps++;
	return STATUS_SUCCESS;
}

LARGE_INTEGER KeQueryPerformanceCounter(PLARGE_INTEGER freq)
{
	LARGE_INTEGER c;

	if (freq)
		freq->QuadPart = QPC_FREQ;
	c.QuadPart = BASE_COUNTER + (LONGLONG)(F.now_ns * (u64)QPC_FREQ / 1000000000ull);
	F.now_ns += 10000;      /* time passes between two readings: every record gets a strictly later timestamp */
	return c;
}

static char devobj;
NTSTATUS IoCreateDevice(PDRIVER_OBJECT d, ULONG ext, PUNICODE_STRING name, DEVICE_TYPE type, ULONG chars,
			BOOLEAN excl, PDEVICE_OBJECT *out)
{
	if (type != FILE_DEVICE_UNKNOWN || !(chars & FILE_DEVICE_SECURE_OPEN) || !excl)
		violation("control device must be FILE_DEVICE_UNKNOWN, SECURE_OPEN and exclusive");
	ustr_ascii(name, F.devname, sizeof(F.devname));
	*out = (PDEVICE_OBJECT)&devobj;
	F.dev_created = 1;
	return STATUS_SUCCESS;
}

void IoDeleteDevice(PDEVICE_OBJECT d)
{
	F.dev_deleted = 1;
}

NTSTATUS IoCreateSymbolicLink(PUNICODE_STRING link, PUNICODE_STRING dev)
{
	ustr_ascii(link, F.linkname, sizeof(F.linkname));
	F.link_created = 1;
	return STATUS_SUCCESS;
}

NTSTATUS IoDeleteSymbolicLink(PUNICODE_STRING link)
{
	F.link_deleted = 1;
	return STATUS_SUCCESS;
}

void IofCompleteRequest(PIRP irp, CCHAR boost)
{
	F.completed++;
}

/* ---- scenario plumbing ----------------------------------------------------------------------------------- */
static DRIVER_OBJECT drv;
static WCHAR rp_w[] = L"\\REGISTRY\\MACHINE\\SYSTEM\\CurrentControlSet\\Services\\gm20b";
static UNICODE_STRING regpath = NTK_USTR_INIT(rp_w);

/* re-arm the trap: the next access to every page of every window is observed (and snapshotted) again */
static void trap_rearm(void)
{
	int s;

	for (s = 0; s < GM20B_SP_COUNT; s++)
		mprotect(SP[s].trap, SP[s].len, PROT_NONE);
	TR.n = 0;
}

static void world_reset(void)
{
	int s;

	free(F.fb);
	memset(&F, 0, sizeof(F));
	F.fail_map_space = -1;
	F.key_ok = F.params_ok = 1;
	for (s = 0; s < GM20B_SP_COUNT; s++) {
		memset(SP[s].rw, 0, SP[s].len < 0x10000 ? SP[s].len : 0x10000);
	}
	trap_rearm();
}

static void set_val(const char *name, u32 v)
{
	F.vals[F.nvals].name = name;
	F.vals[F.nvals].val = v;
	F.vals[F.nvals].type = REG_DWORD;
	F.nvals++;
}

static void preset_powered(void)
{
	mem_wr(GM20B_SP_CAR, CAR_CLK_OUT_ENB_X, GM20B_BIT(CAR_X_GPU_BIT));
	mem_wr(GM20B_SP_CAR, CAR_RST_DEVICES_X, 0);
	mem_wr(GM20B_SP_PMC, PMC_GPU_RG_CNTRL, 0);
	mem_wr(GM20B_SP_BAR0, 0, 0x12b000a1u);
}

/* what the I/O manager puts into every MajorFunction slot before it calls DriverEntry */
static NTSTATUS invalid_request(PDEVICE_OBJECT dev, PIRP irp)
{
	irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
	irp->IoStatus.Information = 0;
	IofCompleteRequest(irp, IO_NO_INCREMENT);
	return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS load(void)
{
	int i;

	memset(&drv, 0, sizeof(drv));
	for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
		drv.MajorFunction[i] = invalid_request;
	return DriverEntry(&drv, &regpath);
}

static u32 nrec(void)
{
	return F.flen >= 32 ? (u32)((F.flen - 32) / 24) : 0;
}

static struct gm20b_jentry rec(u32 i)
{
	struct gm20b_jentry e;

	memcpy(&e, F.fb + 32 + 24 * (size_t)i, 24);
	return e;
}

static int find_rec(u32 code, int match_a, u32 a, u32 from)
{
	u32 i;

	for (i = from; i < nrec(); i++) {
		struct gm20b_jentry e = rec(i);

		if (e.code == code && (!match_a || e.a == a))
			return (int)i;
	}
	return -1;
}

static int durable_n(size_t flushed)
{
	return flushed >= 32 ? (int)((flushed - 32) / 24) : 0;
}

static int durable_has(size_t flushed, u32 code)
{
	int i, n = durable_n(flushed);

	for (i = 0; i < n; i++)
		if (rec((u32)i).code == code)
			return 1;
	return 0;
}

static u32 durable_last(size_t flushed)
{
	int n = durable_n(flushed);

	return n ? rec((u32)(n - 1)).code : 0xffffffffu;
}

static int tr_find(int space)
{
	int i;

	for (i = 0; i < TR.n; i++)
		if (TR.ev[i].space == space)
			return i;
	return -1;
}

/* the registry crumb that was DURABLE at trap event c must name the very record that is newest in the file then */
static void check_crumb(int c)
{
	int n = durable_n(TR.ev[c].flushed);
	struct gm20b_jentry e;

	CHECK(TR.ev[c].have_crumb);
	if (n <= 0)
		return;
	e = rec((u32)(n - 1));
	CHECK(!memcmp(TR.ev[c].crumb, &e, sizeof(e)));
}

/* at trap event c the newest durable record must be the intent `code`, and nothing may still sit in memory */
static void check_intent(int c, u32 code)
{
	CHECK_EQ(TR.ev[c].written, TR.ev[c].flushed);
	CHECK_EQ(durable_last(TR.ev[c].flushed), code);
	check_crumb(c);
}

/* invariants of the journal file that must hold after EVERY scenario */
static void check_file(void)
{
	u32 i, n = nrec(), win_next = 0, core_next = 0;
	u64 tprev = 0;

	if (!F.flen)
		return;
	CHECK_EQ(F.fflushed, F.flen);                              /* everything written is durable */
	for (i = 0; i < n; i++) {
		struct gm20b_jentry e = rec(i);

		if (e.code == J_WIN_LOAD) {
			win_next = GM20B_JF_SEQ_WIN;
			core_next = 0;
		}
		if (e.seq >= GM20B_JF_SEQ_WIN) {
			CHECK_EQ(e.seq, win_next);
			win_next = e.seq + 1;
		} else {
			CHECK_EQ(e.seq, core_next);
			core_next = e.seq + 1;
		}
		CHECK(e.t_ns > tprev);                            /* file order == creation order */
		tprev = e.t_ns;
	}
	if (F.have_crumb && n)
		CHECK(!memcmp(F.crumb, F.fb + F.flen - 24, 24));   /* the registry mirror is the newest record */
	if (F.have_crumb) {                                        /* ... and it was flushed, not just set */
		CHECK(F.have_crumb_dur);
		CHECK(!memcmp(F.crumb, F.crumb_dur, 24));
	}
}

static union { IRP irp; char pad[512]; } IRPU;
static IO_STACK_LOCATION ISL;
static unsigned char sysbuf[4096] __attribute__((aligned(16)));

static NTSTATUS do_major(u8 major, u32 code, const void *in, u32 inlen, void *out, u32 outlen, ULONG_PTR *info)
{
	u32 n = inlen > outlen ? inlen : outlen;
	ULONG_PTR got;
	NTSTATUS st;
	u32 i;

	memset(&IRPU, 0, sizeof(IRPU));
	memset(&ISL, 0, sizeof(ISL));
	memset(sysbuf, 0xCC, sizeof(sysbuf));
	if (in)
		memcpy(sysbuf, in, inlen);
	IRPU.irp.AssociatedIrp.SystemBuffer = n ? sysbuf : NULL;
	IRPU.irp.Tail.Overlay.CurrentStackLocation = &ISL;
	ISL.MajorFunction = major;
	ISL.Parameters.DeviceIoControl.IoControlCode = code;
	ISL.Parameters.DeviceIoControl.InputBufferLength = inlen;
	ISL.Parameters.DeviceIoControl.OutputBufferLength = outlen;
	st = drv.MajorFunction[major](NULL, &IRPU.irp);
	CHECK_EQ(st, IRPU.irp.IoStatus.Status);
	got = IRPU.irp.IoStatus.Information;
	if (info)
		*info = got;
	for (i = n; i < n + 64; i++)
		if (sysbuf[i] != 0xCC) {
			violation("the driver wrote beyond the IOCTL buffer");
			break;
		}
	if (out && got)
		memcpy(out, sysbuf, got < outlen ? got : outlen);
	return st;
}

static NTSTATUS ioctl(u32 code, const void *in, u32 inlen, void *out, u32 outlen, ULONG_PTR *info)
{
	return do_major(IRP_MJ_DEVICE_CONTROL, code, in, inlen, out, outlen, info);
}

static u32 status_field(struct gm20b_ioctl_status *s)
{
	ULONG_PTR info = 0;

	memset(s, 0, sizeof(*s));
	CHECK_EQ(ioctl(GM20B_IOCTL_GET_STATUS, NULL, 0, s, sizeof(*s), &info), STATUS_SUCCESS);
	CHECK_EQ(info, sizeof(*s));
	return s->abi;
}

static NTSTATUS raw_read(u32 sp, u32 off, u32 *val)
{
	struct gm20b_ioctl_rawrd rq = { sp, off };
	ULONG_PTR info = 0;
	NTSTATUS st = ioctl(GM20B_IOCTL_RAW_READ, &rq, sizeof(rq), val, 4, &info);

	if (st == STATUS_SUCCESS)
		CHECK_EQ(info, 4);
	return st;
}

static NTSTATUS raw_write(u32 sp, u32 off, u32 val)
{
	struct gm20b_ioctl_rawwr rq = { sp, off, val };

	return ioctl(GM20B_IOCTL_RAW_WRITE, &rq, sizeof(rq), NULL, 0, NULL);
}

/* ---- scenarios ------------------------------------------------------------------------------------------- */
/* GM20B_DUMP_DIR=<dir>: save journals produced by the REAL shell code, for the PC-side decoder checks and the docs */
static void dump(const char *name, const void *buf, size_t len)
{
	const char *dir = getenv("GM20B_DUMP_DIR");
	char path[512];
	FILE *fp;

	if (!dir)
		return;
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	fp = fopen(path, "wb");
	if (!fp) {
		violation("cannot write a dump file (does GM20B_DUMP_DIR exist?)");
		return;
	}
	if (len)
		(void)!fwrite(buf, 1, len, fp);
	fclose(fp);
}

static void dump_hex(const char *name, const unsigned char *buf, size_t len)
{
	char hex[128];
	size_t i;

	for (i = 0; i < len && 2 * i + 3 < sizeof(hex); i++)
		snprintf(hex + 2 * i, 3, "%02x", buf[i]);
	hex[2 * i] = 0;
	dump(name, hex, strlen(hex));
}

static void sc_defaults(void)
{
	struct gm20b_jf_header h;
	unsigned __int128 want;
	int k, s;

	SCENARIO("no Parameters key: stage 0 only, no hardware access, correct journal skeleton");
	world_reset();
	F.params_ok = 0;
	CHECK_EQ(load(), STATUS_SUCCESS);
	CHECK_EQ(g_win.cfg.max_stage, 0);
	CHECK_EQ(g_win.init_status, 0);
	CHECK_EQ(g_win.g.stage_done, 1);
	CHECK_EQ(TR.n, 0);                                         /* not one access to any register window */
	CHECK(F.flen >= 32);
	memcpy(&h, F.fb, sizeof(h));
	CHECK_EQ(h.magic, GM20B_JF_MAGIC);
	CHECK_EQ(h.entry_size, 24);
	CHECK_EQ(h.header_size, 32);
	CHECK_EQ(rec(0).code, J_WIN_LOAD);
	CHECK_EQ(rec(0).seq, GM20B_JF_SEQ_WIN);
	CHECK_EQ(rec(0).a, GM20B_WIN_VERSION);
	want = (unsigned __int128)BASE_COUNTER * 1000000000u / (unsigned)QPC_FREQ;
	CHECK((unsigned __int128)rec(0).t_ns == want);              /* the clock conversion survives a huge counter */
	CHECK_EQ(rec(1).code, J_WIN_FILE);
	CHECK_EQ(rec(1).a, 0);
	CHECK_EQ(rec(1).b, 1);                                      /* created by this load */
	k = find_rec(J_WIN_KEY, 0, 0, 0);
	CHECK(k >= 2);
	CHECK_EQ(rec((u32)k).a, 0);
	CHECK_EQ(rec((u32)k).b, (u32)ST_NOT_FOUND);
	k = find_rec(J_WIN_REG_MAXSTAGE, 0, 0, 0);
	CHECK(k >= 0 && rec((u32)k).a == 0 && rec((u32)k).b == 0);
	for (s = 0; s < GM20B_SP_COUNT; s++) {
		k = find_rec(J_WIN_MAP, 1, (u32)s, 0);
		CHECK(k >= 0 && rec((u32)k).b != 0);
		CHECK_EQ(F.mapped[s], 1);
	}
	k = find_rec(J_WIN_INIT_BEGIN, 0, 0, 0);
	CHECK(k >= 0);
	CHECK_EQ(durable_last(F.flen), J_WIN_DEVICE);
	k = find_rec(J_STAGE_OK, 1, 0, 0);
	CHECK(k >= 0);
	k = find_rec(J_WIN_INIT_DONE, 0, 0, 0);
	CHECK(k >= 0 && rec((u32)k).a == 0 && rec((u32)k).b == 1);
	CHECK(F.dev_created && F.link_created);
	CHECK(!strcmp(F.devname, "\\Device\\Gm20b"));
	CHECK(!strcmp(F.linkname, "\\DosDevices\\Gm20b"));
	CHECK(g_win.map_ok);
	CHECK(drv.DriverUnload != NULL);
	CHECK(drv.MajorFunction[IRP_MJ_CREATE] && drv.MajorFunction[IRP_MJ_CLOSE] && drv.MajorFunction[IRP_MJ_DEVICE_CONTROL]);
	check_file();
}

static void sc_stage2(void)
{
	int c, p, b, k;

	SCENARIO("MaxStage=2 through the shell: intent records are durable BEFORE the first access to each window");
	world_reset();
	preset_powered();
	set_val("MaxStage", 2);
	set_val("Flags", GM20B_CF_TRACE_REGS);
	set_val("AllowRawMmio", 1);
	CHECK_EQ(load(), STATUS_SUCCESS);
	CHECK_EQ(g_win.init_status, 0);
	CHECK_EQ(g_win.g.stage_done, 3);
	CHECK_EQ(g_win.g.info.boot0, 0x12b000a1u);
	CHECK_EQ(g_win.g.info.chipset, 0x12b);
	k = find_rec(J_WIN_REG_MAXSTAGE, 0, 0, 0);
	CHECK(k >= 0 && rec((u32)k).a == 2 && rec((u32)k).b == 1);
	k = find_rec(J_WIN_REG_FLAGS, 0, 0, 0);
	CHECK(k >= 0 && rec((u32)k).a == 1 && rec((u32)k).b == 1);
	k = find_rec(J_WIN_REG_RAWMMIO, 0, 0, 0);
	CHECK(k >= 0 && rec((u32)k).a == 1 && rec((u32)k).b == 1);
	c = tr_find(GM20B_SP_CAR);
	p = tr_find(GM20B_SP_PMC);
	b = tr_find(GM20B_SP_BAR0);
	CHECK(c >= 0 && p >= 0 && b >= 0);
	if (c >= 0) {
		CHECK(durable_has(TR.ev[c].flushed, J_S1_PROBE_BEGIN));   /* before the first CAR access */
		CHECK(durable_has(TR.ev[c].flushed, J_WIN_INIT_BEGIN));
		check_intent(c, J_S1_PROBE_RD);       /* the read itself was announced, nothing sat in memory, crumb agrees */
		k = durable_n(TR.ev[c].flushed);
		CHECK(k > 0 && rec((u32)(k - 1)).a == GM20B_JADDR(GM20B_SP_CAR, CAR_CLK_OUT_ENB_X));
	}
	if (p >= 0) {
		CHECK(durable_has(TR.ev[p].flushed, J_S1_PROBE_BEGIN));
		check_intent(p, J_S1_PROBE_RD);
		k = durable_n(TR.ev[p].flushed);
		CHECK(k > 0 && rec((u32)(k - 1)).a == GM20B_JADDR(GM20B_SP_PMC, PMC_GPU_RG_CNTRL));
	}
	if (b >= 0) {
		CHECK_EQ(TR.ev[b].off, 0);                                /* BOOT0 is the first BAR0 access */
		CHECK_EQ(durable_last(TR.ev[b].flushed), J_S2_BOOT0_INTENT);
		CHECK_EQ(TR.ev[b].written, TR.ev[b].flushed);
		check_crumb(b);
	}
	CHECK(tr_find(GM20B_SP_MC) < 0);                              /* MC and BAR1 stay untouched in S0..S2 */
	CHECK(tr_find(GM20B_SP_BAR1) < 0);
	k = find_rec(J_S2_BOOT0, 1, 0x12b000a1u, 0);
	CHECK(k >= 0);
	CHECK(find_rec(J_RD, 0, 0, 0) >= 0);                          /* TRACE_REGS reached the file */
	CHECK((mem_rd(GM20B_SP_CAR, 0x284) & GM20B_BIT(24)) != 0);    /* the SET alias was used, ... */
	CHECK_EQ(mem_rd(GM20B_SP_CAR, CAR_CLK_OUT_ENB_X), GM20B_BIT(CAR_X_GPU_BIT));   /* ... never the shared register */
	CHECK_EQ(mem_rd(GM20B_SP_BAR0, 0x140), 0);
	k = find_rec(J_WIN_INIT_DONE, 0, 0, 0);
	CHECK(k >= 0 && rec((u32)k).a == 0 && rec((u32)k).b == 3);
	dump("stage2-ok.bin", F.fb, F.flen);
	if (b >= 0) {   /* what the file and the registry value would hold if the BOOT0 read had hung the machine */
		dump("hang-at-boot0.bin", F.fb, TR.ev[b].flushed);
		dump_hex("hang-at-boot0.crumb.txt", TR.ev[b].crumb, 24);
	}
	check_file();
}

static void sc_cold_chip(void)
{
	struct gm20b_ioctl_status st;
	int k;

	SCENARIO("cold chip (stage 1 cannot verify): fails cleanly, BAR0 is never touched, driver stays loaded");
	world_reset();
	mem_wr(GM20B_SP_BAR0, 0, 0x12b000a1u);                       /* clocks stay off: plain memory ignores the aliases */
	set_val("MaxStage", 2);
	CHECK_EQ(load(), STATUS_SUCCESS);                            /* DriverEntry succeeds regardless */
	CHECK_EQ(g_win.init_status, GM20B_E_HW);
	CHECK_EQ(g_win.g.stage_done, 0);
	CHECK(tr_find(GM20B_SP_BAR0) < 0);
	k = find_rec(J_STAGE_FAIL, 1, 1, 0);
	CHECK(k >= 0 && rec((u32)k).b == (u32)GM20B_E_HW);
	k = find_rec(J_WIN_INIT_DONE, 0, 0, 0);
	CHECK(k >= 0 && rec((u32)k).a == (u32)GM20B_E_HW);
	CHECK(F.dev_created);
	status_field(&st);
	CHECK_EQ(st.init_status, GM20B_E_HW);
	CHECK_EQ(st.fail_stage, 1);
	CHECK_EQ(st.last_err, GM20B_E_HW);
	dump("cold-chip.bin", F.fb, F.flen);
	check_file();
}

static void sc_stub_stage(void)
{
	int i;

	SCENARIO("a stage that is not implemented (S3) fails cleanly and everything before it is torn down");
	world_reset();
	preset_powered();
	set_val("MaxStage", 3);
	CHECK_EQ(load(), STATUS_SUCCESS);
	CHECK(g_win.init_status != 0);
	CHECK_EQ(g_win.g.stage_done, 0);
	CHECK_EQ(g_win.g.fail_stage, 3);
	for (i = 2; i >= 0; i--)
		CHECK(find_rec(J_FINI_OK, 1, (u32)i, 0) >= 0);
	check_file();
}

static void sc_ioctl(void)
{
	struct gm20b_ioctl_status st;
	struct gm20b_jentry je[16];
	ULONG_PTR info = 0;
	u32 v = 0, i, big;
	int rc = 99;

	SCENARIO("IOCTL contract: buffer sizes, status, journal, advance, fini, busy");
	world_reset();
	preset_powered();
	set_val("MaxStage", 2);
	CHECK_EQ(load(), STATUS_SUCCESS);
	CHECK_EQ(do_major(IRP_MJ_CREATE, 0, NULL, 0, NULL, 0, NULL), STATUS_SUCCESS);
	CHECK_EQ(do_major(IRP_MJ_CLOSE, 0, NULL, 0, NULL, 0, NULL), STATUS_SUCCESS);
	CHECK_EQ(do_major(3 /* IRP_MJ_READ */, 0, NULL, 0, NULL, 0, NULL), STATUS_INVALID_DEVICE_REQUEST);
	CHECK_EQ(ioctl(GM20B_CTL(60), NULL, 0, NULL, 0, NULL), STATUS_INVALID_DEVICE_REQUEST);
	CHECK_EQ(ioctl(GM20B_IOCTL_GET_STATUS, NULL, 0, &st, sizeof(st) - 1, NULL), STATUS_BUFFER_TOO_SMALL);
	CHECK_EQ(status_field(&st), GM20B_IOCTL_ABI);
	CHECK_EQ(st.size, sizeof(st));
	CHECK_EQ(st.max_stage, 2);
	CHECK_EQ(st.stage_done, 3);
	CHECK_EQ(st.init_status, 0);
	CHECK_EQ(st.boot0, 0x12b000a1u);
	CHECK_EQ(st.chipset, 0x12b);
	CHECK_EQ(st.driver_version, GM20B_WIN_VERSION);
	CHECK_EQ(st.jf_open_status, 0);
	CHECK(st.jf_bytes > 0);
	CHECK_EQ(st.j_head, g_win.g.j.head);
	/* journal ring */
	CHECK_EQ(ioctl(GM20B_IOCTL_READ_JOURNAL, &v, 2, je, sizeof(je), &info), STATUS_BUFFER_TOO_SMALL);
	CHECK_EQ(ioctl(GM20B_IOCTL_READ_JOURNAL, &v, 4, je, 23, &info), STATUS_BUFFER_TOO_SMALL);
	v = 0;
	CHECK_EQ(ioctl(GM20B_IOCTL_READ_JOURNAL, &v, 4, je, sizeof(je), &info), STATUS_SUCCESS);
	CHECK_EQ(info, sizeof(je));
	for (i = 0; i < 16; i++)
		CHECK_EQ(je[i].seq, i);
	v = g_win.g.j.head;
	CHECK_EQ(ioctl(GM20B_IOCTL_READ_JOURNAL, &v, 4, je, sizeof(je), &info), STATUS_SUCCESS);
	CHECK_EQ(info, 0);
	/* advance / fini */
	CHECK_EQ(ioctl(GM20B_IOCTL_ADVANCE, NULL, 0, NULL, 0, NULL), STATUS_BUFFER_TOO_SMALL);
	v = 1;
	CHECK_EQ(ioctl(GM20B_IOCTL_ADVANCE, &v, 4, &rc, 4, &info), STATUS_SUCCESS);
	CHECK_EQ(rc, 0);
	CHECK_EQ(g_win.g.stage_done, 3);                           /* nothing new to run */
	CHECK_EQ(ioctl(GM20B_IOCTL_FINI, NULL, 0, NULL, 0, NULL), STATUS_SUCCESS);
	CHECK_EQ(g_win.g.stage_done, 0);
	CHECK_EQ(g_win.g.cfg.max_stage, 0);
	CHECK_EQ(ioctl(GM20B_IOCTL_FINI, NULL, 0, NULL, 0, NULL), STATUS_SUCCESS);   /* fini twice is harmless */
	v = 1;
	rc = 99;
	CHECK_EQ(ioctl(GM20B_IOCTL_ADVANCE, &v, 4, &rc, 4, &info), STATUS_SUCCESS);
	CHECK_EQ(rc, 0);
	CHECK_EQ(g_win.g.stage_done, 2);                           /* exactly S0 and S1 this time */
	status_field(&st);
	CHECK_EQ(st.max_stage, 1);
	v = 8;                                                     /* S3 is a stub: clean failure, status in the output */
	CHECK_EQ(ioctl(GM20B_IOCTL_ADVANCE, &v, 4, &rc, 4, &info), STATUS_SUCCESS);
	CHECK(rc != 0);
	CHECK_EQ(g_win.g.stage_done, 0);
	/* busy */
	g_win.busy = 1;
	CHECK_EQ(ioctl(GM20B_IOCTL_GET_STATUS, NULL, 0, &st, sizeof(st), NULL), STATUS_DEVICE_BUSY);
	g_win.busy = 0;
	CHECK_EQ(ioctl(GM20B_IOCTL_GET_STATUS, NULL, 0, &st, sizeof(st), NULL), STATUS_SUCCESS);
	CHECK_EQ(g_win.busy, 0);
	CHECK(F.completed > 10);
	check_file();
	/* ring wrap: only the newest GM20B_J_RING records exist; the reader must not walk off the array */
	for (big = 0; big < 700; big++)
		gm20b_j(&g_win.g, J_NOTE, big, 0);
	v = 0;
	big = g_win.g.j.head;                                      /* the IOCTL's own flush may add J_JOURNAL_LOST later */
	CHECK_EQ(ioctl(GM20B_IOCTL_READ_JOURNAL, &v, 4, je, sizeof(je), &info), STATUS_SUCCESS);
	CHECK_EQ(je[0].seq, big - GM20B_J_RING);
	CHECK_EQ(je[15].seq, big - GM20B_J_RING + 15);
}

static void sc_raw(void)
{
	u32 v = 0, back;
	int k;

	SCENARIO("raw MMIO IOCTLs: off by default; policy when on; BAR guard");
	world_reset();
	preset_powered();
	set_val("MaxStage", 0);
	CHECK_EQ(load(), STATUS_SUCCESS);
	CHECK_EQ(raw_read(GM20B_SP_CAR, 0x280, &v), STATUS_ACCESS_DENIED);   /* AllowRawMmio=0 */
	k = find_rec(J_WIN_RAW_DENIED, 0, 0, 0);
	CHECK(k >= 0 && rec((u32)k).b == WIN_RAW_OFF);
	CHECK_EQ(raw_write(GM20B_SP_PMC, 0x2d4, 0), STATUS_ACCESS_DENIED);
	check_file();

	SCENARIO("raw MMIO IOCTLs: AllowRawMmio=1 (with TRACE_REGS, so core and shell records interleave)");
	world_reset();
	preset_powered();
	set_val("MaxStage", 0);
	set_val("AllowRawMmio", 1);
	set_val("Flags", GM20B_CF_TRACE_REGS);
	CHECK_EQ(load(), STATUS_SUCCESS);
	CHECK_EQ(raw_read(GM20B_SP_CAR, 0x280, &v), STATUS_SUCCESS);
	CHECK_EQ(v, GM20B_BIT(24));
	k = find_rec(J_WIN_RAWRD, 0, 0, 0);
	CHECK(k >= 0);
	CHECK(find_rec(J_WIN_RAWRD_DONE, 1, GM20B_JADDR(GM20B_SP_CAR, 0x280), (u32)k) > k);
	CHECK_EQ(raw_read(GM20B_SP_CAR, 0x284, &v), STATUS_ACCESS_DENIED);   /* write-only alias */
	CHECK_EQ(raw_write(GM20B_SP_CAR, 0x280, 0), STATUS_ACCESS_DENIED);   /* shared register: never written directly */
	CHECK_EQ(raw_write(GM20B_SP_CAR, 0x28c, 0), STATUS_ACCESS_DENIED);
	CHECK_EQ(raw_write(GM20B_SP_CAR, 0x300, 1), STATUS_ACCESS_DENIED);
	CHECK_EQ(mem_rd(GM20B_SP_CAR, 0x280), GM20B_BIT(24));
	CHECK_EQ(raw_write(GM20B_SP_CAR, 0x284, 0x1234), STATUS_SUCCESS);   /* SET alias is allowed */
	CHECK_EQ(mem_rd(GM20B_SP_CAR, 0x284), 0x1234);
	CHECK_EQ(raw_write(GM20B_SP_MC, 0x0, 1), STATUS_ACCESS_DENIED);      /* MC is read-only */
	CHECK_EQ(raw_read(GM20B_SP_MC, 0x0, &v), STATUS_SUCCESS);
	CHECK_EQ(raw_write(GM20B_SP_PMC, 0x2d8, 1), STATUS_ACCESS_DENIED);
	CHECK_EQ(raw_write(GM20B_SP_PMC, 0x2d4, 1), STATUS_SUCCESS);
	CHECK_EQ(mem_rd(GM20B_SP_PMC, 0x2d4), 1);
	CHECK_EQ(raw_read(GM20B_SP_PMC, 0x2d8, &v), STATUS_SUCCESS);
	CHECK_EQ(raw_read(GM20B_SP_CAR, 2, &v), STATUS_ACCESS_DENIED);       /* unaligned */
	CHECK_EQ(raw_read(GM20B_SP_CAR, 0x1000, &v), STATUS_ACCESS_DENIED);  /* beyond the window */
	CHECK_EQ(raw_read(GM20B_SP_PMC, 0x400, &v), STATUS_ACCESS_DENIED);   /* the PMC window is only 1 KiB */
	CHECK_EQ(raw_read(5, 0, &v), STATUS_ACCESS_DENIED);
	k = find_rec(J_WIN_RAW_DENIED, 1, GM20B_JADDR(GM20B_SP_CAR, 2), 0);
	CHECK(k >= 0 && rec((u32)k).b == WIN_RAW_BADREQ);
	/* BAR guard: GPU clamped (PMC GPU_RG_CNTRL bit 0 set by the write above) -> BAR0/BAR1 refused */
	CHECK_EQ(raw_read(GM20B_SP_BAR0, 0, &v), STATUS_ACCESS_DENIED);
	k = find_rec(J_WIN_RAW_DENIED, 1, GM20B_JADDR(GM20B_SP_BAR0, 0), 0);
	CHECK(k >= 0 && rec((u32)k).b == WIN_RAW_UNPOWERED);
	CHECK(tr_find(GM20B_SP_BAR0) < 0);                                   /* and really never touched */
	CHECK_EQ(raw_write(GM20B_SP_PMC, 0x2d4, 0), STATUS_SUCCESS);         /* unclamp */
	CHECK_EQ(raw_read(GM20B_SP_BAR0, 0, &v), STATUS_SUCCESS);
	CHECK_EQ(v, 0x12b000a1u);
	CHECK_EQ(raw_write(GM20B_SP_BAR0, 0x40, 0xdeadbeef), STATUS_SUCCESS);
	back = mem_rd(GM20B_SP_BAR0, 0x40);
	CHECK_EQ(back, 0xdeadbeefu);
	k = find_rec(J_WIN_RAWWR, 1, GM20B_JADDR(GM20B_SP_BAR0, 0x40), 0);
	CHECK(k >= 0 && rec((u32)k).b == 0xdeadbeefu);
	g_win.g.cfg.flags |= GM20B_CF_FORCE_BAR0;                            /* the guard can be bypassed explicitly */
	mem_wr(GM20B_SP_CAR, 0x280, 0);
	CHECK_EQ(raw_read(GM20B_SP_BAR1, 0, &v), STATUS_SUCCESS);
	/* buffer sizes */
	{
		struct gm20b_ioctl_rawrd rq = { GM20B_SP_CAR, 0x280 };
		struct gm20b_ioctl_rawwr rw = { GM20B_SP_PMC, 0x2d4, 0 };

		CHECK_EQ(ioctl(GM20B_IOCTL_RAW_READ, &rq, sizeof(rq), NULL, 0, NULL), STATUS_BUFFER_TOO_SMALL);
		CHECK_EQ(ioctl(GM20B_IOCTL_RAW_READ, &rq, 4, &v, 4, NULL), STATUS_BUFFER_TOO_SMALL);
		CHECK_EQ(ioctl(GM20B_IOCTL_RAW_WRITE, &rw, 8, NULL, 0, NULL), STATUS_BUFFER_TOO_SMALL);
	}
	check_file();
}

static void sc_unload(void)
{
	int a, b, i;
	size_t len;

	SCENARIO("unload: link and device deleted, stages torn down before the file closes, windows unmapped");
	world_reset();
	preset_powered();
	set_val("MaxStage", 2);
	CHECK_EQ(load(), STATUS_SUCCESS);
	drv.DriverUnload(&drv);
	CHECK(F.link_deleted && F.dev_deleted);
	CHECK_EQ(F.unmapped, GM20B_SP_COUNT);
	CHECK_EQ(F.file_closed, 1);
	CHECK_EQ(F.key_closed, 1);
	CHECK_EQ(F.params_closed, 1);
	CHECK(g_win.jf == NULL && g_win.key == NULL);
	CHECK_EQ(durable_last(F.flen), J_WIN_UNLOAD_DONE);
	a = find_rec(J_WIN_UNLOAD, 0, 0, 0);
	CHECK(a >= 0);
	b = find_rec(J_FINI_BEGIN, 1, 1, (u32)a);
	CHECK(b > a);                                              /* S1 fini ran, after the UNLOAD record */
	for (i = 0; i < GM20B_SP_COUNT; i++)
		CHECK_EQ(g_win.map[i] == NULL, 1);
	check_file();

	SCENARIO("a second load appends to the same file (no new header, new J_WIN_LOAD)");
	len = F.flen;
	CHECK_EQ(load(), STATUS_SUCCESS);
	CHECK(F.flen > len);
	CHECK_EQ(rec(nrec() > 0 ? find_rec(J_WIN_LOAD, 0, 0, (u32)((len - 32) / 24)) : 0).code, J_WIN_LOAD);
	i = find_rec(J_WIN_FILE, 0, 0, (u32)((len - 32) / 24));
	CHECK(i >= 0 && rec((u32)i).b == 0);                         /* opened, not created */
	check_file();
}

static void sc_file_failure(void)
{
	struct gm20b_ioctl_status st;
	struct gm20b_jentry je[4];
	ULONG_PTR info = 0;
	u32 v = 0;

	SCENARIO("journal file cannot be opened: the driver still loads, the ring and the registry breadcrumb still work");
	world_reset();
	F.create_status = (NTSTATUS)0xC000003AL;
	CHECK_EQ(load(), STATUS_SUCCESS);
	CHECK(g_win.jf == NULL);
	CHECK_EQ(F.flen, 0);
	CHECK(F.have_crumb);                                       /* the registry mirror still gets the newest record */
	status_field(&st);
	CHECK_EQ(st.jf_open_status, 0xC000003Au);
	CHECK_EQ(ioctl(GM20B_IOCTL_READ_JOURNAL, &v, 4, je, sizeof(je), &info), STATUS_SUCCESS);
	CHECK_EQ(info, sizeof(je));
	drv.DriverUnload(&drv);

	SCENARIO("journal writes start failing at run time: counted, never fatal");
	world_reset();
	set_val("MaxStage", 0);
	CHECK_EQ(load(), STATUS_SUCCESS);
	F.write_status = STATUS_UNSUCCESSFUL;
	v = 1;
	CHECK_EQ(ioctl(GM20B_IOCTL_ADVANCE, &v, 4, NULL, 0, NULL), STATUS_SUCCESS);
	status_field(&st);
	CHECK(st.jf_errors > 0);
	CHECK_EQ(st.jf_last_error, (u32)STATUS_UNSUCCESSFUL);
	CHECK(F.have_crumb);
	F.write_status = 0;
	drv.DriverUnload(&drv);
}

static void sc_map_failure(void)
{
	int k;
	u32 v = 2;
	int rc = 0;

	SCENARIO("one register window cannot be mapped: no hardware access at all, advance refuses, unload frees the rest");
	world_reset();
	preset_powered();
	F.fail_map_space = GM20B_SP_BAR1;
	set_val("MaxStage", 2);
	CHECK_EQ(load(), STATUS_SUCCESS);
	CHECK_EQ(g_win.init_status, GM20B_E_NOMEM);
	CHECK_EQ(g_win.g.stage_done, 0);
	CHECK_EQ(TR.n, 0);
	k = find_rec(J_WIN_MAP, 1, GM20B_SP_BAR1, 0);
	CHECK(k >= 0 && rec((u32)k).b == 0);
	CHECK_EQ(ioctl(GM20B_IOCTL_ADVANCE, &v, 4, &rc, 4, NULL), STATUS_SUCCESS);
	CHECK_EQ(rc, GM20B_E_STATE);
	CHECK_EQ(TR.n, 0);
	drv.DriverUnload(&drv);
	CHECK_EQ(F.unmapped, GM20B_SP_COUNT - 1);
	check_file();
}

static void sc_registry_values(void)
{
	SCENARIO("registry values with the wrong type are ignored (defaults are used and journaled as such)");
	world_reset();
	set_val("MaxStage", 5);
	F.vals[0].type = 1;                                        /* REG_SZ */
	set_val("Flags", 0x10);
	set_val("PollScalePct", 0);
	set_val("GpcKhz", 614400);
	set_val("RailUv", 900000);
	CHECK_EQ(load(), STATUS_SUCCESS);
	CHECK_EQ(g_win.g.cfg.max_stage, 0);
	CHECK_EQ(g_win.g.cfg.flags, 0x10);
	CHECK_EQ(g_win.g.cfg.poll_scale_pct, 100);                 /* 0 means 100 */
	CHECK_EQ(g_win.g.cfg.gpc_khz, 614400);
	CHECK_EQ(g_win.g.cfg.rail_uv, 900000);
	CHECK_EQ(g_win.allow_raw, 0);
	CHECK(g_win.g.hal.rail_gpu_set_uv == NULL);                /* the shell never provides a PMIC hook */
	{
		int k = find_rec(J_WIN_REG_MAXSTAGE, 0, 0, 0);

		CHECK(k >= 0 && rec((u32)k).a == 0 && rec((u32)k).b == 0);
	}
	check_file();

	SCENARIO("registry: no service key at all (breadcrumb impossible, everything else works)");
	world_reset();
	F.key_ok = 0;
	CHECK_EQ(load(), STATUS_SUCCESS);
	CHECK(!F.have_crumb);
	CHECK(F.flen > 32);
	CHECK(g_win.key == NULL);
	check_file();
}

static void sc_hal(void)
{
	struct gm20b_hal *h;
	struct gm20b_dma dm;
	struct gm20b_fw fw;
	u64 t0, t1, before;
	u32 i, nbad;
	int k, rc;
	size_t j;

	SCENARIO("HAL: refused accesses, timing, DMA, firmware");
	world_reset();
	preset_powered();
	set_val("MaxStage", 0);
	CHECK_EQ(load(), STATUS_SUCCESS);
	h = &g_win.g.hal;
	/* refused MMIO accesses return all-ones, write nothing and are journaled (and rate limited) */
	CHECK_EQ(h->rd32(h->ctx, GM20B_SP_BAR0, 3), 0xffffffffu);
	CHECK_EQ(h->rd32(h->ctx, GM20B_SP_PMC, 0x400), 0xffffffffu);
	CHECK_EQ(h->rd32(h->ctx, (enum gm20b_space)9, 0), 0xffffffffu);
	h->wr32(h->ctx, GM20B_SP_BAR0, 0x2, 0x55);
	CHECK_EQ(g_win.bad_access, 4);
	k = find_rec(J_WIN_BAD_ACCESS, 1, GM20B_JADDR(GM20B_SP_BAR0, 3), 0);
	CHECK(k >= 0 && rec((u32)k).b == 3);
	k = find_rec(J_WIN_BAD_ACCESS, 1, GM20B_JADDR(GM20B_SP_PMC, 0x400), 0);
	CHECK(k >= 0 && rec((u32)k).b == 4);
	for (i = 0; i < 100; i++)
		(void)h->rd32(h->ctx, GM20B_SP_BAR0, 1);
	nbad = 0;
	for (j = 0; j < nrec(); j++)
		if (rec((u32)j).code == J_WIN_BAD_ACCESS)
			nbad++;
	CHECK_EQ(nbad, 16);                                        /* a runaway loop cannot flood the file */
	CHECK_EQ(TR.n, 0);                                         /* and none of it reached the device */
	/* timing */
	t0 = h->now_ns(h->ctx);
	before = F.now_ns;
	h->udelay(h->ctx, 120);
	CHECK_EQ(F.stalls, 3);                                     /* 50 + 50 + 20 */
	CHECK_EQ(F.max_stall, 50);
	CHECK_EQ(F.now_ns - before, 120000);
	h->msleep(h->ctx, 3);
	CHECK_EQ(F.sleeps, 1);
	CHECK_EQ(F.now_ns - before, 120000 + 3000000);
	t1 = h->now_ns(h->ctx);
	CHECK(t1 - t0 >= 3120000 && t1 - t0 < 3120000 + 100000);   /* tick granularity of the fake counter */
	/* DMA: aligned by hand, zeroed with 32-bit stores, freed through the original pointer */
	rc = gm20b_dma_alloc(&g_win.g, &dm, 8192, 65536);
	CHECK_EQ(rc, 0);
	CHECK_EQ(dm.phys % 65536, 0);
	CHECK_EQ(dm.size, 8192);
	for (j = 0; j < 8192; j += 4)
		if (gm20b_dma_rd32(&dm, j) != 0) {
			CHECK(0);
			break;
		}
	gm20b_dma_wr64(&dm, 8, 0x1122334455667788ull);
	CHECK_EQ(gm20b_dma_rd64(&dm, 8), 0x1122334455667788ll);
	gm20b_dma_free(&g_win.g, &dm);
	CHECK_EQ(F.dma_allocs, 1);
	CHECK_EQ(F.dma_frees, 1);
	rc = gm20b_dma_alloc(&g_win.g, &dm, 100, 0);               /* default alignment: one page */
	CHECK_EQ(rc, 0);
	CHECK_EQ(dm.phys % 4096, 0);
	gm20b_dma_free(&g_win.g, &dm);
	CHECK_EQ(gm20b_dma_alloc(&g_win.g, &dm, 0, 4096), GM20B_E_INVAL);
	CHECK_EQ(gm20b_dma_alloc(&g_win.g, &dm, 4096, 3000), GM20B_E_INVAL);
	CHECK_EQ(gm20b_fw_get(&g_win.g, "nvidia/gm20b/acr_ucode.bin", &fw), GM20B_E_FW);
	check_file();
}

/* Every raw access, and the BAR guard's own CAR/PMC reads, must be announced durably first.  The trap is re-armed
 * before each IOCTL so that the FIRST access of that IOCTL to each window is observed. */
static void sc_raw_intent(void)
{
	u32 v = 0;
	int c, k;

	SCENARIO("raw MMIO: the intent record AND the registry crumb are durable BEFORE every register access");
	world_reset();
	preset_powered();
	set_val("MaxStage", 0);
	set_val("AllowRawMmio", 1);
	CHECK_EQ(load(), STATUS_SUCCESS);
	CHECK_EQ(TR.n, 0);

	trap_rearm();
	CHECK_EQ(raw_read(GM20B_SP_CAR, 0x280, &v), STATUS_SUCCESS);
	c = tr_find(GM20B_SP_CAR);
	CHECK(c >= 0);
	if (c >= 0)
		check_intent(c, J_WIN_RAWRD);

	trap_rearm();
	CHECK_EQ(raw_write(GM20B_SP_PMC, 0x2d4, 1), STATUS_SUCCESS);          /* clamp the GPU */
	c = tr_find(GM20B_SP_PMC);
	CHECK(c >= 0);
	if (c >= 0)
		check_intent(c, J_WIN_RAWWR);

	/* BAR0 while clamped: the guard reads CAR and PMC (announced first); BAR0 itself is never touched */
	trap_rearm();
	CHECK_EQ(raw_read(GM20B_SP_BAR0, 0, &v), STATUS_ACCESS_DENIED);
	c = tr_find(GM20B_SP_CAR);
	CHECK(c >= 0);
	if (c >= 0)
		check_intent(c, J_WIN_RAW_GUARD);
	c = tr_find(GM20B_SP_PMC);
	CHECK(c >= 0);
	if (c >= 0)
		check_intent(c, J_WIN_RAW_GUARD);
	CHECK(tr_find(GM20B_SP_BAR0) < 0);
	k = find_rec(J_WIN_RAW_GUARD, 1, GM20B_JADDR(GM20B_SP_BAR0, 0), 0);
	CHECK(k >= 0);

	/* unclamp, then read and write BAR0 */
	trap_rearm();
	CHECK_EQ(raw_write(GM20B_SP_PMC, 0x2d4, 0), STATUS_SUCCESS);
	trap_rearm();
	CHECK_EQ(raw_read(GM20B_SP_BAR0, 0, &v), STATUS_SUCCESS);
	CHECK_EQ(v, 0x12b000a1u);
	c = tr_find(GM20B_SP_CAR);
	CHECK(c >= 0);
	if (c >= 0)
		check_intent(c, J_WIN_RAW_GUARD);
	c = tr_find(GM20B_SP_BAR0);
	CHECK(c >= 0);
	if (c >= 0)
		check_intent(c, J_WIN_RAWRD);

	trap_rearm();
	CHECK_EQ(raw_write(GM20B_SP_BAR0, 0x40, 0x55aa55aau), STATUS_SUCCESS);
	c = tr_find(GM20B_SP_BAR0);
	CHECK(c >= 0);
	if (c >= 0)
		check_intent(c, J_WIN_RAWWR);

	/* with FORCE_BAR0 there is no guard and therefore no CAR/PMC access at all */
	g_win.g.cfg.flags |= GM20B_CF_FORCE_BAR0;
	trap_rearm();
	CHECK_EQ(raw_read(GM20B_SP_BAR1, 0, &v), STATUS_SUCCESS);
	CHECK(tr_find(GM20B_SP_CAR) < 0 && tr_find(GM20B_SP_PMC) < 0);
	c = tr_find(GM20B_SP_BAR1);
	CHECK(c >= 0);
	if (c >= 0)
		check_intent(c, J_WIN_RAWRD);
	check_file();
}

/* The file is capped at 2 MiB per load; past the cap only the registry crumb still follows the driver. */
static void sc_journal_cap(void)
{
	struct gm20b_ioctl_status st;
	struct gm20b_jentry cr;
	u32 v = 0, i, bad = 0, last_win = 0, n0;

	SCENARIO("journal file: capped per load, the registry crumb keeps following, nothing else breaks");
	world_reset();
	preset_powered();
	set_val("MaxStage", 0);
	set_val("AllowRawMmio", 1);
	CHECK_EQ(load(), STATUS_SUCCESS);
	for (i = 0; i < 200000u && !g_win.jf_dropped; i++)
		bad += raw_read(GM20B_SP_CAR, 0x280, &v) != STATUS_SUCCESS;
	CHECK_EQ(bad, 0);
	CHECK(g_win.jf_dropped > 0);
	CHECK(F.flen <= 32u + WIN_JF_SESSION_CAP);
	CHECK(F.flen + 1024u >= 32u + WIN_JF_SESSION_CAP);          /* it really filled up to the cap */
	CHECK_EQ((F.flen - 32u) % 24u, 0);                          /* and ends on a record boundary */
	n0 = nrec();
	for (i = 0; i < 50u; i++)
		bad += raw_read(GM20B_SP_CAR, 0x28c, &v) != STATUS_SUCCESS;   /* the driver keeps working */
	CHECK_EQ(bad, 0);
	CHECK_EQ(nrec(), n0);                                       /* the file did not grow */
	for (i = 0; i < n0; i++)
		if (rec(i).seq >= GM20B_JF_SEQ_WIN)
			last_win = rec(i).seq;
	CHECK(F.have_crumb_dur);
	memcpy(&cr, F.crumb_dur, sizeof(cr));
	CHECK(cr.seq > last_win);                                   /* the registry names an operation the file lacks */
	(void)status_field(&st);
	CHECK(st.jf_dropped > 0);
	CHECK_EQ(st.jf_errors, 0);
	drv.DriverUnload(&drv);
	CHECK_EQ(F.file_closed, 1);
}

/* win_rec() only queues; more than WIN_PEND_MAX records in a row must neither overrun the queue nor reorder. */
static void sc_pend_overflow(void)
{
	PDEVICE_OBJECT ctl;
	int link;
	u32 i, bad, base;

	SCENARIO("journal queue: 100 records without a commit neither overrun the queue nor get lost or reordered");
	world_reset();
	preset_powered();
	set_val("MaxStage", 0);
	CHECK_EQ(load(), STATUS_SUCCESS);
	ctl = g_win.ctl;
	link = g_win.link_created;
	bad = g_win.bad_access;
	for (i = 0; i < 100u; i++)
		win_rec(&g_win, J_WIN_DEVICE, 0x1000u + i, 0);
	win_sync(&g_win);
	CHECK(g_win.ctl == ctl);                                    /* the fields right behind pend[] are intact */
	CHECK_EQ(g_win.link_created, link);
	CHECK_EQ(g_win.bad_access, bad);
	CHECK(nrec() >= 100u);
	base = nrec() - 100u;
	for (i = 0; i < 100u; i++) {
		struct gm20b_jentry e = rec(base + i);

		CHECK_EQ(e.code, J_WIN_DEVICE);
		CHECK_EQ(e.a, 0x1000u + i);
	}
	check_file();
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	spaces_init();
	sc_defaults();
	sc_stage2();
	sc_cold_chip();
	sc_stub_stage();
	sc_ioctl();
	sc_raw();
	sc_raw_intent();
	sc_pend_overflow();
	sc_journal_cap();
	sc_unload();
	sc_file_failure();
	sc_map_failure();
	sc_registry_values();
	sc_hal();
	printf("\n%d checks, %d failed\n", nchk, nfail);
	return nfail ? 1 : 0;
}
