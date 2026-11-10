/* SPDX-License-Identifier: MIT */
/* Every vendored NVIDIA register header must compile together under -Werror, and a few known values hold. */
#include "gm20b.h"
#include "test.h"
#include "nvgpu/hw_bus_gm20b.h"
#include "nvgpu/hw_ccsr_gm20b.h"
#include "nvgpu/hw_ce2_gm20b.h"
#include "nvgpu/hw_ctxsw_prog_gm20b.h"
#include "nvgpu/hw_falcon_gm20b.h"
#include "nvgpu/hw_fb_gm20b.h"
#include "nvgpu/hw_fifo_gm20b.h"
#include "nvgpu/hw_flush_gm20b.h"
#include "nvgpu/hw_fuse_gm20b.h"
#include "nvgpu/hw_gmmu_gm20b.h"
#include "nvgpu/hw_gr_gm20b.h"
#include "nvgpu/hw_ltc_gm20b.h"
#include "nvgpu/hw_mc_gm20b.h"
#include "nvgpu/hw_pbdma_gm20b.h"
#include "nvgpu/hw_perf_gm20b.h"
#include "nvgpu/hw_pram_gm20b.h"
#include "nvgpu/hw_pri_ringmaster_gm20b.h"
#include "nvgpu/hw_pri_ringstation_gpc_gm20b.h"
#include "nvgpu/hw_pri_ringstation_sys_gm20b.h"
#include "nvgpu/hw_proj_gm20b.h"
#include "nvgpu/hw_pwr_gm20b.h"
#include "nvgpu/hw_ram_gm20b.h"
#include "nvgpu/hw_therm_gm20b.h"
#include "nvgpu/hw_timer_gm20b.h"
#include "nvgpu/hw_top_gm20b.h"
#include "nvgpu/hw_trim_gm20b.h"

TEST(regs_vendor_headers_compile_together)
{
	T_EQ(mc_boot_0_r(), 0x00000000u);
	T_EQ(mc_enable_r(), 0x00000200u);
}
