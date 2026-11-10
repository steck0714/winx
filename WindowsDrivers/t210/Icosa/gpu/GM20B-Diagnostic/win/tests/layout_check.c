/* SPDX-License-Identifier: MIT */
/* Emits win/tests/layout_table.inc as a table of 64-bit values in the COFF section ".lyt"; never executed. */
#if defined(LAYOUT_NTK_MIN)
#include "ntk_min.h"
#else
#include <ddk/ntddk.h>
#include <stddef.h>
#endif

#define L(name, expr) (unsigned long long)(expr),
__attribute__((section(".lyt"), used)) const unsigned long long layout_values[] = {
#include "layout_table.inc"
};
