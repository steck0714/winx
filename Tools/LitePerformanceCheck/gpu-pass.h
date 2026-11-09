/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LITE_GPU_PASS_H
#define LITE_GPU_PASS_H
#include <stdbool.h>
#include <TegraGpu/windows/control.h>

/* Acceptance of a completed diagnostic report, not proof of shader/Vulkan
 * execution. The caller has already checked the device I/O ABI and size. */
static inline bool gpu_pass(const struct tg_gpu_status *s) {
 return s->tested && !s->power_result && !s->clock_result && !s->execution_result &&
        !s->frontend.result && !s->gr.result && s->gr.stage==15 &&
        s->gr.context_bytes && s->gr.zcull_bytes && s->gr.pm_bytes &&
        !s->acr_firmware_result && !s->acr.result && !s->pmu.result &&
        (!s->gr.priv_security || (s->acr.stage==9 && s->pmu.stage==6 && !s->pmu.reply_code)) &&
        !s->firmware_status && !s->restore_result && !s->resources_owned &&
        s->expected[0]==s->observed[0] && s->expected[1]==s->observed[1];
}
static inline bool gpu_gr_pass(const struct tg_gpu_status *s) {
 return gpu_pass(s) && !s->graphics.result && s->graphics.stage==9 &&
        s->graphics.context_bytes==s->gr.context_bytes &&
        s->graphics.bind_mailbox!=0xffffffff && (s->graphics.bind_mailbox&0x30)==0x10 &&
        s->graphics.save_mailbox!=0xffffffff && (s->graphics.save_mailbox&3)==1 &&
        s->graphics.image_magic==0x600dc0de && s->graphics.image_changed_words;
}
static inline bool gpu_irq_pass(const struct tg_gpu_status *s) {
 return gpu_pass(s) && !s->interrupt.result && !s->interrupt.restore_result &&
        s->irq_gsi==T214_GPU_NONSTALL_GSI && s->irq_expected==2 &&
        s->interrupt.interrupts==s->irq_expected && s->irq_dpcs==s->irq_expected;
}
static inline bool gpu_3d_pass(const struct tg_gpu_status *s) {
 return gpu_gr_pass(s) && s->gr_channel.image_ready==1 && !s->gr_channel.image_result &&
        s->gr_channel.class_id==TG_GPU_GR_ENGINE_CLASS &&
        !s->gr.gr_interrupt && !s->gr.fecs_error_mailbox && !s->gr.fecs_host_interrupt;
}
#endif
