/*
 * gpuinfo.exe - the Mali GPU properties MaliG76Wddm reports to Panfrost (MW_ESCAPE_GPU_INFO, the raw ID registers,
 * indexed like DRM_PANFROST_PARAM_*), with TEXTURE_FEATURES_0 decoded for the block-compressed formats (G22,
 * notes/games-nfsu2.md: the UMD reports no BC1-BC5 support; Panfrost takes it from TEXTURE_FEATURES_0).
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <stdio.h>
#include <d3dkmthk.h>
#include "maliwddm_abi.h"

static const char *names[MW_GPU_PARAMS] = {
    "GPU_ID", "GPU_REVISION", "SHADER_PRESENT", "TILER_PRESENT", "L2_PRESENT", "STACK_PRESENT", "AS_PRESENT",
    "JS_PRESENT", "L2_FEATURES", "CORE_FEATURES", "TILER_FEATURES", "MEM_FEATURES", "MMU_FEATURES",
    "THREAD_FEATURES", "MAX_THREADS", "THREAD_MAX_WORKGROUP_SZ", "THREAD_MAX_BARRIER_SZ", "COHERENCY_FEATURES",
    "TEXTURE_FEATURES0", "TEXTURE_FEATURES1", "TEXTURE_FEATURES2", "TEXTURE_FEATURES3",
};

int main(void)
{
    D3DKMT_ADAPTERINFO info[16];
    D3DKMT_ENUMADAPTERS2 en = { 0 };
    en.NumAdapters = 16;
    en.pAdapters = info;
    if (D3DKMTEnumAdapters2(&en) < 0)
        return 1;
    for (ULONG i = 0; i < en.NumAdapters; i++) {
        MW_ESCAPE_GPU_INFO_DATA g = { 0 };
        D3DKMT_ESCAPE e = { 0 };
        g.Code = MW_ESCAPE_GPU_INFO;
        e.hAdapter = info[i].hAdapter;
        e.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
        e.pPrivateDriverData = &g;
        e.PrivateDriverDataSize = sizeof(g);
        if (D3DKMTEscape(&e) < 0 || !g.Valid)
            continue;
        for (ULONG k = 0; k < MW_GPU_PARAMS; k++)
            if (g.Valid & (1ULL << k))
                printf("%2lu %-24s 0x%llx\n", k, k < 22 && names[k] ? names[k] : "", (unsigned long long)g.Value[k]);
        /* Panfrost pan_format.c: the texfeat bit of each compressed family (MALI_BC1_UNORM etc. = bit numbers). */
        unsigned long long t = g.Value[18];
        printf("TEXTURE_FEATURES0 bits set:");
        for (int b = 0; b < 32; b++)
            if (t & (1ULL << b))
                printf(" %d", b);
        printf("\n");
        return 0;
    }
    printf("no MaliG76Wddm adapter answered MW_ESCAPE_GPU_INFO\n");
    return 1;
}
