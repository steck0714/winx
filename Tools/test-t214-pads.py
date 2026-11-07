#!/usr/bin/env python3
"""Exercise the applied SC7 GPIO/pinmux callbacks with context loss and locks."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(sys.argv[1]).resolve()
source = (root / 'plat/nvidia/tegra/soc/t210/t214/pads.c').read_text()
source = source[source.index('#define PAD_COUNT'):]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PSCI_E_SUCCESS 0
#define PSCI_E_DENIED -3
#define PSCI_E_INTERN_FAIL -6
static uint32_t mmio_read_32(uintptr_t address);
static void mmio_write_32(uintptr_t address,uint32_t value);
static void dsbsy(void){}
static void flush_dcache_range(uintptr_t address,size_t size);
'''
suffix = r'''
static uint32_t gpio[0x1000/4],misc[0x4000/4],old_gpio[0x1000/4],old_misc[0x4000/4];
static unsigned writes,flushes;
static uintptr_t stuck;
static uint32_t field_mask(unsigned offset){
 for(unsigned i=0;i<PAD_COUNT(pad_regs);i++)if(pad_regs[i].offset==offset)return pad_regs[i].mask;
 for(unsigned i=0;i<PAD_COUNT(mux_regs);i++)if(mux_regs[i].offset==offset)return mux_regs[i].mask;
 assert(!"unowned pinmux register");return 0;
}
static uint32_t *reg(uintptr_t address){
 assert(!(address&3));
 if(address>=LITE_GPIO_BASE&&address<LITE_GPIO_BASE+sizeof(gpio)){
  unsigned o=(unsigned)(address-LITE_GPIO_BASE);
  /* Forbid IRQ enable, level, status and acknowledgement even on reads. */
  assert(o<0x800&&(o&0xff)<0x30);
  return &gpio[o/4];
 }
 assert(address>=LITE_MISC_BASE&&address<LITE_MISC_BASE+sizeof(misc));
 unsigned o=(unsigned)(address-LITE_MISC_BASE);(void)field_mask(o);
 return &misc[o/4];
}
static uint32_t mmio_read_32(uintptr_t address){return *reg(address);}
static void mmio_write_32(uintptr_t address,uint32_t value){
 uint32_t *p=reg(address),previous=*p;writes++;
 if(address>=LITE_MISC_BASE){
  unsigned o=(unsigned)(address-LITE_MISC_BASE);uint32_t mask=field_mask(o);
  assert((previous&~mask)==(value&~mask));
  for(unsigned i=0;i<32;i++){
   unsigned g=(unsigned)(gpio_port(i)-LITE_GPIO_BASE)/4;
   assert((gpio[g+8]&255)==pad_context.gpio_out[i]);
   assert((gpio[g]&255)==pad_context.gpio_cnf[i]);
   assert((gpio[g+4]&255)==pad_context.gpio_oe[i]);
  }
  if(o>=0x3000&&(previous&0x80))return; /* LOCK is sticky until context loss. */
 }else{
  unsigned o=(unsigned)(address-LITE_GPIO_BASE);
  assert((previous&~255U)==(value&~255U));
  if((o&0xff)<0x20)for(unsigned i=0;i<32;i++)
   assert((gpio[(gpio_port(i)-LITE_GPIO_BASE)/4+8]&255)==pad_context.gpio_out[i]);
 }
 if(address!=stuck)*p=value;
}
static void flush_dcache_range(uintptr_t address,size_t size){
 assert(address==(uintptr_t)&pad_context&&size==sizeof(pad_context)&&pad_context.valid);flushes++;
}
static void init(void){
 memset(&pad_context,0,sizeof(pad_context));memset(gpio,0,sizeof(gpio));
 memset(misc,0,sizeof(misc));writes=flushes=0;stuck=0;
 assert(PAD_COUNT(pad_regs)==153&&PAD_COUNT(mux_regs)==175);
 assert(field_mask(0x3000)==0x7bff&&field_mask(0xa98)==0xf7f7f000);
 assert(field_mask(0xa9c)==0xf7fffffc&&field_mask(0xac0)==0x00600000);
 for(unsigned i=0;i<32;i++){
  unsigned p=(unsigned)(gpio_port(i)-LITE_GPIO_BASE)/4;
  gpio[p]=0xa500|((i^0xa5)&255);gpio[p+4]=0x5a00|((i^0x5a)&255);
  gpio[p+8]=0xab00|((i^255)&255);
 }
 for(unsigned i=0;i<PAD_COUNT(pad_regs);i++)misc[pad_regs[i].offset/4]=0xabcd1234^i;
 for(unsigned i=0;i<PAD_COUNT(mux_regs);i++)misc[mux_regs[i].offset/4]=0x12345678^i;
 memcpy(old_gpio,gpio,sizeof(gpio));memcpy(old_misc,misc,sizeof(misc));
}
static void lose(void){
 /* Leave arbitrary status/reserved bits that the restore must preserve. */
 for(unsigned i=0;i<sizeof(gpio)/sizeof(gpio[0]);i++)gpio[i]=0xcd000000;
 for(unsigned i=0;i<sizeof(misc)/sizeof(misc[0]);i++)misc[i]=0x80000000;
 writes=0;
}
static void check(void){
 for(unsigned i=0;i<32;i++){
  unsigned p=(unsigned)(gpio_port(i)-LITE_GPIO_BASE)/4;
  for(unsigned r=0;r<3;r++)assert(gpio[p+4*r]==(0xcd000000|(old_gpio[p+4*r]&255)));
 }
 for(unsigned i=0;i<PAD_COUNT(pad_regs);i++){
  unsigned o=pad_regs[i].offset;uint32_t mask=pad_regs[i].mask,value=old_misc[o/4];
  if(o==0xa9c||o==0xab4)value&=~EMMC_PARKING_MASK;
  assert(misc[o/4]==((0x80000000&~mask)|(value&mask)));
 }
 for(unsigned i=0;i<PAD_COUNT(mux_regs);i++){
  unsigned o=mux_regs[i].offset;uint32_t mask=mux_regs[i].mask;
  assert(misc[o/4]==((0x80000000&~mask)|(old_misc[o/4]&mask)));
 }
 /* Pending GPIO IRQs and pinmux holes retain their post-wake values. */
 assert(gpio[0x50/4]==0xcd000000&&gpio[0x70/4]==0xcd000000);
 assert(misc[0x3018/4]==0x80000000&&misc[0x8dc/4]==0x80000000);
 assert(!pad_context.valid);
}
int main(void){
 init();assert(tegra_lite_pads_restore()==PSCI_E_DENIED&&!writes);
 for(unsigned cycle=0;cycle<4;cycle++){
  tegra_lite_pads_save();assert(flushes==cycle+1);lose();
  assert(tegra_lite_pads_restore()==0);check();
  unsigned w=writes;assert(tegra_lite_pads_restore()==PSCI_E_DENIED&&writes==w);
 }
 /* On retained entry, unchanged locked pins do not get rewritten. */
 init();misc[0xa9c/4]&=~EMMC_PARKING_MASK;misc[0xab4/4]&=~EMMC_PARKING_MASK;
 tegra_lite_pads_save();assert(tegra_lite_pads_restore()==0&&!writes);
 const uintptr_t failures[]={LITE_GPIO_BASE+0x20,LITE_GPIO_BASE,LITE_MISC_BASE+0xa98};
 for(unsigned i=0;i<sizeof(failures)/sizeof(failures[0]);i++){
  init();tegra_lite_pads_save();lose();stuck=failures[i];
  assert(tegra_lite_pads_restore()==PSCI_E_INTERN_FAIL&&!pad_context.valid);
 }
 init();tegra_lite_pads_save();lose();misc[0x3000/4]=0x80; /* locked, wrong mux */
 assert(tegra_lite_pads_restore()==PSCI_E_INTERN_FAIL&&!pad_context.valid);
 puts("PASS: pinmux/GPIO repeated loss, output-before-mux, owned-field masks, locked-pin failure and untouched IRQ status");
}
'''
with tempfile.TemporaryDirectory(prefix='lite-sc7-pads-') as temporary:
    p = Path(temporary)
    (p / 'test.c').write_text(prefix + source + suffix)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
