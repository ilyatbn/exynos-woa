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
 *   s10etouchlog [seconds]      default 300; output C:\S10eTmp\req\touchlog.txt (Users-writable)
 *
 * 2026-10-14 (user: "add an object to drag and a basic keyboard ... just the keys and an input box"): the window
 * also has a draggable square, an input box with a calculator-style keypad (keys only, nothing is calculated) and an
 * X to close. The app is per-monitor DPI aware (physical 1080 x 2280 pixels). It counts "suspicious double taps"
 * (two pointer DOWNs within 150 ms and 60 px - the touch bug's signature) and logs them as SUSPECT lines.
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
static unsigned   gSeconds = 300;
static unsigned   gHidReports, gPointerMsgs, gRightClicks, gContextMenus;
static CONTACT    gContacts[MAX_CONTACT];
static WCHAR      gStatus[256] = L"";

static ULONGLONG Ms(void);
static void Log(const char *Fmt, ...);

/* ---- test UI (physical pixels) ---- */
#define KEY_COLS 4
#define KEY_ROWS 5
static const WCHAR *gKeys[KEY_ROWS * KEY_COLS] = {
    L"7", L"8", L"9", L"/",
    L"4", L"5", L"6", L"*",
    L"1", L"2", L"3", L"-",
    L"0", L".", L"=", L"+",
    L"C", L"(", L")", L"\x232B",
};
static RECT       gSquare = { 400, 520, 640, 760 };
static BOOL       gDragging;
static UINT32     gDragPointer;
static POINT      gDragOffset;
static unsigned   gDrags, gDragMoves;
static int        gPressedKey = -1;
static UINT32     gKeyPointer;
static WCHAR      gInput[64] = L"";
static unsigned   gKeyTaps, gSuspectTaps;
static ULONGLONG  gLastDownMs;
static POINT      gLastDownPt;
static HFONT      gFontBig, gFontSmall;

static RECT
ClientRectOf(HWND Wnd)
{
    RECT R;
    GetClientRect(Wnd, &R);
    return R;
}

static RECT
DragArea(RECT C)
{
    RECT R = { 40, 300, C.right - 40, 1080 };
    return R;
}

static RECT
InputBox(RECT C)
{
    RECT R = { 40, 1120, C.right - 40, 1260 };
    return R;
}

static RECT
KeyRect(RECT C, int Index)
{
    int  top = 1300, bottom = C.bottom - 40, left = 40, right = C.right - 40;
    int  w = (right - left) / KEY_COLS, h = (bottom - top) / KEY_ROWS;
    RECT R;
    R.left = left + (Index % KEY_COLS) * w + 8;
    R.top = top + (Index / KEY_COLS) * h + 8;
    R.right = R.left + w - 16;
    R.bottom = R.top + h - 16;
    return R;
}

static RECT
CloseRect(RECT C)
{
    RECT R = { C.right - 180, 20, C.right - 20, 180 };
    return R;
}

static int
KeyAt(RECT C, POINT P)
{
    int I;
    for (I = 0; I < KEY_ROWS * KEY_COLS; I++) {
        RECT R = KeyRect(C, I);
        if (PtInRect(&R, P))
            return I;
    }
    return -1;
}

static void
PressKey(int Index)
{
    size_t n = wcslen(gInput);
    const WCHAR *k = gKeys[Index];

    gKeyTaps++;
    if (wcscmp(k, L"C") == 0) {
        gInput[0] = 0;
    } else if (k[0] == 0x232B) {
        if (n > 0)
            gInput[n - 1] = 0;
    } else if (n + 1 < ARRAYSIZE(gInput)) {
        gInput[n] = k[0];
        gInput[n + 1] = 0;
    }
    Log("KEY   '%lc' -> input \"%ls\"", k[0], gInput);
}

static void
DrawUi(HWND Wnd, HDC Dc)
{
    RECT C = ClientRectOf(Wnd), R;
    HBRUSH bg = CreateSolidBrush(RGB(245, 245, 245));
    HBRUSH area = CreateSolidBrush(RGB(225, 232, 240));
    HBRUSH sq = CreateSolidBrush(gDragging ? RGB(230, 80, 40) : RGB(40, 110, 220));
    HBRUSH key = CreateSolidBrush(RGB(255, 255, 255));
    HBRUSH keyDown = CreateSolidBrush(RGB(120, 170, 250));
    HBRUSH box = CreateSolidBrush(RGB(255, 255, 255));
    HBRUSH close = CreateSolidBrush(RGB(200, 50, 50));
    WCHAR  text[128];
    int    I;

    FillRect(Dc, &C, bg);
    SetBkMode(Dc, TRANSPARENT);
    SelectObject(Dc, gFontSmall);
    R = C;
    R.left = 40;
    R.top = 40;
    R.right -= 220;
    DrawTextW(Dc, gStatus, -1, &R, DT_WORDBREAK);

    R = CloseRect(C);
    FillRect(Dc, &R, close);
    SelectObject(Dc, gFontBig);
    SetTextColor(Dc, RGB(255, 255, 255));
    DrawTextW(Dc, L"X", -1, &R, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(Dc, RGB(0, 0, 0));

    R = DragArea(C);
    FillRect(Dc, &R, area);
    SelectObject(Dc, gFontSmall);
    swprintf_s(text, ARRAYSIZE(text), L"drag the square   (drags %u, moves %u)", gDrags, gDragMoves);
    R.left += 20;
    R.top += 16;
    DrawTextW(Dc, text, -1, &R, DT_SINGLELINE);
    FillRect(Dc, &gSquare, sq);

    R = InputBox(C);
    FillRect(Dc, &R, box);
    FrameRect(Dc, &R, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SelectObject(Dc, gFontBig);
    R.left += 24;
    DrawTextW(Dc, gInput[0] ? gInput : L" ", -1, &R, DT_VCENTER | DT_SINGLELINE);

    for (I = 0; I < KEY_ROWS * KEY_COLS; I++) {
        R = KeyRect(C, I);
        FillRect(Dc, &R, I == gPressedKey ? keyDown : key);
        FrameRect(Dc, &R, (HBRUSH)GetStockObject(GRAY_BRUSH));
        DrawTextW(Dc, gKeys[I], -1, &R, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    DeleteObject(bg);
    DeleteObject(area);
    DeleteObject(sq);
    DeleteObject(key);
    DeleteObject(keyDown);
    DeleteObject(box);
    DeleteObject(close);
}

/* Returns TRUE if the pointer message was consumed by the test UI. */
static BOOL
OnUiPointer(HWND Wnd, UINT Msg, UINT32 Id, POINT P)
{
    RECT C = ClientRectOf(Wnd);
    RECT R;

    if (Msg == WM_POINTERDOWN) {
        ULONGLONG now = Ms();
        LONG dx = P.x - gLastDownPt.x, dy = P.y - gLastDownPt.y;
        if (gLastDownMs != 0 && now - gLastDownMs < 150 && dx * dx + dy * dy < 60 * 60) {
            gSuspectTaps++;
            Log("SUSPECT second DOWN %llu ms after the previous one, %ld px away", now - gLastDownMs,
                (long)(dx < 0 ? -dx : dx) + (long)(dy < 0 ? -dy : dy));
        }
        gLastDownMs = now;
        gLastDownPt = P;
        R = CloseRect(C);
        if (PtInRect(&R, P)) {
            Log("CLOSE tapped");
            DestroyWindow(Wnd);
            return TRUE;
        }
        if (!gDragging && PtInRect(&gSquare, P)) {
            gDragging = TRUE;
            gDragPointer = Id;
            gDragOffset.x = P.x - gSquare.left;
            gDragOffset.y = P.y - gSquare.top;
            gDrags++;
            InvalidateRect(Wnd, NULL, FALSE);
            return TRUE;
        }
        if (gPressedKey < 0) {
            int k = KeyAt(C, P);
            if (k >= 0) {
                gPressedKey = k;
                gKeyPointer = Id;
                InvalidateRect(Wnd, NULL, FALSE);
                return TRUE;
            }
        }
        return FALSE;
    }
    if (Msg == WM_POINTERUPDATE && gDragging && Id == gDragPointer) {
        RECT a = DragArea(C);
        LONG w = gSquare.right - gSquare.left, h = gSquare.bottom - gSquare.top;
        LONG x = P.x - gDragOffset.x, y = P.y - gDragOffset.y;
        if (x < a.left) x = a.left;
        if (y < a.top + 60) y = a.top + 60;
        if (x + w > a.right) x = a.right - w;
        if (y + h > a.bottom) y = a.bottom - h;
        SetRect(&gSquare, x, y, x + w, y + h);
        gDragMoves++;
        InvalidateRect(Wnd, NULL, FALSE);
        return TRUE;
    }
    if (Msg == WM_POINTERUP) {
        if (gDragging && Id == gDragPointer) {
            gDragging = FALSE;
            InvalidateRect(Wnd, NULL, FALSE);
            return TRUE;
        }
        if (gPressedKey >= 0 && Id == gKeyPointer) {
            if (KeyAt(C, P) == gPressedKey)
                PressKey(gPressedKey);
            gPressedKey = -1;
            InvalidateRect(Wnd, NULL, FALSE);
            return TRUE;
        }
    }
    return FALSE;
}

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
        {
            POINT P = { (short)LOWORD(LParam), (short)HIWORD(LParam) };
            ScreenToClient(Wnd, &P);
            if (OnUiPointer(Wnd, Msg, Id, P))
                return 0;   // a UI element took it (no synthesized mouse click)
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
                   L"Touch test - %llu s left. Tap keys, drag the square, hold for right-click.\n"
                   L"key taps %u   suspicious double taps %u   right-clicks %u   HID %u",
                   (ULONGLONG)gSeconds - Ms() / 1000, gKeyTaps, gSuspectTaps, gRightClicks, gHidReports);
        InvalidateRect(Wnd, NULL, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;   /* the whole client area is painted through a back buffer */
    case WM_PAINT: {
        PAINTSTRUCT Ps;
        RECT        Rect;
        HDC         Dc = BeginPaint(Wnd, &Ps);
        HDC         Mem;
        HBITMAP     Bmp, Old;

        GetClientRect(Wnd, &Rect);
        Mem = CreateCompatibleDC(Dc);
        Bmp = CreateCompatibleBitmap(Dc, Rect.right, Rect.bottom);
        Old = (HBITMAP)SelectObject(Mem, Bmp);
        DrawUi(Wnd, Mem);
        BitBlt(Dc, 0, 0, Rect.right, Rect.bottom, Mem, 0, 0, SRCCOPY);
        SelectObject(Mem, Old);
        DeleteObject(Bmp);
        DeleteDC(Mem);
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
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    gFontBig = CreateFontW(-72, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    gFontSmall = CreateFontW(-40, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    if (Cmd != NULL && *Cmd != 0) {
        gSeconds = (unsigned)_wtoi(Cmd);
        if (gSeconds == 0) {
            gSeconds = 300;
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
    fprintf(gLog, "summary: HID reports %u, pointer messages %u, WM_RBUTTONUP %u, WM_CONTEXTMENU %u, key taps %u, "
            "suspicious double taps %u, drags %u (moves %u), input \"%ls\"\n",
            gHidReports, gPointerMsgs, gRightClicks, gContextMenus, gKeyTaps, gSuspectTaps, gDrags, gDragMoves, gInput);
    fclose(gLog);
    return 0;
}
