// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef WINX_SWITCH_SIDEBAND_BUTTONS_H
#define WINX_SWITCH_SIDEBAND_BUTTONS_H

#include <Uefi.h>
#include <Protocol/DevicePath.h>

#define WINX_SWITCH_BUTTON_VOLUME_DOWN  BIT0
#define WINX_SWITCH_BUTTON_VOLUME_UP    BIT1
#define WINX_SWITCH_BUTTON_POWER       BIT2

#define WINX_SWITCH_BUTTON_DEVICE_GUID \
  { 0x9b235a8f, 0x454c, 0x4c75, { 0x9e, 0xd6, 0x2a, 0x42, 0x87, 0xf0, 0x36, 0xbc } }

#pragma pack (1)
typedef struct {
  VENDOR_DEVICE_PATH       Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} WINX_SWITCH_BUTTON_DEVICE_PATH;
#pragma pack ()

#define WINX_SWITCH_BUTTON_DEVICE_PATH_INIT \
  { \
    { \
      { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, \
        { (UINT8)sizeof (VENDOR_DEVICE_PATH), (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) } }, \
      WINX_SWITCH_BUTTON_DEVICE_GUID \
    }, \
    { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, \
      { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } } \
  }

typedef struct _WINX_SWITCH_BUTTON_PROTOCOL WINX_SWITCH_BUTTON_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *WINX_SWITCH_BUTTON_READ_STATE)(
  IN  WINX_SWITCH_BUTTON_PROTOCOL  *This,
  OUT UINT32                       *Pressed
  );

struct _WINX_SWITCH_BUTTON_PROTOCOL {
  WINX_SWITCH_BUTTON_READ_STATE  ReadState;
  EFI_EVENT                     WaitForChange;
};

#endif
