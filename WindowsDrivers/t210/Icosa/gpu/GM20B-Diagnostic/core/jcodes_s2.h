/* SPDX-License-Identifier: MIT */
/*
 * Journal codes for stage 2 (ID).  Range 0x2000-0x2fff.  Owner: the stage-2 implementer.
 * "STEP" = recorded AND flushed to disk before the operation.
 */
#define GM20B_JCODES_S2(X) \
	X(J_S2_GUARD_CLK,      0x2001) /* a=CLK_OUT_ENB_X off b=value (power check before touching BAR0) */ \
	X(J_S2_GUARD_RST,      0x2002) /* a=RST_DEVICES_X off b=value */ \
	X(J_S2_GUARD_CLAMP,    0x2003) /* a=PMC GPU_RG_CNTRL off b=value */ \
	X(J_S2_GATED,          0x2004) /* a=mask (1=gpu clock off 2=in reset 4=clamped): BAR0 NOT touched */ \
	X(J_S2_FORCED,         0x2005) /* a=mask: guard bypassed by GM20B_CF_FORCE_BAR0 (BAR0 will be touched) */ \
	X(J_S2_BOOT0_INTENT,   0x2010) /* STEP a=BAR0 addr of BOOT0: first BAR0 access of the session */ \
	X(J_S2_BOOT0,          0x2011) /* STEP a=boot0 value as read */ \
	X(J_S2_DECODE,         0x2012) /* a=chipset from nvgpu fields b=chipset from nouveau mask (must be equal) */ \
	X(J_S2_BAD_BOOT0,      0x2013) /* a=boot0 b=status returned */ \
	X(J_S2_WRONG_CHIP,     0x2014) /* a=chipset read b=chipset expected (0x12b) */ \
	X(J_S2_PMC_ENABLE,     0x2020) /* a=mc_enable off b=value (read only: S2 enables nothing) */ \
	X(J_S2_INTR_PRE,       0x2021) /* a=BAR0 off b=value (intr_en_0/1, intr_mask_0/1 before masking) */ \
	X(J_S2_INTR_MASK,      0x2022) /* about to mask all GPU interrupts in MC */ \
	X(J_S2_INTR_POST,      0x2023) /* a=BAR0 off b=value after masking */ \
	X(J_S2_INTR_NOT_MASKED, 0x2024) /* b=OR of the four registers, still non-zero after writing 0 (non-fatal) */ \
	X(J_S2_DIAG_ONLY,      0x2030) /* DIAG_ONLY: nothing written */
