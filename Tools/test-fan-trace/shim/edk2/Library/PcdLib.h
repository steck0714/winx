#pragma once
#include <Uefi.h>
extern uint8_t g_pcd_duty;
#define FixedPcdGet8(TokenName) (g_pcd_duty)
