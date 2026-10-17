/*
 * gpuclock [ms] [count] - the Mali's real clock from MaliG76Wddm's MW_ESCAPE_CLOCK (GPU cycle counter + QPC): two
 * readings ms apart (default 1000), count times (default 3). Works from any session (D3DKMTEnumAdapters2). Used to
 * check that an ACPM G3D frequency request really changes the clock (notes/clocks-dvfs.md C5).
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <d3dkmthk.h>
#include "maliwddm_abi.h"

static BOOL Read(D3DKMT_HANDLE a, MW_ESCAPE_CLOCK_DATA *c)
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

int main(int argc, char **argv)
{
    D3DKMT_ADAPTERINFO info[16];
    D3DKMT_ENUMADAPTERS2 en = { 0 };
    int ms = argc > 1 ? atoi(argv[1]) : 1000, n = argc > 2 ? atoi(argv[2]) : 3, k;
    ULONG i;
    D3DKMT_HANDLE mali = 0;
    MW_ESCAPE_CLOCK_DATA a, b;

    en.NumAdapters = 16;
    en.pAdapters = info;
    if (D3DKMTEnumAdapters2(&en) < 0)
        return 1;
    for (i = 0; i < en.NumAdapters && !mali; i++)
        if (Read(info[i].hAdapter, &a))
            mali = info[i].hAdapter;
    if (!mali) {
        printf("no adapter answered MW_ESCAPE_CLOCK (GPU not up?)\n");
        return 2;
    }
    for (k = 0; k < n; k++) {
        Read(mali, &a);
        Sleep(ms);
        Read(mali, &b);
        if (b.Qpc > a.Qpc && b.Cycles >= a.Cycles)
            printf("GPU clock %.1f MHz\n", (double)(b.Cycles - a.Cycles) * (double)b.QpcFreq / (double)(b.Qpc - a.Qpc) / 1e6);
    }
    return 0;
}
