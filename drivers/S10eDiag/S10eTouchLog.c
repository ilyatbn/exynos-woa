/*
 * s10etouchlog.exe - what Windows actually receives from the touchscreen (notes/m12-touch.md, "right-click").
 * User report 2026-10-11: press-and-hold does not right-click. A full-screen window logs, with ms timestamps:
 *   HID   - every raw report of the Touch Screen collection (UsagePage 0x0D / Usage 0x04, RIDEV_INPUTSINK),
 *           parsed per contact (tip, id, x, y), plus the gap since the previous report
 *   PTR   - WM_POINTERDOWN/UPDATE/UP (what the touch stack made of it)
 *   MOUSE - the mouse messages DefWindowProc synthesizes from the pointer messages, including the
 *           press-and-hold right-click (WM_RBUTTONDOWN/UP, WM_CONTEXTMENU) if it fires at all
 * plus per contact: down -> up duration, report count, longest gap between reports while down.
 * Header: GetSystemMetrics(SM_DIGITIZER / SM_MAXIMUMTOUCHES) and the per-user Wisp\Touch registry values.
 *
 *   s10etouchlog [seconds]      default 40; output C:\S10eTmp\req\touchlog.txt (Users-writable)
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CONTACT 16

typedef struct {
    BOOL      Down;
    ULONGLONG DownMs;
    ULONGLONG LastMs;
    ULONGLONG MaxGapMs;
    unsigned  Reports;
} CONTACT;

static FILE      *gLog;
static ULONGLONG  gStartMs;
static ULONGLONG  gLastHidMs;
static unsigned   gSeconds = 40;
static unsigned   gHidReports, gPointerMsgs, gRightClicks, gContextMenus;
static CONTACT    gContacts[MAX_CONTACT];
static WCHAR      gStatus[256] = L"";

static ULONGLONG
Ms(void)
{
    return GetTickCount64() - gStartMs;
}

static void
Log(const char *Fmt, ...)
{
    va_list Args;

    fprintf(gLog, "%7llu ", Ms());
    va_start(Args, Fmt);
    vfprintf(gLog, Fmt, Args);
    va_end(Args);
    fputc('\n', gLog);
}

static void
DumpWispTouch(void)
{
    HKEY  Key;
    DWORD Index;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Wisp\\Touch", 0, KEY_READ, &Key) != ERROR_SUCCESS) {
        fprintf(gLog, "HKCU\\Software\\Microsoft\\Wisp\\Touch: absent (defaults apply)\n");
        return;
    }
    fprintf(gLog, "HKCU\\Software\\Microsoft\\Wisp\\Touch:\n");
    for (Index = 0;; Index++) {
        WCHAR Name[128];
        BYTE  Data[256];
        DWORD NameLen = ARRAYSIZE(Name), DataLen = sizeof(Data), Type;

        if (RegEnumValueW(Key, Index, Name, &NameLen, NULL, &Type, Data, &DataLen) != ERROR_SUCCESS) {
            break;
        }
        if (Type == REG_DWORD) {
            fprintf(gLog, "  %ls = %lu\n", Name, *(DWORD *)Data);
        } else {
            fprintf(gLog, "  %ls (type %lu, %lu bytes)\n", Name, Type, DataLen);
        }
    }
    RegCloseKey(Key);
}

//
// Report 0x54 layout (S6SY761TouchBeyond0 Device.c descriptor): id, 10 x {state, id, x lo, x hi, y lo, y hi},
// scan time (2), contact count (1). Parse by the contact count rather than trusting the length.
//
static void
OnHidReport(const BYTE *Data, DWORD Size)
{
    ULONGLONG Now = Ms();
    char      Line[512];
    int       Used;
    unsigned  Count, I;

    gHidReports++;
    if (Size < 1 + 60 + 3) {
        Log("HID   len %lu (unexpected)", Size);
        return;
    }
    Count = Data[1 + 60 + 2];
    Used = sprintf_s(Line, sizeof(Line), "HID   gap %4llu scan %5u n %u:", Now - gLastHidMs,
                     (unsigned)(Data[61] | (Data[62] << 8)), Count);
    gLastHidMs = Now;
    for (I = 0; I < Count && I < 10; I++) {
        const BYTE *P = Data + 1 + I * 6;
        unsigned    Tip = P[0] & 1, Id = P[1];
        unsigned    X = P[2] | ((P[3] & 0x0F) << 8), Y = P[4] | ((P[5] & 0x0F) << 8);

        Used += sprintf_s(Line + Used, sizeof(Line) - Used, " [id %u %s %u,%u]", Id, Tip ? "DOWN" : "up", X, Y);
        if (Id < MAX_CONTACT) {
            CONTACT *C = &gContacts[Id];
            if (Tip && !C->Down) {
                C->Down = TRUE;
                C->DownMs = C->LastMs = Now;
                C->MaxGapMs = 0;
                C->Reports = 0;
            }
            if (C->Down) {
                if (Now - C->LastMs > C->MaxGapMs) {
                    C->MaxGapMs = Now - C->LastMs;
                }
                C->LastMs = Now;
                C->Reports++;
            }
            if (!Tip && C->Down) {
                C->Down = FALSE;
                Log("%s", Line);
                Log("CONTACT id %u held %llu ms, %u reports, longest gap between reports %llu ms",
                    Id, Now - C->DownMs, C->Reports, C->MaxGapMs);
                Used = sprintf_s(Line, sizeof(Line), "      (continued)");
            }
        }
    }
    Log("%s", Line);
}

static LRESULT CALLBACK
WndProc(HWND Wnd, UINT Msg, WPARAM WParam, LPARAM LParam)
{
    switch (Msg) {
    case WM_INPUT: {
        UINT  Size = 0;
        BYTE *Buffer;

        GetRawInputData((HRAWINPUT)LParam, RID_INPUT, NULL, &Size, sizeof(RAWINPUTHEADER));
        Buffer = (BYTE *)malloc(Size);
        if (Buffer != NULL &&
            GetRawInputData((HRAWINPUT)LParam, RID_INPUT, Buffer, &Size, sizeof(RAWINPUTHEADER)) == Size) {
            RAWINPUT *Raw = (RAWINPUT *)Buffer;
            DWORD     R;

            if (Raw->header.dwType == RIM_TYPEHID) {
                for (R = 0; R < Raw->data.hid.dwCount; R++) {
                    OnHidReport(Raw->data.hid.bRawData + R * Raw->data.hid.dwSizeHid, Raw->data.hid.dwSizeHid);
                }
            }
        }
        free(Buffer);
        break;
    }
    case WM_POINTERDOWN:
    case WM_POINTERUPDATE:
    case WM_POINTERUP: {
        UINT32             Id = GET_POINTERID_WPARAM(WParam);
        POINTER_INPUT_TYPE Type = 0;

        gPointerMsgs++;
        GetPointerType(Id, &Type);
        // UPDATEs are frequent; log them only when a flag other than INCONTACT/INRANGE/PRIMARY/NEW changes
        // would be noise - log every DOWN/UP and every 10th UPDATE.
        if (Msg != WM_POINTERUPDATE || (gPointerMsgs % 10) == 0) {
            Log("PTR   %s id %u type %u flags 0x%04x at %d,%d",
                Msg == WM_POINTERDOWN ? "DOWN  " : Msg == WM_POINTERUP ? "UP    " : "UPDATE",
                Id, (unsigned)Type, (unsigned)HIWORD(WParam), (int)(short)LOWORD(LParam), (int)(short)HIWORD(LParam));
        }
        break;   // fall through to DefWindowProc: it synthesizes the mouse messages incl. press-and-hold
    }
    case WM_LBUTTONDOWN: Log("MOUSE WM_LBUTTONDOWN"); break;
    case WM_LBUTTONUP:   Log("MOUSE WM_LBUTTONUP"); break;
    case WM_RBUTTONDOWN: Log("MOUSE WM_RBUTTONDOWN  <-- press-and-hold right-click"); break;
    case WM_RBUTTONUP:   gRightClicks++; Log("MOUSE WM_RBUTTONUP"); break;
    case WM_CONTEXTMENU: gContextMenus++; Log("MOUSE WM_CONTEXTMENU  <-- right-click delivered"); return 0;
    case WM_TIMER:
        if (Ms() >= (ULONGLONG)gSeconds * 1000) {
            DestroyWindow(Wnd);
            return 0;
        }
        swprintf_s(gStatus, ARRAYSIZE(gStatus),
                   L"Touch log: press and HOLD a finger still for 2-3 s, lift, repeat a few times.   %llu s left"
                   L"   HID %u  pointer %u  right-clicks %u",
                   (ULONGLONG)gSeconds - Ms() / 1000, gHidReports, gPointerMsgs, gRightClicks);
        InvalidateRect(Wnd, NULL, TRUE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT Ps;
        RECT        Rect;
        HDC         Dc = BeginPaint(Wnd, &Ps);

        GetClientRect(Wnd, &Rect);
        Rect.left += 40;
        Rect.top += 120;
        Rect.right -= 40;
        DrawTextW(Dc, gStatus, -1, &Rect, DT_WORDBREAK);
        EndPaint(Wnd, &Ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(Wnd, Msg, WParam, LParam);
}

int WINAPI
wWinMain(HINSTANCE Instance, HINSTANCE Prev, PWSTR Cmd, int Show)
{
    WNDCLASSW       Class = { 0 };
    RAWINPUTDEVICE  Rid;
    HWND            Wnd;
    MSG             Message;
    unsigned        I;

    (void)Prev;
    (void)Show;
    if (Cmd != NULL && *Cmd != 0) {
        gSeconds = (unsigned)_wtoi(Cmd);
        if (gSeconds == 0) {
            gSeconds = 40;
        }
    }
    CreateDirectoryW(L"C:\\S10eTmp", NULL);
    CreateDirectoryW(L"C:\\S10eTmp\\req", NULL);
    if (_wfopen_s(&gLog, L"C:\\S10eTmp\\req\\touchlog.txt", L"w") != 0 || gLog == NULL) {
        return 1;
    }
    setvbuf(gLog, NULL, _IONBF, 0);
    gStartMs = GetTickCount64();
    fprintf(gLog, "s10etouchlog: %u s\nSM_DIGITIZER 0x%x  SM_MAXIMUMTOUCHES %d  SM_TABLETPC %d\n", gSeconds,
            GetSystemMetrics(SM_DIGITIZER), GetSystemMetrics(SM_MAXIMUMTOUCHES), GetSystemMetrics(SM_TABLETPC));
    DumpWispTouch();

    Class.lpfnWndProc = WndProc;
    Class.hInstance = Instance;
    Class.hCursor = LoadCursor(NULL, IDC_ARROW);
    Class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    Class.lpszClassName = L"S10eTouchLog";
    RegisterClassW(&Class);
    Wnd = CreateWindowExW(WS_EX_TOPMOST, Class.lpszClassName, L"S10e touch log", WS_POPUP | WS_VISIBLE,
                          0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                          NULL, NULL, Instance, NULL);
    if (Wnd == NULL) {
        fprintf(gLog, "CreateWindow failed %lu\n", GetLastError());
        return 2;
    }
    Rid.usUsagePage = 0x0D;
    Rid.usUsage = 0x04;
    Rid.dwFlags = RIDEV_INPUTSINK;
    Rid.hwndTarget = Wnd;
    if (!RegisterRawInputDevices(&Rid, 1, sizeof(Rid))) {
        fprintf(gLog, "RegisterRawInputDevices failed %lu\n", GetLastError());
    }
    SetTimer(Wnd, 1, 500, NULL);
    while (GetMessageW(&Message, NULL, 0, 0) > 0) {
        TranslateMessage(&Message);
        DispatchMessageW(&Message);
    }
    for (I = 0; I < MAX_CONTACT; I++) {
        if (gContacts[I].Down) {
            fprintf(gLog, "contact %u still DOWN at exit (down at %llu ms, last report %llu ms)\n",
                    I, gContacts[I].DownMs, gContacts[I].LastMs);
        }
    }
    fprintf(gLog, "summary: HID reports %u, pointer messages %u, WM_RBUTTONUP %u, WM_CONTEXTMENU %u\n",
            gHidReports, gPointerMsgs, gRightClicks, gContextMenus);
    fclose(gLog);
    return 0;
}
