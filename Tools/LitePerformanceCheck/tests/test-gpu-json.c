// SPDX-License-Identifier: BSD-2-Clause-Patent
/* Native ARM64 Windows test: only device I/O is substituted. JSON is written
 * through real WriteFile/stdout. No GPU operation or physical pass is implied.
 */
#define UNICODE
#define _UNICODE
#define WINVER 0x0a00
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <TegraGpu/windows/control.h>
#include <TegraGpu/core/execute.h>

static unsigned fixture,opened;
static DWORD wantedCode,lastError;
static HANDLE WINAPI FixtureOpen(LPCWSTR name,DWORD access,DWORD share,
 LPSECURITY_ATTRIBUTES sa,DWORD disposition,DWORD flags,HANDLE templateFile) {
 (void)share;(void)sa;(void)disposition;(void)flags;(void)templateFile;
 if(lstrcmpW(name,L"\\\\.\\TegraGpuProbe") ||
    access!=(wantedCode==TG_GPU_IOCTL_STATUS?GENERIC_READ:GENERIC_WRITE))ExitProcess(101);
 ++opened;
 if(fixture==3){lastError=ERROR_ACCESS_DENIED;return INVALID_HANDLE_VALUE;}
 return (HANDLE)(ULONG_PTR)123;
}
static BOOL WINAPI FixtureControl(HANDLE handle,DWORD code,LPVOID in,DWORD inBytes,
 LPVOID out,DWORD outBytes,LPDWORD bytes,LPOVERLAPPED overlapped) {
 struct tg_gpu_status s={0};
 struct tg_gpu_request *request=in;
 (void)overlapped;
 if(handle!=(HANDLE)(ULONG_PTR)123 || code!=wantedCode || outBytes!=sizeof(s))ExitProcess(102);
 if(code==TG_GPU_IOCTL_RUN || code==TG_GPU_IOCTL_GR_RUN || code==TG_GPU_IOCTL_IRQ_RUN){
  if(inBytes!=sizeof(*request) || request->version!=TG_GPU_CONTROL_VERSION ||
     request->target_khz!=TG_GPU_CONTROL_DEFAULT_KHZ)ExitProcess(103);
 }else if(in || inBytes)ExitProcess(104);
 s.version=TG_GPU_CONTROL_VERSION+(fixture==4);
 s.tested=fixture!=0;s.target_khz=TG_GPU_CONTROL_DEFAULT_KHZ;
 s.measured_khz=729600;s.rail_uv=800000;s.boot0=0x12e000b1;
 s.stage=fixture==2?306:304;s.execution_result=fixture==2?0:6;
 s.expected[0]=0x46a7712c;s.expected[1]=~s.expected[0];
 s.observed[0]=s.observed[1]=0xdeadbeef;
 if(fixture==2){s.observed[0]=s.expected[0];s.observed[1]=s.expected[1];}
 s.pbdma_enable_before=0;s.pbdma_enable_after=1;s.pbdma_runlist_map=1;
 s.channel_instance_after=0x800fde33;s.runlist_status=2;s.pbdma_status=0x10000004;
 s.pbdma_gp_fetch=0xffffffff;s.frontend.stage=11;s.frontend.ltc_count=2;
 s.frontend.fb_flushes=2;s.frontend.l2_flushes=2;s.frontend.l2_invalidations=2;
 s.frontend.gr_enable=0xd8013120;s.frontend.gr_fifo_access=0x10001;
 s.frontend.gr_gate_ctrl[0]=0xaaaa0010;s.frontend.gr_gate_ctrl[1]=0xbbbb0010;
 s.gr.stage=15;s.gr.fecs_mailbox=0x9000;s.gr.last_register=0x4188ac;s.gr.last_value=2;
 s.gr.context_bytes=0x17000;s.gr.zcull_bytes=0x4000;s.gr.pm_bytes=0x9000;
 s.gr.fecs_host_interrupt=0x10000;s.gr.fecs_ctxsw_status=0x1234;
 s.gr.fecs_error_mailbox=0x18;s.gr.new_context=0x800c0040;
 if(fixture>=6){
  s.gr.priv_security=1;s.acr.stage=9;s.acr.security=1;s.acr.wpr_base=0xffc00000;s.acr.wpr_size=0x200000;
  s.stage=310;s.execution_result=14; /* Authentication alone cannot pass FIFO. */
  if(fixture>=7){
   s.stage=306;s.execution_result=0;s.observed[0]=s.expected[0];s.observed[1]=s.expected[1];
   s.pmu.stage=6;
   if(fixture==7)s.acr_firmware_result=2; /* A format failure cannot be hidden by later fields. */
   if(fixture==8){s.pmu.stage=5;s.pmu.result=10;s.pmu.reply_code=1;}
   if(fixture==10)s.gr.stage=8; /* Ready alone is not context-state initialization. */
   if(fixture==11)s.gr.context_bytes=0;
   if(fixture==12)s.gr.stage=13; /* FECS queries alone omit GPC translation setup. */
  }
 }
 if(fixture>=13 && fixture<=19){
  s.graphics.stage=9;s.graphics.context_bytes=s.gr.context_bytes;
  s.graphics.bind_mailbox=0x10;s.graphics.save_mailbox=1;
  s.graphics.image_magic=0x600dc0de;s.graphics.image_changed_words=123;
  if(fixture==14)s.graphics.stage=7;
  if(fixture==15)s.graphics.result=TG_GPU_GR_CTX_FECS_REJECTED;
  if(fixture==16)s.graphics.image_changed_words=0;
  if(fixture==17)s.graphics.image_magic=0xa5a5a5a5;
  if(fixture==18)s.graphics.save_mailbox=3;
  if(fixture==19)s.graphics.bind_mailbox=0x30;
 }
 if(fixture>=20){
  s.irq_gsi=T214_GPU_NONSTALL_GSI;s.irq_expected=2;
  s.interrupt.interrupts=2;s.irq_dpcs=2;
  s.interrupt.stage=TG_GPU_IRQ_RESTORE;
  s.interrupt.last_register=T214_GPU_REG_BASE+0x144;s.interrupt.last_value=0;
  if(fixture==21)s.interrupt.interrupts=0;
  if(fixture==22)s.interrupt.interrupts=1;
  if(fixture==23)s.irq_dpcs=1;
  if(fixture==24)s.interrupt.result=TG_GPU_IRQ_UNEXPECTED;
  if(fixture==25)s.interrupt.restore_result=TG_GPU_IRQ_REGISTER;
  if(fixture==26)s.irq_expected=0;
  if(fixture==27)s.irq_gsi=0;
  if(fixture==28)s.resources_owned=1;
  if(fixture==29){
   s.execution_result=TG_GPU_EXEC_IRQ;s.interrupt.result=TG_GPU_IRQ_REGISTER;
   s.interrupt.stage=TG_GPU_IRQ_SNAPSHOT;
   s.interrupt.last_register=T214_GPU_REG_BASE+0x644;s.interrupt.last_value=0xffffffff;
  }
 }
 *(struct tg_gpu_status *)out=s;*bytes=sizeof(s)-(fixture==5?4:0);
 return TRUE;
}
static BOOL WINAPI FixtureClose(HANDLE h){(void)h;return TRUE;}
static DWORD WINAPI FixtureError(void){return lastError;}

#define CreateFileW FixtureOpen
#define DeviceIoControl FixtureControl
#define CloseHandle FixtureClose
#define GetLastError FixtureError
#define start UiEntry
#include "../performance-check.c"
#undef start

void start(void) {
 LPWSTR query[]={L"fixture.exe",L"--gpu-json"};
 LPWSTR run[]={L"fixture.exe",L"--gpu-json",L"--gpu-729"};
 LPWSTR runGr[]={L"fixture.exe",L"--gpu-json",L"--gpu-gr-729"};
 LPWSTR runIrq[]={L"fixture.exe",L"--gpu-json",L"--gpu-irq-729"};
 LPWSTR conflictIrq[]={L"fixture.exe",L"--gpu-json",L"--gpu-irq-729",L"--gpu-gr-729"};
 LPWSTR conflictGr[]={L"fixture.exe",L"--gpu-json",L"--gpu-gr-729",L"--gpu-729"};
 LPWSTR conflict[]={L"fixture.exe",L"--gpu-json",L"--gpu-729",L"--gpu-1228"};
 const DWORD results[]={0,0,ERROR_GEN_FAILURE,0,ERROR_ACCESS_DENIED,
                         ERROR_REVISION_MISMATCH,ERROR_REVISION_MISMATCH,ERROR_INVALID_PARAMETER,
                         ERROR_GEN_FAILURE,ERROR_GEN_FAILURE,ERROR_GEN_FAILURE,0,
                         ERROR_GEN_FAILURE,ERROR_GEN_FAILURE,ERROR_GEN_FAILURE};
 const unsigned fixtures[]={0,1,1,2,3,4,5,2,6,7,8,9,10,11,12};
 for(unsigned i=0;i<15;++i){
  DWORD result;fixture=fixtures[i];opened=0;
  arguments=i<2?query:i==7?conflict:run;argumentCount=i<2?2:i==7?4:3;
  wantedCode=i<2?TG_GPU_IOCTL_STATUS:TG_GPU_IOCTL_RUN;
  result=gpu_json();
  if(result!=results[i] || opened!=(i==7?0u:1u))ExitProcess(110+i);
 }
 for(unsigned i=0;i<11;++i){
  fixture=i==10?20:20+i;opened=0;
  arguments=i==10?conflictIrq:runIrq;argumentCount=i==10?4:3;
  wantedCode=TG_GPU_IOCTL_IRQ_RUN;
  DWORD result=gpu_json();
  if(result!=(i==10?ERROR_INVALID_PARAMETER:i?ERROR_GEN_FAILURE:0) ||
     opened!=(i==10?0u:1u))ExitProcess(160+i);
 }
 for(unsigned i=0;i<8;++i){
  fixture=i==7?13:13+i;opened=0;
  arguments=i==7?conflictGr:runGr;argumentCount=i==7?4:3;
  wantedCode=TG_GPU_IOCTL_GR_RUN;
  DWORD result=gpu_json();
  if(result!=(i==7?ERROR_INVALID_PARAMETER:i?ERROR_GEN_FAILURE:0) ||
     opened!=(i==7?0u:1u))ExitProcess(140+i);
 }
 if(!json_text(GetStdHandle(STD_OUTPUT_HANDLE),"{\"fixture_complete\":true}\r\n"))ExitProcess(120);
 ExitProcess(0);
}
