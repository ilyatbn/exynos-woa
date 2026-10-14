// SPDX-License-Identifier: GPL-2.0
/*
 * d2dtext.exe - does Direct2D/DirectWrite text render on the Mali adapter? (notes/gpu-hw-path.md, 2026-10-14: in GPU
 * mode Settings, UAC and the taskbar clock draw their shapes but not their text / icon-font glyphs.)
 *
 * For the Mali adapter and for WARP (the reference), on an offscreen 640x160 BGRA D3D11 texture through a DXGI-surface
 * D2D render target: clear white, draw black text in three modes - grayscale antialiasing, ClearType, aliased - and
 * one Segoe Fluent Icons / MDL2 glyph line; read back and count "ink" pixels (any channel < 128) per band; write
 * d2dtext-<adapter>.bmp next to the exe. PASS = every band on Mali has ink and is within 50 % of WARP's count.
 *
 * Also a raw check of what D2D does underneath (glyph atlas = partial updates of a single-channel texture):
 * "atlas" - an A8_UNORM 256x256 texture, UpdateSubresource of a 16x16 block of 0xFF at (128, 64) with a box, then a
 * full-screen sample into BGRA; PASS = the block (and only the block) is dark in the readback.
 * Runs in session 0 (offscreen only). Exit 0 PASS, 2 a step failed, 4 wrong pixels. Build: build-d2dtext.cmd.
 */
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>

#define CHECK(hr, what)                                                                \
    do {                                                                               \
        HRESULT hr_ = (hr);                                                            \
        if (FAILED(hr_)) {                                                             \
            printf("  %s failed 0x%08lx\n", what, (unsigned long)hr_);                 \
            return -1;                                                                 \
        }                                                                              \
    } while (0)

static const UINT W = 640, H = 160;

static void SaveBmp(const char *Path, const BYTE *Px, UINT Pitch)
{
    BITMAPFILEHEADER F = {};
    BITMAPINFOHEADER I = {};
    FILE *f = fopen(Path, "wb");

    if (f == NULL)
        return;
    I.biSize = sizeof(I);
    I.biWidth = (LONG)W;
    I.biHeight = -(LONG)H;
    I.biPlanes = 1;
    I.biBitCount = 32;
    F.bfType = 0x4D42;
    F.bfOffBits = sizeof(F) + sizeof(I);
    F.bfSize = F.bfOffBits + W * H * 4;
    fwrite(&F, sizeof(F), 1, f);
    fwrite(&I, sizeof(I), 1, f);
    for (UINT y = 0; y < H; y++)
        fwrite(Px + (SIZE_T)y * Pitch, 4, W, f);
    fclose(f);
}

/* Ink pixels in the 4 horizontal bands (40 rows each) of the readback. */
static int TextRun(IDXGIAdapter *Adapter, D3D_DRIVER_TYPE Type, const char *Name, ULONG Ink[4])
{
    ID3D11Device *Dev = NULL;
    ID3D11DeviceContext *Ctx = NULL;
    ID3D11Texture2D *Rt = NULL, *St = NULL;
    IDXGISurface *Surf = NULL;
    ID2D1Factory *F = NULL;
    ID2D1RenderTarget *R = NULL;
    ID2D1SolidColorBrush *Black = NULL;
    IDWriteFactory *Dw = NULL;
    IDWriteTextFormat *Tf = NULL, *If = NULL;
    D3D_FEATURE_LEVEL Fl[2] = { D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 }, Got;
    D3D11_TEXTURE2D_DESC Td = {};
    D3D11_MAPPED_SUBRESOURCE M;
    D2D1_RENDER_TARGET_PROPERTIES P = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_HARDWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    static const wchar_t Text[] = L"12:34 Settings Hello WiFi";
    static const wchar_t Icons[] = L"\xE701 \xE702 \xE713 \xE767 \xE83F \xE8BE";   /* wifi, bt, settings, volume, battery, ... */
    char Bmp[64];

    printf("%s:\n", Name);
    CHECK(D3D11CreateDevice(Adapter, Type, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, Fl, 2, D3D11_SDK_VERSION, &Dev, &Got,
                            &Ctx), "D3D11CreateDevice");
    printf("  feature level 0x%x\n", Got);
    Td.Width = W;
    Td.Height = H;
    Td.MipLevels = Td.ArraySize = 1;
    Td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    Td.SampleDesc.Count = 1;
    Td.Usage = D3D11_USAGE_DEFAULT;
    Td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    CHECK(Dev->CreateTexture2D(&Td, NULL, &Rt), "CreateTexture2D(rt)");
    CHECK(Rt->QueryInterface(__uuidof(IDXGISurface), (void **)&Surf), "QI(IDXGISurface)");
    CHECK(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &F), "D2D1CreateFactory");
    CHECK(F->CreateDxgiSurfaceRenderTarget(Surf, &P, &R), "CreateDxgiSurfaceRenderTarget");
    CHECK(R->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &Black), "CreateSolidColorBrush");
    CHECK(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&Dw), "DWriteCreateFactory");
    CHECK(Dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                               DWRITE_FONT_STRETCH_NORMAL, 26.0f, L"en-us", &Tf), "CreateTextFormat(Segoe UI)");
    if (FAILED(Dw->CreateTextFormat(L"Segoe Fluent Icons", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, 26.0f, L"en-us", &If)))
        If = NULL;

    R->BeginDraw();
    R->Clear(D2D1::ColorF(1, 1, 1, 1));
    R->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    R->DrawText(Text, (UINT32)wcslen(Text), Tf, D2D1::RectF(8, 2, (float)W, 40), Black);
    R->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    R->DrawText(Text, (UINT32)wcslen(Text), Tf, D2D1::RectF(8, 42, (float)W, 80), Black);
    R->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_ALIASED);
    R->DrawText(Text, (UINT32)wcslen(Text), Tf, D2D1::RectF(8, 82, (float)W, 120), Black);
    R->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (If != NULL)
        R->DrawText(Icons, (UINT32)wcslen(Icons), If, D2D1::RectF(8, 122, (float)W, 160), Black);
    CHECK(R->EndDraw(), "EndDraw");

    Td.Usage = D3D11_USAGE_STAGING;
    Td.BindFlags = 0;
    Td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(Dev->CreateTexture2D(&Td, NULL, &St), "CreateTexture2D(staging)");
    Ctx->CopyResource(St, Rt);
    CHECK(Ctx->Map(St, 0, D3D11_MAP_READ, 0, &M), "Map");
    memset(Ink, 0, 4 * sizeof(ULONG));
    for (UINT y = 0; y < H; y++)
        for (UINT x = 0; x < W; x++) {
            const BYTE *p = (const BYTE *)M.pData + (SIZE_T)y * M.RowPitch + x * 4;
            if (p[0] < 128 || p[1] < 128 || p[2] < 128)
                Ink[y / 40]++;
        }
    sprintf_s(Bmp, sizeof(Bmp), "d2dtext-%s.bmp", Name);
    SaveBmp(Bmp, (const BYTE *)M.pData, M.RowPitch);
    Ctx->Unmap(St, 0);
    printf("  ink pixels: grayscale %lu, cleartype %lu, aliased %lu, icons %lu%s -> %s\n", Ink[0], Ink[1], Ink[2],
           Ink[3], If == NULL ? " (no icon font)" : "", Bmp);
    return 0;
}

/* ---- atlas: partial update of an A8 texture, sampled ---- */

static const char AtlasHlsl[] =
    "Texture2D<float4> t : register(t0);\n"
    "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 p = float2((id << 1) & 2, id & 2); return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
    "float4 ps(float4 pos : SV_Position) : SV_Target {\n"
    "  float a = t.Load(int3(pos.xy, 0)).a; return float4(1 - a, 1 - a, 1 - a, 1); }\n";

static int AtlasRun(IDXGIAdapter *Adapter, D3D_DRIVER_TYPE Type, const char *Name, DXGI_FORMAT Fmt, const char *FmtName)
{
    ID3D11Device *Dev = NULL;
    ID3D11DeviceContext *Ctx = NULL;
    ID3D11Texture2D *A = NULL, *Rt = NULL, *St = NULL;
    ID3D11ShaderResourceView *Srv = NULL;
    ID3D11RenderTargetView *Rtv = NULL;
    ID3DBlob *Vb = NULL, *Pb = NULL;
    ID3D11VertexShader *Vs = NULL;
    ID3D11PixelShader *Ps = NULL;
    D3D_FEATURE_LEVEL Fl[2] = { D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 }, Got;
    D3D11_TEXTURE2D_DESC Td = {};
    D3D11_MAPPED_SUBRESOURCE M;
    D3D11_BOX Box = { 128, 64, 0, 144, 80, 1 };
    D3D11_VIEWPORT Vp = { 0, 0, 256, 256, 0, 1 };
    BYTE Block[16 * 16];
    static BYTE Zero[256 * 256];
    ULONG InDark = 0, OutDark = 0;

    CHECK(D3D11CreateDevice(Adapter, Type, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, Fl, 2, D3D11_SDK_VERSION, &Dev, &Got,
                            &Ctx), "D3D11CreateDevice");
    Td.Width = Td.Height = 256;
    Td.MipLevels = Td.ArraySize = 1;
    Td.Format = Fmt;
    Td.SampleDesc.Count = 1;
    Td.Usage = D3D11_USAGE_DEFAULT;
    Td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA Init = { Zero, 256, 0 };
    CHECK(Dev->CreateTexture2D(&Td, &Init, &A), "CreateTexture2D(atlas)");
    memset(Block, 0xFF, sizeof(Block));
    Ctx->UpdateSubresource(A, 0, &Box, Block, 16, 0);
    CHECK(Dev->CreateShaderResourceView(A, NULL, &Srv), "CreateShaderResourceView");
    Td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    Td.BindFlags = D3D11_BIND_RENDER_TARGET;
    CHECK(Dev->CreateTexture2D(&Td, NULL, &Rt), "CreateTexture2D(rt)");
    CHECK(Dev->CreateRenderTargetView(Rt, NULL, &Rtv), "CreateRenderTargetView");
    CHECK(D3DCompile(AtlasHlsl, sizeof(AtlasHlsl) - 1, "atlas", NULL, NULL, "vs", "vs_4_0", 0, 0, &Vb, NULL), "compile vs");
    CHECK(D3DCompile(AtlasHlsl, sizeof(AtlasHlsl) - 1, "atlas", NULL, NULL, "ps", "ps_4_0", 0, 0, &Pb, NULL), "compile ps");
    CHECK(Dev->CreateVertexShader(Vb->GetBufferPointer(), Vb->GetBufferSize(), NULL, &Vs), "CreateVertexShader");
    CHECK(Dev->CreatePixelShader(Pb->GetBufferPointer(), Pb->GetBufferSize(), NULL, &Ps), "CreatePixelShader");
    Ctx->OMSetRenderTargets(1, &Rtv, NULL);
    Ctx->RSSetViewports(1, &Vp);
    Ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Ctx->IASetInputLayout(NULL);
    Ctx->VSSetShader(Vs, NULL, 0);
    Ctx->PSSetShader(Ps, NULL, 0);
    Ctx->PSSetShaderResources(0, 1, &Srv);
    Ctx->Draw(3, 0);
    Td.Usage = D3D11_USAGE_STAGING;
    Td.BindFlags = 0;
    Td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(Dev->CreateTexture2D(&Td, NULL, &St), "CreateTexture2D(staging)");
    Ctx->CopyResource(St, Rt);
    CHECK(Ctx->Map(St, 0, D3D11_MAP_READ, 0, &M), "Map");
    for (UINT y = 0; y < 256; y++)
        for (UINT x = 0; x < 256; x++) {
            BYTE g = ((const BYTE *)M.pData)[(SIZE_T)y * M.RowPitch + x * 4 + 1];
            BOOL In = x >= 128 && x < 144 && y >= 64 && y < 80;
            if (g < 128)
                (In ? InDark : OutDark)++;
        }
    Ctx->Unmap(St, 0);
    printf("%s atlas %s: block dark %lu/256, outside dark %lu -> %s\n", Name, FmtName, InDark, OutDark,
           InDark == 256 && OutDark == 0 ? "PASS" : "FAIL");
    return InDark == 256 && OutDark == 0 ? 0 : 4;
}

int main(void)
{
    IDXGIFactory1 *Fac = NULL;
    IDXGIAdapter1 *A = NULL, *Mali = NULL;
    ULONG InkM[4] = { 0 }, InkW[4] = { 0 };
    int Rc = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&Fac)))
        return 2;
    for (UINT i = 0; Fac->EnumAdapters1(i, &A) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 D;
        A->GetDesc1(&D);
        printf("adapter %u: %ls\n", i, D.Description);
        if (Mali == NULL && wcsstr(D.Description, L"Mali") != NULL)
            Mali = A;
    }
    if (TextRun(NULL, D3D_DRIVER_TYPE_WARP, "warp", InkW) != 0)
        return 2;
    if (Mali == NULL) {
        printf("no Mali adapter\n");
        return 1;
    }
    if (TextRun(Mali, D3D_DRIVER_TYPE_UNKNOWN, "mali", InkM) != 0)
        return 2;
    for (int b = 0; b < 4; b++) {
        BOOL Ok = InkM[b] != 0 && InkM[b] * 2 >= InkW[b] && InkM[b] <= InkW[b] * 2;
        printf("band %d: mali %lu vs warp %lu -> %s\n", b, InkM[b], InkW[b], Ok ? "ok" : "WRONG");
        if (!Ok)
            Rc = 4;
    }
    AtlasRun(NULL, D3D_DRIVER_TYPE_WARP, "warp", DXGI_FORMAT_A8_UNORM, "A8_UNORM");
    if (AtlasRun(Mali, D3D_DRIVER_TYPE_UNKNOWN, "mali", DXGI_FORMAT_A8_UNORM, "A8_UNORM") != 0)
        Rc = 4;
    printf("d2dtext: %s\n", Rc == 0 ? "PASS" : "FAIL");
    return Rc;
}
