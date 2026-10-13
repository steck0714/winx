#!/usr/bin/env python3
"""Acts as the "test binary" for ../check.py: reads whitespace-separated elapsed-second values on stdin,
runs them through the bare-metal AArch64 image in qemu-system-aarch64 and prints one result line each.
Needs RTC_BM_ELF (path to the linked ELF)."""
import os, struct, subprocess, sys, tempfile, time
elf = os.environ["RTC_BM_ELF"]
vals = [int(x) for x in sys.stdin.read().split()]
with tempfile.TemporaryDirectory() as d:
    ts, out = os.path.join(d, "ts.bin"), os.path.join(d, "serial.txt")
    open(ts, "wb").write(struct.pack("<Q", len(vals)) + struct.pack("<%dQ" % len(vals), *vals))
    open(out, "wb").close()
    q = subprocess.Popen(["qemu-system-aarch64", "-M", "virt", "-cpu", "cortex-a57", "-m", "128M", "-smp", "1",
                          "-nographic", "-monitor", "none", "-nic", "none", "-serial", "file:" + out,
                          "-kernel", elf, "-device", "loader,file=%s,addr=0x41000000" % ts],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t0, done = time.time(), False
    while time.time() - t0 < float(os.environ.get("RTC_BM_TIMEOUT", "300")):
        time.sleep(0.3)
        with open(out, "rb") as f:
            f.seek(0, 2); size = f.tell(); f.seek(max(0, size - 8)); tail = f.read()
        if tail.endswith(b"DONE\n"):
            done = True
            break
        if q.poll() is not None:
            break
    q.kill(); q.wait()
    if not done:
        sys.exit("qemu run did not finish (no DONE marker)")
    sys.stdout.write(open(out, "rb").read().decode().replace("DONE\n", ""))
