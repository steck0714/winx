#!/usr/bin/env python3
"""Compare LibGetTime() of Library/VirtualRealTimeClockLib with Python's datetime for many dates.
usage: check.py <test_rtc binary built with -D__DATE__=...> "<Mmm dd yyyy>" <expected base date YYYY-MM-DD>"""
import datetime, random, subprocess, sys
exe, date_str, base_str = sys.argv[1:4]
base = datetime.datetime.strptime(base_str, "%Y-%m-%d").replace(tzinfo=datetime.timezone.utc)
random.seed(1)
# edge seconds: around day / month / year / leap boundaries, plus random spread over ~300 years
vals = {0, 1, 59, 60, 3599, 3600, 86399, 86400, 86401}
for k in range(0, 4000):
    vals.add(k * 86400 + random.choice((0, 86399)))                         # every day boundary for ~11 years
vals.update(random.randrange(0, 300 * 365 * 86400) for _ in range(200000))
# deterministic boundaries for the next 400 years: new year, Feb 28/29, Mar 1, year end
for yr in range(base.year, base.year + 400):
    for (mo, dy) in ((1, 1), (2, 28), (2, 29), (3, 1), (12, 31)):
        try:
            d = datetime.datetime(yr, mo, dy, tzinfo=datetime.timezone.utc)
        except ValueError:
            continue                                   # Feb 29 of a non-leap year
        e = int((d - base).total_seconds())
        if e >= 0:
            vals.update((e, e + 86399))
vals = sorted(vals)
inp = "\n".join(map(str, vals)) + "\n"
out = subprocess.run([exe], input=inp, capture_output=True, text=True, check=True).stdout.splitlines()
assert len(out) == len(vals)
bad = 0
for line in out:
    e, st, Y, M, D, h, m, s, tz, ns = map(int, line.split())
    want = base + datetime.timedelta(seconds=e)
    got = (Y, M, D, h, m, s)
    exp = (want.year, want.month, want.day, want.hour, want.minute, want.second)
    ok = got == exp and st == 0 and tz == 0 and ns == 0 and 1 <= D <= 31 and 1 <= M <= 12
    if not ok:
        bad += 1
        if bad <= 5: print("MISMATCH", e, "got", got, "want", exp)
print(f"{date_str!r:16} base {base_str}: {len(vals)} timestamps, {bad} mismatches")
sys.exit(1 if bad else 0)
