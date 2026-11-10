/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SWITCH_POWER_BUTTON_STATE_H
#define SWITCH_POWER_BUTTON_STATE_H
#include <stdbool.h>
#include <stdint.h>
#define SWPWR_POWER 1u
#define SWPWR_WAKE 2u
struct swpwr_button_state { bool held, suppress_held; };
struct swpwr_button_event { bool emit, pulse; uint8_t buttons; };
static inline struct swpwr_button_event swpwr_button_update(
    struct swpwr_button_state *s, bool held, bool latched_press, bool display_off)
{
    struct swpwr_button_event e = {false,false,0};
    if (s->suppress_held) {
        if (!held) { s->suppress_held=false; s->held=false; }
        return e;
    }
    if (held != s->held) {
        s->held=held; e.emit=true;
        e.buttons=held ? (display_off ? SWPWR_WAKE : SWPWR_POWER) : 0;
    } else if (!held && latched_press) {
        e.emit=e.pulse=true;
        e.buttons=display_off ? SWPWR_WAKE : SWPWR_POWER;
    }
    /* Display changes while held never generate another press. The release
     * clears the original report regardless of the new display state.
     */
    return e;
}
#endif
