/*
 * flipseq - G17 (notes/g17-dxva-mfc.md): does DWM show each frame of a windowed flip-model swap chain once and in
 * order? User report: "maybe I'm seeing the same frames multiple times". A maximized window (covers the screen centre)
 * with an mpv-like swap chain (flip-discard, 3 buffers by default) presents N frames, one every MS milliseconds; frame
 * f is cleared to B8G8R8A8 (B = f & 0xff, G = (f >> 8) & 0xff, R = 0x80, A = 0xff). DWM's UMD logs the centre pixel
 * of every composed frame when C:\S10eTmp\seqpix.flg exists (d3d10_gdi.c TEMP "seqpix"); flipseq.ps1 decodes it.
 *
 *   flipseq [frames=240] [ms=42] [buffers=3] [seq]   (seq: FLIP_SEQUENTIAL instead of FLIP_DISCARD)
 *
 * Run in the logged-on user's session. SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <d3d11.h>
#include <dxgi1_3.h>

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 240;
    int ms = argc > 2 ? atoi(argv[2]) : 42;
    UINT nbuf = argc > 3 ? (UINT)atoi(argv[3]) : 3;
    bool seq = argc > 4 && !strcmp(argv[4], "seq");
    IDXGIFactory2 *fac = NULL;
    IDXGIAdapter1 *ad = NULL, *pick = NULL;
    DXGI_ADAPTER_DESC1 d;
    if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void **)&fac)))
        return 1;
    for (UINT i = 0; fac->EnumAdapters1(i, &ad) == S_OK; i++) {
        ad->GetDesc1(&d);
        if (!pick && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId != 0x1414)
            pick = ad;
        else
            ad->Release();
    }
    if (!pick)
        return 1;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0, got = (D3D_FEATURE_LEVEL)0;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    HRESULT hr = D3D11CreateDevice(pick, D3D_DRIVER_TYPE_UNKNOWN, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                   D3D11_SDK_VERSION, &dev, &got, &ctx);
    printf("D3D11CreateDevice 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr))
        return 1;
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"flipseq";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"flipseq", L"flipseq", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, NULL, NULL,
                                wc.hInstance, NULL);
    ShowWindow(hwnd, SW_MAXIMIZE);
    RECT rc;
    GetClientRect(hwnd, &rc);
    DXGI_SWAP_CHAIN_DESC1 sd = {0};
    sd.Width = (UINT)(rc.right - rc.left);
    sd.Height = (UINT)(rc.bottom - rc.top);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = nbuf;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = seq ? DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL : DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1 *sc = NULL;
    hr = fac->CreateSwapChainForHwnd((IUnknown *)dev, hwnd, &sd, NULL, NULL, &sc);
    printf("swap chain %ux%u %s %u buffers: 0x%08lx\n", sd.Width, sd.Height, seq ? "flip-sequential" : "flip-discard",
           nbuf, (unsigned long)hr);
    if (FAILED(hr))
        return 1;
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&m);
    Sleep(1000);   /* let the maximize animation finish before frame 0 */
    LARGE_INTEGER qf, q0, qn;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&q0);
    for (int f = 0; f < frames; f++) {
        ID3D11Texture2D *bb = NULL;
        ID3D11RenderTargetView *rtv = NULL;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb)) &&
            SUCCEEDED(dev->CreateRenderTargetView((ID3D11Resource *)bb, NULL, &rtv))) {
            float c[4] = { 128 / 255.0f, ((f >> 8) & 0xff) / 255.0f, (f & 0xff) / 255.0f, 1.0f };
            ctx->OMSetRenderTargets(1, &rtv, NULL);
            ctx->ClearRenderTargetView(rtv, c);
            rtv->Release();
        }
        if (bb)
            bb->Release();
        hr = sc->Present(1, 0);
        if (FAILED(hr))
            printf("frame %d present 0x%08lx\n", f, (unsigned long)hr);
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&m);
        /* pace to frame f+1 at (f+1) * ms from the start */
        for (;;) {
            QueryPerformanceCounter(&qn);
            double t = (double)(qn.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart;
            if (t >= (f + 1) * (double)ms)
                break;
            Sleep(1);
        }
    }
    QueryPerformanceCounter(&qn);
    printf("done: %d frames in %.2f s\n", frames, (double)(qn.QuadPart - q0.QuadPart) / qf.QuadPart);
    Sleep(500);
    return 0;
}
