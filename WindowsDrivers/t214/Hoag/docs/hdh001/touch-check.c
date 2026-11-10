// SPDX-License-Identifier: BSD-2-Clause-Patent
#define UNICODE
#define _UNICODE
#define WINVER 0x0a00
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>

// ARM64 WinPE 22621 validates the load configuration before entering the GUI.
// The Windows loader can seed this writable cookie during process startup.
static UINT_PTR image_cookie = 0x00002b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used = {
    .Size = sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64), .SecurityCookie = (ULONG_PTR)&image_cookie
};

typedef struct { LPCWSTR prefix, label; BOOL present, started; ULONG problem; } NODE;
static NODE nodes[] = {
    { L"ACPI\\SWOA0101\\", L"GPIO" }, { L"ACPI\\SWOA0201\\", L"I2C3" },
    { L"ACPI\\SWOA0502\\", L"FTM4" }
};
static DWORD stage, chip, status, irqs, reports, events, errors;
static int hits, pointerX = -1, pointerY = -1;
static UINT32 activeId;
static BOOL armed;
static HFONT font;

static DWORD trace(HKEY key, LPCWSTR name)
{
    DWORD type, value = 0, size = sizeof(value);
    return RegQueryValueExW(key, name, NULL, &type, (BYTE *)&value, &size) == ERROR_SUCCESS &&
           type == REG_DWORD && size == sizeof(value) ? value : 0;
}

static void read_status(void)
{
    DWORD i, j;
    HKEY key;
    HDEVINFO devices = SetupDiGetClassDevsW(NULL, L"ACPI", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    for (j = 0; j < ARRAYSIZE(nodes); ++j) {
        nodes[j].present = nodes[j].started = FALSE;
        nodes[j].problem = 0;
    }
    if (devices != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA dev;
        WCHAR id[220];
        dev.cbSize = sizeof(dev);
        for (i = 0; SetupDiEnumDeviceInfo(devices, i, &dev); ++i) {
            if (!SetupDiGetDeviceInstanceIdW(devices, &dev, id, ARRAYSIZE(id), NULL)) continue;
            for (j = 0; j < ARRAYSIZE(nodes); ++j) {
                int n = lstrlenW(nodes[j].prefix);
                if (lstrlenW(id) < n || CompareStringOrdinal(id, n, nodes[j].prefix, n, TRUE) != CSTR_EQUAL) continue;
                ULONG flags = 0, problem = 0;
                nodes[j].present = TRUE;
                if (CM_Get_DevNode_Status(&flags, &problem, dev.DevInst, 0) == CR_SUCCESS) {
                    nodes[j].started = (flags & DN_STARTED) != 0 && (flags & DN_HAS_PROBLEM) == 0;
                    nodes[j].problem = problem;
                }
            }
        }
        SetupDiDestroyDeviceInfoList(devices);
    }
    stage = chip = reports = status = irqs = events = errors = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\switchtouch\\Parameters",
                      0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        stage = trace(key,L"ProbeStage"); chip = trace(key,L"ChipId"); status = trace(key,L"ProbeStatus");
        irqs = trace(key,L"Irqs"); reports = trace(key,L"Reports");
        events = trace(key,L"Events"); errors = trace(key,L"Errors");
        RegCloseKey(key);
    }
}

static BOOL ready(void)
{
    return nodes[0].started && nodes[1].started && nodes[2].started && stage == 4 && chip == 0x3670 && status == 0;
}

static POINT target(const RECT *r, int index)
{
    const POINT pct[] = {{20,48},{80,72},{30,86}};
    POINT p = pct[index < 3 ? index : 2];
    p.x = r->right * p.x / 100; p.y = r->bottom * p.y / 100;
    return p;
}

static int radius(const RECT *r)
{
    int side = r->right < r->bottom ? r->right : r->bottom;
    return side / 12;
}

static BOOL inside(POINT pos, const RECT *r)
{
    POINT p = target(r, hits);
    LONG dx = pos.x - p.x, dy = pos.y - p.y, rad = radius(r);
    return dx*dx + dy*dy <= rad*rad;
}

static void line(HDC dc, int x, int y, LPCWSTR text)
{
    TextOutW(dc, x, y, text, lstrlenW(text));
}

static LRESULT CALLBACK window_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    RECT r;
    GetClientRect(wnd,&r);
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == 1 && HIWORD(wp) == BN_CLICKED)
            return DefWindowProcW(wnd,WM_CLOSE,0,0);
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) return DefWindowProcW(wnd,WM_CLOSE,0,0);
        break;
    case WM_TIMER:
        read_status(); InvalidateRect(wnd,NULL,FALSE); return 0;
    case WM_POINTERDOWN:
    case WM_POINTERUP: {
        UINT32 id = GET_POINTERID_WPARAM(wp);
        POINTER_INFO pointer;
        if (!GetPointerInfo(id,&pointer) || pointer.pointerType != PT_TOUCH ||
            (msg == WM_POINTERDOWN && !(pointer.pointerFlags & POINTER_FLAG_PRIMARY))) break;
        POINT pt = pointer.ptPixelLocation;
        ScreenToClient(wnd,&pt); pointerX = pt.x; pointerY = pt.y;
        if (msg == WM_POINTERDOWN) {
            activeId = id;
            armed = hits < 3 && ready() && inside(pt,&r) && !(pointer.pointerFlags & POINTER_FLAG_CANCELED);
        } else if (id == activeId) {
            if (armed && ready() && !(pointer.pointerFlags & POINTER_FLAG_CANCELED) && inside(pt,&r)) ++hits;
            armed = FALSE;
        }
        InvalidateRect(wnd,NULL,FALSE); return 0;
    }
    case WM_POINTERCAPTURECHANGED:
        if (GET_POINTERID_WPARAM(wp) == activeId) armed = FALSE;
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd,&ps);
        WCHAR text[160]; int y = 12, step = r.right < 950 ? 27 : 33;
        FillRect(dc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(dc,TRANSPARENT); SelectObject(dc,font);
        SetTextColor(dc,RGB(248,248,248)); line(dc,15,y,L"Switch Lite / Windows touch R1"); y+=step+4;
        for (int i=0;i<3;++i) {
            if (nodes[i].started) wsprintfW(text,L"%s   Started",nodes[i].label);
            else if (nodes[i].present) wsprintfW(text,L"%s   Present   PnP problem %lu",nodes[i].label,nodes[i].problem);
            else wsprintfW(text,L"%s   Not detected",nodes[i].label);
            line(dc,15,y,text); y+=step;
        }
        wsprintfW(text,L"Chip %04lX   Stage %lu   Status %08lX",chip,stage,status);line(dc,15,y,text);y+=step;
        wsprintfW(text,L"IRQs %lu   Events %lu   HID reports %lu   Errors %lu",irqs,events,reports,errors);
        line(dc,15,y,text);y+=step;
        if (pointerX>=0) {wsprintfW(text,L"Windows touch X %d   Y %d",pointerX,pointerY);line(dc,15,y,text);}
        if (hits==3 && ready() && reports>0) {
            SetTextColor(dc,RGB(103,239,158));line(dc,15,r.bottom/2,L"TOUCH PASS - 3 targets received by Windows");
        } else if (hits==3) {
            line(dc,15,r.bottom/2,L"Checking driver report counter...");
        } else {
            POINT p=target(&r,hits);int rad=radius(&r);
            SelectObject(dc,GetStockObject(DC_BRUSH)); SetDCBrushColor(dc,ready()?RGB(125,196,255):RGB(98,99,101));
            Ellipse(dc,p.x-rad,p.y-rad,p.x+rad,p.y+rad);
            wsprintfW(text,L"%d/3",hits+1);SetTextColor(dc,RGB(12,18,26));RECT tr={p.x-rad,p.y-rad,p.x+rad,p.y+rad};
            DrawTextW(dc,text,-1,&tr,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            SetTextColor(dc,RGB(237,237,237));line(dc,15,r.bottom-step-3, ready()?L"Tap and lift on each target.":L"Waiting for the touch devices.");
        }
        EndPaint(wnd,&ps);return 0;
    }
    case WM_DESTROY:
        KillTimer(wnd,1);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(wnd,msg,wp,lp);
}

void WINAPI start(void)
{
    // Windows ARM64 requires loader relocations. Keep the actual WNDPROC and
    // class name in the window class passed to USER32.
    static WNDCLASSW cls = {.lpfnWndProc = window_proc, .lpszClassName = L"Hdh001TouchR1"};
    HINSTANCE inst = GetModuleHandleW(NULL);
    cls.hInstance = inst;
    cls.hCursor = LoadCursorW(NULL,IDC_ARROW);
    if (!RegisterClassW(&cls)) ExitProcess(1);
    int width=GetSystemMetrics(SM_CXSCREEN),height=GetSystemMetrics(SM_CYSCREEN);
    font=CreateFontW(width<950?-20:-25,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,0,0,L"Segoe UI");
    HWND wnd=CreateWindowExW(0,cls.lpszClassName,L"Switch Lite touch",WS_POPUP|WS_VISIBLE,
                             0,0,width,height,NULL,NULL,inst,NULL);
    if (!wnd) ExitProcess(2);
    CreateWindowExW(0,L"BUTTON",L"Close",WS_VISIBLE|WS_CHILD|BS_PUSHBUTTON,
                    width-86,8,76,32,wnd,(HMENU)(UINT_PTR)1,inst,NULL);
    read_status(); SetTimer(wnd,1,800,NULL); UpdateWindow(wnd);
#ifdef LITE_DIAG_SMOKE
    ExitProcess(42);
#endif
    MSG msg;
    while (GetMessageW(&msg,NULL,0,0)>0) {TranslateMessage(&msg);DispatchMessageW(&msg);}
    if (font) DeleteObject(font);
    ExitProcess(0);
}
