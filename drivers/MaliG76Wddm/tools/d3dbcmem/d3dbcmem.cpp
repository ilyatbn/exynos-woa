/*
 * d3dbcmem [max] - G26 (notes/g26-bc-etc2.md): the NFSU2 race-load failure as a test. A 32-bit process (2 GB of address
 * space, like SPEED2.EXE, which is not large-address-aware) creates 1024x1024 BC1 / BC3 textures (alternating, full mip
 * chain, initial data with realistic soft blocks) on the Mali D3D11 device until CreateTexture2D fails or <max>
 * (default 4000) exist, and prints how many fit, their BC size and the upload time per texture. With the G23 RGBA8
 * backing (D3D10UMD_BC_DECODE=1, diagnosis) every texture costs 4-8x its BC size; with the G26 ETC2 backing 1x.
 * Build: build-d3dbcmem.cmd (x86). SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <d3d11.h>
#include <dxgi.h>

static const UINT W = 1024, MIPS = 11;

static void FillSoft(unsigned char *d, size_t bytes, UINT block, UINT seed)
{
    UINT x = seed * 2654435761u + 7u;
    for (size_t i = 0; i < bytes; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        d[i] = (unsigned char)(x >> 8);
    }
    for (size_t b = 0; b + block <= bytes; b += block) {
        unsigned char *c = d + b + (block == 16 ? 8 : 0);   /* the RGB565 endpoints */
        unsigned c0 = c[0] | (c[1] << 8), c1 = c0 ^ (c[2] & 0x0861);
        c[2] = (unsigned char)c1;
        c[3] = (unsigned char)(c1 >> 8);
        if (block == 16) {   /* BC3 alpha endpoints close together */
            int a1 = d[b] + (int)(d[b + 1] % 49) - 24;
            d[b + 1] = (unsigned char)(a1 < 0 ? 0 : a1 > 255 ? 255 : a1);
        }
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int max = argc > 1 ? atoi(argv[1]) : 4000;
    const char *e = getenv("D3D10UMD_BC_DECODE");
    printf("d3dbcmem: %u-bit process, D3D10UMD_BC_DECODE=%s, up to %d textures of %ux%u x %u mips\n",
           (unsigned)(sizeof(void *) * 8), e ? e : "(unset)", max, W, W, MIPS);

    IDXGIFactory1 *fac = NULL;
    IDXGIAdapter1 *ad = NULL, *pick = NULL;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac);
    for (UINT i = 0; fac && fac->EnumAdapters1(i, &ad) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 dd;
        ad->GetDesc1(&dd);
        if (!pick && !(dd.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && dd.VendorId != 0x1414)
            pick = ad;
        else
            ad->Release();
    }
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_1, got;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    HRESULT hr = D3D11CreateDevice(pick, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, &got, &ctx);
    printf("Mali D3D11CreateDevice 0x%08lx FL 0x%x\n", (unsigned long)hr, got);
    if (FAILED(hr))
        return 2;

    /* one data set per format, all mips */
    unsigned char *data[2][MIPS];
    D3D11_SUBRESOURCE_DATA init[2][MIPS];
    size_t bcbytes[2] = {0, 0};
    for (int f = 0; f < 2; f++) {
        UINT block = f ? 16 : 8;
        for (UINT m = 0; m < MIPS; m++) {
            UINT mw = W >> m ? W >> m : 1, bw = (mw + 3) / 4;
            size_t bytes = (size_t)bw * bw * block;
            data[f][m] = (unsigned char *)malloc(bytes);
            FillSoft(data[f][m], bytes, block, f * 100 + m);
            init[f][m].pSysMem = data[f][m];
            init[f][m].SysMemPitch = bw * block;
            init[f][m].SysMemSlicePitch = 0;
            bcbytes[f] += bytes;
        }
    }

    ID3D11Texture2D **tex = (ID3D11Texture2D **)calloc((size_t)max, sizeof(*tex));
    int n = 0;
    double total_mb = 0;
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    double ms_sum = 0;
    for (; n < max; n++) {
        int f = n & 1;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = W;
        td.MipLevels = MIPS;
        td.ArraySize = 1;
        td.Format = f ? DXGI_FORMAT_BC3_UNORM : DXGI_FORMAT_BC1_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        QueryPerformanceCounter(&t0);
        hr = dev->CreateTexture2D(&td, init[f], &tex[n]);
        QueryPerformanceCounter(&t1);
        double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
        if (FAILED(hr) || !tex[n]) {
            printf("texture %d (%s): CreateTexture2D 0x%08lx\n", n, f ? "BC3" : "BC1", (unsigned long)hr);
            break;
        }
        ms_sum += ms;
        total_mb += bcbytes[f] / 1048576.0;
        if (n % 50 == 0)
            printf("  %5d textures, %7.1f MB of BC data, last upload %.1f ms\n", n + 1, total_mb, ms);
        if (n % 16 == 15)
            ctx->Flush();   /* let deferred frees and uploads drain, like a game's frames would */
    }
    printf("RESULT: %d textures fit (%.1f MB of BC data), mean upload %.1f ms per texture\n", n, total_mb,
           n ? ms_sum / n : 0.0);
    for (int i = 0; i < n; i++)
        tex[i]->Release();
    ctx->Release();
    dev->Release();
    return 0;
}
