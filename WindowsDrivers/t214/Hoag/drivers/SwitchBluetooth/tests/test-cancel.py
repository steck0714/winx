#!/usr/bin/env python3
"""Exercise the driver's actual TX/cancel/D0 code with deterministic WDF races."""
from pathlib import Path
import os, re, subprocess, tempfile

source = Path(__file__).resolve().parents[1] / 'src/switchbt.c'
text = source.read_text()
names = ('SwBtDoneFull', 'SwBtDoneAdd', 'SwBtDoneFlush', 'SwBtTxBurst',
         'SwBtStartTx', 'SwBtTakeTx', 'SwBtEvtTxCancel', 'SwBtCompleteTx',
         'SwBtEvtDpc', 'SwBtEvtD0Stopping')
functions = []
for name in names:
    match = re.search(r'^(?:static )?(?:VOID|BOOLEAN|WDFREQUEST|NTSTATUS) ' + name + r'\(', text, re.M)
    assert match, name
    start = text.index('{', match.start()); depth = 1; end = start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}'); end += 1
    functions.append(text[match.start():end])

preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef void VOID; typedef uint8_t UCHAR; typedef uint32_t ULONG, *PULONG;
typedef uintptr_t ULONG_PTR; typedef int32_t NTSTATUS; typedef bool BOOLEAN;
typedef int WDF_POWER_DEVICE_STATE;
#define TRUE true
#define FALSE false
#define STATUS_SUCCESS 0
#define STATUS_CANCELLED -1
#define STATUS_DEVICE_NOT_READY -2
#define STATUS_INVALID_PARAMETER -3
#define NT_SUCCESS(s) ((s) >= 0)
#define FIELD_OFFSET(t,f) offsetof(t,f)
#define RTL_NUMBER_OF(x) (sizeof(x)/sizeof((x)[0]))
#define RtlCopyMemory memcpy
#define UNREFERENCED_PARAMETER(x) (void)(x)
#define SWBT_DONE_MAX 32
#define HCI_MAX_PACKET 1025
#define HciPacketCommand 1
#define HciPacketAclData 2
#define UART_LSR 5
#define UART_LSR_THRE 0x20
#define UART_THR 0
#define UART_IER 1
#define UART_IER_TX 2
#define UART_TX_BURST 16
#define RX_RING_SIZE 8192

typedef struct { UCHAR Type; ULONG DataLen; UCHAR Data[HCI_MAX_PACKET]; } BTHX_HCI_READ_WRITE_CONTEXT, *PBTHX_HCI_READ_WRITE_CONTEXT;
typedef struct { void *data; size_t size; } MEMORY, *WDFMEMORY;
typedef struct REQUEST *WDFREQUEST;
typedef struct DEVICE SWBT_DEVICE, *PSWBT_DEVICE, *WDFDEVICE, *WDFOBJECT, *WDFINTERRUPT;
typedef struct QUEUE { PSWBT_DEVICE dev; WDFREQUEST req[8]; unsigned head,tail; } *WDFQUEUE;
struct REQUEST { WDFQUEUE queue; BTHX_HCI_READ_WRITE_CONTEXT in; ULONG out;
 MEMORY input,output; bool marked,racing,already_cancelled; unsigned completions; NTSTATUS status;
 void (*cancel)(WDFREQUEST); };
struct DEVICE { int Lock; WDFINTERRUPT Interrupt; void *Timer;
 WDFQUEUE TxQueue,EventQueue,AclQueue; WDFREQUEST TxRequest;
 ULONG TxRequestLength,CmdsOut,AclOut,CmdLogNext,CmdLog[8];
 BOOLEAN D0Active,TxFrameActive,TxDone,TxBusy;
 UCHAR TxBuffer[HCI_MAX_PACKET+1],RxRing[RX_RING_SIZE];
 ULONG TxLength,TxSent,TxBytes,Ier,RxHead,RxTail; };
typedef struct { ULONG Count; WDFREQUEST Request[SWBT_DONE_MAX]; NTSTATUS Status[SWBT_DONE_MAX]; ULONG_PTR Information[SWBT_DONE_MAX]; } SWBT_DONE, *PSWBT_DONE;
static bool locked; static unsigned timer_stops, wire_length; static UCHAR wire[4096];
static ULONG lsr;
static void WdfSpinLockAcquire(int lock) { (void)lock; assert(!locked); locked=true; }
static void WdfSpinLockRelease(int lock) { (void)lock; assert(locked); locked=false; }
static void WdfInterruptAcquireLock(WDFINTERRUPT irq) { (void)irq; }
static void WdfInterruptReleaseLock(WDFINTERRUPT irq) { (void)irq; }
static void WdfInterruptQueueDpcForIsr(WDFINTERRUPT irq) { (void)irq; }
static PSWBT_DEVICE SwBtGetDevice(WDFDEVICE d) { return d; }
static WDFDEVICE WdfInterruptGetDevice(WDFINTERRUPT d) { return d; }
static WDFDEVICE WdfIoQueueGetDevice(WDFQUEUE q) { return q->dev; }
static WDFQUEUE WdfRequestGetIoQueue(WDFREQUEST r) { return r->queue; }
static NTSTATUS WdfIoQueueRetrieveNextRequest(WDFQUEUE q,WDFREQUEST *r) {
 if(q->head==q->tail)return STATUS_DEVICE_NOT_READY; *r=q->req[q->head++]; return 0;
}
static NTSTATUS WdfRequestRetrieveInputMemory(WDFREQUEST r,WDFMEMORY *m) { assert(!r->completions); *m=&r->input;return 0; }
static NTSTATUS WdfRequestRetrieveOutputMemory(WDFREQUEST r,WDFMEMORY *m) { assert(!r->completions); *m=&r->output;return 0; }
static void *WdfMemoryGetBuffer(WDFMEMORY m,size_t *size) { *size=m->size;return m->data; }
static NTSTATUS WdfRequestMarkCancelableEx(WDFREQUEST r,void (*fn)(WDFREQUEST)) {
 assert(!r->marked&&!r->completions); if(r->already_cancelled)return STATUS_CANCELLED;
 r->marked=true;r->cancel=fn;return 0;
}
static NTSTATUS WdfRequestUnmarkCancelable(WDFREQUEST r) {
 assert(locked&&r->marked&&!r->completions);
 if(r->racing)return STATUS_CANCELLED; r->marked=false;return 0;
}
static void WdfRequestCompleteWithInformation(WDFREQUEST r,NTSTATUS status,ULONG_PTR info) {
 (void)info;assert(!locked&&!r->marked&&!r->completions);r->completions++;r->status=status;
}
static void WdfRequestComplete(WDFREQUEST r,NTSTATUS status) { WdfRequestCompleteWithInformation(r,status,0); }
static void WdfTimerStop(void *timer,BOOLEAN wait) { (void)timer; assert(wait&&!locked);timer_stops++; }
static ULONG SwBtRead(PSWBT_DEVICE d,ULONG reg) { (void)d;assert(reg==UART_LSR);return lsr; }
static void SwBtWrite(PSWBT_DEVICE d,ULONG reg,ULONG v) { (void)d;if(reg==UART_THR)wire[wire_length++]=(UCHAR)v; }
static void SwBtParse(PSWBT_DEVICE d,PSWBT_DONE done,UCHAR b) { (void)d;(void)done;(void)b;assert(false); }
VOID SwBtEvtTxCancel(WDFREQUEST Request);
'''
tests = r'''
static void init(PSWBT_DEVICE d,struct QUEUE *q) {
 memset(d,0,sizeof(*d));memset(q,0,3*sizeof(*q));for(int i=0;i<3;i++)q[i].dev=d;
 d->D0Active=true;d->Interrupt=d;d->TxQueue=q;d->EventQueue=q+1;d->AclQueue=q+2;
 lsr=0;wire_length=0;timer_stops=0;assert(!locked);
}
static void enqueue(WDFQUEUE q,WDFREQUEST r,UCHAR fill) {
 memset(r,0,sizeof(*r));r->queue=q;r->in.Type=HciPacketCommand;r->in.DataLen=32;
 memset(r->in.Data,fill,32);r->input=(MEMORY){&r->in,sizeof(r->in)};r->output=(MEMORY){&r->out,sizeof(r->out)};
 q->req[q->tail++]=r;
}
static void start(PSWBT_DEVICE d) { SWBT_DONE done={0};WdfSpinLockAcquire(0);SwBtStartTx(d,&done);WdfSpinLockRelease(0);SwBtDoneFlush(&done); }
static void cancel(WDFREQUEST r) { assert(r->marked);r->marked=false;r->cancel(r); }
static void drain_frame(PSWBT_DEVICE d) { while(d->TxBusy)SwBtTxBurst(d);SwBtEvtDpc(d,d); }
int main(void) {
 SWBT_DEVICE d;struct QUEUE q[3];struct REQUEST a,b,c;
 /* Cancellation before delivery to hardware does not send a frame. */
 init(&d,q);enqueue(q,&a,0x11);a.already_cancelled=true;start(&d);
 assert(a.completions==1&&a.status==STATUS_CANCELLED&&!d.TxFrameActive&&wire_length==0);
 /* An upper-stack cancellation completes while CTS/THRE is stalled. */
 init(&d,q);enqueue(q,&a,0x21);start(&d);assert(d.TxBusy&&d.TxFrameActive);
 cancel(&a);assert(a.completions==1&&a.status==STATUS_CANCELLED&&d.TxRequest==NULL);
 enqueue(q,&b,0x42);start(&d);assert(q->head==1&&d.TxFrameActive);
 drain_frame(&d);assert(d.TxRequest==&b&&wire_length==33);
 for(unsigned i=1;i<33;i++)assert(wire[i]==0x21);
 drain_frame(&d);assert(b.completions==1&&b.status==0&&wire_length==66);
 for(unsigned i=34;i<66;i++)assert(wire[i]==0x42);
 /* Cancellation wins Unmark, but its callback runs after the next TX starts. */
 init(&d,q);enqueue(q,&a,1);enqueue(q,&b,2);start(&d);a.racing=true;
 drain_frame(&d);assert(!a.completions&&d.TxRequest==&b);cancel(&a);
 assert(d.TxRequest==&b&&a.completions==1);drain_frame(&d);assert(b.completions==1);
 /* Final D0 exit drains both manual reads and the active write exactly once. */
 init(&d,q);enqueue(q,&a,3);enqueue(q+1,&b,4);enqueue(q+2,&c,5);start(&d);
 SwBtEvtD0Stopping(&d,0);assert(timer_stops==1&&!d.D0Active&&!d.TxFrameActive);
 assert(a.completions==1&&b.completions==1&&c.completions==1&&a.status==STATUS_DEVICE_NOT_READY);
 d.TxDone=true;SwBtEvtDpc(&d,&d);assert(a.completions==1);
 /* The D0 path loses to an already-scheduled cancellation callback. */
 init(&d,q);enqueue(q,&a,6);start(&d);a.racing=true;SwBtEvtD0Stopping(&d,0);
 assert(!a.completions&&!d.D0Active);cancel(&a);assert(a.completions==1);
 puts("PASS: canceled/stalled TX, H4 frame ownership, DPC/cancel race and D0 drain");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='switchbt-cancel-') as tmp:
    c=Path(tmp)/'test.c'; exe=Path(tmp)/'test'
    c.write_text(preamble+'\n'.join(functions)+tests)
    subprocess.run([os.environ.get('CC','clang'),'-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-g',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
