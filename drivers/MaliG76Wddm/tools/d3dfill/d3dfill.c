// SPDX-License-Identifier: GPL-2.0
/*
 * d3dfill.exe - can the full MaliG76Wddm adapter put a GPU-rendered frame on the panel without DWM composing it?
 * (M16 G13, notes/gpu-full-adapter-hang.md "Planned: fullscreen D3D11 clear/triangle test"). DWM's own frames never
 * reach the panel in full mode; this splits the fault: a solid colour seen on the webcam -> Mali render + present +
 * scanout work and the fault is DWM-specific; dark -> the fault is below DWM.
 *
 * Steps (each logged before it runs, with its HRESULT): pick the DXGI adapter whose description contains "Mali";
 * D3D11CreateDevice FL 10_0 (+BGRA, as DWM asks); a WS_POPUP window covering the primary monitor; a 2-buffer
 * DISCARD swap chain, windowed, then SetFullscreenState(TRUE) (exclusive fullscreen bypasses DWM composition; if it
 * is refused the run continues windowed, which goes through DWM). Then 4 phases of 4 s each - red, green, blue,
 * white - clearing and presenting ~30 times a second. Per phase: present count, first failing HRESULT, and a
 * readback of pixel (0,0) of the back buffer through a staging copy right after the clear (proves the GPU wrote it).
 * Must run in the interactive session (run-as-user.ps1): session 0 owns no display.
 *
 * usage: d3dfill.exe [seconds-per-phase]    Exit 0 = every phase presented; 1 no Mali adapter; 2 a step failed.
 * A watchdog thread ends the process 60 s past the planned run with "TIMEOUT in <step>". Build: build-d3dfill.cmd.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#pragma warning(push)
#pragma warning(disable : 4201)     /* nameless unions in the SDK headers (C mode) */
#include <dxgi.h>
#include <d3d11.h>
#pragma warning(pop)

static volatile const char *Step = "start";
static DWORD WatchdogMs;

static void Mark(const char *S)
{
    Step = S;
    printf("  .. %s\n", S);
    fflush(stdout);
}

static DWORD WINAPI Watchdog(LPVOID P)
{
    UNREFERENCED_PARAMETER(P);
    Sleep(WatchdogMs);
    printf("d3dfill: TIMEOUT in %s\n", (const char *)Step);
    fflush(stdout);
    ExitProcess(3);
}

static LRESULT CALLBACK WndProc(HWND W, UINT M, WPARAM Wp, LPARAM Lp)
{
    return DefWindowProcW(W, M, Wp, Lp);
}

static void Pump(void)
{
    MSG Msg;
    while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&Msg);
        DispatchMessageW(&Msg);
    }
}

static IDXGIAdapter1 *FindMali(IDXGIFactory1 *Factory)
{
    IDXGIAdapter1 *Adapter;
    DXGI_ADAPTER_DESC1 Desc;
    UINT I;

    for (I = 0; IDXGIFactory1_EnumAdapters1(Factory, I, &Adapter) == S_OK; I++) {
        IDXGIAdapter1_GetDesc1(Adapter, &Desc);
        printf("adapter %u: %ls flags 0x%x\n", I, Desc.Description, Desc.Flags);
        if (wcsstr(Desc.Description, L"Mali") != NULL)
            return Adapter;
        IDXGIAdapter1_Release(Adapter);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    static const float Colours[4][4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 }, { 1, 1, 1, 1 } };
    static const char *Names[4] = { "red", "green", "blue", "white" };
    DWORD PhaseMs = (argc > 1) ? (DWORD)atoi(argv[1]) * 1000 : 4000;
    IDXGIFactory1 *Factory = NULL;
    IDXGIAdapter1 *Adapter;
    ID3D11Device *Dev = NULL;
    ID3D11DeviceContext *Ctx = NULL;
    IDXGISwapChain *Swap = NULL;
    ID3D11Texture2D *Back = NULL, *Staging = NULL;
    ID3D11RenderTargetView *Rtv = NULL;
    D3D_FEATURE_LEVEL Want = D3D_FEATURE_LEVEL_10_0, Got = 0;
    DXGI_SWAP_CHAIN_DESC Sd;
    D3D11_TEXTURE2D_DESC Td;
    WNDCLASSW Wc;
    HWND Wnd;
    HRESULT Hr;
    int Width, Height, Phase, Failed = 0;

    if (PhaseMs == 0)
        PhaseMs = 4000;
    WatchdogMs = PhaseMs * 4 + 60000;
    CreateThread(NULL, 0, Watchdog, NULL, 0, NULL);
    setvbuf(stdout, NULL, _IONBF, 0);

    /* review 211ede8: a DPI-unaware process gets scaled SM_CXSCREEN/SM_CYSCREEN (the S10e runs above 100 % scaling),
     * so the swap chain and the fullscreen mode request would not match the panel's native size. */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Mark("CreateDXGIFactory1");
    Hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&Factory);
    printf("CreateDXGIFactory1 hr 0x%08lx\n", Hr);
    if (FAILED(Hr))
        return 2;
    Mark("find Mali adapter");
    Adapter = FindMali(Factory);
    if (Adapter == NULL) {
        printf("no Mali adapter\n");
        return 1;
    }

    Mark("D3D11CreateDevice");
    Hr = D3D11CreateDevice((IDXGIAdapter *)Adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                           &Want, 1, D3D11_SDK_VERSION, &Dev, &Got, &Ctx);
    printf("D3D11CreateDevice hr 0x%08lx FL 0x%x\n", Hr, Got);
    if (FAILED(Hr))
        return 2;

    Mark("window");
    Width = GetSystemMetrics(SM_CXSCREEN);
    Height = GetSystemMetrics(SM_CYSCREEN);
    ZeroMemory(&Wc, sizeof(Wc));
    Wc.lpfnWndProc = WndProc;
    Wc.hInstance = GetModuleHandleW(NULL);
    Wc.lpszClassName = L"d3dfill";
    Wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&Wc);
    Wnd = CreateWindowExW(WS_EX_TOPMOST, L"d3dfill", L"d3dfill", WS_POPUP | WS_VISIBLE, 0, 0, Width, Height, NULL,
                          NULL, Wc.hInstance, NULL);
    printf("window %p %dx%d\n", (void *)Wnd, Width, Height);
    if (Wnd == NULL)
        return 2;
    Pump();

    Mark("CreateSwapChain");
    ZeroMemory(&Sd, sizeof(Sd));
    Sd.BufferDesc.Width = (UINT)Width;
    Sd.BufferDesc.Height = (UINT)Height;
    Sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    Sd.BufferDesc.RefreshRate.Numerator = 60;
    Sd.BufferDesc.RefreshRate.Denominator = 1;
    Sd.SampleDesc.Count = 1;
    Sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    Sd.BufferCount = 2;
    Sd.OutputWindow = Wnd;
    Sd.Windowed = TRUE;
    Sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    Hr = IDXGIFactory1_CreateSwapChain(Factory, (IUnknown *)Dev, &Sd, &Swap);
    printf("CreateSwapChain hr 0x%08lx\n", Hr);
    if (FAILED(Hr))
        return 2;
    Mark("SetFullscreenState");
    Hr = IDXGISwapChain_SetFullscreenState(Swap, TRUE, NULL);
    printf("SetFullscreenState(TRUE) hr 0x%08lx (%s)\n", Hr, SUCCEEDED(Hr) ? "exclusive fullscreen" : "staying windowed");

    Mark("back buffer");
    Hr = IDXGISwapChain_GetBuffer(Swap, 0, &IID_ID3D11Texture2D, (void **)&Back);
    printf("GetBuffer hr 0x%08lx\n", Hr);
    if (FAILED(Hr))
        return 2;
    Hr = ID3D11Device_CreateRenderTargetView(Dev, (ID3D11Resource *)Back, NULL, &Rtv);
    printf("CreateRenderTargetView hr 0x%08lx\n", Hr);
    if (FAILED(Hr))
        return 2;
    ID3D11Texture2D_GetDesc(Back, &Td);
    Td.Width = 1;
    Td.Height = 1;
    Td.MipLevels = 1;
    Td.ArraySize = 1;
    Td.Usage = D3D11_USAGE_STAGING;
    Td.BindFlags = 0;
    Td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Td.MiscFlags = 0;
    Hr = ID3D11Device_CreateTexture2D(Dev, &Td, NULL, &Staging);
    printf("staging texture hr 0x%08lx\n", Hr);

    for (Phase = 0; Phase < 4; Phase++) {
        DWORD T0 = GetTickCount(), Presents = 0, Bad = 0;
        HRESULT FirstBad = S_OK;
        int Frame;
        char Buf[64];

        sprintf_s(Buf, sizeof(Buf), "phase %s", Names[Phase]);
        Mark(Buf);
        for (Frame = 0; GetTickCount() - T0 < PhaseMs; Frame++) {
            ID3D11DeviceContext_OMSetRenderTargets(Ctx, 1, &Rtv, NULL);
            ID3D11DeviceContext_ClearRenderTargetView(Ctx, Rtv, Colours[Phase]);
            if (Frame == 0 && Staging != NULL) {
                D3D11_BOX Box = { 0, 0, 0, 1, 1, 1 };
                D3D11_MAPPED_SUBRESOURCE Map;
                ID3D11DeviceContext_CopySubresourceRegion(Ctx, (ID3D11Resource *)Staging, 0, 0, 0, 0,
                                                           (ID3D11Resource *)Back, 0, &Box);
                Hr = ID3D11DeviceContext_Map(Ctx, (ID3D11Resource *)Staging, 0, D3D11_MAP_READ, 0, &Map);
                if (SUCCEEDED(Hr)) {
                    printf("  readback (0,0) = 0x%08lx (BGRA)\n", *(ULONG *)Map.pData);
                    ID3D11DeviceContext_Unmap(Ctx, (ID3D11Resource *)Staging, 0);
                } else {
                    printf("  readback Map hr 0x%08lx\n", Hr);
                }
            }
            Hr = IDXGISwapChain_Present(Swap, 1, 0);
            if (SUCCEEDED(Hr)) {
                Presents++;
            } else {
                if (Bad++ == 0)
                    FirstBad = Hr;
            }
            Pump();
            Sleep(30);
        }
        printf("%s: %d frames, %lu presents ok, %lu failed (first 0x%08lx), %lu ms\n", Names[Phase], Frame, Presents,
               Bad, FirstBad, GetTickCount() - T0);
        if (Presents == 0)
            Failed = 1;
    }

    Mark("teardown");
    IDXGISwapChain_SetFullscreenState(Swap, FALSE, NULL);
    if (Staging != NULL)
        ID3D11Texture2D_Release(Staging);
    ID3D11RenderTargetView_Release(Rtv);
    ID3D11Texture2D_Release(Back);
    IDXGISwapChain_Release(Swap);
    ID3D11DeviceContext_Release(Ctx);
    ID3D11Device_Release(Dev);
    IDXGIAdapter1_Release(Adapter);
    IDXGIFactory1_Release(Factory);
    DestroyWindow(Wnd);
    printf("d3dfill: done, %s\n", Failed ? "a phase presented nothing" : "every phase presented");
    return Failed ? 2 : 0;
}
