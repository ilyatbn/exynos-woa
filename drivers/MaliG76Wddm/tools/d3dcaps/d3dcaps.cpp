/*
 * d3dcaps - what the D3D11 runtime reports for the Mali device next to WARP limited to the same feature level (10_1),
 * printing only the differences: CheckFormatSupport / FORMAT_SUPPORT2 of every DXGI format 1..132, the multisample
 * quality levels (2/4/8x) of the common formats, and CheckFeatureSupport of every feature that answers at FL 10_1.
 * G23 (notes/g23-bc-emulation.md): dgVoodoo works on WARP and gives up on the Mali after its pixel shaders; a
 * difference here that WARP has and the Mali lacks is a candidate cause. Build: build-d3dcaps.cmd (x86 / arm64).
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <stdio.h>
#include <d3d11_4.h>

struct Caps {
    UINT fs[133], fs2[133], ms[133][3];
    BYTE feat[24][64];
    HRESULT feathr[24];
};

static const struct { D3D11_FEATURE f; UINT size; const char *name; } g_feats[] = {
    { D3D11_FEATURE_THREADING, sizeof(D3D11_FEATURE_DATA_THREADING), "THREADING" },
    { D3D11_FEATURE_DOUBLES, sizeof(D3D11_FEATURE_DATA_DOUBLES), "DOUBLES" },
    { D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, sizeof(D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS), "D3D10_X_HARDWARE_OPTIONS" },
    { D3D11_FEATURE_D3D11_OPTIONS, sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS), "D3D11_OPTIONS" },
    { D3D11_FEATURE_ARCHITECTURE_INFO, sizeof(D3D11_FEATURE_DATA_ARCHITECTURE_INFO), "ARCHITECTURE_INFO" },
    { D3D11_FEATURE_D3D9_OPTIONS, sizeof(D3D11_FEATURE_DATA_D3D9_OPTIONS), "D3D9_OPTIONS" },
    { D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT, sizeof(D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT), "SHADER_MIN_PRECISION" },
    { D3D11_FEATURE_D3D9_SHADOW_SUPPORT, sizeof(D3D11_FEATURE_DATA_D3D9_SHADOW_SUPPORT), "D3D9_SHADOW_SUPPORT" },
    { D3D11_FEATURE_D3D11_OPTIONS1, sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS1), "D3D11_OPTIONS1" },
    { D3D11_FEATURE_D3D9_SIMPLE_INSTANCING_SUPPORT, sizeof(D3D11_FEATURE_DATA_D3D9_SIMPLE_INSTANCING_SUPPORT), "D3D9_SIMPLE_INSTANCING" },
    { D3D11_FEATURE_MARKER_SUPPORT, sizeof(D3D11_FEATURE_DATA_MARKER_SUPPORT), "MARKER_SUPPORT" },
    { D3D11_FEATURE_D3D9_OPTIONS1, sizeof(D3D11_FEATURE_DATA_D3D9_OPTIONS1), "D3D9_OPTIONS1" },
    { D3D11_FEATURE_D3D11_OPTIONS2, sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS2), "D3D11_OPTIONS2" },
    { D3D11_FEATURE_D3D11_OPTIONS3, sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS3), "D3D11_OPTIONS3" },
    { D3D11_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, sizeof(D3D11_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT), "GPU_VIRTUAL_ADDRESS" },
    { D3D11_FEATURE_D3D11_OPTIONS4, sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS4), "D3D11_OPTIONS4" },
    { D3D11_FEATURE_SHADER_CACHE, sizeof(D3D11_FEATURE_DATA_SHADER_CACHE), "SHADER_CACHE" },
    { D3D11_FEATURE_D3D11_OPTIONS5, sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS5), "D3D11_OPTIONS5" },
};
static const int NFEAT = sizeof(g_feats) / sizeof(g_feats[0]);
static const DXGI_FORMAT g_msfmt[] = { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM,
                                       DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D32_FLOAT,
                                       DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R32_FLOAT };

static bool Query(bool warp, IDXGIAdapter *ad, Caps *c)
{
    memset(c, 0, sizeof(*c));
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_1, got;
    ID3D11Device *dev = NULL;
    HRESULT hr = D3D11CreateDevice(warp ? NULL : ad, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
                                   &fl, 1, D3D11_SDK_VERSION, &dev, &got, NULL);
    printf("%s: D3D11CreateDevice 0x%08lx FL 0x%x\n", warp ? "WARP" : "Mali", (unsigned long)hr, got);
    if (FAILED(hr))
        return false;
    for (UINT f = 1; f < 133; f++) {
        dev->CheckFormatSupport((DXGI_FORMAT)f, &c->fs[f]);
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 s2 = { (DXGI_FORMAT)f, 0 };
        if (SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &s2, sizeof(s2))))
            c->fs2[f] = s2.OutFormatSupport2;
    }
    for (UINT i = 0; i < sizeof(g_msfmt) / sizeof(g_msfmt[0]); i++)
        for (UINT k = 0; k < 3; k++)
            dev->CheckMultisampleQualityLevels(g_msfmt[i], 2u << k, &c->ms[g_msfmt[i]][k]);
    for (int i = 0; i < NFEAT; i++)
        c->feathr[i] = dev->CheckFeatureSupport(g_feats[i].f, c->feat[i], g_feats[i].size);
    dev->Release();
    return true;
}

int main()
{
    IDXGIFactory1 *fac = NULL;
    IDXGIAdapter1 *ad = NULL, *pick = NULL;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac);
    for (UINT i = 0; fac && fac->EnumAdapters1(i, &ad) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        ad->GetDesc1(&d);
        if (!pick && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId != 0x1414)
            pick = ad;
        else
            ad->Release();
    }
    static Caps m, w;
    if (!pick || !Query(false, pick, &m) || !Query(true, NULL, &w))
        return 2;
    int diffs = 0;
    for (UINT f = 1; f < 133; f++) {
        if (m.fs[f] != w.fs[f]) {
            printf("format %3u: support Mali 0x%08x WARP 0x%08x  WARP-only 0x%08x  Mali-only 0x%08x\n", f, m.fs[f], w.fs[f],
                   w.fs[f] & ~m.fs[f], m.fs[f] & ~w.fs[f]);
            diffs++;
        }
        if (m.fs2[f] != w.fs2[f]) {
            printf("format %3u: support2 Mali 0x%08x WARP 0x%08x\n", f, m.fs2[f], w.fs2[f]);
            diffs++;
        }
    }
    for (UINT i = 0; i < sizeof(g_msfmt) / sizeof(g_msfmt[0]); i++)
        for (UINT k = 0; k < 3; k++)
            if (m.ms[g_msfmt[i]][k] != w.ms[g_msfmt[i]][k]) {
                printf("format %3u %ux quality levels: Mali %u WARP %u\n", (unsigned)g_msfmt[i], 2u << k,
                       m.ms[g_msfmt[i]][k], w.ms[g_msfmt[i]][k]);
                diffs++;
            }
    for (int i = 0; i < NFEAT; i++) {
        if (m.feathr[i] != w.feathr[i] || memcmp(m.feat[i], w.feat[i], g_feats[i].size)) {
            printf("feature %-26s hr Mali 0x%08lx WARP 0x%08lx  data Mali", g_feats[i].name, (unsigned long)m.feathr[i],
                   (unsigned long)w.feathr[i]);
            for (UINT b = 0; b < g_feats[i].size && b < 32; b++)
                printf(" %02x", m.feat[i][b]);
            printf(" | WARP");
            for (UINT b = 0; b < g_feats[i].size && b < 32; b++)
                printf(" %02x", w.feat[i][b]);
            printf("\n");
            diffs++;
        }
    }
    printf("%d differences\n", diffs);
    return 0;
}
