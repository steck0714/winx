/* SPDX-License-Identifier: MIT */
#include "../core/command_stream.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    uint32_t w[16]={0x20010000,0xb197,0x20020100,0x20010014,0xdeadbeef,0};
    assert(tg_gpu_validate_command_stream(w,24)); /* Data may resemble a header. */
    assert(!tg_gpu_validate_command_stream(NULL,4));
    assert(!tg_gpu_validate_command_stream(w,0) && !tg_gpu_validate_command_stream(w,23));
    assert(!tg_gpu_validate_command_stream((void *)((char *)w+1),4));
    for(unsigned sub=0;sub<8;sub++)for(unsigned op=1;op<=5;op++) {
        if(op==2)continue;
        w[0]=(op<<29)|(sub<<13)|(1<<16)|(0x50>>2);w[1]=123;
        assert(!tg_gpu_validate_command_stream(w,op==4?4:8));
        w[0]=(w[0]&~0xfffu)|(0x100>>2);
        assert(tg_gpu_validate_command_stream(w,op==4?4:8));
        if(op!=4)assert(!tg_gpu_validate_command_stream(w,4));
        w[0]|=1u<<12;assert(!tg_gpu_validate_command_stream(w,op==4?4:8));
    }
    w[0]=0x20030000;assert(!tg_gpu_validate_command_stream(w,16)); /* Host span. */
    w[0]=0x20010fff;assert(tg_gpu_validate_command_stream(w,8));
    w[0]=0x20020fff;assert(!tg_gpu_validate_command_stream(w,12)); /* Method wrap. */
    w[0]=0xa0020fff;assert(!tg_gpu_validate_command_stream(w,12));
    w[0]=0x60020fff;assert(tg_gpu_validate_command_stream(w,12));
    w[0]=0x00010000;assert(!tg_gpu_validate_command_stream(w,4)); /* Subdevice. */
    w[0]=0xe0000000;assert(!tg_gpu_validate_command_stream(w,4)); /* End segment. */
    w[0]=0xc0000040;assert(!tg_gpu_validate_command_stream(w,4)); /* Reserved. */
    w[0]=0x20000040;assert(!tg_gpu_validate_command_stream(w,4)); /* Zero count. */
    puts("Linear B06F packet/reference validation passed");return 0;
}
