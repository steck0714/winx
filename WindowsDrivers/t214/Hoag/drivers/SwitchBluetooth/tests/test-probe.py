#!/usr/bin/env python3
"""Run the actual polled probe against firmware/baud failures and retained D0 state."""
from pathlib import Path
import os, re, subprocess, tempfile
source = (Path(__file__).resolve().parents[1] / 'src/switchbt.c').read_text()
names = ('SwBtPrepareHost', 'SwBtReset', 'SwBtReadVersion', 'SwBtPatchram', 'SwBtHighSpeed', 'SwBtProbe', 'SwBtHardwareReset', 'SwBtProbeWithRecovery', 'SwBtStart', 'SwBtEvtD0Entry', 'SwBtEvtD0Exit', 'SwBtEvtInterruptEnable', 'SwBtEvtInterruptDisable')
functions = []
for name in names:
    m = re.search(r'^(?:static )?(?:BOOLEAN|ULONG|VOID|NTSTATUS) ' + name + r'\([^;{]*\)\s*\{', source, re.M)
    assert m, name
    a=source.index('{',m.start()); b=a+1; depth=1
    while depth:
        depth += (source[b]=='{')-(source[b]=='}'); b+=1
    functions.append(source[m.start():b])
constants='\n'.join(line for line in source.splitlines() if re.match(r'#define (?:UART_MCR_|UART_DIV_|CAR_|BCM_|HCI_(?:RESET|READ_))',line))
preamble=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>
typedef void VOID; typedef uint8_t UCHAR,*PUCHAR; typedef uint16_t USHORT;
typedef uint32_t ULONG,*PULONG; typedef wchar_t WCHAR; typedef bool BOOLEAN;
#define TRUE true
#define FALSE false
#define MAXULONG UINT32_MAX
#define RTL_NUMBER_OF(x) (sizeof(x)/sizeof((x)[0]))
#define POOL_TAG 0
#define UART_MCR 4
typedef void *PVOID;
typedef struct { uint64_t QuadPart; } PHYSICAL_ADDRESS;
#define PAGE_READONLY 1
#define PAGE_READWRITE 2
#define PAGE_NOCACHE 4
#define UART_IER 1
#define UART_IER_RX 0x15
#define CHILD_DELAY_TICKS 10
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define STATUS_SUCCESS 0
#define STATUS_DEVICE_HARDWARE_ERROR -1
#define STATUS_DEVICE_NOT_READY -2
#define STATUS_DEVICE_POWER_FAILURE -3
#define STATUS_INVALID_DEVICE_STATE -4
#define NT_SUCCESS(s) ((s)>=0)
typedef int NTSTATUS,WDF_POWER_DEVICE_STATE;
struct DEVICE { volatile ULONG *CarUartd,*Uart,*Car; BOOLEAN HostReady,HighSpeedNow,FirmwareVerified,D0Active,ChipOk,BootGuardPending,TxBusy,TxDone; ULONG FirmwareVersion,Lock,RxGot,ChildTick,Ticks,RxHead,RxTail,Ier; UCHAR BdAddr[6]; };
typedef struct DEVICE *WDFDEVICE,*WDFINTERRUPT;
static ULONG gpio[41],hid=0x2120,uart[16],hwresets,boot_pending;
static bool hardware_recovers;

typedef struct DEVICE SWBT_DEVICE,*PSWBT_DEVICE;
static ULONG car_regs[CAR_LENGTH/4],clock_ops,uart_writes;
#define car (car_regs[(CAR_UARTD_SOURCE-CAR_BASE)/4])
static bool clock_blocked,reset_blocked;
static ULONG host_baud,chip_baud,version,patch_record,downloads,attempts,commands;
static ULONG failed_record,failed_opcode,fw_result,high_result;
static bool dead,reset_recovers,fail_once,bad_status,baud_reject,baud_lost_reply;
static bool reject_address,no_version; static int fail_record; static unsigned fw_loads;
static UCHAR firmware[3*7];static ULONG firmware_size;static ULONG sleep_ms;
static void SwBtTrace(const WCHAR *name,ULONG v) {
 if(!wcscmp(name,L"FirmwareFailedRecord"))failed_record=v;
 if(!wcscmp(name,L"FirmwareFailedOpcode"))failed_opcode=v;
 if(!wcscmp(name,L"FirmwareResult"))fw_result=v;
 if(!wcscmp(name,L"HighSpeedResult"))high_result=v;
 if(!wcscmp(name,L"BootPending"))boot_pending=v;
}
static void SwBtTraceString(const WCHAR *n,const WCHAR *v){(void)n;(void)v;}
static void SwBtSleepMs(ULONG n){sleep_ms+=n;}
static void KeStallExecutionProcessor(ULONG n){(void)n;}
static void SwBtFlushRx(PSWBT_DEVICE d){(void)d;}
static void SwBtWrite(PSWBT_DEVICE d,ULONG r,ULONG v){assert(d->HostReady);(void)r;(void)v;uart_writes++;}
static ULONG READ_REGISTER_ULONG(PULONG p){return *p;}
static void WRITE_REGISTER_ULONG(PULONG p,ULONG v){
 if(p==gpio+40){
  ULONG previous=gpio[8],mask=(v>>8)&0xff;gpio[8]=(gpio[8]&~mask)|(v&mask);
  if(!(previous&16)&&(gpio[8]&16)){
   hwresets++;assert(!(gpio[8]&8));chip_baud=115200;version=0x24092000;
   if(hardware_recovers)dead=false;
  }
 }else if((uintptr_t)p>=(uintptr_t)car_regs&&(uintptr_t)p<(uintptr_t)(car_regs+CAR_LENGTH/4)) {
  ULONG off=(ULONG)((UCHAR *)p-(UCHAR *)car_regs);
  if(p==&car){*p=v;return;}
  assert(v==CAR_UARTD_BIT);clock_ops++;
  if(off==CAR_RESET_U_SET)car_regs[CAR_RESET_U/4]|=v;
  else if(off==CAR_RESET_U_CLEAR){if(!reset_blocked)car_regs[CAR_RESET_U/4]&=~v;}
  else {assert(off==CAR_CLOCK_U_SET);if(!clock_blocked)car_regs[CAR_CLOCK_U/4]|=v;}
 }else *p=v;
}
static PVOID MmMapIoSpaceEx(PHYSICAL_ADDRESS a,size_t n,unsigned flags){
 (void)flags;
 if(a.QuadPart==0x70000804){assert(n==4);return &hid;}
 assert(a.QuadPart==0x6000d10c&&n==0xa4);return gpio;
}
static void MmUnmapIoSpace(PVOID p,size_t n){assert((p==&hid&&n==4)||(p==gpio&&n==0xa4));}
static PSWBT_DEVICE SwBtGetDevice(WDFDEVICE d){return d;}
static void WdfSpinLockAcquire(ULONG lock){(void)lock;}
static void WdfSpinLockRelease(ULONG lock){(void)lock;}
static void SwBtFreeKept(PSWBT_DEVICE d){(void)d;}
static void SwBtStage(ULONG stage){(void)stage;}
static void SwBtFlush(void){}

static void SwBtUartInit(PSWBT_DEVICE d,ULONG div,ULONG mcr){(void)d;(void)mcr;host_baud=div==221?115200:3000000;}
static void SwBtTraceVersion(PSWBT_DEVICE d,UCHAR *e,ULONG max){(void)d;(void)e;(void)max;}
static PUCHAR SwBtLoadFirmware(PULONG n){UCHAR *b=malloc(firmware_size);assert(b);memcpy(b,firmware,firmware_size);*n=firmware_size;fw_loads++;return b;}
static void ExFreePoolWithTag(void *p,ULONG tag){(void)tag;free(p);}
static void SwBtChooseAddress(PSWBT_DEVICE d){memset(d->BdAddr,1,6);}
static BOOLEAN SwBtReadParam(const WCHAR *n,PULONG v){(void)n;(void)v;return FALSE;}
static void RtlStringCchPrintfW(WCHAR *out,size_t n,const WCHAR *fmt,...){(void)fmt;assert(n);out[0]=0;}
'''
model=r'''
static ULONG SwBtPollCommand(PSWBT_DEVICE d,USHORT opcode,const UCHAR *params,UCHAR plen,UCHAR *event,ULONG max) {
 (void)d;(void)plen;assert(max>=15);commands++;
 if(host_baud!=chip_baud)return 0;
 memset(event,0,max);
 if(opcode==HCI_RESET) { if(dead&&!reset_recovers)return 0;dead=false;return 7; }
 if(dead)return 0;
 if(opcode==BCM_DOWNLOAD_MINI){downloads++;patch_record=0;return 7;}
 if(opcode==0xfc4c||opcode==0xfc4e) {
   if((int)patch_record==fail_record&&(!fail_once||attempts++==0)) {
     if(bad_status){event[6]=0x12;return 7;}dead=true;return 0;
   }
   patch_record++;if(opcode==0xfc4e)version=0x43211234;return 7;
 }
 if(opcode==BCM_UPDATE_BAUD) {
   if(baud_reject){event[6]=0x12;return 7;}
   chip_baud=(ULONG)params[2]|((ULONG)params[3]<<8)|((ULONG)params[4]<<16);
   return baud_lost_reply?0:7;
 }
 if(opcode==HCI_READ_VERSION) {
   if(no_version)return 0;
   event[8]=(UCHAR)version;event[9]=(UCHAR)(version>>8);
   event[13]=(UCHAR)(version>>16);event[14]=(UCHAR)(version>>24);return 15;
 }
 if(opcode==BCM_WRITE_BD_ADDR){event[6]=reject_address?0x12:0;return 7;}
 if(opcode==HCI_READ_BD_ADDR)return 13;
 if(opcode==HCI_READ_NAME)return 8;
 assert(false);return 0;
}
'''
tests=r'''
static void init(SWBT_DEVICE *d) {
 memset(d,0,sizeof(*d));memset(car_regs,0,sizeof(car_regs));
 d->Car=car_regs;d->CarUartd=&car;d->Uart=uart;d->HostReady=true;
 car_regs[CAR_PLLP_BASE/4]=CAR_PLLP_READY;car_regs[CAR_CLOCK_U/4]=0x82;car_regs[CAR_RESET_U/4]=0xa0;
 clock_ops=uart_writes=0;clock_blocked=reset_blocked=false;
 memset(gpio,0,sizeof(gpio));gpio[0]=gpio[4]=0x18;gpio[8]=0xf8;hid=0x2120;hwresets=boot_pending=0;hardware_recovers=true;
 host_baud=chip_baud=115200;version=0x24092000;
 patch_record=downloads=attempts=commands=fw_loads=sleep_ms=0;
 failed_record=failed_opcode=fw_result=high_result=0;
 dead=reset_recovers=fail_once=bad_status=baud_reject=baud_lost_reply=false;
 reject_address=no_version=false;fail_record=-1;
 UCHAR f[]={0x4c,0xfc,4,1,2,3,4,0x4c,0xfc,4,5,6,7,8,0x4e,0xfc,4,255,255,255,255};
 memcpy(firmware,f,sizeof(f));firmware_size=sizeof(f);
}
int main(void) {
 SWBT_DEVICE d;UCHAR event[260];init(&d);
 assert(SwBtProbe(&d)&&d.FirmwareVerified&&d.HighSpeedNow&&downloads==1&&patch_record==3);
 /* D0 exit moves the UART back to ROM speed, but the patch remains resident. */
 chip_baud=115200;host_baud=115200;
 assert(SwBtProbe(&d)&&downloads==1&&fw_loads==1&&fw_result==2);
 /* Actual hardware power loss restores the ROM revision: load again. */
 chip_baud=115200;version=0x24092000;
 assert(SwBtProbe(&d)&&downloads==2&&fw_loads==2);
 /* Mid-download timeout must not publish an apparently healthy radio. */
 init(&d);fail_record=1;assert(!SwBtProbe(&d)&&!d.FirmwareVerified);
 assert(fw_result==0xf0000003&&failed_record==1&&failed_opcode==0xfc4c&&downloads==1);
 /* One complete retry is allowed only after an acknowledged reset. */
 init(&d);fail_record=1;fail_once=true;reset_recovers=true;
 assert(SwBtProbe(&d)&&downloads==2&&d.FirmwareVerified);
 /* A negative HCI status is not successful firmware download. */
 init(&d);fail_record=1;bad_status=true;assert(!SwBtProbe(&d));
 assert(downloads==2&&fw_result==0xf0000005&&!d.FirmwareVerified);
 /* Truncated headers/payloads are rejected before entering the minidriver. */
 for(ULONG n=1;n<7;n++) {
   init(&d);assert(SwBtPatchram(&d,firmware,n,event,sizeof(event))==0&&commands==0);
 }
 init(&d);firmware_size--;assert(!SwBtProbe(&d)&&downloads==0);
 /* Baud-change ACK loss: verify the new rate, do not assume 115200. */
 init(&d);baud_lost_reply=true;assert(SwBtProbe(&d)&&d.HighSpeedNow&&high_result==2);
 init(&d);baud_reject=true;assert(SwBtProbe(&d)&&!d.HighSpeedNow&&high_result==1);
 init(&d);dead=true;assert(!SwBtHighSpeed(&d,event,sizeof(event))&&high_result==4);
 init(&d);no_version=true;assert(!SwBtProbe(&d)&&downloads==0);
 init(&d);reject_address=true;assert(!SwBtProbe(&d));
 /* A complete healthy probe never pulses BT_REG_ON. */
 init(&d);assert(SwBtProbeWithRecovery(&d)&&hwresets==0);
 init(&d);dead=true;
 assert(SwBtProbeWithRecovery(&d)&&hwresets==1&&d.FirmwareVerified);
 assert((gpio[8]&~0x18u)==0xe0); /* unrelated pins are preserved. */
 init(&d);dead=true;hardware_recovers=false;
 assert(!SwBtProbeWithRecovery(&d)&&hwresets==1&&!d.FirmwareVerified);
 init(&d);dead=true;hid=0x2100;
 assert(!SwBtProbeWithRecovery(&d)&&hwresets==0);
 init(&d);dead=true;gpio[4]=0;
 assert(!SwBtProbeWithRecovery(&d)&&hwresets==0);
 init(&d);d.D0Active=true;assert(!SwBtHardwareReset(&d)&&hwresets==0);
 init(&d);fail_record=1;fail_once=true;
 assert(SwBtProbeWithRecovery(&d)&&hwresets==1&&downloads==2&&d.FirmwareVerified);
 /* A persisted old BootPending record must not gate a new D0 attempt. */
 init(&d);boot_pending=1;
 assert(SwBtEvtD0Entry(&d,0)==0&&d.ChipOk&&boot_pending==1);
 assert(d.HostReady&&clock_ops==0); /* Healthy cold entry leaves CAR gates alone. */
 assert(!d.ChildTick||d.ChildTick==10);
 assert(SwBtEvtD0Exit(&d,0)==0&&boot_pending==0&&!d.HighSpeedNow&&!d.HostReady);
 /* Three independent host-context losses recover before HCI probing. */
 for(unsigned cycle=0;cycle<3;cycle++) {
  car_regs[CAR_CLOCK_U/4]&=~CAR_UARTD_BIT;car_regs[CAR_RESET_U/4]|=CAR_UARTD_BIT;
  assert(SwBtEvtD0Entry(&d,3)==0&&d.ChipOk&&d.HostReady&&downloads==1);
  assert(clock_ops==3*(cycle+1)&&car_regs[CAR_CLOCK_U/4]==0x82&&car_regs[CAR_RESET_U/4]==0xa0);
  assert(car_regs[CAR_PLLP_BASE/4]==CAR_PLLP_READY&&hwresets==0);
  assert(SwBtEvtInterruptEnable(&d,&d)==0&&d.Ier==UART_IER_RX);
  assert(SwBtEvtInterruptDisable(&d,&d)==0&&d.Ier==0);
  assert(SwBtEvtD0Exit(&d,3)==0&&!d.HostReady);
 }
 /* Do not touch an inaccessible UART, retry firmware or claim a radio. */
 init(&d);d.ChipOk=true;car_regs[CAR_PLLP_BASE/4]=1u<<30;
 assert(SwBtEvtD0Entry(&d,3)==STATUS_DEVICE_POWER_FAILURE&&!d.ChipOk&&!d.HostReady);
 assert(commands==0&&uart_writes==0&&hwresets==0&&clock_ops==0&&boot_pending==0);
 assert(SwBtEvtInterruptEnable(&d,&d)==STATUS_DEVICE_NOT_READY);
 assert(SwBtEvtInterruptDisable(&d,&d)==0&&uart_writes==0);
 for(unsigned i=0;i<2;i++) {
  init(&d);car_regs[CAR_CLOCK_U/4]&=~CAR_UARTD_BIT;
  clock_blocked=(i==0);reset_blocked=(i==1);
  assert(SwBtEvtD0Entry(&d,3)==STATUS_DEVICE_POWER_FAILURE&&!d.HostReady&&!d.ChipOk);
  assert(commands==0&&uart_writes==0&&hwresets==0);
  assert(SwBtEvtD0Exit(&d,3)==0&&uart_writes==0);
 }
 init(&d);d.D0Active=true;
 assert(SwBtPrepareHost(&d)==STATUS_INVALID_DEVICE_STATE&&d.HostReady&&clock_ops==0);
 init(&d);dead=true;hardware_recovers=false;
 assert(SwBtEvtD0Entry(&d,0)==STATUS_DEVICE_HARDWARE_ERROR&&!d.ChipOk&&boot_pending==0);
 puts("PASS: UART clock/reset recovery, inaccessible-host rejection, strict patch/baud handling, bounded GPIO retry and D0 cleanup");

}
'''
with tempfile.TemporaryDirectory(prefix='switchbt-probe-') as tmp:
 c=Path(tmp)/'test.c';exe=Path(tmp)/'test'
 c.write_text(constants+'\n'+preamble+model+'\n'.join(functions)+tests)
 subprocess.run([os.environ.get('CC','clang'),'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
