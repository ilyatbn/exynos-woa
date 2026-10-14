/*
 * s10etap.exe - synthetic touch for remote testing (2026-10-14, user request: "simulate touch by yourself, use a
 * screenshot to see what to touch then try it"). Uses the documented pointer injection API
 * (CreateSyntheticPointerDevice + InjectSyntheticPointerInput, Windows 10 1809+): the input arrives as PT_TOUCH, the
 * same pointer type a finger on the S6SY761 produces, so apps, gestures and the touch keyboard treat it as touch.
 * It does not go through our touch driver (that would need a virtual HID digitizer).
 *
 * Must run in the logged-on user's session (tools/phone/runuser2.ps1). Coordinates are physical panel pixels
 * (1080 x 2280 portrait); the process is per-monitor DPI aware so no scaling applies.
 *
 *   s10etap tap <x> <y> [hold_ms=60]
 *   s10etap hold <x> <y> [ms=1200]              press and hold (right-click on touch)
 *   s10etap drag <x1> <y1> <x2> <y2> [ms=400]   one finger, moves every ~8 ms
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static HSYNTHETICPOINTERDEVICE gDev;

static BOOL Frame(LONG X, LONG Y, POINTER_FLAGS Flags)
{
    POINTER_TYPE_INFO p;

    ZeroMemory(&p, sizeof(p));
    p.type = PT_TOUCH;
    p.touchInfo.pointerInfo.pointerType = PT_TOUCH;
    p.touchInfo.pointerInfo.pointerId = 0;
    p.touchInfo.pointerInfo.pointerFlags = Flags;
    p.touchInfo.pointerInfo.ptPixelLocation.x = X;
    p.touchInfo.pointerInfo.ptPixelLocation.y = Y;
    p.touchInfo.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
    p.touchInfo.rcContact.left = X - 4;
    p.touchInfo.rcContact.right = X + 4;
    p.touchInfo.rcContact.top = Y - 4;
    p.touchInfo.rcContact.bottom = Y + 4;
    p.touchInfo.orientation = 90;
    p.touchInfo.pressure = 32000;
    if (!InjectSyntheticPointerInput(gDev, &p, 1)) {
        fprintf(stderr, "inject (%ld,%ld) flags 0x%x failed: %lu\n", X, Y, (unsigned)Flags, GetLastError());
        return FALSE;
    }
    return TRUE;
}

static BOOL Down(LONG X, LONG Y)
{
    return Frame(X, Y, POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT);
}

static BOOL Move(LONG X, LONG Y)
{
    return Frame(X, Y, POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT);
}

static BOOL Up(LONG X, LONG Y)
{
    return Frame(X, Y, POINTER_FLAG_UP);
}

/* Keep the contact alive while held: Windows expects updates at the digitizer rate. */
static BOOL HoldFor(LONG X, LONG Y, DWORD Ms)
{
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < Ms) {
        Sleep(8);
        if (!Move(X, Y))
            return FALSE;
    }
    return TRUE;
}

int main(int argc, char **argv)
{
    BOOL ok = FALSE;
    LONG x1, y1, x2, y2;
    DWORD ms;

    if (argc < 4) {
        fprintf(stderr, "usage: s10etap tap <x> <y> [hold_ms] | hold <x> <y> [ms] | drag <x1> <y1> <x2> <y2> [ms]\n");
        return 1;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    gDev = CreateSyntheticPointerDevice(PT_TOUCH, 1, POINTER_FEEDBACK_DEFAULT);
    if (gDev == NULL) {
        fprintf(stderr, "CreateSyntheticPointerDevice failed: %lu\n", GetLastError());
        return 2;
    }
    x1 = strtol(argv[2], NULL, 0);
    y1 = strtol(argv[3], NULL, 0);
    if (strcmp(argv[1], "tap") == 0 || strcmp(argv[1], "hold") == 0) {
        ms = argc > 4 ? strtoul(argv[4], NULL, 0) : (argv[1][0] == 't' ? 60 : 1200);
        ok = Down(x1, y1) && HoldFor(x1, y1, ms) && Up(x1, y1);
        printf("%s (%ld,%ld) %lu ms: %s\n", argv[1], x1, y1, ms, ok ? "ok" : "FAILED");
    } else if (strcmp(argv[1], "drag") == 0 && argc >= 6) {
        DWORD t0, t;
        x2 = strtol(argv[4], NULL, 0);
        y2 = strtol(argv[5], NULL, 0);
        ms = argc > 6 ? strtoul(argv[6], NULL, 0) : 400;
        if (ms == 0)
            ms = 1;
        ok = Down(x1, y1) && HoldFor(x1, y1, 80);   /* settle like a finger before moving */
        t0 = GetTickCount();
        while (ok && (t = GetTickCount() - t0) < ms) {
            ok = Move(x1 + (LONG)(((LONGLONG)(x2 - x1) * t) / ms), y1 + (LONG)(((LONGLONG)(y2 - y1) * t) / ms));
            Sleep(8);
        }
        ok = ok && Move(x2, y2) && HoldFor(x2, y2, 50) && Up(x2, y2);
        printf("drag (%ld,%ld)->(%ld,%ld) %lu ms: %s\n", x1, y1, x2, y2, ms, ok ? "ok" : "FAILED");
    } else {
        fprintf(stderr, "unknown command %s\n", argv[1]);
    }
    DestroySyntheticPointerDevice(gDev);
    return ok ? 0 : 3;
}
