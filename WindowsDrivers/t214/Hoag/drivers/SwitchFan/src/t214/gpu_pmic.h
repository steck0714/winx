/* SPDX-License-Identifier: GPL-2.0-only */
#include "../../../TegraGpu/windows/pmic.h"

/* Vulkan-test only. Reuse the existing passive SPB owner. User-mode callers
 * cannot use this transport to program PMIC registers. EN_CTRL writes only
 * change M1's enable bit; CPU M4 and DDR M3 remain owned by their drivers.
 */
static NTSTATUS SwFanGpuPmic(PSWFAN_DEVICE Dev, WDFREQUEST Request, ULONG_PTR *Info)
{
    struct tg_gpu_pmic_request *In, *Out, Op;
    WDF_REQUEST_SEND_OPTIONS Options;
    WDF_MEMORY_DESCRIPTOR Memory;
    UCHAR Buffer[2], Readback;
    ULONG_PTR Bytes;
    NTSTATUS Status;
    if (WdfRequestGetRequestorMode(Request) != KernelMode) return STATUS_ACCESS_DENIED;
    Status = WdfRequestRetrieveInputBuffer(Request, sizeof(Op), (PVOID *)&In, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    Op = *In;
    Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(Op), (PVOID *)&Out, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    if (!Dev->IsLite || Dev->Stopping || !Dev->Pmic || !Dev->CpuConfigured)
        return STATUS_DEVICE_NOT_READY;
    if (!Op.write && Op.reg >= TG_GPU_PMIC_QUERY_ADDRESS) {
        if (Op.reg == TG_GPU_PMIC_QUERY_ADDRESS) Op.value = Dev->Config.PmicAddress;
        else if (Op.reg == TG_GPU_PMIC_QUERY_PLLP) Op.value = SwCpuRead(Dev, 0xa0);
        else if (Op.reg == TG_GPU_PMIC_QUERY_TEMP && Dev->TempValid) Op.value = Dev->LastTemp;
        else return STATUS_INVALID_PARAMETER;
    } else {
        if (Op.address != Dev->Config.PmicAddress || Op.write > 1 || Op.value > 255 ||
            (Op.reg != 0x06 && Op.reg != 0x23 && (Op.reg != 0x26 || Op.write)))
            return STATUS_INVALID_PARAMETER;
        if (Op.write) {
            Buffer[0] = (UCHAR)Op.reg; Buffer[1] = (UCHAR)Op.value;
            if (Op.reg == 0x23 && Op.value > 160) return STATUS_INVALID_PARAMETER;
            if (Op.reg == 0x06) {
                if ((Op.value & 1) && (!Dev->TempValid || Dev->LastTemp >= 80))
                    return STATUS_DEVICE_NOT_READY;
                Status = SwFanReadReg(Dev->Pmic, 0x06, &Readback);
                if (!NT_SUCCESS(Status)) return Status;
                Buffer[1] = (UCHAR)((Readback & ~1u) | (Op.value & 1));
            }
            WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, Buffer, sizeof(Buffer));
            WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
            Options.Timeout = WDF_REL_TIMEOUT_IN_MS(200);
            Bytes = 0;
            Status = WdfIoTargetSendWriteSynchronously(Dev->Pmic, NULL, &Memory, NULL, &Options, &Bytes);
            if (!NT_SUCCESS(Status)) return Status;
            if (Bytes != sizeof(Buffer)) return STATUS_DEVICE_PROTOCOL_ERROR;
        }
        Status = SwFanReadReg(Dev->Pmic, (UCHAR)Op.reg, &Readback);
        if (!NT_SUCCESS(Status)) return Status;
        if (Op.write && Readback != Buffer[1]) return STATUS_DEVICE_DATA_ERROR;
        Op.value = Readback;
    }
    *Out = Op; *Info = sizeof(Op);
    return STATUS_SUCCESS;
}
