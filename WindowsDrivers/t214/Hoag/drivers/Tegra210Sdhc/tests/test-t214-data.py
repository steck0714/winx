#!/usr/bin/env python3
"""Run the actual target filter, write guard, bounce DMA and clock calculator.

Pass a generated winarm-target.h. It stays local; no card identity is committed.
"""
from pathlib import Path
import re, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/nvtsdhc.c').read_text()
board = (root / 'src/t214_sdmmc.h').read_text()
target = Path(sys.argv[1]).read_text()

def function(text, name, result):
    start = re.search(r'\b' + name + r'\s*\(', text).start()
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return result + ' ' + text[start:end] + '\n'

prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
typedef uint32_t ULONG, *PULONG; typedef int32_t LONG;
typedef size_t SIZE_T;
#define RTL_NUMBER_OF(x) (sizeof(x)/sizeof((x)[0]))
typedef uint16_t USHORT; typedef uint8_t UCHAR, BOOLEAN, *PUCHAR;
#define VOID void
#define TRUE 1
#define FALSE 0
#define MAXULONG UINT32_MAX
#define _In_
#define _Out_opt_
#define NT_ASSERT assert
#define NV_T210_LITE_WINARM_WRITE 1
#define SDHC_SPEC_VERSION_3 2
#define SDHC_MAX_CLOCK_DIVISOR 256
#define SDHC_MAX_CLOCK_DIVISOR_SPEC_3 2046
#define SDHC_VERSION 0xfe
#define SDHC_PRESENT_STATE 0x24
#define SDHC_CLOCK_CONTROL 0x2c
#define SDHC_HOST_CONTROL2 0x3e
#define SDHC_TIMEOUT_CONTROL 0x2e
#define SDHC_CC_INTERNAL_CLOCK_ENABLE 1
#define SDHC_CC_CLOCK_STABLE 2
#define SDHC_CC_CLOCK_ENABLE 4
#define SDHC_HC2_EXECUTE_TUNING 0x40
#define STATUS_DEVICE_BUSY -2
#define STATUS_IO_TIMEOUT -3
#define NT_SUCCESS(s) ((s)>=0)
#define KeStallExecutionProcessor(x) ((void)(x))
#define SDHC_HOST_CONTROL 0x28
#define SDHC_POWER_CONTROL 0x29
#define SDHC_PC_3_3V 14
#define SDHC_PC_BUS_POWER 1
#define SDHC_PC_VOLTAGE_MASK 14
#define STATUS_SUCCESS 0
#define STATUS_DEVICE_POWER_FAILURE -1
#define SdPortWait(us) ((void)(us))
typedef int NTSTATUS, SDPORT_BUS_VOLTAGE;
enum { SdBusVoltage33=1, SdBusVoltage30=2, SdBusVoltage18=3, SdBusVoltageOff=4 };
#define SDHC_HC_DMA_SELECT_MASK 0x18
#define SDHC_HC_DMA_SELECT_ADMA32 0x10
#define SDHC_ADMA_SYSADDR_LOW 0x58
#define SDHC_TRANSFER_MODE 0x0c
#define SDHC_TM_DMA_ENABLE 1
#define T214_DMA_BYTES 65536
#define RtlCopyMemory memcpy
#define KeMemoryBarrier() ((void)0)
#define MAX(a,b) ((a)>(b)?(a):(b))
#define min(a,b) ((a)<(b)?(a):(b))
enum { SdTransferTypeNone, SdTransferTypeSingleBlock, SdTransferTypeMultiBlock, SdTransferTypeMultiBlockNoStop };
enum { SdCommandClassStandard, SdCommandClassApp };
enum { SdTransferDirectionRead, SdTransferDirectionWrite };
enum { SdTransferMethodPio, SdTransferMethodSgDma };
enum { SdRequestTypeCommandNoTransfer, SdRequestTypeCommandWithTransfer, SdRequestTypeStartTransfer };
enum { SdResponseTypeR1 };
enum { LiteSdxc=1, LiteGpt=2, LitePartition=4, LiteNtfs=8, LiteMbr=16, LiteFatBoot=32,
       LiteWriteFacts=LiteSdxc|LiteMbr|LiteNtfs, LiteFatWriteFacts=LiteSdxc|LiteMbr|LiteFatBoot };
typedef struct { ULONG Index,Class,ResponseType,TransferDirection,TransferMethod,TransferType,Argument,Length;
 USHORT BlockSize,BlockCount; PUCHAR DataBuffer; } SDPORT_COMMAND,*PSDPORT_COMMAND;
typedef struct { ULONG Type; SDPORT_COMMAND Command; } SDPORT_REQUEST,*PSDPORT_REQUEST;
typedef struct { LONG TargetFacts; ULONG NextReadLba,IssuedWriteLba; USHORT IssuedWriteCount;
 UCHAR IssuedWriteIndex; BOOLEAN AppPrefixAccepted,CrashdumpMode;
 BOOLEAN T214CardOn,T214Voltage18,T214DmaActive,T214DmaComplete,T214DmaFault;
 PUCHAR T214Bounce,T214DmaBuffer; struct { ULONG LowPart; } T214BouncePa;
 ULONG T214DmaLength,T214DmaBlocks,T214DmaCapacity,T214DmaRequests,T214DmaMaxBlocks,T214PioFallbacks; struct { ULONG BaseClockFrequencyKhz; } Capabilities;
 ULONG T214SavedVendor[5],T214SavedEnables,T214SavedSignals;
 ULONG T214SavedSource,T214SavedBaseClock;void *OutstandingRequests[1];
 USHORT T214SavedClock,T214SavedControl2;
 UCHAR T214SavedHost,T214SavedPower,T214SavedTimeout;
 BOOLEAN T214ContextValid;ULONG T214Saves,T214Restores,T214ResumeStatus;
 PUCHAR BaseAddress,T214Car;UCHAR regs[512]; } SDHC_EXTENSION,*PSDHC_EXTENSION;
static UCHAR car_regs[4096];
static struct { ULONG BlocksRead,BlocksWritten; } NvDiag;
static LONG InterlockedExchange(LONG *p,LONG n){LONG o=*p;*p=n;return o;}
static LONG InterlockedOr(LONG *p,LONG n){return *p|=n;}
static LONG InterlockedAnd(LONG *p,LONG n){return *p&=n;}
static bool clock_fail;
static ULONG SdhcReadRegisterUlong(PSDHC_EXTENSION s,ULONG off){ULONG v;memcpy(&v,s->regs+off,4);return v;}
static ULONG T214Read(PUCHAR b,ULONG off){ULONG v;memcpy(&v,b+off,4);return v;}
static void T214Write(PUCHAR b,ULONG off,ULONG v){memcpy(b+off,&v,4);}
static NTSTATUS T214ParentClock(PSDHC_EXTENSION s){(void)s;return 0;}
static NTSTATUS T214ResumeHostClock(PSDHC_EXTENSION s){(void)s;return 0;}
static UCHAR SdhcReadRegisterUchar(PSDHC_EXTENSION s,ULONG off){return s->regs[off];}
static USHORT SdhcReadRegisterUshort(PSDHC_EXTENSION s,ULONG off){USHORT v;memcpy(&v,s->regs+off,2);return v;}
static void SdhcWriteRegisterUchar(PSDHC_EXTENSION s,ULONG off,UCHAR v){s->regs[off]=v;}
static void SdhcWriteRegisterUshort(PSDHC_EXTENSION s,ULONG off,USHORT v){if(off==SDHC_CLOCK_CONTROL&&(v&1)&&!clock_fail)v|=2;memcpy(s->regs+off,&v,2);}
static void SdhcWriteRegisterUlong(PSDHC_EXTENSION s,ULONG off,ULONG v){memcpy(s->regs+off,&v,4);}
'''
code = prefix + target + r'''
+static unsigned power_cycles;
+static NTSTATUS T214Power(PSDHC_EXTENSION s,BOOLEAN on){power_cycles++;s->T214CardOn=on;s->T214Voltage18=0;return 0;}
+'''.replace('\n+','\n')
code += 'static const ULONG T214ContextOffsets[]={0x100,0x1ac,0x1c0,0x1c4,0x1f0};\n'
code += function(board, 'T214SetBusVoltage', 'NTSTATUS')
code += function(board, 'T214SaveContext', 'void')
code += function(board, 'T214RestoreContext', 'void')
for name, result in [('LiteForgetCard','void'),('LiteObserveRead','void'),('LiteObserveReadRange','void'),('LiteBlocksInExtent','BOOLEAN'),('LiteAllowRequest','BOOLEAN')]:
    code += function(source, name, result)
for name, result in [('T214BuildDma','BOOLEAN'),('T214DmaDone','void')]:
    code += function(board, name, result)
code += function(source[source.index('SdhcCalcClockFrequency(\n'):], 'SdhcCalcClockFrequency','USHORT')
original = re.search(r'original\[70\]\s*=\s*\{([^}]+)', target).group(1)
code += '\nstatic const UCHAR physical_tail[70]={' + original + '};\n'
code += r'''
static void u32(UCHAR *p,ULONG n){memcpy(p,&n,4);}
static void u64(UCHAR *p,uint64_t n){memcpy(p,&n,8);}
int main(void){
 SDHC_EXTENSION s={0};SDPORT_REQUEST r={0};
 _Alignas(16) UCHAR bounce[69632],data[65536];
 memset(bounce,0xcc,sizeof(bounce)); memset(data,0xa5,sizeof(data));
 s.T214Bounce=bounce;s.T214BouncePa.LowPart=0x8a000000;s.T214DmaCapacity=65536;
 r.Type=SdRequestTypeCommandWithTransfer;r.Command.Index=17;r.Command.BlockSize=512;
 r.Command.BlockCount=1;r.Command.Length=512;r.Command.DataBuffer=data;
 r.Command.TransferMethod=SdTransferMethodPio;r.Command.TransferDirection=SdTransferDirectionRead;
 r.Command.TransferType=SdTransferTypeSingleBlock;s.NextReadLba=0;
 assert(T214BuildDma(&s,&r));
 memset(bounce,0,512);memcpy(bounce+440,physical_tail,70);bounce[510]=0x55;bounce[511]=0xaa;
 T214DmaDone(&s,&r);
 assert(s.TargetFacts&LiteMbr);assert(data[446]==0x80&&data[450]==7);
#ifdef FAT32_FIRST
 assert(data[466]==0x0c&&LiteU32(data+470)==FAT32_FIRST);
 for(unsigned i=478;i<510;i++)assert(data[i]==0);
#else
 for(unsigned i=462;i<510;i++)assert(data[i]==0);
#endif
 assert(bounce[450]==0x0c); /* Only the OS-visible buffer was filtered. */
 assert(s.T214DmaComplete&&!s.T214DmaActive);
 s.NextReadLba=WINARM_FIRST;r.Command.Argument=WINARM_FIRST;
 assert(T214BuildDma(&s,&r));memset(bounce,0,512);memcpy(bounce+3,"NTFS    ",8);
 bounce[12]=2;bounce[13]=8;bounce[510]=0x55;bounce[511]=0xaa;
 u32(bounce+28,WINARM_FIRST);u64(bounce+40,WINARM_COUNT-1);u64(bounce+72,WINARM_SERIAL);
 T214DmaDone(&s,&r);s.TargetFacts|=LiteSdxc;
 r.Command.Index=25;r.Command.BlockCount=128;r.Command.Length=65536;
 r.Command.TransferDirection=SdTransferDirectionWrite;r.Command.TransferType=SdTransferTypeMultiBlock;
 assert(LiteAllowRequest(&s,&r));
 s.CrashdumpMode=1;assert(!LiteAllowRequest(&s,&r));s.CrashdumpMode=0;
 assert(T214BuildDma(&s,&r));
 assert(!memcmp(data,bounce,65536));
 ULONG *desc=(ULONG*)(bounce+65536);
 assert(desc[0]==0x80000021&&desc[1]==0x8a000000);
 assert(desc[2]==0x80000023&&desc[3]==0x8a008000);
 r.Command.Argument=WINARM_FIRST+WINARM_COUNT-127;assert(!LiteAllowRequest(&s,&r));
 r.Command.Argument=0;assert(!LiteAllowRequest(&s,&r));
 r.Command.Argument=UINT32_MAX-10;assert(!LiteAllowRequest(&s,&r));
 r.Command.Argument=WINARM_FIRST;s.TargetFacts=0;assert(!LiteAllowRequest(&s,&r));
 s.T214DmaFault=1;assert(!T214BuildDma(&s,&r));s.T214DmaFault=0;
 r.Command.BlockCount=129;assert(!T214BuildDma(&s,&r));r.Command.BlockCount=1;
 r.Command.DataBuffer=NULL;assert(!T214BuildDma(&s,&r));r.Command.DataBuffer=data;
 s.NextReadLba=WINARM_FIRST;memset(bounce,0,512);T214DmaDone(&s,&r);assert(!(s.TargetFacts&LiteNtfs));
 s.Capabilities.BaseClockFrequencyKhz=199680;s.regs[SDHC_VERSION]=SDHC_SPEC_VERSION_3;
 s.T214CardOn=1;s.T214Voltage18=0;s.regs[SDHC_POWER_CONTROL]=0;
 assert(T214SetBusVoltage(&s,SdBusVoltage33)==0);
 assert(s.regs[SDHC_POWER_CONTROL]==15&&power_cycles==0);
 s.T214Voltage18=1;assert(T214SetBusVoltage(&s,SdBusVoltage33)==0&&power_cycles==1);
 assert(!s.T214Voltage18&&s.T214CardOn);
 assert(T214SetBusVoltage(&s,SdBusVoltageOff)==0&&power_cycles==2&&!s.T214CardOn);
 ULONG actual;USHORT reg=SdhcCalcClockFrequency(&s,200000,&actual);
 assert(reg==0&&actual==199680);
 for(ULONG target=400;target<200000;target+=137){SdhcCalcClockFrequency(&s,target,&actual);assert(actual<=target);}
 SdhcCalcClockFrequency(&s,25000,&actual);assert(actual==24960);
 /* Range filtering must match the previous per-sector path at every guard
    boundary, including a boot sector in the middle of a multi-block read. */
 ULONG edges[]={0,1,4,WINARM_FIRST,
#ifdef FAT32_FIRST
 FAT32_FIRST,
#endif
 };
 for(unsigned e=0;e<RTL_NUMBER_OF(edges);e++)for(unsigned count=1;count<=128;count++){
  ULONG first=edges[e]>2?edges[e]-2:0;
  UCHAR a[65536],b[65536];memset(a,0x5a,sizeof(a));
  for(unsigned j=0;j<count;j++){
   ULONG lba=first+j;UCHAR *q=a+j*512;
   if(lba==0){memset(q,0,512);memcpy(q+440,physical_tail,70);q[510]=0x55;q[511]=0xaa;}
   if(lba==WINARM_FIRST){memset(q,0,512);memcpy(q+3,"NTFS    ",8);q[12]=2;q[13]=8;q[510]=0x55;q[511]=0xaa;u32(q+28,WINARM_FIRST);u64(q+40,WINARM_COUNT-1);u64(q+72,WINARM_SERIAL);}
#ifdef FAT32_FIRST
   if(lba==FAT32_FIRST){memset(q,0,512);q[12]=2;q[13]=64;q[510]=0x55;q[511]=0xaa;u32(q+28,FAT32_FIRST);u32(q+32,FAT32_COUNT);u32(q+36,128);memcpy(q+82,"FAT32   ",8);}
#endif
  }
  memcpy(b,a,sizeof(a));SDHC_EXTENSION old={0},bulk={0};old.NextReadLba=bulk.NextReadLba=first;
  SDPORT_COMMAND one={0};one.BlockSize=512;
  for(unsigned j=0;j<count;j++){one.DataBuffer=a+j*512;LiteObserveRead(&old,&one);}
  one.DataBuffer=b;LiteObserveReadRange(&bulk,&one,count);
  assert(old.TargetFacts==bulk.TargetFacts&&old.NextReadLba==bulk.NextReadLba&&!memcmp(a,b,count*512));
 }
 /* Normal I/O can span sixteen ADMA descriptors; dump uses 16 KiB separately. */
 static _Alignas(16) UCHAR large_bounce[528384],large_data[524288];
 memset(&s,0,sizeof(s));memset(&r,0,sizeof(r));
 s.T214Bounce=large_bounce;s.T214BouncePa.LowPart=0x8a000000;s.T214DmaCapacity=524288;
 r.Command.DataBuffer=large_data;r.Command.Length=524288;r.Command.BlockCount=1024;
 r.Command.BlockSize=512;r.Command.Index=18;r.Command.TransferMethod=SdTransferMethodPio;
 r.Command.TransferDirection=SdTransferDirectionRead;s.NextReadLba=1000;
 memset(large_bounce,0xa5,524288);assert(T214BuildDma(&s,&r));
 PULONG table=(PULONG)(large_bounce+524288);
 for(unsigned i=0;i<16;i++){assert(table[i*2]==(0x80000021u|(i==15?2:0)));assert(table[i*2+1]==0x8a000000+i*32768);}
 T214DmaDone(&s,&r);assert(!memcmp(large_bounce,large_data,524288));
 assert(s.NextReadLba==2024&&s.T214DmaMaxBlocks==1024&&s.T214DmaRequests==1);
 r.Command.BlockCount=1025;assert(!T214BuildDma(&s,&r)&&s.T214PioFallbacks==1);
 /* Controller retention loss restores only configuration, never old I/O. */
 memset(&s,0,sizeof(s));s.BaseAddress=s.regs;s.T214Car=car_regs;
 s.regs[0x28]=0x12;s.regs[0x29]=0xb;s.regs[0x2e]=0xe;
 SdhcWriteRegisterUshort(&s,0x2c,7);SdhcWriteRegisterUshort(&s,0x3e,0x8b);
 SdhcWriteRegisterUlong(&s,0x34,0x12345678);SdhcWriteRegisterUlong(&s,0x38,0x87654321);
 for(unsigned i=0;i<5;i++)T214Write(s.regs,T214ContextOffsets[i],0x55+i);
 T214SaveContext(&s);assert(s.T214ContextValid&&s.T214Saves==1);
 memset(s.regs,0,sizeof(s.regs));T214RestoreContext(&s);
 assert(s.T214Restores==1&&s.T214ResumeStatus==0&&s.regs[0x28]==0x12&&s.regs[0x29]==0xb);
 assert(SdhcReadRegisterUshort(&s,0x2c)==7&&SdhcReadRegisterUshort(&s,0x3e)==0x8b);
 assert(SdhcReadRegisterUlong(&s,0x34)==0x12345678&&SdhcReadRegisterUlong(&s,0x38)==0x87654321);
 for(unsigned i=0;i<5;i++)assert(T214Read(s.regs,T214ContextOffsets[i])==0x55+i);
 assert(SdhcReadRegisterUlong(&s,0x08)==0&&SdhcReadRegisterUlong(&s,0x0c)==0&&SdhcReadRegisterUlong(&s,0x20)==0&&SdhcReadRegisterUlong(&s,0x30)==0);
 clock_fail=true;T214RestoreContext(&s);assert(s.T214DmaFault&&s.T214ResumeStatus==(ULONG)STATUS_IO_TIMEOUT&&T214Read(s.regs,0x38)==0);clock_fail=false;
 s.T214DmaActive=true;T214SaveContext(&s);assert(!s.T214ContextValid&&s.T214Saves==1);
 puts("PASS: real DMA/filter/write bounds, descriptor limits, error gate, 199.68 MHz divisor, reset-all power restoration");
}
'''
with tempfile.TemporaryDirectory(prefix='t214-sd-test-') as d:
    path = Path(d); (path/'test.c').write_text(code)
    subprocess.run(['clang','-std=c11','-Wall','-Werror','-Wno-unused-function','-fsanitize=address,undefined',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
