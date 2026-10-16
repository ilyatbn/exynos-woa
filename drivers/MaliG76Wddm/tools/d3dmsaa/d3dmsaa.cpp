/*
 * d3dmsaa - FL 10_1 task F1 (notes/gpu-umd-d3d11.md): MSAA on the Mali D3D11 UMD without shaders or a window.
 * Prints the device's feature level, CheckMultisampleQualityLevels for RGBA8/BGRA8/D24S8 at 2/4/8/16 samples, then
 * clears a 64x64 4x RGBA8 render target, ResolveSubresource into a 1x texture, copies to staging and checks the
 * pixels. Exit 0 = resolve gave the clear colour. Build: build-d3dmsaa.cmd.
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <stdio.h>
#include <d3d11.h>
#include <dxgi1_2.h>

int main(int argc, char **argv)
{
    bool warp = argc > 1 && !strcmp(argv[1], "warp");   /* d3dmsaa [warp] [all] */
    IDXGIFactory1 *fac = NULL;
    IDXGIAdapter1 *ad = NULL, *pick = NULL;
    DXGI_ADAPTER_DESC1 d;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac)))
        return 1;
    for (UINT i = 0; fac->EnumAdapters1(i, &ad) == S_OK; i++) {
        ad->GetDesc1(&d);
        if (!pick && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId != 0x1414) {
            pick = ad;
            printf("adapter: %ls\n", d.Description);
        } else {
            ad->Release();
        }
    }
    if (!pick && !warp)
        return 1;
    D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 }, got;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    HRESULT hr = D3D11CreateDevice(warp ? NULL : (IDXGIAdapter *)pick, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN,
                                   NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls, 3, D3D11_SDK_VERSION, &dev, &got, &ctx);
    printf("D3D11CreateDevice 0x%08lx FL 0x%x\n", (unsigned long)hr, got);
    if (FAILED(hr))
        return 2;
    if (argc > 1 && !strcmp(argv[argc - 1], "all")) {
        /* "all": every DXGI format 1..115 - CheckFormatSupport, levels at 4x/8x, and for every format that claims
         * MSAA render-target support at 4x a real 4x RT (or DS) create + RTV/DSV (proof of each claimed level) */
        UINT bad = 0;
        for (UINT f = 1; f <= 115; f++) {
            UINT sup = 0, q4 = 0, q8 = 0;
            if (FAILED(dev->CheckFormatSupport((DXGI_FORMAT)f, &sup)) || !sup)
                continue;
            dev->CheckMultisampleQualityLevels((DXGI_FORMAT)f, 4, &q4);
            dev->CheckMultisampleQualityLevels((DXGI_FORMAT)f, 8, &q8);
            const char *res = "";
            if (q4 && (sup & (D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET))) {
                bool ds = (sup & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL) != 0;
                D3D11_TEXTURE2D_DESC t = {};
                t.Width = t.Height = 32; t.MipLevels = t.ArraySize = 1; t.Format = (DXGI_FORMAT)f;
                t.SampleDesc.Count = 4; t.Usage = D3D11_USAGE_DEFAULT;
                t.BindFlags = ds ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET;
                ID3D11Texture2D *tx = NULL;
                HRESULT h = dev->CreateTexture2D(&t, NULL, &tx);
                if (SUCCEEDED(h)) {
                    if (ds) {
                        ID3D11DepthStencilView *v = NULL;
                        h = dev->CreateDepthStencilView(tx, NULL, &v);
                        if (v) { ctx->ClearDepthStencilView(v, D3D11_CLEAR_DEPTH, 1.0f, 0); v->Release(); }
                    } else {
                        ID3D11RenderTargetView *v = NULL;
                        h = dev->CreateRenderTargetView(tx, NULL, &v);
                        float z[4] = { 0, 0, 0, 0 };
                        if (v) { ctx->ClearRenderTargetView(v, z); v->Release(); }
                    }
                    tx->Release();
                }
                res = SUCCEEDED(h) ? " 4x-create OK" : " 4x-create FAIL";
                if (FAILED(h))
                    bad++;
            }
            printf("fmt %3u support 0x%08x 4x=%u 8x=%u%s" "\n", f, sup, q4, q8, res);
        }
        ctx->Flush();
        printf("all: %u claimed-4x formats failed" "\n", bad);
        return bad ? 8 : 0;
    }
    DXGI_FORMAT fmts[] = { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_D24_UNORM_S8_UINT };
    const char *names[] = { "RGBA8", "BGRA8", "D24S8" };
    for (int f = 0; f < 3; f++) {
        UINT sup = 0;
        dev->CheckFormatSupport(fmts[f], &sup);
        printf("%s support 0x%08x:", names[f], sup);
        for (UINT n = 2; n <= 16; n *= 2) {
            UINT q = 0;
            dev->CheckMultisampleQualityLevels(fmts[f], n, &q);
            printf(" %ux=%u", n, q);
        }
        printf("\n");
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 64; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 4; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *ms = NULL, *rs = NULL, *st = NULL;
    hr = dev->CreateTexture2D(&td, NULL, &ms);
    printf("CreateTexture2D 4x 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr))
        return 3;
    td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = dev->CreateTexture2D(&td, NULL, &rs);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr))
        hr = dev->CreateTexture2D(&td, NULL, &st);
    if (FAILED(hr))
        return 4;
    ID3D11RenderTargetView *rtv = NULL;
    hr = dev->CreateRenderTargetView(ms, NULL, &rtv);
    printf("CreateRenderTargetView 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr))
        return 5;
    float c[4] = { 1.0f, 0.5f, 0.25f, 1.0f };
    ctx->ClearRenderTargetView(rtv, c);
    ctx->ResolveSubresource(rs, 0, ms, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    ctx->CopyResource(st, rs);
    D3D11_MAPPED_SUBRESOURCE m;
    hr = ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
    printf("Map 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr))
        return 6;
    UINT bad = 0, first = 0;
    for (UINT y = 0; y < 64; y++)
        for (UINT x = 0; x < 64; x++) {
            UINT p = ((UINT *)((BYTE *)m.pData + y * m.RowPitch))[x];
            if (y == 0 && x == 0)
                first = p;
            /* R=255 G=128 B=64 A=255, +-1 for unorm rounding */
            BYTE r = p & 0xff, g = (p >> 8) & 0xff, b = (p >> 16) & 0xff, a = p >> 24;
            if (r < 254 || g < 127 || g > 128 || b < 63 || b > 64 || a < 254)
                bad++;
        }
    ctx->Unmap(st, 0);
    printf("resolve: pixel(0,0) 0x%08x, %u of 4096 wrong -> %s\n", first, bad, bad ? "FAIL" : "PASS");
    return bad ? 7 : 0;
}
