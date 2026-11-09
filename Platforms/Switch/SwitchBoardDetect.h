// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only Nintendo Switch board identification for the Hekate -> BL33 path.
#ifndef WINX_SWITCH_BOARD_DETECT_H
#define WINX_SWITCH_BOARD_DETECT_H

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>

#define WINX_APB_MISC_GP_HIDREV  0x70000804UL
#define WINX_FUSE_BASE           0x7000F800UL
#define WINX_FUSE_RESERVED_ODM4  0x1D8UL

typedef enum {
  WinxSwitchBoardUnknown = 0,
  WinxSwitchBoardIcosa,
  WinxSwitchBoardIowa,
  WinxSwitchBoardHoag,
  WinxSwitchBoardAula
} WINX_SWITCH_BOARD;

typedef struct {
  WINX_SWITCH_BOARD Board;
  UINT32            HidRev;
  UINT32            Odm4;
  UINT8             ChipMajor;
  UINT8             BoardCode;
} WINX_SWITCH_BOARD_INFO;

// Hekate reads GP_HIDREV and cached FUSE_RESERVED_ODM4. This mirrors that
// read-only classification; it does not issue fuse commands or change fuses.
STATIC inline
WINX_SWITCH_BOARD_INFO
WinxSwitchDetectBoard (
  VOID
  )
{
  WINX_SWITCH_BOARD_INFO Info;

  ZeroMem (&Info, sizeof (Info));
  Info.Board  = WinxSwitchBoardUnknown;
#ifdef SWITCH_QEMU_TEST
  // The generic QEMU virt machine has no Tegra MMIO window. This opt-in
  // build switch exercises the menu without touching physical board registers.
  return Info;
#else
  Info.HidRev = MmioRead32 (WINX_APB_MISC_GP_HIDREV);
  Info.ChipMajor = (UINT8)((Info.HidRev >> 4) & 0x0F);

  if (Info.ChipMajor == 1) {
    Info.Board = WinxSwitchBoardIcosa;
    return Info;
  }

  if (Info.ChipMajor < 2) {
    return Info;
  }

  Info.Odm4 = MmioRead32 (WINX_FUSE_BASE + WINX_FUSE_RESERVED_ODM4);
  Info.BoardCode = (UINT8)((Info.Odm4 >> 16) & 0x0F);
  switch (Info.BoardCode) {
    case 1: Info.Board = WinxSwitchBoardIowa; break;
    case 2: Info.Board = WinxSwitchBoardHoag; break;
    case 4: Info.Board = WinxSwitchBoardAula; break;
    default: break;
  }
  return Info;
#endif
}

STATIC inline
CONST CHAR16 *
WinxSwitchBoardName (
  IN WINX_SWITCH_BOARD Board
  )
{
  switch (Board) {
    case WinxSwitchBoardIcosa: return L"Icosa / Erista / HAC-001";
    case WinxSwitchBoardIowa:  return L"Iowa / Mariko / HAC-001-01";
    case WinxSwitchBoardHoag:  return L"Hoag / Switch Lite / HDH-001";
    case WinxSwitchBoardAula:  return L"Aula / OLED / HEG-001";
    default:                  return L"Unknown Switch board";
  }
}

STATIC inline
BOOLEAN
WinxSwitchBuildHasPinnedBoard (
  VOID
  )
{
#if defined(SWITCH_BOARD_ICOSA) || defined(SWITCH_BOARD_ERISTA) || \
    defined(SWITCH_BOARD_IOWA)  || defined(SWITCH_BOARD_MARIKO) || \
    defined(SWITCH_BOARD_HOAG)  || defined(SWITCH_BOARD_LITE) || \
    defined(SWITCH_LITE_TOUCH)  || defined(SWITCH_BOARD_AULA) || \
    defined(SWITCH_BOARD_OLED)
  return TRUE;
#else
  return FALSE;
#endif
}

STATIC inline
BOOLEAN
WinxSwitchBuildMatchesBoard (
  IN WINX_SWITCH_BOARD Board
  )
{
#if defined(SWITCH_BOARD_ICOSA) || defined(SWITCH_BOARD_ERISTA)
  return (BOOLEAN)(Board == WinxSwitchBoardIcosa);
#elif defined(SWITCH_BOARD_IOWA) || defined(SWITCH_BOARD_MARIKO)
  return (BOOLEAN)(Board == WinxSwitchBoardIowa);
#elif defined(SWITCH_BOARD_HOAG) || defined(SWITCH_BOARD_LITE) || defined(SWITCH_LITE_TOUCH)
  return (BOOLEAN)(Board == WinxSwitchBoardHoag);
#elif defined(SWITCH_BOARD_AULA) || defined(SWITCH_BOARD_OLED)
  return (BOOLEAN)(Board == WinxSwitchBoardAula);
#else
  return TRUE;
#endif
}

#endif
