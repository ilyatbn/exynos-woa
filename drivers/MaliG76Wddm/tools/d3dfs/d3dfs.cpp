/*
 * d3dfs.exe [W H [seconds]] - G24 (notes/g24-compute-cs4x.md): what a DXGI exclusive-fullscreen swapchain does on the
 * S10e's rotated desktop (NFSU2/dgVoodoo fullscreen came out sideways: the driver's present Blt got Rotate IDENTITY
 * for a 1520x720 back buffer on a 720x1520 primary). Creates a W x H (default 1520x720) BGRA blt-model swapchain,
 * goes fullscreen, presents a frame whose top-left quadrant is green and the rest red for <seconds> (default 4), and
 * prints the output rotation (IDXGIOutput::GetDesc) and the GDI mode / orientation before, during and after.
 * Run in the user's session; the window must own the foreground for SetFullscreenState.
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>

static void state(const char *when, IDXGISwapChain *sc)
{
    DEVMODEW dm = {};
    dm.dmSize = sizeof dm;
    EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm);
    int rot = -1;
    BOOL fs = FALSE;
    if (sc) {
        IDXGIOutput *o = NULL;
        sc->GetFullscreenState(&fs, &o);
        if (!o)
            sc->GetContainingOutput(&o);
        if (o) {
            DXGI_OUTPUT_DESC d;
            if (SUCCEEDED(o->GetDesc(&d)))
                rot = (int)d.Rotation;
            o->Release();
        }
    }
    printf("%-8s gdi %lux%lu orientation %lu | dxgi fullscreen %d output rotation %d (1 identity, 2 rot90, 4 rot270)\n",
           when, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayOrientation, fs, rot);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    UINT w = argc > 2 ? (UINT)atoi(argv[1]) : 1520, h = argc > 2 ? (UINT)atoi(argv[2]) : 720;
    int secs = argc > 3 ? atoi(argv[3]) : 4;
    state("before", NULL);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"d3dfs";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"d3dfs", L"d3dfs", WS_POPUP | WS_VISIBLE, 0, 0, (int)w, (int)h, NULL,
                                NULL, wc.hInstance, NULL);
    /* A process started by a scheduled task may not take the foreground (SetFullscreenState then fails): a synthetic
     * Alt press counts as input to this process, which unlocks SetForegroundWindow (documented foreground rules). */
    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    SetForegroundWindow(hwnd);
    for (int i = 0; i < 20; i++) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&m);
        Sleep(10);
    }
    printf("foreground %s\n", GetForegroundWindow() == hwnd ? "yes" : "NO");

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = w;
    sd.BufferDesc.Height = h;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    IDXGISwapChain *sc = NULL;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_1;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &sd,
                                               &sc, &dev, NULL, &ctx);
    printf("create %ux%u hr 0x%08lx\n", w, h, (unsigned long)hr);
    if (FAILED(hr))
        return 1;
    state("windowed", sc);
    hr = sc->SetFullscreenState(TRUE, NULL);
    printf("SetFullscreenState(TRUE) hr 0x%08lx\n", (unsigned long)hr);
    hr = sc->ResizeBuffers(1, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
    printf("ResizeBuffers hr 0x%08lx\n", (unsigned long)hr);
    state("full", sc);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w / 2;
    td.Height = h / 2;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *green = NULL;
    ID3D11RenderTargetView *grv = NULL;
    dev->CreateTexture2D(&td, NULL, &green);
    dev->CreateRenderTargetView(green, NULL, &grv);
    const float g[4] = {0, 1, 0, 1}, r[4] = {1, 0, 0, 1};
    ctx->ClearRenderTargetView(grv, g);

    for (DWORD t0 = GetTickCount(); GetTickCount() - t0 < (DWORD)secs * 1000;) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&m);
        ID3D11Texture2D *bb = NULL;
        ID3D11RenderTargetView *rtv = NULL;
        sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
        dev->CreateRenderTargetView(bb, NULL, &rtv);
        ctx->ClearRenderTargetView(rtv, r);
        ctx->CopySubresourceRegion(bb, 0, 0, 0, 0, green, 0, NULL);
        hr = sc->Present(1, 0);
        rtv->Release();
        bb->Release();
        if (FAILED(hr)) {
            printf("Present hr 0x%08lx\n", (unsigned long)hr);
            break;
        }
    }
    state("full-end", sc);
    sc->SetFullscreenState(FALSE, NULL);
    state("after", sc);
    grv->Release();
    green->Release();
    sc->Release();
    ctx->Release();
    dev->Release();
    DestroyWindow(hwnd);
    return 0;
}
