/* Host check of the actual Mariko CPU helper: frequency math, PMIC register,
 * voltage/frequency ordering and the thermal fallback. No hardware claims. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/t214/performance.h"
typedef uint32_t ULONG; typedef uint8_t UCHAR; typedef uintptr_t ULONG_PTR;
typedef int32_t NTSTATUS; typedef bool BOOLEAN; typedef unsigned KIRQL;
typedef void VOID; typedef ULONG *PULONG; typedef void *WDFDEVICE;
typedef union {int64_t QuadPart;struct {uint32_t LowPart;int32_t HighPart;};} LARGE_INTEGER;
typedef struct {void *Buffer;size_t Size;} WDF_MEMORY_DESCRIPTOR;
typedef struct {int64_t Timeout;} WDF_REQUEST_SEND_OPTIONS;
#define TRUE true
#define FALSE false
#define NT_SUCCESS(s) ((s)>=0)
#define STATUS_SUCCESS 0
#define STATUS_DEVICE_NOT_READY -1
#define STATUS_DEVICE_CONFIGURATION_ERROR -2
#define STATUS_INVALID_DEVICE_STATE -3
#define STATUS_DEVICE_DATA_ERROR -4
#define STATUS_DEVICE_PROTOCOL_ERROR -5
#define STATUS_IO_TIMEOUT -6
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
#define WDF_REQUEST_SEND_OPTION_TIMEOUT 1
#define WDF_REL_TIMEOUT_IN_MS(n) (n)
#define WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(m,b,s) (*(m)=(WDF_MEMORY_DESCRIPTOR){(b),(s)})
#define WDF_REQUEST_SEND_OPTIONS_INIT(o,f) ((void)(f),memset((o),0,sizeof(*(o))))
#define KernelMode 0
#define DISPATCH_LEVEL 2
static UCHAR voltage=110; static bool bad_read;
static char events[256];static unsigned count;static ULONG *car;
typedef struct {
 UCHAR *Car; LITE_PERFORMANCE Config; void *Pmic; LARGE_INTEGER PmicIds[2];
 ULONG ConnectionCount,BootPllx,BootCpuKhz;UCHAR BootVout;
 BOOLEAN CpuConfigured,CpuApplied,CpuThrottled;
} SWFAN_DEVICE,*PSWFAN_DEVICE;
static PSWFAN_DEVICE SwFanGetDevice(void *d){return d;}
static void SwFanTrace(const void *n,ULONG v){(void)n;(void)v;}
static NTSTATUS SwFanReadReg(void *t,UCHAR r,UCHAR *v){assert(t && r==0x26);*v=bad_read?0:voltage;return 0;}
static NTSTATUS SwFanOpenTarget(void *d,LARGE_INTEGER i,void **t){(void)d;(void)i;*t=(void*)1;return 0;}
static void WdfObjectDelete(void *p){(void)p;}
static NTSTATUS WdfIoTargetSendWriteSynchronously(void *t,void *r,WDF_MEMORY_DESCRIPTOR *m,void *o,WDF_REQUEST_SEND_OPTIONS *s,ULONG_PTR *n){
 (void)r;(void)o;(void)s;assert(t && m->Size==2);UCHAR *b=m->Buffer;
 assert(b[0]==0x26); /* never the GPU/DRAM outputs */
 voltage=b[1];events[count++]='V';*n=2;return 0;
}
static void KeDelayExecutionThread(int m,bool a,LARGE_INTEGER *d){(void)m;(void)a;(void)d;}
static void KeRaiseIrql(unsigned i,KIRQL *o){(void)i;*o=0;}
static void KeLowerIrql(KIRQL i){(void)i;}
static void KeStallExecutionProcessor(unsigned n){(void)n;}
#define READ_REGISTER_ULONG(p) (*(p))
static void regwrite(ULONG *p,ULONG v){
 assert(p==car+0x20/4 || p==car+0xe0/4 || p==car+0xe4/4);
 events[count++]='C';*p=v;
 if(p==car+0xe0/4 && (v&(1u<<30))) *p|=1u<<27;
}
#define WRITE_REGISTER_ULONG(p,v) regwrite((p),(v))
#include "../src/t214/clock.h"
int main(void){
 ULONG regs[64]={0};car=regs;regs[0x20/4]=0x20008888;
 regs[0xe0/4]=(1u<<30)|(1u<<27)|(2u<<20)|(156u<<8)|2u;
 SWFAN_DEVICE d={0};d.Car=(UCHAR*)regs;d.Config.CpuKhz=LITE_CPU_KHZ;
 d.Config.Status=LitePerfBadConfig;d.Config.PmicAddress=0x33;d.ConnectionCount=3;
 SwCpuPrepare(&d);assert(!d.CpuConfigured);
 /* Regression: the physical Lite reported NoBpmp while CPU remained 998.4 MHz. */
 d.Config.Status=LitePerfNoBpmp;
 SwCpuPrepare(&d);assert(d.CpuConfigured && d.BootCpuKhz==998400);
 count=0;SwCpuTemperature(&d,40,TRUE);
 assert(d.CpuApplied && voltage==174 && SwCpuKhz(&d)==1996800 && events[0]=='V');
 count=0;SwCpuTemperature(&d,80,TRUE);
 assert(!d.CpuApplied && voltage==110 && SwCpuKhz(&d)==998400 && events[count-1]=='V');
 count=0;SwCpuTemperature(&d,75,TRUE);assert(!d.CpuApplied && count==0);
 SwCpuTemperature(&d,70,TRUE);assert(d.CpuApplied);
 SwCpuTemperature(&d,0,FALSE);assert(!d.CpuApplied && voltage==110);
 count=0;bad_read=true;SwCpuTemperature(&d,30,TRUE);
 assert(!d.CpuApplied && SwCpuKhz(&d)==998400 && count==1 && events[0]=='V');
 puts("PASS: CPU independent of EMC failure; M4-only writes, voltage ordering, thermal hysteresis and readback failure");
}
