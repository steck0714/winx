/** @file
  Default exception handler

  Copyright (c) 2008 - 2010, Apple Inc. All rights reserved.<BR>
  Copyright (c) 2011 - 2014, ARM Ltd. All rights reserved.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/PeCoffGetEntryPointLib.h>
#include <Library/PrintLib.h>
#include <Library/ArmDisassemblerLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Guid/DebugImageInfoTable.h>
#include <Protocol/DebugSupport.h>
#include <Protocol/LoadedImage.h>

//
// winx: this copy of ArmPkg's DefaultExceptionHandler also draws the dump on the panel framebuffer (see FbText.c),
// even in RELEASE builds where DEBUG() is compiled out. The V-register dump is removed (does not fit on screen).
//
#undef DEBUG
#define DEBUG(Expression)  ExcDebug Expression
#define SerialPortWrite    ExcWrite

VOID
EFIAPI
ExcDebug (
  IN UINTN        ErrorLevel,
  IN CONST CHAR8  *Format,
  ...
  );

UINTN
EFIAPI
ExcWrite (
  IN UINT8  *Buffer,
  IN UINTN  NumberOfBytes
  );

//
// Maximum number of characters to print to serial (UINT8s) and to console if
// available (as UINT16s)
//
#define MAX_PRINT_CHARS  100

STATIC CHAR8  *gExceptionTypeString[] = {
  "Synchronous",
  "IRQ",
  "FIQ",
  "SError"
};

STATIC BOOLEAN  mRecursiveException;

CHAR8 *
GetImageName (
  IN  UINTN  FaultAddress,
  OUT UINTN  *ImageBase,
  OUT UINTN  *PeCoffSizeOfHeaders
  );

STATIC
VOID
DescribeInstructionOrDataAbort (
  IN CHAR8  *AbortType,
  IN UINTN  Iss
  )
{
  CHAR8  *AbortCause;

  switch (Iss & 0x3f) {
    case 0x0: AbortCause = "Address size fault, zeroth level of translation or translation table base register";
      break;
    case 0x1: AbortCause = "Address size fault, first level";
      break;
    case 0x2: AbortCause = "Address size fault, second level";
      break;
    case 0x3: AbortCause = "Address size fault, third level";
      break;
    case 0x4: AbortCause = "Translation fault, zeroth level";
      break;
    case 0x5: AbortCause = "Translation fault, first level";
      break;
    case 0x6: AbortCause = "Translation fault, second level";
      break;
    case 0x7: AbortCause = "Translation fault, third level";
      break;
    case 0x9: AbortCause = "Access flag fault, first level";
      break;
    case 0xa: AbortCause = "Access flag fault, second level";
      break;
    case 0xb: AbortCause = "Access flag fault, third level";
      break;
    case 0xd: AbortCause = "Permission fault, first level";
      break;
    case 0xe: AbortCause = "Permission fault, second level";
      break;
    case 0xf: AbortCause = "Permission fault, third level";
      break;
    case 0x10: AbortCause = "Synchronous external abort";
      break;
    case 0x18: AbortCause = "Synchronous parity error on memory access";
      break;
    case 0x11: AbortCause = "Asynchronous external abort";
      break;
    case 0x19: AbortCause = "Asynchronous parity error on memory access";
      break;
    case 0x14: AbortCause = "Synchronous external abort on translation table walk, zeroth level";
      break;
    case 0x15: AbortCause = "Synchronous external abort on translation table walk, first level";
      break;
    case 0x16: AbortCause = "Synchronous external abort on translation table walk, second level";
      break;
    case 0x17: AbortCause = "Synchronous external abort on translation table walk, third level";
      break;
    case 0x1c: AbortCause = "Synchronous parity error on memory access on translation table walk, zeroth level";
      break;
    case 0x1d: AbortCause = "Synchronous parity error on memory access on translation table walk, first level";
      break;
    case 0x1e: AbortCause = "Synchronous parity error on memory access on translation table walk, second level";
      break;
    case 0x1f: AbortCause = "Synchronous parity error on memory access on translation table walk, third level";
      break;
    case 0x21: AbortCause = "Alignment fault";
      break;
    case 0x22: AbortCause = "Debug event";
      break;
    case 0x30: AbortCause = "TLB conflict abort";
      break;
    case 0x33:
    case 0x34: AbortCause = "IMPLEMENTATION DEFINED";
      break;
    case 0x35:
    case 0x36: AbortCause = "Domain fault";
      break;
    default: AbortCause = "";
      break;
  }

  DEBUG ((DEBUG_ERROR, "\n%a: %a\n", AbortType, AbortCause));
}

STATIC
VOID
DescribeExceptionSyndrome (
  IN UINT32  Esr
  )
{
  CHAR8  *Message;
  UINTN  Ec;
  UINTN  Iss;

  Ec  = Esr >> 26;
  Iss = Esr & 0x00ffffff;

  switch (Ec) {
    case 0x15: Message = "SVC executed in AArch64";
      break;
    case 0x20:
    case 0x21: DescribeInstructionOrDataAbort ("Instruction abort", Iss);
      return;
    case 0x22: Message = "PC alignment fault";
      break;
    case 0x23: Message = "SP alignment fault";
      break;
    case 0x24:
    case 0x25: DescribeInstructionOrDataAbort ("Data abort", Iss);
      return;
    default: return;
  }

  DEBUG ((DEBUG_ERROR, "\n %a \n", Message));
}

#ifndef MDEPKG_NDEBUG
STATIC
CONST CHAR8 *
BaseName (
  IN  CONST CHAR8  *FullName
  )
{
  CONST CHAR8  *Str;

  Str = FullName + AsciiStrLen (FullName);

  while (--Str > FullName) {
    if ((*Str == '/') || (*Str == '\\')) {
      return Str + 1;
    }
  }

  return Str;
}

#endif

/**
  This is the default action to take on an unexpected exception

  Since this is exception context don't do anything crazy like try to allocate memory.

  @param  ExceptionType    Type of the exception
  @param  SystemContext    Register state at the time of the Exception

**/
VOID
DefaultExceptionHandler (
  IN     EFI_EXCEPTION_TYPE  ExceptionType,
  IN OUT EFI_SYSTEM_CONTEXT  SystemContext
  )
{
  CHAR8   Buffer[MAX_PRINT_CHARS];
  CHAR16  UnicodeBuffer[MAX_PRINT_CHARS];
  UINTN   CharCount;
  INT32   Offset;

  if (mRecursiveException) {
    STATIC CHAR8 CONST  Message[] = "\nRecursive exception occurred while dumping the CPU state\n";
    SerialPortWrite ((UINT8 *)Message, sizeof Message - 1);
    CpuDeadLoop ();
  }

  mRecursiveException = TRUE;

  CharCount = AsciiSPrint (Buffer, sizeof (Buffer), "\n\n%a Exception at 0x%016lx\n", gExceptionTypeString[ExceptionType], SystemContext.SystemContextAArch64->ELR);
  SerialPortWrite ((UINT8 *)Buffer, CharCount);

  // Prepare a unicode buffer for ConOut, if applicable, in case the buffer
  // gets reused.
  UnicodeSPrintAsciiFormat (UnicodeBuffer, MAX_PRINT_CHARS, Buffer);

  DEBUG_CODE_BEGIN ();
  CHAR8   *Pdb, *PrevPdb;
  UINTN   ImageBase;
  UINTN   PeCoffSizeOfHeader;
  UINT64  *Fp;
  UINT64  RootFp[2];
  UINTN   Idx;

  PrevPdb = Pdb = GetImageName (SystemContext.SystemContextAArch64->ELR, &ImageBase, &PeCoffSizeOfHeader);
  if (Pdb != NULL) {
    DEBUG ((
      DEBUG_ERROR,
      "PC 0x%012lx (0x%012lx+0x%08x) [ 0] %a\n",
      SystemContext.SystemContextAArch64->ELR,
      ImageBase,
      SystemContext.SystemContextAArch64->ELR - ImageBase,
      BaseName (Pdb)
      ));
  } else {
    DEBUG ((DEBUG_ERROR, "PC 0x%012lx\n", SystemContext.SystemContextAArch64->ELR));
  }

  if ((UINT64 *)SystemContext.SystemContextAArch64->FP != 0) {
    Idx = 0;

    RootFp[0] = ((UINT64 *)SystemContext.SystemContextAArch64->FP)[0];
    RootFp[1] = ((UINT64 *)SystemContext.SystemContextAArch64->FP)[1];
    if (RootFp[1] != SystemContext.SystemContextAArch64->LR) {
      RootFp[0] = SystemContext.SystemContextAArch64->FP;
      RootFp[1] = SystemContext.SystemContextAArch64->LR;
    }

    for (Fp = RootFp; Fp[0] != 0; Fp = (UINT64 *)Fp[0]) {
      Pdb = GetImageName (Fp[1], &ImageBase, &PeCoffSizeOfHeader);
      if (Pdb != NULL) {
        if (Pdb != PrevPdb) {
          Idx++;
          PrevPdb = Pdb;
        }

        DEBUG ((
          DEBUG_ERROR,
          "PC 0x%012lx (0x%012lx+0x%08x) [% 2d] %a\n",
          Fp[1],
          ImageBase,
          Fp[1] - ImageBase,
          Idx,
          BaseName (Pdb)
          ));
      } else {
        DEBUG ((DEBUG_ERROR, "PC 0x%012lx\n", Fp[1]));
      }
    }

    PrevPdb = Pdb = GetImageName (SystemContext.SystemContextAArch64->ELR, &ImageBase, &PeCoffSizeOfHeader);
    if (Pdb != NULL) {
      DEBUG ((DEBUG_ERROR, "\n[ 0] %a\n", Pdb));
    }

    Idx = 0;
    for (Fp = RootFp; Fp[0] != 0; Fp = (UINT64 *)Fp[0]) {
      Pdb = GetImageName (Fp[1], &ImageBase, &PeCoffSizeOfHeader);
      if ((Pdb != NULL) && (Pdb != PrevPdb)) {
        DEBUG ((DEBUG_ERROR, "[% 2d] %a\n", ++Idx, Pdb));
        PrevPdb = Pdb;
      }
    }
  }

  DEBUG_CODE_END ();

  DEBUG ((DEBUG_ERROR, "\n  X0 0x%016lx   X1 0x%016lx   X2 0x%016lx   X3 0x%016lx\n", SystemContext.SystemContextAArch64->X0, SystemContext.SystemContextAArch64->X1, SystemContext.SystemContextAArch64->X2, SystemContext.SystemContextAArch64->X3));
  DEBUG ((DEBUG_ERROR, "  X4 0x%016lx   X5 0x%016lx   X6 0x%016lx   X7 0x%016lx\n", SystemContext.SystemContextAArch64->X4, SystemContext.SystemContextAArch64->X5, SystemContext.SystemContextAArch64->X6, SystemContext.SystemContextAArch64->X7));
  DEBUG ((DEBUG_ERROR, "  X8 0x%016lx   X9 0x%016lx  X10 0x%016lx  X11 0x%016lx\n", SystemContext.SystemContextAArch64->X8, SystemContext.SystemContextAArch64->X9, SystemContext.SystemContextAArch64->X10, SystemContext.SystemContextAArch64->X11));
  DEBUG ((DEBUG_ERROR, " X12 0x%016lx  X13 0x%016lx  X14 0x%016lx  X15 0x%016lx\n", SystemContext.SystemContextAArch64->X12, SystemContext.SystemContextAArch64->X13, SystemContext.SystemContextAArch64->X14, SystemContext.SystemContextAArch64->X15));
  DEBUG ((DEBUG_ERROR, " X16 0x%016lx  X17 0x%016lx  X18 0x%016lx  X19 0x%016lx\n", SystemContext.SystemContextAArch64->X16, SystemContext.SystemContextAArch64->X17, SystemContext.SystemContextAArch64->X18, SystemContext.SystemContextAArch64->X19));
  DEBUG ((DEBUG_ERROR, " X20 0x%016lx  X21 0x%016lx  X22 0x%016lx  X23 0x%016lx\n", SystemContext.SystemContextAArch64->X20, SystemContext.SystemContextAArch64->X21, SystemContext.SystemContextAArch64->X22, SystemContext.SystemContextAArch64->X23));
  DEBUG ((DEBUG_ERROR, " X24 0x%016lx  X25 0x%016lx  X26 0x%016lx  X27 0x%016lx\n", SystemContext.SystemContextAArch64->X24, SystemContext.SystemContextAArch64->X25, SystemContext.SystemContextAArch64->X26, SystemContext.SystemContextAArch64->X27));
  DEBUG ((DEBUG_ERROR, " X28 0x%016lx   FP 0x%016lx   LR 0x%016lx  \n", SystemContext.SystemContextAArch64->X28, SystemContext.SystemContextAArch64->FP, SystemContext.SystemContextAArch64->LR));

  /* We save these as 128bit numbers, but have to print them as two 64bit numbers,
     so swap the 64bit words to correctly represent a 128bit number.  */

  DEBUG ((DEBUG_ERROR, "\n  SP 0x%016lx  ELR 0x%016lx  SPSR 0x%08lx  FPSR 0x%08lx\n ESR 0x%08lx          FAR 0x%016lx\n", SystemContext.SystemContextAArch64->SP, SystemContext.SystemContextAArch64->ELR, SystemContext.SystemContextAArch64->SPSR, SystemContext.SystemContextAArch64->FPSR, SystemContext.SystemContextAArch64->ESR, SystemContext.SystemContextAArch64->FAR));

  DEBUG ((DEBUG_ERROR, "\n ESR : EC 0x%02x  IL 0x%x  ISS 0x%08x\n", (SystemContext.SystemContextAArch64->ESR & 0xFC000000) >> 26, (SystemContext.SystemContextAArch64->ESR >> 25) & 0x1, SystemContext.SystemContextAArch64->ESR & 0x1FFFFFF));

  DescribeExceptionSyndrome (SystemContext.SystemContextAArch64->ESR);

  DEBUG ((DEBUG_ERROR, "\nStack dump:\n"));
  for (Offset = -256; Offset < 256; Offset += 32) {
    DEBUG ((
      DEBUG_ERROR,
      "%c %013lx: %016lx %016lx %016lx %016lx\n",
      Offset == 0 ? '>' : ' ',
      SystemContext.SystemContextAArch64->SP + Offset,
      *(UINT64 *)(SystemContext.SystemContextAArch64->SP + Offset),
      *(UINT64 *)(SystemContext.SystemContextAArch64->SP + Offset + 8),
      *(UINT64 *)(SystemContext.SystemContextAArch64->SP + Offset + 16),
      *(UINT64 *)(SystemContext.SystemContextAArch64->SP + Offset + 24)
      ));
  }

  // Attempt to print that we had a synchronous exception to ConOut.  We do
  // this after the serial logging as ConOut's logging is more complex and we
  // aren't guaranteed to succeed.
  if (gST->ConOut != NULL) {
    gST->ConOut->OutputString (gST->ConOut, UnicodeBuffer);
  }

  ASSERT (FALSE);
  CpuDeadLoop ();
}
