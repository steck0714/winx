/*
 * SwoaHello: stage-0 test driver for Windows on the Nintendo Switch.
 * A non-PnP KMDF driver that only proves the toolchain (clang-cl + lld-link),
 * KMDF linkage and test signing work: on load it writes
 *   HKLM\SYSTEM\CurrentControlSet\Services\SwoaHello\Parameters\Loaded = 1
 *   ...\KmdfVersion = <major*100+minor of the framework actually bound>
 * and it unloads cleanly.
 */
#include <ntddk.h>
#include <wdf.h>

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_UNLOAD SwoaHelloUnload;

static VOID SwoaHelloWriteValue(WDFKEY Key, PCWSTR Name, ULONG Value)
{
    UNICODE_STRING ValueName;
    RtlInitUnicodeString(&ValueName, Name);
    (VOID)WdfRegistryAssignULong(Key, &ValueName, Value);
}

VOID SwoaHelloUnload(WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "SwoaHello: unload\n");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    WDFDRIVER Driver;
    WDFKEY Key;
    NTSTATUS Status;

    WDF_DRIVER_CONFIG_INIT(&Config, WDF_NO_EVENT_CALLBACK);
    Config.DriverInitFlags |= WdfDriverInitNonPnpDriver;
    Config.EvtDriverUnload = SwoaHelloUnload;

    Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, &Driver);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Status = WdfDriverOpenParametersRegistryKey(Driver, KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &Key);
    if (NT_SUCCESS(Status)) {
        SwoaHelloWriteValue(Key, L"Loaded", 1);
        SwoaHelloWriteValue(Key, L"KmdfVersion", KMDF_VERSION_MAJOR * 100 + KMDF_VERSION_MINOR);
        WdfRegistryClose(Key);
    }
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "SwoaHello: loaded (registry 0x%08x)\n", Status);
    return STATUS_SUCCESS;
}
