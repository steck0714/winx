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
#include <string.h>
#include <stdio.h>
typedef uint32_t ULONG, *PULONG; typedef int32_t LONG;
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
#define SDHC_HOST_CONTROL 0x28
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
 BOOLEAN T214DmaActive,T214DmaComplete,T214DmaFault;
 PUCHAR T214Bounce,T214DmaBuffer; struct { ULONG LowPart; } T214BouncePa;
 ULONG T214DmaLength,T214DmaBlocks; struct { ULONG BaseClockFrequencyKhz; } Capabilities;
 UCHAR regs[512]; } SDHC_EXTENSION,*PSDHC_EXTENSION;
static struct { ULONG BlocksRead,BlocksWritten; } NvDiag;
static LONG InterlockedExchange(LONG *p,LONG n){LONG o=*p;*p=n;return o;}
static LONG InterlockedOr(LONG *p,LONG n){return *p|=n;}
static LONG InterlockedAnd(LONG *p,LONG n){return *p&=n;}
static UCHAR SdhcReadRegisterUchar(PSDHC_EXTENSION s,ULONG off){return s->regs[off];}
static USHORT SdhcReadRegisterUshort(PSDHC_EXTENSION s,ULONG off){USHORT v;memcpy(&v,s->regs+off,2);return v;}
static void SdhcWriteRegisterUchar(PSDHC_EXTENSION s,ULONG off,UCHAR v){s->regs[off]=v;}
static void SdhcWriteRegisterUshort(PSDHC_EXTENSION s,ULONG off,USHORT v){memcpy(s->regs+off,&v,2);}
static void SdhcWriteRegisterUlong(PSDHC_EXTENSION s,ULONG off,ULONG v){memcpy(s->regs+off,&v,4);}
'''
code = prefix + target
for name, result in [('LiteForgetCard','void'),('LiteObserveRead','void'),('LiteBlocksInExtent','BOOLEAN'),('LiteAllowRequest','BOOLEAN')]:
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
 s.T214Bounce=bounce;s.T214BouncePa.LowPart=0x8a000000;
 r.Type=SdRequestTypeCommandWithTransfer;r.Command.Index=17;r.Command.BlockSize=512;
 r.Command.BlockCount=1;r.Command.Length=512;r.Command.DataBuffer=data;
 r.Command.TransferMethod=SdTransferMethodPio;r.Command.TransferDirection=SdTransferDirectionRead;
 r.Command.TransferType=SdTransferTypeSingleBlock;s.NextReadLba=0;
 assert(T214BuildDma(&s,&r));
 memset(bounce,0,512);memcpy(bounce+440,physical_tail,70);bounce[510]=0x55;bounce[511]=0xaa;
 T214DmaDone(&s,&r);
 assert(s.TargetFacts&LiteMbr);assert(data[446]==0x80&&data[450]==7);
 for(unsigned i=462;i<510;i++)assert(data[i]==0);
 assert(bounce[450]==0x0c); /* Only the OS-visible buffer was filtered. */
 assert(s.T214DmaComplete&&!s.T214DmaActive);
 s.NextReadLba=WINARM_FIRST;r.Command.Argument=WINARM_FIRST;
 assert(T214BuildDma(&s,&r));memset(bounce,0,512);memcpy(bounce+3,"NTFS    ",8);
 bounce[12]=2;bounce[13]=8;bounce[510]=0x55;bounce[511]=0xaa;
 u32(bounce+28,WINARM_FIRST);u64(bounce+40,WINARM_COUNT-1);u64(bounce+72,WINARM_SERIAL);
 T214DmaDone(&s,&r);s.TargetFacts|=LiteSdxc;
 r.Command.Index=25;r.Command.BlockCount=128;r.Command.Length=65536;
 r.Command.TransferDirection=SdTransferDirectionWrite;r.Command.TransferType=SdTransferTypeMultiBlock;
 assert(LiteAllowRequest(&s,&r));assert(T214BuildDma(&s,&r));
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
 ULONG actual;USHORT reg=SdhcCalcClockFrequency(&s,200000,&actual);
 assert(reg==0&&actual==199680);
 for(ULONG target=400;target<200000;target+=137){SdhcCalcClockFrequency(&s,target,&actual);assert(actual<=target);}
 SdhcCalcClockFrequency(&s,25000,&actual);assert(actual==24960);
 puts("PASS: real DMA/filter/write bounds, descriptor limits, error gate, 199.68 MHz divisor");
}
'''
with tempfile.TemporaryDirectory(prefix='t214-sd-test-') as d:
    path = Path(d); (path/'test.c').write_text(code)
    subprocess.run(['clang','-std=c11','-Wall','-Werror','-Wno-unused-function','-fsanitize=address,undefined',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
