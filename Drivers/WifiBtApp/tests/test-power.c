/* Focused MAX77620 model for the B01 PCIe supply acquisition/release policy. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>
#define STATIC static
#define TRUE 1
#define FALSE 0
#define I2C_5 4
typedef uint8_t UINT8;
typedef uint32_t UINT32;
typedef size_t UINTN;
typedef int BOOLEAN;
static UINT8 reg[256];
static unsigned writes, fail_write, delay;
static int fail_read, no_power_good;
static void Print(const wchar_t *fmt, ...) { (void)fmt; }
static void MicroSecondDelay(UINTN us) { delay += (unsigned)us; }
static int i2c_recv_buf_small(UINT8 *p,UINT32 n,UINT32 bus,UINT32 address,UINT32 offset) {
    assert(n==1 && bus==I2C_5 && address==0x3c && offset<256);
    if(fail_read)return 0;
    if(offset==0x26) *p=(!no_power_good && (reg[0x25]&0xc0)==0xc0)?8:0;
    else *p=reg[offset];
    return 1;
}
static UINT32 i2c_send_byte(UINT32 bus,UINT32 address,UINT32 offset,UINT8 value) {
    assert(bus==I2C_5 && address==0x3c && (offset==0x25 || offset==0x47));
    if(++writes==fail_write)return 0;
    reg[offset]=value;return 1;
}
#include "../t214/Hoag/power.h"
static void reset(void) {
    memset(reg,0,sizeof(reg));reg[0x25]=0x0a;reg[0x47]=0xf8;reg[0x19]=0x60;
    writes=fail_write=delay=0;fail_read=no_power_good=0;
}
int main(void) {
    T214_PCIE_SUPPLY s={0};reset();
    assert(T214PcieSupplyEnable(&s) && s.Changed && reg[0x25]==0xc8 && reg[0x47]==0xf8);
    assert(s.Config==0x0a && s.PowerStatus==8 && delay>=50);
    assert(T214PcieSupplyRestore(&s) && !s.Changed && reg[0x25]==0x0a && reg[0x47]==0xf8);
    reset();memset(&s,0,sizeof(s));reg[0x25]=0xca;reg[0x47]=0x18;
    assert(T214PcieSupplyEnable(&s) && !s.Changed && writes==0);
    assert(T214PcieSupplyRestore(&s) && writes==0 && reg[0x25]==0xca && reg[0x47]==0x18);
    for(unsigned fault=1;fault<=3;fault++) {
        reset();memset(&s,0,sizeof(s));fail_write=fault;
        assert(!T214PcieSupplyEnable(&s) && s.Changed);
        assert(T214PcieSupplyRestore(&s) && reg[0x25]==0x0a && reg[0x47]==0xf8);
    }
    reset();memset(&s,0,sizeof(s));no_power_good=1;
    assert(!T214PcieSupplyEnable(&s) && delay==3000);
    assert(T214PcieSupplyRestore(&s) && reg[0x25]==0x0a);
    reset();memset(&s,0,sizeof(s));fail_read=1;
    assert(!T214PcieSupplyEnable(&s) && !s.Changed && writes==0);
    puts("B01 PCIe rail ramp, shared-rail preservation, restore and I2C failures passed");
    return 0;
}
