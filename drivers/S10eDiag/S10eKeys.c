/*
 * s10ekeys.exe - user-session half of the S10e hardware buttons (user request 2026-10-11). S10eAgent (LocalSystem)
 * polls the key GPIOs and launches this into the console session for actions that need the desktop:
 *
 *   s10ekeys screen     toggle the display: asks Windows for the current console display state
 *                       (GUID_CONSOLE_DISPLAY_STATE, delivered right after registration) and turns it off
 *                       (SC_MONITORPOWER 2) or wakes it (input nudge + SC_MONITORPOWER -1)
 *   s10ekeys powermenu  wake the display and show Reboot / Recovery (TWRP) / Shut down / Cancel
 *   s10ekeys wake       wake the display only
 *
 * Recovery drops C:\S10eTmp\req\recovery.req for the agent (no UAC); reboot/shutdown use shutdown.exe.
 * Lines go to %TEMP%\s10ekeys.log. notes/d8-buttons-agent.md.
 */

#include <windows.h>
#include <stdio.h>

#define ID_REBOOT    101
#define ID_RECOVERY  102
#define ID_SHUTDOWN  103
#define ID_CANCEL    104

static int gDisplayState = -1;   // 0 off, 1 on, 2 dimmed
static int gDpi = 96;

static void
Log(const wchar_t *Text)
{
    wchar_t Path[MAX_PATH];
    FILE *File;
    SYSTEMTIME Now;

    if (GetEnvironmentVariableW(L"TEMP", Path, MAX_PATH) == 0) {
        return;
    }
    wcscat_s(Path, MAX_PATH, L"\\s10ekeys.log");
    if (_wfopen_s(&File, Path, L"a") == 0) {
        GetLocalTime(&Now);
        fwprintf(File, L"%02u:%02u:%02u.%03u %s\n", Now.wHour, Now.wMinute, Now.wSecond, Now.wMilliseconds, Text);
        fclose(File);
    }
}

static int
Px(int Value)
{
    return MulDiv(Value, gDpi, 96);
}

static void
WakeDisplay(void)
{
    INPUT Input[2];

    ZeroMemory(Input, sizeof(Input));
    Input[0].type = INPUT_MOUSE;
    Input[0].mi.dx = 1;
    Input[0].mi.dwFlags = MOUSEEVENTF_MOVE;
    Input[1] = Input[0];
    Input[1].mi.dx = -1;
    SendInput(2, Input, sizeof(INPUT));
    SendMessageTimeoutW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, (LPARAM)-1, SMTO_ABORTIFHUNG, 500, NULL);
}

static LRESULT CALLBACK
StateProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
    if ((Message == WM_POWERBROADCAST) && (WParam == PBT_POWERSETTINGCHANGE)) {
        POWERBROADCAST_SETTING *Setting = (POWERBROADCAST_SETTING *)LParam;

        if (IsEqualGUID(&Setting->PowerSetting, &GUID_CONSOLE_DISPLAY_STATE) && (Setting->DataLength >= sizeof(DWORD))) {
            gDisplayState = (int)*(DWORD *)Setting->Data;
            PostQuitMessage(0);
        }
        return TRUE;
    }
    return DefWindowProcW(Window, Message, WParam, LParam);
}

// The current console display state; -1 if Windows did not report it within 1 s.
static int
QueryDisplayState(HINSTANCE Instance)
{
    WNDCLASSW Class = { 0 };
    HWND Window;
    HPOWERNOTIFY Notify;
    MSG Message;
    UINT_PTR Timer;

    Class.lpfnWndProc = StateProc;
    Class.hInstance = Instance;
    Class.lpszClassName = L"S10eKeysState";
    RegisterClassW(&Class);
    Window = CreateWindowW(L"S10eKeysState", L"", 0, 0, 0, 0, 0, NULL, NULL, Instance, NULL);
    Notify = RegisterPowerSettingNotification(Window, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
    Timer = SetTimer(Window, 1, 1000, NULL);
    while ((gDisplayState < 0) && GetMessageW(&Message, NULL, 0, 0) > 0) {
        if ((Message.message == WM_TIMER) && (Message.wParam == Timer)) {
            break;
        }
        DispatchMessageW(&Message);
    }
    if (Notify != NULL) {
        UnregisterPowerSettingNotification(Notify);
    }
    DestroyWindow(Window);
    return gDisplayState;
}

static void
Run(const wchar_t *CommandLine)
{
    STARTUPINFOW Startup = { sizeof(Startup) };
    PROCESS_INFORMATION Process;
    wchar_t Buffer[MAX_PATH];

    wcscpy_s(Buffer, MAX_PATH, CommandLine);
    Startup.dwFlags = STARTF_USESHOWWINDOW;
    Startup.wShowWindow = SW_HIDE;
    if (CreateProcessW(NULL, Buffer, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &Startup, &Process)) {
        CloseHandle(Process.hThread);
        CloseHandle(Process.hProcess);
    }
}

static BOOL CALLBACK
SetChildFont(HWND Child, LPARAM Font)
{
    SendMessageW(Child, WM_SETFONT, (WPARAM)Font, TRUE);
    return TRUE;
}

static LRESULT CALLBACK
MenuProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
    switch (Message) {
    case WM_CREATE: {
        static const struct { int Id; const wchar_t *Text; } Buttons[] = {
            { ID_REBOOT, L"Reboot" }, { ID_RECOVERY, L"Reboot to recovery (TWRP)" },
            { ID_SHUTDOWN, L"Shut down" }, { ID_CANCEL, L"Cancel" }
        };
        LOGFONTW Face = { 0 };
        int Index;

        for (Index = 0; Index < 4; Index++) {
            CreateWindowW(L"BUTTON", Buttons[Index].Text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                          Px(16), Px(16 + Index * 64), Px(288), Px(52), Window, (HMENU)(INT_PTR)Buttons[Index].Id,
                          NULL, NULL);
        }
        Face.lfHeight = -Px(18);
        wcscpy_s(Face.lfFaceName, LF_FACESIZE, L"Segoe UI");
        EnumChildWindows(Window, SetChildFont, (LPARAM)CreateFontIndirectW(&Face));
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(WParam)) {
        case ID_REBOOT:
            Log(L"powermenu: reboot");
            Run(L"C:\\Windows\\System32\\shutdown.exe /r /t 0");
            break;
        case ID_RECOVERY: {
            HANDLE File;

            Log(L"powermenu: recovery");
            CreateDirectoryW(L"C:\\S10eTmp\\req", NULL);
            File = CreateFileW(L"C:\\S10eTmp\\req\\recovery.req", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            if (File != INVALID_HANDLE_VALUE) {
                CloseHandle(File);
            }
            break;
        }
        case ID_SHUTDOWN:
            Log(L"powermenu: shutdown");
            Run(L"C:\\Windows\\System32\\shutdown.exe /s /t 0");
            break;
        default:
            Log(L"powermenu: cancel");
            break;
        }
        DestroyWindow(Window);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(WParam) == WA_INACTIVE) {
            DestroyWindow(Window);   // tap outside = cancel
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(Window, Message, WParam, LParam);
}

static void
PowerMenu(HINSTANCE Instance)
{
    WNDCLASSW Class = { 0 };
    RECT Rect = { 0, 0, 0, 0 };
    HWND Window;
    MSG Message;
    int Width, Height;

    Class.lpfnWndProc = MenuProc;
    Class.hInstance = Instance;
    Class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    Class.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    Class.lpszClassName = L"S10eKeysMenu";
    RegisterClassW(&Class);
    Rect.right = Px(320);
    Rect.bottom = Px(16 + 4 * 64);
    AdjustWindowRectEx(&Rect, WS_POPUP | WS_CAPTION, FALSE, WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
    Width = Rect.right - Rect.left;
    Height = Rect.bottom - Rect.top;
    Window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"S10eKeysMenu", L"Power", WS_POPUP | WS_CAPTION,
                             (GetSystemMetrics(SM_CXSCREEN) - Width) / 2, (GetSystemMetrics(SM_CYSCREEN) - Height) / 2,
                             Width, Height, NULL, NULL, Instance, NULL);
    ShowWindow(Window, SW_SHOW);
    SetForegroundWindow(Window);
    while (GetMessageW(&Message, NULL, 0, 0) > 0) {
        TranslateMessage(&Message);
        DispatchMessageW(&Message);
    }
}

int WINAPI
wWinMain(HINSTANCE Instance, HINSTANCE Previous, PWSTR CommandLine, int Show)
{
    HDC Screen;
    wchar_t Text[96];

    UNREFERENCED_PARAMETER(Previous);
    UNREFERENCED_PARAMETER(Show);
    SetProcessDPIAware();
    Screen = GetDC(NULL);
    gDpi = GetDeviceCaps(Screen, LOGPIXELSX);
    ReleaseDC(NULL, Screen);

    if (_wcsicmp(CommandLine, L"screen") == 0) {
        int State = QueryDisplayState(Instance);

        swprintf_s(Text, 96, L"screen: display state %d -> %s", State, (State == 1) ? L"off" : L"on");
        Log(Text);
        if (State == 1) {
            SendMessageTimeoutW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, 2, SMTO_ABORTIFHUNG, 500, NULL);
        } else {
            WakeDisplay();
        }
    } else if (_wcsicmp(CommandLine, L"powermenu") == 0) {
        Log(L"powermenu: shown");
        WakeDisplay();
        PowerMenu(Instance);
    } else if (_wcsicmp(CommandLine, L"wake") == 0) {
        WakeDisplay();
    } else {
        swprintf_s(Text, 96, L"unknown command '%.60s'", CommandLine);
        Log(Text);
        return 1;
    }
    return 0;
}
