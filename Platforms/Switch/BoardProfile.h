/* Build-time platform identity shared by firmware components. */
#ifndef WINX_SWITCH_BOARD_PROFILE_H
#define WINX_SWITCH_BOARD_PROFILE_H

#if defined(SWITCH_BOARD_ICOSA) || defined(SWITCH_BOARD_ERISTA)
#include "t210/Icosa/BoardProfile.h"
#elif defined(SWITCH_BOARD_IOWA) || defined(SWITCH_BOARD_MARIKO)
#include "t214/Iowa/BoardProfile.h"
#elif defined(SWITCH_BOARD_AULA) || defined(SWITCH_BOARD_OLED)
#include "t214/Aula/BoardProfile.h"
#elif defined(SWITCH_BOARD_HOAG) || defined(SWITCH_BOARD_LITE) || defined(SWITCH_LITE_TOUCH)
#include "t214/Hoag/BoardProfile.h"
#else
#define WINX_BOARD_PROFILE_UNSPECIFIED 1
#define WINX_BOARD_PROFILE_NAME "legacy/default Switch profile"
#define WINX_BOARD_RETAIL_MODEL "HAC-001"
#endif

#endif
