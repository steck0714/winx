// SPDX-License-Identifier: MIT
// VM integration check: report the current mode, rotate it, and restore it.
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
static UINT_PTR image_cookie = 0x00002b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used = {
    .Size = sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64), .SecurityCookie = (ULONG_PTR)&image_cookie
};
void start(void)
{
    DEVMODEW original = {0}, rotated;
    char text[1024];
    DWORD bytes, length = 0;
    LONG change = DISP_CHANGE_FAILED, restore = DISP_CHANGE_FAILED;
    HANDLE file;
    original.dmSize = sizeof(original);
    if (EnumDisplaySettingsExW(NULL, ENUM_CURRENT_SETTINGS, &original, 0)) {
        length += wsprintfA(text + length, "Current %lu x %lu orientation %lu\r\n",
            original.dmPelsWidth, original.dmPelsHeight, original.dmDisplayOrientation);
        rotated = original;
        rotated.dmDisplayOrientation = (original.dmDisplayOrientation + 1) % 4;
        rotated.dmPelsWidth = original.dmPelsHeight;
        rotated.dmPelsHeight = original.dmPelsWidth;
        rotated.dmFields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT;
        change = ChangeDisplaySettingsExW(NULL, &rotated, NULL, 0, NULL);
        length += wsprintfA(text + length, "Rotate result %ld\r\n", change);
        if (change == DISP_CHANGE_SUCCESSFUL) {
            DEVMODEW actual = {0};
            actual.dmSize = sizeof(actual);
            if (EnumDisplaySettingsExW(NULL, ENUM_CURRENT_SETTINGS, &actual, 0))
                length += wsprintfA(text + length, "Rotated %lu x %lu orientation %lu\r\n",
                    actual.dmPelsWidth, actual.dmPelsHeight, actual.dmDisplayOrientation);
            original.dmFields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT;
            restore = ChangeDisplaySettingsExW(NULL, &original, NULL, 0, NULL);
            length += wsprintfA(text + length, "Restore result %ld\r\n", restore);
        }
    } else length += wsprintfA(text + length, "EnumDisplaySettings failed %lu\r\n", GetLastError());
    file = CreateFileW(L"X:\\display-smoke.txt", GENERIC_WRITE, FILE_SHARE_READ,
                       NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) { WriteFile(file, text, length, &bytes, NULL); CloseHandle(file); }
    ExitProcess(change == DISP_CHANGE_SUCCESSFUL && restore == DISP_CHANGE_SUCCESSFUL ? 0 : 1);
}
