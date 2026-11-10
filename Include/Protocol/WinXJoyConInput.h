// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef WINX_JOYCON_INPUT_H_
#define WINX_JOYCON_INPUT_H_

#include <Uefi.h>

// Button positions match CTCaer/hekate bdk/input/joycon.h's jc_gamepad_rpt_t.
#define WINX_JOYCON_BUTTON_Y       BIT0
#define WINX_JOYCON_BUTTON_X       BIT1
#define WINX_JOYCON_BUTTON_B       BIT2
#define WINX_JOYCON_BUTTON_A       BIT3
#define WINX_JOYCON_BUTTON_SR_R    BIT4
#define WINX_JOYCON_BUTTON_SL_R    BIT5
#define WINX_JOYCON_BUTTON_R       BIT6
#define WINX_JOYCON_BUTTON_ZR      BIT7
#define WINX_JOYCON_BUTTON_MINUS   BIT8
#define WINX_JOYCON_BUTTON_PLUS    BIT9
#define WINX_JOYCON_BUTTON_R3      BIT10
#define WINX_JOYCON_BUTTON_L3      BIT11
#define WINX_JOYCON_BUTTON_HOME    BIT12
#define WINX_JOYCON_BUTTON_CAPTURE BIT13
#define WINX_JOYCON_BUTTON_DOWN    BIT16
#define WINX_JOYCON_BUTTON_UP      BIT17
#define WINX_JOYCON_BUTTON_RIGHT   BIT18
#define WINX_JOYCON_BUTTON_LEFT    BIT19
#define WINX_JOYCON_BUTTON_L       BIT22
#define WINX_JOYCON_BUTTON_ZL      BIT23

#define WINX_JOYCON_CONNECTED_LEFT  BIT0
#define WINX_JOYCON_CONNECTED_RIGHT BIT1

typedef struct {
  UINT32  Buttons;
  UINT8   Connected;
  UINT8   BatteryLeft;
  UINT8   BatteryRight;
  UINT8   Reserved;
} WINX_JOYCON_STATE;

typedef struct _WINX_JOYCON_INPUT_PROTOCOL WINX_JOYCON_INPUT_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *WINX_JOYCON_READ_STATE)(
  IN  WINX_JOYCON_INPUT_PROTOCOL  *This,
  OUT WINX_JOYCON_STATE           *State
  );

struct _WINX_JOYCON_INPUT_PROTOCOL {
  EFI_EVENT               WaitForInput;
  WINX_JOYCON_READ_STATE  ReadState;
};

extern EFI_GUID gWinXJoyConInputProtocolGuid;

#endif
