/*
 * s10ereboot.exe - "Reboot to TWRP" desktop button (user request 2026-10-11: the .cmd launcher failed with
 * "Windows cannot access the specified device, path or file" while every other .cmd launcher worked).
 * Drops C:\S10eTmp\req\recovery.req (Users-writable, no elevation needed); the S10eAgent service
 * (LocalSystem) polls that folder every 2 s and runs `s10ediag flushreset` (recovery reason + flush + reset).
 * If the request is still there after 10 s the agent is not running - say so instead of failing silently.
 */

#include <windows.h>
#include <stdio.h>

#define REQ_DIR   L"C:\\S10eTmp\\req"
#define REQ_FILE  L"C:\\S10eTmp\\req\\recovery.req"

int WINAPI
wWinMain(HINSTANCE Instance, HINSTANCE Prev, PWSTR Cmd, int Show)
{
    HANDLE File;
    DWORD  Written, Error;
    WCHAR  Text[256];
    int    Waited;

    (void)Instance;
    (void)Prev;
    (void)Cmd;
    (void)Show;

    // 2026-10-12: an unintended tap (touch double-click bug, parkinglot/touch-double-tap.md) sent the phone to TWRP
    // mid-session. Ask first.
    if (MessageBoxW(NULL, L"Reboot to TWRP now?", L"Reboot to TWRP",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_TOPMOST) != IDYES) {
        return 0;
    }

    CreateDirectoryW(L"C:\\S10eTmp", NULL);
    CreateDirectoryW(REQ_DIR, NULL);
    File = CreateFileW(REQ_FILE, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE) {
        Error = GetLastError();
        swprintf_s(Text, ARRAYSIZE(Text), L"Could not create %s (error %lu).", REQ_FILE, Error);
        MessageBoxW(NULL, Text, L"Reboot to TWRP", MB_OK | MB_ICONERROR);
        return 1;
    }
    WriteFile(File, "x\r\n", 3, &Written, NULL);
    CloseHandle(File);

    // The agent deletes the file when it acts on it; the reset follows within a second or two.
    for (Waited = 0; Waited < 10000; Waited += 250) {
        if (GetFileAttributesW(REQ_FILE) == INVALID_FILE_ATTRIBUTES) {
            Sleep(5000);   // the reset normally lands here
            return 0;
        }
        Sleep(250);
    }
    MessageBoxW(NULL, L"The request was not picked up within 10 s - the S10eAgent service is not running.\n"
                      L"Use Volume Down + Power (10 s), then Volume Up + Bixby + Power for TWRP.",
                L"Reboot to TWRP", MB_OK | MB_ICONWARNING);
    return 2;
}
