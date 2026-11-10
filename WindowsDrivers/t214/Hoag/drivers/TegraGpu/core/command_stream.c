/* SPDX-License-Identifier: MIT */
#include "command_stream.h"
#include "nouveau/headers/drf.h"
#include "nouveau/headers/nvidia/classes/clb06f.h"

static bool method_allowed(uint32_t method)
{
    return method>=0x100 || method==0 || method==NVB06F_WFI;
}
bool tg_gpu_validate_command_stream(const uint32_t *words,size_t bytes)
{
    if(!words || !bytes || (bytes&3) || ((uintptr_t)words&3))return false;
    for(size_t n=0;n<bytes/4;) {
        uint32_t header=words[n++],op=NVVAL_GET(header,NVB06F,DMA_INCR,OPCODE);
        uint32_t method=NVVAL_GET(header,NVB06F,DMA_INCR,ADDRESS)*4;
        uint32_t count=NVVAL_GET(header,NVB06F,DMA_INCR,COUNT),last;
        if(header==NVB06F_DMA_NOP)continue;
        if(header&(1u<<12))return false;
        if(op==NVB06F_DMA_IMMD_OPCODE_VALUE) {
            if(!method_allowed(method))return false;
            continue;
        }
        if((op!=NVB06F_DMA_INCR_OPCODE_VALUE && op!=NVB06F_DMA_NONINCR_OPCODE_VALUE &&
            op!=NVB06F_DMA_ONEINCR_OPCODE_VALUE) || !count || count>bytes/4-n)return false;
        last=method;
        if(op==NVB06F_DMA_INCR_OPCODE_VALUE) {
            if(count-1>(0x3ffc-method)/4)return false;
            last=method+(count-1)*4;
        } else if(op==NVB06F_DMA_ONEINCR_OPCODE_VALUE && count>1) {
            if(method==0x3ffc)return false;
            last=method+4;
        }
        if(!method_allowed(method) || !method_allowed(last))return false;
        /* Incrementing packets can span a forbidden host register even if
         * their first and last methods are both individually permitted. */
        if(last!=method && method<0x100)return false;
        n+=count;
    }
    return true;
}
