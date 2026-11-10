#!/usr/bin/env python3
"""Exercise the real miniport snapshot queue without a Windows boot disk."""
from pathlib import Path
import re
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'src/nvtsdhc.c').read_text()
diag = source[source.index('typedef struct _NV_DIAG {'):source.index('} NV_DIAG;') + len('} NV_DIAG;')]
start = source.index('#if NV_DIAG_CLOCK\n/* Snapshot only')
code = source[start:source.index('\n#if NV_DIAG_REG\n\nstatic VOID\nNvDiagThread', start)]
fields = sorted(set(re.findall(r'SdhcExtension->(\w+)', code)) -
                {'Capabilities', 'T214Bounce', 'T214Scr', 'T214SwitchStatus'})
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
typedef uint32_t ULONG; typedef int32_t LONG; typedef int64_t LONG64,LONGLONG;
typedef uint16_t USHORT; typedef uint8_t UCHAR,BOOLEAN;
typedef wchar_t WCHAR; typedef const wchar_t *PCWSTR;
typedef void *PVOID,*HANDLE; typedef void VOID; typedef int KTIMER,KDPC;
typedef int UNICODE_STRING,OBJECT_ATTRIBUTES;
typedef struct { void (*f)(void*); void *context; } WORK_QUEUE_ITEM;
#define _In_
#define NT_ASSERT assert
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
#define UNREFERENCED_PARAMETER(a) ((void)(a))
#define NV_DIAG_CLOCK 1
#define NV_T214_SDR104 1
#define NV_T214_DDR200 1
#define NV_T210_LITE_WINARM_WRITE 1
#define OBJ_KERNEL_HANDLE 1
#define OBJ_CASE_INSENSITIVE 2
#define KEY_SET_VALUE 1
#define REG_OPTION_NON_VOLATILE 0
#define DelayedWorkQueue 0
#define NT_SUCCESS(s) ((s)>=0)
static int in_callback,queued,writes,create_failure;
static uint64_t now;
static ULONG last_commands,last_adma,last_saves;
static WORK_QUEUE_ITEM *work;
static LONG InterlockedExchange(volatile LONG *p,LONG v){LONG old=*p;*p=v;return old;}
static LONG InterlockedCompareExchange(volatile LONG *p,LONG v,LONG cmp){LONG old=*p;if(old==cmp)*p=v;return old;}
static LONG64 InterlockedExchange64(volatile LONG64 *p,LONG64 v){LONG64 old=*p;*p=v;return old;}
static LONG64 InterlockedCompareExchange64(volatile LONG64 *p,LONG64 v,LONG64 cmp){LONG64 old=*p;if(old==cmp)*p=v;return old;}
static uint64_t KeQueryInterruptTime(void){return now;}
static void RtlInitUnicodeString(UNICODE_STRING *s,PCWSTR n){(void)s;(void)n;}
static void InitializeObjectAttributes(OBJECT_ATTRIBUTES*a,UNICODE_STRING*b,int c,void*d,void*e){(void)a;(void)b;(void)c;(void)d;(void)e;}
static int ZwCreateKey(HANDLE *key,int a,OBJECT_ATTRIBUTES*b,int c,void*d,int e,void*f){
 (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;assert(!in_callback);*key=(void*)1;return create_failure?-1:0;
}
static void NvDiagSetValue(HANDLE key,PCWSTR name,ULONG value){
 assert(key && !in_callback);writes++;
 if(!wcscmp(name,L"Commands"))last_commands=value;
 if(!wcscmp(name,L"AdmaBlocks"))last_adma=value;
 if(!wcscmp(name,L"Saves"))last_saves=value;
}
static void ZwClose(HANDLE key){assert(key && !in_callback);}
static void ExInitializeWorkItem(WORK_QUEUE_ITEM*w,void(*f)(void*),void*c){assert(!queued);w->f=f;w->context=c;}
static void ExQueueWorkItem(WORK_QUEUE_ITEM*w,int q){(void)q;assert(!queued);queued=1;work=w;}
static void run_worker(void){assert(queued);queued=0;work->f(work->context);}
'''
prefix += (Path(__file__).resolve().parents[1] / 'src/t214_ddr200_policy.h').read_text()
prefix += diag + '\nstatic NV_DIAG NvDiag;\n'
prefix += 'typedef struct { struct { ULONG BaseClockFrequencyKhz; } Capabilities; void *T214Bounce;\n'
prefix += 'UCHAR T214Scr[8],T214SwitchStatus[64];\n'
prefix += '\n'.join('ULONG ' + name + ';' for name in fields)
prefix += '\n} SDHC_EXTENSION,*PSDHC_EXTENSION;\n'
tests = r'''
int main(void){
 SDHC_EXTENSION slot={0};
 NvDiag.RegistryPath[0]=L'x';NvDiag.Commands=123;slot.T214DmaBlocks=456;
 in_callback=1;NvDiagSaveClock(&slot);assert(queued && writes==0);
 for(int i=0;i<100000;i++)NvDiagSaveClock(&slot);
 assert(queued && writes==0);in_callback=0;
 /* No deferred access to the slot extension or live counters. */
 memset(&slot,0,sizeof(slot));NvDiag.Commands=999;
 run_worker();assert(last_commands==123 && last_adma==456 && last_saves==1);
 int initial_writes=writes;
 for(int i=0;i<100000;i++){now=299999999;in_callback=1;NvDiagSavePeriodic(&slot);}
 assert(!queued && writes==initial_writes);
 now=300000000;NvDiagSavePeriodic(&slot);assert(queued);
 in_callback=0;run_worker();assert(last_commands==999 && last_saves==2);
 /* A clock/identity transition is retained without waiting 30 seconds. */
 in_callback=1;NvDiagSaveClock(&slot);assert(queued);in_callback=0;run_worker();
 assert(last_saves==3);
 /* Early boot registry failure releases the queue; crashdump never queues. */
 now+=300000000;create_failure=1;NvDiagSavePeriodic(&slot);run_worker();assert(!NvDiagSaveQueued);
 create_failure=0;slot.CrashdumpMode=1;NvDiagSaveClock(&slot);assert(!queued);
 slot.CrashdumpMode=0;NvDiag.RegistryPath[0]=0;NvDiagSaveClock(&slot);assert(!queued);
 puts("PASS: queued-only registry I/O, immutable snapshot, bounded refresh and failed-save recovery");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='lite-sd-diag-') as temp:
    path = Path(temp)
    (path / 'test.c').write_text(prefix + code + tests)
    subprocess.run(['clang', '-std=c11', '-Werror', '-Wno-unknown-pragmas',
                    '-fsanitize=address,undefined', '-g', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
