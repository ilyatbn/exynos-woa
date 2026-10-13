// SPDX-License-Identifier: GPL-2.0
/*
 * gfxperf.exe - what the desktop costs, once a second (M16 G13, notes/gpu-full-adapter-hang.md). The user's full-mode
 * test (2026-10-13): desktop composed on the Mali adapter but "barely working", right-click > 1 min. In full mode
 * (FullAdapter 1, GpuEnable 0) every D3D draw - DWM's composition included - runs in Mesa softpipe on the CPU inside
 * maliumd.dll; this tool measures how fast that is, side by side with display-only mode.
 *
 *   gfxperf <seconds> [interval-ms]       (default 120 s, 1000 ms), one line per sample on stdout
 *
 * Per sample:
 *   dwm  - composition rate from DwmGetCompositionTimingInfo(NULL): frames composed / s (cFrame delta), refresh
 *          rate, frames dropped / missed (deltas). Needs the logged-on user's session (DWM's); in session 0 it
 *          prints the HRESULT instead.
 *   cpu  - total and per logical CPU "% Processor Utility" and "Processor Frequency" (MHz) from the
 *          "Processor Information" counter set (PDH, English counter names).
 *   gpu  - "GPU Engine" "Utilization Percentage" summed per adapter LUID + engine type (instance names carry
 *          luid_0x..._phys_0_eng_N_engtype_X).
 *   top  - the 4 processes with the most "% Processor Time" (100 = one CPU).
 * Exit 0 always. Build: build-gfxperf.cmd.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct {
    PDH_HCOUNTER H;
    const wchar_t *Path;
} COUNTER;

static PDH_HQUERY Q;
static COUNTER CpuUtil = { NULL, L"\\Processor Information(*)\\% Processor Utility" };
static COUNTER CpuFreq = { NULL, L"\\Processor Information(*)\\Processor Frequency" };
static COUNTER GpuUtil = { NULL, L"\\GPU Engine(*)\\Utilization Percentage" };
static COUNTER ProcCpu = { NULL, L"\\Process(*)\\% Processor Time" };

static void Add(COUNTER *C)
{
    PDH_STATUS s = PdhAddEnglishCounterW(Q, C->Path, 0, &C->H);
    if (s != ERROR_SUCCESS) {
        printf("pdh: add %ls failed 0x%08lx\n", C->Path, (unsigned long)s);
        C->H = NULL;
    }
}

/* The counter's instances as a formatted (double) array; caller frees. NULL when unavailable. */
static PDH_FMT_COUNTERVALUE_ITEM_W *Items(COUNTER *C, DWORD *N)
{
    DWORD bytes = 0;
    PDH_FMT_COUNTERVALUE_ITEM_W *a;

    *N = 0;
    if (C->H == NULL)
        return NULL;
    if (PdhGetFormattedCounterArrayW(C->H, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, N, NULL) != PDH_MORE_DATA)
        return NULL;
    a = (PDH_FMT_COUNTERVALUE_ITEM_W *)malloc(bytes);
    if (a == NULL)
        return NULL;
    if (PdhGetFormattedCounterArrayW(C->H, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, N, a) != ERROR_SUCCESS) {
        free(a);
        *N = 0;
        return NULL;
    }
    return a;
}

/* "0,3" -> 3 for Processor Information instances; -1 for _Total / "0,_Total". */
static int CpuIndex(const wchar_t *Name)
{
    const wchar_t *c = wcschr(Name, L',');
    if (c == NULL || c[1] == L'_')
        return -1;
    return _wtoi(c + 1);
}

static void PrintCpu(void)
{
    DWORD nu = 0, nf = 0;
    PDH_FMT_COUNTERVALUE_ITEM_W *u = Items(&CpuUtil, &nu), *f = Items(&CpuFreq, &nf);
    double util[64] = { 0 }, freq[64] = { 0 }, total = -1;
    int max = -1;

    for (DWORD i = 0; u != NULL && i < nu; i++) {
        int k = CpuIndex(u[i].szName);
        if (wcscmp(u[i].szName, L"_Total") == 0)
            total = u[i].FmtValue.doubleValue;
        else if (k >= 0 && k < 64) {
            util[k] = u[i].FmtValue.doubleValue;
            if (k > max)
                max = k;
        }
    }
    for (DWORD i = 0; f != NULL && i < nf; i++) {
        int k = CpuIndex(f[i].szName);
        if (k >= 0 && k < 64) {
            freq[k] = f[i].FmtValue.doubleValue;
            if (k > max)
                max = k;
        }
    }
    printf(" cpu %5.1f%% [", total);
    for (int k = 0; k <= max; k++)
        printf("%s%.0f%%@%.0f", k ? " " : "", util[k], freq[k]);
    printf("]");
    free(u);
    free(f);
}

/* Sum per "luid_0x..._0x..._..._engtype_X": print each adapter+engine type with nonzero use. */
static void PrintGpu(void)
{
    DWORD n = 0;
    PDH_FMT_COUNTERVALUE_ITEM_W *g = Items(&GpuUtil, &n);
    struct { wchar_t Key[96]; double V; } sum[32];
    int ns = 0;

    printf(" gpu");
    if (g == NULL) {
        printf(" n/a");
        return;
    }
    for (DWORD i = 0; i < n; i++) {
        const wchar_t *l = wcsstr(g[i].szName, L"luid_"), *t = wcsstr(g[i].szName, L"engtype_");
        wchar_t key[96];
        int j;
        if (l == NULL || t == NULL)
            continue;
        /* "luid_0xHHHHHHHH_0xLLLLLLLL_..." -> the low part (16 characters in) plus the engine type */
        swprintf(key, 96, L"%.10ls:%ls", l + 16 < t ? l + 16 : l, t + 8);
        for (j = 0; j < ns; j++)
            if (wcscmp(sum[j].Key, key) == 0)
                break;
        if (j == ns) {
            if (ns == 32)
                continue;
            wcscpy_s(sum[ns].Key, 96, key);
            sum[ns].V = 0;
            ns++;
        }
        sum[j].V += g[i].FmtValue.doubleValue;
    }
    for (int j = 0; j < ns; j++)
        if (sum[j].V >= 0.5)
            printf(" %ls=%.0f%%", sum[j].Key, sum[j].V);
    free(g);
}

static void PrintTop(void)
{
    DWORD n = 0;
    PDH_FMT_COUNTERVALUE_ITEM_W *p = Items(&ProcCpu, &n);
    int best[4] = { -1, -1, -1, -1 };

    printf(" top");
    if (p == NULL) {
        printf(" n/a");
        return;
    }
    for (DWORD i = 0; i < n; i++) {
        if (wcscmp(p[i].szName, L"_Total") == 0 || wcscmp(p[i].szName, L"Idle") == 0)
            continue;
        for (int b = 0; b < 4; b++) {
            if (best[b] < 0 || p[i].FmtValue.doubleValue > p[best[b]].FmtValue.doubleValue) {
                for (int m = 3; m > b; m--)
                    best[m] = best[m - 1];
                best[b] = (int)i;
                break;
            }
        }
    }
    for (int b = 0; b < 4; b++)
        if (best[b] >= 0)
            printf(" %ls=%.0f", p[best[b]].szName, p[best[b]].FmtValue.doubleValue);
    free(p);
}

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 120;
    int interval = argc > 2 ? atoi(argv[2]) : 1000;
    DWM_TIMING_INFO prev = { 0 }, cur;
    BOOL havePrev = FALSE;
    LARGE_INTEGER f, t0, t;
    double tPrev = 0;

    if (seconds <= 0)
        seconds = 120;
    if (interval < 200)
        interval = 200;
    setvbuf(stdout, NULL, _IONBF, 0);
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    if (PdhOpenQueryW(NULL, 0, &Q) != ERROR_SUCCESS) {
        printf("pdh: open failed\n");
        Q = NULL;
    } else {
        Add(&CpuUtil);
        Add(&CpuFreq);
        Add(&GpuUtil);
        Add(&ProcCpu);
        PdhCollectQueryData(Q);   /* rate counters need two samples */
    }
    printf("gfxperf: %d s every %d ms, session %lu\n", seconds, interval, (unsigned long)WTSGetActiveConsoleSessionId());
    for (;;) {
        double now;
        HRESULT hr;

        Sleep(interval);
        QueryPerformanceCounter(&t);
        now = (double)(t.QuadPart - t0.QuadPart) / (double)f.QuadPart;
        if (Q != NULL)
            PdhCollectQueryData(Q);
        ZeroMemory(&cur, sizeof(cur));
        cur.cbSize = sizeof(cur);
        hr = DwmGetCompositionTimingInfo(NULL, &cur);
        printf("t %6.1f", now);
        if (FAILED(hr)) {
            printf(" dwm hr 0x%08lx", (unsigned long)hr);
            havePrev = FALSE;
        } else if (havePrev) {
            double dt = now - tPrev;
            printf(" dwm %5.1f fps (refresh %.1f Hz) dropped %llu missed %llu",
                   (double)(cur.cFrame - prev.cFrame) / dt,
                   cur.rateRefresh.uiDenominator ? (double)cur.rateRefresh.uiNumerator / cur.rateRefresh.uiDenominator : 0.0,
                   (unsigned long long)(cur.cFramesDropped - prev.cFramesDropped),
                   (unsigned long long)(cur.cFramesMissed - prev.cFramesMissed));
        } else {
            printf(" dwm first sample");
        }
        if (SUCCEEDED(hr)) {
            prev = cur;
            havePrev = TRUE;
        }
        tPrev = now;
        if (Q != NULL) {
            PrintCpu();
            PrintGpu();
            PrintTop();
        }
        printf("\n");
        if (now >= seconds)
            break;
    }
    if (Q != NULL)
        PdhCloseQuery(Q);
    printf("gfxperf: done\n");
    return 0;
}
