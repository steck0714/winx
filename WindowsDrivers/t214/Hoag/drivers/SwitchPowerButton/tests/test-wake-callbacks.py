#!/usr/bin/env python3
"""Exercise actual S0 IRQ/power callbacks with explicit WDF/I2C/VHF substitutes."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/switchpwrbtn.c').read_text()

def function(name):
    match = re.search(r'^(?:static )?(?:VOID|BOOLEAN|NTSTATUS|int32_t) ' + name +
                      r'\([^;]*?\)\n\{', source, re.M)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]

prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "irq_route.h"
#include "button_state.h"
typedef int32_t NTSTATUS, LONG;
typedef uint32_t ULONG; typedef uint8_t UCHAR; typedef unsigned KIRQL;
typedef bool BOOLEAN; typedef void VOID;
typedef void *WDFDEVICE, *WDFINTERRUPT, *WDFOBJECT;
typedef unsigned WDF_POWER_DEVICE_STATE;
typedef struct {long long QuadPart;} LARGE_INTEGER;
typedef enum {PowerActionNone,PowerActionSleep,PowerActionShutdown,
    PowerActionShutdownReset,PowerActionShutdownOff} POWER_ACTION;
#define WdfPowerDeviceD3Final 5
#define WdfDeviceFailedNoRestart 1
#define TRUE true
#define FALSE false
#define STATUS_SUCCESS 0
#define NT_SUCCESS(s) ((s)>=0)
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define WDF_REL_TIMEOUT_IN_MS(n) (n)
#define MAX77620_ONOFFIRQ 0x0b
#define MAX77620_ONOFFSTAT 0x15
#define ONOFFIRQ_EN0_R 0x08
#define ONOFFSTAT_EN0 0x04
#define POLL_MS 50
typedef struct {
 void *I2c, *Interrupt, *Timer, *Vhf;
 LARGE_INTEGER I2cId;
 struct swpwr_irq_route IrqRoute;
 struct swpwr_button_state Button;
 bool VhfReady, Stopping;
 unsigned TimerLock;
 volatile LONG IrqFault,LastIrqStatus,LastIrqBits,Irqs,LastReportStatus,DisplayOff;
 LONG Presses,Reports,PowerReports,WakeReports;
} SWPWR_DEVICE,*PSWPWR_DEVICE;
static uint8_t regs[32], reports[8];
static unsigned reads,writes,edge_reads,nreports,registry_writes,workitems,failures,timers;
static bool read_fail,irq_lock;
static POWER_ACTION action;
static PSWPWR_DEVICE SwPwrGetDevice(void *d){return d;}
static void *WdfInterruptGetDevice(void *d){return d;}
static LONG InterlockedCompareExchange(volatile LONG *p,LONG v,LONG c){LONG old=*p;if(old==c)*p=v;return old;}
static LONG InterlockedIncrement(volatile LONG *p){return ++*p;}
static LONG InterlockedExchange(volatile LONG *p,LONG v){LONG old=*p;*p=v;return old;}
static void SwPwrTrace(const wchar_t *n,ULONG v){(void)n;(void)v;assert(!irq_lock);++registry_writes;}
static NTSTATUS SwPwrReadReg(void *t,UCHAR r,UCHAR *v){
 (void)t;++reads;if(read_fail)return (int32_t)0xc0000185u;
 *v=regs[r];if(r==MAX77620_ONOFFIRQ){++edge_reads;regs[r]=0;}return 0;
}
static NTSTATUS SwPwrWriteReg(void *t,UCHAR r,UCHAR v,UCHAR m){(void)t;(void)m;++writes;regs[r]=v;return 0;}
static NTSTATUS SwPwrOpenTarget(void *d,LARGE_INTEGER id,void **t){(void)id;*t=d;return 0;}
static void SwPwrReport(PSWPWR_DEVICE d,UCHAR b){(void)d;assert(nreports<sizeof(reports));reports[nreports++]=b;}
static bool WdfInterruptQueueWorkItemForIsr(void *d){(void)d;++workitems;return true;}
static void WdfDeviceSetFailed(void *d,unsigned why){(void)d;assert(!irq_lock&&why==1);++failures;}
static POWER_ACTION WdfDeviceGetSystemPowerAction(void *d){(void)d;return action;}
static void KeAcquireSpinLock(unsigned *l,KIRQL *i){(void)l;*i=0;}
static void KeReleaseSpinLock(unsigned *l,KIRQL i){(void)l;(void)i;}
static void WdfTimerStop(void *t,bool wait){(void)t;assert(wait);}
static void WdfTimerStart(void *t,unsigned ms){(void)t;assert(ms==50);++timers;}
'''
names = ['SwPwrIrqRead','SwPwrIrqWrite','SwPwrSetIrq','SwPwrTraceReports',
         'SwPwrEvtIsr','SwPwrEvtInterruptWorkItem','SwPwrEvtD0Entry',
         'SwPwrEvtD0Exit','SwPwrEvtSuspend','SwPwrEvtRestart']
tests = r'''
static void init(PSWPWR_DEVICE d){
 memset(d,0,sizeof(*d));memset(regs,0,sizeof(regs));
 d->Interrupt=d;d->Vhf=d;d->I2c=d;
 reads=writes=edge_reads=nreports=registry_writes=workitems=failures=timers=0;
 read_fail=irq_lock=false;action=PowerActionNone;
}
int main(void){
 SWPWR_DEVICE d;init(&d);
 regs[MAX77620_ONOFFIRQ]=8;
 assert(SwPwrEvtD0Entry(&d,5)==0 && !d.IrqRoute.armed && !edge_reads);
 assert(regs[SWPWR_TOP_MASK]&SWPWR_TOP_ONOFF);
 /* Initial VHF startup drops inherited edges separately, then arms once. */
 regs[MAX77620_ONOFFIRQ]=0;d.VhfReady=true;
 assert(SwPwrEvtRestart(&d)==0 && d.IrqRoute.armed && !timers);
 unsigned before=reads;
 regs[MAX77620_ONOFFIRQ]=8;d.DisplayOff=1;
 assert(SwPwrEvtD0Entry(&d,3)==0 && reads==before && !edge_reads);
 irq_lock=true;assert(SwPwrEvtIsr(&d,0));irq_lock=false;
 assert(nreports==2 && reports[0]==SWPWR_WAKE && reports[1]==0);
 assert(edge_reads==1 && !registry_writes && workitems==1);
 before=reads;assert(SwPwrEvtRestart(&d)==0 && reads==before);
 SwPwrEvtInterruptWorkItem(&d,NULL);assert(registry_writes && !failures);
 /* Release-only and stale-controller interrupts never emit a new press. */
 regs[MAX77620_ONOFFIRQ]=4;d.DisplayOff=0;
 irq_lock=true;assert(SwPwrEvtIsr(&d,0));assert(!SwPwrEvtIsr(&d,0));irq_lock=false;
 assert(nreports==2 && !d.IrqFault);
 regs[MAX77620_ONOFFIRQ]=8;
 irq_lock=true;assert(SwPwrEvtIsr(&d,0));irq_lock=false;
 assert(nreports==4 && reports[2]==SWPWR_POWER && reports[3]==0);
 /* Device-only and S0 sleep keep masks armed. Final exit restores ownership. */
 before=reads;action=PowerActionSleep;
 assert(SwPwrEvtSuspend(&d)==0 && SwPwrEvtD0Exit(&d,3)==0);
 assert(reads==before && d.IrqRoute.armed);
 assert(SwPwrEvtD0Exit(&d,5)==0 && !d.IrqRoute.saved && !d.IrqRoute.armed);
 assert(regs[SWPWR_TOP_MASK]==0 && regs[SWPWR_GLOBAL_MASK]==0 && regs[SWPWR_EDGE_MASK]==0);
 /* A genuine I2C failure becomes a failed device, outside the ISR lock. */
 init(&d);assert(SwPwrEvtD0Entry(&d,5)==0);d.VhfReady=true;
 assert(SwPwrEvtRestart(&d)==0);read_fail=true;irq_lock=true;
 assert(SwPwrEvtIsr(&d,0));irq_lock=false;
 assert(d.IrqFault && !registry_writes && !nreports);
 before=reads;assert(SwPwrEvtD0Entry(&d,3)<0 && reads==before);
 SwPwrEvtInterruptWorkItem(&d,NULL);assert(failures==1);
 /* Older firmware retains its polling restart and held-button suppression. */
 init(&d);d.Interrupt=NULL;d.Timer=&d;regs[MAX77620_ONOFFSTAT]=4;
 assert(SwPwrEvtD0Entry(&d,5)==0 && !reads);
 assert(SwPwrEvtRestart(&d)==0 && d.Button.suppress_held && timers==1);
 puts("Power wake callbacks, edge retention, pulses and fault retirement: PASS (WDF/I2C/VHF model)");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='switchpwr-wake-') as directory:
    path = Path(directory)
    unit = path / 'wake.c'
    unit.write_text(prefix + '\n'.join(function(n) for n in names) + tests)
    subprocess.run([os.environ.get('CC','clang'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', '-g',
                    '-I'+str(root / 'src'), str(unit), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
