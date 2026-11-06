/* Check PCI memory-window decoding against the endpoint BAR allocations. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define STATIC static
#define TRUE 1
#define FALSE 0
typedef int BOOLEAN;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
#include "../WifiPciLayout.h"

int main(void)
{
    UINT32 a0,a2,window;
    assert(PexMemoryLayout(0x13000000,0x0d000000,0x8000,0x400000,&a0,&a2,&window));
    assert(a0==0x13000000 && a2==0x13400000 && window==0x13701300);
    UINT32 base=(window&0xfff0)<<16,limit=(window&0xfff00000)|0xfffff;
    assert(base==a0 && limit==a2+0x400000-1);
    // The old 0x13301300 bridge window excluded the full firmware TCM range.
    assert(a2+0x180000>0x133fffff && a2+0x180000+0xc0000-1<=limit);
    for(UINT32 bar2=0x100000;bar2<=0x1000000;bar2<<=1) {
        assert(PexMemoryLayout(0x13000000,0x0d000000,0x8000,bar2,&a0,&a2,&window));
        base=(window&0xfff0)<<16;limit=(window&0xfff00000)|0xfffff;
        assert(!(a2&(bar2-1)) && a2>=a0+0x8000 && base<=a0 && limit>=a2+bar2-1);
    }
    assert(!PexMemoryLayout(0x13000000,0x400000,0x8000,0x400000,&a0,&a2,&window));
    assert(!PexMemoryLayout(0xfff00000,0x200000,0x8000,0x400000,&a0,&a2,&window));
    assert(!PexMemoryLayout(0x13000000,0x0d000000,0,0x400000,&a0,&a2,&window));
    assert(!PexMemoryLayout(0x13000000,0x0d000000,0x8000,0x300000,&a0,&a2,&window));
    puts("PCI bridge window covers both BARs and the BCM4356 TCM range");
    return 0;
}
