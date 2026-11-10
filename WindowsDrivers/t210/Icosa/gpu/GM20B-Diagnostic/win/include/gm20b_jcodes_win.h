/* SPDX-License-Identifier: MIT */
/*
 * Journal codes written by the Windows shell itself.  Range 0x0f00-0x0fff (inside the core namespace, unused by
 * the core).  These records carry seq >= 0xF0000000 and are interleaved with the core's records in file order.
 * "STEP" = made durable BEFORE the operation it announces (the last line of a hung run names the culprit).
 */
#ifndef GM20B_JCODES_WIN_H
#define GM20B_JCODES_WIN_H

#define GM20B_JCODES_WIN(X) \
	X(J_WIN_LOAD,          0x0f01) /* a=driver version b=ioctl ABI: a new driver load starts here */ \
	X(J_WIN_FILE,          0x0f02) /* a=NTSTATUS opening the journal file b=1 if created by this load */ \
	X(J_WIN_KEY,           0x0f03) /* a=NTSTATUS opening the service key b=NTSTATUS opening Parameters */ \
	X(J_WIN_REG_MAXSTAGE,  0x0f10) /* a=value used b=1 from registry / 0 default */ \
	X(J_WIN_REG_FLAGS,     0x0f11) /* a=value used b=1 from registry / 0 default */ \
	X(J_WIN_REG_GPCKHZ,    0x0f12) /* a=value used b=1 from registry / 0 default */ \
	X(J_WIN_REG_RAILUV,    0x0f13) /* a=value used b=1 from registry / 0 default */ \
	X(J_WIN_REG_POLLPCT,   0x0f14) /* a=value used b=1 from registry / 0 default */ \
	X(J_WIN_REG_RAWMMIO,   0x0f15) /* a=value used b=1 from registry / 0 default */ \
	X(J_WIN_MAP,           0x0f20) /* a=space index b=low 32 bits of the mapped VA (0 = MmMapIoSpaceEx failed) */ \
	X(J_WIN_INIT_BEGIN,    0x0f30) /* STEP a=max_stage b=flags: gm20b_init is about to run */ \
	X(J_WIN_INIT_DONE,     0x0f31) /* a=gm20b status (0 ok, negative = GM20B_E_*) b=stages done */ \
	X(J_WIN_DEVICE,        0x0f40) /* a=NTSTATUS IoCreateDevice b=NTSTATUS IoCreateSymbolicLink */ \
	X(J_WIN_IOCTL,         0x0f41) /* STEP a=ioctl code b=argument (ADVANCE: new max stage) */ \
	X(J_WIN_IOCTL_DONE,    0x0f42) /* a=ioctl code b=gm20b status */ \
	X(J_WIN_UNLOAD,        0x0f50) /* STEP a=stages done: DriverUnload begins (fini follows) */ \
	X(J_WIN_UNLOAD_DONE,   0x0f51) /* the driver is gone */ \
	X(J_WIN_BAD_ACCESS,    0x0f62) /* a=(space<<28|off) b=1 bad space 2 unmapped 3 unaligned 4 out of range */ \
	X(J_WIN_RAWRD,         0x0f70) /* STEP a=(space<<28|off): raw read about to happen */ \
	X(J_WIN_RAWRD_DONE,    0x0f71) /* a=(space<<28|off) b=value */ \
	X(J_WIN_RAWWR,         0x0f72) /* STEP a=(space<<28|off) b=value: raw write about to happen */ \
	X(J_WIN_RAWWR_DONE,    0x0f73) /* a=(space<<28|off) */ \
	X(J_WIN_RAW_DENIED,    0x0f74) /* a=(space<<28|off) b=1 AllowRawMmio off 2 policy 3 GPU not powered 4 bad request */ \
	X(J_WIN_RAW_GUARD,     0x0f75) /* STEP a=(space<<28|off) of a BAR0/BAR1 access: CAR+PMC are about to be read to see whether the GPU is powered */

enum gm20b_jcode_win {
#define X(name, val) name = (val),
	GM20B_JCODES_WIN(X)
#undef X
};

#endif
