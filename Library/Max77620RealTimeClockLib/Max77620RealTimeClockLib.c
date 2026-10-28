/** @file
*
*  Real-time clock for Nintendo Switch (Tegra210) backed by the MAX77620 PMIC RTC (I2C5, 0x68).
*
*  The RTC is read ONCE while boot services are still available (after PmicDxe has brought up
*  I2C5) and then kept as "RTC seconds at sync" + "ARM generic counter at sync". Every later
*  GetTime is answered from that soft clock, so the runtime service never touches I2C after
*  ExitBootServices() (the I2C MMIO is not mapped for runtime). SetTime before ExitBootServices
*  also writes the hardware RTC; at runtime it only moves the soft clock.
*
*  If the RTC cannot be read or holds an implausible value, the clock falls back to the previous
*  behaviour (firmware build date + time since power-on), and SetTime updates the soft clock only.
*
*  Register layout and the 24h/binary assumptions follow CTCaer's hekate (bdk/rtc/max77620-rtc.c, GPL-2.0).
*
**/

#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/RealTimeClockLib.h>
#include <Library/TimerLib.h>
#include <Library/ArmGenericTimerCounterLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeLib.h>
#include <Library/I2cLib.h>

#define RTC_I2C_ADDR        0x68
#define RTC_CONTROL_REG     0x03
#define  RTC_BIN_FORMAT     BIT0
#define  RTC_24H            BIT1
#define RTC_UPDATE0_REG     0x04
#define  RTC_WRITE_UPDATE   BIT0
#define  RTC_READ_UPDATE    BIT4
#define RTC_SEC_REG         0x07
#define RTC_MIN_REG         0x08
#define RTC_HOUR_REG        0x09
#define  RTC_HOUR_PM_MASK   BIT6
#define RTC_WEEKDAY_REG     0x0A
#define RTC_MONTH_REG       0x0B
#define RTC_YEAR_REG        0x0C
#define RTC_DAY_REG         0x0D

//
// PMIC protocol GUID (Include/Protocol/Pmic.h): installed by PmicDxe once I2C1/I2C5 are up.
//
STATIC EFI_GUID  mPmicGuid = { 0x9c11c45d, 0xc497, 0x4e95, { 0xac, 0x18, 0x9f, 0x91, 0xca, 0x8b, 0x9a, 0xd1 } };

//
// Soft clock state. Plain integers only (valid after SetVirtualAddressMap).
//
STATIC BOOLEAN  mSynced      = FALSE;   // soft clock initialised (from RTC or from build date)
STATIC BOOLEAN  mHwValid     = FALSE;   // soft clock came from / may be written to the hardware RTC
STATIC UINTN    mSyncTries   = 0;
STATIC BOOLEAN  mFallbackSet = FALSE;   // soft clock already running from the build date
STATIC INT64    mBaseEpoch   = 0;       // seconds since 1970 at mBaseCounter
STATIC UINT64   mBaseCounter = 0;       // generic counter value at sync

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


STATIC
UINT8
BcdToBin (
  IN UINT8  V
  )
{
	return (UINT8) ((V >> 4) * 10 + (V & 0xF));
}

STATIC
BOOLEAN
PmicReady (
  VOID
  )
{
	VOID  *Interface;

	if (EfiAtRuntime () || gBS == NULL) {
		return FALSE;
	}
	return !EFI_ERROR (gBS->LocateProtocol (&mPmicGuid, NULL, &Interface));
}

/**
  Read the hardware RTC. Returns FALSE (and touches nothing) unless I2C5 is known to be up and
  the contents are a plausible calendar date (2020..2099).
**/
STATIC
BOOLEAN
HwReadEpoch (
  OUT INT64  *Epoch
  )
{
	UINT8   Ctl;
	UINT8   Sec, Min, Hour, Day, Month, Year;
	UINT32  Y;

	if (!PmicReady ()) {
		return FALSE;
	}

	// Latch the running RTC into the readable registers, then give it a moment.
	i2c_send_byte (I2C_5, RTC_I2C_ADDR, RTC_UPDATE0_REG, RTC_READ_UPDATE);
	MicroSecondDelay (16000);

	Ctl   = i2c_recv_byte (I2C_5, RTC_I2C_ADDR, RTC_CONTROL_REG);
	Sec   = i2c_recv_byte (I2C_5, RTC_I2C_ADDR, RTC_SEC_REG)   & 0x7F;
	Min   = i2c_recv_byte (I2C_5, RTC_I2C_ADDR, RTC_MIN_REG)   & 0x7F;
	Hour  = i2c_recv_byte (I2C_5, RTC_I2C_ADDR, RTC_HOUR_REG);
	Day   = i2c_recv_byte (I2C_5, RTC_I2C_ADDR, RTC_DAY_REG)   & 0x1F;
	Month = i2c_recv_byte (I2C_5, RTC_I2C_ADDR, RTC_MONTH_REG) & 0x1F;
	Year  = i2c_recv_byte (I2C_5, RTC_I2C_ADDR, RTC_YEAR_REG)  & 0x7F;

	if (!(Ctl & RTC_BIN_FORMAT)) {
		Sec = BcdToBin (Sec); Min = BcdToBin (Min); Day = BcdToBin (Day);
		Month = BcdToBin (Month); Year = BcdToBin (Year);
		Hour = (UINT8) ((Hour & RTC_HOUR_PM_MASK) | BcdToBin (Hour & 0x1F));
	}

	if (Ctl & RTC_24H) {
		Hour &= 0x1F;
	} else {
		UINT8 H12 = Hour & 0x1F;
		Hour = (UINT8) (H12 % 12 + ((Hour & RTC_HOUR_PM_MASK) ? 12 : 0));
	}

	Y = 2000U + Year;
	if (Y < 2020 || Y > 2099 || Month < 1 || Month > 12 || Day < 1 || Day > 31 ||
	    Hour > 23 || Min > 59 || Sec > 59) {
		DEBUG ((DEBUG_WARN, "RTC: implausible hw time %u-%u-%u %u:%u:%u ctl=%02x\n", Y, Month, Day, Hour, Min, Sec, Ctl));
		return FALSE;
	}

	*Epoch = DaysFromCivil ((INT64) Y, Month, Day) * 86400 + (INT64) Hour * 3600 + (INT64) Min * 60 + Sec;
	return TRUE;
}

STATIC
BOOLEAN
HwWriteEpoch (
  IN INT64  Epoch
  )
{
	UINT8   Ctl;
	INT64   Days;
	INT64   Year;
	UINT8   Month, Day, Hour, Min, Sec, Weekday;
	UINT32  OfDay;

	if (!PmicReady ()) {
		return FALSE;
	}

	Ctl = i2c_recv_byte (I2C_5, RTC_I2C_ADDR, RTC_CONTROL_REG);
	if ((Ctl & (RTC_BIN_FORMAT | RTC_24H)) != (RTC_BIN_FORMAT | RTC_24H)) {
		// Only touch the RTC in the format Horizon/hekate use (binary, 24 h).
		return FALSE;
	}

	Days  = Epoch / 86400;
	OfDay = (UINT32) (Epoch % 86400);
	CivilFromDays (Days, &Year, &Month, &Day);
	if (Year < 2020 || Year > 2099) {
		return FALSE;
	}
	Hour = (UINT8) (OfDay / 3600);
	Min  = (UINT8) ((OfDay % 3600) / 60);
	Sec  = (UINT8) (OfDay % 60);
	// 1970-01-01 was a Thursday. Weekday register: bit0 = Monday .. bit6 = Sunday (hekate decoding).
	Weekday = (UINT8) (1U << ((UINT32) ((Days + 3) % 7 + 7) % 7));

	i2c_send_byte (I2C_5, RTC_I2C_ADDR, RTC_SEC_REG,     Sec);
	i2c_send_byte (I2C_5, RTC_I2C_ADDR, RTC_MIN_REG,     Min);
	i2c_send_byte (I2C_5, RTC_I2C_ADDR, RTC_HOUR_REG,    Hour);
	i2c_send_byte (I2C_5, RTC_I2C_ADDR, RTC_WEEKDAY_REG, Weekday);
	i2c_send_byte (I2C_5, RTC_I2C_ADDR, RTC_DAY_REG,     Day);
	i2c_send_byte (I2C_5, RTC_I2C_ADDR, RTC_MONTH_REG,   Month);
	i2c_send_byte (I2C_5, RTC_I2C_ADDR, RTC_YEAR_REG,    (UINT8) (Year - 2000));
	// Push the registers into the running RTC.
	i2c_send_byte (I2C_5, RTC_I2C_ADDR, RTC_UPDATE0_REG, RTC_WRITE_UPDATE);
	MicroSecondDelay (16000);
	return TRUE;
}

STATIC
VOID
SoftSet (
  IN INT64  Epoch
  )
{
	mBaseEpoch   = Epoch;
	mBaseCounter = GetPerformanceCounter ();
}

/**
  Initialise the soft clock on first use. While boot services are available and the PMIC driver
  has not appeared yet we retry (a few times) instead of freezing on the build-date fallback.
**/
STATIC
VOID
EnsureSynced (
  VOID
  )
{
	INT64  Epoch;

	if (mSynced) {
		return;
	}

	if (!EfiAtRuntime () && mSyncTries < 20) {
		mSyncTries++;
		if (HwReadEpoch (&Epoch)) {
			SoftSet (Epoch);
			mHwValid = TRUE;
			mSynced  = TRUE;
			return;
		}
		if (PmicReady ()) {
			// PMIC is up but the RTC value is unusable: stop retrying, use the build date.
			mSyncTries = 20;
		} else if (mSyncTries < 20) {
			// PMIC not yet up: run from the build date for now but try again next call.
			if (!mFallbackSet) {
				SoftSet (BuildDateEpochSeconds ());
				mFallbackSet = TRUE;
			}
			return;
		}
	}

	if (!mFallbackSet) {
		SoftSet (BuildDateEpochSeconds ());
		mFallbackSet = TRUE;
	}
	mSynced = TRUE;
}

/**
Returns the current time and date information.
**/
EFI_STATUS
EFIAPI
LibGetTime(
	OUT EFI_TIME                *Time,
	OUT  EFI_TIME_CAPABILITIES  *Capabilities
)
{
	UINT32  Freq = ArmGenericTimerGetTimerFreq();
	UINT64  Now;
	INT64   Seconds;
	INT64   Days;
	UINT32  OfDay;
	INT64   Year;

	if (Time == NULL) {
		return EFI_INVALID_PARAMETER;
	}
	if (Freq == 0) {
		return EFI_DEVICE_ERROR;
	}

	EnsureSynced ();

	if (Capabilities) {
		Capabilities->Accuracy = 0;
		Capabilities->Resolution = 1;
		Capabilities->SetsToZero = FALSE;
	}

	Now     = GetPerformanceCounter ();
	Seconds = mBaseEpoch + (INT64) ((Now - mBaseCounter) / Freq);
	Days    = Seconds / 86400;
	OfDay   = (UINT32) (Seconds % 86400);

	CivilFromDays(Days, &Year, &Time->Month, &Time->Day);
	Time->Year   = (UINT16) Year;
	Time->Hour   = (UINT8) (OfDay / 3600);
	Time->Minute = (UINT8) ((OfDay % 3600) / 60);
	Time->Second = (UINT8) (OfDay % 60);
	Time->Nanosecond = 0;
	Time->TimeZone   = 0;   // the PMIC RTC holds UTC
	Time->Daylight   = 0;

	return EFI_SUCCESS;
}

/**
Sets the current time and date. Before ExitBootServices the hardware RTC is written as well.
**/
EFI_STATUS
EFIAPI
LibSetTime(
	IN EFI_TIME                *Time
)
{
	INT64  Epoch;
	INT64  Zone;

	if (Time == NULL || Time->Year < 2020 || Time->Year > 2099 || Time->Month < 1 || Time->Month > 12 ||
	    Time->Day < 1 || Time->Day > 31 || Time->Hour > 23 || Time->Minute > 59 || Time->Second > 59) {
		return EFI_INVALID_PARAMETER;
	}

	EnsureSynced ();

	// EFI_TIME is local time; TimeZone is minutes WEST of UTC (or EFI_UNSPECIFIED_TIMEZONE = already UTC/unknown).
	Zone = (Time->TimeZone == EFI_UNSPECIFIED_TIMEZONE) ? 0 : (INT64) Time->TimeZone * 60;
	Epoch = DaysFromCivil (Time->Year, Time->Month, Time->Day) * 86400 +
	        (INT64) Time->Hour * 3600 + (INT64) Time->Minute * 60 + Time->Second + Zone;

	SoftSet (Epoch);
	mSynced = TRUE;

	if (!EfiAtRuntime ()) {
		if (HwWriteEpoch (Epoch)) {
			mHwValid = TRUE;
		}
	}
	return EFI_SUCCESS;
}

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

EFI_STATUS
EFIAPI
LibSetWakeupTime(
	IN BOOLEAN      Enabled,
	OUT EFI_TIME    *Time
)
{
	return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
LibRtcInitialize(
	IN EFI_HANDLE                            ImageHandle,
	IN EFI_SYSTEM_TABLE                      *SystemTable
)
{
	return EFI_SUCCESS;
}

VOID
EFIAPI
LibRtcVirtualNotifyEvent(
	IN EFI_EVENT        Event,
	IN VOID             *Context
)
{
	return;
}
