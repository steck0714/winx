/* SPDX-License-Identifier: GPL-2.0-only */
/* Compatibility entry point; board policy is selected at build time. */
#ifndef WIFI_PREPARE_H
#define WIFI_PREPARE_H

#include "../../Platforms/Switch/BoardProfile.h"
#include "WifiPrepareCommon.h"

#ifdef SWITCH_WIFI_ERISTA
#if !defined(WINX_BOARD_PROFILE_ICOSA)
#error "SWITCH_WIFI_ERISTA must be built with the Erista board profile"
#endif
#include "t210/Icosa/Prepare.h"
#elif defined(WINX_BOARD_PROFILE_HOAG)
#include "t214/Hoag/Prepare.h"
#else
#error "WifiPrepare has no validated power profile for this Switch board"
#endif

#endif
