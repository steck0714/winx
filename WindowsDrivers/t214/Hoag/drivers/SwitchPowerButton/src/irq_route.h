/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SWPWR_IRQ_ROUTE_H
#define SWPWR_IRQ_ROUTE_H
#include <stdbool.h>
#include <stdint.h>

#define SWPWR_TOP_MASK 0x0d
#define SWPWR_GLOBAL_MASK 0x0e
#define SWPWR_EDGE_MASK 0x12
#define SWPWR_TOP_ONOFF 0x02
#define SWPWR_GLOBAL_INTM 0x01
#define SWPWR_EN0_F 0x04
#define SWPWR_EN0_R 0x08

struct swpwr_irq_route {
    uint8_t top, global, edge;
    bool saved, armed;
};
typedef int32_t (*swpwr_irq_read)(void *, uint8_t, uint8_t *);
typedef int32_t (*swpwr_irq_write)(void *, uint8_t, uint8_t, uint8_t);

/* Callers serialize mask changes through the device's power callbacks.
 * The top route stays masked until VHF and the passive ISR are ready.
 */
static inline int32_t swpwr_irq_snapshot(struct swpwr_irq_route *s, void *ctx,
    swpwr_irq_read read, swpwr_irq_write write)
{
    uint8_t top, global, edge;
    int32_t status;
    if (s->armed) return 0; /* Never clear or mask a latched wake on resume. */
    status = read(ctx, SWPWR_TOP_MASK, &top);
    if (status < 0) return status;
    if (!s->saved) {
        status = read(ctx, SWPWR_GLOBAL_MASK, &global);
        if (status >= 0) status = read(ctx, SWPWR_EDGE_MASK, &edge);
        if (status < 0) return status;
        s->top = top; s->global = global; s->edge = edge; s->saved = true;
    }
    return write(ctx, SWPWR_TOP_MASK, top | SWPWR_TOP_ONOFF, SWPWR_TOP_ONOFF);
}

/* Preserve unrelated bits, mask the top route before changing child masks,
 * and restore the top route last. Retain the snapshot on any failed restore.
 */
static inline int32_t swpwr_irq_set(struct swpwr_irq_route *s, void *ctx,
    swpwr_irq_read read, swpwr_irq_write write, bool enable)
{
    uint8_t top, global, edge;
    int32_t status;
    if (enable && s->armed) return 0;
    if (!s->saved) return enable ? (int32_t)0xc0000184u : 0;
    s->armed = false;
    status = read(ctx, SWPWR_TOP_MASK, &top);
    if (status >= 0) status = read(ctx, SWPWR_GLOBAL_MASK, &global);
    if (status >= 0) status = read(ctx, SWPWR_EDGE_MASK, &edge);
    if (status < 0) return status;
    status = write(ctx, SWPWR_TOP_MASK, top | SWPWR_TOP_ONOFF, SWPWR_TOP_ONOFF);
    if (status >= 0) status = write(ctx, SWPWR_EDGE_MASK,
        enable ? (edge | SWPWR_EN0_F) & ~SWPWR_EN0_R :
                 (edge & ~0x0c) | (s->edge & 0x0c), 0x0c);
    if (status >= 0) status = write(ctx, SWPWR_GLOBAL_MASK,
        enable ? global & ~SWPWR_GLOBAL_INTM :
                 (global & ~SWPWR_GLOBAL_INTM) | (s->global & SWPWR_GLOBAL_INTM), SWPWR_GLOBAL_INTM);
    if (status >= 0) status = write(ctx, SWPWR_TOP_MASK,
        enable ? top & ~SWPWR_TOP_ONOFF :
                 (top & ~SWPWR_TOP_ONOFF) | (s->top & SWPWR_TOP_ONOFF), SWPWR_TOP_ONOFF);
    if (status >= 0) {
        s->armed = enable;
        if (!enable) s->saved = false;
    }
    return status;
}
#endif
