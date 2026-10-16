/*
 * d3dswap - G15 S4 (notes/gpu-wddm2-design.md): a DWM-bound composition swap chain like Windows Terminal's, small
 * enough to run under cdb. D3D11 device (BGRA) on the first hardware adapter, IDXGIFactory2::
 * CreateSwapChainForComposition (flip sequential, 2 buffers), DirectComposition target on a window, then N clears
 * + presents. With a NULL DXGI tracked fence the third present's buffer acquire dies in
 * d3d11!NDXGI::CDevice::WaitTrackedFenceImpl (Terminal's crash). Prints each step; exit 0 = all presents done.
 * Run in the logged-on user's session (DWM). Build: build-d3dswap.cmd.
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <stdio.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 10;
    /* d3dswap FRAMES [comp|hwnd] [seq|disc] [BUFFERS]: which swap chains get a DXGI tracked fence */
    bool comp = !(argc > 2 && !strcmp(argv[2], "hwnd"));
    bool disc = argc > 3 && !strcmp(argv[3], "disc");
    UINT nbuf = argc > 4 ? (UINT)atoi(argv[4]) : 2;
    bool warp = argc > 5 && !strcmp(argv[5], "warp");   /* control: the same swap chain on WARP */
    printf("mode %s effect %s buffers %u\n", comp ? "composition" : "hwnd", disc ? "flip-discard" : "flip-sequential", nbuf);
    IDXGIFactory2 *fac = NULL;
    IDXGIAdapter1 *ad = NULL, *pick = NULL;
    DXGI_ADAPTER_DESC1 d;
    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void **)&fac);
    printf("CreateDXGIFactory2 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr))
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
    if (!pick)
        return 1;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0, got = (D3D_FEATURE_LEVEL)0;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    hr = D3D11CreateDevice(warp ? NULL : (IDXGIAdapter *)pick, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, NULL,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1, D3D11_SDK_VERSION, &dev, &got, &ctx);
    if (warp)
        puts("device: WARP");
    printf("D3D11CreateDevice 0x%08lx FL 0x%x\n", (unsigned long)hr, got);
    if (FAILED(hr))
        return 1;

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"d3dswap";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(comp ? WS_EX_NOREDIRECTIONBITMAP : 0, L"d3dswap", L"d3dswap", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                100, 100, 400, 300, NULL, NULL, wc.hInstance, NULL);
    printf("window %p\n", (void *)hwnd);

    DXGI_SWAP_CHAIN_DESC1 sd = {0};
    sd.Width = 400;
    sd.Height = 300;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = nbuf;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = disc ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = comp ? DXGI_ALPHA_MODE_PREMULTIPLIED : DXGI_ALPHA_MODE_IGNORE;
    IDXGISwapChain1 *sc = NULL;
    if (comp)
        hr = fac->CreateSwapChainForComposition((IUnknown *)dev, &sd, NULL, &sc);
    else
        hr = fac->CreateSwapChainForHwnd((IUnknown *)dev, hwnd, &sd, NULL, NULL, &sc);
    printf("CreateSwapChain 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr))
        return 1;
    if (!comp)
        goto frames_loop;

    IDCompositionDevice *dc = NULL;
    IDXGIDevice *xd = NULL;
    dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&xd);
    hr = DCompositionCreateDevice(xd, __uuidof(IDCompositionDevice), (void **)&dc);
    printf("DCompositionCreateDevice 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr))
        return 1;
    IDCompositionTarget *tgt = NULL;
    IDCompositionVisual *vis = NULL;
    hr = dc->CreateTargetForHwnd(hwnd, TRUE, &tgt);
    printf("CreateTargetForHwnd 0x%08lx\n", (unsigned long)hr);
    dc->CreateVisual(&vis);
    vis->SetContent((IUnknown *)sc);
    tgt->SetRoot(vis);
    hr = dc->Commit();
    printf("Commit 0x%08lx\n", (unsigned long)hr);
frames_loop:

    for (int f = 0; f < frames; f++) {
        ID3D11Texture2D *bb = NULL;
        ID3D11RenderTargetView *rtv = NULL;
        hr = sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
        if (SUCCEEDED(hr))
            hr = dev->CreateRenderTargetView((ID3D11Resource *)bb, NULL, &rtv);
        if (rtv) {
            float c[4] = { (f & 1) ? 0.8f : 0.1f, 0.3f, 0.6f, 1.0f };
            ctx->OMSetRenderTargets(1, &rtv, NULL);
            ctx->ClearRenderTargetView(rtv, c);
            rtv->Release();
        }
        if (bb)
            bb->Release();
        hr = sc->Present(1, 0);
        printf("frame %d present 0x%08lx\n", f, (unsigned long)hr);
        fflush(stdout);
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&m);
    }
    printf("done\n");
    return 0;
}
