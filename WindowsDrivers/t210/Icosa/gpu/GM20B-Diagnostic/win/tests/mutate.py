#!/usr/bin/env python3
"""Mutation sanity check: break the code on purpose in a scratch copy and see whether the tests notice.

KILLED   = a test failed (good: the test suite guards that behaviour)
SURVIVED = everything still passes (bad: a hole in the tests, or an equivalent mutant)
INVALID  = the mutant did not apply or did not compile (fix the mutation, not the code)
"""
import concurrent.futures as cf
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

SRC = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
OWN_WORK = not os.environ.get("MUTATE_WORK")
WORK = os.environ.get("MUTATE_WORK") or tempfile.mkdtemp(prefix="gm20b-mutate-")
RESULTS = os.path.join(WORK, "results.txt")

# (id, description, file, pattern, replacement, kind, regex, expected_count)
M = [
    ("M00", "control: no change", None, None, None, "host", False, 0),
    ("M01", "win_jf_commit: no ZwFlushBuffersFile", "win/driver/win_journal.c",
     "\t\t(void)ZwFlushBuffersFile(d->jf, &iosb);\n", "\t\t(void)iosb;\n", "host", False, 1),
    ("M02", "raw_policy: direct CAR writes allowed", "win/driver/gm20b_sys.c",
     "if (is_write != (int)is_alias)", "if (!is_write && is_alias)", "host", False, 1),
    ("M03", "win_rec: core ring not flushed first", "win/driver/win_journal.c",
     r"\tif \(d->g_valid\)\n\t\tgm20b_jflush\(&d->g\);\s*/\* older core records go to the file first \*/\n", "",
     "host", True, 1),
    ("M04", "S1: clamp removal before reset assert", "core/stage1_power.c",
     r"(\tGM20B_STEP\(g, J_S1_RST_ASSERT.*?gm20b_udelay\(g, 10\);\n\n)"
     r"(\tGM20B_STEP\(g, J_S1_CLAMP_REMOVE.*?gm20b_udelay\(g, 10\);\n\n)", r"\2\1", "core", True, 1),
    ("M05", "win_bad_access: not committed", "win/driver/win_journal.c",
     r"(win_rec\(d, J_WIN_BAD_ACCESS[^\n]*\n)\t\twin_jf_commit\(d, NULL, 0\);\n", r"\1", "host", True, 1),
    ("M06", "breadcrumb: no ZwFlushKey", "win/driver/win_journal.c",
     r"\t\tst = ZwFlushKey\(d->key\);\n", "", "host", True, 1),
    ("M07", "hal: unaligned MMIO not rejected", "win/driver/hal_win.c",
     r"\telse if \(off & 3u\)\n\t\twhy = 3;[^\n]*\n", "", "host", True, 1),
    ("M08", "hal: out-of-range MMIO not rejected", "win/driver/hal_win.c",
     "else if (off > d->map_size[sp] - 4u)", "else if (0)", "host", False, 1),
    ("M09", "IOCTL FINI keeps max_stage", "win/driver/gm20b_sys.c",
     r"\n[ \t]*d->g\.cfg\.max_stage = 0;[^\n]*", "", "host", True, 1),
    ("M10", "DriverEntry fails when bring-up failed", "win/driver/gm20b_sys.c",
     r"return STATUS_SUCCESS;\s*/\* stay loaded even if the bring-up failed \*/",
     "return g_win.init_status ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;", "host", True, 1),
    ("M11", "IOCTL busy flag disabled", "win/driver/gm20b_sys.c",
     "if (__sync_lock_test_and_set(&d->busy, 1)) {", "if (0) {", "host", False, 1),
    ("M12", "raw: BAR guard removed", "win/driver/gm20b_sys.c",
     r"if \(gpu_unpowered\(d\)\)", "if (0)", "host", True, 1),
    ("M13", "raw: MC writes allowed", "win/driver/gm20b_sys.c",
     "if (sp == (u32)GM20B_SP_MC && is_write)", "if (0)", "host", False, 1),
    ("M14", "raw: any PMC write allowed", "win/driver/gm20b_sys.c",
     "&& is_write && off != PMC_GPU_RG_CNTRL)", "&& is_write && 0)", "host", False, 1),
    ("M15", "win_step: not committed", "win/driver/win_journal.c",
     r"(void win_step\([^)]*\)\n\{\n\twin_rec\(d, code, a, b\);\n)\twin_jf_commit\(d, NULL, 0\);\n", r"\1",
     "host", True, 1),
    ("M16", "S1: clamp removal only recorded, not flushed", "core/stage1_power.c",
     "GM20B_STEP(g, J_S1_CLAMP_REMOVE,", "gm20b_j(g, J_S1_CLAMP_REMOVE,", "core", False, 1),
    ("M17", "S1: clock enable through the shared register", "core/stage1_power.c",
     "car_wr(g, CAR_CLK_OUT_ENB_SET_X, CLK_BITS);", "car_wr(g, CAR_CLK_OUT_ENB_X, CLK_BITS);", "core", False, 1),
    ("M18", "journal: 2 MiB cap removed", "win/driver/win_journal.c",
     "if (d->jf_bytes + len > WIN_JF_SESSION_CAP) {", "if (0) {", "host", False, 1),
    ("M19", "journal file header magic wrong", "win/driver/win_journal.c",
     "h.magic = GM20B_JF_MAGIC;", "h.magic = 0;", "host", False, 1),
    ("M20", "shell records without the 0xF0000000 seq base", "win/driver/win_journal.c",
     "e->seq = GM20B_JF_SEQ_WIN + d->rec_seq++;", "e->seq = d->rec_seq++;", "host", False, 1),
    ("M21", "pend[] overflow by one", "win/driver/win_journal.c",
     "if (d->npend >= WIN_PEND_MAX)", "if (d->npend > WIN_PEND_MAX)", "host", False, 1),
    ("M22", "journal file opened without WRITE_THROUGH", "win/driver/win_journal.c",
     " | FILE_WRITE_THROUGH", "", "host", False, 1),
    ("M23", "journal writes at offset 0 instead of append", "win/driver/win_journal.c",
     "off.QuadPart = -1;", "off.QuadPart = 0;", "host", False, 1),
    ("M24", "raw: aliases readable", "win/driver/gm20b_sys.c",
     "if (is_write != (int)is_alias)", "if (is_write && !is_alias)", "host", False, 1),
    ("M25", "unload/FINI: gm20b_fini never called", "win/driver/gm20b_sys.c",
     r"\n[ \t]*gm20b_fini\([^;]*;", "", "host", True, None),
    ("M26", "raw guard: no intent record before reading CAR/PMC", "win/driver/gm20b_sys.c",
     r"\t\twin_step\(d, J_WIN_RAW_GUARD,[^\n]*\n", "", "host", True, 1),
    ("M27", "raw guard: intent only queued, not durable", "win/driver/gm20b_sys.c",
     "win_step(d, J_WIN_RAW_GUARD,", "win_rec(d, J_WIN_RAW_GUARD,", "host", False, 1),
    ("M28", "S1 rail: lower voltage bound removed", "core/stage1_power.c",
     "g->cfg.rail_uv < GM20B_RAIL_MIN_UV ||", "0 ||", "core", False, 1),
    ("M29", "S1 rail: hook called without ALLOW_RAIL", "core/stage1_power.c",
     "if (g->cfg.flags & GM20B_CF_ALLOW_RAIL) {", "if (g->hal.rail_gpu_set_uv) {", "core", False, 1),
    ("M30", "S2: wrong chip accepted", "core/stage2_id.c",
     "if (chipset != chipset_nv || chipset != GM20B_CHIPSET_GM20B) {", "if (0) {", "core", False, 1),
    ("M31", "S2: dead BOOT0 pattern accepted", "core/stage2_id.c",
     "if (GM20B_IS_DEAD(boot0)) {", "if (0) {", "core", False, 1),
    ("M32", "S2: BOOT0 intent only recorded, not flushed", "core/stage2_id.c",
     "GM20B_STEP(g, J_S2_BOOT0_INTENT,", "gm20b_j(g, J_S2_BOOT0_INTENT,", "core", False, 1),
    ("M33", "S1: power verification result ignored", "core/stage1_power.c",
     "\treturn verify_powered(g);\n", "\t(void)verify_powered(g);\n\treturn 0;\n", "core", False, 1),
    ("M34", "S1 probe: reads not announced one by one", "core/stage1_power.c",
     "GM20B_STEP(g, J_S1_PROBE_RD, GM20B_JADDR(sp, off), 0);", "gm20b_j(g, J_S1_PROBE_RD, GM20B_JADDR(sp, off), 0);",
     "core", False, 1),
    ("M35", "S1 probe: announces the wrong register", "core/stage1_power.c",
     "GM20B_STEP(g, J_S1_PROBE_RD, GM20B_JADDR(sp, off), 0);", "GM20B_STEP(g, J_S1_PROBE_RD, GM20B_JADDR(sp, 0), 0);",
     "core", False, 1),
]


def copy_tree(dst):
    os.makedirs(dst)
    for n in ("core", "regs", "sim", "win", "Makefile"):
        s = os.path.join(SRC, n)
        d = os.path.join(dst, n)
        if os.path.isdir(s):
            shutil.copytree(s, d, ignore=shutil.ignore_patterns("build", "__pycache__"))
        else:
            shutil.copy2(s, d)


def apply(root, spec):
    mid, desc, f, pat, rep, kind, rx, cnt = spec
    if f is None:
        return None
    path = os.path.join(root, f)
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    if rx:
        new, n = re.subn(pat, rep, text, flags=re.S)
    else:
        n = text.count(pat)
        new = text.replace(pat, rep)
    if n == 0 or (cnt is not None and n != cnt):
        return "pattern matched %d times (expected %s)" % (n, cnt)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(new)
    return None


EVID = re.compile(r"(FAIL|violation|AddressSanitizer|runtime error|Assertion|assert|[1-9][0-9]* failed|"
                  r"[1-9][0-9]* of [0-9]+ failed)", re.I)


def run(cmd, cwd, timeout=420):
    p = subprocess.run(cmd, cwd=cwd, shell=True, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


PRI = [re.compile(r"^\s*FAIL\b"), re.compile(r"ERROR: AddressSanitizer|runtime error:"), EVID]


def evidence(out):
    lines = out.splitlines()
    for rx in PRI:
        for ln in lines:
            if rx.search(ln) and "0 failed" not in ln:
                return ln.strip()[:150]
    return lines[-1].strip()[:150] if lines else "(no output)"


def one(spec):
    mid, desc = spec[0], spec[1]
    kind = spec[5]
    root = os.path.join(WORK, mid)
    shutil.rmtree(root, ignore_errors=True)
    t0 = time.time()
    copy_tree(root)
    bad = apply(root, spec)
    if bad:
        return mid, desc, "INVALID", bad, time.time() - t0
    cmds = []
    if kind == "core":
        cmds.append("make test 2>&1")
    cmds.append("make -C win check-host 2>&1")
    last = ""
    for c in cmds:
        rc, out = run(c, root)
        if rc != 0:
            if re.search(r"\.[ch]:\d+:\d+: error:", out) and "checks," not in out and " tests," not in out:
                return mid, desc, "INVALID", "does not compile: " + evidence(out), time.time() - t0
            return mid, desc, "KILLED", evidence(out), time.time() - t0
        last = out
    return mid, desc, "SURVIVED", evidence(last), time.time() - t0


def main():
    only = set(sys.argv[1:])
    specs = [s for s in M if not only or s[0] in only]
    os.makedirs(WORK, exist_ok=True)
    res = []
    with cf.ThreadPoolExecutor(max_workers=2) as ex:
        for r in ex.map(one, specs):
            res.append(r)
            line = "%-4s %-9s %-52s | %s (%.0fs)" % (r[0], r[2], r[1], r[3], r[4])
            print(line, flush=True)
            with open(RESULTS, "a", encoding="utf-8") as fh:
                fh.write(line + "\n")
    surv = [r for r in res if r[2] == "SURVIVED" and r[0] != "M00"]
    inv = [r for r in res if r[2] == "INVALID"]
    print("\nkilled=%d survived=%d invalid=%d  (M00 control must be SURVIVED)" %
          (sum(1 for r in res if r[2] == "KILLED"), len(surv), len(inv)))
    for r in surv:
        print("  SURVIVOR:", r[0], r[1])
    for r in inv:
        print("  INVALID :", r[0], r[1], "->", r[3])
    ctl = [r for r in res if r[0] == "M00"]
    ok = (not ctl or ctl[0][2] == "SURVIVED") and not surv and not inv
    if ok and OWN_WORK:
        shutil.rmtree(WORK, ignore_errors=True)     # nothing to look at: every mutant was caught
    else:
        print("work dir (kept for inspection):", WORK)
    print("MUTATION CHECK:", "PASS" if ok else "FAIL", flush=True)
    return 0 if ok else 1


sys.exit(main())
