/*
 * kmdstats.c - MaliG76Wddm's counters (MW_ESCAPE_STATS) sampled twice, N seconds apart, as rates: presents, CPU-copy
 * presents (PresentBlts: the scan-out latch copying a primary into the framebuffer at DPC level), GPU-presented,
 * skipped, renders, submits, GPU interrupts. G17 pacing (notes/g17-dxva-mfc.md): where DWM's frames go.
 *
 *   kmdstats [seconds]
 *   kmdstats busy [seconds]   sample the job queue (~500x/s measured: escape + Sleep(1)): share of samples with a job running (~ GPU busy) and
 *                             the queue depth histogram
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "maliwddm_abi.h"

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")

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
    int busy = argc > 1 && !strcmp(argv[1], "busy");
    int secs = argc > 1 + busy ? atoi(argv[1 + busy]) : 5;
    D3DKMT_HANDLE a = FindAdapter();
    MW_ESCAPE_STATS_DATA s0 = { 0 }, s1 = { 0 };
    if (!a) {
        printf("no MaliG76Wddm adapter\n");
        return 1;
    }
    s0.Code = s1.Code = MW_ESCAPE_STATS;
    if (busy) {
        /* G17 pacing (notes/g17-dxva-mfc.md): how often the KMD has a job chain on the GPU */
        unsigned n = 0, run = 0, depth[5] = { 0 };
        LARGE_INTEGER f, t0, t;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        timeBeginPeriod(1);
        do {
            MW_ESCAPE_STATS_DATA x = { 0 };
            x.Code = MW_ESCAPE_STATS;
            if (KmtEscape(a, &x, sizeof(x)) < 0)
                return 1;
            n++;
            run += x.Running != 0;
            depth[x.Queued > 4 ? 4 : x.Queued]++;
            Sleep(1);
            QueryPerformanceCounter(&t);
        } while ((t.QuadPart - t0.QuadPart) < (LONGLONG)secs * f.QuadPart);
        timeEndPeriod(1);
        printf("busy over %d s: %u samples, job running in %.1f %%; queued 0:%u 1:%u 2:%u 3:%u 4+:%u\n", secs, n,
               100.0 * run / n, depth[0], depth[1], depth[2], depth[3], depth[4]);
        return 0;
    }
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
    /* G17 (notes/g17-dxva-mfc.md): failures and VidMm paging over the interval - candidates for ~1 s GPU stalls */
    printf("failed %u timed out %u resets %u | allocations %u (now %u) | maps %u map fails %u unmaps %u | paging ops", s1.JobsFailed - s0.JobsFailed,
           s1.JobsTimedOut - s0.JobsTimedOut, s1.Resets - s0.Resets, s1.Allocations - s0.Allocations, s1.Allocations, s1.Maps - s0.Maps, s1.MapFails - s0.MapFails,
           s1.Unmaps - s0.Unmaps);
    for (int i = 0; i < 16; i++)
        if (s1.PagingOps[i] != s0.PagingOps[i])
            printf(" [%d] %u", i, s1.PagingOps[i] - s0.PagingOps[i]);
    printf("\n");
    return 0;
}
