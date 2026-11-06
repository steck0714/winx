// SPDX-License-Identifier: BSD-2-Clause-Patent
#define UNICODE
#define _UNICODE
#define WINVER 0x0a00
#define _WIN32_WINNT 0x0a00
#include <windows.h>
static UINT_PTR image_cookie = 0x00002b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used = {
 .Size = sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64), .SecurityCookie = (ULONG_PTR)&image_cookie
};
static HFONT font;
static DWORD cpu,ram,requested,hand,error,soc,board,duty,runs,failures,throttled;
static DWORD lastRuns,stale;
static BOOL present,live;
typedef struct { DWORD version, brightness, duty, displayState; } BACKLIGHT_STATUS;
static BACKLIGHT_STATUS backlight;
static BOOL backlightAvailable;
static DWORD backlightError;
static DWORD backlightSetError;
static HWND darker,lighter;
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
static LRESULT CALLBACK proc(HWND w,UINT msg,WPARAM a,LPARAM b) {
 if(msg==WM_CREATE){
  darker=CreateWindowExW(0,L"BUTTON",L"Darker",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,24,580,180,56,w,(HMENU)10,GetModuleHandleW(NULL),NULL);
  lighter=CreateWindowExW(0,L"BUTTON",L"Brighter",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,222,580,180,56,w,(HMENU)11,GetModuleHandleW(NULL),NULL);
  SendMessageW(darker,WM_SETFONT,(WPARAM)font,TRUE);SendMessageW(lighter,WM_SETFONT,(WPARAM)font,TRUE);
  read_status();SetTimer(w,1,1000,NULL);return 0;
 }
 if(msg==WM_COMMAND && (LOWORD(a)==10 || LOWORD(a)==11) && backlightAvailable){
  DWORD level=backlight.brightness;
  if(LOWORD(a)==10)level=level>34?level-26:8;else level=level<229?level+26:255;
  if(backlight_io(0x0022A044,&level,sizeof(level),NULL,0)){backlightSetError=0;read_status();}
  else backlightSetError=backlightError;
  InvalidateRect(w,NULL,TRUE);return 0;
 }
 if(msg==WM_TIMER){read_status();InvalidateRect(w,NULL,TRUE);return 0;}
 if(msg==WM_PAINT){
  PAINTSTRUCT p;RECT r;WCHAR text[192];HDC dc=BeginPaint(w,&p);GetClientRect(w,&r);
  FillRect(dc,&r,(HBRUSH)GetStockObject(WHITE_BRUSH));SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);
  BOOL ready=present && live && stale<6 && failures==0 && hand==1 && error==0 && cpu==1996800 && ram==2100000;
  SetTextColor(dc,ready?RGB(0,125,50):RGB(160,85,0));
  line(dc,26,ready?L"CPU / RAM targets reached":throttled?L"Thermal throttling":L"Current performance");
  SetTextColor(dc,RGB(20,30,45));
  wsprintfW(text,L"CPU     %lu.%lu MHz",cpu/1000,(cpu%1000)/100);line(dc,86,text);
  wsprintfW(text,L"RAM     %lu MHz",ram/1000);line(dc,130,text);
  wsprintfW(text,L"Temperature   SoC %lu C / board %lu C",soc,board);line(dc,174,text);
  wsprintfW(text,L"Fan PWM     %lu%%",duty*100/236);line(dc,218,text);
  line(dc,262,L"GPU overclock: disabled");
  wsprintfW(text,L"Samples %lu   CPU target %lu MHz",runs,requested/1000);line(dc,322,text);
  wsprintfW(text,L"Boot status %lu   CPU error 0x%08lX",hand,error);line(dc,366,text);
  if(!present || !live || stale>=6)line(dc,420,L"Waiting for fresh fan readings");
  line(dc,478,L"Runtime readings; not a stability test.");
  if(backlightAvailable)wsprintfW(text,L"Brightness   %lu%%",(backlight.brightness*100+127)/255);
  else wsprintfW(text,L"Brightness unavailable (0x%08lX)",backlightError);
  line(dc,530,text);
  if(backlightSetError){wsprintfW(text,L"Brightness change failed: 0x%08lX",backlightSetError);line(dc,644,text);}
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
 DeleteObject(font);ExitProcess(0);
}
