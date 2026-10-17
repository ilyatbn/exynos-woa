/*
 * clocks - a small window with the S10e's real per-core CPU clocks and the GPU clock (user 2026-10-17: "make
 * clocks.exe a gui app with Core #0-5 - x / y, keep it simple"):
 *     Core #0 - 1456 / 1950 MHz      (measured now / table maximum)
 *     ...
 *     GPU - 260 / 702 MHz
 * Each core is measured by a ~2 ms burst of dependent single-cycle integer ops on a thread pinned to it (ops/ns ~=
 * GHz); the GPU by MaliG76Wddm's MW_ESCAPE_CLOCK (cycle counter over QPC, no admin rights needed). Windows' Task
 * Manager cannot show these: it only knows firmware boot values. Refresh: 1 s. Console variant: clockscli.c.
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <d3dkmthk.h>
#include "maliwddm_abi.h"

static unsigned long long volatile gSink;
static volatile LONG gMhz[8], gGpuMhz = -1;
static DWORD gCpus;
static HWND gWnd;

static double Now(void)
{
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static double AddChain(unsigned long long it)
{
    unsigned long long a = 1, b = 3, i;
    double t0 = Now(), t1;
    for (i = 0; i < it; i++) {
        a ^= b; a += i; a ^= b; a += i; a ^= b; a += i; a ^= b; a += i;
    }
    t1 = Now();
    gSink = a;
    return (double)(it * 8ull) / ((t1 - t0) * 1e9);
}

static BOOL GpuRead(D3DKMT_HANDLE a, MW_ESCAPE_CLOCK_DATA *c)
{
    D3DKMT_ESCAPE e = { 0 };
    ZeroMemory(c, sizeof(*c));
    c->Code = MW_ESCAPE_CLOCK;
    e.hAdapter = a;
    e.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    e.pPrivateDriverData = c;
    e.PrivateDriverDataSize = sizeof(*c);
    return D3DKMTEscape(&e) >= 0 && c->Valid;
}

static D3DKMT_HANDLE FindMali(void)
{
    D3DKMT_ADAPTERINFO info[16];
    D3DKMT_ENUMADAPTERS2 en = { 0 };
    MW_ESCAPE_CLOCK_DATA c;
    en.NumAdapters = 16;
    en.pAdapters = info;
    if (D3DKMTEnumAdapters2(&en) < 0)
        return 0;
    for (ULONG i = 0; i < en.NumAdapters; i++)
        if (GpuRead(info[i].hAdapter, &c))
            return info[i].hAdapter;
    return 0;
}

static DWORD WINAPI Measure(LPVOID p)
{
    D3DKMT_HANDLE mali = FindMali();
    (void)p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    for (;;) {
        MW_ESCAPE_CLOCK_DATA g0 = { 0 }, g1 = { 0 };
        BOOL g = mali && GpuRead(mali, &g0);
        for (DWORD c = 0; c < gCpus; c++) {
            SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << c);
            Sleep(0);
            AddChain(50000);
            InterlockedExchange(&gMhz[c], (LONG)(AddChain(500000) * 1000.0 + 0.5));
        }
        SetThreadAffinityMask(GetCurrentThread(), ~(DWORD_PTR)0);
        Sleep(1000);
        if (g && GpuRead(mali, &g1) && g1.Qpc > g0.Qpc && g1.Cycles >= g0.Cycles)
            InterlockedExchange(&gGpuMhz, (LONG)((double)(g1.Cycles - g0.Cycles) * (double)g1.QpcFreq /
                                                 (double)(g1.Qpc - g0.Qpc) / 1e6 + 0.5));
        InvalidateRect(gWnd, NULL, TRUE);
    }
}

static LRESULT CALLBACK WndProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        UINT dpi = GetDpiForWindow(w);
        HFONT f = CreateFontW(-MulDiv(16, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                              CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        HGDIOBJ old = SelectObject(dc, f);
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        int y = tm.tmHeight / 2, x = tm.tmAveCharWidth * 2;
        wchar_t line[64];
        for (DWORD c = 0; c < gCpus; c++) {
            unsigned max = c >= 4 ? 2400 : 1950;   /* A75 (fast) table top / A55 (small) table top */
            int n = swprintf(line, 64, L"Core #%lu - %4ld / %u MHz", c, gMhz[c], max);
            TextOutW(dc, x, y, line, n);
            y += tm.tmHeight;
        }
        int n = gGpuMhz >= 0 ? swprintf(line, 64, L"GPU     - %4ld / 702 MHz", gGpuMhz)
                             : swprintf(line, 64, L"GPU     -    ? / 702 MHz");
        TextOutW(dc, x, y, line, n);
        SelectObject(dc, old);
        DeleteObject(f);
        EndPaint(w, &ps);
        return 0;
    }
    if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev; (void)cmd;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    gCpus = si.dwNumberOfProcessors > 8 ? 8 : si.dwNumberOfProcessors;
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"S10eClocks";
    RegisterClassW(&wc);
    UINT dpi = GetDpiForSystem();
    gWnd = CreateWindowExW(WS_EX_TOPMOST, L"S10eClocks", L"S10e clocks", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                           WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(330, dpi, 96),
                           MulDiv(40 + 26 * (int)(gCpus + 1), dpi, 96), NULL, NULL, hi, NULL);
    ShowWindow(gWnd, show);
    CreateThread(NULL, 0, Measure, NULL, 0, NULL);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
