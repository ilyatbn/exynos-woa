/*
 * kmdstats.c - MaliG76Wddm's counters (MW_ESCAPE_STATS) sampled twice, N seconds apart, as rates: presents, CPU-copy
 * presents (PresentBlts: the scan-out latch copying a primary into the framebuffer at DPC level), GPU-presented,
 * skipped, renders, submits, GPU interrupts. G17 pacing (notes/g17-dxva-mfc.md): where DWM's frames go.
 *
 *   kmdstats [seconds]
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <stdio.h>
#include <stdlib.h>
#include "maliwddm_abi.h"

#pragma comment(lib, "gdi32.lib")

static NTSTATUS KmtEscape(D3DKMT_HANDLE adapter, void *data, UINT size)
{
    D3DKMT_ESCAPE e = { 0 };
    e.hAdapter = adapter;
    e.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    e.pPrivateDriverData = data;
    e.PrivateDriverDataSize = size;
    return D3DKMTEscape(&e);
}

/* the adapter that answers the GPU-info escape with a G76 GPU_ID (as m5test) */
static D3DKMT_HANDLE FindAdapter(void)
{
    D3DKMT_ENUMADAPTERS2 en = { 0 };
    D3DKMT_ADAPTERINFO info[16];
    en.NumAdapters = 16;
    en.pAdapters = info;
    if (D3DKMTEnumAdapters2(&en) < 0)
        return 0;
    for (ULONG i = 0; i < en.NumAdapters; i++) {
        MW_ESCAPE_GPU_INFO_DATA g = { 0 };
        g.Code = MW_ESCAPE_GPU_INFO;
        if (KmtEscape(info[i].hAdapter, &g, sizeof(g)) >= 0 && g.Count && g.Valid && (g.Value[0] & 0xFF0F) == 0x7201)
            return info[i].hAdapter;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 5;
    D3DKMT_HANDLE a = FindAdapter();
    MW_ESCAPE_STATS_DATA s0 = { 0 }, s1 = { 0 };
    if (!a) {
        printf("no MaliG76Wddm adapter\n");
        return 1;
    }
    s0.Code = s1.Code = MW_ESCAPE_STATS;
    if (KmtEscape(a, &s0, sizeof(s0)) < 0)
        return 1;
    Sleep(secs * 1000);
    if (KmtEscape(a, &s1, sizeof(s1)) < 0)
        return 1;
    double s = secs;
#define R(f) ((double)(s1.f - s0.f) / s)
    printf("over %d s per second: presents %.1f (cpu copy %.1f, gpu %.1f, fills %.1f, skipped %.1f), renders %.1f, "
           "submits %.1f, irqs %.1f, jobs %.1f\n", secs, R(Presents), R(PresentBlts), R(PresentGpu), R(PresentFills),
           R(PresentSkips), R(Renders), R(Submits), R(Irqs), R(JobsDone));
    printf("fence %u/%u queued %u running %u\n", s1.LastCompletedFence, s1.LastSubmittedFence, s1.Queued, s1.Running);
    return 0;
}
