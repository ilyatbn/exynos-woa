/*
 * dwmrate.c - how fast DWM composes (DwmGetCompositionTimingInfo, MS Learn: DWM_TIMING_INFO): composed frames, missed
 * frames and refresh counts over an interval, G17 V6d frame pacing (notes/g17-dxva-mfc.md). Run in the user session.
 *
 *   dwmrate [seconds]
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "dwmapi.lib")

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 3;
    DWM_TIMING_INFO a = { sizeof(a) }, b = { sizeof(b) };
    HRESULT hr = DwmGetCompositionTimingInfo(NULL, &a);
    if (FAILED(hr)) {
        printf("DwmGetCompositionTimingInfo 0x%08lx\n", hr);
        return 1;
    }
    Sleep(secs * 1000);
    DwmGetCompositionTimingInfo(NULL, &b);
    double s = secs;
    printf("refresh rate %u/%u, compose rate %u/%u\n", a.rateRefresh.uiNumerator, a.rateRefresh.uiDenominator,
           a.rateCompose.uiNumerator, a.rateCompose.uiDenominator);
    printf("over %d s: refreshes %.1f/s, DWM frames %.1f/s, frames displayed %.1f/s\n", secs,
           (double)(b.cRefresh - a.cRefresh) / s, (double)(b.cFrame - a.cFrame) / s,
           (double)(b.cFramesDisplayed - a.cFramesDisplayed) / s);
    printf("dropped %llu, missed %llu, late %llu (DWM_TIMING_INFO counters over the interval)\n",
           (unsigned long long)(b.cFramesDropped - a.cFramesDropped),
           (unsigned long long)(b.cFramesMissed - a.cFramesMissed),
           (unsigned long long)(b.cFramesLate - a.cFramesLate));
    return 0;
}
