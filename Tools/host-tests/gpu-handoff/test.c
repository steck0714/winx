// SPDX-License-Identifier: GPL-2.0-only
// Host test for Library/PlatformBootManagerLib/GpuHandoff.h with a register model.
#include <stdio.h>
#include <string.h>
#include "../../../Library/PlatformBootManagerLib/GpuHandoff.h"

struct model {
    uint32_t hidrev, clock_x, reset_x, clamp, mc_flush;
    bool mc_drains;      /* MC_FLUSH_STATUS follows the request */
    bool bus_error;      /* every read returns all ones */
    unsigned writes;
    uint32_t last_clamp_write_mc_status; /* MC status when the clamp was written */
};

static uint32_t rd(void *c, uint32_t a)
{
    struct model *m = c;
    if (m->bus_error) return UINT32_MAX;
    switch (a) {
    case GH_APB_MISC_HIDREV: return m->hidrev;
    case GH_CAR_CLOCK_X:     return m->clock_x;
    case GH_CAR_RESET_X:     return m->reset_x;
    case GH_PMC_GPU_CLAMP:   return m->clamp;
    case GH_MC_FLUSH:        return m->mc_flush;
    case GH_MC_FLUSH_STATUS: return (m->mc_drains && (m->mc_flush & GH_MC_GPU_BIT)) ? GH_MC_GPU_BIT : 0;
    }
    return 0;
}
static void wr(void *c, uint32_t a, uint32_t v)
{
    struct model *m = c;
    m->writes++;
    switch (a) {
    case GH_CAR_CLOCK_SET: m->clock_x |= v; break;
    case GH_CAR_CLOCK_CLR: m->clock_x &= ~v; break;
    case GH_CAR_RESET_SET: m->reset_x |= v; break;
    case GH_CAR_RESET_CLR: m->reset_x &= ~v; break;
    case GH_PMC_GPU_CLAMP:
        m->last_clamp_write_mc_status = rd(c, GH_MC_FLUSH_STATUS);
        m->clamp = v; break;
    case GH_MC_FLUSH:      m->mc_flush = v; break;
    default: break;
    }
}
static void dl(void *c, uint32_t us) { (void)c; (void)us; }

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

static struct model mk(uint32_t hid, bool cold)
{
    struct model m = { .hidrev = hid, .mc_drains = true };
    m.reset_x = cold ? GH_GPU_GATE : 0;
    m.clamp = cold ? 1 : 0;
    m.clock_x = cold ? 0 : GH_GPU_CLOCKS;
    return m;
}

int main(void)
{
    struct gpu_handoff_report r;
    struct model m;
    struct gpu_handoff_io io;
    enum gpu_handoff_result res;

    memset(&io, 0, sizeof io);
    io.read32 = rd; io.write32 = wr; io.delay_us = dl;

    /* 1. Cold GPU on each board: nothing is written, twice in a row. */
    for (int board = 0; board < 2; ++board) {
        uint32_t hid = board ? 0x2120u : 0x2110u;
        m = mk(hid | 0x1, true); io.context = &m;   /* low nibble = minor rev */
        res = gpu_handoff_prepare(&io, hid, &r);
        CHECK(res == GPU_HANDOFF_COLD); CHECK(m.writes == 0); CHECK(!r.wrote);
        res = gpu_handoff_prepare(&io, hid, &r);
        CHECK(res == GPU_HANDOFF_COLD); CHECK(m.writes == 0);
    }

    /* 2. Running GPU (clocks on, reset released, unclamped): isolated, drained first. */
    m = mk(0x2121, false); io.context = &m;
    res = gpu_handoff_prepare(&io, GH_HIDREV_T210B01, &r);
    CHECK(res == GPU_HANDOFF_RESTORED); CHECK(r.wrote);
    CHECK(gh_is_cold(m.reset_x, m.clamp)); CHECK((m.clock_x & GH_GPU_CLOCKS) == 0);
    CHECK(m.last_clamp_write_mc_status & GH_MC_GPU_BIT);   /* clamp only after MC drained */
    CHECK((m.mc_flush & GH_MC_GPU_BIT) == 0);               /* our flush request released */
    CHECK(gpu_handoff_prepare(&io, GH_HIDREV_T210B01, &r) == GPU_HANDOFF_COLD);

    /* 3. Flush request already set by someone else stays set. */
    m = mk(0x2111, false); m.mc_flush = GH_MC_GPU_BIT; io.context = &m;
    CHECK(gpu_handoff_prepare(&io, GH_HIDREV_T210, &r) == GPU_HANDOFF_RESTORED);
    CHECK(m.mc_flush & GH_MC_GPU_BIT);

    /* 4. MC never drains: clamp/reset/clock are NOT touched. */
    m = mk(0x2121, false); m.mc_drains = false; io.context = &m;
    res = gpu_handoff_prepare(&io, GH_HIDREV_T210B01, &r);
    CHECK(res == GPU_HANDOFF_DRAIN_TIMEOUT);
    CHECK(m.clamp == 0); CHECK(m.reset_x == 0); CHECK(m.clock_x == GH_GPU_CLOCKS);
    CHECK((m.mc_flush & GH_MC_GPU_BIT) == 0);

    /* 5. Wrong SoC: Erista build on a T210B01 and the reverse. Nothing is written. */
    m = mk(0x2121, false); io.context = &m;
    CHECK(gpu_handoff_prepare(&io, GH_HIDREV_T210, &r) == GPU_HANDOFF_WRONG_SOC);
    CHECK(m.writes == 0);
    m = mk(0x2111, false); io.context = &m;
    CHECK(gpu_handoff_prepare(&io, GH_HIDREV_T210B01, &r) == GPU_HANDOFF_WRONG_SOC);
    CHECK(m.writes == 0);

    /* 6. Bus error (all ones): nothing is written. */
    m = mk(0x2121, false); m.bus_error = true; io.context = &m;
    CHECK(gpu_handoff_prepare(&io, GH_HIDREV_T210B01, &r) == GPU_HANDOFF_BUS_ERROR);
    CHECK(m.writes == 0);

    /* 7. Bad arguments. */
    CHECK(gpu_handoff_prepare(NULL, GH_HIDREV_T210, &r) == GPU_HANDOFF_BAD_ARGUMENT);
    memset(&io, 0, sizeof io);
    CHECK(gpu_handoff_prepare(&io, GH_HIDREV_T210, NULL) == GPU_HANDOFF_BAD_ARGUMENT);

    printf(failures ? "FAILED (%d)\n" : "PASS\n", failures);
    return failures != 0;
}
