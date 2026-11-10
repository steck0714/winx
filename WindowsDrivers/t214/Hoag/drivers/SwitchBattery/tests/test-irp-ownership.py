#!/usr/bin/env python3
"""Exercise the real battc handoff with immediate and pending completions."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "src/switchbattery.c").read_text()
start = source.index("NTSTATUS SwBatPreprocessDeviceControl(")
end = source.index("NTSTATUS SwBatEvtDeviceAdd(", start)
handler = source[start:end]
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef int32_t NTSTATUS;
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xc00000bb)
#define STATUS_INVALID_DEVICE_REQUEST ((NTSTATUS)0xc0000010)
#define STATUS_PENDING ((NTSTATUS)0x103)
typedef struct { unsigned held; } LOCK;
typedef struct { unsigned stack, pending; } IRP, *PIRP;
typedef struct { LOCK *ClassLock, *Lock; void *ClassHandle; } DEVICE;
typedef DEVICE *WDFDEVICE, *PSWBAT_DEVICE;
static unsigned class_calls, wdf_calls;
static NTSTATUS class_status;
static DEVICE *SwBatGetDevice(WDFDEVICE d) { return d; }
static void WdfWaitLockAcquire(LOCK *l, void *unused) {
    (void)unused; assert(!l->held); l->held = 1;
}
static void WdfWaitLockRelease(LOCK *l) { assert(l->held); l->held = 0; }
static NTSTATUS BatteryClassIoctl(void *handle, PIRP irp) {
    DEVICE *d = handle;
    assert(d->ClassLock->held);
    /* A synchronous battery callback must be able to take the I2C lock. */
    WdfWaitLockAcquire(d->Lock, NULL);
    WdfWaitLockRelease(d->Lock);
    ++class_calls;
    assert(!irp->stack);
    if (class_status == STATUS_PENDING) irp->pending = 1;
    else if (class_status != STATUS_NOT_SUPPORTED) free(irp);
    return class_status;
}
static void IoSkipCurrentIrpStackLocation(PIRP irp) { ++irp->stack; }
static NTSTATUS WdfDeviceWdmDispatchPreprocessedIrp(WDFDEVICE d, PIRP irp) {
    assert(!d->ClassLock->held && irp->stack == 1 && !irp->pending);
    ++wdf_calls; free(irp); return STATUS_INVALID_DEVICE_REQUEST;
}
'''
checks = r'''
int main(void) {
    LOCK gate = {0}, io = {0};
    DEVICE d = { &gate, &io, NULL }; d.ClassHandle = &d;
    const NTSTATUS results[] = {0, (NTSTATUS)0xc000000d, STATUS_PENDING,
                                STATUS_NOT_SUPPORTED};
    for (unsigned i = 0; i < sizeof(results)/sizeof(results[0]); ++i) {
        IRP *irp = calloc(1, sizeof(*irp));
        class_calls = wdf_calls = 0; class_status = results[i];
        NTSTATUS result = SwBatPreprocessDeviceControl(&d, irp);
        assert(class_calls == 1 && !gate.held && !io.held);
        if (class_status == STATUS_NOT_SUPPORTED) {
            assert(result == STATUS_INVALID_DEVICE_REQUEST && wdf_calls == 1);
        } else {
            assert(result == class_status && wdf_calls == 0);
            if (class_status == STATUS_PENDING) { assert(irp->pending); free(irp); }
        }
    }
    d.ClassHandle = NULL; class_calls = wdf_calls = 0;
    assert(SwBatPreprocessDeviceControl(&d, calloc(1, sizeof(IRP))) ==
           STATUS_INVALID_DEVICE_REQUEST);
    assert(!class_calls && wdf_calls == 1 && !gate.held);
    puts("Battery IRP ownership: handled, failed, pending and fallback paths passed");
}
'''
with tempfile.TemporaryDirectory(prefix="switch-battery-irp-") as directory:
    root = Path(directory)
    (root / "test.c").write_text(harness + handler + checks)
    subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-g", str(root / "test.c"),
                    "-o", str(root / "test")], check=True)
    subprocess.run([str(root / "test")], check=True)
