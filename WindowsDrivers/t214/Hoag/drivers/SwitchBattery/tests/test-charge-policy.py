#!/usr/bin/env python3
"""Exercise actual BQ24193 writes, error paths and the two-resource restart."""
from pathlib import Path
import re
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "src/switchbattery.c").read_text()
def function(name):
    start = re.search(r"^(?:static )?NTSTATUS " + re.escape(name) + r"\(", source, re.M).start()
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"
defines = "\n".join(re.findall(r"^#define BQ_[^\n]+", source, re.M))
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
typedef uint8_t UCHAR;
typedef uint32_t ULONG;
typedef int32_t NTSTATUS;
#define NT_SUCCESS(s) ((s) >= 0)
#define STATUS_SUCCESS 0
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xc00000a3)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xc00000bb)
#define STATUS_DEVICE_DATA_ERROR ((NTSTATUS)0xc000009c)
#define IO_ERROR ((NTSTATUS)0xc0000185)
#define MAXULONG UINT32_MAX
#define FALSE 0
#define POLL_MS 10000
#define USBC_POLL_MS 500
#define WDF_REL_TIMEOUT_IN_MS(x) (x)
#define VOID void
struct device { void *Charger, *Pd, *Timer, *UsbcTimer; unsigned Lock, Stopping, PdConfigured; };
typedef struct device *PSWBAT_DEVICE, *WDFDEVICE;
static uint8_t regs[256], before[256];
static unsigned ops, writes, fail_at, corrupt, timers, pd_calls;
static uint32_t trace_before, trace_after, trace_status;
static void SwBatTrace(const wchar_t *n, ULONG v) {
    if (!wcscmp(n,L"ChargeTimerBefore")) trace_before=v;
    if (!wcscmp(n,L"ChargeTimerAfter")) trace_after=v;
    if (!wcscmp(n,L"ChargePolicyStatus")) trace_status=v;
}
static NTSTATUS SwBatReadRegs(void *target,UCHAR reg,UCHAR *out,ULONG size) {
    assert(target && size==1);
    if (++ops==fail_at) return IO_ERROR;
    *out=regs[reg];
    if (corrupt && writes && reg==5) *out^=0x80;
    return 0;
}
static NTSTATUS SwBatWrite8(void *target,UCHAR reg,UCHAR val) {
    assert(target && reg==5); /* Never change mode, limits, fault latches or gauge. */
    if (++ops==fail_at) return IO_ERROR;
    regs[reg]=val;++writes;return 0;
}
static PSWBAT_DEVICE SwBatGetDevice(WDFDEVICE d){return d;}
static void WdfWaitLockAcquire(unsigned lock,void *unused){(void)unused;assert(lock==1);}
static void WdfWaitLockRelease(unsigned lock){assert(lock==1);}
static void WdfTimerStart(void *t,unsigned ms){assert(t && ms);++timers;}
static NTSTATUS SwBatConfigurePd(WDFDEVICE d,PSWBAT_DEVICE p){assert(d==p);++pd_calls;return 0;}
static void SwBatUsbcStart(PSWBAT_DEVICE d){assert(d->Pd && (regs[5]&0xbe)==0x8a);}
static void reset(unsigned timer) {
    memset(regs,0x55,sizeof(regs));regs[10]=0x2f;regs[2]=0x48;regs[5]=timer;
    memcpy(before,regs,sizeof(regs));
    ops=writes=fail_at=corrupt=timers=pd_calls=0;
    trace_before=trace_after=trace_status=UINT32_MAX;
}
'''
checks = r'''
int main(void) {
    struct device d={.Charger=&d,.Lock=1,.Timer=&d,.Stopping=1};
    /* All incoming REG05 settings preserve indicator/JEITA bits and limits. */
    for (unsigned v=0;v<256;++v) {
        reset(v);unsigned target=(v&0x41)|0x8a;
        assert(SwBatConfigureChargePolicy(&d)==0);
        assert(regs[5]==target && writes==(v!=target));
        assert(trace_status==0 && trace_before==v && trace_after==target);
        for (unsigned i=0;i<256;++i) if(i!=5) assert(regs[i]==before[i]);
        writes=0;
        assert(SwBatConfigureChargePolicy(&d)==0 && !writes); /* idempotent resume */
    }
    for(unsigned n=1;n<=5;++n) {
        reset(2);fail_at=n;
        assert(SwBatConfigureChargePolicy(&d)==IO_ERROR && trace_status==(uint32_t)IO_ERROR);
        assert(ops==n);
        if(n<=4) assert(!writes);
    }
    reset(2);corrupt=1;
    assert(SwBatConfigureChargePolicy(&d)==STATUS_DEVICE_DATA_ERROR);
    reset(2);regs[2]|=1;
    assert(SwBatConfigureChargePolicy(&d)==STATUS_NOT_SUPPORTED && !writes);
    reset(2);regs[10]=0;
    assert(SwBatConfigureChargePolicy(&d)==STATUS_NOT_SUPPORTED && !writes);
    reset(2);d.Charger=NULL;
    assert(SwBatConfigureChargePolicy(&d)==STATUS_DEVICE_NOT_READY && !ops);
    d.Charger=&d;
    reset(2);assert(SwBatEvtSelfManagedIoRestart(&d)==0);
    assert(regs[5]==0x8a && !d.Stopping && timers==1 && !pd_calls);
    reset(2);d.Pd=d.UsbcTimer=&d;
    assert(SwBatEvtSelfManagedIoRestart(&d)==0);
    assert(regs[5]==0x8a && timers==2 && pd_calls==1 && d.PdConfigured);
    puts("Battery charge policy: bounded REG05 writes, preserved limits, I2C errors and PD-independent restart passed");
}
'''
with tempfile.TemporaryDirectory(prefix="switch-charge-policy-") as tmp:
    root=Path(tmp)
    (root/"test.c").write_text(harness+defines+"\n"+function("SwBatConfigureChargePolicy")+function("SwBatEvtSelfManagedIoRestart")+checks)
    subprocess.run(["clang","-std=c11","-Wall","-Wextra","-Werror","-fsanitize=address,undefined","-g",str(root/"test.c"),"-o",str(root/"test")],check=True)
    subprocess.run([str(root/"test")],check=True)
