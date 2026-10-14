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

#define BANDS 8
static const UINT W = 640, H = 40 * BANDS;
static const char *const BandName[BANDS] = { "text grayscale", "text cleartype", "text aliased", "icon font",
                                             "FillRectangle", "glyph outline FillGeometry", "A8 FillOpacityMask",
                                             "BGRA DrawBitmap" };

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

/* ---- D2DTEXT_HOOK=1: log what D2D asks of the D3D11 device (vtable patches on the Mali device only) ---- */

static void *OrigVtbl[2][64];
static BOOL Hooked;

static void Patch(void **Vtbl, int Index, void *Fn, void **Orig)
{
    DWORD Old;

    *Orig = Vtbl[Index];
    VirtualProtect(&Vtbl[Index], sizeof(void *), PAGE_READWRITE, &Old);
    Vtbl[Index] = Fn;
    VirtualProtect(&Vtbl[Index], sizeof(void *), Old, &Old);
}

static void Disasm(const char *Kind, const void *Code, SIZE_T Len)
{
    ID3DBlob *Text = NULL;

    printf("  [hook] %s, %zu bytes of DXBC\n", Kind, Len);
    if (SUCCEEDED(D3DDisassemble(Code, Len, D3D_DISASM_ENABLE_INSTRUCTION_NUMBERING, NULL, &Text))) {
        const char *S = (const char *)Text->GetBufferPointer();
        /* the instructions only (skip the comment header) */
        for (const char *L = S; *L != 0;) {
            const char *E = strchr(L, '\n');
            size_t N = E ? (size_t)(E - L) : strlen(L);
            if (N > 0 && L[0] != '/')
                printf("    %.*s\n", (int)N, L);
            L += N + (E ? 1 : 0);
        }
        Text->Release();
    }
}

typedef HRESULT(STDMETHODCALLTYPE *PFN_CVS)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, void **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CIL)(ID3D11Device *, const D3D11_INPUT_ELEMENT_DESC *, UINT, const void *, SIZE_T,
                                            ID3D11InputLayout **);
typedef void(STDMETHODCALLTYPE *PFN_DI)(ID3D11DeviceContext *, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE *PFN_D)(ID3D11DeviceContext *, UINT, UINT);
typedef void(STDMETHODCALLTYPE *PFN_DII)(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE *PFN_DIN)(ID3D11DeviceContext *, UINT, UINT, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *PFN_MAP)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP, UINT,
                                            D3D11_MAPPED_SUBRESOURCE *);
typedef void(STDMETHODCALLTYPE *PFN_TOPO)(ID3D11DeviceContext *, D3D11_PRIMITIVE_TOPOLOGY);
typedef void(STDMETHODCALLTYPE *PFN_VB)(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *, const UINT *, const UINT *);

static HRESULT STDMETHODCALLTYPE HkCVS(ID3D11Device *D, const void *C, SIZE_T L, ID3D11ClassLinkage *K, void **O)
{
    Disasm("CreateVertexShader", C, L);
    return ((PFN_CVS)OrigVtbl[0][12])(D, C, L, K, O);
}
static HRESULT STDMETHODCALLTYPE HkCPS(ID3D11Device *D, const void *C, SIZE_T L, ID3D11ClassLinkage *K, void **O)
{
    Disasm("CreatePixelShader", C, L);
    return ((PFN_CVS)OrigVtbl[0][15])(D, C, L, K, O);
}
static HRESULT STDMETHODCALLTYPE HkCGS(ID3D11Device *D, const void *C, SIZE_T L, ID3D11ClassLinkage *K, void **O)
{
    Disasm("CreateGeometryShader", C, L);
    return ((PFN_CVS)OrigVtbl[0][13])(D, C, L, K, O);
}
static HRESULT STDMETHODCALLTYPE HkCIL(ID3D11Device *D, const D3D11_INPUT_ELEMENT_DESC *E, UINT N, const void *C,
                                      SIZE_T L, ID3D11InputLayout **O)
{
    printf("  [hook] CreateInputLayout %u elements\n", N);
    for (UINT i = 0; i < N; i++)
        printf("    %s%u fmt %d slot %u off %u %s step %u\n", E[i].SemanticName, E[i].SemanticIndex, E[i].Format,
               E[i].InputSlot, E[i].AlignedByteOffset,
               E[i].InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA ? "INSTANCE" : "vertex", E[i].InstanceDataStepRate);
    return ((PFN_CIL)OrigVtbl[0][11])(D, E, N, C, L, O);
}
static void STDMETHODCALLTYPE HkDI(ID3D11DeviceContext *C, UINT N, UINT S, INT B)
{
    printf("  [hook] DrawIndexed %u start %u base %d\n", N, S, B);
    ((PFN_DI)OrigVtbl[1][12])(C, N, S, B);
}
static void STDMETHODCALLTYPE HkD(ID3D11DeviceContext *C, UINT N, UINT S)
{
    printf("  [hook] Draw %u start %u\n", N, S);
    ((PFN_D)OrigVtbl[1][13])(C, N, S);
}
static void STDMETHODCALLTYPE HkDII(ID3D11DeviceContext *C, UINT N, UINT I, UINT S, INT B, UINT SI)
{
    printf("  [hook] DrawIndexedInstanced %u x %u start %u base %d inst %u\n", N, I, S, B, SI);
    ((PFN_DII)OrigVtbl[1][20])(C, N, I, S, B, SI);
}
static void STDMETHODCALLTYPE HkDIN(ID3D11DeviceContext *C, UINT N, UINT I, UINT S, UINT SI)
{
    printf("  [hook] DrawInstanced %u x %u start %u inst %u\n", N, I, S, SI);
    ((PFN_DIN)OrigVtbl[1][21])(C, N, I, S, SI);
}
static HRESULT STDMETHODCALLTYPE HkMap(ID3D11DeviceContext *C, ID3D11Resource *R, UINT Sub, D3D11_MAP T, UINT F,
                                       D3D11_MAPPED_SUBRESOURCE *M)
{
    D3D11_RESOURCE_DIMENSION Dim;
    R->GetType(&Dim);
    printf("  [hook] Map dim %d sub %u type %d flags %u\n", Dim, Sub, T, F);
    return ((PFN_MAP)OrigVtbl[1][14])(C, R, Sub, T, F, M);
}
static void STDMETHODCALLTYPE HkTopo(ID3D11DeviceContext *C, D3D11_PRIMITIVE_TOPOLOGY T)
{
    printf("  [hook] Topology %d\n", T);
    ((PFN_TOPO)OrigVtbl[1][24])(C, T);
}
static void STDMETHODCALLTYPE HkVB(ID3D11DeviceContext *C, UINT S, UINT N, ID3D11Buffer *const *B, const UINT *St,
                                   const UINT *O)
{
    for (UINT i = 0; i < N; i++)
        printf("  [hook] IASetVertexBuffers slot %u stride %u offset %u%s\n", S + i, St ? St[i] : 0, O ? O[i] : 0,
               B && B[i] ? "" : " (null)");
    ((PFN_VB)OrigVtbl[1][18])(C, S, N, B, St, O);
}

static void HookDevice(ID3D11Device *Dev, ID3D11DeviceContext *Ctx)
{
    void **Dv = *(void ***)Dev, **Cv = *(void ***)Ctx;

    if (Hooked || GetEnvironmentVariableA("D2DTEXT_HOOK", NULL, 0) == 0)
        return;
    Hooked = TRUE;
    Patch(Dv, 11, (void *)HkCIL, &OrigVtbl[0][11]);
    Patch(Dv, 12, (void *)HkCVS, &OrigVtbl[0][12]);
    Patch(Dv, 13, (void *)HkCGS, &OrigVtbl[0][13]);
    Patch(Dv, 15, (void *)HkCPS, &OrigVtbl[0][15]);
    Patch(Cv, 12, (void *)HkDI, &OrigVtbl[1][12]);
    Patch(Cv, 13, (void *)HkD, &OrigVtbl[1][13]);
    Patch(Cv, 14, (void *)HkMap, &OrigVtbl[1][14]);
    Patch(Cv, 18, (void *)HkVB, &OrigVtbl[1][18]);
    Patch(Cv, 20, (void *)HkDII, &OrigVtbl[1][20]);
    Patch(Cv, 21, (void *)HkDIN, &OrigVtbl[1][21]);
    Patch(Cv, 24, (void *)HkTopo, &OrigVtbl[1][24]);
}

/* Ink pixels in the 4 horizontal bands (40 rows each) of the readback. */
static int TextRun(IDXGIAdapter *Adapter, D3D_DRIVER_TYPE Type, const char *Name, ULONG Ink[BANDS])
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
    if (Type != D3D_DRIVER_TYPE_WARP)
        HookDevice(Dev, Ctx);
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

    /* band 4: a plain filled rectangle (D2D's own geometry path) */
    R->FillRectangle(D2D1::RectF(8, 168, 200, 192), Black);

    /* band 5: the same text as glyph outlines, filled as geometry (no glyph atlas) */
    {
        IDWriteFontCollection *Fc = NULL;
        IDWriteFontFamily *Ff = NULL;
        IDWriteFont *Fn = NULL;
        IDWriteFontFace *Face = NULL;
        ID2D1PathGeometry *G = NULL;
        ID2D1GeometrySink *Sink = NULL;
        UINT32 Cp[64];
        UINT16 Gi[64];
        UINT32 N = (UINT32)wcslen(Text), Idx = 0;
        BOOL Exists = FALSE;

        for (UINT32 i = 0; i < N; i++)
            Cp[i] = Text[i];
        if (SUCCEEDED(Dw->GetSystemFontCollection(&Fc, FALSE)) && SUCCEEDED(Fc->FindFamilyName(L"Segoe UI", &Idx, &Exists)) &&
            Exists && SUCCEEDED(Fc->GetFontFamily(Idx, &Ff)) &&
            SUCCEEDED(Ff->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                               &Fn)) &&
            SUCCEEDED(Fn->CreateFontFace(&Face)) && SUCCEEDED(Face->GetGlyphIndices(Cp, N, Gi)) &&
            SUCCEEDED(F->CreatePathGeometry(&G)) && SUCCEEDED(G->Open(&Sink)) &&
            SUCCEEDED(Face->GetGlyphRunOutline(26.0f, Gi, NULL, NULL, N, FALSE, FALSE, Sink)) && SUCCEEDED(Sink->Close())) {
            R->SetTransform(D2D1::Matrix3x2F::Translation(8, 228));
            R->FillGeometry(G, Black);
            R->SetTransform(D2D1::Matrix3x2F::Identity());
        } else {
            printf("  glyph outline setup failed\n");
        }
    }

    /* band 6: an A8 opacity mask (what grayscale glyph runs become) */
    {
        static BYTE Mask[32 * 192];
        ID2D1Bitmap *Mb = NULL;
        ID2D1BitmapBrush *Bb = NULL;
        D2D1_BITMAP_PROPERTIES Bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

        for (UINT y = 0; y < 32; y++)
            for (UINT x = 0; x < 192; x++)
                Mask[y * 192 + x] = ((x / 8 + y / 8) & 1) ? 0xFF : 0x00;
        R->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        if (SUCCEEDED(R->CreateBitmap(D2D1::SizeU(192, 32), Mask, 192, &Bp, &Mb)) &&
            SUCCEEDED(R->CreateBitmapBrush(Mb, &Bb))) {
            Bb->SetTransform(D2D1::Matrix3x2F::Translation(8, 244));
            R->FillOpacityMask(Mb, Black, D2D1_OPACITY_MASK_CONTENT_TEXT_NATURAL, D2D1::RectF(8, 244, 200, 276),
                               D2D1::RectF(0, 0, 192, 32));
        } else {
            printf("  A8 bitmap setup failed\n");
        }
        R->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }

    /* band 7: a BGRA bitmap (plain texture sampling through D2D) */
    {
        static UINT Px[32 * 192];
        ID2D1Bitmap *Cb = NULL;
        D2D1_BITMAP_PROPERTIES Bp =
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

        for (UINT i = 0; i < 32 * 192; i++)
            Px[i] = ((i % 192) / 8 + (i / 192) / 8) & 1 ? 0xFF000000 : 0xFFFFFFFF;
        if (SUCCEEDED(R->CreateBitmap(D2D1::SizeU(192, 32), Px, 192 * 4, &Bp, &Cb)))
            R->DrawBitmap(Cb, D2D1::RectF(8, 284, 200, 316));
        else
            printf("  BGRA bitmap setup failed\n");
    }
    CHECK(R->EndDraw(), "EndDraw");

    Td.Usage = D3D11_USAGE_STAGING;
    Td.BindFlags = 0;
    Td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(Dev->CreateTexture2D(&Td, NULL, &St), "CreateTexture2D(staging)");
    Ctx->CopyResource(St, Rt);
    CHECK(Ctx->Map(St, 0, D3D11_MAP_READ, 0, &M), "Map");
    memset(Ink, 0, BANDS * sizeof(ULONG));
    for (UINT y = 0; y < H; y++)
        for (UINT x = 0; x < W; x++) {
            const BYTE *p = (const BYTE *)M.pData + (SIZE_T)y * M.RowPitch + x * 4;
            if (p[0] < 128 || p[1] < 128 || p[2] < 128)
                Ink[y / 40]++;
        }
    sprintf_s(Bmp, sizeof(Bmp), "d2dtext-%s.bmp", Name);
    SaveBmp(Bmp, (const BYTE *)M.pData, M.RowPitch);
    Ctx->Unmap(St, 0);
    printf("  readback -> %s%s\n", Bmp, If == NULL ? " (no icon font)" : "");
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

/* ---- feature probes: the D3D features D2D's shaders use (D2DTEXT_HOOK log, 2026-10-14) ---- */

struct Probe {
    const char *Name;
    const char *Hlsl;
    D3D11_INPUT_ELEMENT_DESC Ied[3];
    UINT NIed;
    const void *V0; UINT V0Size, V0Stride;      /* slot 0 (per vertex) */
    const void *V1; UINT V1Size, V1Stride;      /* slot 1 (per instance), or NULL */
    UINT Verts, Instances;
    const UINT *Buf; UINT BufN;                 /* t0 = Buffer<uint> R32_UINT, or NULL */
    ULONG ExpectMin, ExpectMax;                 /* dark pixels of 256x256 */
};

static int ProbeRun(ID3D11Device *Dev, ID3D11DeviceContext *Ctx, const Probe *P)
{
    ID3D11Texture2D *Rt = NULL, *St = NULL;
    ID3D11RenderTargetView *Rtv = NULL;
    ID3DBlob *Vb = NULL, *Pb = NULL, *Err = NULL;
    ID3D11VertexShader *Vs = NULL;
    ID3D11PixelShader *Ps = NULL;
    ID3D11InputLayout *Il = NULL;
    ID3D11Buffer *B0 = NULL, *B1 = NULL, *Tb = NULL;
    ID3D11ShaderResourceView *Srv = NULL;
    D3D11_TEXTURE2D_DESC Td = {};
    D3D11_BUFFER_DESC Bd = {};
    D3D11_SUBRESOURCE_DATA Init = {};
    D3D11_MAPPED_SUBRESOURCE M;
    D3D11_VIEWPORT Vp = { 0, 0, 256, 256, 0, 1 };
    static const float White[4] = { 1, 1, 1, 1 };
    ULONG Dark = 0;
    UINT Strides[2] = { P->V0Stride, P->V1Stride }, Offs[2] = { 0, 0 };

    Td.Width = Td.Height = 256;
    Td.MipLevels = Td.ArraySize = 1;
    Td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    Td.SampleDesc.Count = 1;
    Td.Usage = D3D11_USAGE_DEFAULT;
    Td.BindFlags = D3D11_BIND_RENDER_TARGET;
    CHECK(Dev->CreateTexture2D(&Td, NULL, &Rt), "probe rt");
    CHECK(Dev->CreateRenderTargetView(Rt, NULL, &Rtv), "probe rtv");
    if (FAILED(D3DCompile(P->Hlsl, strlen(P->Hlsl), P->Name, NULL, NULL, "vs", "vs_4_0", 0, 0, &Vb, &Err)) ||
        FAILED(D3DCompile(P->Hlsl, strlen(P->Hlsl), P->Name, NULL, NULL, "ps", "ps_4_0", 0, 0, &Pb, &Err))) {
        printf("  probe %s: compile: %s\n", P->Name, Err ? (const char *)Err->GetBufferPointer() : "?");
        return -1;
    }
    CHECK(Dev->CreateVertexShader(Vb->GetBufferPointer(), Vb->GetBufferSize(), NULL, &Vs), "probe vs");
    CHECK(Dev->CreatePixelShader(Pb->GetBufferPointer(), Pb->GetBufferSize(), NULL, &Ps), "probe ps");
    if (P->NIed != 0)
        CHECK(Dev->CreateInputLayout(P->Ied, P->NIed, Vb->GetBufferPointer(), Vb->GetBufferSize(), &Il), "probe il");
    Bd.Usage = D3D11_USAGE_DEFAULT;
    Bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    if (P->V0 != NULL) {
        Bd.ByteWidth = P->V0Size;
        Init.pSysMem = P->V0;
        CHECK(Dev->CreateBuffer(&Bd, &Init, &B0), "probe vb0");
    }
    if (P->V1 != NULL) {
        Bd.ByteWidth = P->V1Size;
        Init.pSysMem = P->V1;
        CHECK(Dev->CreateBuffer(&Bd, &Init, &B1), "probe vb1");
    }
    if (P->Buf != NULL) {
        D3D11_SHADER_RESOURCE_VIEW_DESC Sd = {};
        Bd.ByteWidth = P->BufN * 4;
        Bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        Init.pSysMem = P->Buf;
        CHECK(Dev->CreateBuffer(&Bd, &Init, &Tb), "probe tbo");
        Sd.Format = DXGI_FORMAT_R32_UINT;
        Sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        Sd.Buffer.FirstElement = 0;
        Sd.Buffer.NumElements = P->BufN;
        CHECK(Dev->CreateShaderResourceView(Tb, &Sd, &Srv), "probe tbo srv");
    }
    Ctx->ClearRenderTargetView(Rtv, White);
    Ctx->OMSetRenderTargets(1, &Rtv, NULL);
    Ctx->RSSetViewports(1, &Vp);
    Ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    Ctx->IASetInputLayout(Il);
    {
        ID3D11Buffer *Bs[2] = { B0, B1 };
        Ctx->IASetVertexBuffers(0, 2, Bs, Strides, Offs);
    }
    Ctx->VSSetShader(Vs, NULL, 0);
    Ctx->PSSetShader(Ps, NULL, 0);
    Ctx->PSSetShaderResources(0, 1, &Srv);
    if (P->Instances != 0)
        Ctx->DrawInstanced(P->Verts, P->Instances, 0, 0);
    else
        Ctx->Draw(P->Verts, 0);
    Td.Usage = D3D11_USAGE_STAGING;
    Td.BindFlags = 0;
    Td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(Dev->CreateTexture2D(&Td, NULL, &St), "probe staging");
    Ctx->CopyResource(St, Rt);
    CHECK(Ctx->Map(St, 0, D3D11_MAP_READ, 0, &M), "probe map");
    for (UINT y = 0; y < 256; y++)
        for (UINT x = 0; x < 256; x++)
            if (((const BYTE *)M.pData)[(SIZE_T)y * M.RowPitch + x * 4 + 1] < 128)
                Dark++;
    Ctx->Unmap(St, 0);
    BOOL Ok = Dark >= P->ExpectMin && Dark <= P->ExpectMax;
    printf("  probe %-26s dark %6lu (expect %lu..%lu) -> %s\n", P->Name, Dark, P->ExpectMin, P->ExpectMax,
           Ok ? "ok" : "WRONG");
    return Ok ? 0 : 4;
}

static int Probes(IDXGIAdapter *Adapter, D3D_DRIVER_TYPE Type, const char *Name)
{
    ID3D11Device *Dev = NULL;
    ID3D11DeviceContext *Ctx = NULL;
    D3D_FEATURE_LEVEL Fl[2] = { D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 }, Got;
    int Rc = 0;
    /* full-target strip in floats and in shorts */
    static const float QuadF[8] = { -1, -1, -1, 1, 1, -1, 1, 1 };
    static const short QuadS[8] = { -1, -1, -1, 1, 1, -1, 1, 1 };
    /* a unit quad (0..1) plus per-instance offsets: two quadrants (bottom-left, top-right) */
    static const float Unit[8] = { 0, 0, 0, 1, 1, 0, 1, 1 };
    static const float InstF[4] = { -1, -1, 0, 0 };
    static const short InstS[4] = { -1, -1, 0, 0 };
    static const short InstS8[8] = { -1, -1, 0, 0, 0, 0, 0, 0 };   /* stride 8: instance 1 at +8 */
    static const int InstI[4] = { -1, -1, 0, 0 };
    static const unsigned short InstH[4] = { 0xBC00, 0xBC00, 0, 0 };   /* half -1.0, -1.0 / 0, 0 */
    static const UINT Bits[4] = { 1, 0, 1, 0 };
    /* position float2 + RGBA8 colour, 12-byte stride: white, and green = 64/255 = 0.25 */
    struct PC { float X, Y; UINT C; };
    static const PC QuadC[4] = { { -1, -1, 0xFFFFFFFF }, { -1, 1, 0xFFFFFFFF }, { 1, -1, 0xFFFFFFFF }, { 1, 1, 0xFFFFFFFF } };
    static const PC QuadG[4] = { { -1, -1, 0xFF404040 }, { -1, 1, 0xFF404040 }, { 1, -1, 0xFF404040 }, { 1, 1, 0xFF404040 } };
    static const Probe Ps[] = {
        { "float2 position", "float4 vs(float2 p : POSITION) : SV_Position { return float4(p, 0, 1); }\n"
                             "float4 ps() : SV_Target { return float4(0, 0, 0, 1); }\n",
          { { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 } }, 1,
          QuadF, sizeof(QuadF), 8, NULL, 0, 0, 4, 0, NULL, 0, 65536, 65536 },
        { "R16G16_SINT position", "float4 vs(int2 p : POSITION) : SV_Position { return float4(float2(p), 0, 1); }\n"
                                  "float4 ps() : SV_Target { return float4(0, 0, 0, 1); }\n",
          { { "POSITION", 0, DXGI_FORMAT_R16G16_SINT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 } }, 1,
          QuadS, sizeof(QuadS), 4, NULL, 0, 0, 4, 0, NULL, 0, 65536, 65536 },
        { "instanced float2 offset",
          "float4 vs(float2 p : POSITION, float2 o : TEXCOORD0) : SV_Position { return float4(p + o, 0, 1); }\n"
          "float4 ps() : SV_Target { return float4(0, 0, 0, 1); }\n",
          { { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 } }, 2,
          Unit, sizeof(Unit), 8, InstF, sizeof(InstF), 8, 4, 2, NULL, 0, 32768, 32768 },
        { "instanced R16G16_SINT offset",
          "float4 vs(float2 p : POSITION, int2 o : TEXCOORD0) : SV_Position { return float4(p + float2(o), 0, 1); }\n"
          "float4 ps() : SV_Target { return float4(0, 0, 0, 1); }\n",
          { { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R16G16_SINT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 } }, 2,
          Unit, sizeof(Unit), 8, InstS, sizeof(InstS), 4, 4, 2, NULL, 0, 32768, 32768 },
        { "inst R16G16_SINT stride 8",
          "float4 vs(float2 p : POSITION, int2 o : TEXCOORD0) : SV_Position { return float4(p + float2(o), 0, 1); }\n"
          "float4 ps() : SV_Target { return float4(0, 0, 0, 1); }\n",
          { { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R16G16_SINT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 } }, 2,
          Unit, sizeof(Unit), 8, InstS8, sizeof(InstS8), 8, 4, 2, NULL, 0, 32768, 32768 },
        { "inst R32G32_SINT stride 8",
          "float4 vs(float2 p : POSITION, int2 o : TEXCOORD0) : SV_Position { return float4(p + float2(o), 0, 1); }\n"
          "float4 ps() : SV_Target { return float4(0, 0, 0, 1); }\n",
          { { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_SINT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 } }, 2,
          Unit, sizeof(Unit), 8, InstI, sizeof(InstI), 8, 4, 2, NULL, 0, 32768, 32768 },
        { "inst R16G16_FLOAT stride 4",
          "float4 vs(float2 p : POSITION, float2 o : TEXCOORD0) : SV_Position { return float4(p + o, 0, 1); }\n"
          "float4 ps() : SV_Target { return float4(0, 0, 0, 1); }\n",
          { { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 } }, 2,
          Unit, sizeof(Unit), 8, InstH, sizeof(InstH), 4, 4, 2, NULL, 0, 32768, 32768 },
        { "R8G8B8A8_UNORM colour (white)",
          "struct V { float4 p : SV_Position; float4 c : COLOR; };\n"
          "V vs(float2 p : POSITION, float4 c : COLOR) { V o; o.p = float4(p, 0, 1); o.c = c; return o; }\n"
          "float4 ps(V i) : SV_Target { return i.c; }\n",
          { { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 } }, 2,
          QuadC, sizeof(QuadC), 12, NULL, 0, 0, 4, 0, NULL, 0, 0, 0 },
        { "R8G8B8A8_UNORM colour (grey)",
          "struct V { float4 p : SV_Position; float4 c : COLOR; };\n"
          "V vs(float2 p : POSITION, float4 c : COLOR) { V o; o.p = float4(p, 0, 1); o.c = c; return o; }\n"
          "float4 ps(V i) : SV_Target { return i.c.g > 0.2 && i.c.g < 0.3 ? float4(0, 0, 0, 1) : float4(1, 1, 1, 1); }\n",
          { { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 } }, 2,
          QuadG, sizeof(QuadG), 12, NULL, 0, 0, 4, 0, NULL, 0, 65536, 65536 },
        { "Buffer<uint> ld in PS",
          "Buffer<uint> t : register(t0);\n"
          "float4 vs(float2 p : POSITION) : SV_Position { return float4(p, 0, 1); }\n"
          "float4 ps(float4 pos : SV_Position) : SV_Target {\n"
          "  uint b = t.Load((int)pos.x / 64); return b ? float4(0, 0, 0, 1) : float4(1, 1, 1, 1); }\n",
          { { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 } }, 1,
          QuadF, sizeof(QuadF), 8, NULL, 0, 0, 4, 0, Bits, 4, 32768, 32768 },
    };

    printf("%s probes:\n", Name);
    CHECK(D3D11CreateDevice(Adapter, Type, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, Fl, 2, D3D11_SDK_VERSION, &Dev, &Got,
                            &Ctx), "D3D11CreateDevice");
    {
        char Path[MAX_PATH] = "(not loaded)";
        HMODULE Umd = GetModuleHandleA("maliumd.dll");
        if (Umd != NULL)
            GetModuleFileNameA(Umd, Path, MAX_PATH);
        printf("  UMD: %s\n", Path);
    }
    for (size_t i = 0; i < sizeof(Ps) / sizeof(Ps[0]); i++)
        if (ProbeRun(Dev, Ctx, &Ps[i]) != 0)
            Rc = 4;
    return Rc;
}

/* Mesa's d3d10umd reports unsupported DDIs / states with OutputDebugStringA (Debug.cpp DebugPrintf): catch the
 * debug-print exceptions in-process and print them, deduplicated (no debugger on the phone). */
static char Seen[64][160];
static int NSeen;

static LONG CALLBACK DbgPrintVeh(EXCEPTION_POINTERS *E)
{
    const EXCEPTION_RECORD *R = E->ExceptionRecord;
    char Line[160];

    if (R->ExceptionCode == 0x4001000A && R->NumberParameters >= 2)   /* DBG_PRINTEXCEPTION_WIDE_C */
        WideCharToMultiByte(CP_ACP, 0, (const wchar_t *)R->ExceptionInformation[1], -1, Line, sizeof(Line), NULL, NULL);
    else if (R->ExceptionCode == 0x40010006 && R->NumberParameters >= 2)   /* DBG_PRINTEXCEPTION_C */
        strncpy_s(Line, sizeof(Line), (const char *)R->ExceptionInformation[1], _TRUNCATE);
    else
        return EXCEPTION_CONTINUE_SEARCH;
    for (int i = 0; i < NSeen; i++)
        if (strcmp(Seen[i], Line) == 0)
            return EXCEPTION_CONTINUE_SEARCH;
    if (NSeen < 64)
        strcpy_s(Seen[NSeen++], sizeof(Seen[0]), Line);
    printf("  [debug] %s%s", Line, strchr(Line, '\n') ? "" : "\n");
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(void)
{
    AddVectoredExceptionHandler(1, DbgPrintVeh);
    IDXGIFactory1 *Fac = NULL;
    IDXGIAdapter1 *A = NULL, *Mali = NULL;
    ULONG InkM[BANDS] = { 0 }, InkW[BANDS] = { 0 };
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
    for (int b = 0; b < BANDS; b++) {
        BOOL Ok = InkM[b] != 0 && InkM[b] * 2 >= InkW[b] && InkM[b] <= InkW[b] * 2;
        printf("band %d %-28s mali %6lu vs warp %6lu -> %s\n", b, BandName[b], InkM[b], InkW[b], Ok ? "ok" : "WRONG");
        if (!Ok)
            Rc = 4;
    }
    Probes(NULL, D3D_DRIVER_TYPE_WARP, "warp");
    if (Probes(Mali, D3D_DRIVER_TYPE_UNKNOWN, "mali") != 0)
        Rc = 4;
    AtlasRun(NULL, D3D_DRIVER_TYPE_WARP, "warp", DXGI_FORMAT_A8_UNORM, "A8_UNORM");
    if (AtlasRun(Mali, D3D_DRIVER_TYPE_UNKNOWN, "mali", DXGI_FORMAT_A8_UNORM, "A8_UNORM") != 0)
        Rc = 4;
    printf("d2dtext: %s\n", Rc == 0 ? "PASS" : "FAIL");
    return Rc;
}
