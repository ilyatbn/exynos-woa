/* wifi-connect - set the network the phone's WiFi driver (Exynos9820Wlan, ACPI\SAMS9825) joins.
   Writes HKLM\SYSTEM\CurrentControlSet\Services\Exynos9820Wlan\Parameters Ssid + Passphrase (REG_SZ; a 64-hex-digit
   key goes to Psk instead), restarts the device (disable + enable) so the driver reconnects without a reboot, and
   shows the tail of its run log. The password goes only into that registry value (the driver reads it there); it
   is never logged or saved anywhere else. Needs administrator rights (manifest). Win32, no runtime dependencies.
   notes/m12-wifi-m7.md. */
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <wchar.h>

#define ID_SSID   101
#define ID_PASS   102
#define ID_GO     103
#define ID_LOG    104
#define ID_SHOW   105
#define ID_LOGBTN 106

static const wchar_t *kParams = L"SYSTEM\\CurrentControlSet\\Services\\Exynos9820Wlan\\Parameters";
static const wchar_t *kHwid   = L"ACPI\\SAMS9825";
static const wchar_t *kLog    = L"C:\\Windows\\System32\\LogFiles\\Exynos9820Wlan-run.txt";
static HWND gSsid, gPass, gLog;

static void SetLog(const wchar_t *Text)
{
    SetWindowTextW(gLog, Text);
}

static void ShowLogTail(void)
{
    static char    Buf[16384];
    static wchar_t W[16384];
    HANDLE         F = CreateFileW(kLog, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER  Size;
    DWORD          Got = 0, I;

    if (F == INVALID_HANDLE_VALUE) {
        SetLog(L"(no run log yet - the driver writes it about 30 s after it starts)");
        return;
    }
    GetFileSizeEx(F, &Size);
    if (Size.QuadPart > (LONGLONG)sizeof(Buf) - 1) {
        LARGE_INTEGER Off;
        Off.QuadPart = Size.QuadPart - (LONGLONG)(sizeof(Buf) - 1);
        SetFilePointerEx(F, Off, NULL, FILE_BEGIN);
    }
    ReadFile(F, Buf, sizeof(Buf) - 1, &Got, NULL);
    CloseHandle(F);
    for (I = 0; I < Got; I++) {
        W[I] = (wchar_t)(unsigned char)Buf[I];
    }
    W[Got] = 0;
    SetLog(W);
    SendMessageW(gLog, EM_SETSEL, Got, Got);
    SendMessageW(gLog, EM_SCROLLCARET, 0, 0);
}

/* Disable + enable every present device with the WLAN hardware id (SetupAPI DIF_PROPERTYCHANGE). */
static BOOL RestartWlan(wchar_t *Msg, size_t MsgLen)
{
    HDEVINFO        Set = SetupDiGetClassDevsW(NULL, L"ACPI", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    SP_DEVINFO_DATA D;
    DWORD           I, Found = 0, Ok = 0;
    wchar_t         Ids[1024];

    if (Set == INVALID_HANDLE_VALUE) {
        swprintf(Msg, MsgLen, L"SetupDiGetClassDevs failed (%lu)", GetLastError());
        return FALSE;
    }
    D.cbSize = sizeof(D);
    for (I = 0; SetupDiEnumDeviceInfo(Set, I, &D); I++) {
        wchar_t *P;

        ZeroMemory(Ids, sizeof(Ids));
        if (!SetupDiGetDeviceRegistryPropertyW(Set, &D, SPDRP_HARDWAREID, NULL, (PBYTE)Ids, sizeof(Ids) - 4, NULL)) {
            continue;
        }
        for (P = Ids; *P; P += wcslen(P) + 1) {
            if (_wcsicmp(P, kHwid) == 0) {
                SP_PROPCHANGE_PARAMS Pc;
                DWORD                Step, Good = 1;

                Found++;
                for (Step = 0; Step < 2; Step++) {
                    ZeroMemory(&Pc, sizeof(Pc));
                    Pc.ClassInstallHeader.cbSize          = sizeof(SP_CLASSINSTALL_HEADER);
                    Pc.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
                    Pc.StateChange                        = Step == 0 ? DICS_DISABLE : DICS_ENABLE;
                    Pc.Scope                              = DICS_FLAG_GLOBAL;
                    if (!SetupDiSetClassInstallParamsW(Set, &D, &Pc.ClassInstallHeader, sizeof(Pc)) ||
                        !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, Set, &D)) {
                        Good = 0;
                    }
                    if (Step == 0) {
                        Sleep(1500);
                    }
                }
                Ok += Good;
                break;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(Set);
    if (Found == 0) {
        swprintf(Msg, MsgLen, L"WLAN device (%s) not found", kHwid);
    } else {
        swprintf(Msg, MsgLen, L"Saved. WLAN driver restarted (%lu of %lu ok). The join takes about 40 s; this log refreshes "
                 L"every 5 s. Look for \"JOIN 0\".", Ok, Found);
    }
    return Found != 0 && Ok == Found;
}

static void Connect(HWND Wnd)
{
    wchar_t Ssid[64], Pass[128], Msg[256];
    HKEY    K;
    LONG    Rc;
    size_t  Pl;

    GetWindowTextW(gSsid, Ssid, 64);
    GetWindowTextW(gPass, Pass, 128);
    Pl = wcslen(Pass);
    if (wcslen(Ssid) == 0 || wcslen(Ssid) > 32) {
        MessageBoxW(Wnd, L"The network name must be 1-32 characters.", L"wifi-connect", MB_ICONWARNING);
        return;
    }
    if (Pl != 64 && (Pl < 8 || Pl > 63)) {
        MessageBoxW(Wnd, L"The password must be 8-63 characters (WPA2-PSK).", L"wifi-connect", MB_ICONWARNING);
        SecureZeroMemory(Pass, sizeof(Pass));
        return;
    }
    Rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kParams, 0, NULL, 0, KEY_SET_VALUE, NULL, &K, NULL);
    if (Rc == ERROR_SUCCESS) {
        Rc = RegSetValueExW(K, L"Ssid", 0, REG_SZ, (const BYTE *)Ssid, (DWORD)((wcslen(Ssid) + 1) * sizeof(wchar_t)));
        if (Rc == ERROR_SUCCESS) {
            Rc = RegSetValueExW(K, Pl == 64 ? L"Psk" : L"Passphrase", 0, REG_SZ, (const BYTE *)Pass,
                                (DWORD)((Pl + 1) * sizeof(wchar_t)));
            RegDeleteValueW(K, Pl == 64 ? L"Passphrase" : L"Psk");
        }
        RegCloseKey(K);
    }
    SecureZeroMemory(Pass, sizeof(Pass));
    SetWindowTextW(gPass, L"");
    if (Rc != ERROR_SUCCESS) {
        swprintf(Msg, 256, L"Could not write the registry (error %ld). Run as administrator.", Rc);
        MessageBoxW(Wnd, Msg, L"wifi-connect", MB_ICONERROR);
        return;
    }
    SetLog(L"Saved. Restarting the WiFi driver...");
    UpdateWindow(gLog);
    RestartWlan(Msg, 256);
    SetLog(Msg);
    SetTimer(Wnd, 1, 5000, NULL);
}

static LRESULT CALLBACK WndProc(HWND Wnd, UINT M, WPARAM W, LPARAM L)
{
    static int Ticks;

    switch (M) {
    case WM_CREATE: {
        HFONT F = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND  C[8];
        int   I = 0;

        C[I++] = CreateWindowW(L"STATIC", L"Network name (SSID):", WS_CHILD | WS_VISIBLE, 12, 14, 150, 20, Wnd, NULL, NULL, NULL);
        C[I++] = gSsid = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                         170, 10, 300, 24, Wnd, (HMENU)ID_SSID, NULL, NULL);
        C[I++] = CreateWindowW(L"STATIC", L"Password:", WS_CHILD | WS_VISIBLE, 12, 48, 150, 20, Wnd, NULL, NULL, NULL);
        C[I++] = gPass = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL, 170, 44, 300, 24, Wnd,
                                         (HMENU)ID_PASS, NULL, NULL);
        C[I++] = CreateWindowW(L"BUTTON", L"Show", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 480, 46, 70, 20, Wnd,
                               (HMENU)ID_SHOW, NULL, NULL);
        C[I++] = CreateWindowW(L"BUTTON", L"Connect", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 170, 80, 120, 30, Wnd,
                               (HMENU)ID_GO, NULL, NULL);
        C[I++] = CreateWindowW(L"BUTTON", L"Show log", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 300, 80, 120, 30, Wnd,
                               (HMENU)ID_LOGBTN, NULL, NULL);
        C[I++] = gLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                        12, 122, 560, 300, Wnd, (HMENU)ID_LOG, NULL, NULL);
        for (I = 0; I < 8; I++) {
            SendMessageW(C[I], WM_SETFONT, (WPARAM)F, TRUE);
        }
        {
            HKEY    K;
            wchar_t Ssid[64];
            DWORD   Sz = sizeof(Ssid) - sizeof(wchar_t);

            ZeroMemory(Ssid, sizeof(Ssid));
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kParams, 0, KEY_QUERY_VALUE, &K) == ERROR_SUCCESS) {
                if (RegQueryValueExW(K, L"Ssid", NULL, NULL, (BYTE *)Ssid, &Sz) == ERROR_SUCCESS) {
                    SetWindowTextW(gSsid, Ssid);
                }
                RegCloseKey(K);
            }
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(W) == ID_GO) {
            Ticks = 0;
            Connect(Wnd);
        } else if (LOWORD(W) == ID_LOGBTN) {
            ShowLogTail();
        } else if (LOWORD(W) == ID_SHOW) {
            BOOL On = SendMessageW((HWND)L, BM_GETCHECK, 0, 0) == BST_CHECKED;

            SendMessageW(gPass, EM_SETPASSWORDCHAR, On ? 0 : 0x25CF, 0);
            InvalidateRect(gPass, NULL, TRUE);
        }
        return 0;
    case WM_TIMER:
        ShowLogTail();
        if (++Ticks >= 12) {
            KillTimer(Wnd, 1);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(Wnd, M, W, L);
}

int WINAPI wWinMain(HINSTANCE Inst, HINSTANCE Prev, PWSTR Cmd, int Show)
{
    WNDCLASSW C;
    HWND      Wnd;
    MSG       M;

    (void)Prev;
    (void)Cmd;
    ZeroMemory(&C, sizeof(C));
    C.lpfnWndProc   = WndProc;
    C.hInstance     = Inst;
    C.hCursor       = LoadCursor(NULL, IDC_ARROW);
    C.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    C.lpszClassName = L"S10eWifiConnect";
    RegisterClassW(&C);
    Wnd = CreateWindowW(C.lpszClassName, L"Phone WiFi - connect", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                        CW_USEDEFAULT, CW_USEDEFAULT, 600, 470, NULL, NULL, Inst, NULL);
    ShowWindow(Wnd, Show);
    while (GetMessageW(&M, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(Wnd, &M)) {
            TranslateMessage(&M);
            DispatchMessageW(&M);
        }
    }
    return 0;
}
