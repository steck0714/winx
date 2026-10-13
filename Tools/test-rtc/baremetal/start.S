// Bare-metal entry for the AArch64 RTC test (run under qemu-system-aarch64 -M virt, MMU off).
.section .text.boot
.global _start
_start:
    ldr x0, =0x40800000
    mov sp, x0
    bl  main_
1:  wfi
    b   1b
