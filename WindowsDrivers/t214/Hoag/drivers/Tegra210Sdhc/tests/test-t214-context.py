#!/usr/bin/env python3
"""Exercise actual SD context callbacks with lost CAR/SDHCI state and faults."""
from pathlib import Path
import re
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'src/t214_sdmmc.h').read_text()


def function(name):
    start = re.search(r'static (?:VOID|NTSTATUS) ' + name + r'\([^;]+?\)\s*\{', source).start()
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t ULONG; typedef uint16_t USHORT; typedef uint8_t UCHAR,BOOLEAN,*PUCHAR;
typedef int32_t NTSTATUS;
#define VOID void
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_DEVICE_BUSY -1
#define STATUS_IO_TIMEOUT -2
#define STATUS_DEVICE_CONFIGURATION_ERROR -3
#define STATUS_DEVICE_POWER_FAILURE -4
#define NT_SUCCESS(x) ((x)>=0)
#define RTL_NUMBER_OF(x) (sizeof(x)/sizeof((x)[0]))
#define KeStallExecutionProcessor(x) ((void)(x))
#define SDHC_PRESENT_STATE 0x24
#define SDHC_HOST_CONTROL 0x28
#define SDHC_POWER_CONTROL 0x29
#define SDHC_CLOCK_CONTROL 0x2c
#define SDHC_TIMEOUT_CONTROL 0x2e
#define SDHC_HOST_CONTROL2 0x3e
#define SDHC_CC_INTERNAL_CLOCK_ENABLE 1
#define SDHC_CC_CLOCK_STABLE 2
#define SDHC_CC_CLOCK_ENABLE 4
#define SDHC_HC2_EXECUTE_TUNING 0x40
typedef struct {
 PUCHAR T214Car,BaseAddress;void *OutstandingRequests[1];
 BOOLEAN T214DmaActive,T214ContextValid,T214DmaFault;
 BOOLEAN T214DdrActive,T214DdrRecoverNeeded,T214DdrFatal;
 ULONG T214SavedVendor[5],T214SavedEnables,T214SavedSignals,T214SavedSource,T214SavedBaseClock;
 USHORT T214SavedClock,T214SavedControl2;
 UCHAR T214SavedHost,T214SavedPower,T214SavedTimeout;
 ULONG T214Saves,T214Restores,T214ResumeStatus;
 struct {ULONG BaseClockFrequencyKhz;} Capabilities;
} SDHC_EXTENSION,*PSDHC_EXTENSION;
static UCHAR car[4096],host[512];
static const ULONG bit=1u<<14;
static BOOLEAN gate_fail,reset_fail,source_fail,pll_fail,stable_fail,checking;
static unsigned host_accesses,card_starts,reset_writes;
static PSDHC_EXTENSION current;
static ULONG get(PUCHAR b,ULONG o){ULONG v;memcpy(&v,b+o,4);return v;}
static void put(PUCHAR b,ULONG o,ULONG v){memcpy(b+o,&v,4);}
static void accessible(void){assert((get(car,0x10)&bit)&&!(get(car,4)&bit));host_accesses++;}
static ULONG T214Read(PUCHAR b,ULONG o){if(b==host)accessible();return get(b,o);}
static void T214Write(PUCHAR b,ULONG o,ULONG v){
 if(b==car){
  switch(o){
  case 0x300:assert(v==bit);put(car,4,get(car,4)|v);reset_writes++;break;
  case 0x304:assert(v==bit);if(!reset_fail)put(car,4,get(car,4)&~v);break;
  case 0x320:assert(v==bit);if(!gate_fail)put(car,0x10,get(car,0x10)|v);break;
  case 0x150:
   if((get(car,0x10)&bit)&&!(get(car,4)&bit))assert(!(host[0x2c]&4));
   if(!source_fail)put(car,o,v);break;
  case 0x5a8:put(car,o,v);break;
  case 0x5a4:
   if((v&(1u<<30))&&!pll_fail)v|=1u<<27;
   put(car,o,v);break;
  default:assert(!"unexpected CAR write");
  }return;
 }
 accessible();assert(o==0x100||o==0x1ac||o==0x1c0||o==0x1c4||o==0x1f0);
 put(b,o,v);
}
static ULONG SdhcReadRegisterUlong(PSDHC_EXTENSION s,ULONG o){accessible();return get(s->BaseAddress,o);}
static USHORT SdhcReadRegisterUshort(PSDHC_EXTENSION s,ULONG o){USHORT v;accessible();memcpy(&v,s->BaseAddress+o,2);return v;}
static UCHAR SdhcReadRegisterUchar(PSDHC_EXTENSION s,ULONG o){accessible();return s->BaseAddress[o];}
static void SdhcWriteRegisterUlong(PSDHC_EXTENSION s,ULONG o,ULONG v){
 accessible();assert(o==0x34||o==0x38);
 if(checking&&o==0x38&&v)assert((host[0x2c]&5)==(s->T214SavedClock&5));
 put(s->BaseAddress,o,v);
}
static void SdhcWriteRegisterUshort(PSDHC_EXTENSION s,ULONG o,USHORT v){
 accessible();assert(o==0x2c||o==0x3e);
 if(o==0x2c){
  if(v&4){
   assert(get(car,0x150)==s->T214SavedSource);
   assert((get(car,0x5a4)&0x48000000)==0x48000000);
   assert(SdhcReadRegisterUshort(s,0x3e)==s->T214SavedControl2);
   assert((v&~2u)==s->T214SavedClock);card_starts++;
  }
  if((v&1)&&!stable_fail)v|=2;else v&=~2u;
 }
 memcpy(s->BaseAddress+o,&v,2);
}
static void SdhcWriteRegisterUchar(PSDHC_EXTENSION s,ULONG o,UCHAR v){
 accessible();assert(o==0x28||o==0x29||o==0x2e);s->BaseAddress[o]=v;
}
static const ULONG T214ContextOffsets[]={0x100,0x1ac,0x1c0,0x1c4,0x1f0};
'''
for name in ['T214ParentClock', 'T214SaveContext', 'T214ResumeHostClock', 'T214RestoreContext']:
    code += function(name)

code += r'''
static void init(PSDHC_EXTENSION s,BOOLEAN ddr,USHORT clock){
 memset(s,0,sizeof(*s));memset(car,0,sizeof(car));memset(host,0,sizeof(host));current=s;
 gate_fail=reset_fail=source_fail=pll_fail=stable_fail=checking=FALSE;
 host_accesses=card_starts=reset_writes=0;s->T214Car=car;s->BaseAddress=host;
 s->T214DdrActive=ddr;s->Capabilities.BaseClockFrequencyKhz=ddr?399360:199680;
 put(car,0x10,0x90000002|bit);put(car,4,0x10000004);
 put(car,0x5a4,0x48006804);put(car,0x150,ddr?0xe0000003:0x60000000);
 host[0x28]=0x12;host[0x29]=0xb;host[0x2e]=0xe;
 memcpy(host+0x2c,&clock,2);host[0x3e]=ddr?0x8c:0x8b;
 put(host,0x34,0x00ff0033);put(host,0x38,0x00010001);
 for(unsigned i=0;i<5;i++)put(host,T214ContextOffsets[i],0x12340000+i);
 T214SaveContext(s);assert(s->T214ContextValid&&s->T214Saves==1);checking=TRUE;
}
static void lose(BOOLEAN gate,BOOLEAN reset){
 memset(host,0,sizeof(host));put(car,0x150,0);put(car,0x5a4,1u<<18);
 if(gate)put(car,0x10,get(car,0x10)&~bit);
 if(reset)put(car,4,get(car,4)|bit);
 host_accesses=card_starts=reset_writes=0;
}
int main(void){
 SDHC_EXTENSION s;
 for(unsigned ddr=0;ddr<2;ddr++)for(unsigned loss=0;loss<4;loss++){
  init(&s,ddr,ddr?0x107:7);
  for(unsigned cycle=0;cycle<3;cycle++){
   lose(loss&1,loss&2);T214RestoreContext(&s);
   assert(s.T214Restores==cycle+1&&!s.T214ResumeStatus&&!s.T214DmaFault);
   assert(card_starts==1&&get(car,0x150)==s.T214SavedSource);
   assert(get(car,0x10)==(0x90000002|bit)&&get(car,4)==0x10000004);
   assert(reset_writes==(loss!=0));
   assert(s.Capabilities.BaseClockFrequencyKhz==(ddr?399360u:199680u));
   assert(host[0x28]==0x12&&host[0x29]==0xb&&host[0x2e]==0xe);
   for(unsigned i=0;i<5;i++)assert(get(host,T214ContextOffsets[i])==0x12340000+i);
   assert(!get(host,8)&&!get(host,0xc)&&!get(host,0x30)&&!get(host,0x58));
   T214SaveContext(&s);
  }
 }
 /* A retained, running DDR host must be gated before CAR changes. */
 init(&s,TRUE,0x107);T214RestoreContext(&s);assert(card_starts==1&&!reset_writes);
 /* Clock-off and internal-clock-only snapshots remain off at the card. */
 for(unsigned c=0;c<2;c++){
  init(&s,TRUE,c?0x101:0);lose(TRUE,TRUE);T214RestoreContext(&s);
  assert(!s.T214ResumeStatus&&!card_starts&&!(host[0x2c]&4));
 }
 for(unsigned fault=0;fault<5;fault++){
  init(&s,TRUE,0x107);lose(TRUE,TRUE);
  gate_fail=fault==0;reset_fail=fault==1;source_fail=fault==2;pll_fail=fault==3;stable_fail=fault==4;
  T214RestoreContext(&s);assert(s.T214DmaFault&&s.T214ResumeStatus&&!s.T214Restores);
#if NV_T214_DDR200
  assert(s.T214DdrRecoverNeeded&&s.T214DdrFatal);
#endif
  assert(!card_starts&&!get(host,0x38));if(fault<4)assert(!host_accesses);
 }
 init(&s,FALSE,7);s.OutstandingRequests[0]=&s;T214SaveContext(&s);
 assert(!s.T214ContextValid&&s.T214ResumeStatus==(ULONG)STATUS_DEVICE_BUSY);
 host_accesses=0;T214RestoreContext(&s);assert(!host_accesses&&!card_starts);
 puts("PASS: SD/DDR first-clock ordering, repeated CAR/host loss, untouched I/O state and bounded resume failures");
}
'''
with tempfile.TemporaryDirectory(prefix='t214-context-') as directory:
    p = Path(directory)
    (p / 'test.c').write_text(code)
    for ddr in (0, 1):
        subprocess.run(['clang', '-std=c11', '-Wall', '-Werror', '-Wno-unused-function',
                        '-fsanitize=address,undefined', f'-DNV_T214_DDR200={ddr}',
                        str(p / 'test.c'), '-o', str(p / 'test')], check=True)
        subprocess.run([str(p / 'test')], check=True)
