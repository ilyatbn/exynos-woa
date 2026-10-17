/*
 * vblank.c - measure the display's vertical blank interval as user mode sees it (D3DKMTWaitForVerticalBlankEvent on
 * the primary display's adapter / VidPN source), G17 V6d frame pacing (notes/g17-dxva-mfc.md). Prints the mean,
 * min and max interval over N waits and the implied rate.
 *
 *   vblank [N] [rt]   (rt: the waiting thread at TIME_CRITICAL priority - separates wake-up latency from the
 *                     vsync source)
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int Cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 120;
    double *iv = (double *)calloc(n > 0 ? n : 1, sizeof(double));
    int late = 0;
    if (argc > 2 && !strcmp(argv[2], "rt"))
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    D3DKMT_OPENADAPTERFROMHDC oa = { 0 };
    D3DKMT_WAITFORVERTICALBLANKEVENT wv = { 0 };
    LARGE_INTEGER f, prev, now;
    double sum = 0, mn = 1e9, mx = 0;
    NTSTATUS st;

    oa.hDc = CreateDCW(L"DISPLAY", NULL, NULL, NULL);
    st = D3DKMTOpenAdapterFromHdc(&oa);
    if (st != 0) {
        printf("D3DKMTOpenAdapterFromHdc 0x%08lx\n", (unsigned long)st);
        return 1;
    }
    wv.hAdapter = oa.hAdapter;
    wv.VidPnSourceId = oa.VidPnSourceId;
    QueryPerformanceFrequency(&f);
    D3DKMTWaitForVerticalBlankEvent(&wv);   /* align to a vblank first */
    QueryPerformanceCounter(&prev);
    for (int i = 0; i < n; i++) {
        st = D3DKMTWaitForVerticalBlankEvent(&wv);
        if (st != 0) {
            printf("wait %d: 0x%08lx\n", i, (unsigned long)st);
            return 1;
        }
        QueryPerformanceCounter(&now);
        double ms = (double)(now.QuadPart - prev.QuadPart) * 1000.0 / f.QuadPart;
        prev = now;
        sum += ms;
        iv[i] = ms;
        late += ms > 25.0;   /* a vblank missed (more than 1.5 frames) */
        if (ms < mn) mn = ms;
        if (ms > mx) mx = ms;
    }
    printf("vblank: %d intervals, mean %.2f ms (%.1f Hz), min %.2f, max %.2f ms\n", n, sum / n, 1000.0 * n / sum, mn,
           mx);
    qsort(iv, n, sizeof(double), Cmp);
    printf("percentiles: p10 %.2f p50 %.2f p90 %.2f ms; intervals > 25 ms: %d of %d\n", iv[n / 10], iv[n / 2],
           iv[n * 9 / 10], late, n);
    return 0;
}
