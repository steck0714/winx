/* SPDX-License-Identifier: GPL-2.0-only */
#include "../src/button_state.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    struct swpwr_button_state s={0};
    struct swpwr_button_event e;
    for (unsigned i=0;i<6;++i) {
        /* Active press sleeps; a later screen-off press must only wake. */
        e=swpwr_button_update(&s,true,true,false);
        assert(e.emit && !e.pulse && e.buttons==SWPWR_POWER);
        e=swpwr_button_update(&s,true,false,true); assert(!e.emit);
        e=swpwr_button_update(&s,false,false,true); assert(e.emit && !e.buttons);
        e=swpwr_button_update(&s,true,true,true);
        assert(e.emit && !e.pulse && e.buttons==SWPWR_WAKE);
        e=swpwr_button_update(&s,true,false,false); assert(!e.emit);
        e=swpwr_button_update(&s,false,false,false); assert(e.emit && !e.buttons);
        e=swpwr_button_update(&s,false,false,false); assert(!e.emit);
    }
    e=swpwr_button_update(&s,false,true,true); assert(e.emit && e.pulse && e.buttons==SWPWR_WAKE);
    e=swpwr_button_update(&s,false,false,false); assert(!e.emit);
    e=swpwr_button_update(&s,false,true,false); assert(e.emit && e.pulse && e.buttons==SWPWR_POWER);
    s.suppress_held=true;
    e=swpwr_button_update(&s,true,true,false); assert(!e.emit);
    e=swpwr_button_update(&s,true,false,false); assert(!e.emit);
    e=swpwr_button_update(&s,false,true,false); assert(!e.emit);
    e=swpwr_button_update(&s,true,true,false); assert(e.emit && e.buttons==SWPWR_POWER);
    puts("Power-button sleep/wake routing, held transitions and resume suppression: PASS (model)");
    return 0;
}
