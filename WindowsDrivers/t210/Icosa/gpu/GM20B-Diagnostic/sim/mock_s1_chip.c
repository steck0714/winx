/* SPDX-License-Identifier: MIT */
#include "mock_s1_chip.h"

#include <string.h>

#define GPU_BIT 0x01000000u /* bit 24 */

void mock_chip_update(struct mock_chip *c)
{
	bool alive = c->rail_on && (c->clk_x & GPU_BIT) && !(c->rst_x & GPU_BIT) && !(c->rg & 1u);

	mock_space_alive(c->m, GM20B_SP_BAR0, alive);
}

static u32 car_rd(struct mock *m, void *user, enum gm20b_space sp, u32 off, u32 cur)
{
	struct mock_chip *c = user;

	(void)sp;
	switch (off) {
	case 0x280: return c->clk_x;
	case 0x28c: return c->rst_x;
	case 0x284: case 0x288: case 0x290: case 0x294:
		mock_violation(m, "read of write-only CAR alias 0x%x", off);
		return 0;
	default:
		return cur;
	}
}

static void car_wr(struct mock *m, void *user, enum gm20b_space sp, u32 off, u32 val, u32 old)
{
	struct mock_chip *c = user;

	(void)sp; (void)old;
	switch (off) {
	case 0x284: c->clk_x |= val;  c->n_alias_wr++; break;
	case 0x288: c->clk_x &= ~val; c->n_alias_wr++; break;
	case 0x290: c->rst_x |= val;  c->n_alias_wr++; break;
	case 0x294: c->rst_x &= ~val; c->n_alias_wr++; break;
	case 0x280:
		c->clk_x = val;
		mock_violation(m, "direct write to shared CLK_OUT_ENB_X (use SET/CLR alias)");
		break;
	case 0x28c:
		c->rst_x = val;
		mock_violation(m, "direct write to shared RST_DEVICES_X (use SET/CLR alias)");
		break;
	default:
		mock_violation(m, "write to shared CAR register 0x%x", off);
		return;
	}
	mock_chip_update(c);
}

static u32 pmc_rd(struct mock *m, void *user, enum gm20b_space sp, u32 off, u32 cur)
{
	struct mock_chip *c = user;

	(void)m; (void)sp;
	return off == 0x2d4 ? c->rg : cur;
}

static void pmc_wr(struct mock *m, void *user, enum gm20b_space sp, u32 off, u32 val, u32 old)
{
	struct mock_chip *c = user;

	(void)sp; (void)old;
	if (off != 0x2d4) {
		mock_violation(m, "write to PMC register 0x%x (only GPU_RG_CNTRL is allowed)", off);
		return;
	}
	if (!c->pmc_wr_ignored)
		c->rg = val;
	mock_chip_update(c);
}

void mock_chip_install(struct mock *m, struct mock_chip *c, bool cold)
{
	memset(c, 0, sizeof(*c));
	c->m = m;
	c->rail_on = true;
	if (cold) {
		c->clk_x = 0;
		c->rst_x = GPU_BIT;
		c->rg = 1;
	} else {
		c->clk_x = GPU_BIT | 0x20000000u;
		c->rst_x = 0;
		c->rg = 0;
	}
	mock_hook(m, GM20B_SP_CAR, 0x000, 0xfff, car_rd, car_wr, c);
	mock_hook(m, GM20B_SP_PMC, 0x000, 0x3ff, pmc_rd, pmc_wr, c);
	mock_chip_update(c);
}
