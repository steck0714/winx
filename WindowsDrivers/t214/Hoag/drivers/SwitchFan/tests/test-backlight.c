/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdint.h>
typedef uint32_t ULONG;
#include "../src/backlight.h"

int main(void)
{
    /* Display power must be able to reach zero without losing the user's level. */
    assert(SwBacklightDuty(150, 0) == 0);
    assert(SwBacklightDuty(150, 1) == 150);
    assert(SwBacklightDuty(150, 2) == 50);
    assert(SwBacklightDuty(8, 2) == 8);
    assert(SwBacklightClamp(0) == 8);
    assert(SwBacklightClamp(UINT32_MAX) == 255);
    /* Clear a stale duty, preserving a nonzero divider through off/on. */
    assert(SwBacklightCsr(0x80FF0003u, 0) == 0x80000003u);
    assert(SwBacklightCsr(0x80000003u, 150) == 0x80960003u);
    assert(SwBacklightCsr(0x00001234u, 255) == 0x80FF1234u);
    return 0;
}
