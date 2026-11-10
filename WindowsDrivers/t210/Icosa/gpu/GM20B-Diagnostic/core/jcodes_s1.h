/* SPDX-License-Identifier: MIT */
/*
 * Journal codes for stage 1 (POWER).  Range 0x1000-0x1fff.  Owner: the stage-1 implementer.
 * "STEP" = recorded AND flushed to disk before the operation.  The LAST line of a journal
 * from a machine that hung names the culprit.
 */
#define GM20B_JCODES_S1(X) \
	X(J_S1_PROBE_BEGIN,   0x1001) /* STEP: about to do the first CAR/PMC read of the session */ \
	X(J_S1_PRE_CLK,       0x1002) /* a=CLK_OUT_ENB_X off b=value (pre-state) */ \
	X(J_S1_PRE_RST,       0x1003) /* a=RST_DEVICES_X off b=value (bit24 = GPU in reset) */ \
	X(J_S1_PRE_CLAMP,     0x1004) /* a=PMC GPU_RG_CNTRL off b=value (bit0 = gated/clamped) */ \
	X(J_S1_PRE_PLLP_BASE, 0x1005) /* a=PLLP_BASE off b=value */ \
	X(J_S1_PRE_PLLP_OUTC, 0x1006) /* a=PLLP_OUTC off b=value */ \
	X(J_S1_PWR_CLK,       0x1007) /* a=1 PLLP_OUT5 looks running / 0 not  b=derived kHz (expect 204000) */ \
	X(J_S1_PROBE_RD,      0x1008) /* STEP a=(space<<28|off): about to read this CAR/PMC register (one line per register) */ \
	X(J_S1_RAIL_SKIP,     0x1010) /* a=0: ALLOW_RAIL not set, rail untouched */ \
	X(J_S1_RAIL_REFUSED,  0x1011) /* a=rail_uv b=1 if hook present: request refused, nothing touched */ \
	X(J_S1_RAIL_SET,      0x1012) /* STEP a=uv */ \
	X(J_S1_RAIL_DONE,     0x1013) /* a=uv b=hook return value */ \
	X(J_S1_DIAG_ONLY,     0x1020) /* DIAG_ONLY: probes done, no writes */ \
	X(J_S1_CLK_ENABLE,    0x1030) /* STEP a=CLK_OUT_ENB_SET_X b=bits */ \
	X(J_S1_RST_ASSERT,    0x1031) /* STEP a=RST_DEVICES_SET_X b=bits */ \
	X(J_S1_CLAMP_REMOVE,  0x1032) /* STEP a=PMC GPU_RG_CNTRL off b=0 written */ \
	X(J_S1_RST_DEASSERT,  0x1033) /* STEP a=RST_DEVICES_CLR_X b=bits */ \
	X(J_S1_POST_CLK,      0x1040) /* a=CLK_OUT_ENB_X off b=value (post-state) */ \
	X(J_S1_POST_RST,      0x1041) /* a=RST_DEVICES_X off b=value */ \
	X(J_S1_POST_CLAMP,    0x1042) /* a=GPU_RG_CNTRL off b=value */ \
	X(J_S1_PLLG_REF,      0x1043) /* a=1 pllg_ref gate bit is set after enable, 0 not (needed by S3 only) */ \
	X(J_S1_VERIFY_FAIL,   0x1044) /* STEP a=mask of failures (1=gpu clock off 2=still in reset 4=still clamped) */ \
	X(J_S1_FINI_RST,      0x1050) /* STEP a=RST_DEVICES_SET_X b=bits (hold GPU in reset) */ \
	X(J_S1_FINI_CLK,      0x1051) /* STEP a=CLK_OUT_ENB_CLR_X b=bits (only clocks WE enabled) */ \
	X(J_S1_FINI_POST,     0x1052) /* a=RST_DEVICES_X value b=CLK_OUT_ENB_X value */
