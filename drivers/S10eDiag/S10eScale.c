/*
 * s10escale.exe - pick a display scale from 100 % to 300 % in 25 % steps (user request 2026-10-11: Settings
 * stops at 175 % on this 1080x2280 panel; 175 felt too small, the default too large).
 *
 * Uses Windows' own "custom scaling" (the value Settings > Display > Custom scaling writes): HKCU\Control
 * Panel\Desktop LogPixels = 96 * scale / 100 and Win8DpiScaling = 1. It accepts any value, but Windows reads it
 * at sign-in, so it takes effect after signing out. 100 % removes the custom value (back to Windows' choice).
 * Runs in the user's session, no admin. Every apply appends a line to %TEMP%\s10escale.log.
 */

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>

#define SCALE_MIN     100
#define SCALE_MAX     300
#define SCALE_STEP    25
#define ID_SLIDER     100
#define ID_APPLY      101
#define ID_SIGNOUT    102
#define ID_LABEL      103

static const wchar_t *DESKTOP_KEY = L"Control Panel\\Desktop";
static HWND gSlider, gLabel;
static int gDpi = 96;

static int
Px(int Value)
{
    return MulDiv(Value, gDpi, 96);
}

static int
CurrentScale(void)
{
    DWORD Value = 0, Size = sizeof(Value), Enabled = 0;
    HKEY Key;
    int Scale = 0;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, DESKTOP_KEY, 0, KEY_READ, &Key) == ERROR_SUCCESS) {
        if (RegQueryValueExW(Key, L"Win8DpiScaling", NULL, NULL, (BYTE *)&Enabled, &Size) == ERROR_SUCCESS && Enabled) {
            Size = sizeof(Value);
            if (RegQueryValueExW(Key, L"LogPixels", NULL, NULL, (BYTE *)&Value, &Size) == ERROR_SUCCESS && Value) {
                Scale = (int)(Value * 100 / 96);
            }
        }
        RegCloseKey(Key);
    }
    if (Scale == 0) {
        Scale = MulDiv(gDpi, 100, 96);   // what this session runs at
    }
    Scale = ((Scale + SCALE_STEP / 2) / SCALE_STEP) * SCALE_STEP;
    return (Scale < SCALE_MIN) ? SCALE_MIN : (Scale > SCALE_MAX) ? SCALE_MAX : Scale;
}

static BOOL
ApplyScale(int Scale)
{
    HKEY Key;
    LSTATUS Status;
    DWORD LogPixels = (DWORD)(96 * Scale / 100);
    DWORD Enabled = (Scale == SCALE_MIN) ? 0 : 1;
    wchar_t Path[MAX_PATH];
    FILE *File;

    Status = RegOpenKeyExW(HKEY_CURRENT_USER, DESKTOP_KEY, 0, KEY_SET_VALUE, &Key);
    if (Status == ERROR_SUCCESS) {
        if (Enabled) {
            Status = RegSetValueExW(Key, L"LogPixels", 0, REG_DWORD, (const BYTE *)&LogPixels, sizeof(LogPixels));
        } else {
            RegDeleteValueW(Key, L"LogPixels");
        }
        if (Status == ERROR_SUCCESS) {
            Status = RegSetValueExW(Key, L"Win8DpiScaling", 0, REG_DWORD, (const BYTE *)&Enabled, sizeof(Enabled));
        }
        RegCloseKey(Key);
    }
    if (GetEnvironmentVariableW(L"TEMP", Path, MAX_PATH) != 0) {
        wcscat_s(Path, MAX_PATH, L"\\s10escale.log");
        if (_wfopen_s(&File, Path, L"a") == 0) {
            fwprintf(File, L"scale %d%% LogPixels %lu Win8DpiScaling %lu -> status %ld\n", Scale, LogPixels, Enabled, Status);
            fclose(File);
        }
    }
    return Status == ERROR_SUCCESS;
}

static int
SliderScale(void)
{
    return SCALE_MIN + (int)SendMessageW(gSlider, TBM_GETPOS, 0, 0) * SCALE_STEP;
}

static void
UpdateLabel(void)
{
    wchar_t Text[64];

    swprintf_s(Text, 64, L"Scale: %d %%", SliderScale());
    SetWindowTextW(gLabel, Text);
}

static BOOL CALLBACK
SetChildFont(HWND Child, LPARAM Font)
{
    SendMessageW(Child, WM_SETFONT, (WPARAM)Font, TRUE);
    return TRUE;
}

static LRESULT CALLBACK
WndProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
    switch (Message) {
    case WM_CREATE: {
        HFONT Font;
        NONCLIENTMETRICSW Metrics = { sizeof(Metrics) };
        int Width = Px(320);

        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(Metrics), &Metrics, 0);
        Font = CreateFontIndirectW(&Metrics.lfMessageFont);
        gLabel = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER,
                               Px(10), Px(10), Width, Px(24), Window, (HMENU)ID_LABEL, NULL, NULL);
        gSlider = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_HORZ,
                                Px(10), Px(40), Width, Px(40), Window, (HMENU)ID_SLIDER, NULL, NULL);
        SendMessageW(gSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, (SCALE_MAX - SCALE_MIN) / SCALE_STEP));
        SendMessageW(gSlider, TBM_SETPOS, TRUE, (CurrentScale() - SCALE_MIN) / SCALE_STEP);
        CreateWindowW(L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                      Px(10), Px(90), Px(150), Px(36), Window, (HMENU)ID_APPLY, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Apply + sign out", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      Px(180), Px(90), Px(150), Px(36), Window, (HMENU)ID_SIGNOUT, NULL, NULL);
        CreateWindowW(L"STATIC", L"Takes effect after signing out and back in.", WS_CHILD | WS_VISIBLE | SS_CENTER,
                      Px(10), Px(134), Width, Px(24), Window, NULL, NULL, NULL);
        EnumChildWindows(Window, SetChildFont, (LPARAM)Font);
        UpdateLabel();
        return 0;
    }
    case WM_HSCROLL:
        UpdateLabel();
        return 0;
    case WM_COMMAND:
        if (LOWORD(WParam) == ID_APPLY || LOWORD(WParam) == ID_SIGNOUT) {
            if (!ApplyScale(SliderScale())) {
                MessageBoxW(Window, L"Could not write the scale setting.", L"S10e scale", MB_ICONERROR);
            } else if (LOWORD(WParam) == ID_SIGNOUT) {
                ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
            } else {
                MessageBoxW(Window, L"Saved. Sign out and back in to apply it.", L"S10e scale", MB_ICONINFORMATION);
            }
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(Window, Message, WParam, LParam);
}

int WINAPI
wWinMain(HINSTANCE Instance, HINSTANCE Previous, PWSTR CommandLine, int Show)
{
    INITCOMMONCONTROLSEX Controls = { sizeof(Controls), ICC_BAR_CLASSES };
    WNDCLASSW Class = { 0 };
    HWND Window;
    HDC Screen;
    MSG Message;
    RECT Rect;

    UNREFERENCED_PARAMETER(Previous);
    UNREFERENCED_PARAMETER(CommandLine);
    SetProcessDPIAware();
    Screen = GetDC(NULL);
    gDpi = GetDeviceCaps(Screen, LOGPIXELSX);
    ReleaseDC(NULL, Screen);
    InitCommonControlsEx(&Controls);

    Class.lpfnWndProc = WndProc;
    Class.hInstance = Instance;
    Class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    Class.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    Class.lpszClassName = L"S10eScale";
    RegisterClassW(&Class);

    Rect.left = 0;
    Rect.top = 0;
    Rect.right = Px(340);
    Rect.bottom = Px(166);
    AdjustWindowRect(&Rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    Window = CreateWindowW(L"S10eScale", L"Display scale", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, Rect.right - Rect.left, Rect.bottom - Rect.top,
                           NULL, NULL, Instance, NULL);
    ShowWindow(Window, Show);
    while (GetMessageW(&Message, NULL, 0, 0) > 0) {
        TranslateMessage(&Message);
        DispatchMessageW(&Message);
    }
    return 0;
}
