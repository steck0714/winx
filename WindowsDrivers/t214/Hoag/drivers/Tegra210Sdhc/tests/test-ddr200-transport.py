#!/usr/bin/env python3
"""Run the actual private DDR200 transaction with an SDHCI/card fault model.

The model checks sequencing and failure handling, not electrical timing.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
typedef uint32_t ULONG,*PULONG;typedef uint16_t USHORT;
typedef uint8_t UCHAR,*PUCHAR;typedef bool BOOLEAN;typedef int NTSTATUS;
#define VOID void
#define TRUE true
#define FALSE false
#define MAXULONG UINT32_MAX
#define NT_SUCCESS(s) ((s)>=0)
#define RtlCopyMemory memcpy
#define InterlockedExchange(p,v) (*(p)=(v))
enum { STATUS_SUCCESS=0,STATUS_IO_TIMEOUT=-1,STATUS_IO_DEVICE_ERROR=-2,
 STATUS_INVALID_PARAMETER=-3,STATUS_DEVICE_BUSY=-4,STATUS_DEVICE_PROTOCOL_ERROR=-5,
 STATUS_DEVICE_CONFIGURATION_ERROR=-6,STATUS_NOT_SUPPORTED=-7,
 STATUS_INVALID_DEVICE_STATE=-8,STATUS_WRONG_VOLUME=-9,STATUS_DATA_ERROR=-10 };
enum {SDHC_BLOCK_SIZE=4,SDHC_BLOCK_COUNT=6,SDHC_ARGUMENT=8,SDHC_TRANSFER_MODE=12,
 SDHC_COMMAND=14,SDHC_RESPONSE=16,SDHC_DATA_PORT=32,SDHC_PRESENT_STATE=36,
 SDHC_CLOCK_CONTROL=44,SDHC_TIMEOUT_CONTROL=46,SDHC_RESET=47,
 SDHC_INTERRUPT_STATUS=48,SDHC_HOST_CONTROL2=62};
#define SDHC_RESET_CMD 2
#define SDHC_RESET_DAT 4
#define SDHC_CC_CLOCK_STABLE 2
#define SDHC_CC_CLOCK_ENABLE 4
#define SDHC_IS_CMD_COMPLETE 1
#define SDHC_IS_TRANSFER_COMPLETE 2
#define SDHC_IS_BUFFER_READ_READY 32
#define SDHC_IS_ERROR_INTERRUPT 0x8000
#define SDHC_TM_TRANSFER_READ 16
#define SDHC_HC2_EXECUTE_TUNING 64
#define SDHC_HC2_SELECT_SAMPLING_CLOCK 128
#define WINARM_FIRST 1000
#define LiteWriteFacts 25
#if TEST_VISIBLE
#define FAT32_FIRST 2000
#define LiteFatBoot 32
#endif
enum {SdResetTypeAll,SdBusVoltage33,SdBusWidth1Bit=1,SdBusWidth4Bit=4,
 SdhcSpeedModeNormal=0,SdhcSpeedModeSDR104=7};
typedef struct {
 PUCHAR BaseAddress,T214Car;BOOLEAN T214DmaActive,T214Voltage18,T214Tuned;
 void *OutstandingRequests[1];
 struct {ULONG BaseClockFrequencyKhz;} Capabilities;
 UCHAR T214Scr[8],T214SwitchStatus[64],T214Cid[16];
 ULONG T214CardInfoValid,T214Rca,T214Tap;
 BOOLEAN T214DdrAttempted,T214DdrActive,T214DdrFatal,T214DdrRecoverNeeded;
 ULONG T214DdrStatus,T214DdrRecoveryStatus,T214DdrFallbacks;
 ULONG T214DdrWindow,T214DdrTap,T214DdrClockKhz,TargetFacts,SpeedMode;
} SDHC_EXTENSION,*PSDHC_EXTENSION;
static struct {ULONG ClockKhzRequested,ClockKhzActual,CarSdmmc1After,RegClockControl;} NvDiag;
static UCHAR regs[512],car[4096],fifo[512];
static ULONG cid[4],card_rca,card_mode,command_count,power_cycles,read_count;
static ULONG last_command,last_argument,read_offset,read_size,commands[2048],clock_us;
static unsigned good_first,good_last;
static bool timeout_tuning,corrupt_ddr,lost_vendor_reply,power_fails,wrong_cid,changed_rca;
static bool clock_fails,cmd_timeout,app_prefix,reset_stuck;
static ULONG raw32(PUCHAR p,ULONG off){ULONG v;memcpy(&v,p+off,4);return v;}
static void put32(PUCHAR p,ULONG off,ULONG v){memcpy(p+off,&v,4);}
static USHORT raw16(PUCHAR p,ULONG off){USHORT v;memcpy(&v,p+off,2);return v;}
static void put16(PUCHAR p,ULONG off,USHORT v){memcpy(p+off,&v,2);}
static void sector(PUCHAR p,ULONG lba){
 for(unsigned i=0;i<512;i++)p[i]=(UCHAR)(i*37+lba);
 p[0]=lba==0?0x5a:lba==WINARM_FIRST?0xa5:0xf1;p[510]=0x55;p[511]=0xaa;
}
static int LiteBytesEqual(const UCHAR *a,const UCHAR *b,unsigned n){return !memcmp(a,b,n);}
static int LiteSelectWindowsPartition(UCHAR *p){return p[0]==0x5a&&p[510]==0x55&&p[511]==0xaa;}
static int LiteIsBootSector(const UCHAR *p){return p[0]==0xa5&&p[510]==0x55&&p[511]==0xaa;}
#if TEST_VISIBLE
static int LiteIsFatBootSector(const UCHAR *p){return p[0]==0xf1&&p[510]==0x55&&p[511]==0xaa;}
#endif
static void model_command(USHORT value){
 ULONG index=value>>8,arg=raw32(regs,8);last_command=index;last_argument=arg;
 assert(command_count<2048);commands[command_count++]=index;
 read_offset=0;read_size=raw16(regs,4);memset(fifo,0,sizeof(fifo));
 put32(regs,0x30,1|(read_size?32:2));put32(regs,0x10,0x900);
 if(cmd_timeout){put32(regs,0x30,0);return;}
 if(index==19){assert(!(raw16(regs,0x2c)&4));put32(regs,0x30,0);return;}
 if(index==0){card_mode=0;app_prefix=false;}
 else if(index==2){for(unsigned i=0;i<4;i++)put32(regs,0x10+i*4,cid[i]^(wrong_cid&&i==0?1:0));}
 else if(index==3)put32(regs,0x10,changed_rca?card_rca+0x10000:card_rca);
 else if(index==8)put32(regs,0x10,0x1aa);
 else if(index==55){put32(regs,0x10,0x920);app_prefix=true;}
 else if(index==41){assert(app_prefix);app_prefix=false;put32(regs,0x10,0xc1ff8000);}
 else if(index==6&&!read_size){assert(app_prefix&&arg==2);app_prefix=false;}
 else if(index==6){
  assert(read_size==64);
  unsigned access=arg&15,system=(arg>>4)&15;
  if(arg&0x80000000u){
   if(access!=15)card_mode=(card_mode&0xf0)|access;
   if(system!=15)card_mode=(card_mode&15)|(system<<4);
  }
  fifo[10]=0x40;fifo[13]=8;fifo[16]=(UCHAR)card_mode;
  if(lost_vendor_reply&&system==14){put32(regs,0x30,0);lost_vendor_reply=false;}
 }else if(index==17){
  assert(read_size==512);read_count++;sector(fifo,arg);
  if(corrupt_ddr&&(card_mode>>4)==14)fifo[23]^=1;
 }
}
static ULONG T214Read(PUCHAR b,ULONG off){
 if(b==regs&&off==SDHC_DATA_PORT){
  assert(read_offset+4<=read_size);ULONG v=raw32(fifo,read_offset);read_offset+=4;
  if(read_offset==read_size)put32(regs,0x30,raw32(regs,0x30)|2);
  return v;
 }
 return raw32(b,off);
}
static void T214Write(PUCHAR b,ULONG off,ULONG v){
 if(b==regs&&off==0x30)put32(b,off,raw32(b,off)&~v);else put32(b,off,v);
}
static USHORT SdhcReadRegisterUshort(PSDHC_EXTENSION s,ULONG off){return raw16(s->BaseAddress,off);}
static UCHAR SdhcReadRegisterUchar(PSDHC_EXTENSION s,ULONG off){return s->BaseAddress[off];}
static void SdhcWriteRegisterUlong(PSDHC_EXTENSION s,ULONG off,ULONG v){T214Write(s->BaseAddress,off,v);}
static void SdhcWriteRegisterUshort(PSDHC_EXTENSION s,ULONG off,USHORT v){
 if(off==0x30)put16(s->BaseAddress,off,raw16(s->BaseAddress,off)&~v);
 else {
  if(off==0x2c&&(v&1)&&!clock_fails)v|=2;
  put16(s->BaseAddress,off,v);
  if(off==SDHC_COMMAND)model_command(v);
 }
}
static void SdhcWriteRegisterUchar(PSDHC_EXTENSION s,ULONG off,UCHAR v){
 s->BaseAddress[off]=v;
 if(off==SDHC_RESET&&!reset_stuck){
  s->BaseAddress[off]=0;
  if(last_command==19){
   unsigned tap=(raw32(regs,0x100)>>16)&255;
   USHORT hc=raw16(regs,0x3e);
   if(tap>=good_first&&tap<=good_last)hc|=128;else hc&=~128u;
   if(tap==127)hc&=~64u;
   put16(regs,0x3e,hc);put32(regs,0x30,timeout_tuning?0:32);
  }
 }
}
static void KeStallExecutionProcessor(ULONG us){clock_us+=us;}
static void SdPortWait(ULONG us){clock_us+=us;}
static BOOLEAN T214WaitClear(PUCHAR b,ULONG off,ULONG mask,ULONG us){(void)us;return !(raw32(b,off)&mask);}
static NTSTATUS SdhcConvertErrorToStatus(USHORT e){(void)e;return STATUS_IO_DEVICE_ERROR;}
static NTSTATUS T214Power(PSDHC_EXTENSION s,BOOLEAN on){
 assert(on);power_cycles++;if(power_fails)return STATUS_IO_DEVICE_ERROR;
 s->T214DdrActive=false;s->T214Voltage18=false;s->T214CardInfoValid=s->T214Rca=s->TargetFacts=0;
 s->SpeedMode=0;card_mode=0;clock_fails=false;memset(regs,0,sizeof(regs));return 0;
}
static NTSTATUS SdhcResetHost(PSDHC_EXTENSION s,int type){(void)type;assert(!s->T214DdrActive);return 0;}
static NTSTATUS T214SetBusVoltage(PSDHC_EXTENSION s,int v){(void)s;assert(v==SdBusVoltage33);return 0;}
static NTSTATUS SdhcSetBusWidth(PSDHC_EXTENSION s,int w){(void)s;assert(w==1||w==4);return 0;}
static NTSTATUS SdhcSetClock(PSDHC_EXTENSION s,ULONG rate){assert(!s->T214DdrActive);NvDiag.ClockKhzActual=rate;return 0;}
static NTSTATUS T214Signaling(PSDHC_EXTENSION s,BOOLEAN on){assert(on&&last_command==11);s->T214Voltage18=true;put16(regs,0x3e,8);return 0;}
static NTSTATUS T214Tune(PSDHC_EXTENSION s){assert(card_mode==3);s->T214Tuned=true;return 0;}
'''
code += '#include "' + str(root / 'src/t214_ddr200.h') + '"\n'
code += r'''
static void init(SDHC_EXTENSION *s){
 memset(s,0,sizeof(*s));memset(regs,0,sizeof(regs));memset(car,0,sizeof(car));
 s->BaseAddress=regs;s->T214Car=car;s->T214Voltage18=true;s->SpeedMode=SdhcSpeedModeSDR104;
 s->T214CardInfoValid=7;s->T214Rca=card_rca=0x12340000;s->TargetFacts=LiteWriteFacts;
 s->T214Scr[0]=2;s->T214Scr[1]=4;s->T214Scr[2]=0x80;s->T214Scr[3]=0x80;
 s->T214SwitchStatus[10]=0x40;s->T214SwitchStatus[13]=8;s->T214SwitchStatus[16]=3;
 for(unsigned i=0;i<4;i++)cid[i]=i+1;memcpy(s->T214Cid,cid,16);
 put32(car,0x5a4,0x48006804);put32(car,0x150,3u<<29);
 put16(regs,0x2c,7);put16(regs,0x3e,0x8b);put32(regs,0x34,0x12345678);put32(regs,0x38,0x87654321);
 card_mode=3;command_count=power_cycles=read_count=last_command=clock_us=0;
 good_first=32;good_last=95;
 timeout_tuning=corrupt_ddr=lost_vendor_reply=power_fails=wrong_cid=changed_rca=false;
 clock_fails=cmd_timeout=app_prefix=reset_stuck=false;
}
int main(void){
 SDHC_EXTENSION s;ULONG data[128];
 unsigned expected_facts=LiteWriteFacts;
#ifdef FAT32_FIRST
 expected_facts|=LiteFatBoot;
#endif
 init(&s);s.T214Scr[3]=0;
 assert(T214DdrTry(&s)==0&&!s.T214DdrActive&&command_count==0&&power_cycles==0);
 init(&s);s.T214CardInfoValid=0;
 assert(T214DdrTry(&s)==0&&!s.T214DdrAttempted&&command_count==0);
 init(&s);assert(T214DdrTry(&s)==0&&s.T214DdrActive&&card_mode==0xe0);
 assert(s.T214DdrWindow==64&&s.T214DdrTap==63&&NvDiag.ClockKhzActual==199680);
 assert(read_count==5&&power_cycles==0&&raw32(car,0x150)==0xe0000003);
 assert((raw16(regs,0x3e)&7)==4&&s.T214Tuned);
 assert(T214DdrClock(&s,0)==0&&NvDiag.ClockKhzActual==0&&!(raw16(regs,0x2c)&5));
 assert(T214DdrClock(&s,200000)==0&&NvDiag.ClockKhzActual==199680);
 assert(T214DdrClock(&s,208000)==0&&NvDiag.ClockKhzRequested==200000&&NvDiag.ClockKhzActual==199680);
 /* Re-tuning stays in DDR, with no extra vendor switch or power cycle. */
 assert(T214DdrTry(&s)==0&&s.T214DdrActive&&power_cycles==0);
 good_first=40;good_last=46;
 assert(T214DdrTry(&s)==0&&!s.T214DdrActive&&!s.T214DdrFatal&&power_cycles==1);
 assert(s.T214DdrWindow==7&&s.T214DdrFallbacks==1&&card_mode==3&&s.TargetFacts==expected_facts);
 /* Reset-all loses the SDPORT-owned event masks. Recovery must restore both
  * masks before the next interrupt-driven disk request can complete. */
 assert(raw32(regs,0x34)==0x12345678&&raw32(regs,0x38)==0x87654321);
 unsigned before=command_count;assert(T214DdrTry(&s)==0&&command_count==before);
 init(&s);good_last=38;
 assert(T214DdrTry(&s)==0&&!s.T214DdrActive&&s.T214DdrWindow==7&&power_cycles==1);
 init(&s);lost_vendor_reply=true;
 assert(T214DdrTry(&s)==0&&!s.T214DdrActive&&card_mode==3&&power_cycles==1);
 init(&s);corrupt_ddr=true;
 assert(T214DdrTry(&s)==0&&!s.T214DdrActive&&(NTSTATUS)s.T214DdrStatus==STATUS_DATA_ERROR);
 init(&s);timeout_tuning=true;
 assert(T214DdrTry(&s)==0&&!s.T214DdrActive&&clock_us<2000000);
 init(&s);good_last=38;changed_rca=true;
 assert(T214DdrTry(&s)==STATUS_DEVICE_PROTOCOL_ERROR&&s.T214DdrFatal&&s.TargetFacts==0);
 assert(raw32(regs,0x34)==0x12345678&&raw32(regs,0x38)==0x87654321);
 /* Only a new native SDPORT enumeration may repair a changed RCA. */
 assert(!s.T214DdrRecoverNeeded);
 card_mode=3;s.T214CardInfoValid=4;s.T214Rca=0x43210000;
 assert(T214DdrTry(&s)==0&&!s.T214DdrFatal&&!s.T214DdrActive&&s.TargetFacts==0);
 init(&s);good_last=38;wrong_cid=true;
 assert(T214DdrTry(&s)==STATUS_WRONG_VOLUME&&s.T214DdrFatal&&s.TargetFacts==0);
 init(&s);good_last=38;power_fails=true;
 assert(T214DdrTry(&s)==STATUS_IO_DEVICE_ERROR&&s.T214DdrFatal);
 assert(raw32(regs,0x34)==0x12345678&&raw32(regs,0x38)==0x87654321);
 init(&s);clock_fails=true;
 assert(T214DdrTry(&s)==0&&!s.T214DdrActive&&power_cycles==1);
 /* Restoring zero masks matters too: a polling/crash-free caller must not
  * acquire interrupts it never requested. Successful recovery clears private
  * completion flags before any original signal mask is restored. */
 init(&s);good_last=38;put32(regs,0x34,0);put32(regs,0x38,0);
 assert(T214DdrTry(&s)==0&&!s.T214DdrActive&&!s.T214DdrFatal);
 assert(raw32(regs,0x34)==0&&raw32(regs,0x38)==0&&raw32(regs,0x30)==0);
 init(&s);s.OutstandingRequests[0]=&s;
 assert(T214DdrTry(&s)==STATUS_DEVICE_BUSY&&command_count==0);
 init(&s);
 assert(T214DdrCommand(&s,24,WINARM_FIRST,0x1a,data,sizeof(data),NULL)==STATUS_INVALID_PARAMETER);
 assert(T214DdrCommand(&s,17,WINARM_FIRST,0x1a,data,516,NULL)==STATUS_INVALID_PARAMETER);
 assert(command_count==0);
 cmd_timeout=true;
 assert(T214DdrCommand(&s,13,card_rca,0x1a,NULL,0,NULL)==STATUS_IO_TIMEOUT&&clock_us==50000);
 assert(raw32(regs,0x34)==0x12345678&&raw32(regs,0x38)==0x87654321);
 puts("PASS: actual DDR transaction/tuning, readback mismatch, narrow-window/timeout recovery, RCA/CID fail-closed, SDPORT event-mask restoration, no private data writes");
}
'''
with tempfile.TemporaryDirectory(prefix='lite-ddr200-transport-') as temp:
    path = Path(temp)
    (path / 'test.c').write_text(code)
    for visible in (0, 1):
        subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        f'-DTEST_VISIBLE={visible}', '-fsanitize=address,undefined',
                        '-g', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)
