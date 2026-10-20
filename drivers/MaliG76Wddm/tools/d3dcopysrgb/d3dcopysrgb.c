/*
 * d3dcopysrgb - CopySubresourceRegion / CopyResource between the sRGB and the linear variant of one format must copy
 * the bits unchanged (no colour-space conversion) - regression test for Mesa 0195 (notes/games-unity.md G33: these
 * copies became queued GPU blits with both sides viewed in the linear format; before, a CPU fallback).
 * Cases (each verified texel by texel through a same-format copy into a staging texture):
 *   1 staging R8G8B8A8_UNORM -> default R8G8B8A8_UNORM_SRGB, box (5,3)-(261,131) at (17,9), rest keeps its fill
 *   2 default R8G8B8A8_UNORM_SRGB -> staging R8G8B8A8_UNORM, whole subresource
 *   3 CopyResource default B8G8R8A8_UNORM (3 mips) -> default B8G8R8A8_UNORM_SRGB, mip 1 checked
 *   4 staging R8G8B8A8_UNORM -> slice 1 of a 2-slice default R8G8B8A8_UNORM_SRGB array, slice 0 keeps its fill
 *   d3dcopysrgb [warp]
 * SPDX-License-Identifier: GPL-2.0
 */
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <d3d11.h>

#define W 300
#define H 200
#define FILL 0x11223344u

static UINT32 Pat(UINT x, UINT y, UINT salt)
{
    UINT32 v = (x * 2654435761u) ^ (y * 40503u) ^ (salt * 0x9e3779b9u);
    return v | 0x80808080u;   /* high bytes: an sRGB<->linear conversion would change them */
}

static ID3D11Texture2D *Tex(ID3D11Device *dev, DXGI_FORMAT f, UINT w, UINT h, UINT mips, UINT slices,
                            D3D11_USAGE usage, UINT32 fill, UINT salt, int pattern)
{
    D3D11_TEXTURE2D_DESC d = { 0 };
    d.Width = w; d.Height = h; d.MipLevels = mips; d.ArraySize = slices; d.Format = f;
    d.SampleDesc.Count = 1; d.Usage = usage;
    d.BindFlags = usage == D3D11_USAGE_STAGING ? 0 : D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = usage == D3D11_USAGE_STAGING ? D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE : 0;
    D3D11_SUBRESOURCE_DATA init[16];
    UINT32 *buf[16] = { 0 };
    for (UINT s = 0; s < slices; s++) {
        for (UINT m = 0; m < mips; m++) {
            UINT i = s * mips + m, mw = max(w >> m, 1u), mh = max(h >> m, 1u);
            buf[i] = malloc((size_t)mw * mh * 4);
            for (UINT y = 0; y < mh; y++)
                for (UINT x = 0; x < mw; x++)
                    buf[i][y * mw + x] = pattern ? Pat(x, y, salt + i) : fill;
            init[i].pSysMem = buf[i]; init[i].SysMemPitch = mw * 4; init[i].SysMemSlicePitch = 0;
        }
    }
    ID3D11Texture2D *t = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(dev, &d, init, &t);
    for (UINT i = 0; i < 16; i++) free(buf[i]);
    if (FAILED(hr)) printf("CreateTexture2D fmt %d hr 0x%08lx\n", f, hr);
    return t;
}

/* Reads subresource sub of t (copied into a staging texture of the same format) and checks every texel of mip size
 * mw x mh: inside [x0,x1)x[y0,y1) it must be Pat(x - dx, y - dy, salt), outside it must be fill. */
static int Check(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *t, UINT sub, UINT mw, UINT mh,
                 UINT x0, UINT y0, UINT x1, UINT y1, int dx, int dy, UINT salt, UINT32 fill, const char *name)
{
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(t, &d);
    ID3D11Texture2D *st = t;
    if (d.Usage != D3D11_USAGE_STAGING) {
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &st))) { printf("%s: staging failed\n", name); return 0; }
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)t);
    }
    D3D11_MAPPED_SUBRESOURCE m;
    int bad = 0, first = 1;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, sub, D3D11_MAP_READ, 0, &m))) {
        printf("%s: Map failed\n", name); bad = 1;
    } else {
        for (UINT y = 0; y < mh; y++) {
            const UINT32 *row = (const UINT32 *)((const BYTE *)m.pData + (size_t)y * m.RowPitch);
            for (UINT x = 0; x < mw; x++) {
                int in = x >= x0 && x < x1 && y >= y0 && y < y1;
                UINT32 want = in ? Pat(x - dx, y - dy, salt) : fill;
                if (row[x] != want) {
                    if (first) printf("%s: first diff at (%u,%u) 0x%08x want 0x%08x (%s)\n", name, x, y, row[x], want,
                                      in ? "copied" : "outside");
                    first = 0; bad++;
                }
            }
        }
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, sub);
    }
    if (st != t) ID3D11Texture2D_Release(st);
    printf("%-44s %s (%d texels differ)\n", name, bad ? "FAIL" : "PASS", bad);
    return !bad;
}

int main(int argc, char **argv)
{
    int warp = argc > 1 && !strcmp(argv[1], "warp");
    ID3D11Device *dev; ID3D11DeviceContext *ctx;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(NULL, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                   D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr)) { printf("D3D11CreateDevice hr 0x%08lx\n", hr); return 1; }
    int ok = 1;

    /* 1: staging UNORM -> default SRGB, partial box at an offset */
    ID3D11Texture2D *s1 = Tex(dev, DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, 1, D3D11_USAGE_STAGING, 0, 1, 1);
    ID3D11Texture2D *d1 = Tex(dev, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, W, H, 1, 1, D3D11_USAGE_DEFAULT, FILL, 0, 0);
    D3D11_BOX b1 = { 5, 3, 0, 261, 131, 1 };
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)d1, 0, 17, 9, 0, (ID3D11Resource *)s1, 0, &b1);
    ok &= Check(dev, ctx, d1, 0, W, H, 17, 9, 17 + 256, 9 + 128, 17 - 5, 9 - 3, 1, FILL,
                "1 staging unorm -> default srgb (box)");

    /* 2: default SRGB (pattern) -> staging UNORM, whole */
    ID3D11Texture2D *d2 = Tex(dev, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, W, H, 1, 1, D3D11_USAGE_DEFAULT, 0, 2, 1);
    ID3D11Texture2D *s2 = Tex(dev, DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, 1, D3D11_USAGE_STAGING, FILL, 0, 0);
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)s2, 0, 0, 0, 0, (ID3D11Resource *)d2, 0, NULL);
    ok &= Check(dev, ctx, s2, 0, W, H, 0, 0, W, H, 0, 0, 2, FILL, "2 default srgb -> staging unorm (whole)");

    /* 3: CopyResource BGRA UNORM 3 mips -> BGRA SRGB, check mip 1 */
    ID3D11Texture2D *d3a = Tex(dev, DXGI_FORMAT_B8G8R8A8_UNORM, W, H, 3, 1, D3D11_USAGE_DEFAULT, 0, 3, 1);
    ID3D11Texture2D *d3b = Tex(dev, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, W, H, 3, 1, D3D11_USAGE_DEFAULT, FILL, 0, 0);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)d3b, (ID3D11Resource *)d3a);
    ok &= Check(dev, ctx, d3b, 1, W / 2, H / 2, 0, 0, W / 2, H / 2, 0, 0, 3 + 1, FILL,
                "3 CopyResource bgra unorm -> srgb (mip 1)");

    /* 4: staging UNORM -> slice 1 of a SRGB array */
    ID3D11Texture2D *s4 = Tex(dev, DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 1, D3D11_USAGE_STAGING, 0, 4, 1);
    ID3D11Texture2D *d4 = Tex(dev, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 64, 64, 1, 2, D3D11_USAGE_DEFAULT, FILL, 0, 0);
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)d4, 1, 0, 0, 0, (ID3D11Resource *)s4, 0, NULL);
    ok &= Check(dev, ctx, d4, 1, 64, 64, 0, 0, 64, 64, 0, 0, 4, FILL, "4 staging unorm -> srgb array slice 1");
    ok &= Check(dev, ctx, d4, 0, 64, 64, 0, 0, 0, 0, 0, 0, 0, FILL, "4 srgb array slice 0 untouched");

    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
