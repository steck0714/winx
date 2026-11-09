// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef WINX_SWITCH_MOUSE_CURSOR_H
#define WINX_SWITCH_MOUSE_CURSOR_H

#include <Uefi.h>

typedef struct {
  UINT32   ScreenWidth;
  UINT32   ScreenHeight;
  UINT32   X;
  UINT32   Y;
  BOOLEAN  Visible;
  BOOLEAN  LeftButton;
  BOOLEAN  RightButton;
} WINX_MOUSE_CURSOR_STATE;

typedef struct _WINX_MOUSE_CURSOR_PROTOCOL WINX_MOUSE_CURSOR_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *WINX_MOUSE_CURSOR_GET_STATE)(
  IN  WINX_MOUSE_CURSOR_PROTOCOL  *This,
  OUT WINX_MOUSE_CURSOR_STATE     *State
  );

struct _WINX_MOUSE_CURSOR_PROTOCOL {
  WINX_MOUSE_CURSOR_GET_STATE  GetState;
  EFI_EVENT                   WaitForChange;
};

#endif
