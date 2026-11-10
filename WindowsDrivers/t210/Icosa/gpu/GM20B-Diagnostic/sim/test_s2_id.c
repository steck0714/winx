/* SPDX-License-Identifier: MIT */
/* Stage 2 (ID) and the S0-S2 pipeline against the chip model in mock_s1_chip.c. */
#include "mock.h"
#include "mock_s1_chip.h"
#include "test.h"

#include <string.h>

#define GPU_BIT 0x01000000u

struct fx {
	struct mock *m;
	struct mock_chip chip;
	struct gm20b g;
};

static void fx_init(struct fx *f, bool cold, u32 max_stage, u32 flags, u32 boot0)
{
	struct gm20b_hal hal;
	struct gm20b_cfg cfg;

	f->m = mock_new();
	mock_chip_install(f->m, &f->chip, cold);
	mock_set(f->m, GM20B_SP_BAR0, 0x0, boot0);
	mock_hal(f->m, &hal);
	memset(&cfg, 0, sizeof(cfg));
	cfg.max_stage = max_stage;
	cfg.flags = flags;
	gm20b_ctx_init(&f->g, &hal, &cfg);
}

static void fx_done(struct fx *f)
{
	mock_free(f->m);
}

static u32 count_space(struct mock *m, enum gm20b_space sp, u32 from)
{
	u32 i, n = 0;

	for (i = from; i < mock_ntrace(m); i++)
		if (mock_trace(m, i)->space == (u8)sp)
			n++;
	return n;
}

static u32 count_bar0_writes(struct mock *m)
{
	u32 i, n = 0;

	for (i = 0; i < mock_ntrace(m); i++)
		if (mock_trace(m, i)->space == (u8)GM20B_SP_BAR0 && mock_trace(m, i)->is_wr)
			n++;
	return n;
}

static int first_bar0(struct mock *m)
{
	u32 i;

	for (i = 0; i < mock_ntrace(m); i++)
		if (mock_trace(m, i)->space == (u8)GM20B_SP_BAR0)
			return (int)i;
	return -1;
}

TEST(s2_reads_boot0_first_and_fills_info)
{
	struct fx f;
	int i, j;

	fx_init(&f, false, 0, 0, 0x12b000a1u);
	T_EQ(gm20b_s2_init(&f.g), GM20B_OK);
	T_EQ(f.g.info.boot0, 0x12b000a1u);
	T_EQ(f.g.info.chipset, 0x12b);
	T_EQ(f.g.info.chiprev, 0xa1);
	i = first_bar0(f.m);
	T_ASSERT(i >= 0);
	T_EQ(mock_trace(f.m, (u32)i)->off, 0);
	T_EQ(mock_trace(f.m, (u32)i)->is_wr, 0);
	j = mock_jfind(f.m, J_S2_BOOT0_INTENT, 0xffffffffu, 0);       /* intent flushed first */
	T_ASSERT(j >= 0 && (u32)j < mock_trace(f.m, (u32)i)->jlen);
	T_EQ(mock_nviolations(f.m), 0);
	fx_done(&f);
}

TEST(s2_guard_blocks_bar0_when_not_powered)
{
	static const u32 masks[3] = { 1, 2, 4 };
	u32 k;

	for (k = 0; k < 3; k++) {
		struct fx f;

		fx_init(&f, false, 0, 0, 0x12b000a1u);
		if (masks[k] == 1)
			f.chip.clk_x = 0;                 /* clock gate off */
		if (masks[k] == 2)
			f.chip.rst_x = GPU_BIT;           /* held in reset */
		if (masks[k] == 4)
			f.chip.rg = 1;                    /* clamped */
		mock_chip_update(&f.chip);
		T_EQ(gm20b_s2_init(&f.g), GM20B_E_STATE);
		gm20b_jflush(&f.g);
		T_EQ(count_space(f.m, GM20B_SP_BAR0, 0), 0);
		T_EQ(mock_nviolations(f.m), 0);
		T_ASSERT(mock_jfind(f.m, J_S2_GATED, masks[k], 0) >= 0);
		fx_done(&f);
	}
}

TEST(s2_force_flag_reads_bar0_anyway)
{
	struct fx f;

	fx_init(&f, true, 0, GM20B_CF_FORCE_BAR0, 0x12b000a1u);   /* cold: the mock BAR0 is dead */
	T_EQ(gm20b_s2_init(&f.g), GM20B_E_IO);                    /* reads all-ones */
	T_EQ(count_space(f.m, GM20B_SP_BAR0, 0), 1);
	T_EQ(mock_nviolations(f.m), 1);                           /* exactly the forced access */
	gm20b_jflush(&f.g);
	T_ASSERT(mock_jfind(f.m, J_S2_FORCED, 0xffffffffu, 0) >= 0);
	fx_done(&f);
}

TEST(s2_bad_boot0_variants)
{
	static const u32 vals[5] = { 0x00000000u, 0xffffffffu, 0xbadf5040u, 0x13b000a1u, 0x12a000a1u };
	static const int want[5] = { GM20B_E_NODEV, GM20B_E_IO, GM20B_E_IO, GM20B_E_NODEV, GM20B_E_NODEV };
	u32 k;

	for (k = 0; k < 5; k++) {
		struct fx f;
		int jb;

		fx_init(&f, false, 0, 0, vals[k]);
		T_EQ(gm20b_s2_init(&f.g), want[k]);
		gm20b_jflush(&f.g);
		jb = mock_jfind(f.m, J_S2_BOOT0, vals[k], 0);
		T_ASSERT(jb >= 0);
		/* a failed run re-reads the power state so the log explains itself */
		T_ASSERT(mock_jfind(f.m, J_S2_GUARD_CLK, 0xffffffffu, (u32)jb + 1) >= 0);
		T_EQ(count_bar0_writes(f.m), 0);
		fx_done(&f);
	}
}

TEST(s2_masks_interrupts_and_enables_nothing)
{
	struct fx f;

	fx_init(&f, false, 0, 0, 0x12b000a1u);
	mock_set(f.m, GM20B_SP_BAR0, 0x140, 1);
	mock_set(f.m, GM20B_SP_BAR0, 0x144, 1);
	mock_set(f.m, GM20B_SP_BAR0, 0x640, 0xffff);
	mock_set(f.m, GM20B_SP_BAR0, 0x644, 0x10);
	mock_set(f.m, GM20B_SP_BAR0, 0x200, 0x1234);              /* PMC_ENABLE */
	T_EQ(gm20b_s2_init(&f.g), GM20B_OK);
	T_EQ(mock_get(f.m, GM20B_SP_BAR0, 0x140), 0);
	T_EQ(mock_get(f.m, GM20B_SP_BAR0, 0x144), 0);
	T_EQ(mock_get(f.m, GM20B_SP_BAR0, 0x640), 0);
	T_EQ(mock_get(f.m, GM20B_SP_BAR0, 0x644), 0);
	T_EQ(mock_get(f.m, GM20B_SP_BAR0, 0x200), 0x1234);
	T_EQ(mock_find(f.m, GM20B_SP_BAR0, 0x200, 1, 0), -1);     /* never written */
	T_EQ(count_bar0_writes(f.m), 4);                          /* only the four mask registers */
	T_EQ(f.g.s2.pmc_enable, 0x1234);
	fx_done(&f);
}

TEST(s2_diag_only_writes_nothing)
{
	struct fx f;

	fx_init(&f, false, 0, GM20B_CF_DIAG_ONLY, 0x12b000a1u);
	mock_set(f.m, GM20B_SP_BAR0, 0x640, 0xffff);
	T_EQ(gm20b_s2_init(&f.g), GM20B_OK);
	T_EQ(count_bar0_writes(f.m), 0);
	T_EQ(mock_get(f.m, GM20B_SP_BAR0, 0x640), 0xffff);
	fx_done(&f);
}

TEST(pipeline_s0_to_s2_cold_chip)
{
	struct fx f;
	int last_car_wr, b0;

	fx_init(&f, true, 2, 0, 0x12b000a1u);
	T_EQ(gm20b_init(&f.g), GM20B_OK);
	T_EQ(f.g.info.chipset, 0x12b);
	T_EQ(mock_nviolations(f.m), 0);
	last_car_wr = mock_find(f.m, GM20B_SP_CAR, 0x294, 1, 0);   /* reset deassert = last S1 write */
	b0 = first_bar0(f.m);
	T_ASSERT(last_car_wr >= 0 && b0 > last_car_wr);            /* BAR0 only after the power-up */
	b0 = (int)mock_ntrace(f.m);
	gm20b_fini(&f.g);
	T_ASSERT(f.chip.rst_x & GPU_BIT);
	T_EQ(count_space(f.m, GM20B_SP_BAR0, (u32)b0), 0);        /* teardown never touches BAR0 */
	T_EQ(mock_nviolations(f.m), 0);
	T_EQ(mock_dma_live(f.m), 0);
	fx_done(&f);
}

TEST(pipeline_stage1_failure_never_reaches_bar0)
{
	struct fx f;

	fx_init(&f, true, 2, 0, 0x12b000a1u);
	f.chip.pmc_wr_ignored = true;
	T_EQ(gm20b_init(&f.g), GM20B_E_HW);
	T_EQ(f.g.fail_stage, 1);
	T_EQ(count_space(f.m, GM20B_SP_BAR0, 0), 0);
	T_EQ(mock_nviolations(f.m), 0);
	fx_done(&f);
}

TEST(pipeline_diag_only_on_cold_chip_is_safe)
{
	struct fx f;

	fx_init(&f, true, 2, GM20B_CF_DIAG_ONLY, 0x12b000a1u);
	T_EQ(gm20b_init(&f.g), GM20B_E_STATE);                    /* S1 only looked, S2's guard refuses */
	T_EQ(f.g.fail_stage, 2);
	T_EQ(count_space(f.m, GM20B_SP_BAR0, 0), 0);
	T_EQ(mock_nviolations(f.m), 0);
	fx_done(&f);
}
