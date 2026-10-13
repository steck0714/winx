#!/usr/bin/env python3
"""Run hekate's real fan code and Drivers/FanDxe against the same fake register file and compare.

usage: compare.py <trace_hekate> <trace_fan>

What is checked, per scenario:
  * the ordered list of register writes of FanDxe equals hekate's fan_set_duty() sequence once the
    DELIBERATE differences are removed (tach pad/GPIO S7, USB_VBUS_EN0 pad/GPIO CC4, and writes that
    do not change a register);
  * the final value of every register that either side touched is identical (except the same
    deliberate ones);
  * the backlight PWM (CSR_0) and every GPIO bit that is not part of the fan are untouched;
  * T210B01 and duty 0: FanDxe writes nothing.
It passes or fails; it says nothing about the real board.
"""
import subprocess, sys, itertools

CAR, GPIO, PINMUX, PWM, PMC = 0x60006000, 0x6000D000, 0x70003000, 0x7000A000, 0x7000E400
ALIAS = {CAR + 0x300, CAR + 0x304, CAR + 0x320, CAR + 0x324}
# registers hekate writes and FanDxe deliberately does not (see the header of FanDxe.c)
OMIT = {
    PINMUX + 0x1EC,                              # CAM1_PWDN pad (tachometer)
    GPIO + 0x408, GPIO + 0x418,                  # GPIO S7 CNF / OE (tachometer)
    PINMUX + 0x1A8,                              # USB_VBUS_EN0 pad
    GPIO + 0x700, GPIO + 0x710, GPIO + 0x720,    # GPIO CC4 CNF / OE / OUT
}
# hekate clock_enable(&_clock_pwm), hand-derived from bdk/soc/clock.c clock_enable() and _clock_pwm
CLK_PWM = [(CAR + 0x300, 1 << 17), (CAR + 0x324, 1 << 17), (CAR + 0x110, (6 << 29) | 4),
           (CAR + 0x320, 1 << 17), (CAR + 0x304, 1 << 17)]

def run(binary, args):
    out = subprocess.run([binary] + args, capture_output=True, text=True, check=False)
    if out.returncode != 0:
        raise SystemExit(f"{binary} {args}: rc={out.returncode}\n{out.stderr}")
    init, writes, final, status = {}, [], {}, None
    for line in out.stdout.splitlines():
        f = line.split()
        if not f: continue
        if f[0] == 'I': init[int(f[1], 16)] = int(f[2], 16)
        elif f[0] == 'W': writes.append((int(f[1], 16), int(f[2], 16), int(f[3], 16)))
        elif f[0] == 'S': final[int(f[1], 16)] = int(f[2], 16)
        elif f[0] == '#' and len(f) > 2 and f[1] == 'status': status = int(f[2])
    return init, writes, final, status

def effective(writes, drop=()):
    """Drop omitted registers and writes that do not change the register (the PMC read-modify-write
    no-ops); keep the CAR set/clear aliases, whose effect is on another register."""
    return [(a, v) for (a, v, old) in writes if a not in drop and (a in ALIAS or v != old)]

def fmt(seq):
    return "\n".join(f"    {a:08x} <= {v:08x}" for a, v in seq)

def check(name, cond, detail=""):
    if not cond:
        print(f"  FAIL {name}\n{detail}")
    return cond

def scenario(hbin, fbin, label, fan_args, hek_args=None, expect_clock_enable=False,
             expect_silent=False, expect_status=0):
    hek_args = fan_args if hek_args is None else hek_args
    ok = True
    hi, hw, hf, _ = run(hbin, hek_args)
    fi, fw, ff, fst = run(fbin, fan_args)
    ok &= check("same initial state", hi == fi)
    ok &= check(f"status {expect_status}", fst == expect_status, f"    got {fst}")
    if expect_silent:
        ok &= check("no register writes at all", not fw, fmt([(a, v) for a, v, _ in fw]))
        ok &= check("final state == initial state", ff == fi)
        print(f"{'PASS' if ok else 'FAIL'}  {label}")
        return ok

    ok &= check("FanDxe never writes an omitted register", not any(a in OMIT for a, _, _ in fw),
                fmt([(a, v) for a, v, _ in fw if a in OMIT]))
    hseq = effective(hw, drop=OMIT)
    fseq = effective(fw)
    if expect_clock_enable:
        ok &= check("PWM clock bring-up == hekate clock_enable(&_clock_pwm)", fseq[:5] == CLK_PWM,
                    "    mine:\n" + fmt(fseq[:5]) + "\n    expected:\n" + fmt(CLK_PWM))
        fseq = fseq[5:]
    ok &= check("write sequence == hekate fan_set_duty() minus deliberate omissions", fseq == hseq,
                "    hekate:\n" + fmt(hseq) + "\n    FanDxe:\n" + fmt(fseq))
    skip = set(OMIT)
    if expect_clock_enable:
        skip |= {CAR + 0x004, CAR + 0x010, CAR + 0x110}        # hekate does not touch them on Erista
    diffs = [(a, hf.get(a), ff.get(a)) for a in sorted(set(hf) | set(ff)) if a not in skip and hf.get(a) != ff.get(a)]
    ok &= check("final register state identical", not diffs,
                "\n".join(f"    {a:08x} hekate={h} fandxe={f}" for a, h, f in diffs))
    # independent invariants on the FanDxe result
    duty = min(fan_args_duty(fan_args), 235)
    ok &= check("PWM CSR_1 = EN | (236-duty)<<16", ff[PWM + 0x10] == 0x80000000 | ((236 - duty) << 16),
                f"    {ff[PWM + 0x10]:08x}")
    ok &= check("backlight PWM CSR_0 untouched", ff[PWM] == fi[PWM] and not any(a == PWM for a, _, _ in fw))
    ok &= check("pinmux LCD_GPIO2 = 1 (PWM1), SATA_LED_ACTIVE = 1", ff[PINMUX + 0x20C] == 1 and ff[PINMUX + 0x4C] == 1)
    ok &= check("USB_VBUS_EN0 pad untouched", ff[PINMUX + 0x1A8] == fi[PINMUX + 0x1A8])
    # GPIO: only V4 (cnf cleared) and A5 (cnf/oe/out set) may differ
    want = dict(fi)
    want[GPIO + 0x504] &= ~(1 << 4)
    for off in (0x000, 0x010, 0x020):
        want[GPIO + off] |= (1 << 5)
    gd = [(a, want[a], ff[a]) for a in sorted(ff) if GPIO <= a < GPIO + 0x800 and want[a] != ff[a]]
    ok &= check("GPIO: only V4 -> SFIO and A5 -> output high changed", not gd,
                "\n".join(f"    {a:08x} want={w:08x} got={g:08x}" for a, w, g in gd))
    # PMC: bit 21 cleared, everything else preserved
    for off in (0x44, 0xE4):
        ok &= check(f"PMC+{off:#x}: bit 21 clear, other bits preserved",
                    ff[PMC + off] == fi[PMC + off] & ~(1 << 21), f"    {ff[PMC + off]:08x}")
    if not expect_clock_enable:
        ok &= check("CAR untouched", all(ff[a] == fi[a] for a in (CAR + 4, CAR + 0x10, CAR + 0x110)))
    else:
        ok &= check("PWM clock ends enabled and out of reset",
                    (ff[CAR + 0x10] >> 17) & 1 == 1 and (ff[CAR + 4] >> 17) & 1 == 0)
    print(f"{'PASS' if ok else 'FAIL'}  {label}  ({len(fseq)} effective writes)")
    return ok

def fan_args_duty(args):
    for a in args:
        if a.startswith('--duty='): return int(a[7:], 0)
    return 76

def main():
    hbin, fbin = sys.argv[1], sys.argv[2]
    results = []
    for duty, pmc, grand, seed in itertools.product((1, 51, 76, 102, 235), (0, 1), (0, 1), (7, 12345)):
        if not grand and seed != 7: continue            # zero-filled GPIO does not depend on the seed
        a = [f"--duty={duty}", f"--pmc-bits={pmc}", f"--gpio-rand={grand}", f"--seed={seed}"]
        results.append(scenario(hbin, fbin, f"Erista duty={duty:3d} pmc_bits={pmc} gpio_rand={grand} seed={seed}", a))
    # PWM clock off: FanDxe brings it up exactly as hekate's clock_enable() would
    for state, text in (("off", "clock off + in reset"), ("rst", "clock on but held in reset"),
                        ("noclk", "clock off, not in reset")):
        for grand in (0, 1):
            a = ["--duty=76", f"--pwm-clk={state}", f"--gpio-rand={grand}", "--pmc-bits=1"]
            results.append(scenario(hbin, fbin, f"Erista duty=76 PWM {text}, gpio_rand={grand}", a,
                                    expect_clock_enable=True))
    # PCD is UINT8: 255 is clamped to 235 (hekate itself clamps at 236, which means "off", so compare with 235)
    results.append(scenario(hbin, fbin, "Erista duty=255 -> clamped to 235",
                            ["--duty=255"], hek_args=["--duty=235"]))
    # must stay silent
    results.append(scenario(hbin, fbin, "T210B01 (Mariko) -> no writes", ["--duty=76", "--hidrev=0x00012120"],
                            expect_silent=True, expect_status=3))
    results.append(scenario(hbin, fbin, "duty=0 -> no writes", ["--duty=0"], hek_args=["--duty=76"],
                            expect_silent=True, expect_status=0))
    n_ok = sum(results)
    print(f"\n{n_ok}/{len(results)} scenarios passed")
    sys.exit(0 if n_ok == len(results) else 1)

main()
