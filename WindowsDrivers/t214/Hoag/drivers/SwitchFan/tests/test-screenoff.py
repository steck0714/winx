#!/usr/bin/env python3
"""Exercise the actual fan timer and D0 callbacks with a simulated TMP451/CPU."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'src/switchfan.c').read_text()
names = ['SwFanSetDuty', 'SwFanDutyForTemp', 'SwFanEvtTimer',
         'SwFanEvtD0Entry', 'SwFanEvtD0Exit', 'SwFanApplyBacklight', 'SwFanDisplayStateCallback']

def function(name):
    match = re.search(r'^(?:static )?(?:VOID|ULONG|NTSTATUS) ' + name + r'\([^;]*?\)\n\{', source, re.M)
    assert match, name
    end = match.end()
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
typedef uint32_t ULONG; typedef uint8_t UCHAR; typedef int32_t LONG;
typedef int32_t NTSTATUS; typedef bool BOOLEAN; typedef void VOID;
typedef ULONG *PULONG; typedef void *WDFDEVICE; typedef void *WDFTIMER;
typedef unsigned WDF_POWER_DEVICE_STATE;
typedef void *PVOID; typedef const int *LPCGUID;
static const int SwFanDisplayStateGuid=1;
#define IsEqualGUID(a,b) ((a)==(b))
#define PWM_CSR_0 0
#define WdfPowerDeviceD3Final 5
typedef enum {PowerActionNone,PowerActionSleep,PowerActionHibernate,
              PowerActionShutdown,PowerActionShutdownReset} POWER_ACTION;
#define TRUE true
#define FALSE false
#define STATUS_SUCCESS 0
#define NT_SUCCESS(s) ((s)>=0)
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define PWM_DUTY_MAX 236u
#define PWM_CSR_EN (1u<<31)
#define PWM_ABSOLUTE_OFF (0x100u<<16)
#define LITE_CPU_KHZ 1996800u
#define TMP451_REMOTE 1
#define TMP451_LOCAL 0
#define FAILSAFE_DUTY 153u
#define POLL_MS 2000
#define WDF_REL_TIMEOUT_IN_MS(n) (n)
typedef struct {
 UCHAR *Pwm,*Car,*BacklightPwm; ULONG FanOffset,Duty,DisplayState,LastTemp,BootCpuKhz,Brightness;
 void *Sensor,*Timer,*BacklightLock,*Device;
 BOOLEAN IsLite,Stopping,BacklightReady,CpuConfigured,CpuApplied,TempValid,CpuOverclockEnabled;
 struct {ULONG CpuKhz;} Config;
 ULONG Failures; LONG Runs;
} SWFAN_DEVICE,*PSWFAN_DEVICE;
static PSWFAN_DEVICE SwFanGetDevice(void *d){return d;}
static PSWFAN_DEVICE SwFanGetTimer(void *d){return d;}
static ULONG temp=35,sleep_off; static bool sensor_fail,clock_fail;
static POWER_ACTION action; static char order[64];static unsigned next;
static bool backlight_locked;static unsigned registry_writes;
static void event(char c){assert(next<sizeof(order));order[next++]=c;}
static void SwFanTrace(const wchar_t *n,ULONG v){assert(!backlight_locked);registry_writes++;if(!wcscmp(n,L"SleepFanOff"))sleep_off=v;}
static void WdfWaitLockAcquire(void *l,void *t){(void)l;(void)t;assert(!backlight_locked);backlight_locked=true;}
static void WdfWaitLockRelease(void *l){(void)l;assert(backlight_locked);backlight_locked=false;}
static NTSTATUS SwFanReadReg(void *s,UCHAR reg,UCHAR *v){
 (void)s;assert(reg==0||reg==1);if(sensor_fail)return -1;*v=(UCHAR)temp;return 0;
}
static NTSTATUS SwCpuApply(PSWFAN_DEVICE d,BOOLEAN oc){
 assert(!oc);event('c');if(clock_fail)return -1;d->CpuApplied=false;return 0;
}
static ULONG SwCpuKhz(PSWFAN_DEVICE d){return d->CpuApplied?LITE_CPU_KHZ:d->BootCpuKhz;}
static void SwCpuTemperature(PSWFAN_DEVICE d,ULONG t,BOOLEAN valid){
 if(d->CpuConfigured){d->CpuApplied=d->CpuOverclockEnabled&&valid&&t<80;event(d->CpuApplied?'O':'c');}
}
static void write_pwm(PULONG p,ULONG v){*p=v;event(v==(PWM_CSR_EN|PWM_ABSOLUTE_OFF)?'f':'F');}
#define WRITE_REGISTER_ULONG(p,v) write_pwm((p),(v))
#define READ_REGISTER_ULONG(p) (*(p))
static LONG InterlockedIncrement(volatile LONG *p){return ++*p;}
static void WdfTimerStart(void *t,unsigned ms){(void)t;assert(ms==2000);}
static POWER_ACTION WdfDeviceGetSystemPowerAction(void *d){(void)d;return action;}
'''
tests = r'''
static ULONG pwm;
static void init(PSWFAN_DEVICE d){
 memset(d,0,sizeof(*d));d->Pwm=(UCHAR*)&pwm;d->Device=d;
 d->IsLite=d->BacklightReady=d->CpuConfigured=d->CpuOverclockEnabled=true;d->CpuApplied=true;
 d->Config.CpuKhz=LITE_CPU_KHZ;d->BootCpuKhz=998400;d->DisplayState=1;d->Duty=51;
 temp=35;sensor_fail=clock_fail=false;action=PowerActionNone;sleep_off=0;next=0;
}
int main(void){
 SWFAN_DEVICE d;
 init(&d);SwFanEvtTimer(&d);assert(d.Duty==51&&d.CpuApplied&&order[0]=='F'&&order[1]=='O');
 /* Drop CPU overclock before a cool screen-off session stops the fan. */
 init(&d);d.DisplayState=0;SwFanEvtTimer(&d);
 assert(!d.CpuApplied&&d.Duty==0&&d.TempValid&&order[0]=='c'&&order[1]=='f');
 /* Wake must start cooling before the CPU goes back to 2 GHz. */
 d.DisplayState=1;next=0;SwFanEvtTimer(&d);
 assert(d.Duty==51&&d.CpuApplied&&order[0]=='F'&&order[1]=='O');
 /* Screen off is not proof that heat production stopped. */
 init(&d);d.DisplayState=0;temp=55;SwFanEvtTimer(&d);assert(d.Duty==102&&!d.CpuApplied);
 init(&d);d.DisplayState=0;clock_fail=true;SwFanEvtTimer(&d);assert(d.Duty==51&&d.CpuApplied);
 init(&d);d.DisplayState=0;d.CpuConfigured=false;SwFanEvtTimer(&d);assert(d.Duty==51);
 init(&d);d.DisplayState=2;SwFanEvtTimer(&d);assert(d.Duty==51&&d.CpuApplied);
 /* Stock-clock diagnostics retain the ordinary thermal curve on both sides
    of display-off, without the optional overclock's cooling floor. */
 init(&d);d.CpuOverclockEnabled=false;d.CpuApplied=false;
 SwFanEvtTimer(&d);assert(!d.CpuApplied&&d.Duty==0&&d.TempValid);
 d.DisplayState=0;next=0;SwFanEvtTimer(&d);assert(!d.CpuApplied&&d.Duty==0);
 d.DisplayState=1;next=0;temp=55;SwFanEvtTimer(&d);assert(!d.CpuApplied&&d.Duty==102);
 init(&d);d.CpuOverclockEnabled=false;d.CpuApplied=false;d.CpuConfigured=false;
 SwFanEvtTimer(&d);assert(d.Duty==51); /* Unconfirmed clock retains cooling. */
 /* A failed sensor immediately restarts an already stopped fan. */
 init(&d);d.DisplayState=0;d.Duty=0;sensor_fail=true;SwFanEvtTimer(&d);
 assert(d.Duty==153&&!d.TempValid&&!d.CpuApplied&&d.Failures==1);
 /* Existing Erista behavior and shutdown/removal cooling stay intact. */
 init(&d);d.IsLite=false;d.DisplayState=0;SwFanEvtTimer(&d);assert(d.CpuApplied);
 for(action=PowerActionNone;action<=PowerActionShutdownReset;action++){
  POWER_ACTION saved=action;init(&d);action=saved;d.CpuApplied=false;
  d.TempValid=true;d.LastTemp=35;d.Duty=76;SwFanEvtD0Exit(&d,3);
  bool sleeping=action==PowerActionSleep||action==PowerActionHibernate;
  assert(d.Duty==(sleeping?0:76));assert(sleep_off==(unsigned)sleeping);
 }
 init(&d);action=PowerActionSleep;d.CpuApplied=false;d.TempValid=true;d.LastTemp=55;
 d.Duty=76;SwFanEvtD0Exit(&d,3);assert(d.Duty==76&&!sleep_off);
 d.LastTemp=35;d.TempValid=false;SwFanEvtD0Exit(&d,3);assert(d.Duty==76);
 d.TempValid=true;SwFanEvtD0Exit(&d,WdfPowerDeviceD3Final);assert(d.Duty==76);
 d.Duty=0;sleep_off=1;SwFanEvtD0Entry(&d,3);assert(d.Duty==76&&!d.TempValid&&!sleep_off);
 init(&d);d.Stopping=true;SwFanEvtTimer(&d);assert(next==0);
 /* The power callback never writes a registry hive while holding its lock. */
 init(&d);ULONG backlight=0x80000031;d.BacklightPwm=(UCHAR*)&backlight;d.Brightness=150;
 unsigned before=registry_writes;
 for(unsigned i=0;i<40;i++){
  ULONG state=i%2;next=0;
  assert(SwFanDisplayStateCallback(&SwFanDisplayStateGuid,&state,sizeof(state),&d)==0);
  assert(d.DisplayState==state&&((backlight>>16)&0x7fff)==(state?150u:0u));
  assert((backlight&0xffff)==0x31&&!backlight_locked&&registry_writes==before);
 }
 puts("PASS: screen-off cooling, CPU/fan ordering, thermal failures, D0 sleep/wake and shutdown isolation");
}
'''
with tempfile.TemporaryDirectory(prefix='switchfan-test-') as tmp:
    c = Path(tmp) / 'screenoff.c'
    exe = Path(tmp) / 'screenoff'
    backlight = (Path(__file__).resolve().parents[1] / 'src/backlight.h').read_text()
    c.write_text(prefix + backlight + '\n'.join(function(n) for n in names) + tests)
    subprocess.run([os.environ.get('CC', 'clang'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
