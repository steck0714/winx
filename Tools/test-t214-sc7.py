#!/usr/bin/env python3
"""Exercise the applied ATF CPU_SUSPEND validator with BPMP success/faults."""
from pathlib import Path
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).resolve()
text = (source / "plat/nvidia/tegra/soc/t210/plat_psci_handlers.c").read_text()
start = text.index("int32_t tegra_soc_validate_power_state(")
end = text.index("\n}\n", start) + 3
validator = text[start:end]
assert "SWITCH_LITE_SC7" in validator, "Apply patch 0005 before running this test"
helper = (source / "plat/nvidia/tegra/soc/t210/t214/sc7.c").read_text()
helper = helper[helper.index("#define LITE_PMC_"):]

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <bpmp.h>
#define MPIDR_AFFLVL0 0
#define MPIDR_AFFLVL1 1
#define PLAT_MAX_PWR_LVL 2
#define PSTATE_ID_CORE_POWERDN 7
#define PSTATE_ID_CLUSTER_IDLE 16
#define PSTATE_ID_SOC_POWERDN 27
#define PLAT_MAX_OFF_STATE 28
#define PLAT_SYS_SUSPEND_STATE_ID 27
#define PSCI_E_SUCCESS 0
#define PSCI_E_NOT_SUPPORTED -1
#define PSCI_E_INVALID_PARAMS -2
#define PSCI_E_DENIED -3
#define PMC_CONFIG 0
#define ERROR(...) ((void)0)
typedef struct { uint8_t pwr_domain_state[3]; } psci_power_state_t;
typedef struct { uint64_t sc7entry_fw_base; } plat_params_from_bl2_t;
static plat_params_from_bl2_t params;
static int init_status, reply_status, init_calls, requests;
static bool b01;
static uint32_t pmc[0x200/4];
static unsigned pmc_writes, delays;
static bool wake_write_fault;
static const plat_params_from_bl2_t *bl31_get_plat_params(void) { return &params; }
static int psci_get_pstate_id(unsigned int state) { return state & 0xffff; }
#if SWITCH_LITE_SC7
static bool tegra_chipid_is_t210_b01(void) { return b01; }
#endif
int tegra_bpmp_init(void) { init_calls++; return init_status; }
int32_t tegra_bpmp_send_receive_atomic(int mrq, const void *out, int out_size,
                                     void *in, int in_size) {
    const uint32_t *words = out;
    assert(mrq == MRQ_ENABLE_SUSPEND && out_size == 8);
    assert(words[0] == TEGRA_PM_SC7 && words[1] == 0);
    assert(in != NULL && in_size == 0); /* No payload is defined by this MRQ. */
    requests++;
    return reply_status;
}
#if SWITCH_LITE_SC7
static uint32_t tegra_pmc_read_32(uint32_t offset) {
    assert(offset < sizeof(pmc) && (offset & 3) == 0);
    return pmc[offset/4];
}
static void tegra_pmc_write_32(uint32_t offset, uint32_t value) {
    assert(requests == 1 && reply_status == 0);
    assert(offset < sizeof(pmc) && (offset & 3) == 0);
    pmc_writes++;
    if (offset == 0x160 && wake_write_fault) return;
    if (offset == 0x168) {
        assert(value == (1u << 19)); /* Preserve other pending wake events. */
        pmc[offset/4] &= ~value;
    } else pmc[offset/4] = value;
}
static void udelay(unsigned int us) {
    assert(us == 120);
    assert(delays < 2);
    assert(!!(pmc[0] & (1u << 5)) == (delays == 0));
    delays++;
}
#endif
'''
harness += "\n#if SWITCH_LITE_SC7\n" + helper + "\n#endif\n"
harness += validator
harness += r'''
static void run(unsigned int state, int expected, unsigned expected_requests) {
    psci_power_state_t out = { { 0, 0, 0 } }, before = out;
    requests = init_calls = pmc_writes = delays = 0;
    memset(pmc, 0, sizeof(pmc));
    pmc[0] = 0x20001; /* Preserve the PMIC interrupt inversion. */
    pmc[0x0c/4] = 0x13579;
    pmc[0x10/4] = 0x24680;
    pmc[0x14/4] = 0x5500;
    pmc[0x160/4] = 0x12345;
    pmc[0x164/4] = 0x87654;
    pmc[0x168/4] = (1u << 19) | 0x80;
    assert(tegra_soc_validate_power_state(state, &out) == expected);
    assert(requests == (int)expected_requests);
    if (expected_requests && reply_status == 0) {
        assert(delays == 2 && pmc_writes == 7);
        assert(pmc[0] == 0x20001);
        assert(pmc[0x0c/4] == 0x13579 && pmc[0x10/4] == 0x24680);
        assert(pmc[0x14/4] == 0x5500 && pmc[0x168/4] == 0x80);
        assert(pmc[0x164/4] == (0x87654 & ~(1u << 19)));
        assert(pmc[0x160/4] == (wake_write_fault ? 0x12345 : 0x92345));
    } else assert(pmc_writes == 0 && delays == 0);
    if (expected != 0) assert(memcmp(&out, &before, sizeof(out)) == 0);
    else if ((state & 0xffff) == PSTATE_ID_SOC_POWERDN) {
        assert(init_calls == 1);
        assert(out.pwr_domain_state[0] == PLAT_MAX_OFF_STATE);
        assert(out.pwr_domain_state[1] == PLAT_MAX_OFF_STATE);
        assert(out.pwr_domain_state[2] == PLAT_SYS_SUSPEND_STATE_ID);
    } else {
        assert(init_calls == 0 && requests == 0);
        assert(out.pwr_domain_state[0] == PSTATE_ID_CORE_POWERDN);
        assert(out.pwr_domain_state[1] == ((state & 0xffff) == 16 ? 16 : 0));
        assert(out.pwr_domain_state[2] == 0);
    }
}
int main(void) {
    const unsigned sc7 = 0x0201001b;
    b01 = true;
    run(0x00010007, 0, 0); run(0x01010010, 0, 0);
    run(0x020100ff, PSCI_E_INVALID_PARAMS, 0);
    run(sc7, 0, SWITCH_LITE_SC7);
    /* Repeat entry must obtain a new acknowledgement, including after resume. */
    reply_status = -110;
    run(sc7, SWITCH_LITE_SC7 ? PSCI_E_DENIED : 0, SWITCH_LITE_SC7);
    reply_status = -22;
    run(sc7, SWITCH_LITE_SC7 ? PSCI_E_DENIED : 0, SWITCH_LITE_SC7);
    reply_status = 1;
    run(sc7, SWITCH_LITE_SC7 ? PSCI_E_DENIED : 0, SWITCH_LITE_SC7);
    reply_status = 0; wake_write_fault = true;
    run(sc7, SWITCH_LITE_SC7 ? PSCI_E_DENIED : 0, SWITCH_LITE_SC7);
    wake_write_fault = false;
    reply_status = 0; init_status = -1;
    run(sc7, PSCI_E_NOT_SUPPORTED, 0);
    params.sc7entry_fw_base = 0x80000000;
    run(sc7, SWITCH_LITE_SC7 ? PSCI_E_NOT_SUPPORTED : 0, 0);
    /* Keep the original non-B01 SC7-entry fallback unchanged. */
    b01 = false;
    run(sc7, 0, 0);
    params.sc7entry_fw_base = 0;
    run(sc7, PSCI_E_NOT_SUPPORTED, 0);
    init_status = 0;
    run(sc7, 0, 0);
    puts("PASS: SC7 acknowledgement, PMIC wake routing, failures and legacy paths");
}
'''
with tempfile.TemporaryDirectory(prefix="lite-sc7-") as temporary:
    directory = Path(temporary)
    c = directory / "test.c"
    c.write_text(harness)
    for enabled in (0, 1):
        binary = directory / f"test-{enabled}"
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", f"-DSWITCH_LITE_SC7={enabled}",
            "-I", str(source / "plat/nvidia/tegra/include/drivers"),
            str(c), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
