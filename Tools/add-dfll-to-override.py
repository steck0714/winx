#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Add the reviewed FAN0 DFLL resources to a disassembled Windows ACPI override.
Only FAN0's raw _CRS buffer changes. Compile the output with iasl afterwards.
The UEFI table alone cannot add resources when Windows overrides that table.
"""
import argparse,re,struct
from pathlib import Path
RESOURCES=((0x70110000,0x300,1),(0x6000662c,8,1),(0x600062f4,4,1),
           (0x6000635c,12,0),(0x60006438,8,1),(0x60006448,8,1),(0x60006128,4,0))
def patch(source):
 pattern=r'(Device \(FAN0\).*?Name \(_CRS, Buffer \()(?P<size>0x[0-9A-Fa-f]+)(\).*?\{)(?P<data>.*?)(\}\))'
 found=list(re.finditer(pattern,source,re.S))
 if len(found)!=1:raise ValueError('Expected one FAN0 raw _CRS buffer')
 m=found[0]
 if '"SWOA0402"' not in m.group(1):raise ValueError('FAN0 hardware identity differs')
 text=re.sub(r'/\*.*?\*/|//[^\n]*','',m.group('data'),flags=re.S)
 data=bytes(int(v,16) for v in re.findall(r'0x([0-9a-fA-F]+)',text))
 if len(data)!=int(m.group('size'),16) or not data.endswith(b'\x79\x00'):raise ValueError('Resource length/end tag differs')
 memory=[];i=0
 while i<len(data)-2:
  tag=data[i];length=int.from_bytes(data[i+1:i+3],'little') if tag&0x80 else tag&7
  header=3 if tag&0x80 else 1
  if i+header+length>len(data)-2:raise ValueError('Truncated resource')
  if tag==0x86:
   if length!=9:raise ValueError('Unexpected memory descriptor')
   memory.append(struct.unpack('<BII',data[i+3:i+12]))
  i+=header+length
 if i!=len(data)-2 or (0,0xa9fb0000,0x1000) not in memory:raise ValueError('Original performance handoff missing')
 for address,length,rw in RESOURCES:
  if any(a<address+length and address<a+n for _,a,n in memory):raise ValueError('DFLL resource already exists or overlaps')
 descriptors=b''.join(b'\x86\x09\x00'+struct.pack('<BII',rw,address,length) for address,length,rw in RESOURCES)
 data=data[:-2]+descriptors+data[-2:]
 lines=['                '+', '.join(f'0x{x:02X}' for x in data[i:i+12])+(',' if i+12<len(data) else '') for i in range(0,len(data),12)]
 replacement=m.group(1)+f'0x{len(data):X}'+m.group(3)+'\n'+'\n'.join(lines)+'\n            '+m.group(5)
 return source[:m.start()]+replacement+source[m.end():]
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('input',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
 a.output.write_text(patch(a.input.read_text()))
