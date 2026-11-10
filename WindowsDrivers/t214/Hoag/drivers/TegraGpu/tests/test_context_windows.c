/* SPDX-License-Identifier: MIT */
#include "../windows/context.h"
#include "../windows/work_queue.h"
#include "../windows/allocation.h"
#include "../windows/query.h"
#include "../windows/adapter.h"
#include "../windows/paging_dispatch.h"
#include "../windows/submit_ddi.h"
#include "../platform/t214_resources.h"
#include <ntimage.h>
static NTSTATUS SubmitModel(TG_GPU_CONTEXT_ENGINE *,const DXGKARG_SUBMITCOMMANDVIRTUAL *);

static ULONG_PTR image_cookie=0x00002b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used={
    .Size=sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64),.SecurityCookie=(ULONGLONG)&image_cookie
};
__declspec(dllimport) void __stdcall ExitProcess(ULONG);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(ULONG);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE,const void *,ULONG,ULONG *,void *);
#define CHECK(x) do { if (!(x)) ExitProcess(__LINE__); } while (0)
void *memset(void *p,int value,size_t n)
{ UCHAR *d=p; while(n--) *d++=(UCHAR)value; return p; }
void *memcpy(void *p,const void *source,size_t n)
{ UCHAR *d=p;const UCHAR *s=source;while(n--) *d++=*s++;return p; }

/* Real WDK types and backend code, with explicit OS/GPU substitutes. Nothing
 * here allocates through live Dxgkrnl or proves physical GPU completion.
 */
static _Alignas(PAGE_SIZE) UCHAR pools[64][TG_GPU_SUBMIT_MAX_BYTES+PAGE_SIZE], memory[64][TG_GPU_TABLE_BYTES];
static _Alignas(PAGE_SIZE) UCHAR graphics_memory[4][TG_GPU_CONTEXT_PRIVATE_BYTES];
static BOOLEAN graphics_used[4];
static BOOLEAN pool_used[64], memory_used[64], fail_pool, fail_allocate, fail_free;
static HANDLE fail_memory_handle;
static UINT pool_fail_after=UINT32_MAX,memory_fail_after=UINT32_MAX;
static _Alignas(PAGE_SIZE) UCHAR adapter_table[TG_GPU_ADAPTER_TABLE_BYTES];
static _Alignas(PAGE_SIZE) UCHAR paging_tables[2*TG_GPU_TABLE_BYTES];
static BOOLEAN adapter_table_used,adapter_table_fail_free;
static BOOLEAN fail_sync, in_sync;
static BOOLEAN fail_reserve;
static UINT interrupts, dpcs, queues, frees, expected_fence,recovery_interrupts;
static UINT expected_driver_fence;
static UINT preemption_interrupts,expected_preemption,expected_preempt_completed;
static TG_GPU_CONTEXT_ENGINE *notify_engine;
static TG_GPU_QUEUE_ENTRY *reuse_private;
static TG_GPU_CONTEXT *notify_context;
static UINT notify_references;
static KIRQL level;
static VOID NTAPI InitializeLock(PKSPIN_LOCK lock) { *lock=0; }
VOID (NTAPI * const __imp_KeInitializeSpinLock)(PKSPIN_LOCK)=InitializeLock;
static VOID NTAPI InitializeEvent(PRKEVENT event,EVENT_TYPE type,BOOLEAN state)
{ CHECK(event && type==SynchronizationEvent);event->Header.SignalState=state; }
static LONG NTAPI SetEvent(PRKEVENT event,KPRIORITY priority,BOOLEAN wait)
{ CHECK(event && !priority && !wait);event->Header.SignalState=1;return 0; }
VOID (NTAPI * const __imp_KeInitializeEvent)(PRKEVENT,EVENT_TYPE,BOOLEAN)=InitializeEvent;
LONG (NTAPI * const __imp_KeSetEvent)(PRKEVENT,KPRIORITY,BOOLEAN)=SetEvent;
static VOID NTAPI InitializeMutex(PRKMUTEX m,ULONG rank)
{CHECK(m && !rank);m->Header.SignalState=1;}
static NTSTATUS NTAPI WaitSingle(PVOID object,KWAIT_REASON reason,KPROCESSOR_MODE mode,BOOLEAN alertable,PLARGE_INTEGER timeout)
{
    PKMUTEX m=object;
    CHECK(m && reason==Executive && mode==KernelMode && !alertable && !timeout && level==PASSIVE_LEVEL);
    CHECK(m->Header.SignalState<=1);--m->Header.SignalState;return STATUS_SUCCESS;
}
static LONG NTAPI ReleaseMutex(PRKMUTEX m,BOOLEAN wait)
{LONG old=m->Header.SignalState;CHECK(!wait && old<=0 && level==PASSIVE_LEVEL);++m->Header.SignalState;return old;}
VOID (NTAPI * const __imp_KeInitializeMutex)(PRKMUTEX,ULONG)=InitializeMutex;
NTSTATUS (NTAPI * const __imp_KeWaitForSingleObject)(PVOID,KWAIT_REASON,KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER)=WaitSingle;
LONG (NTAPI * const __imp_KeReleaseMutex)(PRKMUTEX,BOOLEAN)=ReleaseMutex;
/* Resource-only adapter tests must never open a bridge or start a thread. */
static NTSTATUS NTAPI UnexpectedClose(HANDLE h){(void)h;CHECK(FALSE);return STATUS_UNSUCCESSFUL;}
static NTSTATUS NTAPI UnexpectedJoin(HANDLE h,BOOLEAN a,PLARGE_INTEGER t)
{(void)h;(void)a;(void)t;CHECK(FALSE);return STATUS_UNSUCCESSFUL;}
NTSTATUS (NTAPI * const __imp_ZwClose)(HANDLE)=UnexpectedClose;
NTSTATUS (NTAPI * const __imp_ZwWaitForSingleObject)(HANDLE,BOOLEAN,PLARGE_INTEGER)=UnexpectedJoin;
#include "unexpected_platform.h"
static PVOID NTAPI Pool(POOL_FLAGS flags,SIZE_T bytes,ULONG tag)
{
    UINT i;
    CHECK(flags==POOL_FLAG_NON_PAGED && bytes<=sizeof(pools[0]) && tag!=0 &&
        (level==PASSIVE_LEVEL || (level==DISPATCH_LEVEL && tag==TG_GPU_QUEUE_TAG)));
    if(fail_pool) return NULL;
    if(pool_fail_after!=UINT32_MAX) {
        if(!pool_fail_after) return NULL;
        --pool_fail_after;
    }
    for(i=0;i<64;++i) if(!pool_used[i]) {
        pool_used[i]=TRUE; memset(pools[i],0,sizeof(pools[i])); return pools[i];
    }
    return NULL;
}
static VOID NTAPI FreePool(PVOID p,ULONG tag)
{
    UINT i; CHECK(tag!=0 && (level==PASSIVE_LEVEL || (level==DISPATCH_LEVEL && tag==TG_GPU_QUEUE_TAG)));
    for(i=0;i<64;++i) if(p==pools[i]) { CHECK(pool_used[i]);pool_used[i]=FALSE;return; }
    CHECK(FALSE);
}
PVOID (NTAPI * const __imp_ExAllocatePool2)(POOL_FLAGS,SIZE_T,ULONG)=Pool;
VOID (NTAPI * const __imp_ExFreePoolWithTag)(PVOID,ULONG)=FreePool;
static KIRQL NTAPI Lock(PKSPIN_LOCK lock)
{
    KIRQL previous=level;CHECK(!*lock && !in_sync);*lock=1;level=DISPATCH_LEVEL;return previous;
}
static VOID NTAPI Unlock(PKSPIN_LOCK lock,KIRQL previous)
{ CHECK(*lock==1 && level==DISPATCH_LEVEL);*lock=0;level=previous; }
KIRQL (NTAPI * const __imp_KeAcquireSpinLockRaiseToDpc)(PKSPIN_LOCK)=Lock;
VOID (NTAPI * const __imp_KeReleaseSpinLock)(PKSPIN_LOCK,KIRQL)=Unlock;
static PHYSICAL_ADDRESS NTAPI Physical(PVOID address)
{
    ULONG_PTR offset=(ULONG_PTR)address-(ULONG_PTR)memory;
    PHYSICAL_ADDRESS p;
    ULONG_PTR table_offset=(ULONG_PTR)address-(ULONG_PTR)adapter_table;
    ULONG_PTR graphics_offset=(ULONG_PTR)address-(ULONG_PTR)graphics_memory;
    ULONG_PTR paging_offset=(ULONG_PTR)address-(ULONG_PTR)paging_tables;
    if(paging_offset<sizeof(paging_tables)){p.QuadPart=0x94000000+paging_offset;return p;}
    if(table_offset<sizeof(adapter_table)){p.QuadPart=0x92000000+table_offset;return p;}
    if(graphics_offset<sizeof(graphics_memory)) {
        p.QuadPart=0x93000000+(graphics_offset/TG_GPU_CONTEXT_PRIVATE_BYTES)*0x100000+
            graphics_offset%TG_GPU_CONTEXT_PRIVATE_BYTES;return p;
    }
    CHECK(offset<sizeof(memory));p.QuadPart=0x91000000+offset;return p;
}
PHYSICAL_ADDRESS (NTAPI * const __imp_MmGetPhysicalAddress)(PVOID)=Physical;
static NTSTATUS NTAPI Allocate(HANDLE device,DXGKARGCB_ALLOCATECONTIGUOUSMEMORY *a)
{
    UINT i;CHECK(device==(HANDLE)1 && level==PASSIVE_LEVEL);
    CHECK((a->NumberOfBytes==PAGE_SIZE || a->NumberOfBytes==TG_GPU_COMMAND_BYTES ||
           a->NumberOfBytes==TG_GPU_TABLE_BYTES || a->NumberOfBytes==TG_GPU_ADAPTER_TABLE_BYTES ||
           a->NumberOfBytes==TG_GPU_CONTEXT_PRIVATE_BYTES) && a->CacheType==DXGK_MEMORY_CACHING_TYPE_NON_CACHED);
    CHECK(a->LowestAcceptableAddress.QuadPart==0x80000000 && a->HighestAcceptableAddress.QuadPart==0xffffffff);
    if(fail_allocate) return STATUS_INSUFFICIENT_RESOURCES;
    if(memory_fail_after!=UINT32_MAX){if(!memory_fail_after)return STATUS_INSUFFICIENT_RESOURCES;--memory_fail_after;}
    if(a->NumberOfBytes==TG_GPU_CONTEXT_PRIVATE_BYTES) {
        for(i=0;i<4;++i)if(!graphics_used[i]) {
            graphics_used[i]=TRUE;a->hMemoryHandle=(HANDLE)(ULONG_PTR)(512+i);
            a->pMemory=graphics_memory[i];return STATUS_SUCCESS;
        }
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if(a->NumberOfBytes==TG_GPU_ADAPTER_TABLE_BYTES){
        CHECK(!adapter_table_used);adapter_table_used=TRUE;
        a->hMemoryHandle=(HANDLE)256;a->pMemory=adapter_table;return STATUS_SUCCESS;
    }
    for(i=0;i<64;++i) if(!memory_used[i]) {
        memory_used[i]=TRUE;a->hMemoryHandle=(HANDLE)(ULONG_PTR)(i+1);a->pMemory=memory[i];return STATUS_SUCCESS;
    }
    return STATUS_INSUFFICIENT_RESOURCES;
}
static NTSTATUS NTAPI Free(HANDLE device,const DXGKARGCB_FREECONTIGUOUSMEMORY *a)
{
    UINT i=(UINT)(ULONG_PTR)a->hMemoryHandle-1;
    if((ULONG_PTR)a->hMemoryHandle>=512 && (ULONG_PTR)a->hMemoryHandle<516) {
        i=(UINT)(ULONG_PTR)a->hMemoryHandle-512;
        CHECK(device==(HANDLE)1 && level==PASSIVE_LEVEL && graphics_used[i]);
        if(fail_free)return STATUS_UNSUCCESSFUL;
        graphics_used[i]=FALSE;++frees;return STATUS_SUCCESS;
    }
    if(a->hMemoryHandle==(HANDLE)256){
        CHECK(device==(HANDLE)1 && level==PASSIVE_LEVEL && adapter_table_used);
        if(fail_free || adapter_table_fail_free)return STATUS_UNSUCCESSFUL;
        adapter_table_used=FALSE;++frees;return STATUS_SUCCESS;
    }
    CHECK(device==(HANDLE)1 && level==PASSIVE_LEVEL && i<64 && memory_used[i]);
    if(fail_free || a->hMemoryHandle==fail_memory_handle) return STATUS_UNSUCCESSFUL;
    memory_used[i]=FALSE;++frees;return STATUS_SUCCESS;
}
static NTSTATUS NTAPI Synchronize(HANDLE device,PKSYNCHRONIZE_ROUTINE routine,
                                  PVOID context,ULONG message,PBOOLEAN result)
{
    CHECK(device==(HANDLE)1 && !message && level<=DISPATCH_LEVEL);
    if(fail_sync) return STATUS_UNSUCCESSFUL;
    in_sync=TRUE;*result=routine(context);in_sync=FALSE;return STATUS_SUCCESS;
}
static VOID NTAPI Interrupt(HANDLE device,DXGKARGCB_NOTIFY_INTERRUPT_DATA *data)
{
    CHECK(device==(HANDLE)1 && in_sync);
    if(data->InterruptType==DXGK_INTERRUPT_GPU_ENGINE_TIMEOUT) {
        CHECK(!data->GpuEngineTimeout.NodeOrdinal && !data->GpuEngineTimeout.EngineOrdinal);
        ++recovery_interrupts;return;
    }
    if(data->InterruptType==DXGK_INTERRUPT_DMA_PREEMPTED) {
        CHECK(data->DmaPreempted.PreemptionFenceId==expected_preemption);
        CHECK(data->DmaPreempted.LastCompletedFenceId==expected_preempt_completed);
        CHECK(!data->DmaPreempted.NodeOrdinal && !data->DmaPreempted.EngineOrdinal);
        CHECK(notify_engine && notify_engine->LastCompletedFence==expected_preempt_completed);
        CHECK(!notify_engine->Active && !notify_engine->Channel.referenced);
        if(notify_context)CHECK(notify_context->References==notify_references);
        ++preemption_interrupts;return;
    }
    CHECK(data->InterruptType==DXGK_INTERRUPT_DMA_COMPLETED);
    CHECK(data->DmaCompleted.NodeOrdinal==0 && data->DmaCompleted.EngineOrdinal==0);
    CHECK(data->DmaCompleted.SubmissionFenceId==expected_fence);++interrupts;
    if(notify_engine)CHECK(notify_engine->LastCompletedFence==expected_driver_fence);
    if(notify_context) CHECK(notify_context->References==notify_references);
    if(reuse_private) {
        /* Model immediate OS reuse after fence notification. The worker must
         * not retain or dereference anything inside this buffer afterward.
         */
        memset(reuse_private,0xa5,sizeof(*reuse_private));reuse_private=NULL;
    }
}
static BOOLEAN NTAPI Queue(HANDLE device)
{ CHECK(device==(HANDLE)1 && in_sync);++queues;return FALSE; /* DPC already queued */ }
static VOID NTAPI Dpc(HANDLE device)
{ CHECK(device==(HANDLE)1 && !in_sync);++dpcs; }
static NTSTATUS NTAPI Reserve(HANDLE device,DXGKARGCB_RESERVEGPUVIRTUALADDRESSRANGE *a)
{
    CHECK(device==(HANDLE)1 && level==PASSIVE_LEVEL && a->hDxgkProcess==(HANDLE)100);
    CHECK(a->SizeInBytes==TG_GPU_INTERNAL_COVERAGE && a->Alignment==TG_GPU_INTERNAL_COVERAGE);
    CHECK(!a->Flags && !a->BaseAddress);
    if(fail_reserve) return STATUS_INSUFFICIENT_RESOURCES;
    a->StartVirtualAddress=TG_GPU_INTERNAL_COVERAGE;return STATUS_SUCCESS;
}

/* Model a Dxgkrnl handle lookup and track references across the open DDI. */
static HANDLE os_allocation[8];
static UINT os_references[8],handle_acquires,handle_releases;
static VOID *APIENTRY AcquireHandle(const DXGKARGCB_GETHANDLEDATA *a,DXGKARG_RELEASE_HANDLE *release)
{
    CHECK(a && release && level==PASSIVE_LEVEL && a->Type==DXGK_HANDLE_ALLOCATION && !a->Flags.Value);
    *release=NULL;
    if(!a->hObject || a->hObject>=8 || !os_allocation[a->hObject]) return NULL;
    ++os_references[a->hObject];++handle_acquires;
    *release=(HANDLE)(ULONG_PTR)a->hObject;return os_allocation[a->hObject];
}
static VOID APIENTRY ReleaseHandle(const DXGKARGCB_RELEASEHANDLEDATA a)
{
    UINT id=(UINT)(ULONG_PTR)a.ReleaseHandle;
    CHECK(level==PASSIVE_LEVEL && a.Type==DXGK_HANDLE_ALLOCATION && id>0 && id<8 && os_references[id]);
    --os_references[id];++handle_releases;
}
static UINT PoolsInUse(void)
{ UINT i,count=0;for(i=0;i<64;++i) count+=pool_used[i]!=FALSE;return count; }

static void TestAllocations(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_CONTEXT_ENGINE *other,
                            DRIVER_INITIALIZATION_DATA *ddi,TG_GPU_DEVICE *device)
{
    struct tg_gpu_allocation_request desc[2]={
        {TG_GPU_ALLOCATION_ABI,TG_GPU_ALLOCATION_CPU_VISIBLE,4097},
        {TG_GPU_ALLOCATION_ABI,0,65536}
    };
    DXGK_ALLOCATIONINFO info[2]={
        {.pPrivateDriverData=&desc[0],.PrivateDriverDataSize=sizeof(desc[0])},
        {.pPrivateDriverData=&desc[1],.PrivateDriverDataSize=sizeof(desc[1])}
    };
    DXGKARG_CREATEALLOCATION create={.NumAllocations=2,.pAllocationInfo=info,.Flags.Resource=1};
    DXGKARG_DESTROYALLOCATION destroy={0};
    DXGK_OPENALLOCATIONINFO oi[2]={{.hAllocation=1},{.hAllocation=2}},oi2={.hAllocation=1};
    DXGKARG_OPENALLOCATION open={.NumAllocations=2,.pOpenAllocation=oi,.Flags.Create=1,
        .SubresourceIndex=UINT32_MAX}; /* unused for opaque GPU buffers */
    DXGKARG_OPENALLOCATION open2={.NumAllocations=1,.pOpenAllocation=&oi2,.Flags.ReadOnly=1};
    DXGKARG_CLOSEALLOCATION close={0};
    DXGKARG_CREATEDEVICE second={.hDevice=(HANDLE)202,.hKmdProcess=device->Process};
    HANDLE resource,handles[2],opened[2],readonly;UINT before=PoolsInUse(),i;
    CHECK(NT_SUCCESS(ddi->DxgkDdiCreateDevice(e,&second)));
    ++before;
    desc[1].version=99;
    CHECK(ddi->DxgkDdiCreateAllocation(e,&create)==STATUS_GRAPHICS_DRIVER_MISMATCH);
    CHECK(!info[0].hAllocation && !info[1].hAllocation && !create.hResource && PoolsInUse()==before);
    desc[1].version=TG_GPU_ALLOCATION_ABI;
    desc[1].bytes=UINT64_MAX;
    CHECK(ddi->DxgkDdiCreateAllocation(e,&create)==STATUS_INVALID_PARAMETER);
    desc[1].bytes=65536;info[0].FlagsWddm2.CreateInVpr=1;
    CHECK(ddi->DxgkDdiCreateAllocation(e,&create)==STATUS_INVALID_PARAMETER);
    info[0].FlagsWddm2.Value=0x80000800;
    for(i=0;i<3;++i) {
        pool_fail_after=i;
        CHECK(ddi->DxgkDdiCreateAllocation(e,&create)==STATUS_NO_MEMORY);
        CHECK(!info[0].hAllocation && !info[1].hAllocation && !create.hResource);
        CHECK(!e->AllocationCount && !e->AllocationBytes && !e->ResourceCount &&
              !e->Creating && PoolsInUse()==before);
    }
    pool_fail_after=UINT32_MAX;
    CHECK(NT_SUCCESS(ddi->DxgkDdiCreateAllocation(e,&create)));
    resource=create.hResource;handles[0]=info[0].hAllocation;handles[1]=info[1].hAllocation;
    CHECK(resource && handles[0] && handles[1] && e->AllocationCount==2 && e->ResourceCount==1);
    CHECK(info[0].Size==8192 && info[1].Size==65536 && info[0].Alignment==PAGE_SIZE);
    CHECK(e->AllocationBytes==8192+65536);
    CHECK(TgGpuValidatePagingAllocation(e,handles[0],PAGE_SIZE,PAGE_SIZE)==STATUS_SUCCESS);
    CHECK(TgGpuValidatePagingAllocation(e,handles[0],8192,1)==STATUS_INVALID_BUFFER_SIZE);
    CHECK(TgGpuValidatePagingAllocation(e,handles[0],UINT64_MAX,1)==STATUS_INVALID_BUFFER_SIZE);
    CHECK(TgGpuValidatePagingAllocation(other,handles[0],0,1)==STATUS_INVALID_PARAMETER);
    CHECK(info[0].PreferredSegment.SegmentId0==2 && info[0].SupportedWriteSegmentSet==2);
    CHECK(info[0].FlagsWddm2.Value==0x80000881 && info[1].FlagsWddm2.Value==0x80);
    CHECK(!info[0].FlagsWddm2.AccessedPhysically && !info[0].FlagsWddm2.Cached &&
          !info[0].FlagsWddm2.ExplicitResidencyNotification && info[0].AllocationPriority==D3DDDI_ALLOCATIONPRIORITY_NORMAL);
    /* Appending failure must not free or alter the existing resource. */
    pool_fail_after=1;
    CHECK(ddi->DxgkDdiCreateAllocation(e,&create)==STATUS_NO_MEMORY);
    CHECK(create.hResource==resource && e->AllocationCount==2 && e->ResourceCount==1 && PoolsInUse()==before+3);
    CHECK(e->AllocationBytes==8192+65536);
    pool_fail_after=UINT32_MAX;
    os_allocation[1]=handles[0];os_allocation[2]=handles[1];
    /* Each failure unwinds acquired OS references and all device-open objects. */
    pool_fail_after=1;
    CHECK(ddi->DxgkDdiOpenAllocation(device,&open)==STATUS_NO_MEMORY);
    CHECK(!oi[0].hDeviceSpecificAllocation && !oi[1].hDeviceSpecificAllocation);
    CHECK(!os_references[1] && !os_references[2] && !device->OpenAllocations);
    pool_fail_after=UINT32_MAX;oi[1].hAllocation=7;
    CHECK(ddi->DxgkDdiOpenAllocation(device,&open)==STATUS_INVALID_PARAMETER);
    CHECK(!os_references[1] && !device->OpenAllocations && PoolsInUse()==before+3);
    oi[1].hAllocation=2;
    CHECK(NT_SUCCESS(ddi->DxgkDdiOpenAllocation(device,&open)));
    opened[0]=oi[0].hDeviceSpecificAllocation;opened[1]=oi[1].hDeviceSpecificAllocation;
    CHECK(!os_references[1] && !os_references[2] && device->OpenAllocations==2);
    CHECK(NT_SUCCESS(ddi->DxgkDdiOpenAllocation(second.hDevice,&open2)));readonly=oi2.hDeviceSpecificAllocation;
    CHECK(!os_references[1] && handle_acquires==handle_releases);
    CHECK(TgGpuValidateOpenAllocationRange(device,opened[0],8191,1,TRUE)==STATUS_SUCCESS);
    CHECK(TgGpuValidateOpenAllocationRange(device,opened[0],8192,1,FALSE)==STATUS_INVALID_BUFFER_SIZE);
    CHECK(TgGpuValidateOpenAllocationRange(device,opened[0],UINT64_MAX,2,FALSE)==STATUS_INVALID_BUFFER_SIZE);
    CHECK(TgGpuValidateOpenAllocationRange(second.hDevice,readonly,0,4,TRUE)==STATUS_ACCESS_DENIED);
    CHECK(TgGpuValidateOpenAllocationRange(device,readonly,0,4,FALSE)==STATUS_INVALID_PARAMETER);
    CHECK(ddi->DxgkDdiDestroyDevice(device)==STATUS_DEVICE_BUSY);
    destroy.NumAllocations=2;destroy.pAllocationList=handles;destroy.hResource=resource;destroy.Flags.DestroyResource=1;
    CHECK(ddi->DxgkDdiDestroyAllocation(e,&destroy)==STATUS_DEVICE_BUSY);
    CHECK(ddi->DxgkDdiDestroyAllocation(other,&destroy)==STATUS_INVALID_PARAMETER);
    close.NumAllocations=2;close.pOpenHandleList=opened;
    CHECK(ddi->DxgkDdiCloseAllocation(second.hDevice,&close)==STATUS_INVALID_PARAMETER);
    opened[1]=opened[0];
    CHECK(ddi->DxgkDdiCloseAllocation(device,&close)==STATUS_INVALID_PARAMETER && device->OpenAllocations==2);
    opened[1]=oi[1].hDeviceSpecificAllocation;
    CHECK(NT_SUCCESS(ddi->DxgkDdiCloseAllocation(device,&close)));
    CHECK(!device->OpenAllocations && !os_references[1] && !os_references[2]);
    close.NumAllocations=1;close.pOpenHandleList=&readonly;
    CHECK(NT_SUCCESS(ddi->DxgkDdiCloseAllocation(second.hDevice,&close)) && !os_references[1]);
    /* A failed bulk destroy has no partial side effects. */
    handles[1]=handles[0];
    CHECK(ddi->DxgkDdiDestroyAllocation(e,&destroy)==STATUS_INVALID_PARAMETER && e->AllocationCount==2);
    handles[1]=os_allocation[2];destroy.NumAllocations=1;
    CHECK(ddi->DxgkDdiDestroyAllocation(e,&destroy)==STATUS_DEVICE_BUSY && e->AllocationCount==2);
    destroy.NumAllocations=2;
    CHECK(NT_SUCCESS(ddi->DxgkDdiDestroyAllocation(e,&destroy)));
    CHECK(!e->AllocationCount && !e->AllocationBytes && !e->ResourceCount && PoolsInUse()==before);
    os_allocation[1]=os_allocation[2]=NULL;
    /* Model the close-before-destroy ordering for a device-owned allocation. */
    create.NumAllocations=1;create.hResource=NULL;create.Flags.Value=0;
    CHECK(NT_SUCCESS(ddi->DxgkDdiCreateAllocation(e,&create)));
    os_allocation[3]=info[0].hAllocation;oi2.hAllocation=3;
    CHECK(NT_SUCCESS(ddi->DxgkDdiOpenAllocation(second.hDevice,&open2)));
    readonly=oi2.hDeviceSpecificAllocation;
    destroy=(DXGKARG_DESTROYALLOCATION){.NumAllocations=1,.pAllocationList=&os_allocation[3]};
    CHECK(!os_references[3] && ddi->DxgkDdiDestroyAllocation(e,&destroy)==STATUS_DEVICE_BUSY);
    CHECK(NT_SUCCESS(ddi->DxgkDdiCloseAllocation(second.hDevice,&close)));
    CHECK(NT_SUCCESS(ddi->DxgkDdiDestroyAllocation(e,&destroy)));
    os_allocation[3]=NULL;
    CHECK(!e->AllocationCount && !e->AllocationBytes && !e->Creating);
    /* The adapter charges rounded bytes across separate allocation calls. */
    desc[0].bytes=TG_GPU_MEMORY_BUDGET_BYTES-PAGE_SIZE+1;
    CHECK(NT_SUCCESS(ddi->DxgkDdiCreateAllocation(e,&create)));
    handles[0]=info[0].hAllocation;
    CHECK(e->AllocationBytes==TG_GPU_MEMORY_BUDGET_BYTES);
    desc[0].bytes=PAGE_SIZE;
    CHECK(ddi->DxgkDdiCreateAllocation(e,&create)==STATUS_GRAPHICS_NO_VIDEO_MEMORY);
    CHECK(!info[0].hAllocation && e->AllocationCount==1 && e->AllocationBytes==TG_GPU_MEMORY_BUDGET_BYTES);
    destroy.pAllocationList=handles;
    CHECK(NT_SUCCESS(ddi->DxgkDdiDestroyAllocation(e,&destroy)));
    CHECK(!e->AllocationBytes && NT_SUCCESS(ddi->DxgkDdiCreateAllocation(e,&create)));
    destroy.pAllocationList=&info[0].hAllocation;
    CHECK(NT_SUCCESS(ddi->DxgkDdiDestroyAllocation(e,&destroy)) && !e->AllocationBytes);
    CHECK(handle_acquires==handle_releases);
    CHECK(NT_SUCCESS(ddi->DxgkDdiDestroyDevice(second.hDevice)));
    CHECK(PoolsInUse()==before-1);
}

/* Register-state substitute for the hot channel path, not a GPU emulator.
 * It never invents GPU completion. Cache flush copies only an explicit model
 * GPU store into the CPU alias. It checks submission/teardown order
 * and can withhold preemption or reset acknowledgement to exercise lifetime.
 */
static struct {
    UINT32 mc,pb,map,sched,inst,ctrl,preempt,pbdma,runlist,runbase,root,expected_root;
    UINT32 clamp,reset,clocks,out5,mc_flush;
    UINT tlbs,flushes,reads,writes;
    UINT32 fifo_fault;
    UINT32 userd_put;
    UINT32 gpu_fence,*cpu_fence;
    UINT doorbells;
    UCHAR enable,voltage;
    BOOLEAN fail_preempt,fail_reset,fail_tlb,delay_reference;
} hw={.mc=0x100,.pb=1,.map=1,.sched=1,.enable=0x41,.voltage=110,.expected_root=0x900000};
static UINT32 Read(void *context,UINT32 address)
{
    CHECK(context==&hw && level==PASSIVE_LEVEL);++hw.reads;
    switch(address) {
    case 0x57000200:return hw.mc;
    case 0x57000204:return hw.pb;
    case 0x57002390:return hw.map;
    case 0x57001704:return 0x80090000;
    case 0x57002254:return 0x10000013;
    case 0x5801308c:return hw.userd_put;
    case 0x58013048:return hw.gpu_fence;
    case 0x57002630:return hw.sched;
    case 0x57800000:return hw.inst;
    case 0x57800004:return hw.ctrl;
    case 0x57002270:return hw.runbase;
    case 0x57002274:return hw.runlist;
    case 0x57002284:return 0;
    case 0x57002634:return hw.preempt;
    case 0x57003080:return hw.pbdma;
    case 0x57100c80:return hw.fail_tlb ? 0x10000 : 0x18000;
    case 0x57070000:case 0x57070010:case 0x57070004:return 0;
    case 0x57002100:return hw.fifo_fault;
    case 0x5700259c:case 0x57040108:case 0x57040148:
    case 0x57001100:case 0x57120058:case 0x5712005c:return 0;
    case 0x7000e6d4:return hw.clamp;
    case 0x6000628c:return hw.reset;
    case 0x60006280:return hw.clocks;
    case 0x6000667c:return hw.out5;
    case 0x70019970:return hw.mc_flush;
    case 0x70019974:return hw.mc_flush&4;
    default:CHECK(FALSE);return UINT32_MAX;
    }
}
static void Write(void *context,UINT32 address,UINT32 value)
{
    CHECK(context==&hw && level==PASSIVE_LEVEL);++hw.writes;
    switch(address) {
    case 0x5801308c:
        CHECK(hw.inst && hw.ctrl && hw.runlist==2 && !hw.sched && value==2);
        hw.userd_put=value;++hw.doorbells;break;
    case 0x57000200:hw.mc=value;break;
    case 0x57000204:hw.pb=value;break;
    case 0x57002630:hw.sched=value;break;
    case 0x57800000:
        CHECK(hw.sched&1);
        if(value) {CHECK(hw.tlbs && hw.root==hw.expected_root);hw.gpu_fence=*hw.cpu_fence;}
        else CHECK(!hw.runlist && !hw.pbdma && !hw.preempt);
        hw.inst=value;break;
    case 0x57800004:
        if(value&0x400) { CHECK(hw.inst&0x80000000);hw.ctrl=1; }
        if(value&0x800) hw.ctrl=0;
        break;
    case 0x57002270:hw.runbase=value;break;
    case 0x57002274:
        CHECK(hw.sched&1);
        if(value) {
            CHECK(value==2 && hw.ctrl && hw.inst && hw.runbase==0x00091003);
            hw.pbdma=0x3000; /* valid TSG zero */
        } else CHECK(!hw.ctrl && !hw.preempt);
        hw.runlist=value;break;
    case 0x57002634:
        CHECK(value==0x01000000 && !hw.ctrl && (hw.sched&1));
        hw.preempt=hw.fail_preempt ? 0x100000 : 0;
        if(!hw.preempt) hw.pbdma=0;
        break;
    case 0x57100cb8:hw.root=value;break;
    case 0x57100cbc:CHECK(value==0x80000001);++hw.tlbs;break;
    case 0x57070000:CHECK(value==1);++hw.flushes;break;
    case 0x57070010:
        CHECK(value==1 && hw.cpu_fence);
        if(!hw.delay_reference || !hw.inst)*hw.cpu_fence=hw.gpu_fence;
        break;
    case 0x57070004:CHECK(value==1);break;
    case 0x7000e6d4:hw.clamp=value;break;
    case 0x60006290:if(!hw.fail_reset) hw.reset|=value;break;
    case 0x60006288:hw.clocks&=~value;break;
    case 0x60006284:hw.clocks|=value;break;
    case 0x6000667c:hw.out5=value;break;
    case 0x70019970:hw.mc_flush=value;break;
    default:CHECK(FALSE);
    }
}
static void Delay(void *context,UINT32 us)
{ CHECK(context==&hw && level==PASSIVE_LEVEL && us); }
static bool ReadPmic(void *context,UINT8 address,UINT8 reg,UINT8 *value)
{
    CHECK(context==&hw && level==PASSIVE_LEVEL && address==0x33 && (reg==6 || reg==0x23));
    *value=reg==6 ? hw.enable : hw.voltage;return true;
}
static bool WritePmic(void *context,UINT8 address,UINT8 reg,UINT8 value)
{
    CHECK(context==&hw && level==PASSIVE_LEVEL && address==0x33 && (reg==6 || reg==0x23));
    if(reg==6) hw.enable=value;else hw.voltage=value;return true;
}

#include "resource_fixture.h"

static __declspec(noinline) void TestAdapterQueries(TG_GPU_CONTEXT_ENGINE *e,DRIVER_INITIALIZATION_DATA *d)
{
    DXGKARG_QUERYADAPTERINFO a={0};
    DXGK_QUERYSEGMENTIN4 input={0};
    DXGK_QUERYSEGMENTOUT4 segment,before;
    struct { DXGK_SEGMENTDESCRIPTOR4 desc;UCHAR extension[32]; } output[2];
    DXGK_GPUMMUCAPS mmu;
    DXGK_QUERYPAGETABLELEVELDESCIN level={0};
    DXGK_PAGE_TABLE_LEVEL_DESC desc;
    DXGK_PHYSICALADAPTERCAPS physical;
    UINT i;UINT64 address;
    CHECK(d->DxgkDdiQueryAdapterInfo==TgGpuDdiQueryAdapterInfo);
    a.Type=DXGKQAITYPE_QUERYSEGMENT4;a.pInputData=&input;a.InputDataSize=sizeof(input);
    a.pOutputData=&segment;a.OutputDataSize=sizeof(segment);
    memset(&segment,0xa5,sizeof(segment));segment.NbSegment=0;
    before=segment;
    CHECK(NT_SUCCESS(d->DxgkDdiQueryAdapterInfo(e,&a)) && segment.NbSegment==2);
    /* Count query must not consume its poison descriptor pointer/stride or
     * write any other member (including padding).
     */
    for(i=sizeof(UINT);i<sizeof(segment);++i)
        CHECK(((UCHAR *)&segment)[i]==((UCHAR *)&before)[i]);
    memset(&output,0xa5,sizeof(output));segment.pSegmentDescriptor=(BYTE *)&output;
    segment.SegmentDescriptorStride=sizeof(output[0]);
    CHECK(NT_SUCCESS(d->DxgkDdiQueryAdapterInfo(e,&a)));
    CHECK(segment.NbSegment==2 && segment.SegmentDescriptorStride==sizeof(output[0]));
    CHECK(segment.PagingBufferSegmentId==TG_GPU_PAGING_APERTURE_SEGMENT &&
          segment.PagingBufferSize==TG_GPU_DMA_BUFFER_BYTES);
    CHECK(segment.PagingBufferPrivateDataSize==TG_GPU_PAGING_PRIVATE_BYTES);
    CHECK(output[0].desc.BaseAddress.QuadPart==e->Tables->Memory.SystemPhysicalAddress.QuadPart);
    CHECK(output[0].desc.Size==e->Tables->Memory.Bytes && output[0].desc.Flags.CpuVisible);
    CHECK(output[0].desc.CpuTranslatedAddress.QuadPart==output[0].desc.BaseAddress.QuadPart);
    CHECK(output[0].desc.Flags.PopulatedFromSystemMemory && !output[0].desc.Flags.Aperture &&
          !output[0].desc.Flags.CacheCoherent && !output[0].desc.Flags.PreservedDuringStandby &&
          !output[0].desc.Flags.PreservedDuringHibernate && !output[0].desc.Flags.DirectFlip);
    CHECK(output[1].desc.Flags.Value==1 && output[1].desc.Size==TG_GPU_PAGING_APERTURE_BYTES &&
          output[1].desc.CommitLimit==output[1].desc.Size);
    for(i=0;i<sizeof(output[0].extension);++i)
        CHECK(output[0].extension[i]==0xa5 && output[1].extension[i]==0xa5);
    CHECK(NT_SUCCESS(TgGpuResolveTableSegment((PVOID)e->Tables,1,0,TG_GPU_TABLE_BYTES,&address)));
    CHECK(address==(UINT64)output[0].desc.BaseAddress.QuadPart);
    segment.SegmentDescriptorStride=sizeof(output[0].desc)-1;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_INVALID_PARAMETER);
    segment.SegmentDescriptorStride=sizeof(output[0]);input.PhysicalAdapterIndex=1;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_INVALID_PARAMETER);
    input.PhysicalAdapterIndex=0;a.OutputDataSize=sizeof(segment)-1;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_BUFFER_TOO_SMALL);
    a.OutputDataSize=sizeof(segment);a.pOutputData=NULL;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_INVALID_PARAMETER);

    a.Type=DXGKQAITYPE_GPUMMUCAPS;a.pOutputData=&mmu;a.OutputDataSize=sizeof(mmu);
    memset(&mmu,0xa5,sizeof(mmu));
    CHECK(NT_SUCCESS(d->DxgkDdiQueryAdapterInfo(e,&a)));
    CHECK(mmu.Value==0x21 && mmu.VirtualAddressBitCount==40 && mmu.PageTableLevelCount==2);
    CHECK(mmu.PageTableUpdateMode==DXGK_PAGETABLEUPDATE_GPU_PHYSICAL);
    CHECK(!mmu.LeafPageTableSizeFor64KPagesInBytes && !mmu.LegacyBehaviors.SourcePageTableVaInTransfer);
    a.InputDataSize=sizeof(input)-1;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_BUFFER_TOO_SMALL);
    a.InputDataSize=sizeof(input);input.PhysicalAdapterIndex=1;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_INVALID_PARAMETER);
    input.PhysicalAdapterIndex=0;
    a.Type=DXGKQAITYPE_PAGETABLELEVELDESC;a.pInputData=&level;a.InputDataSize=sizeof(level);
    a.pOutputData=&desc;a.OutputDataSize=sizeof(desc);
    for(level.LevelIndex=0;level.LevelIndex<mmu.PageTableLevelCount;++level.LevelIndex) {
        CHECK(NT_SUCCESS(d->DxgkDdiQueryAdapterInfo(e,&a)));
        CHECK(desc.PageTableIndexBitCount==14 && desc.PageTableSegmentId==1);
        CHECK(desc.PagingProcessPageTableSegmentId==1 && desc.PageTableSizeInBytes==TG_GPU_TABLE_BYTES);
        CHECK(desc.PageTableAlignmentInBytes==PAGE_SIZE);
    }
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_INVALID_PARAMETER);
    level.LevelIndex=0;level.PhysicalAdapterIndex=1;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_INVALID_PARAMETER);
    a.Type=DXGKQAITYPE_PHYSICALADAPTERCAPS;a.pInputData=&input;a.InputDataSize=sizeof(input);
    a.pOutputData=&physical;a.OutputDataSize=sizeof(physical);
    memset(&physical,0xa5,sizeof(physical));
    CHECK(NT_SUCCESS(d->DxgkDdiQueryAdapterInfo(e,&a)));
    CHECK(physical.NumExecutionNodes==1 && physical.PagingNodeIndex==0);
    CHECK(physical.DxgkPhysicalAdapterHandle==e->Dxgk.DeviceHandle && physical.Flags.Value==2);
    CHECK(!physical.VPRPagingNode);
    input.PhysicalAdapterIndex=1;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_INVALID_PARAMETER);
    input.PhysicalAdapterIndex=0;e->Stopping=TRUE;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_DEVICE_NOT_READY);e->Stopping=FALSE;
    a.Flags.Value=1;CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_NOT_SUPPORTED);a.Flags.Value=0;
    a.Type=DXGKQAITYPE_DRIVERCAPS;
    memset(&physical,0xa5,sizeof(physical));
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_BUFFER_TOO_SMALL);
    for(i=0;i<sizeof(physical);++i) CHECK(((UCHAR *)&physical)[i]==0xa5);
    DXGK_DRIVERCAPS driver={0};
    a.pOutputData=&driver;a.OutputDataSize=sizeof(driver);
    CHECK(NT_SUCCESS(d->DxgkDdiQueryAdapterInfo(e,&a)));
    CHECK(driver.WDDMVersion==DXGKDDI_WDDMv2_6 && driver.MiscCaps.ComputeOnly &&
          driver.GpuEngineTopology.NbAsymetricProcessingNodes==1);
    CHECK(driver.MemoryManagementCaps.GpuMmuSupported && driver.SchedulingCaps.PreemptionAware);
    CHECK(driver.SupportPerEngineTDR);
    struct {struct tg_gpu_adapter_info info;UCHAR guard[16];} abi;
    struct tg_gpu_adapter_info request={.version=TG_GPU_ADAPTER_ABI,.size=sizeof(request)};
    memset(&abi,0xa5,sizeof(abi));
    a.Type=DXGKQAITYPE_UMDRIVERPRIVATE;a.pInputData=&request;a.InputDataSize=sizeof(request);
    a.pOutputData=&abi;a.OutputDataSize=sizeof(abi);
    CHECK(NT_SUCCESS(d->DxgkDdiQueryAdapterInfo(e,&a)));
    CHECK(abi.info.version==1 && abi.info.size==48 && !abi.info.reserved);
    CHECK(abi.info.memory_budget_bytes==UINT64_C(536870912));
    CHECK(abi.info.vendor==0x10de && abi.info.chipset==0x12e && abi.info.sm==53 && abi.info.gr_class==0xb197);
    CHECK(abi.info.flags==(e->AdapterIdentityReady?TG_GPU_ADAPTER_LUID_VALID:0u));
    CHECK(!(abi.info.flags&TG_GPU_ADAPTER_NATIVE_SUBMIT));
    CHECK(abi.info.luid_low==(e->AdapterIdentityReady?e->AdapterLuid.LowPart:0u));
    CHECK(abi.info.luid_high==(e->AdapterIdentityReady?e->AdapterLuid.HighPart:0));
    for(i=0;i<sizeof(abi.guard);++i)CHECK(abi.guard[i]==0xa5);
    a.OutputDataSize=sizeof(abi.info)-1;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_BUFFER_TOO_SMALL);
    a.OutputDataSize=sizeof(abi);a.InputDataSize=sizeof(request)-1;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_BUFFER_TOO_SMALL);
    a.InputDataSize=sizeof(request);++request.version;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_GRAPHICS_DRIVER_MISMATCH);
    --request.version;--request.size;
    CHECK(d->DxgkDdiQueryAdapterInfo(e,&a)==STATUS_GRAPHICS_DRIVER_MISMATCH);
    ++request.size;
    /* Retain the in/out form where the request header aliases the reply. */
    abi.info=request;a.pInputData=&abi.info;
    CHECK(NT_SUCCESS(d->DxgkDdiQueryAdapterInfo(e,&a)));
    CHECK(abi.info.memory_budget_bytes==UINT64_C(536870912));
    CHECK(d->DxgkDdiQueryAdapterInfo(NULL,&a)==STATUS_INVALID_PARAMETER);
    CHECK(d->DxgkDdiQueryAdapterInfo(e,NULL)==STATUS_INVALID_PARAMETER);
    /* The Lite's public KMT entry supplies a 48-byte output and no input. */
    memset(&abi,0xa5,sizeof(abi));
    a.pInputData=NULL;a.InputDataSize=0;a.OutputDataSize=sizeof(abi.info);
    CHECK(NT_SUCCESS(d->DxgkDdiQueryAdapterInfo(e,&a)));
    CHECK(abi.info.version==TG_GPU_ADAPTER_ABI && abi.info.size==sizeof(abi.info) &&
          abi.info.memory_budget_bytes==TG_GPU_MEMORY_BUDGET_BYTES);
    CHECK(abi.info.luid_low==(e->AdapterIdentityReady?e->AdapterLuid.LowPart:0u) &&
          abi.info.luid_high==(e->AdapterIdentityReady?e->AdapterLuid.HighPart:0));
    for(i=0;i<sizeof(abi.guard);++i)CHECK(abi.guard[i]==0xa5);
}

static void TestGraphicsTemplate(TG_GPU_CONTEXT_ENGINE *e)
{
    static _Alignas(PAGE_SIZE) UCHAR source[TG_GPU_GR_GOLD_OFFSET+TG_GPU_GR_IMAGE_MAX_BYTES];
    struct tg_gpu_gr_work work={source,0x94000000,sizeof(source)};
    struct tg_gpu_gr_context_status golden={.stage=9,.context_bytes=512,.gpcs=1,.tpcs=2,.zculls=4,
        .image_magic=0x600dc0de,.image_changed_words=64};
    UINT32 *image=(UINT32 *)(source+TG_GPU_GR_GOLD_OFFSET);
    memset(image,0xa5,512);image[0xfc/4]=0x600dc0de;
    golden.result=1;
    CHECK(TgGpuInitializeGraphicsTemplate(e,&work,&golden)==STATUS_INVALID_PARAMETER);
    CHECK(!e->Graphics.Memory.Handle && !e->Creating);golden.result=0;
    golden.tpcs=1;
    CHECK(TgGpuInitializeGraphicsTemplate(e,&work,&golden)==STATUS_INVALID_PARAMETER);
    golden.tpcs=2;
    image[0xfc/4]=0;
    CHECK(TgGpuInitializeGraphicsTemplate(e,&work,&golden)==STATUS_DEVICE_DATA_ERROR);
    image[0xfc/4]=0x600dc0de;fail_allocate=TRUE;
    CHECK(TgGpuInitializeGraphicsTemplate(e,&work,&golden)==STATUS_INSUFFICIENT_RESOURCES);
    CHECK(!e->Graphics.Memory.Handle && !e->Creating);fail_allocate=FALSE;
    CHECK(NT_SUCCESS(TgGpuInitializeGraphicsTemplate(e,&work,&golden)));
    CHECK(e->Graphics.Memory.Handle && e->Graphics.ImageBytes==512 && e->GraphicsGeneration==1);
    CHECK(((UINT32 *)e->Graphics.Memory.CpuAddress)[0xfc/4]==0x600dc0de);
    CHECK(((UINT32 *)e->Graphics.Memory.CpuAddress)[0x100/4]==0xa5a5a5a5);
    CHECK(TgGpuInitializeGraphicsTemplate(e,&work,&golden)==STATUS_INVALID_DEVICE_STATE);
    memset(source,0,sizeof(source)); /* The engine owns an independent immutable copy. */
    CHECK(((UINT32 *)e->Graphics.Memory.CpuAddress)[0xfc/4]==0x600dc0de);
}
static void TestGraphicsImage(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_CONTEXT *c,TG_GPU_INTERNAL_VA *v)
{
    UINT32 *instance=c->Instance.CpuAddress;
    UINT32 *image=(UINT32 *)((UCHAR *)instance+TG_GPU_CONTEXT_GR_OFFSET);
    UINT32 *patch=(UINT32 *)((UCHAR *)image+TG_GPU_CONTEXT_PATCH_OFFSET);
    UINT64 base=v->Base+TG_GPU_CONTEXT_GR_VA_OFFSET;
    UINT64 physical=(UINT64)c->Instance.SystemPhysicalAddress.QuadPart+TG_GPU_CONTEXT_GR_OFFSET;
    UINT64 *leaf=v->Leaf.CpuAddress;
    CHECK(c->Instance.Bytes==TG_GPU_CONTEXT_PRIVATE_BYTES && c->GraphicsGeneration==e->GraphicsGeneration);
    CHECK(instance[132]==((UINT32)base|4) && instance[133]==(UINT32)(base>>32));
    CHECK(image[0xfc/4]==0x600dc0de && image[0xf0/4]==c->ContextId && !image[0xf4/4] && !image[0xf8/4]);
    CHECK(image[0x10/4]==21 && image[0x14/4]==(UINT32)(base+TG_GPU_CONTEXT_PATCH_OFFSET));
    CHECK(image[0x18/4]==(UINT32)((base+TG_GPU_CONTEXT_PATCH_OFFSET)>>32));
    CHECK(image[0xa0/4]==2 && image[0xa4/4]==(UINT32)(base+TG_GPU_CONTEXT_ACCESS_OFFSET));
    CHECK(image[0xa8/4]==(UINT32)((base+TG_GPU_CONTEXT_ACCESS_OFFSET)>>32));
    CHECK(patch[0]==0x40800c && patch[1]==(UINT32)((base+TG_GPU_CONTEXT_PAGEPOOL_OFFSET)>>8));
    CHECK(patch[12]==0x408004 && patch[13]==(UINT32)((base+TG_GPU_CONTEXT_BUNDLE_OFFSET)>>8));
    CHECK(leaf[0]==UINT64_C(0x100910007) && leaf[1]==UINT64_C(0x100910015));
    CHECK(leaf[2]==UINT64_C(0x100910023) && !leaf[3]);
    for(UINT i=0;i<TG_GPU_CONTEXT_GR_BYTES/PAGE_SIZE;++i) {
        UINT64 pte=leaf[TG_GPU_CONTEXT_GR_VA_OFFSET/PAGE_SIZE+i];
        CHECK(((pte>>4)&0xffffff)==(physical>>12)+i && (pte&3)==3 && (pte&(UINT64_C(1)<<32)));
        CHECK(!!(pte&4)==(i*PAGE_SIZE>=TG_GPU_CONTEXT_ACCESS_OFFSET));
    }
    CHECK(v->GraphicsPhysical==physical && !leaf[TG_GPU_CONTEXT_GR_VA_OFFSET/PAGE_SIZE-1]);
    CHECK(!leaf[(TG_GPU_CONTEXT_GR_VA_OFFSET+TG_GPU_CONTEXT_GR_BYTES)/PAGE_SIZE] && !leaf[TG_GPU_TABLE_ENTRIES-1]);
}
static TG_GPU_CONTEXT *RecreateContext(DRIVER_INITIALIZATION_DATA *ddi,TG_GPU_DEVICE *device,
    TG_GPU_CONTEXT_ENGINE *e,TG_GPU_CONTEXT *old,DXGKARG_SETROOTPAGETABLE *root)
{
    DXGKARG_CREATECONTEXT create={.hContext=(HANDLE)104,.EngineAffinity=1};
    create.Flags.VirtualAddressing=1;
    CHECK(NT_SUCCESS(ddi->DxgkDdiDestroyContext(old)));
    CHECK(NT_SUCCESS(ddi->DxgkDdiCreateContext(device,&create)));
    root->hContext=create.hContext;root->Address.SegmentOffset=0;root->NumEntries=2;
    ddi->DxgkDdiSetRootPageTable(e,root);
    CHECK(((TG_GPU_CONTEXT *)create.hContext)->Root.Valid);
    return create.hContext;
}
static __declspec(noinline) void TestAdapterLifecycle(const DXGKRNL_INTERFACE *callbacks)
{
    DXGKRNL_INTERFACE dxgk=*callbacks;DRIVER_INITIALIZATION_DATA d={0};
    DXGK_START_INFO start={.RequiredDmaQueueEntry=32,.AdapterLuid={0x12345678,-3}};
    TG_GPU_ADAPTER *a=NULL;ULONG sources=5,children=6;UINT before=hw.reads+hw.writes;
    dxgk.Size=sizeof(dxgk);dxgk.DxgkCbGetDeviceInformation=ResourceInfo;
    dxgk.DxgkCbMapMemory=ResourceMap;dxgk.DxgkCbUnmapMemory=ResourceUnmap;
    TgGpuRegisterAdapterDdis(&d);
    CHECK(d.DxgkDdiAddDevice==TgGpuDdiAddDevice && d.DxgkDdiStartDevice==TgGpuDdiStartDevice);
    CHECK(d.DxgkDdiStopDevice==TgGpuDdiStopDevice && d.DxgkDdiRemoveDevice==TgGpuDdiRemoveDevice);
    CHECK(d.DxgkDdiResetEngine && d.DxgkDdiQueryEngineStatus &&
          d.DxgkDdiQueryDependentEngineGroup && d.DxgkDdiCollectDbgInfo);
    CHECK(d.DxgkDdiSubmitCommandVirtual==TgGpuDdiSubmitCommandVirtual && d.DxgkDdiInterruptRoutine==NULL);
    CHECK(d.DxgkDdiAddDevice(NULL,(PVOID *)&a)==STATUS_INVALID_PARAMETER && !a);
    fail_pool=TRUE;
    CHECK(d.DxgkDdiAddDevice((PDEVICE_OBJECT)1,(PVOID *)&a)==STATUS_NO_MEMORY && !a);
    fail_pool=FALSE;
    CHECK(NT_SUCCESS(d.DxgkDdiAddDevice((PDEVICE_OBJECT)1,(PVOID *)&a)));
    CHECK(a && a->PhysicalDevice==(PDEVICE_OBJECT)1 && a->State==TgGpuAdapterAdded);
    const DXGK_WDDMDEVICECAPSIN caps_in={.DxgkrnlInterfaceVersion=DXGKDDI_INTERFACE_VERSION};
    DXGK_WDDMDEVICECAPS caps={0};
    DXGKARG_QUERYADAPTERINFO query={
        .Type=DXGKQAITYPE_WDDMDEVICECAPS,.pInputData=(PVOID)&caps_in,
        .InputDataSize=sizeof(caps_in),.pOutputData=&caps,.OutputDataSize=sizeof(caps)-1
    };
    CHECK(!a->Engine.Dxgk.DeviceHandle && !a->Engine.Tables);
    CHECK(d.DxgkDdiQueryAdapterInfo(a,&query)==STATUS_BUFFER_TOO_SMALL);
    query.OutputDataSize=sizeof(caps);
    CHECK(NT_SUCCESS(d.DxgkDdiQueryAdapterInfo(a,&query)) && caps.WDDMVersion==DXGKDDI_WDDMv2_6);
    CHECK(NT_SUCCESS(d.DxgkDdiStopDevice(a)));
    --dxgk.Size;
    CHECK(d.DxgkDdiStartDevice(a,&start,&dxgk,&sources,&children)==STATUS_INVALID_PARAMETER);
    ++dxgk.Size;
    CHECK(sources==5 && children==6 && a->State==TgGpuAdapterAdded);
    ResourceList(FALSE); /* Native completion requires the declared LIC resource. */
    CHECK(d.DxgkDdiStartDevice(a,&start,&dxgk,&sources,&children)==STATUS_DEVICE_CONFIGURATION_ERROR);
    CHECK(!sources && !children && !resource_live && a->State==TgGpuAdapterAdded);
    ResourceList(TRUE);resource_fail_map=4;resource_fail_unmap=2;
    CHECK(d.DxgkDdiStartDevice(a,&start,&dxgk,&sources,&children)==STATUS_UNSUCCESSFUL);
    CHECK(resource_live==2 && a->State==TgGpuAdapterStopping);
    CHECK(d.DxgkDdiRemoveDevice(a)==STATUS_DEVICE_BUSY);
    CHECK(d.DxgkDdiStartDevice(a,&start,&dxgk,&sources,&children)==STATUS_INVALID_DEVICE_STATE);
    resource_fail_unmap=0;CHECK(NT_SUCCESS(d.DxgkDdiStopDevice(a)) && !resource_live);
    for(UINT n=0;n<2;++n) {
        ResourceList(TRUE);memory_fail_after=n;
        CHECK(d.DxgkDdiStartDevice(a,&start,&dxgk,&sources,&children)==STATUS_INSUFFICIENT_RESOURCES);
        CHECK(a->State==TgGpuAdapterAdded && !adapter_table_used && !resource_live);
    }
    memory_fail_after=UINT32_MAX;
    ResourceList(TRUE);CHECK(NT_SUCCESS(d.DxgkDdiStartDevice(a,&start,&dxgk,&sources,&children)));
    CHECK(!sources && !children && a->State==TgGpuAdapterReady && a->EngineReady && a->QueueReady);
    CHECK(a->Engine.Tables==&a->Tables && a->Queue.Engine==&a->Engine && a->Engine.QueueOwner==&a->Queue);
    CHECK(a->Tables.Memory.Bytes==2u*1024*1024 && adapter_table_used);
    CHECK(a->Engine.AdapterIdentityReady && a->Engine.AdapterLuid.LowPart==0x12345678);
    TestAdapterQueries(&a->Engine,&d);
    CHECK(d.DxgkDdiRemoveDevice(a)==STATUS_DEVICE_BUSY);
    CHECK(d.DxgkDdiStartDevice(a,&start,&dxgk,&sources,&children)==STATUS_INVALID_DEVICE_STATE);
    DXGKARG_CREATEDEVICE device={.hDevice=(HANDLE)500};device.Flags.SystemDevice=1;
    CHECK(NT_SUCCESS(d.DxgkDdiCreateDevice(a,&device)));
    CHECK(d.DxgkDdiStopDevice(a)==STATUS_DEVICE_BUSY && a->EngineReady && !a->QueueReady);
    CHECK(a->Engine.Stopping && !a->Engine.AdapterIdentityReady && resource_live && adapter_table_used);
    CHECK(NT_SUCCESS(d.DxgkDdiDestroyDevice(device.hDevice)));
    fail_free=TRUE;CHECK(d.DxgkDdiStopDevice(a)==STATUS_UNSUCCESSFUL && a->Engine.Commands.Handle);
    fail_free=FALSE;adapter_table_fail_free=TRUE;
    CHECK(d.DxgkDdiStopDevice(a)==STATUS_UNSUCCESSFUL && !a->EngineReady && adapter_table_used);
    CHECK(resource_live && a->State==TgGpuAdapterStopping);
    adapter_table_fail_free=FALSE;resource_fail_unmap=5;
    CHECK(d.DxgkDdiStopDevice(a)==STATUS_UNSUCCESSFUL && resource_live==16 && !adapter_table_used);
    resource_fail_unmap=0;
    CHECK(NT_SUCCESS(d.DxgkDdiStopDevice(a)) && !resource_live && a->State==TgGpuAdapterAdded);
    ResourceList(TRUE);CHECK(NT_SUCCESS(d.DxgkDdiStartDevice(a,&start,&dxgk,&sources,&children)));
    CHECK(NT_SUCCESS(d.DxgkDdiStopDevice(a)));
    CHECK(NT_SUCCESS(d.DxgkDdiRemoveDevice(a)));
    CHECK(hw.reads+hw.writes==before); /* This module never touches hardware. */
    for(UINT i=0;i<64;++i)CHECK(!pool_used[i] && !memory_used[i]);
    CHECK(!adapter_table_used && !resource_live);
}

__declspec(noinline) static void TestPagingDispatch(const DXGKRNL_INTERFACE *dxgk)
{
    static TG_GPU_CONTEXT_ENGINE e;static TG_GPU_WORK_QUEUE q;static TG_GPU_HARDWARE hardware;
    static TG_GPU_QUEUE_ENTRY priv[5];static _Alignas(PAGE_SIZE) UCHAR commands[2][512];
    static TG_GPU_TABLE_SEGMENT tables={.Memory={(HANDLE)99,paging_tables,{.QuadPart=0x94000000},sizeof(paging_tables)},.SegmentId=1};
    DRIVER_INITIALIZATION_DATA ddi={0};DXGKARG_GETROOTPAGETABLESIZE size={.NumberOfPte=1};
    DXGKARG_CREATEPROCESS process={.hDxgkProcess=(HANDLE)100,.Flags.SystemProcess=1};
    DXGKARG_CREATEDEVICE device={.hDevice=(HANDLE)101};
    DXGKARG_CREATECONTEXT context={.hContext=(HANDLE)102,.EngineAffinity=1,.Flags.VirtualAddressing=1};
    DXGKARG_SETROOTPAGETABLE set={.Address={1,0,0},.NumEntries=TG_GPU_TABLE_ENTRIES};
    DXGKARG_BUILDPAGINGBUFFER a={.Operation=DXGK_OPERATION_UPDATE_PAGE_TABLE};
    DXGKARG_SUBMITCOMMANDVIRTUAL submit={.DmaBufferVirtualAddress=0x10000,.DmaBufferSize=40,
        .SubmissionFenceId=100,.pDmaBufferPrivateData=&priv[4],.DmaBufferPrivateDataSize=sizeof(priv[4])};
    DXGK_PTE ptes[3]={{.Valid=1,.Segment=1,.PageAddress=TG_GPU_TABLE_BYTES>>PAGE_SHIFT}};
    struct tg_gpu_power power={.owned=true,.ready=true,.pmic_address=0x33,.original_enable=0x40,.original_voltage=50};
    struct tg_gpu_clock clock={0};
    struct tg_gpu_power_io io={&hw,Read,Write,ReadPmic,WritePmic,Delay};
    UINT64 *leaf=(UINT64 *)(paging_tables+TG_GPU_TABLE_BYTES);UINT before=interrupts;
    CHECK(TgGpuInitializeContextEngine(&e,dxgk,&tables)==STATUS_SUCCESS);TestGraphicsTemplate(&e);
    TgGpuRegisterContextDdis(&ddi);
    CHECK(ddi.DxgkDdiBuildPagingBuffer==TgGpuDdiBuildPagingBuffer && !ddi.DxgkDdiSubmitCommandVirtual);
    CHECK(ddi.DxgkDdiGetRootPageTableSize(&e,&size)==TG_GPU_TABLE_BYTES && size.NumberOfPte==TG_GPU_TABLE_ENTRIES);
    CHECK(ddi.DxgkDdiCreateProcess(&e,&process)==STATUS_SUCCESS);device.hKmdProcess=process.hKmdProcess;
    CHECK(ddi.DxgkDdiCreateDevice(&e,&device)==STATUS_SUCCESS);
    DXGKARG_CREATECONTEXT paging_context={.hContext=(HANDLE)103,.EngineAffinity=1,
        .Flags={.SystemContext=1,.VirtualAddressing=1}};
    CHECK(ddi.DxgkDdiCreateContext(device.hDevice,&paging_context)==STATUS_SUCCESS);
    CHECK(paging_context.ContextInfo.DmaBufferSize==TG_GPU_DMA_BUFFER_BYTES &&
          paging_context.ContextInfo.DmaBufferPrivateDataSize==TG_GPU_PAGING_PRIVATE_BYTES &&
          paging_context.ContextInfo.DmaBufferSegmentSet==(1u<<(TG_GPU_PAGING_APERTURE_SEGMENT-1)));
    CHECK(ddi.DxgkDdiDestroyContext(paging_context.hContext)==STATUS_SUCCESS);
    CHECK(ddi.DxgkDdiCreateContext(device.hDevice,&context)==STATUS_SUCCESS);
    set.hContext=context.hContext;ddi.DxgkDdiSetRootPageTable(&e,&set);
    a.UpdatePageTable=(DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE){.PageTableLevel=1,
        .UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL,.PageTableAddress.CpuVirtual=paging_tables,
        .pPageTableEntries=ptes,.NumPageTableEntries=1,.Flags.InitialUpdate=1};
    CHECK(ddi.DxgkDdiBuildPagingBuffer(&e,&a)==STATUS_INVALID_PARAMETER); /* missing process */
    a.UpdatePageTable.hProcess=process.hKmdProcess;
    ((TG_GPU_PROCESS *)process.hKmdProcess)->Flags.SystemProcess=0;
    CHECK(ddi.DxgkDdiBuildPagingBuffer(&e,&a)==STATUS_INVALID_PARAMETER);
    ((TG_GPU_PROCESS *)process.hKmdProcess)->Flags.SystemProcess=1;
    CHECK(ddi.DxgkDdiBuildPagingBuffer(&e,&a)==STATUS_SUCCESS);
    CHECK(((UINT64 *)paging_tables)[0]!=0 && !e.Creating && !((TG_GPU_PROCESS *)process.hKmdProcess)->PagingReferences);
    /* VidMm maps its table backing in CPU_VIRTUAL mode after the clear. */
    a.UpdatePageTable.PageTableLevel=0;
    a.UpdatePageTable.Flags.InitialUpdate=0;
    a.UpdatePageTable.PageTableAddress.CpuVirtual=leaf;
    a.pDmaBuffer=commands[0];a.DmaSize=sizeof(commands[0]);
    a.pDmaBufferPrivateData=&priv[0];a.DmaBufferPrivateDataSize=sizeof(priv[0]);
    CHECK(ddi.DxgkDdiBuildPagingBuffer(&e,&a)==STATUS_SUCCESS);
    CHECK(leaf[0]==UINT64_C(0x100940201) && a.pDmaBuffer==commands[0] &&
          a.DmaSize==sizeof(commands[0]) && !priv[0].Paging.Magic &&
          a.pDmaBufferPrivateData==&priv[0] && a.DmaBufferPrivateDataSize==sizeof(priv[0]));
    /* VidMm's aperture offsets must become the two distinct system PFNs. */
    struct { MDL Mdl; PFN_NUMBER Pages[3]; } backing={0};
    DXGKARG_BUILDPAGINGBUFFER aperture={.Operation=DXGK_OPERATION_MAP_APERTURE_SEGMENT};
    UINT64 expected;
    backing.Mdl.ByteCount=3*PAGE_SIZE;
    backing.Pages[1]=0x140005;backing.Pages[2]=0x80234;
    aperture.MapApertureSegment.SegmentId=TG_GPU_PAGING_APERTURE_SEGMENT;
    aperture.MapApertureSegment.OffsetInPages=3;
    aperture.MapApertureSegment.NumberOfPages=2;
    aperture.MapApertureSegment.pMdl=&backing.Mdl;
    aperture.MapApertureSegment.MdlOffset=1;
    CHECK(ddi.DxgkDdiBuildPagingBuffer(&e,&aperture)==STATUS_SUCCESS);
    backing.Pages[1]=0x88234;backing.Pages[2]=1;
    CHECK(ddi.DxgkDdiBuildPagingBuffer(&e,&aperture)==STATUS_INVALID_ADDRESS);
    for(UINT i=0;i<2;++i)ptes[i]=(DXGK_PTE){.Valid=1,.Segment=TG_GPU_PAGING_APERTURE_SEGMENT,.PageAddress=3+i};
    a.UpdatePageTable.NumPageTableEntries=2;a.UpdatePageTable.StartIndex=2;
    CHECK(ddi.DxgkDdiBuildPagingBuffer(&e,&a)==STATUS_SUCCESS);
    CHECK(tg_gpu_pte_4k(UINT64_C(0x140005000),false,true,&expected) && leaf[2]==expected);
    CHECK(tg_gpu_pte_4k(UINT64_C(0x80234000),false,true,&expected) && leaf[3]==expected);
    aperture=(DXGKARG_BUILDPAGINGBUFFER){.Operation=DXGK_OPERATION_UNMAP_APERTURE_SEGMENT};
    aperture.UnmapApertureSegment.SegmentId=TG_GPU_PAGING_APERTURE_SEGMENT;
    aperture.UnmapApertureSegment.OffsetInPages=3;
    aperture.UnmapApertureSegment.NumberOfPages=2;
    CHECK(ddi.DxgkDdiBuildPagingBuffer(&e,&aperture)==STATUS_SUCCESS);
    CHECK(ddi.DxgkDdiBuildPagingBuffer(&e,&a)==STATUS_INVALID_ADDRESS);
    leaf[2]=leaf[3]=0;
    for(UINT i=0;i<3;++i)ptes[i]=(DXGK_PTE){.Valid=1,.PageAddress=(0x80010000u>>PAGE_SHIFT)+i};
    a.UpdatePageTable.PageTableLevel=0;a.UpdatePageTable.UpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;
    a.UpdatePageTable.PageTableAddress.GpuPhysical=(D3DGPU_PHYSICAL_ADDRESS){1,0,TG_GPU_TABLE_BYTES};
    a.UpdatePageTable.StartIndex=7;a.UpdatePageTable.NumPageTableEntries=3;
    a.DmaSize=64;
    a.DmaBufferGpuVirtualAddress=0x100000;
    fail_pool=TRUE;CHECK(ddi.DxgkDdiBuildPagingBuffer(&e,&a)==STATUS_NO_MEMORY);fail_pool=FALSE;
    a.hSystemContext=context.hContext;
    CHECK(ddi.DxgkDdiBuildPagingBuffer(&e,&a)==STATUS_INVALID_PARAMETER);a.hSystemContext=NULL;
    ptes[2].NoExecute=1;
    CHECK(ddi.DxgkDdiBuildPagingBuffer(&e,&a)==STATUS_NOT_SUPPORTED && !priv[0].Paging.Magic);
    ptes[2].NoExecute=0;
    CHECK(ddi.DxgkDdiBuildPagingBuffer(&e,&a)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER);
    CHECK(!a.MultipassOffset && a.DmaSize==64 && a.pDmaBuffer==commands[0] &&
          a.pDmaBufferPrivateData==&priv[0] && a.DmaBufferPrivateDataSize==sizeof(priv[0]));
    a.DmaSize=72;
    CHECK(ddi.DxgkDdiBuildPagingBuffer(&e,&a)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER);
    CHECK(a.MultipassOffset==1 && !a.DmaSize && priv[0].Paging.BuiltBytes==72 && !leaf[7] &&
          a.pDmaBufferPrivateData==&priv[0]+1 && !a.DmaBufferPrivateDataSize);
    UINT private_used=sizeof(priv[0])-a.DmaBufferPrivateDataSize;
    a.pDmaBuffer=commands[1];a.DmaSize=sizeof(commands[1]);
    a.pDmaBufferPrivateData=&priv[1];a.DmaBufferPrivateDataSize=3*sizeof(priv[1]);
    a.DmaBufferGpuVirtualAddress=0x200000;
    CHECK(ddi.DxgkDdiBuildPagingBuffer(&e,&a)==STATUS_SUCCESS && !a.MultipassOffset);
    CHECK(priv[1].Paging.BuiltBytes==80 && !leaf[8] && !leaf[9]);
    a.Operation=DXGK_OPERATION_FLUSH_TLB;a.DmaBufferWriteOffset=80;
    a.FlushTlb=(DXGK_BUILDPAGINGBUFFER_FLUSHTLB){.RootPageTableAddress={1,0,0},.hProcess=process.hKmdProcess};
    DXGKARG_BUILDPAGINGBUFFER exhausted=a;exhausted.DmaBufferPrivateDataSize=0;
    CHECK(ddi.DxgkDdiBuildPagingBuffer(&e,&exhausted)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER);
    CHECK(exhausted.pDmaBuffer==a.pDmaBuffer && exhausted.DmaSize==a.DmaSize &&
          exhausted.pDmaBufferPrivateData==a.pDmaBufferPrivateData && !exhausted.DmaBufferPrivateDataSize);
    CHECK(ddi.DxgkDdiBuildPagingBuffer(&e,&a)==STATUS_SUCCESS);
    CHECK(priv[1].Paging.BuiltBytes==80 && priv[2].Paging.WriteOffset==80 &&
          priv[2].Paging.BuiltBytes==144 && a.DmaSize==sizeof(commands[1])-144 &&
          a.pDmaBufferPrivateData==&priv[3] && a.DmaBufferPrivateDataSize==sizeof(priv[3]));
    memset(ptes,0,sizeof(ptes)); /* builder retained no OS PTE array */
    memset(&hw,0,sizeof(hw));hw.mc=0x100;hw.pb=hw.map=hw.sched=1;hw.enable=0x41;hw.voltage=110;
    hw.expected_root=0x940000;hw.cpu_fence=(UINT32 *)((UCHAR *)e.Commands.CpuAddress+TG_GPU_COMMAND_FENCE);
    e.UserdBar1Ready=TRUE;e.UserdBar1Offset=0x13000;
    CHECK(TgGpuInitializeWorkQueue(&q,&e)==STATUS_SUCCESS);
    submit.hContext=context.hContext;CHECK(TgGpuQueueSubmit(&q,&submit)==STATUS_SUCCESS);
    submit.Flags.Paging=1;submit.hContext=NULL;submit.DmaBufferVirtualAddress=0x100000;submit.DmaBufferSize=72;
    submit.pDmaBufferPrivateData=&priv[0];submit.DmaBufferPrivateDataSize=private_used;submit.SubmissionFenceId=101;
    submit.DmaBufferUmdPrivateDataSize=1;CHECK(TgGpuQueueSubmit(&q,&submit)==STATUS_INVALID_PARAMETER);
    submit.DmaBufferUmdPrivateDataSize=0;CHECK(TgGpuQueueSubmit(&q,&submit)==STATUS_SUCCESS);
    CHECK(TgGpuQueueSubmit(&q,&submit)==STATUS_DEVICE_BUSY);
    submit.DmaBufferVirtualAddress=0x200000;submit.DmaBufferSize=144;submit.pDmaBufferPrivateData=&priv[1];submit.SubmissionFenceId=102;
    submit.DmaBufferPrivateDataSize=sizeof(priv[1]);
    CHECK(TgGpuQueueSubmit(&q,&submit)==STATUS_INVALID_PARAMETER);
    submit.DmaBufferPrivateDataSize=3*sizeof(priv[1])-a.DmaBufferPrivateDataSize;
    CHECK(TgGpuQueueSubmit(&q,&submit)==STATUS_SUCCESS);
    CHECK(TgGpuServiceWorkQueue(&q,&io,&power)==STATUS_PENDING && !leaf[7]);
    CHECK(TgGpuServiceWorkQueue(&q,&io,&power)==STATUS_PENDING && !leaf[7] && interrupts==before);
    hw.gpu_fence=q.Current.Expected;expected_fence=100;reuse_private=&priv[4];
    CHECK(TgGpuServiceWorkQueue(&q,&io,&power)==STATUS_SUCCESS && !reuse_private && !leaf[7]);
    expected_fence=101;reuse_private=&priv[0];
    CHECK(TgGpuServiceWorkQueue(&q,&io,&power)==STATUS_SUCCESS && !reuse_private && leaf[7] && !leaf[8]);
    expected_fence=102;reuse_private=&priv[1];
    CHECK(TgGpuServiceWorkQueue(&q,&io,&power)==STATUS_SUCCESS && !reuse_private && leaf[8] && leaf[9]);
    CHECK(e.LastCompletedFence==102 && interrupts==before+3 && !e.Active && !e.Notification && hw.root==0x940000);
    /* Real DDI statuses with controlled hardware readiness. Rejected 104
     * follows running 103 but precedes the valid null-render packet 105. */
    hardware.Engine=&e;hardware.Ready=TRUE;hardware.Power.owned=hardware.Power.ready=true;e.HardwareOwner=&hardware;
    TgGpuRegisterSubmissionDdi(&ddi);UINT pools=PoolsInUse(),accesses;
    submit=(DXGKARG_SUBMITCOMMANDVIRTUAL){.hContext=context.hContext,.DmaBufferVirtualAddress=0x10000,
        .DmaBufferSize=40,.pDmaBufferPrivateData=&priv[0],.DmaBufferPrivateDataSize=sizeof(priv[0]),.SubmissionFenceId=103};
    CHECK(SubmitModel(&e,&submit)==STATUS_SUCCESS && PoolsInUse()==pools+2);
    submit.SubmissionFenceId=104;submit.DmaBufferSize=3;submit.pDmaBufferPrivateData=NULL;submit.DmaBufferPrivateDataSize=0;
    CHECK(SubmitModel(&e,&submit)==STATUS_INVALID_PARAMETER && PoolsInUse()==pools+3);
    CHECK(e.LastCompletedFence==102 && !e.RecoveryPending);
    submit.SubmissionFenceId=105;submit.DmaBufferSize=40;submit.Flags.NullRendering=1;
    submit.pDmaBufferPrivateData=&priv[1];submit.DmaBufferPrivateDataSize=sizeof(priv[1]);
    CHECK(SubmitModel(&e,&submit)==STATUS_SUCCESS);
    CHECK(TgGpuServiceWorkQueue(&q,&io,&power)==STATUS_PENDING && e.LastCompletedFence==102);
    hw.gpu_fence=q.Current.Expected;expected_fence=103;notify_engine=&e;expected_driver_fence=104;
    CHECK(TgGpuServiceWorkQueue(&q,&io,&power)==STATUS_SUCCESS && e.LastCompletedFence==104);
    CHECK(interrupts==before+4 && PoolsInUse()==pools); /* no extra DMA event for 104 */
    accesses=hw.reads+hw.writes;expected_fence=expected_driver_fence=105;
    CHECK(TgGpuServiceWorkQueue(&q,&io,&power)==STATUS_SUCCESS && e.LastCompletedFence==105);
    CHECK(hw.reads+hw.writes==accesses && interrupts==before+5);
    submit.Flags.Value=0;submit.SubmissionFenceId=106;submit.DmaBufferSize=3;
    submit.pDmaBufferPrivateData=NULL;submit.DmaBufferPrivateDataSize=0;
    CHECK(SubmitModel(&e,&submit)==STATUS_INVALID_PARAMETER && e.LastCompletedFence==106);
    CHECK(PoolsInUse()==pools && interrupts==before+5 && hw.reads+hw.writes==accesses);
    /* Context-switch-to-null may carry no private/DMA buffer. */
    submit.Flags.ContextSwitch=1;submit.Flags.Paging=1;submit.hContext=NULL;submit.SubmissionFenceId=107;submit.DmaBufferSize=0;
    CHECK(SubmitModel(&e,&submit)==STATUS_SUCCESS);
    expected_fence=expected_driver_fence=107;
    CHECK(TgGpuServiceWorkQueue(&q,&io,&power)==STATUS_SUCCESS && e.LastCompletedFence==107);
    CHECK(PoolsInUse()==pools && hw.reads+hw.writes==accesses && interrupts==before+6);
    notify_engine=NULL;
    /* A second submission can use the unused tail of the same DMA buffer.
     * Failed TLB invalidation pins its slot until reset proves isolation. */
    a.DmaBufferWriteOffset=144;
    CHECK(ddi.DxgkDdiBuildPagingBuffer(&e,&a)==STATUS_SUCCESS);
    CHECK(priv[3].Paging.WriteOffset==144 && priv[3].Paging.BuiltBytes==208 &&
          a.pDmaBufferPrivateData==&priv[4] && !a.DmaBufferPrivateDataSize);
    submit.Flags.Value=1;submit.pDmaBufferPrivateData=&priv[3];submit.DmaBufferPrivateDataSize=sizeof(priv[3]);
    submit.DmaBufferVirtualAddress=0x200000+144;submit.DmaBufferSize=64;submit.SubmissionFenceId=108;
    CHECK(TgGpuQueueSubmit(&q,&submit)==STATUS_SUCCESS);
    {
        DXGKARG_PREEMPTCOMMAND preempt={.PreemptionFenceId=900};
        TG_GPU_PAGING_BUFFER saved=priv[3].Paging;UINT events=preemption_interrupts;
        UINT accesses_before=hw.reads+hw.writes;
        notify_engine=&e;notify_context=NULL;expected_preemption=900;expected_preempt_completed=107;
        level=DISPATCH_LEVEL;CHECK(TgGpuDdiPreemptCommand(&e,&preempt)==STATUS_SUCCESS);level=PASSIVE_LEVEL;
        CHECK(TgGpuServiceWorkQueue(&q,NULL,NULL)==STATUS_SUCCESS && preemption_interrupts==events+1);
        CHECK(hw.reads+hw.writes==accesses_before && e.LastCompletedFence==107);
        CHECK(priv[3].Paging.Magic==saved.Magic && priv[3].Paging.CpuAddress==saved.CpuAddress &&
            priv[3].Paging.GpuAddress==saved.GpuAddress && priv[3].Paging.BuiltBytes==saved.BuiltBytes);
        notify_engine=NULL;
    }
    CHECK(TgGpuQueueSubmit(&q,&submit)==STATUS_SUCCESS);hw.fail_tlb=TRUE;
    CHECK(TgGpuServiceWorkQueue(&q,&io,&power)==STATUS_IO_TIMEOUT && e.Active==&q.Current);
    CHECK(q.Current.Paging && e.LastCompletedFence==107 && interrupts==before+6);
    hw.fail_tlb=FALSE;hw.fail_reset=TRUE;
    CHECK(TgGpuResetWorkQueue(&q,&io,&power,&clock)==STATUS_DEVICE_DATA_ERROR && q.Current.Engine);
    hw.fail_reset=FALSE;CHECK(TgGpuResetWorkQueue(&q,&io,&power,&clock)==STATUS_SUCCESS);
    CHECK(!q.Current.Engine && !e.Active && !e.Notification && e.LastCompletedFence==107 && interrupts==before+6);
    fail_pool=fail_sync=TRUE;UINT faults=recovery_interrupts;
    CHECK(SubmitModel(&e,&submit)==STATUS_SUCCESS);
    fail_pool=fail_sync=FALSE;
    CHECK(e.RecoveryPending && !e.RecoveryNotified && e.RecoveryStatus==STATUS_NO_MEMORY && q.Paused);
    CHECK(e.LastCompletedFence==107 && recovery_interrupts==faults);
    CHECK(TgGpuRequestEngineRecovery(&e,STATUS_DEVICE_DATA_ERROR)==STATUS_SUCCESS && recovery_interrupts==faults+1);
    CHECK(TgGpuRequestEngineRecovery(&e,STATUS_DEVICE_NOT_READY)==STATUS_SUCCESS && recovery_interrupts==faults+1);
    CHECK(e.RecoveryStatus==STATUS_NO_MEMORY && e.LastCompletedFence==107);
    CHECK(TgGpuStopWorkQueue(&q)==STATUS_SUCCESS);
    CHECK(ddi.DxgkDdiDestroyContext(context.hContext)==STATUS_SUCCESS);
    CHECK(ddi.DxgkDdiDestroyDevice(device.hDevice)==STATUS_SUCCESS);
    CHECK(ddi.DxgkDdiDestroyProcess(&e,process.hKmdProcess)==STATUS_SUCCESS);
    e.HardwareOwner=NULL;CHECK(TgGpuStopContextEngine(&e)==STATUS_SUCCESS);
    for(UINT i=0;i<64;++i)CHECK(!pool_used[i] && !memory_used[i]);
    for(UINT i=0;i<4;++i)CHECK(!graphics_used[i]);
}

/* Existing scheduler fixtures now supply the real inline submission ABI.
 * GPU execution is still an explicit model; these NOPs are not a draw. */
static NTSTATUS SubmitModel(TG_GPU_CONTEXT_ENGINE *e,const DXGKARG_SUBMITCOMMANDVIRTUAL *input)
{
    struct {struct tg_gpu_submit_request Header;UINT Words[64];} payload={0};
    DXGKARG_SUBMITCOMMANDVIRTUAL a=*input;
    if(!a.Flags.Paging && !a.Flags.ContextSwitch && !a.Flags.NullRendering &&
       a.DmaBufferSize && !(a.DmaBufferSize&3) && a.DmaBufferSize<=sizeof(payload.Words) &&
       !a.DmaBufferUmdPrivateDataSize && a.pDmaBufferPrivateData &&
       a.DmaBufferPrivateDataSize>=sizeof(TG_GPU_QUEUE_ENTRY)) {
        payload.Header=(struct tg_gpu_submit_request){TG_GPU_SUBMIT_ABI,
            sizeof(payload.Header)+a.DmaBufferSize,a.DmaBufferSize,0};
        a.pDmaBufferPrivateData=&payload;a.DmaBufferPrivateDataSize=sizeof(payload);
        a.DmaBufferUmdPrivateDataSize=payload.Header.size;
    }
    NTSTATUS status=TgGpuDdiSubmitCommandVirtual(e,&a);
    memset(payload.Words,0xdd,sizeof(payload.Words));
    return status;
}

static VOID RequestPreempt(TG_GPU_CONTEXT_ENGINE *e,UINT fence)
{
    DXGKARG_PREEMPTCOMMAND a={.PreemptionFenceId=fence};UINT accesses=hw.reads+hw.writes;
    level=DISPATCH_LEVEL;CHECK(TgGpuDdiPreemptCommand(e,&a)==STATUS_SUCCESS);
    CHECK(level==DISPATCH_LEVEL && !e->Creating && hw.reads+hw.writes==accesses);level=PASSIVE_LEVEL;
}
static __declspec(noinline) VOID TestPreemption(const DXGKRNL_INTERFACE *dxgk)
{
    static TG_GPU_CONTEXT_ENGINE e;static TG_GPU_WORK_QUEUE q;static TG_GPU_HARDWARE owner;
    static TG_GPU_QUEUE_ENTRY entries[3];static _Alignas(PAGE_SIZE) UCHAR table[TG_GPU_TABLE_BYTES];
    static TG_GPU_TABLE_SEGMENT tables={.Memory={(HANDLE)4,table,{.QuadPart=0x90000000},sizeof(table)},.SegmentId=1};
    DXGKARG_CREATEPROCESS process={.hDxgkProcess=(HANDLE)100};DXGKARG_CREATEDEVICE device={0};
    DXGKARG_CREATECONTEXT create={.EngineAffinity=1,.Flags.VirtualAddressing=1};
    DXGKARG_SETROOTPAGETABLE root={.Address={1,0,0},.NumEntries=2};
    DXGKARG_SUBMITCOMMANDVIRTUAL submit={.DmaBufferVirtualAddress=0x10000,.DmaBufferSize=40,
        .pDmaBufferPrivateData=&entries[0],.DmaBufferPrivateDataSize=sizeof(entries[0])};
    struct tg_gpu_power power={.owned=true,.ready=true,.stage=TG_GPU_STAGE_READY};
    struct tg_gpu_power_io io={&hw,Read,Write,ReadPmic,WritePmic,Delay};
    struct tg_gpu_clock clock={0};DRIVER_INITIALIZATION_DATA ddi={0};TG_GPU_CONTEXT *c;
    UINT events=preemption_interrupts,dma=interrupts,pools,accesses,faults=recovery_interrupts;
    CHECK(TgGpuInitializeContextEngine(&e,dxgk,&tables)==STATUS_SUCCESS);TestGraphicsTemplate(&e);
    CHECK(TgGpuDdiCreateProcess(&e,&process)==STATUS_SUCCESS);device.hKmdProcess=process.hKmdProcess;
    CHECK(TgGpuDdiCreateDevice(&e,&device)==STATUS_SUCCESS);
    CHECK(TgGpuDdiCreateContext(device.hDevice,&create)==STATUS_SUCCESS);c=create.hContext;
    root.hContext=c;TgGpuDdiSetRootPageTable(&e,&root);CHECK(c->Root.Valid);
    CHECK(TgGpuInitializeWorkQueue(&q,&e)==STATUS_SUCCESS);pools=PoolsInUse();
    memset(&hw,0,sizeof(hw));hw.mc=0x100;hw.pb=hw.map=hw.sched=1;hw.expected_root=0x900000;
    hw.cpu_fence=(UINT32 *)((UCHAR *)e.Commands.CpuAddress+TG_GPU_COMMAND_FENCE);
    owner.Engine=&e;owner.Ready=TRUE;owner.Power=power;e.HardwareOwner=&owner;
    e.UserdBar1Ready=TRUE;e.UserdBar1Offset=0x13000;e.LastCompletedFence=17;
    notify_engine=&e;notify_context=c;notify_references=0;
    TgGpuRegisterAdapterDdis(&ddi);CHECK(ddi.DxgkDdiPreemptCommand==TgGpuDdiPreemptCommand);
    CHECK(ddi.DxgkDdiResetFromTimeout==TgGpuDdiResetFromTimeout &&
        ddi.DxgkDdiRestartFromTimeout==TgGpuDdiRestartFromTimeout);
    expected_preemption=500;expected_preempt_completed=17;RequestPreempt(&e,500);
    CHECK(q.Preemptions==1 && PoolsInUse()==pools+1 && preemption_interrupts==events);
    CHECK(TgGpuServiceWorkQueue(&q,NULL,NULL)==STATUS_SUCCESS && preemption_interrupts==events+1);
    CHECK(!q.Preemptions && PoolsInUse()==pools && e.LastCompletedFence==17 && interrupts==dma);
    /* Cancel only the admitted prefix; the newer null-render packet remains. */
    submit.hContext=c;submit.SubmissionFenceId=200;CHECK(SubmitModel(&e,&submit)==STATUS_SUCCESS);
    expected_preemption=501;RequestPreempt(&e,501);
    submit.Flags.NullRendering=1;submit.SubmissionFenceId=201;submit.pDmaBufferPrivateData=&entries[1];
    CHECK(SubmitModel(&e,&submit)==STATUS_SUCCESS && c->References==1);
    accesses=hw.reads+hw.writes;
    CHECK(TgGpuServiceWorkQueue(&q,NULL,NULL)==STATUS_SUCCESS && preemption_interrupts==events+2);
    CHECK(!c->References && !entries[0].Work.Engine && e.LastCompletedFence==17 && interrupts==dma);
    CHECK(hw.reads+hw.writes==accesses && !IsListEmpty(&q.Pending));
    expected_fence=expected_driver_fence=201;
    CHECK(TgGpuServiceWorkQueue(&q,NULL,NULL)==STATUS_SUCCESS && e.LastCompletedFence==201);
    /* A real in-flight record must finish and retire before preemption; the
     * second captured record is canceled without touching its command bytes. */
    submit.Flags.Value=0;submit.SubmissionFenceId=202;submit.pDmaBufferPrivateData=&entries[0];
    CHECK(SubmitModel(&e,&submit)==STATUS_SUCCESS);
    submit.SubmissionFenceId=203;submit.pDmaBufferPrivateData=&entries[1];
    CHECK(SubmitModel(&e,&submit)==STATUS_SUCCESS);
    ((UCHAR *)e.Commands.CpuAddress)[TG_GPU_COMMAND_BYTES-1]=0xa5;
    CHECK(TgGpuServiceWorkQueue(&q,&io,&power)==STATUS_PENDING && c->References==2);
    CHECK(q.CurrentOwned && q.CurrentOwned->Snapshot && q.CurrentOwned->Snapshot->Bytes==40);
    CHECK(q.Current.DmaAddress==q.Current.InternalVa+TG_GPU_COMMAND_CAPTURE &&
        !*(UINT *)((UCHAR *)e.Commands.CpuAddress+TG_GPU_COMMAND_CAPTURE) &&
        !((UCHAR *)e.Commands.CpuAddress)[TG_GPU_COMMAND_BYTES-1]);
    expected_preemption=502;expected_preempt_completed=202;RequestPreempt(&e,502);
    submit.Flags.NullRendering=1;submit.SubmissionFenceId=204;submit.pDmaBufferPrivateData=&entries[2];
    CHECK(SubmitModel(&e,&submit)==STATUS_SUCCESS);
    CHECK(TgGpuServiceWorkQueue(&q,&io,&power)==STATUS_PENDING && c->References==2);
    CHECK(preemption_interrupts==events+2 && e.LastCompletedFence==201);
    hw.gpu_fence=q.Current.Expected;expected_fence=expected_driver_fence=202;notify_references=1;
    CHECK(TgGpuServiceWorkQueue(&q,&io,&power)==STATUS_SUCCESS && e.LastCompletedFence==202);
    CHECK(preemption_interrupts==events+2 && c->References==1);notify_references=0;accesses=hw.reads+hw.writes;
    CHECK(TgGpuServiceWorkQueue(&q,&io,&power)==STATUS_SUCCESS && preemption_interrupts==events+3);
    CHECK(hw.reads+hw.writes==accesses && !c->References && e.LastCompletedFence==202);
    expected_fence=expected_driver_fence=204;
    CHECK(TgGpuServiceWorkQueue(&q,NULL,NULL)==STATUS_SUCCESS && e.LastCompletedFence==204);
    /* Several preemption fences must each notify exactly once, in order. */
    submit.Flags.Value=0;submit.SubmissionFenceId=205;submit.pDmaBufferPrivateData=&entries[0];
    CHECK(SubmitModel(&e,&submit)==STATUS_SUCCESS);RequestPreempt(&e,503);
    submit.SubmissionFenceId=206;submit.pDmaBufferPrivateData=&entries[1];
    CHECK(SubmitModel(&e,&submit)==STATUS_SUCCESS);RequestPreempt(&e,504);
    expected_preemption=503;expected_preempt_completed=204;notify_references=1;
    CHECK(TgGpuServiceWorkQueue(&q,NULL,NULL)==STATUS_SUCCESS && q.Preemptions==1 && c->References==1);
    expected_preemption=504;notify_references=0;
    CHECK(TgGpuServiceWorkQueue(&q,NULL,NULL)==STATUS_SUCCESS && !q.Preemptions && !c->References);
    CHECK(preemption_interrupts==events+5 && e.LastCompletedFence==204 && PoolsInUse()==pools);
    /* A rejected fence behind an aborted packet must not overtake it. */
    submit.SubmissionFenceId=207;submit.pDmaBufferPrivateData=&entries[0];
    CHECK(SubmitModel(&e,&submit)==STATUS_SUCCESS);
    submit.SubmissionFenceId=208;submit.DmaBufferSize=3;submit.pDmaBufferPrivateData=&entries[1];
    CHECK(SubmitModel(&e,&submit)==STATUS_INVALID_PARAMETER);
    RequestPreempt(&e,505);expected_preemption=505;expected_preempt_completed=204;
    CHECK(TgGpuServiceWorkQueue(&q,NULL,NULL)==STATUS_SUCCESS && e.LastCompletedFence==204 && !c->References);
    CHECK(preemption_interrupts==events+6 && PoolsInUse()==pools);
    /* Failed interrupt synchronization retains the owned marker until reset. */
    RequestPreempt(&e,506);fail_sync=TRUE;
    CHECK(TgGpuServiceWorkQueue(&q,NULL,NULL)==STATUS_UNSUCCESSFUL && e.Notification==&q.Current);
    CHECK(PoolsInUse()==pools+1 && preemption_interrupts==events+6);
    fail_sync=FALSE;CHECK(TgGpuResetWorkQueue(&q,NULL,&power,&clock)==STATUS_SUCCESS);
    CHECK(!e.Notification && !q.Current.Engine && PoolsInUse()==pools && e.LastCompletedFence==204);
    CHECK(TgGpuResumeWorkQueue(&q,&power)==STATUS_SUCCESS);
    fail_pool=TRUE;accesses=hw.reads+hw.writes;RequestPreempt(&e,507);fail_pool=FALSE;
    CHECK(e.RecoveryPending && e.RecoveryNotified && e.RecoveryStatus==STATUS_NO_MEMORY && q.Paused);
    CHECK(recovery_interrupts==faults+1 && preemption_interrupts==events+6 && e.LastCompletedFence==204);
    CHECK(hw.reads+hw.writes==accesses && PoolsInUse()==pools);
    e.Resetting=TRUE;
    CHECK(TgGpuRequestEngineRecovery(&e,STATUS_DEVICE_DATA_ERROR)==STATUS_DEVICE_NOT_READY);
    CHECK(recovery_interrupts==faults+1 && e.RecoveryStatus==STATUS_NO_MEMORY);e.Resetting=FALSE;
    CHECK(TgGpuStopWorkQueue(&q)==STATUS_SUCCESS);e.HardwareOwner=NULL;
    notify_engine=NULL;notify_context=NULL;
    CHECK(TgGpuDdiDestroyContext(c)==STATUS_SUCCESS && TgGpuDdiDestroyDevice(device.hDevice)==STATUS_SUCCESS);
    CHECK(TgGpuDdiDestroyProcess(&e,process.hKmdProcess)==STATUS_SUCCESS && TgGpuStopContextEngine(&e)==STATUS_SUCCESS);
    for(UINT i=0;i<64;++i)CHECK(!pool_used[i] && !memory_used[i]);
    for(UINT i=0;i<4;++i)CHECK(!graphics_used[i]);
}

static VOID TestCommandSnapshots(void)
{
    static struct {struct tg_gpu_submit_request Header;UINT Words[TG_GPU_SUBMIT_MAX_BYTES/4];} payload;
    TG_GPU_COMMAND_SNAPSHOT *snapshot=NULL;UINT pools=PoolsInUse();
    DXGKARG_SUBMITCOMMANDVIRTUAL a={.DmaBufferSize=TG_GPU_SUBMIT_MAX_BYTES,
        .pDmaBufferPrivateData=&payload,.DmaBufferPrivateDataSize=sizeof(payload),
        .DmaBufferUmdPrivateDataSize=sizeof(payload)};
    payload.Header=(struct tg_gpu_submit_request){TG_GPU_SUBMIT_ABI,sizeof(payload),a.DmaBufferSize,0};
    CHECK(TgGpuCaptureCommandSnapshot(&a,&snapshot)==STATUS_SUCCESS && snapshot &&
        snapshot->Bytes==TG_GPU_SUBMIT_MAX_BYTES && PoolsInUse()==pools+1);
    payload.Words[0]=0x20010014;CHECK(!snapshot->Words[0]);
    TgGpuFreeCommandSnapshot(snapshot);snapshot=NULL;
    CHECK(TgGpuCaptureCommandSnapshot(&a,&snapshot)==STATUS_INVALID_PARAMETER && !snapshot);
    payload.Words[0]=0;payload.Header.reserved=1;
    CHECK(TgGpuCaptureCommandSnapshot(&a,&snapshot)==STATUS_INVALID_PARAMETER);
    payload.Header.reserved=0;payload.Header.version++;
    CHECK(TgGpuCaptureCommandSnapshot(&a,&snapshot)==STATUS_INVALID_PARAMETER);
    payload.Header.version--;payload.Header.size--;
    CHECK(TgGpuCaptureCommandSnapshot(&a,&snapshot)==STATUS_INVALID_PARAMETER);
    payload.Header.size++;a.DmaBufferUmdPrivateDataSize--;
    CHECK(TgGpuCaptureCommandSnapshot(&a,&snapshot)==STATUS_INVALID_PARAMETER);
    a.DmaBufferUmdPrivateDataSize++;a.DmaBufferPrivateDataSize--;
    CHECK(TgGpuCaptureCommandSnapshot(&a,&snapshot)==STATUS_INVALID_PARAMETER);
    a.DmaBufferPrivateDataSize++;a.DmaBufferSize+=4;
    CHECK(TgGpuCaptureCommandSnapshot(&a,&snapshot)==STATUS_INVALID_PARAMETER);
    a.DmaBufferSize-=4;fail_pool=TRUE;
    CHECK(TgGpuCaptureCommandSnapshot(&a,&snapshot)==STATUS_NO_MEMORY && !snapshot);fail_pool=FALSE;
    a.pDmaBufferPrivateData=NULL;
    CHECK(TgGpuCaptureCommandSnapshot(&a,&snapshot)==STATUS_INVALID_PARAMETER && PoolsInUse()==pools);
}

void start(void)
{
    static TG_GPU_CONTEXT_ENGINE e,other;
    static TG_GPU_WORK_QUEUE queue;
    static TG_GPU_QUEUE_ENTRY entries[4];
    static _Alignas(PAGE_SIZE) UCHAR table[TG_GPU_TABLE_BYTES];
    static TG_GPU_TABLE_SEGMENT tables={.Memory={(HANDLE)4,table,{.QuadPart=0x90000000},sizeof(table)},.SegmentId=1};
    DXGKRNL_INTERFACE dxgk={0};DRIVER_INITIALIZATION_DATA ddi={0};
    DXGKARG_CREATEPROCESS process={.hDxgkProcess=(HANDLE)100},failed={0};
    DXGKARG_CREATEDEVICE device={.hDevice=(HANDLE)101};
    DXGKARG_CREATECONTEXT create={.hContext=(HANDLE)102,.EngineAffinity=1};
    DXGKARG_SETROOTPAGETABLE root={.Address={1,0,0},.NumEntries=2};
    DXGKARG_SUBMITCOMMANDVIRTUAL submit={.DmaBufferVirtualAddress=0x10000,.DmaBufferSize=40,.SubmissionFenceId=7};
    DXGKARG_QUERYCURRENTFENCE query={0};
    TG_GPU_CONTEXT *c,*c2; TG_GPU_CONTEXT_WORK work={0},blocked={0};
    struct tg_gpu_power power={.owned=true,.ready=true,.stage=TG_GPU_STAGE_READY,
        .pmic_address=0x33,.original_enable=0x40,.original_voltage=50};
    struct tg_gpu_clock clock={0};
    struct tg_gpu_power_io io={.context=&hw,.read32=Read,.write32=Write,.delay_us=Delay,
        .read_pmic=ReadPmic,.write_pmic=WritePmic};
    UINT32 *instance,*fence;UINT i,before;ULONG written;UINT64 pde;
    struct tg_gpu_channel rejected={0};
    static const char pass[]="PASS: WDK private GR images/mappings/reset epochs, adapter/LUID/512MiB ABI and ordered queue (controlled substitutes; no live WDDM or GPU pass)\r\n";
    TestCommandSnapshots();hw.fifo_fault=0x100;before=hw.writes;
    CHECK(tg_gpu_channel_submit(&io,&power,0x90000000,0x91004000,0x91003000,0x13000,&e,&rejected)==TG_GPU_CHANNEL_FAULT);
    CHECK(!rejected.referenced && hw.writes==before && rejected.fifo_interrupt==0x100);
    hw.fifo_fault=0;
    dxgk.Version=DXGKDDI_INTERFACE_VERSION_WDDM2_4;dxgk.DeviceHandle=(HANDLE)1;
    dxgk.DxgkCbAllocateContiguousMemory=Allocate;dxgk.DxgkCbFreeContiguousMemory=Free;
    dxgk.DxgkCbSynchronizeExecution=Synchronize;dxgk.DxgkCbNotifyInterrupt=Interrupt;
    dxgk.DxgkCbQueueDpc=Queue;dxgk.DxgkCbNotifyDpc=Dpc;
    dxgk.DxgkCbReserveGpuVirtualAddressRange=Reserve;
    dxgk.DxgkCbAcquireHandleData=AcquireHandle;dxgk.DxgkCbReleaseHandleData=ReleaseHandle;
    CHECK(NT_SUCCESS(TgGpuInitializeContextEngine(&e,&dxgk,&tables)));
    CHECK(NT_SUCCESS(TgGpuInitializeContextEngine(&other,&dxgk,&tables)));
    TestGraphicsTemplate(&e);
    TgGpuRegisterContextDdis(&ddi);
    TestAdapterQueries(&e,&ddi);
    fail_pool=TRUE;CHECK(ddi.DxgkDdiCreateProcess(&e,&failed)==STATUS_NO_MEMORY && !failed.hKmdProcess);fail_pool=FALSE;
    fail_reserve=TRUE;CHECK(ddi.DxgkDdiCreateProcess(&e,&process)==STATUS_INSUFFICIENT_RESOURCES);fail_reserve=FALSE;
    CHECK(NT_SUCCESS(ddi.DxgkDdiCreateProcess(&e,&process)));
    CHECK(((TG_GPU_PROCESS *)process.hKmdProcess)->Internal.Base==TG_GPU_INTERNAL_COVERAGE);
    {
        UINT64 *pt=((TG_GPU_PROCESS *)process.hKmdProcess)->Internal.Leaf.CpuAddress;
        CHECK(pt[0]==UINT64_C(0x100910007) && pt[1]==UINT64_C(0x100910015));
        CHECK(pt[2]==UINT64_C(0x100910023) && pt[3]==0 && pt[TG_GPU_TABLE_ENTRIES-1]==0);
    }
    device.hKmdProcess=process.hKmdProcess;
    CHECK(ddi.DxgkDdiCreateDevice(&other,&device)==STATUS_INVALID_PARAMETER);
    CHECK(NT_SUCCESS(ddi.DxgkDdiCreateDevice(&e,&device)));
    TestAllocations(&e,&other,&ddi,device.hDevice);
    CHECK(ddi.DxgkDdiDestroyProcess(&e,process.hKmdProcess)==STATUS_DEVICE_BUSY);
    create.Flags.VirtualAddressing=1;
    fail_allocate=TRUE;
    CHECK(ddi.DxgkDdiCreateContext(device.hDevice,&create)==STATUS_INSUFFICIENT_RESOURCES);
    CHECK(((TG_GPU_DEVICE *)device.hDevice)->Contexts==0);fail_allocate=FALSE;
    CHECK(NT_SUCCESS(ddi.DxgkDdiCreateContext(device.hDevice,&create)));c=create.hContext;
    /* Exercise packed GR address rebasing above 4 GiB before the first work.
     * Preparation will build the real reserved-base image because generation
     * is still zero. This is CPU format validation, never a GPU write.
     */
    CHECK(NT_SUCCESS(TgGpuBuildContextGraphics(&e.Graphics,&c->Instance,UINT64_C(0x140000000),1234)));
    instance=c->Instance.CpuAddress;
    CHECK(instance[132]==0x40100004 && instance[133]==1);
    CHECK(((UINT32 *)((UCHAR *)instance+TG_GPU_CONTEXT_GR_OFFSET))[0xa8/4]==1);
    CHECK(create.ContextInfo.Caps.NoPatchingRequired && !create.ContextInfo.Caps.DriverManagesResidency);
    CHECK(create.ContextInfo.DmaBufferPrivateDataSize==sizeof(TG_GPU_QUEUE_ENTRY)+
        sizeof(struct tg_gpu_submit_request)+TG_GPU_SUBMIT_MAX_BYTES);
    create.hContext=(HANDLE)103;CHECK(NT_SUCCESS(ddi.DxgkDdiCreateContext(device.hDevice,&create)));c2=create.hContext;
    CHECK(c!=c2 && ((TG_GPU_DEVICE *)device.hDevice)->Contexts==2);
    CHECK(ddi.DxgkDdiDestroyDevice(device.hDevice)==STATUS_DEVICE_BUSY);
    root.hContext=c;ddi.DxgkDdiSetRootPageTable(&e,&root);
    instance=c->Instance.CpuAddress;
    fence=(UINT32 *)((UCHAR *)e.Commands.CpuAddress+TG_GPU_COMMAND_FENCE);
    hw.cpu_fence=fence;
    CHECK(c->Root.Valid && instance[128]==0x90000804 && instance[130]==0x07fff000);
    submit.hContext=c;CHECK(NT_SUCCESS(TgGpuAcquireContextWork(&e,&submit,&work)));
    CHECK(work.Root.Valid && c->References==1 && work.FencePhysicalAddress==0x91002048);
    CHECK(*fence==~work.Expected && instance[0x800/4]==0);
    CHECK(TgGpuSignalContextWork(&e,&work)==STATUS_DEVICE_NOT_READY);
    ((UINT64 *)table)[1]=0; /* VidMm made the reserved root entry invalid */
    CHECK(NT_SUCCESS(TgGpuPrepareContextWork(&e,&work)));
    TestGraphicsImage(&e,c,&((TG_GPU_PROCESS *)process.hKmdProcess)->Internal);
    CHECK(tg_gpu_pde_small((UINT64)((TG_GPU_PROCESS *)process.hKmdProcess)->Internal.Leaf.SystemPhysicalAddress.QuadPart,&pde));
    CHECK(((UINT64 *)table)[1]==pde && ((UINT64 *)table)[0]==0);
    CHECK(work.Prepared && instance[128]==0x90000804 && instance[130]==0x07fff000);
    CHECK(TgGpuPrepareContextWork(&e,&work)==STATUS_INVALID_DEVICE_STATE);
    CHECK(TgGpuAcquireContextWork(&e,&submit,&blocked)==STATUS_DEVICE_BUSY && !blocked.Context);
    CHECK(ddi.DxgkDdiDestroyContext(c)==STATUS_DEVICE_BUSY);
    CHECK(TgGpuSignalContextWork(&e,&work)==STATUS_DEVICE_NOT_READY && !interrupts);
    before=hw.reads+hw.writes;
    CHECK(TgGpuSubmitContextWork(&e,&work,&io,&power)==STATUS_DEVICE_NOT_READY);
    CHECK(!work.Attempted && !e.Channel.referenced && hw.reads+hw.writes==before);
    e.UserdBar1Offset=0x13000;e.UserdBar1Ready=TRUE; /* explicit cold-backend substitute */
    hw.fifo_fault=0x40000000; /* normal runlist event cannot reject the channel */
    CHECK(NT_SUCCESS(TgGpuSubmitContextWork(&e,&work,&io,&power)));
    hw.fifo_fault=0;
    CHECK(work.Submitted && e.Channel.referenced && !hw.sched && hw.runlist==2);
    CHECK(e.Channel.bound && e.Channel.submitted && hw.userd_put==2 && hw.doorbells==1);
    CHECK(hw.inst==((UINT32)((UINT64)c->Instance.SystemPhysicalAddress.QuadPart>>12)|0x80000000));
    before=hw.writes;
    CHECK(TgGpuSubmitContextWork(&e,&work,&io,&power)==STATUS_INVALID_DEVICE_STATE && hw.writes==before);
    CHECK(TgGpuContextWorkFinished(&e,&work)==STATUS_PENDING && interrupts==0);
    /* A busy root change invalidates future admission but does not rewrite
     * the queued snapshot or RAMIN while the hardware may still reference it.
     */
    root.NumEntries=3;ddi.DxgkDdiSetRootPageTable(&e,&root);
    CHECK(!c->Root.Valid && c->RootStatus==STATUS_GRAPHICS_ALLOCATION_BUSY);
    CHECK(work.Root.Entries==2 && instance[130]==0x07fff000);
    *fence=work.Expected; /* explicit simulated GPU write */
    CHECK(TgGpuContextWorkFinished(&e,&work)==STATUS_SUCCESS);
    CHECK(TgGpuSignalContextWork(&e,&work)==STATUS_DEVICE_NOT_READY);
    CHECK(TgGpuObserveContextWork(&e,&work,&io,&power)==STATUS_PENDING);
    *fence=~work.Expected;hw.gpu_fence=work.Expected;hw.delay_reference=TRUE;
    CHECK(TgGpuObserveContextWork(&e,&work,&io,&power)==STATUS_SUCCESS);
    CHECK(work.GpuCompleted && !work.CpuCompleted && *fence!=work.Expected);
    hw.fail_preempt=TRUE;
    CHECK(TgGpuQuiesceContextWork(&e,&work,&io,&power)==STATUS_IO_TIMEOUT);
    CHECK(e.Channel.referenced && hw.inst && hw.runlist && c->References==1 && !interrupts);
    CHECK(ddi.DxgkDdiDestroyContext(c)==STATUS_DEVICE_BUSY);
    hw.fail_preempt=FALSE;
    CHECK(NT_SUCCESS(TgGpuQuiesceContextWork(&e,&work,&io,&power)) && work.Retired && c->References==1);
    CHECK(!e.Channel.referenced && !hw.inst && !hw.runlist && hw.flushes==4 && hw.tlbs==2);
    CHECK(TgGpuSignalContextWork(&e,&work)==STATUS_DEVICE_NOT_READY && !interrupts);
    CHECK(TgGpuObserveContextWork(&e,&work,&io,&power)==STATUS_SUCCESS && work.CpuCompleted);
    CHECK(*fence==work.Expected && hw.flushes==6);hw.delay_reference=FALSE;
    fail_sync=TRUE;CHECK(TgGpuSignalContextWork(&e,&work)==STATUS_UNSUCCESSFUL && !work.Notified);
    CHECK(!interrupts && !c->References && !work.Context && e.Notification==&work);
    CHECK(TgGpuInitializeWorkQueue(&queue,&e)==STATUS_DEVICE_BUSY && !e.QueueOwner);
    CHECK(TgGpuCaptureContextWork(&e,&submit,&work)==STATUS_DEVICE_BUSY);
    fail_sync=FALSE;expected_fence=7;notify_context=c;notify_references=0;
    CHECK(NT_SUCCESS(TgGpuSignalContextWork(&e,&work)) && work.Notified);
    CHECK(interrupts==1 && queues==1 && !c->References && !e.Notification);
    CHECK(NT_SUCCESS(TgGpuSignalContextWork(&e,&work)) && interrupts==1);
    ddi.DxgkDdiDpcRoutine(&e);CHECK(dpcs==1);
    CHECK(NT_SUCCESS(ddi.DxgkDdiQueryCurrentFence(&e,&query)) && query.CurrentFence==7);
    CHECK(NT_SUCCESS(TgGpuRetireContextWork(&e,&work,&io,&power)) && !work.Context && !c->References);
    CHECK(!e.Channel.referenced && !hw.inst && !hw.runlist && hw.flushes==6 && hw.tlbs==2);
    CHECK(TgGpuAcquireContextWork(&e,&submit,&work)==STATUS_DEVICE_NOT_READY); /* lost root is internal state */
    ddi.DxgkDdiSetRootPageTable(&e,&root);CHECK(c->Root.Valid && instance[130]==0x0bfff000);
    root.Address.SegmentOffset=sizeof(table)-PAGE_SIZE;root.NumEntries=TG_GPU_TABLE_ENTRIES;
    ddi.DxgkDdiSetRootPageTable(&e,&root);CHECK(!c->Root.Valid);
    CHECK(TgGpuAcquireContextWork(&e,&submit,&work)==STATUS_DEVICE_NOT_READY);
    root.Address.SegmentOffset=0;root.NumEntries=2;ddi.DxgkDdiSetRootPageTable(&e,&root);
    submit.DmaBufferVirtualAddress=TG_GPU_INTERNAL_COVERAGE;
    CHECK(TgGpuAcquireContextWork(&e,&submit,&work)==STATUS_INVALID_PARAMETER); /* KMD-only range */
    submit.DmaBufferVirtualAddress=c->Root.VaLimit-4;
    CHECK(TgGpuAcquireContextWork(&e,&submit,&work)==STATUS_INVALID_PARAMETER);
    submit.DmaBufferVirtualAddress=0x10000;submit.SubmissionFenceId=8;
    CHECK(NT_SUCCESS(TgGpuAcquireContextWork(&e,&submit,&work)));
    CHECK(NT_SUCCESS(TgGpuPrepareContextWork(&e,&work)));
    CHECK(NT_SUCCESS(TgGpuSubmitContextWork(&e,&work,&io,&power)));
    CHECK(TgGpuContextWorkFinished(&e,&work)==STATUS_PENDING);
    hw.fail_reset=TRUE;
    CHECK(TgGpuResetContextWork(&e,&work,&io,&power,&clock)==STATUS_DEVICE_DATA_ERROR);
    /* power_down now stops at the failed reset readback, before changing its
     * ready/clock/rail state. All DMA backing must remain owned for retry. */
    CHECK(e.Channel.referenced && c->References==1 && power.owned && power.ready);
    CHECK(!work.Submitted && hw.clamp==1 && !hw.reset);
    hw.fail_reset=FALSE;
    CHECK(NT_SUCCESS(TgGpuResetContextWork(&e,&work,&io,&power,&clock)));
    CHECK(!e.Channel.referenced && !c->References && !work.Context && !power.owned && !e.UserdBar1Ready);
    CHECK(hw.enable==0x40 && hw.voltage==50 && (hw.reset&0x01000000));
    CHECK(e.LastCompletedFence==7 && interrupts==1);
    CHECK(c->GraphicsGeneration!=e.GraphicsGeneration);
    CHECK(TgGpuAcquireContextWork(&e,&submit,&work)==STATUS_DEVICE_NOT_READY);
    c=RecreateContext(&ddi,device.hDevice,&e,c,&root);submit.hContext=c;instance=c->Instance.CpuAddress;
    /* Cancellation before any hardware attempt performs no MMIO and does not
     * manufacture a completed submission fence either.
     */
    before=hw.reads+hw.writes;
    CHECK(NT_SUCCESS(TgGpuAcquireContextWork(&e,&submit,&work)));
    CHECK(NT_SUCCESS(TgGpuRetireContextWork(&e,&work,NULL,NULL)));
    CHECK(hw.reads+hw.writes==before && e.LastCompletedFence==7);
    /* Start a fresh controlled cold GPU and submit multiple contexts before
     * any fence completes. Admission must not overwrite the active marker.
     */
    power.owned=power.ready=true;e.UserdBar1Ready=TRUE;hw.mc=0x100;hw.pb=hw.map=hw.sched=1;
    hw.clamp=hw.reset=hw.inst=hw.ctrl=hw.preempt=hw.pbdma=hw.runlist=0;
    hw.enable=0x41;hw.voltage=110;
    root.hContext=c2;root.Address.SegmentOffset=PAGE_SIZE;
    ddi.DxgkDdiSetRootPageTable(&e,&root);CHECK(c2->Root.Valid);
    CHECK(NT_SUCCESS(TgGpuInitializeWorkQueue(&queue,&e)));
    submit.DmaBufferPrivateDataSize=sizeof(entries[0]);submit.pDmaBufferPrivateData=&entries[0];
    submit.DmaBufferUmdPrivateDataSize=4;
    CHECK(TgGpuQueueSubmit(&queue,&submit)==STATUS_INVALID_PARAMETER && !c->References);
    submit.DmaBufferUmdPrivateDataSize=0;
    for(i=0;i<4;++i) {
        submit.hContext=(i&1) ? c2 : c;
        submit.pDmaBufferPrivateData=&entries[i];submit.SubmissionFenceId=10+i;
        memset(&entries[i],0xcc,sizeof(entries[i]));
        CHECK(NT_SUCCESS(TgGpuQueueSubmit(&queue,&submit)));
    }
    CHECK(c->References==2 && c2->References==2 && !e.Active);
    CHECK(TgGpuQueueSubmit(&queue,&submit)==STATUS_DEVICE_BUSY);
    CHECK(TgGpuAcquireContextWork(&e,&submit,&blocked)==STATUS_DEVICE_BUSY);
    CHECK(TgGpuServiceWorkQueue(&queue,&io,&power)==STATUS_PENDING);
    CHECK(queue.Current.Context==c && queue.Current.SubmissionFence==10 && *fence==~queue.Current.Expected);
    ((UINT32 *)((UCHAR *)c->Instance.CpuAddress+TG_GPU_CONTEXT_GR_OFFSET))[0x100/4]=0x12345678; /* simulated saved state */
    CHECK(TgGpuServiceWorkQueue(&queue,&io,&power)==STATUS_PENDING && interrupts==1);
    *fence=queue.Current.Expected; /* A CPU word alone must never complete work. */
    CHECK(TgGpuServiceWorkQueue(&queue,&io,&power)==STATUS_PENDING && interrupts==1);
    *fence=~queue.Current.Expected;
    hw.gpu_fence=queue.Current.Expected;expected_fence=10;notify_context=c;notify_references=1;
    reuse_private=&entries[0];
    CHECK(TgGpuServiceWorkQueue(&queue,&io,&power)==STATUS_SUCCESS && !reuse_private);
    CHECK(c->References==1 && c2->References==2 && e.LastCompletedFence==10 && !e.Active);
    CHECK(((UCHAR *)&entries[0])[sizeof(entries[0])-1]==0xa5);
    hw.expected_root=0x900010;
    CHECK(TgGpuServiceWorkQueue(&queue,&io,&power)==STATUS_PENDING && queue.Current.Context==c2);
    {
        TG_GPU_INTERNAL_VA *v=&((TG_GPU_PROCESS *)process.hKmdProcess)->Internal;
        UINT64 owner=v->GraphicsPhysical;
        TgGpuUnmapContextGraphics(v,&c->Instance); /* preserve the other context's mapping */
        CHECK(v->GraphicsPhysical==owner && owner==(UINT64)c2->Instance.SystemPhysicalAddress.QuadPart+TG_GPU_CONTEXT_GR_OFFSET);
    }
    hw.gpu_fence=queue.Current.Expected;expected_fence=11;notify_context=c2;notify_references=1;
    reuse_private=&entries[1];
    CHECK(TgGpuServiceWorkQueue(&queue,&io,&power)==STATUS_SUCCESS && !reuse_private);
    CHECK(c->References==1 && c2->References==1 && e.LastCompletedFence==11);
    hw.expected_root=0x900000;hw.fail_tlb=TRUE;
    CHECK(TgGpuServiceWorkQueue(&queue,&io,&power)==STATUS_IO_TIMEOUT);
    CHECK(((UINT32 *)((UCHAR *)c->Instance.CpuAddress+TG_GPU_CONTEXT_GR_OFFSET))[0x100/4]==0x12345678);
    CHECK(queue.Failure==STATUS_IO_TIMEOUT && e.Channel.referenced && queue.Current.Context==c);
    CHECK(TgGpuServiceWorkQueue(&queue,&io,&power)==STATUS_IO_TIMEOUT && c2->References==1);
    hw.fail_tlb=FALSE;hw.fail_reset=TRUE;
    CHECK(TgGpuResetWorkQueue(&queue,&io,&power,&clock)==STATUS_DEVICE_DATA_ERROR);
    CHECK(queue.Paused && c->References==1 && c2->References==1);
    hw.fail_reset=FALSE;
    CHECK(NT_SUCCESS(TgGpuResetWorkQueue(&queue,&io,&power,&clock)));
    CHECK(!c->References && !c2->References && !queue.Current.Context && IsListEmpty(&queue.Pending));
    CHECK(e.LastCompletedFence==11 && interrupts==3 && !e.Active && !e.Notification);
    submit.hContext=c;
    CHECK(TgGpuCaptureContextWork(&e,&submit,&blocked)==STATUS_DEVICE_NOT_READY && !blocked.Context);
    c=RecreateContext(&ddi,device.hDevice,&e,c,&root);submit.hContext=c;
    CHECK(TgGpuResumeWorkQueue(&queue,&power)==STATUS_DEVICE_NOT_READY);
    power.owned=power.ready=true;
    CHECK(!e.UserdBar1Ready && TgGpuResumeWorkQueue(&queue,&power)==STATUS_DEVICE_NOT_READY);
    /* Model the adapter's cold reinitialization, then fail only synchronized
     * notification. Hardware/context ownership is already retired: reset
     * cancels the engine-only notice without another register access.
     */
    power.owned=power.ready=true;e.UserdBar1Ready=TRUE;hw.mc=0x100;hw.pb=hw.map=hw.sched=1;
    hw.clamp=hw.reset=hw.inst=hw.ctrl=hw.preempt=hw.pbdma=hw.runlist=0;
    CHECK(NT_SUCCESS(TgGpuResumeWorkQueue(&queue,&power)));
    submit.hContext=c;submit.SubmissionFenceId=14;submit.pDmaBufferPrivateData=&entries[0];
    CHECK(NT_SUCCESS(TgGpuQueueSubmit(&queue,&submit)));
    CHECK(TgGpuServiceWorkQueue(&queue,&io,&power)==STATUS_PENDING);
    hw.gpu_fence=queue.Current.Expected;fail_sync=TRUE;
    CHECK(TgGpuServiceWorkQueue(&queue,&io,&power)==STATUS_UNSUCCESSFUL);
    CHECK(!c->References && !queue.Current.Context && e.Notification==&queue.Current);
    fail_sync=FALSE;before=hw.reads+hw.writes;
    CHECK(NT_SUCCESS(TgGpuResetWorkQueue(&queue,&io,&power,&clock)));
    CHECK(hw.reads+hw.writes==before && !e.Notification && e.LastCompletedFence==11 && interrupts==3);
    CHECK(NT_SUCCESS(TgGpuStopWorkQueue(&queue)) && !e.QueueOwner);
    notify_context=NULL;
    before=frees;fail_free=TRUE;CHECK(ddi.DxgkDdiDestroyContext(c)==STATUS_UNSUCCESSFUL);
    CHECK(c->Instance.Handle && frees==before && !c->Closing);fail_free=FALSE;
    CHECK(!((TG_GPU_PROCESS *)process.hKmdProcess)->Internal.GraphicsPhysical);
    CHECK(NT_SUCCESS(ddi.DxgkDdiDestroyContext(c)));
    CHECK(TgGpuStopContextEngine(&e)==STATUS_DEVICE_BUSY);
    CHECK(ddi.DxgkDdiCreateProcess(&e,&failed)==STATUS_DEVICE_NOT_READY);
    CHECK(NT_SUCCESS(ddi.DxgkDdiDestroyContext(c2)));
    CHECK(NT_SUCCESS(ddi.DxgkDdiDestroyDevice(device.hDevice)));
    CHECK(NT_SUCCESS(ddi.DxgkDdiDestroyProcess(&e,process.hKmdProcess)));
    fail_memory_handle=e.Graphics.Memory.Handle;
    CHECK(TgGpuStopContextEngine(&e)==STATUS_UNSUCCESSFUL && !e.Commands.Handle && e.Graphics.Memory.Handle);
    fail_memory_handle=NULL;
    CHECK(NT_SUCCESS(TgGpuStopContextEngine(&e)));
    CHECK(NT_SUCCESS(TgGpuStopContextEngine(&other)));
    for(i=0;i<64;++i) CHECK(!pool_used[i] && !memory_used[i]);
    for(i=0;i<4;++i)CHECK(!graphics_used[i]);
    TestAdapterLifecycle(&dxgk);
    TestPagingDispatch(&dxgk);
    TestPreemption(&dxgk);
    WriteFile(GetStdHandle((ULONG)-11),pass,sizeof(pass)-1,&written,NULL);ExitProcess(0);
}
