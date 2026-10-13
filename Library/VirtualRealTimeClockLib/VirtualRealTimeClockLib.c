/** @file
*
*  Implement dummy EFI RealTimeClock runtime services.
*
*  Copyright (c), 2018, Andrei Warkentin <andrey.warkentin@gmail.com>
*  Copyright (c) Microsoft Corporation. All rights reserved.
*
*  This program and the accompanying materials
*  are licensed and made available under the terms and conditions of the BSD License
*  which accompanies this distribution.  The full text of the license may be found at
*  http://opensource.org/licenses/bsd-license.php
*
*  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
*  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
*
**/

#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/RealTimeClockLib.h>
#include <Library/TimerLib.h>
#include <Library/ArmGenericTimerCounterLib.h>

//
// Calendar helpers (proleptic Gregorian, days counted from 1970-01-01).
// Public-domain algorithms by Howard Hinnant ("chrono-compatible low-level date algorithms").
// Kept free of globals and pointers so they stay valid after SetVirtualAddressMap().
//
STATIC
INT64
DaysFromCivil (
  IN INT64  Year,
  IN UINTN  Month,    // 1..12
  IN UINTN  Day       // 1..31
  )
{
	INT64  Era;
	INT64  Yoe;
	INT64  Doy;
	INT64  Doe;

	Year -= (Month <= 2) ? 1 : 0;
	Era   = (Year >= 0 ? Year : Year - 399) / 400;
	Yoe   = Year - Era * 400;                                                       // [0, 399]
	Doy   = (153 * ((INT64) Month + (Month > 2 ? -3 : 9)) + 2) / 5 + (INT64) Day - 1; // [0, 365]
	Doe   = Yoe * 365 + Yoe / 4 - Yoe / 100 + Doy;                                  // [0, 146096]

	return Era * 146097 + Doe - 719468;
}

STATIC
VOID
CivilFromDays (
  IN  INT64   Days,
  OUT INT64   *Year,
  OUT UINT8   *Month,
  OUT UINT8   *Day
  )
{
	INT64  Z;
	INT64  Era;
	INT64  Doe;
	INT64  Yoe;
	INT64  Doy;
	INT64  Mp;
	INT64  M;

	Z   = Days + 719468;
	Era = (Z >= 0 ? Z : Z - 146096) / 146097;
	Doe = Z - Era * 146097;                                                         // [0, 146096]
	Yoe = (Doe - Doe / 1460 + Doe / 36524 - Doe / 146096) / 365;                    // [0, 399]
	Doy = Doe - (365 * Yoe + Yoe / 4 - Yoe / 100);                                  // [0, 365]
	Mp  = (5 * Doy + 2) / 153;                                                      // [0, 11]
	M   = Mp < 10 ? Mp + 3 : Mp - 9;                                                // [1, 12]

	*Day   = (UINT8) (Doy - (153 * Mp + 2) / 5 + 1);                                // [1, 31]
	*Month = (UINT8) M;
	*Year  = Yoe + Era * 400 + (M <= 2 ? 1 : 0);
}

/**
  Seconds since 1970-01-01 00:00:00 of 00:00:00 on the day this firmware was compiled.

  There is no battery-backed clock reachable from here, so the date the firmware was built is
  the best "not wildly wrong" date to start counting from (the previous constant, June 2018,
  makes every TLS certificate look not-yet-valid). The OS corrects it as soon as it has any
  time source; the firmware never claims better than "build date + time since power-on".

  __DATE__ is "Mmm dd yyyy" with the day space-padded ("Oct  5 2026").
**/
STATIC
INT64
BuildDateEpochSeconds (
  VOID
  )
{
	CONST CHAR8  Date[] = __DATE__;
	CONST CHAR8  Names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	UINTN        Month;
	UINTN        Day;
	INT64        Year;

	for (Month = 0; Month < 12; Month++) {
		if (Date[0] == Names[Month * 3] && Date[1] == Names[Month * 3 + 1] && Date[2] == Names[Month * 3 + 2]) {
			break;
		}
	}

	Day  = (Date[4] == ' ' ? 0 : (UINTN) (Date[4] - '0')) * 10 + (UINTN) (Date[5] - '0');
	Year = (INT64) (Date[7] - '0') * 1000 + (INT64) (Date[8] - '0') * 100 +
	       (INT64) (Date[9] - '0') * 10 + (INT64) (Date[10] - '0');

	if (Month == 12 || Day < 1 || Day > 31 || Year < 2020 || Year > 2200) {
		// Unparsable or implausible: fall back to the old fixed date rather than invent one.
		return DaysFromCivil (2018, 6, 1) * 86400;
	}

	return DaysFromCivil (Year, Month + 1, Day) * 86400;
}

/**
Returns the current time and date information, and the time-keeping capabilities
of the virtual RTC.

This LibGetTime reports the build date of the firmware plus the time since the ARM generic
counter started. It is still a virtual clock (nothing is kept across boots, SetTime is not
supported), but the fields are a valid calendar date and the value never goes backwards.

@param  Time                  A pointer to storage to receive a snapshot of the current time.
@param  Capabilities          An optional pointer to a buffer to receive the real time clock
device's capabilities.

@retval EFI_SUCCESS           The operation completed successfully.
@retval EFI_INVALID_PARAMETER Time is NULL.
@retval EFI_DEVICE_ERROR      The time could not be retrieved due to hardware error.

**/
EFI_STATUS
EFIAPI
LibGetTime(
	OUT EFI_TIME                *Time,
	OUT  EFI_TIME_CAPABILITIES  *Capabilities
)
{
	UINT32 Freq = ArmGenericTimerGetTimerFreq();

	if (Time == NULL) {
		return EFI_INVALID_PARAMETER;
	}

	//
	// Depend on ARM generic timer to report date/time relative to the
	// start of CPU timer counting where date and time will always
	// be relative to the date/time 1/1/1900 00H:00M:00S
	//

	ASSERT(Freq != 0);
	if (Freq == 0) {
		return EFI_DEVICE_ERROR;
	}

	if (Capabilities) {
		Capabilities->Accuracy = 0;
		Capabilities->Resolution = Freq;
		Capabilities->SetsToZero = FALSE;
	}

	UINT64 ElapsedSeconds = GetPerformanceCounter() / Freq;

	//
	// Wall clock = build date + time since the generic counter started (power-on). Month/day/year
	// are real calendar values (Day is 1..31), not an uptime split into fields as before.
	//
	INT64   Seconds = BuildDateEpochSeconds() + (INT64) ElapsedSeconds;
	INT64   Days    = Seconds / 86400;
	UINT32  OfDay   = (UINT32) (Seconds % 86400);
	INT64   Year;

	CivilFromDays(Days, &Year, &Time->Month, &Time->Day);
	Time->Year   = (UINT16) Year;
	Time->Hour   = (UINT8) (OfDay / 3600);
	Time->Minute = (UINT8) ((OfDay % 3600) / 60);
	Time->Second = (UINT8) (OfDay % 60);

	//
	// Not required to report in our special case
	//

	Time->Nanosecond = 0;
	Time->TimeZone = 0;
	Time->Daylight = 0;

	return EFI_SUCCESS;
}


/**
Sets the current local time and date information.

@param  Time                  A pointer to the current time.

@retval EFI_SUCCESS           The operation completed successfully.
@retval EFI_INVALID_PARAMETER A time field is out of range.
@retval EFI_DEVICE_ERROR      The time could not be set due due to hardware error.

**/
EFI_STATUS
EFIAPI
LibSetTime(
	IN EFI_TIME                *Time
)
{
	//
	// The virtual clock is read-only.
	//
	return EFI_UNSUPPORTED;
}


/**
Returns the current wakeup alarm clock setting.

@param  Enabled               Indicates if the alarm is currently enabled or disabled.
@param  Pending               Indicates if the alarm signal is pending and requires acknowledgement.
@param  Time                  The current alarm setting.

@retval EFI_SUCCESS           The alarm settings were returned.
@retval EFI_INVALID_PARAMETER Any parameter is NULL.
@retval EFI_DEVICE_ERROR      The wakeup time could not be retrieved due to a hardware error.

**/
EFI_STATUS
EFIAPI
LibGetWakeupTime(
	OUT BOOLEAN     *Enabled,
	OUT BOOLEAN     *Pending,
	OUT EFI_TIME    *Time
)
{
	return EFI_UNSUPPORTED;
}


/**
Sets the system wakeup alarm clock time.

@param  Enabled               Enable or disable the wakeup alarm.
@param  Time                  If Enable is TRUE, the time to set the wakeup alarm for.

@retval EFI_SUCCESS           If Enable is TRUE, then the wakeup alarm was enabled. If
Enable is FALSE, then the wakeup alarm was disabled.
@retval EFI_INVALID_PARAMETER A time field is out of range.
@retval EFI_DEVICE_ERROR      The wakeup time could not be set due to a hardware error.
@retval EFI_UNSUPPORTED       A wakeup timer is not supported on this platform.

**/
EFI_STATUS
EFIAPI
LibSetWakeupTime(
	IN BOOLEAN      Enabled,
	OUT EFI_TIME    *Time
)
{
	return EFI_UNSUPPORTED;
}


/**
This is the declaration of an EFI image entry point. This can be the entry point to an application
written to this specification, an EFI boot service driver, or an EFI runtime driver.

@param  ImageHandle           Handle that identifies the loaded image.
@param  SystemTable           System Table for this image.

@retval EFI_SUCCESS           The operation completed successfully.

**/
EFI_STATUS
EFIAPI
LibRtcInitialize(
	IN EFI_HANDLE                            ImageHandle,
	IN EFI_SYSTEM_TABLE                      *SystemTable
)
{
	return EFI_SUCCESS;
}


/**
Fixup internal data so that EFI can be call in virtual mode.
Call the passed in Child Notify event and convert any pointers in
lib to virtual mode.

@param[in]    Event   The Event that is being processed
@param[in]    Context Event Context
**/
VOID
EFIAPI
LibRtcVirtualNotifyEvent(
	IN EFI_EVENT        Event,
	IN VOID             *Context
)
{
	return;
}
