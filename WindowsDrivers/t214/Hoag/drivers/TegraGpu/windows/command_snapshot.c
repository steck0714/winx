/* SPDX-License-Identifier: MIT */
#include "command_snapshot.h"
#include "../core/command_stream.h"
#define TAG 0x53674754u
NTSTATUS TgGpuCaptureCommandSnapshot(const DXGKARG_SUBMITCOMMANDVIRTUAL *a,TG_GPU_COMMAND_SNAPSHOT **out)
{
    struct tg_gpu_submit_request request;TG_GPU_COMMAND_SNAPSHOT *s;
    PAGED_CODE();
    if(!out)return STATUS_INVALID_PARAMETER;
    *out=NULL;
    if(!a || !a->pDmaBufferPrivateData || a->DmaBufferUmdPrivateDataSize<sizeof(request) ||
       a->DmaBufferUmdPrivateDataSize>a->DmaBufferPrivateDataSize ||
       !a->DmaBufferSize || a->DmaBufferSize>TG_GPU_SUBMIT_MAX_BYTES || (a->DmaBufferSize&3))
        return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(&request,a->pDmaBufferPrivateData,sizeof(request));
    if(request.version!=TG_GPU_SUBMIT_ABI || request.reserved || request.command_bytes!=a->DmaBufferSize ||
       request.size!=sizeof(request)+request.command_bytes || request.size!=a->DmaBufferUmdPrivateDataSize)
        return STATUS_INVALID_PARAMETER;
    s=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*s)+request.command_bytes,TAG);
    if(!s)return STATUS_NO_MEMORY;
    s->Bytes=request.command_bytes;
    RtlCopyMemory(s->Words,(const UCHAR *)a->pDmaBufferPrivateData+sizeof(request),s->Bytes);
    if(!tg_gpu_validate_command_stream(s->Words,s->Bytes)) {
        ExFreePoolWithTag(s,TAG);return STATUS_INVALID_PARAMETER;
    }
    *out=s;return STATUS_SUCCESS;
}
VOID TgGpuFreeCommandSnapshot(TG_GPU_COMMAND_SNAPSHOT *s)
{if(s)ExFreePoolWithTag(s,TAG);}
