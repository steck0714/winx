/* SPDX-License-Identifier: ISC */
#include "../core/pci_intx.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct fixture {
    uint32_t cfg[64], values[8];
    uint16_t offsets[8], ignore;
    unsigned writes;
} fixture;

static uint32_t read_config(void *context, uint16_t offset)
{
    fixture *f = context;
    assert(offset < 256 && !(offset & 3));
    return f->cfg[offset/4];
}

static void write_config(void *context, uint16_t offset, uint32_t value)
{
    fixture *f = context;
    assert(f->writes < 8 && !(offset & 3));
    f->offsets[f->writes] = offset;
    f->values[f->writes++] = value;
    if (offset == f->ignore) return;
    if (offset == 4) {
        assert(!(value >> 16) && !(value & 4)); /* never W1C status or start DMA */
        f->cfg[1] = (f->cfg[1] & 0xffff0000) | value;
    } else f->cfg[offset/4] = value;
}

static void reset(fixture *f)
{
    memset(f,0,sizeof(*f));
    f->cfg[1] = 0x80100002; /* status/cap list, memory decoding enabled */
    f->cfg[0x3c/4] = 0x100; /* INTA */
    f->cfg[0x34/4] = 0x40;
    f->cfg[0x40/4] = 0x00004810; /* PCIe capability followed by MSI */
    f->cfg[0x48/4] = 0x01816005; /* 64-bit MSI with enable set */
    f->cfg[0x60/4] = 0x80030011; /* MSI-X with three table-size bits */
}

int main(void)
{
    fixture f;
    const swifi_pci_config io = {&f,read_config,write_config};
    reset(&f);
    assert(swifi_select_intx(&io)==SWIFI_INTX_OK);
    const uint16_t order[5]={4,0x48,0x60,0x60,4};
    assert(f.writes==5 && !memcmp(f.offsets,order,sizeof(order)));
    assert(f.values[0]==0x402 && f.values[4]==2);
    assert(f.cfg[0x48/4]==0x01806005 && f.cfg[0x60/4]==0x40030011);
    assert(f.cfg[1]==0x80100002);
    f.writes=0;
    assert(swifi_select_intx(&io)==SWIFI_INTX_OK && !f.writes);
    reset(&f); f.cfg[1]=0x80000402; /* no capability list */
    assert(swifi_select_intx(&io)==SWIFI_INTX_OK && f.writes==1);
    reset(&f); f.cfg[0x48/4]=0x00014005; /* cyclic chain */
    assert(swifi_select_intx(&io)==SWIFI_INTX_INVALID_CAPS && !f.writes);
    reset(&f); f.cfg[0x34/4]=0x43;
    assert(swifi_select_intx(&io)==SWIFI_INTX_INVALID_CAPS && !f.writes);
    reset(&f); f.cfg[0x3c/4]=0;
    assert(swifi_select_intx(&io)==SWIFI_INTX_NO_PIN && !f.writes);
    reset(&f); f.cfg[1]|=4;
    assert(swifi_select_intx(&io)==SWIFI_INTX_BUS_MASTER && !f.writes);
    reset(&f); f.cfg[0x48/4]=UINT32_MAX;
    assert(swifi_select_intx(&io)==SWIFI_INTX_NO_DEVICE && !f.writes);
    reset(&f); f.ignore=0x48;
    assert(swifi_select_intx(&io)==SWIFI_INTX_WRITE_FAILED && f.writes==2 && (f.cfg[1]&0x400));
    puts("PCI INTx: capability walk, MSI/MSI-X disable, command readback and retry passed");
    return 0;
}
