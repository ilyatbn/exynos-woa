/*
 * panelon [0|1] - light (1, default) or darken (0) the internal panel through MaliG76Wddm's MW_ESCAPE_PANEL_POWER
 * (DCS 0x29 / 0x28). Works from a non-interactive session (SSH, SYSTEM): enumerates adapters with
 * D3DKMTEnumAdapters2 instead of a GDI display name. Rescue for a panel left dark when s10ekeys' blank window was
 * killed (e.g. by a shutdown) before it could light the panel again (notes/gpu-umd-d3d11.md 2026-10-17).
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <d3dkmthk.h>
#include "maliwddm_abi.h"

int main(int argc, char **argv)
{
    D3DKMT_ADAPTERINFO info[16];
    D3DKMT_ENUMADAPTERS2 en = { 0 };
    ULONG i;
    int on = argc > 1 ? atoi(argv[1]) : 1, done = 0;

    en.NumAdapters = 16;
    en.pAdapters = info;
    if (D3DKMTEnumAdapters2(&en) < 0) {
        printf("EnumAdapters2 failed\n");
        return 1;
    }
    for (i = 0; i < en.NumAdapters; i++) {
        MW_ESCAPE_PANEL_POWER_DATA d = { 0 };
        D3DKMT_ESCAPE e = { 0 };
        NTSTATUS st;
        d.Code = MW_ESCAPE_PANEL_POWER;
        d.On = on ? 1 : 0;
        e.hAdapter = info[i].hAdapter;
        e.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
        e.pPrivateDriverData = &d;
        e.PrivateDriverDataSize = sizeof(d);
        st = D3DKMTEscape(&e);
        printf("adapter %lu: escape 0x%08lx done %u\n", i, (unsigned long)st, d.Done);
        if (st >= 0 && d.Done)
            done = 1;
    }
    return done ? 0 : 2;
}
