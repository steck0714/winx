/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "../gpu-pass.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
 struct tg_gpu_status good={0};
 good.tested=1;good.expected[0]=good.observed[0]=0x12345678;
 good.expected[1]=good.observed[1]=0x87654321;
 good.gr.stage=15;good.gr.context_bytes=79872;good.gr.zcull_bytes=66048;good.gr.pm_bytes=2304;
 good.gr.priv_security=1;good.acr.stage=9;good.pmu.stage=6;
 good.graphics.stage=9;good.graphics.context_bytes=79872;
 good.graphics.bind_mailbox=0x10;good.graphics.save_mailbox=1;
 good.graphics.image_magic=0x600dc0de;good.graphics.image_changed_words=19328;
 good.gr_channel.image_ready=1;good.gr_channel.class_id=TG_GPU_GR_ENGINE_CLASS;
 assert(gpu_pass(&good)&&gpu_gr_pass(&good)&&gpu_3d_pass(&good));
 assert(!gpu_irq_pass(&good));
 struct tg_gpu_status bad;
 for(unsigned test=0;test<15;++test){
  bad=good;
  if(test==0)bad.gr_channel.image_ready=0;
  if(test==1)bad.gr_channel.image_ready=2;
  if(test==2)bad.gr_channel.class_id=0xb06f; /* PBDMA class is not graphics. */
  if(test==3)bad.gr_channel.image_result=TG_GPU_GR_CTX_VISIBILITY;
  if(test==4)bad.gr.gr_interrupt=1;
  if(test==5)bad.gr.fecs_error_mailbox=1;
  if(test==6)bad.observed[0]=0xdeadbeef;
  if(test==7)bad.observed[1]=0xdeadbeef;
  if(test==8)bad.resources_owned=1;
  if(test==9)bad.restore_result=1;
  if(test==10)bad.graphics.image_changed_words=0;
  if(test==11)bad.graphics.stage=8;
  if(test==12)bad.graphics.image_magic=0xa5a5a5a5;
  if(test==13)bad.pmu.reply_code=1;
  if(test==14)bad.gr.fecs_host_interrupt=0x10000;
  assert(!gpu_3d_pass(&bad));
  if(test<6 || test==14)assert(gpu_gr_pass(&bad)); /* Golden/common readback alone is insufficient. */
 }
 bad=good;bad.irq_expected=2;bad.irq_gsi=T214_GPU_NONSTALL_GSI;
 bad.interrupt.interrupts=bad.irq_dpcs=2;assert(gpu_irq_pass(&bad));
 puts("GPU diagnostic acceptance gates: PASS (status fixtures only)");
 return 0;
}
