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
static DWORD value(HKEY k,LPCWSTR name) {
 DWORD t,v=0,n=sizeof(v);
 return RegQueryValueExW(k,name,NULL,&t,(BYTE*)&v,&n)==ERROR_SUCCESS && t==REG_DWORD && n==sizeof(v)?v:0;
}
static void read_status(void) {
 HKEY k;present=FALSE;
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
 if(msg==WM_CREATE){SetTimer(w,1,1000,NULL);return 0;}
 if(msg==WM_TIMER){read_status();InvalidateRect(w,NULL,TRUE);return 0;}
 if(msg==WM_PAINT){
  PAINTSTRUCT p;RECT r;WCHAR text[192];HDC dc=BeginPaint(w,&p);GetClientRect(w,&r);
  FillRect(dc,&r,(HBRUSH)GetStockObject(WHITE_BRUSH));SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);
  BOOL ready=present && live && stale<6 && failures==0 && hand==1 && error==0 && cpu==1996800 && ram==2100000;
  SetTextColor(dc,ready?RGB(0,125,50):RGB(160,85,0));
  line(dc,26,ready?L"CPU / RAM 已达到目标频率":throttled?L"温控降频中":L"等待运行状态");
  SetTextColor(dc,RGB(20,30,45));
  wsprintfW(text,L"CPU     %lu.%lu MHz",cpu/1000,(cpu%1000)/100);line(dc,86,text);
  wsprintfW(text,L"内存    %lu MHz",ram/1000);line(dc,130,text);
  wsprintfW(text,L"温度    芯片 %lu°C / 主板 %lu°C",soc,board);line(dc,174,text);
  wsprintfW(text,L"风扇 PWM    %lu%%",duty*100/236);line(dc,218,text);
  line(dc,262,L"GPU 超频：未启用");
  wsprintfW(text,L"温控采样 %lu   CPU 目标 %lu MHz",runs,requested/1000);line(dc,322,text);
  wsprintfW(text,L"启动状态 %lu   驱动错误 0x%08lX",hand,error);line(dc,366,text);
  if(!present || !live || stale>=6)line(dc,420,L"温控驱动尚未提供新的读数");
  line(dc,478,L"数值来自运行时读回；不代表稳定性测试。 ");
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
 HWND w=CreateWindowExW(0,c.lpszClassName,L"Switch Lite - Performance",WS_OVERLAPPEDWINDOW|WS_VISIBLE,0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN),NULL,NULL,h,NULL);
 UpdateWindow(w);while(GetMessageW(&m,NULL,0,0)>0){TranslateMessage(&m);DispatchMessageW(&m);}
 DeleteObject(font);ExitProcess(0);
}
