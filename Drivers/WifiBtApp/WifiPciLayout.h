/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef WIFI_PCI_LAYOUT_H
#define WIFI_PCI_LAYOUT_H

/* Allocate both endpoint BARs inside one non-prefetchable outbound aperture.
 * PCI bridge memory base/limit fields have 1 MiB granularity, inclusive limit.
 */
STATIC BOOLEAN
PexMemoryLayout (UINT32 Base, UINT32 Length, UINT32 Size0, UINT32 Size2,
                 UINT32 *Bar0, UINT32 *Bar2, UINT32 *BridgeWindow)
{
  UINT64 Address2, End, ApertureEnd = (UINT64)Base + Length;
  if ((Base & 0xFFFFFU) || !Length || !Size0 || !Size2 ||
      (Size0 & (Size0 - 1U)) || (Size2 & (Size2 - 1U)) ||
      ApertureEnd > 0x100000000ULL || (Base & (Size0 - 1U))) return FALSE;
  Address2 = ((UINT64)Base + Size0 + Size2 - 1U) & ~((UINT64)Size2 - 1U);
  End = Address2 + Size2;
  if (End > ApertureEnd) return FALSE;
  *Bar0 = Base;
  *Bar2 = (UINT32)Address2;
  *BridgeWindow = ((UINT32)(End - 1U) & 0xFFF00000U) | ((Base >> 16) & 0xFFF0U);
  return TRUE;
}
#endif
