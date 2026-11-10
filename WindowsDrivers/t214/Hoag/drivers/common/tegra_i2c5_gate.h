/* SPDX-License-Identifier: GPL-2.0-only
 * Serialize the boot SD miniport's LDO2 operation with the I2C5 SPB driver.
 * \Device exists during boot-driver initialization; \BaseNamedObjects does not.
 */
#pragma once
static PKEVENT TegraI2c5OpenGate(PHANDLE Handle)
{
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"\\Device\\TegraI2c5GateV1");
    return IoCreateSynchronizationEvent(&name, Handle);
}
static NTSTATUS TegraI2c5Acquire(PKEVENT Gate)
{
    LARGE_INTEGER limit;
    if (Gate == NULL) return STATUS_SUCCESS; /* Other I2C controllers. */
    limit.QuadPart = -10000000LL;
    NTSTATUS status = KeWaitForSingleObject(Gate, Executive, KernelMode, FALSE, &limit);
    return status == STATUS_SUCCESS ? STATUS_SUCCESS : STATUS_IO_TIMEOUT;
}
static VOID TegraI2c5Release(PKEVENT Gate)
{
    if (Gate != NULL) KeSetEvent(Gate, IO_NO_INCREMENT, FALSE);
}
