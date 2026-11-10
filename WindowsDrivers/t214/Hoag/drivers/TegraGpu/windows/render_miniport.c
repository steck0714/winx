/* SPDX-License-Identifier: MIT */
#include "adapter.h"
#include "reset_backend.h"

static VOID record_status(PCWSTR name, NTSTATUS status)
{
    ULONG value=(ULONG)status;
    (VOID)RtlWriteRegistryValue(RTL_REGISTRY_SERVICES,L"tegragpurender\\Parameters",
                              name,REG_DWORD,&value,sizeof(value));
}

static NTSTATUS start_device(PVOID context,DXGK_START_INFO *start,
    DXGKRNL_INTERFACE *dxgk,ULONG *sources,ULONG *children)
{
    TG_GPU_ADAPTER *a=context;
    NTSTATUS status=TgGpuDdiStartDevice(context,start,dxgk,sources,children);
    record_status(L"ResourceStatus",status);
    if (!NT_SUCCESS(status)) return status;

    /* VidMm can exercise paging before the NVK command ABI is ready. */
    a->Engine.PagingOnly=TRUE;
    status=TgGpuStartPlatformAdapter(a);
    record_status(L"HardwareStatus",status);
    return status;
}

static NTSTATUS set_power(PVOID context,ULONG uid,DEVICE_POWER_STATE state,POWER_ACTION action)
{
    TG_GPU_ADAPTER *a=context;
    UNREFERENCED_PARAMETER(action);
    PAGED_CODE();
    if (uid!=DISPLAY_ADAPTER_HW_ID || !a->EngineReady) return STATUS_SUCCESS;
    if (state==PowerDeviceD0) {
        return a->State==TgGpuAdapterSuspended ? TgGpuRestartAdapterHardware(a) : STATUS_SUCCESS;
    }
    if (a->State!=TgGpuAdapterReady && a->State!=TgGpuAdapterSuspended) return STATUS_SUCCESS;
    if (a->State==TgGpuAdapterSuspended && a->Hardware.Isolated) return STATUS_SUCCESS;
    NTSTATUS status=TgGpuSuspendBackend(&a->Engine,&a->Queue,&a->Hardware,&a->Worker);
    if (a->Engine.Resetting) {
        a->State=TgGpuAdapterSuspended;
        a->QueueReady=a->Queue.Engine==&a->Engine;
    }
    return status;
}

static BOOLEAN interrupt(PVOID context,ULONG message)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(message);
    /* The cold backend leaves hardware sources masked. The worker reports
     * verified completions through DxgkCbSynchronizeExecution and NotifyDpc. */
    return FALSE;
}

static NTSTATUS query_children(PVOID context,PDXGK_CHILD_DESCRIPTOR children,ULONG bytes)
{
    UNREFERENCED_PARAMETER(context);
    if (!children || bytes<sizeof(*children)) return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(children,sizeof(*children)); /* terminator; no display outputs */
    return STATUS_SUCCESS;
}

static NTSTATUS query_child_status(PVOID context,PDXGK_CHILD_STATUS child,BOOLEAN nondestructive)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(child);
    UNREFERENCED_PARAMETER(nondestructive);
    return STATUS_INVALID_PARAMETER; /* zero child UIDs */
}

static NTSTATUS query_device_descriptor(PVOID context,ULONG uid,PDXGK_DEVICE_DESCRIPTOR descriptor)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(uid);
    UNREFERENCED_PARAMETER(descriptor);
    return STATUS_GRAPHICS_CHILD_DESCRIPTOR_NOT_SUPPORTED;
}

static VOID reset_device(PVOID context)
{
    /* VGA reset has no work on this adapter. GPU recovery uses the TDR DDIs. */
    UNREFERENCED_PARAMETER(context);
}

static NTSTATUS get_node(HANDLE adapter,UINT node,DXGKARG_GETNODEMETADATA *metadata)
{
    if (!adapter || node || !metadata) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(metadata,sizeof(*metadata));
    metadata->EngineType=DXGK_ENGINE_TYPE_3D;
    metadata->GpuMmuSupported=TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS dispatch_io(PVOID context,ULONG source,PVIDEO_REQUEST_PACKET request)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(source);
    UNREFERENCED_PARAMETER(request);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS query_interface(PVOID context,PQUERY_INTERFACE request)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(request);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS APIENTRY describe_allocation(HANDLE adapter,DXGKARG_DESCRIBEALLOCATION *args)
{
    UNREFERENCED_PARAMETER(adapter);
    UNREFERENCED_PARAMETER(args);
    /* All supported allocations are opaque buffers without display modes. */
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS APIENTRY standard_allocation(HANDLE adapter,DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *args)
{
    UNREFERENCED_PARAMETER(adapter);
    UNREFERENCED_PARAMETER(args);
    /* No standard display, GDI or virtual GPU surfaces. */
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS APIENTRY calibrate_clock(HANDLE adapter,UINT32 node,UINT32 engine,
    DXGKARG_CALIBRATEGPUCLOCK *args)
{
    UNREFERENCED_PARAMETER(adapter);
    UNREFERENCED_PARAMETER(node);
    UNREFERENCED_PARAMETER(engine);
    UNREFERENCED_PARAMETER(args);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS APIENTRY format_history(HANDLE context,DXGKARG_FORMATHISTORYBUFFER *args)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(args);
    return STATUS_NOT_SUPPORTED;
}

static VOID APIENTRY set_stable_power(HANDLE adapter,const DXGKARG_SETSTABLEPOWERSTATE *args)
{
    /* The powered GPU already runs at a fixed clock. */
    UNREFERENCED_PARAMETER(adapter);
    UNREFERENCED_PARAMETER(args);
}

static NTSTATUS APIENTRY set_vm_data(HANDLE adapter,const DXGKARG_SETVIRTUALMACHINEDATA *args)
{
    UNREFERENCED_PARAMETER(adapter);
    UNREFERENCED_PARAMETER(args);
    return STATUS_NOT_SUPPORTED;
}

static VOID unload(VOID) {}

DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT object,PUNICODE_STRING path)
{
    record_status(L"InitializeStatus",STATUS_PENDING);
    DRIVER_INITIALIZATION_DATA d={0};
    d.Version=DXGKDDI_INTERFACE_VERSION;
    TgGpuRegisterAdapterDdis(&d);
    d.DxgkDdiStartDevice=start_device;
    d.DxgkDdiSetPowerState=set_power;
    d.DxgkDdiInterruptRoutine=interrupt;
    d.DxgkDdiQueryChildRelations=query_children;
    d.DxgkDdiQueryChildStatus=query_child_status;
    d.DxgkDdiQueryDeviceDescriptor=query_device_descriptor;
    d.DxgkDdiResetDevice=reset_device;
    d.DxgkDdiGetNodeMetadata=get_node;
    d.DxgkDdiDispatchIoRequest=dispatch_io;
    d.DxgkDdiQueryInterface=query_interface;
    d.DxgkDdiUnload=unload;
    d.DxgkDdiQueryCurrentFence=NULL; /* MCDM uses scheduler fence notifications. */
    d.DxgkDdiDescribeAllocation=describe_allocation;
    d.DxgkDdiGetStandardAllocationDriverData=standard_allocation;
    d.DxgkDdiCalibrateGpuClock=calibrate_clock;
    d.DxgkDdiFormatHistoryBuffer=format_history;
    d.DxgkDdiSetStablePowerState=set_stable_power;
    d.DxgkDdiSetVirtualMachineData=set_vm_data;
    NTSTATUS status=DxgkInitialize(object,path,&d);
    record_status(L"InitializeStatus",status);
    return status;
}
