// SPDX-License-Identifier: BSD-2-Clause-Patent
#define UNICODE
#define _UNICODE
#define WINVER 0x0a00
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <shellapi.h>
#include <TegraGpu/windows/control.h>
static UINT_PTR image_cookie = 0x00002b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used = {
 .Size = sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64), .SecurityCookie = (ULONG_PTR)&image_cookie
};
static HFONT font,controlFont;
static DWORD cpu,ram,requested,hand,error,soc,board,duty,runs,failures,throttled;
static DWORD lastRuns,stale;
static BOOL present,live;
typedef struct { DWORD version, brightness, duty, displayState; } BACKLIGHT_STATUS;
static BACKLIGHT_STATUS backlight;
static BOOL backlightAvailable;
static DWORD backlightError;
static DWORD backlightSetError;
static HWND darker,lighter,gpuChoice,gpuRun;
static struct tg_gpu_status gpu, gpuWorkerResult;
static BOOL gpuAvailable;
static DWORD gpuError,gpuWorkerError,gpuTestError;
static volatile LONG gpuBusy;
static DWORD gpuRequested=TG_GPU_CONTROL_DEFAULT_KHZ;
#define WM_GPU_DONE (WM_APP+1)
#define WM_GPU_RUN (WM_APP+2)
static BOOL gpu_io(DWORD code, DWORD target, struct tg_gpu_status *out, DWORD *error) {
 DWORD bytes=0;
 struct tg_gpu_request in={TG_GPU_CONTROL_VERSION,target};
 HANDLE h=CreateFileW(L"\\\\.\\TegraGpuProbe",code==TG_GPU_IOCTL_RUN?GENERIC_WRITE:GENERIC_READ,
                     FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
 if(h==INVALID_HANDLE_VALUE){*error=GetLastError();return FALSE;}
 BOOL ok=DeviceIoControl(h,code,code==TG_GPU_IOCTL_RUN?&in:NULL,
                        code==TG_GPU_IOCTL_RUN?sizeof(in):0,out,sizeof(*out),&bytes,NULL);
 *error=ok?0:GetLastError();CloseHandle(h);
 if(ok && (bytes!=sizeof(*out) || out->version!=TG_GPU_CONTROL_VERSION)){*error=ERROR_REVISION_MISMATCH;return FALSE;}
 return ok;
}
static DWORD WINAPI gpu_worker(void *context) {
 HWND w=(HWND)context;
 gpu_io(TG_GPU_IOCTL_RUN,gpuRequested,&gpuWorkerResult,&gpuWorkerError);
 PostMessageW(w,WM_GPU_DONE,0,0);return 0;
}
static void start_gpu(HWND w) {
 if(InterlockedCompareExchange(&gpuBusy,1,0))return;
 gpuTestError=0;
 gpuRequested=SendMessageW(gpuChoice,CB_GETCURSEL,0,0)==1?TG_GPU_CONTROL_OC_KHZ:TG_GPU_CONTROL_DEFAULT_KHZ;
 EnableWindow(gpuRun,FALSE);EnableWindow(gpuChoice,FALSE);
 HANDLE t=CreateThread(NULL,0,gpu_worker,w,0,NULL);
 if(t)CloseHandle(t);else{gpuWorkerError=GetLastError();PostMessageW(w,WM_GPU_DONE,0,0);}
 InvalidateRect(w,NULL,TRUE);
}
static BOOL has_switch(LPCWSTR wanted) {
 LPCWSTR cmd=GetCommandLineW();
 for(;*cmd;++cmd){LPCWSTR a=cmd,b=wanted;while(*b && *a==*b){++a;++b;}if(!*b && (!*a || *a==L' '))return TRUE;}
 return FALSE;
}
static BOOL backlight_io(DWORD code,void *in,DWORD inSize,void *out,DWORD outSize) {
 DWORD bytes=0;
 HANDLE h=CreateFileW(L"\\\\.\\SwitchBacklight",code==0x00226040?GENERIC_READ:GENERIC_WRITE,
                     FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
 if(h==INVALID_HANDLE_VALUE){backlightError=GetLastError();return FALSE;}
 BOOL ok=DeviceIoControl(h,code,in,inSize,out,outSize,&bytes,NULL);
 backlightError=ok?0:GetLastError();CloseHandle(h);
 return ok && (outSize==0 || bytes==outSize);
}
static DWORD value(HKEY k,LPCWSTR name) {
 DWORD t,v=0,n=sizeof(v);
 return RegQueryValueExW(k,name,NULL,&t,(BYTE*)&v,&n)==ERROR_SUCCESS && t==REG_DWORD && n==sizeof(v)?v:0;
}
static void read_status(void) {
 HKEY k;present=FALSE;
 gpuAvailable=gpu_io(TG_GPU_IOCTL_STATUS,0,&gpu,&gpuError);
 EnableWindow(gpuRun,gpuAvailable);EnableWindow(gpuChoice,gpuAvailable);
 backlightAvailable=backlight_io(0x00226040,NULL,0,&backlight,sizeof(backlight)) && backlight.version==2;
 EnableWindow(darker,backlightAvailable);EnableWindow(lighter,backlightAvailable);
 if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"SYSTEM\\CurrentControlSet\\Services\\switchfan\\Parameters",0,KEY_QUERY_VALUE,&k)!=ERROR_SUCCESS)return;
 present=TRUE;
 cpu=value(k,L"CpuActualKhz");ram=value(k,L"RamActualKhz");requested=value(k,L"CpuRequestedKhz");
 hand=value(k,L"HandoffStatus");error=value(k,L"CpuLastError");soc=value(k,L"TempSoC");
 board=value(k,L"TempBoard");duty=value(k,L"Duty");runs=value(k,L"Runs");
 failures=value(k,L"SensorFailures");throttled=value(k,L"CpuThrottled");RegCloseKey(k);
 if(runs!=lastRuns){live=TRUE;stale=0;lastRuns=runs;}else ++stale;
}
static void line(HDC dc,int y,LPCWSTR text) { TextOutW(dc,24,y,text,lstrlenW(text)); }
static void gpu_line(HDC dc,int y,LPCWSTR text) { TextOutW(dc,660,y,text,lstrlenW(text)); }
static void layout_controls(HWND w) {
 RECT r;GetClientRect(w,&r);
 int width=r.right>0?r.right:1,height=r.bottom>0?r.bottom:1;
 HWND controls[]={darker,lighter,gpuChoice,gpuRun};
 const int rects[][4]={{24,580,180,56},{222,580,180,56},{660,110,420,160},{660,178,420,56}};
 int sizeX=MulDiv(24,width,1200),sizeY=MulDiv(24,height,680),size=sizeX<sizeY?sizeX:sizeY;
 if(size<12)size=12;
 HFONT next=CreateFontW(size,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
 for(unsigned i=0;i<4;++i)if(controls[i]){
  MoveWindow(controls[i],MulDiv(rects[i][0],width,1200),MulDiv(rects[i][1],height,680),MulDiv(rects[i][2],width,1200),MulDiv(rects[i][3],height,680),TRUE);
  if(next)SendMessageW(controls[i],WM_SETFONT,(WPARAM)next,TRUE);
 }
 if(next){if(controlFont)DeleteObject(controlFont);controlFont=next;}
}
static LRESULT CALLBACK proc(HWND w,UINT msg,WPARAM a,LPARAM b) {
 if(msg==WM_CREATE){
  darker=CreateWindowExW(0,L"BUTTON",L"Darker",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,24,580,180,56,w,(HMENU)10,GetModuleHandleW(NULL),NULL);
  lighter=CreateWindowExW(0,L"BUTTON",L"Brighter",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,222,580,180,56,w,(HMENU)11,GetModuleHandleW(NULL),NULL);
  SendMessageW(darker,WM_SETFONT,(WPARAM)font,TRUE);SendMessageW(lighter,WM_SETFONT,(WPARAM)font,TRUE);
  gpuChoice=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,660,110,330,160,w,(HMENU)20,GetModuleHandleW(NULL),NULL);
  SendMessageW(gpuChoice,CB_ADDSTRING,0,(LPARAM)L"729.6 MHz (default)");
  SendMessageW(gpuChoice,CB_ADDSTRING,0,(LPARAM)L"1228.8 MHz (overclock)");
  SendMessageW(gpuChoice,CB_SETCURSEL,has_switch(L"--gpu-1228")?1:0,0);
  gpuRun=CreateWindowExW(0,L"BUTTON",L"Apply and test GPU",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,660,178,330,56,w,(HMENU)21,GetModuleHandleW(NULL),NULL);
  SendMessageW(gpuChoice,WM_SETFONT,(WPARAM)font,TRUE);SendMessageW(gpuRun,WM_SETFONT,(WPARAM)font,TRUE);
  layout_controls(w);read_status();
  if(!has_switch(L"--gpu-729") && !has_switch(L"--gpu-1228"))SendMessageW(gpuChoice,CB_SETCURSEL,gpu.target_khz==TG_GPU_CONTROL_OC_KHZ?1:0,0);
  SetTimer(w,1,1000,NULL);
  if(has_switch(L"--gpu-729") || has_switch(L"--gpu-1228"))PostMessageW(w,WM_GPU_RUN,0,0);
  return 0;
 }
 if(msg==WM_COMMAND && (LOWORD(a)==10 || LOWORD(a)==11) && backlightAvailable){
  DWORD level=backlight.brightness;
  if(LOWORD(a)==10)level=level>34?level-26:8;else level=level<229?level+26:255;
  if(backlight_io(0x0022A044,&level,sizeof(level),NULL,0)){backlightSetError=0;if(!gpuBusy)read_status();}
  else backlightSetError=backlightError;
  InvalidateRect(w,NULL,TRUE);return 0;
 }
 if((msg==WM_COMMAND && LOWORD(a)==21) || msg==WM_GPU_RUN){start_gpu(w);return 0;}
 if(msg==WM_GPU_DONE){
  InterlockedExchange(&gpuBusy,0);gpuTestError=gpuWorkerError;
  if(!gpuTestError){gpu=gpuWorkerResult;gpuAvailable=TRUE;}
  if(gpuTestError==ERROR_ACCESS_DENIED){
   WCHAR exe[MAX_PATH];
   if(GetModuleFileNameW(NULL,exe,MAX_PATH)){
    HINSTANCE opened=ShellExecuteW(w,L"runas",exe,gpuRequested==TG_GPU_CONTROL_OC_KHZ?L"--gpu-1228":L"--gpu-729",NULL,SW_SHOWNORMAL);
    if((INT_PTR)opened>32){DestroyWindow(w);return 0;}
   }
  }
  EnableWindow(gpuRun,gpuAvailable);EnableWindow(gpuChoice,gpuAvailable);
  InvalidateRect(w,NULL,TRUE);return 0;
 }
 if(msg==WM_TIMER){if(!gpuBusy)read_status();InvalidateRect(w,NULL,TRUE);return 0;}
 if(msg==WM_CLOSE && gpuBusy)return 0;
 if(msg==WM_SIZE){layout_controls(w);InvalidateRect(w,NULL,TRUE);return 0;}
 if(msg==WM_PAINT){
  PAINTSTRUCT p;RECT r;WCHAR text[192];HDC dc=BeginPaint(w,&p);GetClientRect(w,&r);
  FillRect(dc,&r,(HBRUSH)GetStockObject(WHITE_BRUSH));
  SetMapMode(dc,MM_ANISOTROPIC);SetWindowExtEx(dc,1200,680,NULL);SetViewportExtEx(dc,r.right,r.bottom,NULL);
  SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);
  BOOL ready=present && live && stale<6 && failures==0 && hand==1 && error==0 && cpu==1996800 && ram==2100000;
  SetTextColor(dc,ready?RGB(0,125,50):RGB(160,85,0));
  line(dc,26,ready?L"CPU / RAM targets reached":throttled?L"Thermal throttling":L"Current performance");
  SetTextColor(dc,RGB(20,30,45));
  wsprintfW(text,L"CPU     %lu.%lu MHz",cpu/1000,(cpu%1000)/100);line(dc,86,text);
  wsprintfW(text,L"RAM     %lu MHz",ram/1000);line(dc,130,text);
  wsprintfW(text,L"Temperature   SoC %lu C / board %lu C",soc,board);line(dc,174,text);
  wsprintfW(text,L"Fan PWM     %lu%%",duty*100/236);line(dc,218,text);
  line(dc,262,L"Renderer: CPU software");
  wsprintfW(text,L"Samples %lu   CPU target %lu MHz",runs,requested/1000);line(dc,322,text);
  wsprintfW(text,L"Boot status %lu   CPU error 0x%08lX",hand,error);line(dc,366,text);
  if(!present || !live || stale>=6)line(dc,420,L"Waiting for fresh fan readings");
  else if(gpu.tested && !gpuBusy && !gpuTestError && !gpuError){
   wsprintfW(text,L"PB: %08lX -> %08lX",gpu.pbdma_enable_before,gpu.pbdma_enable_after);line(dc,420,text);
   if(gpu.stage>=304)wsprintfW(text,L"GET %lu PUT %lu  PB %08lX",gpu.userd_gp_get,gpu.userd_gp_put,gpu.pbdma_status);
   else wsprintfW(text,L"PB runlist map %08lX",gpu.pbdma_runlist_map);
   line(dc,448,text);
  }
  line(dc,478,L"Runtime readings; not a stability test.");
  if(backlightAvailable)wsprintfW(text,L"Brightness   %lu%%",(backlight.brightness*100+127)/255);
  else wsprintfW(text,L"Brightness unavailable (0x%08lX)",backlightError);
  line(dc,530,text);
  if(backlightSetError){wsprintfW(text,L"Brightness change failed: 0x%08lX",backlightSetError);line(dc,644,text);}
  gpu_line(dc,26,L"GPU - experimental");
  gpu_line(dc,68,L"Choose a GPU clock profile");
  if(gpuBusy)wsprintfW(text,L"Running clock and FIFO readback...");
  else if(gpuTestError || gpuError)wsprintfW(text,L"GPU unavailable/error: 0x%08lX",gpuTestError?gpuTestError:gpuError);
  else if(!gpu.tested)wsprintfW(text,L"GPU has not been tested yet");
  else wsprintfW(text,L"Last measurement: %lu.%lu MHz",gpu.measured_khz/1000,(gpu.measured_khz%1000)/100);
  TextOutW(dc,660,264,text,lstrlenW(text));
  if(gpu.tested && !gpuBusy && !gpuTestError && !gpuError){
   wsprintfW(text,L"GPU rail: %lu mV  BOOT0: %08lX",gpu.rail_uv/1000,gpu.boot0);TextOutW(dc,660,308,text,lstrlenW(text));
   BOOL pass=!gpu.power_result && !gpu.clock_result && !gpu.execution_result && !gpu.restore_result && !gpu.resources_owned && gpu.expected[0]==gpu.observed[0] && gpu.expected[1]==gpu.observed[1];
   SetTextColor(dc,pass?RGB(0,125,50):RGB(160,85,0));
   LPCWSTR result=pass?L"GPU FIFO READBACK PASS":L"GPU check did not pass";
   TextOutW(dc,660,352,result,lstrlenW(result));SetTextColor(dc,RGB(20,30,45));
   wsprintfW(text,L"Stage %lu  P:%lu C:%lu E:%lu",gpu.stage,gpu.power_result,gpu.clock_result,gpu.execution_result);TextOutW(dc,660,396,text,lstrlenW(text));
   if(gpu.stage>=301 && gpu.stage<304){
    wsprintfW(text,L"MC: %08lX -> %08lX",gpu.mc_enable_before,gpu.mc_enable_after);TextOutW(dc,660,440,text,lstrlenW(text));
    if(gpu.mc_enable_before&0x100)wsprintfW(text,L"INST: %08lX -> %08lX",gpu.channel_instance_before,gpu.channel_instance_after);
    else wsprintfW(text,L"INST: reset -> %08lX",gpu.channel_instance_after);
   }else{
    wsprintfW(text,L"Read 1: %08lX / %08lX",gpu.observed[0],gpu.expected[0]);TextOutW(dc,660,440,text,lstrlenW(text));
    wsprintfW(text,L"Read 2: %08lX / %08lX",gpu.observed[1],gpu.expected[1]);
   }
   TextOutW(dc,660,484,text,lstrlenW(text));
   wsprintfW(text,L"Restore %08lX  owned %lu",gpu.restore_result,gpu.resources_owned);TextOutW(dc,660,528,text,lstrlenW(text));
  }
  if(gpu.tested && gpu.execution_result && !gpuBusy && !gpuTestError && !gpuError){
   wsprintfW(text,L"FIFO %08lX  MMU %08lX",gpu.fifo_interrupt,gpu.mmu_fault);gpu_line(dc,580,text);
   wsprintfW(text,L"PBDMA %08lX  CH %08lX",gpu.pbdma_interrupt,gpu.channel_status);gpu_line(dc,624,text);
  }else{
   gpu_line(dc,580,L"GPU powers down after this test.");
   gpu_line(dc,624,L"FIFO pass is not Vulkan rendering.");
  }
  EndPaint(w,&p);return 0;
 }
 if(msg==WM_DESTROY){KillTimer(w,1);PostQuitMessage(0);return 0;}
 return DefWindowProcW(w,msg,a,b);
}
void start(void) {
 HINSTANCE h=GetModuleHandleW(NULL);WNDCLASSW c={0};MSG m;
 c.lpfnWndProc=proc;c.hInstance=h;c.hCursor=LoadCursorW(NULL,IDC_ARROW);c.lpszClassName=L"LitePerformanceCheck";
 RegisterClassW(&c);
 font=CreateFontW(24,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
 HWND w=CreateWindowExW(0,c.lpszClassName,L"Switch Lite - Performance and brightness",WS_OVERLAPPEDWINDOW|WS_VISIBLE,0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN),NULL,NULL,h,NULL);
 UpdateWindow(w);while(GetMessageW(&m,NULL,0,0)>0){TranslateMessage(&m);DispatchMessageW(&m);}
 if(controlFont)DeleteObject(controlFont);DeleteObject(font);ExitProcess(0);
}
