// SPDX-License-Identifier: GPL-2.0
/*
 * d3dtri.exe - D3D11 rendering tests on the Mali adapter (M16 hardware path, notes/gpu-hw-path.md H2/H4/H5).
 *
 *   d3dtri.exe tri               offscreen: clear a 64x64 RGBA8 target blue, draw one red triangle over the centre,
 *                                read it back; PASS = centre red and corner blue (the check of wddm-mali-g52
 *                                tests/d3d11-triangle/triangle.cpp, which passed on a G52 through WDDM in WinPE).
 *   d3dtri.exe cube <seconds>    a spinning, depth-tested, vertex-coloured cube (vertex + index buffer, per-frame
 *                                constant buffer, D24S8 depth) in a window covering the primary monitor, presented
 *                                every frame; prints frames per second each second and the total. A readback of the
 *                                centre pixel after the first frame checks that something was drawn.
 *   d3dtri.exe gdi <seconds>     no D3D: a GDI window with a square moving every 33 ms - a pure DWM composition load
 *                                (prints paints per second; what reaches the panel is DWM's rate).
 *   d3dtri.exe tex               offscreen: a 2x2 texture (red, green / blue, white) on a full-target triangle,
 *                                left half Texture2D.Sample (TGSI SAMPLE), right half .Load (SAMPLE_I), both
 *                                behind GetDimensions == 2x2 (SVIEWINFO); PASS = the four quadrant colours.
 *
 * The renderer is whatever the adapter's UMD picks: with Mesa 0020 Panfrost when the KMD's GPU half is up, unless
 * GALLIUM_DRIVER overrides it (set GALLIUM_DRIVER=panfrost / softpipe to choose). Each step is logged before it
 * runs with its HRESULT; a watchdog thread ends the process with "TIMEOUT in <step>" (a GPU hang must not hang the
 * harness). Must run in the interactive session for "cube" (run-as-user.ps1); "tri" also works in session 0.
 * D3DTRI_FLIP=1 makes the swap chain flip model (FLIP_SEQUENTIAL) instead of DISCARD.
 * D3DTRI_ANY=1 takes the first adapter instead (testing the program itself on a PC).
 * Exit: 0 PASS, 1 no Mali adapter, 2 a step failed, 3 timeout, 4 wrong pixels. Build: build-d3dtri.cmd.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wchar.h>
#pragma warning(push)
#pragma warning(disable : 4201)     /* nameless unions in the SDK headers (C mode) */
#include <dxgi.h>
#include <d3d11.h>
#include <d3dcompiler.h>
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
    printf("d3dtri: TIMEOUT in %s\n", (const char *)Step);
    fflush(stdout);
    ExitProcess(3);
}

#define CHECK(hr, what)                                                                  \
    do {                                                                                 \
        HRESULT hr_ = (hr);                                                              \
        printf("%s hr 0x%08lx\n", what, (unsigned long)hr_);                             \
        fflush(stdout);                                                                  \
        if (FAILED(hr_))                                                                 \
            return 2;                                                                    \
    } while (0)

static IDXGIAdapter1 *FindMali(void)
{
    IDXGIFactory1 *Fac = NULL;
    IDXGIAdapter1 *A = NULL, *Pick = NULL;
    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&Fac);

    printf("CreateDXGIFactory1 hr 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr))
        return NULL;
    for (UINT i = 0; IDXGIFactory1_EnumAdapters1(Fac, i, &A) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 D;
        IDXGIAdapter1_GetDesc1(A, &D);
        printf("adapter %u: %ls (vendor 0x%04x device 0x%04x, luid %08lx:%08lx, flags 0x%x)\n", i, D.Description,
               D.VendorId, D.DeviceId, (unsigned long)D.AdapterLuid.HighPart, (unsigned long)D.AdapterLuid.LowPart,
               D.Flags);
        if (Pick == NULL && (wcsstr(D.Description, L"Mali") != NULL || GetEnvironmentVariableA("D3DTRI_ANY", NULL, 0) != 0))
            Pick = A;
        else
            IDXGIAdapter1_Release(A);
    }
    IDXGIFactory1_Release(Fac);
    return Pick;
}

static HRESULT Compile(const char *Src, const char *Entry, const char *Target, ID3DBlob **Out)
{
    ID3DBlob *Err = NULL;
    HRESULT hr = D3DCompile(Src, strlen(Src), "d3dtri", NULL, NULL, Entry, Target, 0, 0, Out, &Err);

    if (FAILED(hr))
        printf("compile %s: %s\n", Entry, Err != NULL ? (const char *)ID3D10Blob_GetBufferPointer(Err) : "?");
    if (Err != NULL)
        ID3D10Blob_Release(Err);
    return hr;
}

/* ---- tri: offscreen pixel check ---- */

static const char TriHlsl[] =
    "float4 vs(float2 p : POSITION) : SV_Position { return float4(p, 0.0, 1.0); }\n"
    "float4 ps() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }\n";

static int Tri(ID3D11Device *Dev, ID3D11DeviceContext *Ctx)
{
    D3D11_TEXTURE2D_DESC Td = { 0 };
    ID3D11Texture2D *Rt = NULL, *St = NULL;
    ID3D11RenderTargetView *Rtv = NULL;
    ID3DBlob *Vsb = NULL, *Psb = NULL;
    ID3D11VertexShader *Vs = NULL;
    ID3D11PixelShader *Ps = NULL;
    ID3D11Buffer *Vb = NULL;
    ID3D11InputLayout *Il = NULL;
    ID3D11RasterizerState *Rs = NULL;
    static const float Verts[] = { 0.0f, 0.8f, 0.8f, -0.8f, -0.8f, -0.8f };
    D3D11_BUFFER_DESC Bd = { 0 };
    D3D11_SUBRESOURCE_DATA Init = { Verts, 0, 0 };
    D3D11_INPUT_ELEMENT_DESC Ied = { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    D3D11_RASTERIZER_DESC Rd = { 0 };
    D3D11_VIEWPORT Vp = { 0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f };
    D3D11_MAPPED_SUBRESOURCE M;
    static const float Blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    UINT Stride = 8, Offset = 0, Centre, Corner, Red = 0, Bl = 0, Other = 0;

    Td.Width = Td.Height = 64;
    Td.MipLevels = Td.ArraySize = 1;
    Td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    Td.SampleDesc.Count = 1;
    Td.Usage = D3D11_USAGE_DEFAULT;
    Td.BindFlags = D3D11_BIND_RENDER_TARGET;
    Mark("render target");
    CHECK(ID3D11Device_CreateTexture2D(Dev, &Td, NULL, &Rt), "CreateTexture2D(rt)");
    CHECK(ID3D11Device_CreateRenderTargetView(Dev, (ID3D11Resource *)Rt, NULL, &Rtv), "CreateRenderTargetView");
    Mark("shaders");
    CHECK(Compile(TriHlsl, "vs", "vs_4_0", &Vsb), "D3DCompile(vs)");
    CHECK(Compile(TriHlsl, "ps", "ps_4_0", &Psb), "D3DCompile(ps)");
    CHECK(ID3D11Device_CreateVertexShader(Dev, ID3D10Blob_GetBufferPointer(Vsb), ID3D10Blob_GetBufferSize(Vsb), NULL,
                                          &Vs), "CreateVertexShader");
    CHECK(ID3D11Device_CreatePixelShader(Dev, ID3D10Blob_GetBufferPointer(Psb), ID3D10Blob_GetBufferSize(Psb), NULL,
                                         &Ps), "CreatePixelShader");
    Mark("vertex buffer");
    Bd.ByteWidth = sizeof(Verts);
    Bd.Usage = D3D11_USAGE_DEFAULT;
    Bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    CHECK(ID3D11Device_CreateBuffer(Dev, &Bd, &Init, &Vb), "CreateBuffer");
    CHECK(ID3D11Device_CreateInputLayout(Dev, &Ied, 1, ID3D10Blob_GetBufferPointer(Vsb), ID3D10Blob_GetBufferSize(Vsb),
                                         &Il), "CreateInputLayout");
    Rd.FillMode = D3D11_FILL_SOLID;
    Rd.CullMode = D3D11_CULL_NONE;
    Rd.DepthClipEnable = TRUE;
    CHECK(ID3D11Device_CreateRasterizerState(Dev, &Rd, &Rs), "CreateRasterizerState");

    Mark("draw");
    ID3D11DeviceContext_ClearRenderTargetView(Ctx, Rtv, Blue);
    ID3D11DeviceContext_OMSetRenderTargets(Ctx, 1, &Rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(Ctx, 1, &Vp);
    ID3D11DeviceContext_RSSetState(Ctx, Rs);
    ID3D11DeviceContext_IASetVertexBuffers(Ctx, 0, 1, &Vb, &Stride, &Offset);
    ID3D11DeviceContext_IASetInputLayout(Ctx, Il);
    ID3D11DeviceContext_IASetPrimitiveTopology(Ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(Ctx, Vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(Ctx, Ps, NULL, 0);
    ID3D11DeviceContext_Draw(Ctx, 3, 0);

    Mark("readback");
    Td.Usage = D3D11_USAGE_STAGING;
    Td.BindFlags = 0;
    Td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(ID3D11Device_CreateTexture2D(Dev, &Td, NULL, &St), "CreateTexture2D(staging)");
    ID3D11DeviceContext_CopyResource(Ctx, (ID3D11Resource *)St, (ID3D11Resource *)Rt);
    CHECK(ID3D11DeviceContext_Map(Ctx, (ID3D11Resource *)St, 0, D3D11_MAP_READ, 0, &M), "Map");
#define PX(x, y) (*(const UINT *)((const BYTE *)M.pData + (y) * M.RowPitch + (x) * 4))
    for (int y = 0; y < 64; y += 4) {
        char Line[33];
        for (int x = 0; x < 64; x += 2) {
            UINT C = PX(x, y);
            Line[x / 2] = C == 0xff0000ffu ? '#' : C == 0xffff0000u ? '.' : '?';
        }
        Line[32] = 0;
        printf("  %s\n", Line);
    }
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            UINT C = PX(x, y);
            if (C == 0xff0000ffu)
                Red++;
            else if (C == 0xffff0000u)
                Bl++;
            else
                Other++;
        }
    Centre = PX(32, 32);
    Corner = PX(1, 1);
#undef PX
    ID3D11DeviceContext_Unmap(Ctx, (ID3D11Resource *)St, 0);
    printf("pixels: red %u, blue %u, other %u\n", Red, Bl, Other);
    printf("centre=0x%08x (want 0xff0000ff) corner=0x%08x (want 0xffff0000) -> %s\n", Centre, Corner,
           (Centre == 0xff0000ffu && Corner == 0xffff0000u) ? "PASS" : "FAIL");
    return (Centre == 0xff0000ffu && Corner == 0xffff0000u) ? 0 : 4;
}

/* ---- tex: D3D10 sampling (SAMPLE, SAMPLE_I, SVIEWINFO) ---- */

static const char TexHlsl[] =
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) { V o; float2 t = float2((id << 1) & 2, id & 2);"
    " o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = t; return o; }\n"
    "Texture2D tx : register(t0); SamplerState sm : register(s0);\n"
    "float4 ps(V i) : SV_Target { uint w, h; tx.GetDimensions(w, h);"
    " if (w != 2 || h != 2) return float4(1, 0, 1, 1);"
    " if (i.p.x < 32) return tx.Sample(sm, i.uv);"
    " return tx.Load(int3(int2(i.uv * 2), 0)); }\n";

static int Tex(ID3D11Device *Dev, ID3D11DeviceContext *Ctx)
{
    static const UINT Texels[4] = { 0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffffffffu };
    static const struct { int X, Y; UINT Want; const char *Name; } Probe[4] = {
        { 16, 16, 0xff0000ffu, "top-left red (Sample)" }, { 48, 16, 0xff00ff00u, "top-right green (Load)" },
        { 16, 48, 0xffff0000u, "bottom-left blue (Sample)" }, { 48, 48, 0xffffffffu, "bottom-right white (Load)" },
    };
    D3D11_TEXTURE2D_DESC Td = { 0 };
    D3D11_SUBRESOURCE_DATA Init = { Texels, 8, 0 };
    D3D11_SAMPLER_DESC Sd = { 0 };
    D3D11_VIEWPORT Vp = { 0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f };
    D3D11_MAPPED_SUBRESOURCE M;
    ID3D11Texture2D *Rt = NULL, *St = NULL, *Tx = NULL;
    ID3D11RenderTargetView *Rtv = NULL;
    ID3D11ShaderResourceView *Srv = NULL;
    ID3D11SamplerState *Smp = NULL;
    ID3DBlob *Vsb = NULL, *Psb = NULL;
    ID3D11VertexShader *Vs = NULL;
    ID3D11PixelShader *Ps = NULL;
    static const float Black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    int Bad = 0;

    Td.Width = Td.Height = 64;
    Td.MipLevels = Td.ArraySize = 1;
    Td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    Td.SampleDesc.Count = 1;
    Td.Usage = D3D11_USAGE_DEFAULT;
    Td.BindFlags = D3D11_BIND_RENDER_TARGET;
    Mark("render target");
    CHECK(ID3D11Device_CreateTexture2D(Dev, &Td, NULL, &Rt), "CreateTexture2D(rt)");
    CHECK(ID3D11Device_CreateRenderTargetView(Dev, (ID3D11Resource *)Rt, NULL, &Rtv), "CreateRenderTargetView");
    Mark("texture");
    Td.Width = Td.Height = 2;
    Td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    CHECK(ID3D11Device_CreateTexture2D(Dev, &Td, &Init, &Tx), "CreateTexture2D(2x2)");
    CHECK(ID3D11Device_CreateShaderResourceView(Dev, (ID3D11Resource *)Tx, NULL, &Srv), "CreateShaderResourceView");
    Sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    Sd.AddressU = Sd.AddressV = Sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    Sd.MaxLOD = D3D11_FLOAT32_MAX;
    Sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    CHECK(ID3D11Device_CreateSamplerState(Dev, &Sd, &Smp), "CreateSamplerState");
    Mark("shaders");
    CHECK(Compile(TexHlsl, "vs", "vs_4_0", &Vsb), "D3DCompile(vs)");
    CHECK(Compile(TexHlsl, "ps", "ps_4_0", &Psb), "D3DCompile(ps)");
    CHECK(ID3D11Device_CreateVertexShader(Dev, ID3D10Blob_GetBufferPointer(Vsb), ID3D10Blob_GetBufferSize(Vsb), NULL,
                                          &Vs), "CreateVertexShader");
    CHECK(ID3D11Device_CreatePixelShader(Dev, ID3D10Blob_GetBufferPointer(Psb), ID3D10Blob_GetBufferSize(Psb), NULL,
                                         &Ps), "CreatePixelShader");
    Mark("draw");
    ID3D11DeviceContext_ClearRenderTargetView(Ctx, Rtv, Black);
    ID3D11DeviceContext_OMSetRenderTargets(Ctx, 1, &Rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(Ctx, 1, &Vp);
    ID3D11DeviceContext_IASetInputLayout(Ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(Ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(Ctx, Vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(Ctx, Ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(Ctx, 0, 1, &Srv);
    ID3D11DeviceContext_PSSetSamplers(Ctx, 0, 1, &Smp);
    ID3D11DeviceContext_Draw(Ctx, 3, 0);
    Mark("readback");
    Td.Width = Td.Height = 64;
    Td.Usage = D3D11_USAGE_STAGING;
    Td.BindFlags = 0;
    Td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(ID3D11Device_CreateTexture2D(Dev, &Td, NULL, &St), "CreateTexture2D(staging)");
    ID3D11DeviceContext_CopyResource(Ctx, (ID3D11Resource *)St, (ID3D11Resource *)Rt);
    CHECK(ID3D11DeviceContext_Map(Ctx, (ID3D11Resource *)St, 0, D3D11_MAP_READ, 0, &M), "Map");
    for (int i = 0; i < 4; i++) {
        UINT C = *(const UINT *)((const BYTE *)M.pData + Probe[i].Y * M.RowPitch + Probe[i].X * 4);
        printf("%-28s 0x%08x (want 0x%08x)%s\n", Probe[i].Name, C, Probe[i].Want, C == Probe[i].Want ? "" : "  <- WRONG");
        if (C != Probe[i].Want)
            Bad++;
    }
    ID3D11DeviceContext_Unmap(Ctx, (ID3D11Resource *)St, 0);
    printf("tex -> %s\n", Bad ? "FAIL" : "PASS");
    return Bad ? 4 : 0;
}

/* ---- cube: a spinning 3D workload ---- */

static const char CubeHlsl[] =
    "cbuffer C : register(b0) { float4x4 mvp; };\n"
    "struct V { float4 p : SV_Position; float4 c : COLOR; };\n"
    "V vs(float3 p : POSITION, float3 c : COLOR) { V o; o.p = mul(mvp, float4(p, 1.0)); o.c = float4(c, 1.0);"
    " return o; }\n"
    "float4 ps(V i) : SV_Target { return i.c; }\n";

typedef struct { float P[3], C[3]; } VERTEX;

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

/* Row-major 4x4, column vectors on the right (HLSL mul(m, v) with the default column_major packing reads the
 * transpose, so Mvp is stored transposed below). */
static void MatMul(float R[16], const float A[16], const float B[16])
{
    float T[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            T[i * 4 + j] = 0.0f;
            for (int k = 0; k < 4; k++)
                T[i * 4 + j] += A[i * 4 + k] * B[k * 4 + j];
        }
    memcpy(R, T, sizeof(T));
}

static void CubeMvp(float Out[16], float T, float Aspect)
{
    float Ca = cosf(T), Sa = sinf(T), Cb = cosf(T * 0.7f), Sb = sinf(T * 0.7f);
    float Ry[16] = { Ca, 0, Sa, 0, 0, 1, 0, 0, -Sa, 0, Ca, 0, 0, 0, 0, 1 };
    float Rx[16] = { 1, 0, 0, 0, 0, Cb, -Sb, 0, 0, Sb, Cb, 0, 0, 0, 0, 1 };
    float Tr[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 4.0f, 0, 0, 0, 1 };
    float N = 0.1f, F = 100.0f, Fy = 1.0f / tanf(0.6f);
    float Pr[16] = { Fy / Aspect, 0, 0, 0, 0, Fy, 0, 0, 0, 0, F / (F - N), -N * F / (F - N), 0, 0, 1, 0 };
    float M[16], Mv[16], Mvp[16];

    MatMul(M, Ry, Rx);
    MatMul(Mv, Tr, M);
    MatMul(Mvp, Pr, Mv);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            Out[j * 4 + i] = Mvp[i * 4 + j];
}

static int Cube(ID3D11Device *Dev, ID3D11DeviceContext *Ctx, DWORD Seconds)
{
    static const VERTEX Verts[8] = {
        { { -1, -1, -1 }, { 1, 0, 0 } }, { { 1, -1, -1 }, { 0, 1, 0 } }, { { 1, 1, -1 }, { 0, 0, 1 } },
        { { -1, 1, -1 }, { 1, 1, 0 } },  { { -1, -1, 1 }, { 1, 0, 1 } }, { { 1, -1, 1 }, { 0, 1, 1 } },
        { { 1, 1, 1 }, { 1, 1, 1 } },    { { -1, 1, 1 }, { 0.2f, 0.2f, 0.2f } },
    };
    static const USHORT Idx[36] = { 0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                                    3, 6, 2, 3, 7, 6, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5 };
    static const float Clear[4] = { 0.25f, 0.45f, 0.85f, 1.0f }   /* bright blue: visible on the webcam */;
    WNDCLASSW Wc = { 0 };
    HMONITOR Mon;
    MONITORINFO Mi = { sizeof(Mi) };
    HWND Wnd;
    IDXGIDevice *Xd = NULL;
    IDXGIAdapter *Xa = NULL;
    IDXGIFactory *Xf = NULL;
    IDXGISwapChain *Sc = NULL;
    DXGI_SWAP_CHAIN_DESC Scd = { 0 };
    ID3D11Texture2D *Bb = NULL, *Ds = NULL, *St = NULL;
    ID3D11RenderTargetView *Rtv = NULL;
    ID3D11DepthStencilView *Dsv = NULL;
    D3D11_TEXTURE2D_DESC Td = { 0 };
    ID3DBlob *Vsb = NULL, *Psb = NULL;
    ID3D11VertexShader *Vs = NULL;
    ID3D11PixelShader *Ps = NULL;
    ID3D11Buffer *Vb = NULL, *Ib = NULL, *Cb = NULL;
    ID3D11InputLayout *Il = NULL;
    D3D11_BUFFER_DESC Bd = { 0 };
    D3D11_SUBRESOURCE_DATA Init = { 0 };
    D3D11_INPUT_ELEMENT_DESC Ied[2] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    D3D11_VIEWPORT Vp;
    UINT W, H, Stride = sizeof(VERTEX), Offset = 0;
    LARGE_INTEGER Freq, T0, Now, Sec;
    ULONG Frames = 0, SecFrames = 0, PresentFails = 0;
    HRESULT FirstFail = S_OK;
    float Mvp[16];

    Mark("window");
    Wc.lpfnWndProc = WndProc;
    Wc.hInstance = GetModuleHandleW(NULL);
    Wc.lpszClassName = L"d3dtri";
    RegisterClassW(&Wc);
    Mon = MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(Mon, &Mi);
    W = (UINT)(Mi.rcMonitor.right - Mi.rcMonitor.left);
    H = (UINT)(Mi.rcMonitor.bottom - Mi.rcMonitor.top);
    printf("monitor %ux%u\n", W, H);
    Wnd = CreateWindowExW(WS_EX_TOPMOST, L"d3dtri", L"d3dtri", WS_POPUP | WS_VISIBLE, Mi.rcMonitor.left,
                          Mi.rcMonitor.top, (int)W, (int)H, NULL, NULL, Wc.hInstance, NULL);
    if (Wnd == NULL) {
        printf("CreateWindowExW failed %lu\n", GetLastError());
        return 2;
    }
    Pump();

    Mark("swap chain");
    CHECK(ID3D11Device_QueryInterface(Dev, &IID_IDXGIDevice, (void **)&Xd), "QueryInterface(IDXGIDevice)");
    CHECK(IDXGIDevice_GetAdapter(Xd, &Xa), "GetAdapter");
    CHECK(IDXGIAdapter_GetParent(Xa, &IID_IDXGIFactory, (void **)&Xf), "GetParent(IDXGIFactory)");
    Scd.BufferDesc.Width = W;
    Scd.BufferDesc.Height = H;
    Scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    Scd.SampleDesc.Count = 1;
    Scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    Scd.BufferCount = 2;
    Scd.OutputWindow = Wnd;
    Scd.Windowed = TRUE;
    Scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    /* D3DTRI_FLIP=1: the flip model most current apps use (DWM composes the swap chain's shared buffers) instead
     * of the legacy blt model, whose redirected presents DWM never composed on the S10e (notes/gpu-hw-path.md H4c). */
    if (GetEnvironmentVariableA("D3DTRI_FLIP", NULL, 0) != 0)
        Scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    printf("swap effect %s\n", Scd.SwapEffect == DXGI_SWAP_EFFECT_DISCARD ? "discard (blt model)" : "flip sequential");
    CHECK(IDXGIFactory_CreateSwapChain(Xf, (IUnknown *)Dev, &Scd, &Sc), "CreateSwapChain");
    CHECK(IDXGISwapChain_GetBuffer(Sc, 0, &IID_ID3D11Texture2D, (void **)&Bb), "GetBuffer");
    CHECK(ID3D11Device_CreateRenderTargetView(Dev, (ID3D11Resource *)Bb, NULL, &Rtv), "CreateRenderTargetView");
    Td.Width = W;
    Td.Height = H;
    Td.MipLevels = Td.ArraySize = 1;
    Td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    Td.SampleDesc.Count = 1;
    Td.Usage = D3D11_USAGE_DEFAULT;
    Td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    CHECK(ID3D11Device_CreateTexture2D(Dev, &Td, NULL, &Ds), "CreateTexture2D(depth)");
    CHECK(ID3D11Device_CreateDepthStencilView(Dev, (ID3D11Resource *)Ds, NULL, &Dsv), "CreateDepthStencilView");

    Mark("shaders");
    CHECK(Compile(CubeHlsl, "vs", "vs_4_0", &Vsb), "D3DCompile(vs)");
    CHECK(Compile(CubeHlsl, "ps", "ps_4_0", &Psb), "D3DCompile(ps)");
    CHECK(ID3D11Device_CreateVertexShader(Dev, ID3D10Blob_GetBufferPointer(Vsb), ID3D10Blob_GetBufferSize(Vsb), NULL,
                                          &Vs), "CreateVertexShader");
    CHECK(ID3D11Device_CreatePixelShader(Dev, ID3D10Blob_GetBufferPointer(Psb), ID3D10Blob_GetBufferSize(Psb), NULL,
                                         &Ps), "CreatePixelShader");
    CHECK(ID3D11Device_CreateInputLayout(Dev, Ied, 2, ID3D10Blob_GetBufferPointer(Vsb), ID3D10Blob_GetBufferSize(Vsb),
                                         &Il), "CreateInputLayout");

    Mark("buffers");
    Bd.Usage = D3D11_USAGE_DEFAULT;
    Bd.ByteWidth = sizeof(Verts);
    Bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    Init.pSysMem = Verts;
    CHECK(ID3D11Device_CreateBuffer(Dev, &Bd, &Init, &Vb), "CreateBuffer(vb)");
    Bd.ByteWidth = sizeof(Idx);
    Bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    Init.pSysMem = Idx;
    CHECK(ID3D11Device_CreateBuffer(Dev, &Bd, &Init, &Ib), "CreateBuffer(ib)");
    Bd.ByteWidth = sizeof(Mvp);
    Bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    CHECK(ID3D11Device_CreateBuffer(Dev, &Bd, NULL, &Cb), "CreateBuffer(cb)");

    Vp.TopLeftX = Vp.TopLeftY = 0.0f;
    Vp.Width = (float)W;
    Vp.Height = (float)H;
    Vp.MinDepth = 0.0f;
    Vp.MaxDepth = 1.0f;
    QueryPerformanceFrequency(&Freq);
    QueryPerformanceCounter(&T0);
    Sec = T0;
    Mark("frames");
    for (;;) {
        HRESULT hr;
        float T;

        QueryPerformanceCounter(&Now);
        if ((ULONG64)(Now.QuadPart - T0.QuadPart) >= (ULONG64)Seconds * (ULONG64)Freq.QuadPart)
            break;
        T = (float)(Now.QuadPart - T0.QuadPart) / (float)Freq.QuadPart;
        CubeMvp(Mvp, T, (float)W / (float)H);
        ID3D11DeviceContext_UpdateSubresource(Ctx, (ID3D11Resource *)Cb, 0, NULL, Mvp, 0, 0);
        ID3D11DeviceContext_ClearRenderTargetView(Ctx, Rtv, Clear);
        ID3D11DeviceContext_ClearDepthStencilView(Ctx, Dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        ID3D11DeviceContext_OMSetRenderTargets(Ctx, 1, &Rtv, Dsv);
        ID3D11DeviceContext_RSSetViewports(Ctx, 1, &Vp);
        ID3D11DeviceContext_IASetVertexBuffers(Ctx, 0, 1, &Vb, &Stride, &Offset);
        ID3D11DeviceContext_IASetIndexBuffer(Ctx, Ib, DXGI_FORMAT_R16_UINT, 0);
        ID3D11DeviceContext_IASetInputLayout(Ctx, Il);
        ID3D11DeviceContext_IASetPrimitiveTopology(Ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(Ctx, Vs, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(Ctx, 0, 1, &Cb);
        ID3D11DeviceContext_PSSetShader(Ctx, Ps, NULL, 0);
        ID3D11DeviceContext_DrawIndexed(Ctx, 36, 0, 0);
        if (Frames == 0) {
            /* first frame: did the GPU draw the cube? centre pixel of the back buffer through a staging copy */
            D3D11_MAPPED_SUBRESOURCE M;
            D3D11_TEXTURE2D_DESC Sd;
            ID3D11Texture2D_GetDesc(Bb, &Sd);
            Sd.Usage = D3D11_USAGE_STAGING;
            Sd.BindFlags = 0;
            Sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            Sd.MiscFlags = 0;
            Mark("first-frame readback");
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(Dev, &Sd, NULL, &St))) {
                ID3D11DeviceContext_CopyResource(Ctx, (ID3D11Resource *)St, (ID3D11Resource *)Bb);
                if (SUCCEEDED(ID3D11DeviceContext_Map(Ctx, (ID3D11Resource *)St, 0, D3D11_MAP_READ, 0, &M))) {
                    UINT C = *(const UINT *)((const BYTE *)M.pData + (H / 2) * M.RowPitch + (W / 2) * 4);
                    UINT K = *(const UINT *)((const BYTE *)M.pData + 2 * M.RowPitch + 2 * 4);
                    printf("first frame: centre 0x%08x (cube), corner 0x%08x (clear colour = 0xff4073d9)\n", C, K);
                    ID3D11DeviceContext_Unmap(Ctx, (ID3D11Resource *)St, 0);
                }
                ID3D11Texture2D_Release(St);
            }
            Mark("frames");
        }
        hr = IDXGISwapChain_Present(Sc, 0, 0);
        if (FAILED(hr)) {
            if (PresentFails++ == 0)
                FirstFail = hr;
        }
        Frames++;
        SecFrames++;
        Pump();
        if ((ULONG64)(Now.QuadPart - Sec.QuadPart) >= (ULONG64)Freq.QuadPart) {
            printf("t %5.1f fps %lu\n", T, SecFrames);
            fflush(stdout);
            SecFrames = 0;
            Sec = Now;
        }
    }
    printf("cube: %lu frames in %lu s = %.1f fps, %lu present failures (first 0x%08lx)\n", Frames, Seconds,
           (double)Frames / (double)Seconds, PresentFails, (unsigned long)FirstFail);
    return PresentFails == 0 ? 0 : 2;
}

/* ---- gdi: a DWM load without D3D in the app ---- */

static LONG GdiX, GdiW, GdiH;
static ULONG GdiPaints;

static LRESULT CALLBACK GdiProc(HWND W, UINT M, WPARAM Wp, LPARAM Lp)
{
    if (M == WM_PAINT) {
        PAINTSTRUCT Ps;
        HDC Dc = BeginPaint(W, &Ps);
        RECT All = { 0, 0, GdiW, GdiH }, Box = { GdiX, GdiH / 3, GdiX + GdiW / 4, GdiH / 3 + GdiW / 4 };
        HBRUSH Bg = CreateSolidBrush(RGB(30, 60, 120)), Fg = CreateSolidBrush(RGB(255, 200, 0));
        FillRect(Dc, &All, Bg);
        FillRect(Dc, &Box, Fg);
        DeleteObject(Bg);
        DeleteObject(Fg);
        EndPaint(W, &Ps);
        GdiPaints++;
        return 0;
    }
    if (M == WM_ERASEBKGND)
        return 1;
    return DefWindowProcW(W, M, Wp, Lp);
}

/* A window covering the primary monitor whose yellow square moves every 33 ms (WM_PAINT through GDI): every move
 * is a change DWM has to compose, so the panel shows how fast the compositor runs. Prints paints per second. */
static int Gdi(DWORD Seconds)
{
    WNDCLASSW Wc = { 0 };
    MONITORINFO Mi = { sizeof(Mi) };
    HMONITOR Mon = MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    HWND Wnd;
    DWORD T0 = GetTickCount(), Sec = T0, Last = 0;

    GetMonitorInfoW(Mon, &Mi);
    GdiW = Mi.rcMonitor.right - Mi.rcMonitor.left;
    GdiH = Mi.rcMonitor.bottom - Mi.rcMonitor.top;
    Wc.lpfnWndProc = GdiProc;
    Wc.hInstance = GetModuleHandleW(NULL);
    Wc.lpszClassName = L"d3dtri-gdi";
    RegisterClassW(&Wc);
    Mark("gdi window");
    Wnd = CreateWindowExW(WS_EX_TOPMOST, L"d3dtri-gdi", L"d3dtri gdi", WS_POPUP | WS_VISIBLE, Mi.rcMonitor.left,
                          Mi.rcMonitor.top, GdiW, GdiH, NULL, NULL, Wc.hInstance, NULL);
    if (Wnd == NULL) {
        printf("CreateWindowExW failed %lu\n", GetLastError());
        return 2;
    }
    printf("gdi window %ldx%ld\n", GdiW, GdiH);
    Mark("gdi frames");
    while (GetTickCount() - T0 < Seconds * 1000) {
        DWORD Now = GetTickCount();
        if (Now - Last >= 33) {
            Last = Now;
            GdiX = (LONG)(((Now - T0) / 4) % (ULONG)(GdiW - GdiW / 4));
            InvalidateRect(Wnd, NULL, FALSE);
        }
        Pump();
        if (Now - Sec >= 1000) {
            printf("t %5.1f paints %lu\n", (Now - T0) / 1000.0, GdiPaints);
            fflush(stdout);
            Sec = Now;
        }
        Sleep(1);
    }
    printf("gdi: %lu paints in %lu s\n", GdiPaints, Seconds);
    DestroyWindow(Wnd);
    return 0;
}

int main(int argc, char **argv)
{
    const char *Mode = argc > 1 ? argv[1] : "tri";
    DWORD Seconds = argc > 2 ? (DWORD)strtoul(argv[2], NULL, 10) : 20;
    D3D_FEATURE_LEVEL Fl[2] = { D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 }, Got = 0;
    ID3D11Device *Dev = NULL;
    ID3D11DeviceContext *Ctx = NULL;
    IDXGIAdapter1 *Mali;
    HRESULT hr;
    char Gd[64];
    int Rc;

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    if (Seconds == 0 || Seconds > 3600)
        Seconds = 20;
    WatchdogMs = (strcmp(Mode, "cube") == 0 || strcmp(Mode, "gdi") == 0 ? Seconds * 1000 : 0) + 90000;
    CreateThread(NULL, 0, Watchdog, NULL, 0, NULL);
    if (GetEnvironmentVariableA("GALLIUM_DRIVER", Gd, sizeof(Gd)) == 0)
        strcpy(Gd, "(unset)");
    printf("d3dtri %s, GALLIUM_DRIVER=%s\n", Mode, Gd);

    if (strcmp(Mode, "gdi") == 0) {
        Rc = Gdi(Seconds);
        printf("d3dtri: done, exit %d\n", Rc);
        fflush(stdout);
        return Rc;
    }
    Mark("find Mali adapter");
    Mali = FindMali();
    if (Mali == NULL) {
        printf("no Mali adapter\n");
        return 1;
    }
    Mark("D3D11CreateDevice");
    hr = D3D11CreateDevice((IDXGIAdapter *)Mali, D3D_DRIVER_TYPE_UNKNOWN, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, Fl, 2,
                           D3D11_SDK_VERSION, &Dev, &Got, &Ctx);
    printf("D3D11CreateDevice hr 0x%08lx FL 0x%x\n", (unsigned long)hr, Got);
    if (FAILED(hr))
        return 2;
    Rc = strcmp(Mode, "cube") == 0 ? Cube(Dev, Ctx, Seconds) : strcmp(Mode, "tex") == 0 ? Tex(Dev, Ctx) : Tri(Dev, Ctx);
    printf("d3dtri: done, exit %d\n", Rc);
    fflush(stdout);
    return Rc;
}
