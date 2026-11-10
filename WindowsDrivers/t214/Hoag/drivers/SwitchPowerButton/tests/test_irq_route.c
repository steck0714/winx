/* SPDX-License-Identifier: GPL-2.0-only */
#include "../src/irq_route.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define IO_ERROR ((int32_t)0xc0000185u)
struct pmic {
    uint8_t regs[0x20];
    unsigned calls, fail_at, writes;
    bool fail_after_write;
};
static int32_t read_reg(void *ctx, uint8_t reg, uint8_t *value)
{
    struct pmic *p = ctx;
    /* Mask transitions must never consume the clear-on-read wake edge. */
    assert(reg == SWPWR_TOP_MASK || reg == SWPWR_GLOBAL_MASK || reg == SWPWR_EDGE_MASK);
    if (++p->calls == p->fail_at) return IO_ERROR;
    *value = p->regs[reg];
    return 0;
}
static int32_t write_reg(void *ctx, uint8_t reg, uint8_t value, uint8_t mask)
{
    struct pmic *p = ctx;
    bool fail = ++p->calls == p->fail_at;
    assert(((value ^ p->regs[reg]) & ~mask) == 0);
    if (reg != SWPWR_TOP_MASK) assert(p->regs[SWPWR_TOP_MASK] & SWPWR_TOP_ONOFF);
    if (fail && !p->fail_after_write) return IO_ERROR;
    p->regs[reg] = value;
    ++p->writes;
    return fail ? IO_ERROR : 0; /* Also model successful write / failed readback. */
}
static int32_t snapshot(struct swpwr_irq_route *s, struct pmic *p)
{
    return swpwr_irq_snapshot(s,p,read_reg,write_reg);
}
static int32_t set_route(struct swpwr_irq_route *s, struct pmic *p, bool enable)
{
    return swpwr_irq_set(s,p,read_reg,write_reg,enable);
}
static void check_owned(const struct pmic *p, const struct pmic *original)
{
    assert(((p->regs[SWPWR_TOP_MASK] ^ original->regs[SWPWR_TOP_MASK]) & SWPWR_TOP_ONOFF) == 0);
    assert(((p->regs[SWPWR_GLOBAL_MASK] ^ original->regs[SWPWR_GLOBAL_MASK]) & SWPWR_GLOBAL_INTM) == 0);
    assert(((p->regs[SWPWR_EDGE_MASK] ^ original->regs[SWPWR_EDGE_MASK]) & 0x0c) == 0);
}
int main(void)
{
    for (unsigned seed = 0; seed < 256; ++seed) {
        struct pmic original = {0};
        original.regs[SWPWR_TOP_MASK] = seed;
        original.regs[SWPWR_GLOBAL_MASK] = seed * 73u;
        original.regs[SWPWR_EDGE_MASK] = seed * 137u;
        struct pmic p = original;
        struct swpwr_irq_route s = {0};
        assert(set_route(&s,&p,true) < 0 && !p.calls);
        assert(snapshot(&s,&p) == 0 && s.saved && !s.armed);
        assert(p.regs[SWPWR_TOP_MASK] & SWPWR_TOP_ONOFF);
        assert(set_route(&s,&p,true) == 0 && s.armed);
        assert(!(p.regs[SWPWR_TOP_MASK] & SWPWR_TOP_ONOFF));
        assert(!(p.regs[SWPWR_GLOBAL_MASK] & SWPWR_GLOBAL_INTM));
        assert((p.regs[SWPWR_EDGE_MASK] & 0x0c) == SWPWR_EN0_F);
        unsigned calls = p.calls;
        assert(snapshot(&s,&p) == 0 && set_route(&s,&p,true) == 0 && p.calls == calls);
        /* An unrelated owner can change all bits outside this driver's masks. */
        p.regs[SWPWR_TOP_MASK] ^= 0xfd;
        p.regs[SWPWR_GLOBAL_MASK] ^= 0xfe;
        p.regs[SWPWR_EDGE_MASK] ^= 0xf3;
        struct pmic changed = p;
        assert(set_route(&s,&p,false) == 0 && !s.saved && !s.armed);
        check_owned(&p,&original);
        assert(((p.regs[SWPWR_TOP_MASK] ^ changed.regs[SWPWR_TOP_MASK]) & 0xfd) == 0);
        assert(((p.regs[SWPWR_GLOBAL_MASK] ^ changed.regs[SWPWR_GLOBAL_MASK]) & 0xfe) == 0);
        assert(((p.regs[SWPWR_EDGE_MASK] ^ changed.regs[SWPWR_EDGE_MASK]) & 0xf3) == 0);

        for (unsigned after = 0; after < 2; ++after) {
            /* Every read, write and write-readback failure retains enough
             * ownership for a successful later restore, including partial arm.
             */
            for (unsigned phase = 0; phase < 3; ++phase) {
                unsigned operations = phase == 0 ? 4 : 7;
                for (unsigned fail = 1; fail <= operations; ++fail) {
                    p = original; memset(&s,0,sizeof(s));
                    if (phase > 0) assert(snapshot(&s,&p) == 0);
                    if (phase > 1) assert(set_route(&s,&p,true) == 0);
                    p.calls = 0; p.fail_at = fail; p.fail_after_write = after;
                    int32_t status = phase == 0 ? snapshot(&s,&p) : set_route(&s,&p,phase == 1);
                    assert(status == IO_ERROR && !s.armed);
                    assert(s.saved == (phase > 0 || fail == 4));
                    p.fail_at = 0;
                    assert(set_route(&s,&p,false) == 0 && !s.saved && !s.armed);
                    check_owned(&p,&original);
                }
            }
        }
    }
    /* A saved, not-yet-armed retry must mask the route without resnapshotting. */
    struct pmic p = {0}; struct swpwr_irq_route s = {0};
    assert(snapshot(&s,&p) == 0);
    p.regs[SWPWR_TOP_MASK] = 0;
    assert(snapshot(&s,&p) == 0 && s.top == 0);
    assert(p.regs[SWPWR_TOP_MASK] == SWPWR_TOP_ONOFF);
    puts("Power wake IRQ mask ownership, ordering, resume and injected failures: PASS (model)");
    return 0;
}
