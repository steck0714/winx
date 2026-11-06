/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SWIFI_REPORT_H
#define SWIFI_REPORT_H
#include <ntddk.h>
#include <wdf.h>

typedef enum SWIFI_BOOT_STEP {
    SwifiDeviceAdd = 1, SwifiPrepare, SwifiEnterD0, SwifiRadioOnline, SwifiStationStart, SwifiRuntimeFailure, SwifiQuiesce
} SWIFI_BOOT_STEP;

typedef enum SWIFI_HW_STEP {
    SwifiHwIsolation = 1, SwifiHwInterrupt, SwifiHwMemory, SwifiHwDma,
    SwifiHwEndpoint, SwifiHwIntx, SwifiHwFirmware, SwifiHwRings, SwifiHwControl
} SWIFI_HW_STEP;

typedef enum SWIFI_RX_CASE {
    SwifiRxEtherBeforeKey, SwifiRxEtherUncertain, SwifiRxNativeClear,
    SwifiRxNativeProtected, SwifiRxMetaSeen, SwifiRxRssiSeen, SwifiRxMetaInvalid,
    SwifiRxCases
} SWIFI_RX_CASE;

typedef struct SWIFI_REPORT_DATA {
    ULONG Session, Step, FailedStep, HardwareStep;
    NTSTATUS HardwareStatus;
    NTSTATUS StepStatus, FailureStatus, ScanStatus, JoinStatus;
    ULONG Scans, Attempts, KeyInstalls, KeyRemovals, KeyMask;
    ULONG FilterFlags, FilterCount, FilterMode, IrqSeen, RingSeen;
    NTSTATUS FilterStatus;
    ULONG Count[SwifiRxCases];
} SWIFI_REPORT_DATA;

typedef struct SWIFI_REPORT {
    KSPIN_LOCK Lock;
    WDFWORKITEM Worker;
    BOOLEAN Scheduled, Dirty;
    SWIFI_REPORT_DATA Data;
} SWIFI_REPORT;

VOID SwifiReportOpen(WDFDEVICE Device, SWIFI_REPORT *Report);
VOID SwifiReportStep(SWIFI_REPORT *R, SWIFI_BOOT_STEP Step, NTSTATUS Status);
VOID SwifiReportIo(SWIFI_REPORT *R, LONG IrqSeen, LONG RingSeen);
VOID SwifiReportHardware(SWIFI_REPORT *R, SWIFI_HW_STEP Step, NTSTATUS Status);
VOID SwifiReportScan(SWIFI_REPORT *R, NTSTATUS Status);
VOID SwifiReportJoin(SWIFI_REPORT *R, NTSTATUS Status);
VOID SwifiReportFilter(SWIFI_REPORT *R, ULONG Flags, ULONG Count, ULONG Mode, NTSTATUS Status);
VOID SwifiReportKeys(SWIFI_REPORT *R, ULONG Mask, ULONG Adds, ULONG Removes);
VOID SwifiReportRx(SWIFI_REPORT *R, SWIFI_RX_CASE Case);
VOID SwifiReportFlush(SWIFI_REPORT *R);

#endif
