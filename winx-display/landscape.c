// SPDX-License-Identifier: MIT
// Apply the Lite panel's default orientation once per user. Windows retains it.
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>

static UINT_PTR image_cookie = 0x00002b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used = {
    .Size = sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64), .SecurityCookie = (ULONG_PTR)&image_cookie
};
static const WCHAR Key[] = L"Software\\SwitchLite\\Display";
static void Record(PCWSTR Name, DWORD Value)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, Key, 0, NULL, 0, KEY_SET_VALUE,
                        NULL, &key, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(key, Name, 0, REG_DWORD, (const BYTE *)&Value, sizeof(Value));
        RegCloseKey(key);
    }
}

void start(void)
{
    DEVMODEW mode = {0};
    HKEY key;
    DWORD applied = 0, size = sizeof(applied), type = 0;
    LONG result;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, Key, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        RegQueryValueExW(key, L"DefaultLandscapeApplied", NULL, &type, (BYTE *)&applied, &size);
        RegCloseKey(key);
        if (type == REG_DWORD && applied == 1) ExitProcess(0);
    }
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsExW(NULL, ENUM_CURRENT_SETTINGS, &mode, 0)) {
        Record(L"LastError", GetLastError());
        ExitProcess(1);
    }
    if (mode.dmPelsWidth == 1280 && mode.dmPelsHeight == 720) {
        Record(L"DefaultLandscapeApplied", 1);
        ExitProcess(0);
    }
    if (mode.dmPelsWidth != 720 || mode.dmPelsHeight != 1280) {
        Record(L"LastError", ERROR_NOT_SUPPORTED);
        ExitProcess(2);
    }
    // KMDOD Rotate90 maps (x,y) to panel (y,1279-x), matching Hekate ROTATE=2
    // and the touch driver's existing portrait HID coordinate transform.
    mode.dmDisplayOrientation = DMDO_90;
    mode.dmPelsWidth = 1280;
    mode.dmPelsHeight = 720;
    mode.dmFields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT;
    result = ChangeDisplaySettingsExW(NULL, &mode, NULL, CDS_UPDATEREGISTRY, NULL);
    Record(L"LastDisplayResult", (DWORD)result);
    if (result == DISP_CHANGE_SUCCESSFUL) Record(L"DefaultLandscapeApplied", 1);
    ExitProcess(result == DISP_CHANGE_SUCCESSFUL ? 0 : 3);
}
