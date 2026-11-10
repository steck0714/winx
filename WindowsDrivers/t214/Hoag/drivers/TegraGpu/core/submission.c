/* SPDX-License-Identifier: MIT
 * RAMFC fields follow nouveau gk104_chan_ramfc_write; see ../NOTICE.
 */
#include "submission.h"
#include "gpfifo.h"
#include "gm20b_mmu.h"
#include "runlist.h"
#include "nouveau/headers/drf.h"
#include "nouveau/headers/nvidia/classes/clb06f.h"

bool tg_gpu_prepare_submission(void *commands, size_t bytes, uint64_t physical,
    uint64_t va, void *instance, size_t instance_bytes, uint64_t instance_physical,
    uint64_t app_va, uint32_t app_bytes, uint32_t fence_value)
{
    struct tg_gpu_gpfifo_entry app, completion, *ring=commands;
    uint32_t *ramfc=instance, *push, *userd, *runlist, i;
    const uint64_t low=UINT64_C(0x80000000), high=UINT64_C(0x100000000);
    if (!commands || !instance || ((uintptr_t)commands & 0xfff) ||
        ((uintptr_t)instance & 0xfff) || bytes<TG_GPU_COMMAND_BYTES || instance_bytes<0x1000 ||
        (physical & 0xfff) || physical<low || physical>high-TG_GPU_COMMAND_BYTES ||
        (instance_physical & 0xfff) || instance_physical<low || instance_physical>high-0x1000 ||
        (va & 0xfff) || va>=TG_GPU_VA_LIMIT || TG_GPU_COMMAND_RUNLIST>TG_GPU_VA_LIMIT-va ||
        (app_bytes & 3) || !fence_value || fence_value==UINT32_MAX ||
        !tg_gpu_build_gpfifo(app_va,app_bytes/4,false,&app) ||
        !tg_gpu_build_gpfifo(va+TG_GPU_COMMAND_PUSH,4,true,&completion))
        return false;
    /* The command ring and this completion packet are never user-writable.
     * Do not offer this privilege bit for an application-provided entry.
     */
    completion.high |= NVDEF(NVB06F,GP_ENTRY1,PRIV,KERNEL);
    for (i=0;i<TG_GPU_COMMAND_CAPTURE/4;++i) ((uint32_t *)commands)[i]=0;
    for (i=0;i<64;++i) ramfc[i]=0; /* preserve PDB/VA-limit at words 128..131 */
    push=(uint32_t *)((uint8_t *)commands+TG_GPU_COMMAND_PUSH);
    userd=(uint32_t *)((uint8_t *)commands+TG_GPU_COMMAND_USERD);
    runlist=(uint32_t *)((uint8_t *)commands+TG_GPU_COMMAND_RUNLIST);
    ring[0]=app; ring[1]=completion;
    /* .52 physically proved that even a KERNEL entry's virtual semaphore
     * store is USER at the MMU. Do not make USERD writable through the process
     * VA. The channel's own Reference update uses its physical USERD backing.
     * WFI explicitly precedes it; consuming the ring is not completion.
     * Full UMD command validation must exclude caller-issued SET_REFERENCE.
     */
    push[0]=NVDEF(NVB06F,DMA_INCR,OPCODE,VALUE) | NVVAL(NVB06F,DMA_INCR,COUNT,1) |
            NVVAL(NVB06F,DMA_INCR,ADDRESS,NVB06F_WFI>>2);
    push[1]=NVDEF(NVB06F,WFI,SCOPE,ALL);
    push[2]=NVDEF(NVB06F,DMA_INCR,OPCODE,VALUE) | NVVAL(NVB06F,DMA_INCR,COUNT,1) |
            NVVAL(NVB06F,DMA_INCR,ADDRESS,NVB06F_SET_REFERENCE>>2);
    push[3]=fence_value;
    *(uint32_t *)((uint8_t *)commands+TG_GPU_COMMAND_FENCE)=~fence_value;
    /* Empty until the hot channel path publishes GP_PUT through BAR1. */
    userd[35]=0;
    ramfc[0]=0;
    ramfc[2]=(uint32_t)(physical+TG_GPU_COMMAND_USERD); /* SoC DRAM: target 0 */
    ramfc[4]=0xface; ramfc[12]=0x102;
    ramfc[18]=(uint32_t)va; ramfc[19]=(uint32_t)(va>>32)|(9u<<16);
    ramfc[33]=0x20400000; ramfc[37]=0x30000001;
    ramfc[39]=0x100; ramfc[43]=0x1f;
    /* HCE physical-transfer privilege stays disabled. */
    ramfc[62]=0x10003080; ramfc[63]=0x10000010;
    tg_gpu_runlist_single(runlist);
    return true;
}
