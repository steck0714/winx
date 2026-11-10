/* SPDX-License-Identifier: MIT */
/*
 * gm20bctl.exe -- user-mode companion of gm20b.sys.  Freestanding (kernel32 only, no CRT), so it builds with just
 * clang + lld-link.
 *
 *   gm20bctl status                          state of the running driver
 *   gm20bctl journal [from_seq]              the driver's in-memory journal (last 512 records) with names
 *   gm20bctl advance <stage>                 run more stages (0..8); stages already done are kept
 *   gm20bctl fini                            tear every stage down (GPU back into reset)
 *   gm20bctl rawread  <space> <off>          needs AllowRawMmio=1;  space = bar0 bar1 car pmc mc (or 0..4)
 *   gm20bctl rawwrite <space> <off> <value>  same; refused for MC, for CAR except the SET/CLR aliases, for PMC
 *                                            except GPU_RG_CNTRL, and for BAR0/BAR1 while the GPU is not powered
 *   gm20bctl decode [file]                   decode a journal FILE (default C:\Windows\gm20b-journal.bin);
 *                                            works without the driver, e.g. after a hang and a reboot
 */
#include <stdarg.h>
#include <stddef.h>
#include "gm20b_ioctl.h"
#include "gm20b_journal_file.h"
#include "gm20b_jcodes.h"
#include "gm20b_jcodes_win.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef void *HANDLE;
typedef unsigned long DWORD;
typedef int BOOL;

#define K32 __declspec(dllimport)
K32 HANDLE CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
K32 BOOL ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
K32 BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
K32 BOOL CloseHandle(HANDLE);
K32 HANDLE GetStdHandle(DWORD);
K32 BOOL DeviceIoControl(HANDLE, DWORD, void *, DWORD, void *, DWORD, DWORD *, void *);
K32 const char *GetCommandLineA(void);
K32 DWORD GetLastError(void);
K32 void ExitProcess(unsigned int);

#define GENERIC_READ     0x80000000u
#define GENERIC_WRITE    0x40000000u
#define SHARE_READ       1u
#define SHARE_WRITE      2u
#define OPEN_EXISTING    3u
#define BAD_HANDLE       ((HANDLE)(long long)-1)
#define STD_OUT          ((DWORD)-11)

struct jentry {
	u32 seq, code, a, b;
	u64 t_ns;
};
_Static_assert(sizeof(struct jentry) == GM20B_JF_ENTRY_SIZE, "journal entry layout");

/* ---- memory helpers the compiler may call -------------------------------------------------------------------- */
#define NOOPT __attribute__((optnone, noinline))
NOOPT void *memset(void *d, int c, size_t n)
{
	u8 *p = (u8 *)d;

	while (n--)
		*p++ = (u8)c;
	return d;
}

NOOPT void *memcpy(void *d, const void *s, size_t n)
{
	u8 *p = (u8 *)d;
	const u8 *q = (const u8 *)s;

	while (n--)
		*p++ = *q++;
	return d;
}

/* ---- tiny printf ---------------------------------------------------------------------------------------------- */
static char obuf[4096];
static u32 olen;

static void oflush(void)
{
	DWORD w;

	if (olen)
		WriteFile(GetStdHandle(STD_OUT), obuf, olen, &w, 0);
	olen = 0;
}

static void oc(char c)
{
	if (olen == sizeof(obuf))
		oflush();
	obuf[olen++] = c;
}

static void emit_str(const char *s, u32 w, int left)
{
	u32 n = 0;

	while (s[n])
		n++;
	for (; !left && n < w; w--)
		oc(' ');
	for (u32 i = 0; s[i]; i++)
		oc(s[i]);
	for (; left && n < w; w--)
		oc(' ');
}

static void emit_num(u64 v, u32 base, int neg, u32 w, char pad, int left)
{
	char t[24];
	u32 n = 0, total;

	do {
		u32 dgt = (u32)(v % base);

		t[n++] = (char)(dgt < 10 ? '0' + dgt : 'a' + dgt - 10);
		v /= base;
	} while (v && n < sizeof(t));
	total = n + (neg ? 1u : 0u);
	if (!left && pad == '0' && neg)
		oc('-');
	for (; !left && total < w; w--)
		oc(pad);
	if (!(pad == '0' && !left && neg) && neg)
		oc('-');
	while (n)
		oc(t[--n]);
	for (; left && total < w; w--)
		oc(' ');
}

static void pf(const char *f, ...)
{
	va_list ap;

	va_start(ap, f);
	for (; *f; f++) {
		int left = 0, ll = 0;
		char pad = ' ';
		u32 w = 0;

		if (*f != '%') {
			oc(*f);
			continue;
		}
		f++;
		if (*f == '-') {
			left = 1;
			f++;
		}
		if (*f == '0') {
			pad = '0';
			f++;
		}
		while (*f >= '0' && *f <= '9')
			w = w * 10 + (u32)(*f++ - '0');
		while (*f == 'l') {
			ll++;
			f++;
		}
		switch (*f) {
		case 'u':
			emit_num(ll ? va_arg(ap, u64) : va_arg(ap, u32), 10, 0, w, pad, left);
			break;
		case 'x':
			emit_num(ll ? va_arg(ap, u64) : va_arg(ap, u32), 16, 0, w, pad, left);
			break;
		case 'd': {
			long long v = ll ? va_arg(ap, long long) : (long long)va_arg(ap, int);

			emit_num(v < 0 ? (u64)(-v) : (u64)v, 10, v < 0, w, pad, left);
			break;
		}
		case 's':
			emit_str(va_arg(ap, const char *), w, left);
			break;
		case 'c':
			oc((char)va_arg(ap, int));
			break;
		case '%':
			oc('%');
			break;
		default:
			oc('?');
			break;
		}
	}
	va_end(ap);
}

/* ---- names ---------------------------------------------------------------------------------------------------- */
struct jn {
	u32 code;
	const char *name;
};
static const struct jn jnames[] = {
#define X(n, v) { v, #n },
	GM20B_JCODES_ALL(X)
	GM20B_JCODES_WIN(X)
#undef X
};

static const char *jname(u32 code)
{
	for (u32 i = 0; i < sizeof(jnames) / sizeof(jnames[0]); i++)
		if (jnames[i].code == code)
			return jnames[i].name;
	return "J_?";
}

static const char *const stage_names[9] = { "MAP", "POWER", "ID", "CLK", "PRIV", "MEM", "FIFO", "ACR", "GR" };
static const char *const err_names[11] = { "OK", "TIMEOUT", "IO", "NOMEM", "NODEV", "NOTIMPL", "INVAL", "FW",
					   "GATED", "STATE", "HW" };
static const char *const space_names[5] = { "BAR0", "BAR1", "CAR", "PMC", "MC" };

static const char *stage_name(u32 s)
{
	return s < 9 ? stage_names[s] : "?";
}

static const char *err_name(int e)
{
	return (e <= 0 && -e <= 10) ? err_names[-e] : "ERR?";
}

static void hint(const struct jentry *e)
{
	switch (e->code) {
	case J_STAGE_BEGIN: case J_STAGE_OK: case J_STAGE_SKIP: case J_FINI_BEGIN: case J_FINI_OK:
		pf("  [%s]", stage_name(e->a));
		break;
	case J_STAGE_FAIL:
		pf("  [%s: %s]", stage_name(e->a), err_name((int)e->b));
		break;
	case J_INIT_DONE:
		pf("  [stages done %u, %s]", e->a, err_name((int)e->b));
		break;
	case J_WIN_INIT_DONE:
		pf("  [%s, stages done %u]", err_name((int)e->a), e->b);
		break;
	case J_WIN_IOCTL_DONE:
		pf("  [%s]", err_name((int)e->b));
		break;
	case J_S2_BOOT0:
		pf("  [chipset %03x rev %02x]", (e->a >> 20) & 0x1ffu, e->a & 0xffu);
		break;
	case J_RD: case J_WR: case J_WAIT_TIMEOUT: case J_WAIT_OK: case J_S2_BOOT0_INTENT: case J_WIN_BAD_ACCESS:
	case J_WIN_RAWRD: case J_WIN_RAWRD_DONE: case J_WIN_RAWWR: case J_WIN_RAWWR_DONE: case J_WIN_RAW_DENIED:
		if ((e->a >> 28) < 5)
			pf("  [%s+%x]", space_names[e->a >> 28], e->a & 0x0fffffffu);
		break;
	default:
		break;
	}
}

static void print_entry(const struct jentry *e)
{
	pf("%08x %6llu.%03u ms  %-20s a=%08x b=%08x", e->seq, e->t_ns / 1000000ull, (u32)((e->t_ns / 1000ull) % 1000ull),
	   jname(e->code), e->a, e->b);
	hint(e);
	pf("\n");
}

/* ---- command line --------------------------------------------------------------------------------------------- */
static char cmdbuf[1024];
static char *argv_[16];

static int split_args(void)
{
	const char *s = GetCommandLineA();
	int argc = 0, inq = 0;
	u32 n = 0;
	char *w = cmdbuf;

	while (*s && n < sizeof(cmdbuf) - 1) {
		while (*s == ' ' || *s == '\t')
			s++;
		if (!*s)
			break;
		if (argc < 15)
			argv_[argc++] = w;
		while (*s && (inq || (*s != ' ' && *s != '\t')) && n < sizeof(cmdbuf) - 2) {
			if (*s == '"') {
				inq = !inq;
				s++;
				continue;
			}
			*w++ = *s++;
			n++;
		}
		*w++ = 0;
		n++;
	}
	return argc;
}

static int parse_num(const char *s, u32 *out)
{
	u64 v = 0;
	u32 base = 10;

	if (!*s)
		return -1;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
		if (!*s)
			return -1;
	}
	for (; *s; s++) {
		u32 d;

		if (*s >= '0' && *s <= '9')
			d = (u32)(*s - '0');
		else if (base == 16 && *s >= 'a' && *s <= 'f')
			d = (u32)(*s - 'a' + 10);
		else if (base == 16 && *s >= 'A' && *s <= 'F')
			d = (u32)(*s - 'A' + 10);
		else
			return -1;
		v = v * base + d;
		if (v > 0xffffffffull)
			return -1;
	}
	*out = (u32)v;
	return 0;
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static int parse_space(const char *s, u32 *out)
{
	for (u32 i = 0; i < 5; i++) {
		char lc[8];
		u32 k = 0;

		for (; space_names[i][k] && k < 7; k++)
			lc[k] = (char)(space_names[i][k] | 0x20);   /* lower-case the upper-case names */
		lc[k] = 0;
		for (k = 0; lc[k]; k++)
			if (lc[k] >= '0' && lc[k] <= '9')
				lc[k] = space_names[i][k];            /* digits keep their value */
		if (streq(s, lc)) {
			*out = i;
			return 0;
		}
	}
	return parse_num(s, out) == 0 && *out < 5 ? 0 : -1;
}

/* ---- device --------------------------------------------------------------------------------------------------- */
static HANDLE open_dev(void)
{
	HANDLE h = CreateFileA(GM20B_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);

	if (h == BAD_HANDLE) {
		pf("cannot open %s (Win32 error %u).\n"
		   "Is gm20b.sys running (sc start gm20b) and is this an elevated prompt? "
		   "The device is exclusive: close other gm20bctl instances.\n",
		   GM20B_USER_PATH, (u32)GetLastError());
		return 0;
	}
	return h;
}

static int ioctl_(HANDLE h, u32 code, void *in, u32 inlen, void *out, u32 outlen, u32 *ret)
{
	DWORD got = 0;
	BOOL ok = DeviceIoControl(h, code, in, inlen, out, outlen, &got, 0);

	if (ret)
		*ret = (u32)got;
	if (!ok)
		pf("DeviceIoControl %08x failed: Win32 error %u\n", code, (u32)GetLastError());
	return ok;
}

static int cmd_status(HANDLE h)
{
	struct gm20b_ioctl_status s;
	u32 ret = 0;

	if (!ioctl_(h, GM20B_IOCTL_GET_STATUS, 0, 0, &s, sizeof(s), &ret) || ret < sizeof(s))
		return 1;
	pf("driver version    %08x   ioctl abi %u%s\n", s.driver_version, s.abi,
	   s.abi == GM20B_IOCTL_ABI ? "" : "  (MISMATCH with this tool)");
	pf("max stage         %u (%s)\n", s.max_stage, stage_name(s.max_stage));
	pf("stages done       %u%s%s\n", s.stage_done, s.stage_done ? "  last completed: " : "",
	   s.stage_done ? stage_name(s.stage_done - 1u) : "");
	pf("last init status  %d (%s)\n", s.init_status, err_name(s.init_status));
	if (s.last_err)
		pf("failed            stage %u (%s) with %d (%s)\n", s.fail_stage, stage_name(s.fail_stage), s.last_err,
		   err_name(s.last_err));
	pf("flags             %08x   AllowRawMmio=%u\n", s.flags, s.allow_raw);
	pf("journal           recorded %u, handed to the file %u, lost %u\n", s.j_head, s.j_flushed, s.j_lost);
	pf("journal file      open status %08x, %u bytes written, %u write errors (last %08x), %u dropped (cap)\n",
	   s.jf_open_status, s.jf_bytes, s.jf_errors, s.jf_last_error, s.jf_dropped);
	pf("registry crumb    %u errors      refused MMIO accesses %u\n", s.bc_errors, s.bad_access);
	if (s.boot0)
		pf("BOOT0             %08x   chipset %03x rev %02x\n", s.boot0, s.chipset, s.chiprev);
	if (s.gpc_count)
		pf("GPC/TPC/LTC       %u / %u / %u   SMMU enabled: %u\n", s.gpc_count, s.tpc_total, s.ltc_count,
		   s.smmu_enabled);
	return 0;
}

static int cmd_journal(HANDLE h, u32 from)
{
	static struct jentry buf[64];
	u32 ret = 0, n = 0;

	for (;;) {
		u32 i, cnt;

		if (!ioctl_(h, GM20B_IOCTL_READ_JOURNAL, &from, 4, buf, sizeof(buf), &ret))
			return 1;
		cnt = ret / (u32)sizeof(struct jentry);
		if (!cnt)
			break;
		for (i = 0; i < cnt; i++)
			print_entry(&buf[i]);
		from = buf[cnt - 1].seq + 1u;
		n += cnt;
	}
	pf("-- %u records (the full history is in %s)\n", n, GM20B_JF_PATH_USER);
	return 0;
}

static int cmd_advance(HANDLE h, u32 stage)
{
	int rc = 0;
	u32 ret = 0;

	if (!ioctl_(h, GM20B_IOCTL_ADVANCE, &stage, 4, &rc, 4, &ret))
		return 1;
	pf("advance(%u = %s): status %d (%s)\n", stage, stage_name(stage), rc, err_name(rc));
	return rc ? 1 : 0;
}

static int cmd_rawread(HANDLE h, u32 sp, u32 off)
{
	struct gm20b_ioctl_rawrd rq;
	u32 v = 0, ret = 0;

	rq.space = sp;
	rq.off = off;
	if (!ioctl_(h, GM20B_IOCTL_RAW_READ, &rq, sizeof(rq), &v, 4, &ret))
		return 1;
	pf("%s+%x = %08x\n", space_names[sp], off, v);
	return 0;
}

static int cmd_rawwrite(HANDLE h, u32 sp, u32 off, u32 val)
{
	struct gm20b_ioctl_rawwr rq;

	rq.space = sp;
	rq.off = off;
	rq.val = val;
	if (!ioctl_(h, GM20B_IOCTL_RAW_WRITE, &rq, sizeof(rq), 0, 0, 0))
		return 1;
	pf("%s+%x <- %08x\n", space_names[sp], off, val);
	return 0;
}

static int cmd_decode(const char *path)
{
	static u8 rbuf[sizeof(struct jentry) * 128];
	struct gm20b_jf_header hdr;
	struct jentry last;
	HANDLE f = CreateFileA(path, GENERIC_READ, SHARE_READ | SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
	DWORD got = 0;
	u32 have = 0, total = 0, loads = 0;

	if (f == BAD_HANDLE) {
		pf("cannot open %s (Win32 error %u)\n", path, (u32)GetLastError());
		return 1;
	}
	if (!ReadFile(f, &hdr, sizeof(hdr), &got, 0) || got != sizeof(hdr) || hdr.magic != GM20B_JF_MAGIC ||
	    hdr.entry_size != GM20B_JF_ENTRY_SIZE) {
		pf("%s is not a gm20b journal file\n", path);
		CloseHandle(f);
		return 1;
	}
	memset(&last, 0, sizeof(last));
	for (;;) {
		u32 off = 0;

		if (!ReadFile(f, rbuf + have, (DWORD)(sizeof(rbuf) - have), &got, 0) || got == 0)
			break;
		have += (u32)got;
		while (have - off >= sizeof(struct jentry)) {
			struct jentry e;

			memcpy(&e, rbuf + off, sizeof(e));      /* the buffer offset is not 8-aligned: copy, never cast */
			off += (u32)sizeof(e);
			if (e.code == J_WIN_LOAD)
				pf("=== driver load #%u ===\n", ++loads);
			print_entry(&e);
			last = e;
			total++;
		}
		have -= off;
		memcpy(rbuf, rbuf + off, have);
	}
	CloseHandle(f);
	pf("-- %u records in %u driver load(s)\n", total, loads);
	if (total) {
		pf("LAST RECORD:  ");
		print_entry(&last);
		pf("If the machine hung, it hung AFTER this record was made durable: look at the operation this record\n"
		   "announces (records marked STEP in core/jcodes_*.h announce the next hardware access).\n");
	}
	return 0;
}

static int real_main(void)
{
	int argc = split_args();
	const char *c;
	HANDLE h;
	u32 a, b, d;
	int rc = 1;

	if (argc < 2) {
		pf("usage: gm20bctl status | journal [from_seq] | advance <stage> | fini | rawread <space> <off> |\n"
		   "                rawwrite <space> <off> <value> | decode [file]\n");
		return 2;
	}
	c = argv_[1];
	if (streq(c, "decode")) {
		rc = cmd_decode(argc > 2 ? argv_[2] : GM20B_JF_PATH_USER);
		goto out;
	}
	h = open_dev();
	if (!h)
		return 1;
	if (streq(c, "status")) {
		rc = cmd_status(h);
	} else if (streq(c, "journal")) {
		a = 0;
		if (argc > 2 && parse_num(argv_[2], &a))
			pf("bad number: %s\n", argv_[2]);
		else
			rc = cmd_journal(h, a);
	} else if (streq(c, "advance")) {
		if (argc < 3 || parse_num(argv_[2], &a) || a > 8)
			pf("usage: gm20bctl advance <0..8>\n");
		else
			rc = cmd_advance(h, a);
	} else if (streq(c, "fini")) {
		rc = ioctl_(h, GM20B_IOCTL_FINI, 0, 0, 0, 0, 0) ? 0 : 1;
		if (!rc)
			pf("all stages torn down (GPU held in reset, clocks we enabled are off)\n");
	} else if (streq(c, "rawread")) {
		if (argc < 4 || parse_space(argv_[2], &a) || parse_num(argv_[3], &b))
			pf("usage: gm20bctl rawread <bar0|bar1|car|pmc|mc> <offset>\n");
		else
			rc = cmd_rawread(h, a, b);
	} else if (streq(c, "rawwrite")) {
		if (argc < 5 || parse_space(argv_[2], &a) || parse_num(argv_[3], &b) || parse_num(argv_[4], &d))
			pf("usage: gm20bctl rawwrite <bar0|bar1|car|pmc|mc> <offset> <value>\n");
		else
			rc = cmd_rawwrite(h, a, b, d);
	} else {
		pf("unknown command: %s\n", c);
	}
	CloseHandle(h);
out:
	oflush();
	return rc;
}

void mainCRTStartup(void)
{
	int rc = real_main();

	oflush();
	ExitProcess((unsigned int)rc);
}
