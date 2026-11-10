#!/usr/bin/env python3
"""Exercise real capability decoding, sampling-window limits and metadata input.

These are host tests, not acceptance of physical DDR200 transfers or fallback.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/nvtsdhc.c').read_text()
start = re.search(r'static VOID T214ObserveCardInfo\(', source).start()
brace = source.index('{', start)
end, depth = brace + 1, 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
observe = source[start:end]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define VOID void
#define RtlCopyMemory memcpy
typedef struct { unsigned Index,Class,TransferDirection,TransferType;
 unsigned BlockCount,BlockSize,Length; unsigned char *DataBuffer; } COMMAND,*PSDPORT_COMMAND;
typedef struct { unsigned Type; COMMAND Command; } REQUEST,*PSDPORT_REQUEST;
typedef struct { unsigned char T214Scr[8],T214SwitchStatus[64]; unsigned T214CardInfoValid; } SLOT,*PSDHC_EXTENSION;
enum { SdRequestTypeCommandWithTransfer=1,SdRequestTypeStartTransfer,
 SdTransferDirectionRead,SdTransferDirectionWrite,SdTransferTypeSingleBlock,
 SdTransferTypeMultiBlock,SdCommandClassApp,SdCommandClassStandard };
'''
code += '#include "' + str(root / 'src/t214_ddr200_policy.h') + '"\n' + observe
code += r'''
int main(void) {
 unsigned char scr[8]={2,4,0x80,0x80},status[64]={0};
 status[10]=0x40;status[13]=8;
 assert(T214Ddr200Supported(scr,status));
 for(unsigned x=0;x<16;x++){
  scr[2]=0x80|(x>>2);scr[3]=(unsigned char)(x<<6);
  assert(!!T214Ddr200Supported(scr,status)==(x>=2));
 }
 scr[2]=0x80;scr[3]=0x80;
 scr[0]=0x12;assert(!T214Ddr200Supported(scr,status));scr[0]=2;
 scr[1]=1;assert(!T214Ddr200Supported(scr,status));scr[1]=4;
 scr[2]=0;assert(!T214Ddr200Supported(scr,status));scr[2]=0x80;
 status[10]=0x20;assert(!T214Ddr200Supported(scr,status));status[10]=0x40;
 status[13]=4;assert(!T214Ddr200Supported(scr,status));status[13]=8;
 unsigned bits[8]={0},tap,width;
 assert(!T214Ddr200Window(bits,128,&tap,&width)&&width==0);
 /* Seven good taps ending just before a failing final tap are still seven. */
 for(unsigned i=120;i<127;i++)bits[i/32]|=1u<<(i%32);
 assert(!T214Ddr200Window(bits,128,&tap,&width)&&width==7);
 bits[3]|=1u<<31;
 assert(T214Ddr200Window(bits,128,&tap,&width)&&width==8&&tap==123);
 memset(bits,0,sizeof(bits));bits[0]=7;bits[3]=0xf8000000u;
 assert(!T214Ddr200Window(bits,128,&tap,&width));
 memset(bits,0xff,sizeof(bits));
 assert(T214Ddr200Window(bits,128,&tap,&width)&&width==128&&tap==63);
 assert(T214Ddr200Window(bits,256,&tap,&width)&&width==256&&tap==127);
 assert(!T214Ddr200Window(bits,257,&tap,&width));
 assert(T214Ddr200CarDivider(200000)==3);
 assert(T214Ddr200CarDivider(199680)==3);
 assert(T214Ddr200CarDivider(25000)==38);
 assert(T214Ddr200CarDivider(0)==~0u&&T214Ddr200CarDivider(200001)==~0u);
 for(unsigned f=1;f<=200000;f++){
  unsigned d=T214Ddr200CarDivider(f);
  if(d!=~0u)assert(d>=3&&d<=255&&998400/(d+2)<=f);
 }
 SLOT slot={0};REQUEST r={0};
 r.Type=SdRequestTypeStartTransfer;r.Command.Index=51;r.Command.Class=SdCommandClassApp;
 r.Command.TransferDirection=SdTransferDirectionRead;r.Command.TransferType=SdTransferTypeSingleBlock;
 r.Command.BlockSize=r.Command.Length=8;r.Command.DataBuffer=scr;
 r.Command.BlockCount=1;T214ObserveCardInfo(&slot,&r);assert(!slot.T214CardInfoValid);
 r.Command.BlockCount=0;r.Type=SdRequestTypeCommandWithTransfer;
 T214ObserveCardInfo(&slot,&r);assert(!slot.T214CardInfoValid);
 r.Type=SdRequestTypeStartTransfer;r.Command.Class=SdCommandClassStandard;
 T214ObserveCardInfo(&slot,&r);assert(!slot.T214CardInfoValid);
 r.Command.Class=SdCommandClassApp;T214ObserveCardInfo(&slot,&r);
 assert(slot.T214CardInfoValid==1&&!memcmp(slot.T214Scr,scr,8));
 r.Command.Index=6;r.Command.Class=SdCommandClassStandard;
 r.Command.BlockSize=r.Command.Length=64;r.Command.DataBuffer=status;
 r.Command.TransferDirection=SdTransferDirectionWrite;
 T214ObserveCardInfo(&slot,&r);assert(slot.T214CardInfoValid==1);
 r.Command.TransferDirection=SdTransferDirectionRead;T214ObserveCardInfo(&slot,&r);
 assert(slot.T214CardInfoValid==3&&!memcmp(slot.T214SwitchStatus,status,64));
 assert(T214Ddr200Supported(slot.T214Scr,slot.T214SwitchStatus));
 puts("PASS: SD6/vendor detection, successful metadata reads, eight-tap boundary, nonwrapping windows, DDR CAR divider limits");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='lite-ddr200-') as temp:
    path = Path(temp)
    (path / 'test.c').write_text(code)
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
