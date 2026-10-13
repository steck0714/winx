// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// bl33_stub.S - BL33 entry shim for hekate's L4T launcher (SWITCH_HEKATE flavour).
//
// bl33.bin = this 4 KiB stub + TEGRA210_EFI.fd (see Tools/make-bl33.py).
// hekate loads bl33.bin at 0xAA000000 (BL33_LOAD_BASE in bootloader/l4t/l4t.c); BL31 then
// enters it with pc = 0xAA000000 and spsr = SPSR_EL2T (EL2, SP_EL0, D/A/I/F all clear).
//
// hekate contract, read from l4t.c (master 45cd81e) - NOT run on hardware:
//   * For every L4T launch (even with uart_port=0) hekate evaluates
//         *(u32 *)(0xAA000000 + *(u32 *)(0xAA000000 + 0x10))  ==  0xEDFE0DD0 (FDT magic)
//     so the u32 at +0x10 MUST be a small offset that stays inside this image, otherwise hekate
//     dereferences a wild pointer before it ever launches. The word it points to must not be
//     the FDT magic (we are not a DTB; hekate would patch UART strings into it).
//
// What the stub does: mask D/A/I/F (the entry SPSR leaves them unmasked and no vectors are
// installed yet), select SP_ELx, and branch to the FD at stub + 0x1000. The FD's first
// instruction is a PC-relative branch into PrePi's _ModuleEntryPoint, which sets up its own
// stack (ArmPlatformPeiBootAction does `msr SPSel, #1` as well; both are harmless).
//
// Build: aarch64-linux-gnu-as, ld -Ttext=0xAA000000, objcopy -O binary (make-bl33.py does it).

	.text
	.global	_start
_start:
	b	stub_main			// +0x00  entry point (pc = BL33_LOAD_BASE)

	.org	0x10
	.word	stub_tag - _start		// +0x10  offset hekate dereferences (= 0x20)

	.org	0x20
stub_tag:
	.ascii	"WINX"				// +0x20  must differ from the FDT magic
	.ascii	"HKT1"				// +0x24  stub layout tag, verified by make-bl33.py

	.org	0x40
stub_main:
	msr	daifset, #0xf			// mask Debug, SError, IRQ, FIQ
	msr	spsel, #1			// use SP_ELx
	isb
	b	fd_entry			// UEFI FD at stub + 0x1000 (= 0xAA001000)

	.org	0x1000
fd_entry:
