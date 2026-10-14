// SPDX-License-Identifier: GPL-2.0
/*
 * overlay.cpp - the cube's on-screen counters (2026-10-14, user: "add counters on it? fps, gpu_model, gpu_clock,
 * gpu_temp"): Direct2D/DirectWrite text drawn into the swap chain's back buffer after the cube, before Present.
 * Text through D2D is itself a test of Mesa 0064 (TGSI texel buffers + untyped VS inputs).
 *
 * GPU clock: the KMD's MW_ESCAPE_CLOCK (cycle counter + QPC, KMD >= 1.0.39.1) sampled about once a second; the
 * rate is (dcycles / dQPC). GPU temperature: no sensor under Windows yet (notes/thermal-research.md) -> "n/a".
 */
#include <windows.h>
#include <winternl.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d2d1.h>
#include <dwrite.h>
#pragma warning(push)
#pragma warning(disable : 4201)
#include <d3dkmthk.h>
#pragma warning(pop)
#include <stdio.h>
#include "maliwddm_abi.h"

static ID2D1Factory *F;
static ID2D1RenderTarget *R;
static ID2D1SolidColorBrush *Fg, *Bg;
static IDWriteFactory *Dw;
static IDWriteTextFormat *Tf;
static wchar_t Model[128], Api[128];
static float BufW = 540.0f, FontPx = 17.0f;   /* back-buffer width (the cube is DPI-unaware: 540 at 200 %) */
static D3DKMT_HANDLE Kmt;
static MW_ESCAPE_CLOCK_DATA Last;
static double ClockMhz = -1.0;

static BOOL ReadClock(MW_ESCAPE_CLOCK_DATA *C)
{
    D3DKMT_ESCAPE E = {};

    if (Kmt == 0)
        return FALSE;
    C->Code = MW_ESCAPE_CLOCK;
    E.hAdapter = Kmt;
    E.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    E.pPrivateDriverData = C;
    E.PrivateDriverDataSize = sizeof(*C);
    return D3DKMTEscape(&E) >= 0 && C->Valid != 0;
}

/* 0 on success; the cube runs without the overlay otherwise. */
extern "C" int OverlayInit(IDXGISwapChain *Sc, IDXGIAdapter *Adapter)
{
    IDXGISurface *Surf = NULL;
    DXGI_ADAPTER_DESC Ad;
    D2D1_RENDER_TARGET_PROPERTIES P = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

    if (FAILED(Adapter->GetDesc(&Ad)))
        return 1;
    wcsncpy_s(Model, 128, Ad.Description, _TRUNCATE);
    {
        D3DKMT_OPENADAPTERFROMLUID O = {};
        O.AdapterLuid = Ad.AdapterLuid;
        if (wcsstr(Ad.Description, L"Mali") != NULL && D3DKMTOpenAdapterFromLuid(&O) >= 0)
            Kmt = O.hAdapter;
    }
    ReadClock(&Last);
    {
        ID3D11Device *Dev = NULL;
        DXGI_SWAP_CHAIN_DESC Scd;
        D3D_FEATURE_LEVEL Fl = (D3D_FEATURE_LEVEL)0;

        if (SUCCEEDED(Sc->GetDevice(__uuidof(ID3D11Device), (void **)&Dev))) {
            Fl = Dev->GetFeatureLevel();
            Dev->Release();
        }
        Sc->GetDesc(&Scd);
        BufW = (float)Scd.BufferDesc.Width;
        FontPx = BufW / 32.0f;
        swprintf_s(Api, 128, L"Direct3D 11 (feature level %u_%u), %s swap chain", (unsigned)(Fl >> 12),
                   (unsigned)((Fl >> 8) & 0xF),
                   Scd.SwapEffect == DXGI_SWAP_EFFECT_DISCARD ? L"blt-model" : L"flip-model");
    }
    if (FAILED(Sc->GetBuffer(0, __uuidof(IDXGISurface), (void **)&Surf)))
        return 2;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &F)) ||
        FAILED(F->CreateDxgiSurfaceRenderTarget(Surf, &P, &R)) ||
        FAILED(R->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &Fg)) ||
        FAILED(R->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.55f), &Bg)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&Dw)) ||
        FAILED(Dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, FontPx, L"en-us", &Tf))) {
        Surf->Release();
        return 3;
    }
    Surf->Release();
    return 0;
}

/* Once a second: the GPU clock over the last interval. */
extern "C" void OverlayTick(void)
{
    MW_ESCAPE_CLOCK_DATA C = {};

    if (ReadClock(&C) && Last.Valid && C.Qpc > Last.Qpc && C.Cycles >= Last.Cycles)
        ClockMhz = (double)(C.Cycles - Last.Cycles) * (double)C.QpcFreq / (double)(C.Qpc - Last.Qpc) / 1e6;
    if (C.Valid)
        Last = C;
}

extern "C" void OverlayDraw(double Fps)
{
    wchar_t T[512], Clk[32];

    if (R == NULL)
        return;
    if (ClockMhz >= 0)
        swprintf_s(Clk, 32, L"%.0f MHz", ClockMhz);
    else
        wcscpy_s(Clk, 32, Kmt ? L"n/a (KMD < 1.0.39)" : L"n/a");
    swprintf_s(T, 512, L"%s\n%s\nFPS  %.1f\nGPU clock  %s\nGPU temp  n/a (no sensor driver yet)", Api, Model, Fps,
               Clk);
    R->BeginDraw();
    R->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(8, 8, BufW - 8, 8 + FontPx * 7.6f), 8, 8), Bg);
    R->DrawText(T, (UINT32)wcslen(T), Tf, D2D1::RectF(16, 12, BufW - 16, 8 + FontPx * 7.4f), Fg);
    if (FAILED(R->EndDraw())) {
        R->Release();       /* D2DERR_RECREATE_TARGET etc.: drop the overlay rather than the cube */
        R = NULL;
    }
}
