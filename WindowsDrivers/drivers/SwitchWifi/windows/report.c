/* SPDX-License-Identifier: GPL-2.0-only */
#include "report.h"
#include <ntstrsafe.h>

typedef struct REPORT_WORK { SWIFI_REPORT *Report; } REPORT_WORK;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(REPORT_WORK, ReportWork);

static VOID WriteReport(WDFWORKITEM Work)
{
    SWIFI_REPORT *R = ReportWork(Work)->Report;
    UNICODE_STRING Name;
    RtlInitUnicodeString(&Name, L"SwitchWifiReport");
    for (;;) {
        KIRQL Irql;
        KeAcquireSpinLock(&R->Lock,&Irql);
        if (!R->Dirty) {
            R->Scheduled = FALSE;
            KeReleaseSpinLock(&R->Lock,Irql);
            return;
        }
        R->Dirty = FALSE;
        SWIFI_REPORT_DATA D = R->Data;
        KeReleaseSpinLock(&R->Lock,Irql);
        WCHAR Text[384];
        NTSTATUS Status = RtlStringCchPrintfW(Text, RTL_NUMBER_OF(Text),
            L"v=3 session=%u step=%u/%08X failure=%u/%08X scan=%u/%08X join=%u/%08X "
            L"keys=%u add=%u del=%u eth_pre=%u eth_unknown=%u native_clear=%u "
            L"native_protected=%u meta=%u rssi=%u malformed=%u",
            D.Session,D.Step,(ULONG)D.StepStatus,D.FailedStep,(ULONG)D.FailureStatus,
            D.Scans,(ULONG)D.ScanStatus,D.Attempts,(ULONG)D.JoinStatus,D.KeyMask,
            D.KeyInstalls,D.KeyRemovals,D.Count[SwifiRxEtherBeforeKey],D.Count[SwifiRxEtherUncertain],
            D.Count[SwifiRxNativeClear],D.Count[SwifiRxNativeProtected],D.Count[SwifiRxMetaSeen],
            D.Count[SwifiRxRssiSeen],D.Count[SwifiRxMetaInvalid]);
        if (NT_SUCCESS(Status)) {
            UNICODE_STRING Value;
            RtlInitUnicodeString(&Value, Text);
            (VOID)WdfRegistryAssignUnicodeString(R->Key, &Name, &Value);
        }
    }
}

VOID SwifiReportOpen(WDFDEVICE Device, SWIFI_REPORT *R)
{
    WDF_OBJECT_ATTRIBUTES A;
    WDF_WORKITEM_CONFIG Config;
    KeInitializeSpinLock(&R->Lock);
    WDF_OBJECT_ATTRIBUTES_INIT(&A);
    A.ParentObject = Device;
    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DEVICE, KEY_SET_VALUE,
        &A, &R->Key))) return;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A, REPORT_WORK);
    A.ParentObject = Device;
    WDF_WORKITEM_CONFIG_INIT(&Config, WriteReport);
    Config.AutomaticSerialization = FALSE;
    if (NT_SUCCESS(WdfWorkItemCreate(&Config, &A, &R->Worker)))
        ReportWork(R->Worker)->Report = R;
}

static VOID ScheduleLocked(SWIFI_REPORT *R)
{
    R->Dirty = TRUE;
    if (R->Worker && !R->Scheduled) {
        R->Scheduled = TRUE;
        WdfWorkItemEnqueue(R->Worker);
    }
}

VOID SwifiReportStep(SWIFI_REPORT *R, SWIFI_BOOT_STEP Step, NTSTATUS Status)
{
    KIRQL Irql;
    KeAcquireSpinLock(&R->Lock,&Irql);
    if (Step == SwifiPrepare && Status == STATUS_PENDING) {
        ULONG Session = R->Data.Session+1;
        RtlZeroMemory(&R->Data,sizeof(R->Data));
        R->Data.Session = Session;
    }
    R->Data.Step = Step;
    R->Data.StepStatus = Status;
    if (!NT_SUCCESS(Status) && !R->Data.FailedStep) {
        R->Data.FailedStep = Step;
        R->Data.FailureStatus = Status;
    }
    ScheduleLocked(R);
    KeReleaseSpinLock(&R->Lock,Irql);
}

VOID SwifiReportScan(SWIFI_REPORT *R, NTSTATUS Status)
{
    KIRQL Irql;
    KeAcquireSpinLock(&R->Lock,&Irql);
    ++R->Data.Scans;
    R->Data.ScanStatus = Status;
    ScheduleLocked(R);
    KeReleaseSpinLock(&R->Lock,Irql);
}

VOID SwifiReportJoin(SWIFI_REPORT *R, NTSTATUS Status)
{
    KIRQL Irql;
    KeAcquireSpinLock(&R->Lock,&Irql);
    ++R->Data.Attempts;
    R->Data.JoinStatus = Status;
    ScheduleLocked(R);
    KeReleaseSpinLock(&R->Lock,Irql);
}

VOID SwifiReportKeys(SWIFI_REPORT *R, ULONG Mask, ULONG Adds, ULONG Removes)
{
    KIRQL Irql;
    KeAcquireSpinLock(&R->Lock,&Irql);
    if (R->Data.KeyMask != Mask || Adds || Removes) {
        R->Data.KeyMask = Mask;
        R->Data.KeyInstalls += Adds;
        R->Data.KeyRemovals += Removes;
        ScheduleLocked(R);
    }
    KeReleaseSpinLock(&R->Lock,Irql);
}

VOID SwifiReportRx(SWIFI_REPORT *R, SWIFI_RX_CASE Case)
{
    KIRQL Irql;
    KeAcquireSpinLock(&R->Lock,&Irql);
    /* Save the first observation immediately. Exact later counts are
     * included on the next transition/flush, without a disk write per RX. */
    if (++R->Data.Count[Case] == 1) ScheduleLocked(R);
    KeReleaseSpinLock(&R->Lock,Irql);
}

VOID SwifiReportFlush(SWIFI_REPORT *R)
{
    if (!R->Worker) return;
    KIRQL Irql;
    KeAcquireSpinLock(&R->Lock,&Irql);
    ScheduleLocked(R);
    KeReleaseSpinLock(&R->Lock,Irql);
    WdfWorkItemFlush(R->Worker);
}
