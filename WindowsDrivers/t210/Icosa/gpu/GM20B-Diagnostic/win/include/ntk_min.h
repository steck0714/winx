/* SPDX-License-Identifier: MIT */
/*
 * ntk_min.h -- the minimal set of Windows kernel-mode declarations the gm20b driver uses.
 *
 * There is no WDK in the build container (and mingw's ddk headers reject ARM64), so this header
 * re-declares the few types, constants and ntoskrnl imports we need, with the WDK's names.
 * Windows ARM64 is LLP64 like AMD64: every layout below equals the AMD64 one.  Do not trust this
 * file on faith: win/tests/check_layout.sh compiles the same offsetof()/sizeof() table against
 * the mingw x86_64 ddk headers and fails the build on any difference.
 */
#ifndef NTK_MIN_H
#define NTK_MIN_H

#include <stddef.h>
#include <stdint.h>

#if defined(NTK_HOST_TEST)
/* Linux unit-test build of the shell logic (win/tests/host_shell_test.c): same sizes as Windows' LLP64. */
#define NTK_IMPORT
typedef unsigned int ULONG;
typedef int LONG;
#else
#define NTK_IMPORT __declspec(dllimport)
typedef unsigned long ULONG;
typedef long LONG;
#endif

typedef LONG NTSTATUS;
typedef unsigned char UCHAR, BOOLEAN;
typedef char CHAR, CCHAR;
typedef unsigned short USHORT, WCHAR;
typedef short CSHORT;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG, ULONG_PTR;
typedef ULONG_PTR SIZE_T;
typedef void *PVOID, *HANDLE, **PHANDLE;
typedef WCHAR *PWSTR;
typedef ULONG *PULONG;
typedef ULONG ACCESS_MASK, DEVICE_TYPE;
typedef UCHAR KIRQL;
typedef CCHAR KPROCESSOR_MODE;

#define TRUE  1
#define FALSE 0
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)

typedef union _LARGE_INTEGER {
	struct { ULONG LowPart; LONG HighPart; } u;
	LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;
typedef LARGE_INTEGER PHYSICAL_ADDRESS, *PPHYSICAL_ADDRESS;

typedef struct _LIST_ENTRY {
	struct _LIST_ENTRY *Flink;
	struct _LIST_ENTRY *Blink;
} LIST_ENTRY;

typedef struct _UNICODE_STRING {
	USHORT Length;
	USHORT MaximumLength;
	PWSTR  Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

/* static UNICODE_STRING from a WCHAR array: NTK_USTR_INIT(arr) */
#define NTK_USTR_INIT(arr) { (USHORT)(sizeof(arr) - sizeof(WCHAR)), (USHORT)sizeof(arr), (PWSTR)(arr) }

typedef struct _OBJECT_ATTRIBUTES {
	ULONG Length;
	HANDLE RootDirectory;
	PUNICODE_STRING ObjectName;
	ULONG Attributes;
	PVOID SecurityDescriptor;
	PVOID SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

#define InitializeObjectAttributes(p, n, a, r, s) \
	do { \
		(p)->Length = sizeof(OBJECT_ATTRIBUTES); \
		(p)->RootDirectory = (r); \
		(p)->Attributes = (a); \
		(p)->ObjectName = (n); \
		(p)->SecurityDescriptor = (s); \
		(p)->SecurityQualityOfService = NULL; \
	} while (0)

typedef struct _IO_STATUS_BLOCK {
	union {
		NTSTATUS Status;
		PVOID Pointer;
	};
	ULONG_PTR Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

typedef struct _DEVICE_OBJECT DEVICE_OBJECT, *PDEVICE_OBJECT;   /* opaque: never dereferenced */
typedef struct _IRP IRP, *PIRP;
typedef struct _DRIVER_OBJECT DRIVER_OBJECT, *PDRIVER_OBJECT;
typedef NTSTATUS (*PDRIVER_DISPATCH)(PDEVICE_OBJECT, PIRP);
typedef void (*PDRIVER_UNLOAD)(PDRIVER_OBJECT);

#define IRP_MJ_CREATE           0x00
#define IRP_MJ_CLOSE            0x02
#define IRP_MJ_DEVICE_CONTROL   0x0e
#define IRP_MJ_MAXIMUM_FUNCTION 0x1b

struct _DRIVER_OBJECT {
	CSHORT Type;
	CSHORT Size;
	PDEVICE_OBJECT DeviceObject;
	ULONG Flags;
	PVOID DriverStart;
	ULONG DriverSize;
	PVOID DriverSection;
	PVOID DriverExtension;
	UNICODE_STRING DriverName;
	PUNICODE_STRING HardwareDatabase;
	PVOID FastIoDispatch;
	PVOID DriverInit;
	PVOID DriverStartIo;
	PDRIVER_UNLOAD DriverUnload;
	PDRIVER_DISPATCH MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1];
};

typedef struct _IO_STACK_LOCATION {
	UCHAR MajorFunction;
	UCHAR MinorFunction;
	UCHAR Flags;
	UCHAR Control;
	union {
		struct {
			ULONG OutputBufferLength;
			ULONG pad0;
			ULONG InputBufferLength;    /* POINTER_ALIGNMENT in the WDK: 8-byte aligned member */
			ULONG pad1;
			ULONG IoControlCode;        /* POINTER_ALIGNMENT */
			ULONG pad2;
			PVOID Type3InputBuffer;
		} DeviceIoControl;
		struct {
			PVOID Argument1;
			PVOID Argument2;
			PVOID Argument3;
			PVOID Argument4;
		} Others;
	} Parameters;
	PDEVICE_OBJECT DeviceObject;
	PVOID FileObject;
	PVOID CompletionRoutine;
	PVOID Context;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;

/* Only the members up to Tail.Overlay.CurrentStackLocation are declared (sizeof(IRP) is NOT the real one). */
struct _IRP {
	CSHORT Type;
	USHORT Size;
	PVOID MdlAddress;
	ULONG Flags;
	union {
		PVOID MasterIrp;
		LONG IrpCount;
		PVOID SystemBuffer;
	} AssociatedIrp;
	LIST_ENTRY ThreadListEntry;
	IO_STATUS_BLOCK IoStatus;
	CCHAR RequestorMode;
	BOOLEAN PendingReturned;
	CHAR StackCount;
	CHAR CurrentLocation;
	BOOLEAN Cancel;
	KIRQL CancelIrql;
	CCHAR ApcEnvironment;
	UCHAR AllocationFlags;
	PIO_STATUS_BLOCK UserIosb;
	PVOID UserEvent;
	union {
		struct { PVOID UserApcRoutine; PVOID UserApcContext; } AsynchronousParameters;
		LARGE_INTEGER AllocationSize;
	} Overlay;
	PVOID CancelRoutine;
	PVOID UserBuffer;
	union {
		struct {
			PVOID DriverContext[4];
			PVOID Thread;
			PVOID AuxiliaryBuffer;
			LIST_ENTRY ListEntry;
			PIO_STACK_LOCATION CurrentStackLocation;
			PVOID OriginalFileObject;
		} Overlay;
	} Tail;
};
#define IoGetCurrentIrpStackLocation(irp) ((irp)->Tail.Overlay.CurrentStackLocation)
#define IO_NO_INCREMENT 0

typedef struct _KEY_VALUE_PARTIAL_INFORMATION {
	ULONG TitleIndex;
	ULONG Type;
	ULONG DataLength;
	UCHAR Data[1];
} KEY_VALUE_PARTIAL_INFORMATION, *PKEY_VALUE_PARTIAL_INFORMATION;
typedef int KEY_VALUE_INFORMATION_CLASS;

typedef enum _MEMORY_CACHING_TYPE {
	MmNonCached = 0,
	MmCached = 1,
	MmWriteCombined = 2
} MEMORY_CACHING_TYPE;

/* ---- constants (all values cross-checked by win/tests/check_layout.sh) ---- */
#define STATUS_SUCCESS                  ((NTSTATUS)0x00000000L)
#define STATUS_DEVICE_BUSY              ((NTSTATUS)0x80000011L)
#define STATUS_UNSUCCESSFUL             ((NTSTATUS)0xC0000001L)
#define STATUS_INVALID_PARAMETER        ((NTSTATUS)0xC000000DL)
#define STATUS_INVALID_DEVICE_REQUEST   ((NTSTATUS)0xC0000010L)
#define STATUS_ACCESS_DENIED            ((NTSTATUS)0xC0000022L)
#define STATUS_BUFFER_TOO_SMALL         ((NTSTATUS)0xC0000023L)

#define FILE_DEVICE_UNKNOWN             0x00000022
#define FILE_DEVICE_SECURE_OPEN         0x00000100
#define SYNCHRONIZE                     0x00100000L
#define FILE_APPEND_DATA                0x00000004
#define FILE_SHARE_READ                 0x00000001
#define FILE_ATTRIBUTE_NORMAL           0x00000080
#define FILE_OPEN_IF                    0x00000003
#define FILE_WRITE_THROUGH              0x00000002
#define FILE_NON_DIRECTORY_FILE         0x00000040
#define FILE_SYNCHRONOUS_IO_NONALERT    0x00000020
#define FILE_CREATED                    0x00000002
#define OBJ_CASE_INSENSITIVE            0x00000040L
#define OBJ_KERNEL_HANDLE               0x00000200L
#define KEY_QUERY_VALUE                 0x0001
#define KEY_SET_VALUE                   0x0002
#define REG_BINARY                      3
#define REG_DWORD                       4
#define KeyValuePartialInformation      2
#define PAGE_READWRITE                  0x04
#define PAGE_NOCACHE                    0x200
#define KernelMode                      0

/* ---- ntoskrnl imports (exactly the ones listed in win/ntoskrnl.def) ---- */
NTK_IMPORT NTSTATUS ZwCreateFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
				 ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
NTK_IMPORT NTSTATUS ZwWriteFile(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, PLARGE_INTEGER, PULONG);
NTK_IMPORT NTSTATUS ZwClose(HANDLE);
NTK_IMPORT NTSTATUS ZwFlushBuffersFile(HANDLE, PIO_STATUS_BLOCK);
NTK_IMPORT NTSTATUS ZwOpenKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
NTK_IMPORT NTSTATUS ZwQueryValueKey(HANDLE, PUNICODE_STRING, KEY_VALUE_INFORMATION_CLASS, PVOID, ULONG, PULONG);
NTK_IMPORT NTSTATUS ZwSetValueKey(HANDLE, PUNICODE_STRING, ULONG, ULONG, PVOID, ULONG);
NTK_IMPORT NTSTATUS ZwFlushKey(HANDLE);
NTK_IMPORT PVOID MmMapIoSpaceEx(PHYSICAL_ADDRESS, SIZE_T, ULONG);
NTK_IMPORT void MmUnmapIoSpace(PVOID, SIZE_T);
NTK_IMPORT PVOID MmAllocateContiguousMemorySpecifyCache(SIZE_T, PHYSICAL_ADDRESS, PHYSICAL_ADDRESS,
							PHYSICAL_ADDRESS, MEMORY_CACHING_TYPE);
NTK_IMPORT void MmFreeContiguousMemory(PVOID);
NTK_IMPORT PHYSICAL_ADDRESS MmGetPhysicalAddress(PVOID);
NTK_IMPORT void KeStallExecutionProcessor(ULONG);
NTK_IMPORT NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE, BOOLEAN, PLARGE_INTEGER);
NTK_IMPORT LARGE_INTEGER KeQueryPerformanceCounter(PLARGE_INTEGER);
NTK_IMPORT NTSTATUS IoCreateDevice(PDRIVER_OBJECT, ULONG, PUNICODE_STRING, DEVICE_TYPE, ULONG, BOOLEAN,
				   PDEVICE_OBJECT *);
NTK_IMPORT void IoDeleteDevice(PDEVICE_OBJECT);
NTK_IMPORT NTSTATUS IoCreateSymbolicLink(PUNICODE_STRING, PUNICODE_STRING);
NTK_IMPORT NTSTATUS IoDeleteSymbolicLink(PUNICODE_STRING);
NTK_IMPORT void IofCompleteRequest(PIRP, CCHAR);

#endif /* NTK_MIN_H */
