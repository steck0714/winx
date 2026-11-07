#!/usr/bin/env python3
"""Exercise the applied T214 CAR snapshot/restore code under register loss."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(sys.argv[1]).resolve()
source = (root / 'plat/nvidia/tegra/soc/t210/t214/clocks.c').read_text()
source = source[source.index('#define CB'):]
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define TEGRA_CAR_RESET_BASE 0x60006000U
#define PSCI_E_SUCCESS 0
#define PSCI_E_DENIED -3
#define PSCI_E_INTERN_FAIL -6
static uint32_t mmio_read_32(uintptr_t address);
static void mmio_write_32(uintptr_t address,uint32_t value);
static void udelay(unsigned int delay);
static void dsbsy(void);
static void flush_dcache_range(uintptr_t address,size_t size);
'''
suffix = r'''
static uint32_t regs[CAR_SIZE/4],original[CAR_SIZE/4];
static unsigned writes,bank_writes,flushes,elapsed,reads;
static unsigned failing;
static bool restoring;
static unsigned index_of(uintptr_t address){
 assert(address>=TEGRA_CAR_RESET_BASE&&address<TEGRA_CAR_RESET_BASE+CAR_SIZE&&!(address&3));
 return (unsigned)(address-TEGRA_CAR_RESET_BASE)/4;
}
static uint32_t mmio_read_32(uintptr_t address){
 unsigned n=index_of(address);reads++;assert(reads<100000);return regs[n];
}
static void locks(void){
 for(unsigned i=0;i<COUNT(plls);i++){
  const struct pll_desc *p=&plls[i];
  regs[p->lock_reg/4]&=~p->lock_mask;
  if((regs[p->base/4]&PLL_ON)&&!(regs[p->iddq_reg/4]&p->iddq_mask)&&
     (!p->reset_reg||!(regs[p->reset_reg/4]&p->reset_mask))&&failing!=p->base)
    regs[p->lock_reg/4]|=p->lock_mask;
 }
 regs[0x0c0/4]&=~PLL_LOCK;
 if((regs[0x0c0/4]&PLL_ON)&&!(regs[0x0cc/4]&CB(31))&&failing!=0x0c0)
  regs[0x0c0/4]|=PLL_LOCK;
 regs[0x0ec/4]&=~CB(11);
 if((regs[0x0e8/4]&CB(31))&&!(regs[0x0ec/4]&CB(13))&&failing!=0x0e8)
  regs[0x0ec/4]|=CB(11);
 if((regs[0x484/4]&CB(15))&&!(regs[0x484/4]&CB(14))&&failing!=0x52c)
  regs[0x52c/4]|=CB(31);
}
static void mmio_write_32(uintptr_t address,uint32_t value){
 unsigned n=index_of(address),off=n*4;writes++;
 /* Warmboot-owned DRAM/boot PLLs are outside this port's write authority. */
 assert(off!=0x090&&off!=0x098&&off!=0x09c&&off!=0x0a0&&off!=0x0ac);
 assert(off!=0x19c&&off!=0x5e8&&off!=0x5ec);
 if(off==0x52c)value=(value&~CB(31))|(regs[n]&CB(31));
 for(unsigned i=0;i<COUNT(plls);i++){
  const struct pll_desc *p=&plls[i];
  if(off==p->base&&(value&PLL_ON)&&p->base==0x4c4&&(value&CB(22)))
   assert(regs[0x52c/4]&CB(31));
  if(off==p->base&&(value&PLL_ON)){
   unsigned vreg=off==0x0b0?0x0b4:(off==0x5a4?0x5e4:(off==0x4c4?0x4cc:0));
   if(vreg)assert((regs[vreg/4]&0xf0000000U)==(saved(vreg)&0xf0000000U));
  }
 }
 for(unsigned i=0;i<COUNT(banks);i++)if(off==banks[i].enable||off==banks[i].reset){
  bank_writes++;
  assert(regs[0x0a0/4]&PLL_LOCK);
  for(unsigned j=0;j<COUNT(plls);j++)if(saved(plls[j].base)&PLL_ON)
   assert(regs[plls[j].lock_reg/4]&plls[j].lock_mask);
  assert(regs[0x0c0/4]&PLL_LOCK);assert(regs[0x0ec/4]&CB(11));
 }
 regs[n]=value;locks();
}
static void udelay(unsigned int delay){assert(delay<=20);elapsed+=delay;assert(elapsed<30000);}
static void dsbsy(void){}
static void flush_dcache_range(uintptr_t address,size_t size){
 assert(address==(uintptr_t)&context&&size==sizeof(context)&&context.valid);flushes++;
}
static void init(void){
 memset(regs,0,sizeof(regs));memset(&context,0,sizeof(context));
 writes=bank_writes=flushes=elapsed=reads=0;failing=0;restoring=false;
 for(unsigned i=0;i<COUNT(source_ranges);i++)
  for(unsigned o=source_ranges[i][0];o<=source_ranges[i][1];o+=4)regs[o/4]=0x20000000|o;
 for(unsigned i=0;i<COUNT(scalars);i++)regs[scalars[i]/4]=0x10000000|scalars[i];
 for(unsigned i=0;i<COUNT(banks);i++){
  regs[banks[i].enable/4]=banks[i].valid&0xa55aa55a;regs[banks[i].reset/4]=0x1234+i;
 }
 for(unsigned i=0;i<COUNT(plls);i++){
  const struct pll_desc *p=&plls[i];regs[p->base/4]=PLL_ON|0x6802;
  for(unsigned j=0;j<COUNT(p->misc);j++)if(p->misc[j].offset)
   regs[p->misc[j].offset/4]=0x00040000&p->misc[j].mask;
  regs[p->iddq_reg/4]&=~p->iddq_mask;
  if(p->reset_reg)regs[p->reset_reg/4]&=~p->reset_mask;
 }
 regs[0x4c4/4]|=CB(22); /* Exercise the UTMIPLL -> PLLRE dependency. */
 regs[0x0a0/4]=PLL_ON|PLL_LOCK|0x6804;regs[0x090/4]=0x48112802;
 regs[0x19c/4]=0x4111;regs[0x050/4]=0x50000002;
 regs[0x0c0/4]=PLL_ON|0x11902;regs[0x0cc/4]=CB(29);
 regs[0x0c8/4]=0;regs[0x530/4]=CB(24)|CB(7)|CB(6);
 regs[0x52c/4]=CB(31)|CB(24)|CB(6);regs[0x480/4]=0x190101;
 regs[0x484/4]=0x10080;regs[0x488/4]=0x2180000;
 regs[0x0e8/4]=CB(31)|0x0e007d02;regs[0x0ec/4]=CB(9)|CB(8);
 regs[0x48c/4]=CB(31)|CB(24)|CB(3);regs[0x068/4]=0x20010025;
 locks();memcpy(original,regs,sizeof(regs));
}
static void lose(void){
 uint32_t pll_m=regs[0x090/4],pll_p=regs[0x0a0/4],emc=regs[0x19c/4];
 memset(regs,0,sizeof(regs));regs[0x090/4]=pll_m;regs[0x0a0/4]=pll_p;regs[0x19c/4]=emc;
 regs[0x050/4]=CB(11); /* Preserve an oscillator bit outside Linux's mask. */
 writes=bank_writes=elapsed=reads=0;restoring=true;
}
static void check(void){
 for(unsigned i=0;i<COUNT(plls);i++){
  const struct pll_desc *p=&plls[i];
  assert(regs[p->base/4]==original[p->base/4]);
  for(unsigned j=0;j<COUNT(p->misc);j++)if(p->misc[j].offset){
   unsigned off=p->misc[j].offset;uint32_t mask=p->misc[j].mask;
   assert((regs[off/4]&mask)==(original[off/4]&mask));
  }
 }
 for(unsigned i=0;i<COUNT(source_ranges);i++)
  for(unsigned o=source_ranges[i][0];o<=source_ranges[i][1];o+=4)assert(regs[o/4]==original[o/4]);
 for(unsigned i=0;i<COUNT(banks);i++){
  assert(regs[banks[i].enable/4]==original[banks[i].enable/4]);
  assert(regs[banks[i].reset/4]==original[banks[i].reset/4]);
 }
 for(unsigned i=0;i<COUNT(scalars);i++){
  unsigned o=scalars[i];uint32_t mask=o==0x50?0xf00003f2U:(o==0x680?0x30000000U:0xffffffffU);
  if((regs[o/4]&mask)!=(original[o/4]&mask))
   fprintf(stderr,"CAR %03x restored %08x expected %08x (mask %08x)\n",o,regs[o/4],original[o/4],mask);
  assert((regs[o/4]&mask)==(original[o/4]&mask));
 }
 assert(regs[0x050/4]&CB(11));assert(!context.valid);
}
int main(void){
 init();assert(tegra_lite_clocks_restore()==PSCI_E_DENIED&&!writes);
 for(unsigned cycle=0;cycle<4;cycle++){
  tegra_lite_clocks_save();assert(context.valid&&flushes==cycle+1);lose();
  assert(tegra_lite_clocks_restore()==0);check();
  unsigned before=writes;assert(tegra_lite_clocks_restore()==PSCI_E_DENIED&&writes==before);
 }
 /* Aborted/retained entry: no PLL frequency change or lock loss. */
 init();tegra_lite_clocks_save();assert(tegra_lite_clocks_restore()==0);
 assert(regs[0x0e8/4]==original[0x0e8/4]);
 /* Hardware-sequenced USB/PCIe parents can be gated in the snapshot. */
 init();regs[0x0c0/4]&=~(PLL_ON|PLL_LOCK);regs[0x0e8/4]&=~CB(31);
 regs[0x0ec/4]&=~CB(11);tegra_lite_clocks_save();lose();
 assert(tegra_lite_clocks_restore()==0);
 assert(regs[0x0c0/4]&PLL_LOCK);assert(regs[0x0ec/4]&CB(11));
 assert(regs[0x530/4]==original[0x530/4]&&regs[0x48c/4]==original[0x48c/4]);
 for(unsigned i=0;i<COUNT(plls)+3;i++){
  init();tegra_lite_clocks_save();lose();
  failing=i<COUNT(plls)?plls[i].base:(i==COUNT(plls)?0x0c0:(i==COUNT(plls)+1?0x52c:0x0e8));
  assert(tegra_lite_clocks_restore()==PSCI_E_INTERN_FAIL&&!bank_writes&&!context.valid);
 }
 init();tegra_lite_clocks_save();lose();regs[0x0a0/4]&=~PLL_LOCK;
 assert(tegra_lite_clocks_restore()==PSCI_E_INTERN_FAIL&&!bank_writes);
 init();tegra_lite_clocks_save();lose();regs[0x0e0/4]=original[0x0e0/4]^0x100;
 uint32_t live_x=regs[0x0e0/4];
 assert(tegra_lite_clocks_restore()==PSCI_E_INTERN_FAIL&&!bank_writes&&regs[0x0e0/4]==live_x);
 puts("PASS: CAR/PLL repeated loss, USB reference dependency, clock/reset ordering, bounded failures and untouched DRAM clocks");
}
'''
with tempfile.TemporaryDirectory(prefix='lite-sc7-clocks-') as temporary:
    p = Path(temporary)
    (p / 'test.c').write_text(prefix + source + suffix)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
