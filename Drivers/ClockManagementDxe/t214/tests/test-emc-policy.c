// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#define STATIC static
#define CONST const
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef size_t UINTN;
#include "../EmcPolicy.h"

int main(void)
{
  // 2100 MHz is not a literal DVB row: the 2133 MHz voltage ceiling covers it.
  assert(LiteEmcRequiredMv(2100000, 2100000000ULL) == 725);
  // If only the trained stock timing is available, it must stay observable as
  // 1600 MHz rather than being rejected for lacking an exact 2100 MHz row.
  assert(LiteEmcRequiredMv(2100000, 1600000000ULL) == 675);
  // A firmware-rounded rate above the requested cap must never be programmed.
  assert(LiteEmcRequiredMv(2100000, 2133000000ULL) == 0);
  assert(LiteEmcRequiredMv(2100000, 0) == 0);
  assert(LiteEmcRequiredMv(2100000, UINT64_MAX) == 0);
  // A one-Hz crossing must use the next voltage band, not truncate to kHz.
  assert(LiteEmcRequiredMv(2100000, 1065600001ULL) == 650);
  return 0;
}
