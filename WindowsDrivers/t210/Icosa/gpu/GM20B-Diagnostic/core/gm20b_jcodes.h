/* SPDX-License-Identifier: MIT */
/*
 * Journal event codes (X-macro). Shared by the core, the Windows driver and the
 * user-mode tool so a journal file can be decoded without guessing.
 *
 * Namespaces:  0x0000-0x0fff core,  0x1000-0x1fff stage 1, 0x2000-0x2fff stage 2, ...
 * Each stage owns its own jcodes_sN.h; do not put stage codes anywhere else.
 */
#ifndef GM20B_JCODES_H
#define GM20B_JCODES_H

#define GM20B_JCODES_CORE(X) \
	X(J_NONE,          0x0000) \
	X(J_STAGE_BEGIN,   0x0001) /* a=stage */ \
	X(J_STAGE_OK,      0x0002) /* a=stage */ \
	X(J_STAGE_FAIL,    0x0003) /* a=stage b=err */ \
	X(J_STAGE_SKIP,    0x0004) /* a=stage (gated by cfg.max_stage) */ \
	X(J_FINI_BEGIN,    0x0005) /* a=stage */ \
	X(J_FINI_OK,       0x0006) /* a=stage */ \
	X(J_NOTE,          0x0007) /* a,b free-form */ \
	X(J_INIT_BEGIN,    0x0008) /* a=max_stage b=flags */ \
	X(J_INIT_DONE,     0x0009) /* a=stage_done b=err */ \
	X(J_WAIT_TIMEOUT,  0x0010) /* a=(space<<28|off) b=last value */ \
	X(J_WAIT_OK,       0x0011) /* a=(space<<28|off) b=value (only when tracing) */ \
	X(J_RD,            0x0020) /* a=(space<<28|off) b=value   (cfg TRACE_REGS) */ \
	X(J_WR,            0x0021) /* a=(space<<28|off) b=value   (cfg TRACE_REGS) */ \
	X(J_PROBE,         0x0022) /* a=tag/offset b=value: diagnostic read-out */ \
	X(J_ERR,           0x0030) /* a=where b=err */ \
	X(J_JOURNAL_LOST,  0x0031) /* a=entries overwritten before flush */

#include "jcodes_s1.h"
#include "jcodes_s2.h"
#include "jcodes_s3.h"
#include "jcodes_s4.h"
#include "jcodes_s5.h"
#include "jcodes_s6.h"
#include "jcodes_s7.h"
#include "jcodes_s8.h"

#define GM20B_JCODES_ALL(X) \
	GM20B_JCODES_CORE(X) GM20B_JCODES_S1(X) GM20B_JCODES_S2(X) GM20B_JCODES_S3(X) \
	GM20B_JCODES_S4(X) GM20B_JCODES_S5(X) GM20B_JCODES_S6(X) GM20B_JCODES_S7(X) \
	GM20B_JCODES_S8(X)

enum gm20b_jcode {
#define X(name, val) name = (val),
	GM20B_JCODES_ALL(X)
#undef X
};

/* a-field helper for register-related events. */
#define GM20B_JADDR(sp, off)  ((((u32)(sp)) << 28) | ((u32)(off) & 0x0fffffffu))

#endif /* GM20B_JCODES_H */
