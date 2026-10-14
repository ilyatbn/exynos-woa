// SPDX-License-Identifier: GPL-2.0
/*
 * GpuCheck.exe - a desktop app that shows, for a person looking at the phone, that the Mali GPU renders
 * (notes/gpu-hw-path.md, 2026-10-14 user request "how do I see that the gpu is working myself").
 *
 * One window: a text panel on top, updated every second, and a spinning cube below it, drawn with D3D11 on the
 * Mali adapter (the same cube, swap chain model and DPI behaviour as d3dtri cube, the path soak s2 ran for 7 min).
 * The panel lists:
 *   - the adapter the cube runs on (DXGI description, vendor/device id) and whether it is the Mali hardware or
 *     Microsoft's software renderer (Basic Render Driver = WARP on the CPU);
 *   - the D3D feature level of the device;
 *   - a pixel readback of the first frame (the cube's centre pixel differs from the background colour);
 *   - frames per second;
 *   - Windows' own counter "GPU Engine / Utilization Percentage" for the Mali adapter's 3D engine (what Task
 *     Manager shows on WDDM 2.x drivers; ours is WDDM 1.3, so Task Manager has no GPU page for it);
 *   - the Mali KMD's job counters (MW_ESCAPE_STATS): jobs the GPU hardware completed, per second and in total,
 *     failed jobs, GPU resets, and the GPU_ID register (MW_ESCAPE_GPU_INFO).
 * Every reading that is not available says so instead of failing. Build: build-gpucheck.cmd.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <wchar.h>
#pragma warning(push)
#pragma warning(disable : 4201)     /* nameless unions in the SDK headers (C mode) */
#include <dxgi.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <d3dkmthk.h>
#include <pdh.h>
#include <pdhmsg.h>
#pragma warning(pop)
#include "maliwddm_abi.h"

/* ---- the Mali KMD's counters (D3DKMT escape, as m5test) ---- */

static D3DKMT_HANDLE KmtAdapter;

static BOOL KmtOpen(LUID Luid)
{
    D3DKMT_OPENADAPTERFROMLUID O = { 0 };

    O.AdapterLuid = Luid;
    if (D3DKMTOpenAdapterFromLuid(&O) < 0)
        return FALSE;
    KmtAdapter = O.hAdapter;
    return TRUE;
}

static BOOL KmtEscape(void *Data, UINT Size)
{
    D3DKMT_ESCAPE E = { 0 };

    if (KmtAdapter == 0)
        return FALSE;
    E.hAdapter = KmtAdapter;
    E.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    E.pPrivateDriverData = Data;
    E.PrivateDriverDataSize = Size;
    return D3DKMTEscape(&E) >= 0;
}

/* ---- Windows' GPU engine counter for one adapter LUID (as gfxperf) ---- */

static PDH_HQUERY PdhQ;
static PDH_HCOUNTER PdhC;
static wchar_t LuidTag[64];

static void PdhOpen(LUID Luid)
{
    swprintf(LuidTag, 64, L"luid_0x%08lx_0x%08lx", (unsigned long)Luid.HighPart, (unsigned long)Luid.LowPart);
    if (PdhOpenQueryW(NULL, 0, &PdhQ) != ERROR_SUCCESS) {
        PdhQ = NULL;
        return;
    }
    if (PdhAddEnglishCounterW(PdhQ, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &PdhC) != ERROR_SUCCESS) {
        PdhCloseQuery(PdhQ);
        PdhQ = NULL;
        return;
    }
    PdhCollectQueryData(PdhQ);   /* rate counter: the first sample only primes it */
}

/* Sum of the 3D engine utilisation of all processes on the Mali adapter, or -1 when unavailable. */
static double PdhMali3d(void)
{
    DWORD Bytes = 0, N = 0;
    PDH_FMT_COUNTERVALUE_ITEM_W *Items;
    double Sum = 0.0;
    BOOL Found = FALSE;

    if (PdhQ == NULL || PdhCollectQueryData(PdhQ) != ERROR_SUCCESS)
        return -1.0;
    if (PdhGetFormattedCounterArrayW(PdhC, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &Bytes, &N, NULL) != PDH_MORE_DATA)
        return -1.0;
    Items = (PDH_FMT_COUNTERVALUE_ITEM_W *)HeapAlloc(GetProcessHeap(), 0, Bytes);
    if (Items == NULL)
        return -1.0;
    if (PdhGetFormattedCounterArrayW(PdhC, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &Bytes, &N, Items) == ERROR_SUCCESS) {
        for (DWORD i = 0; i < N; i++) {
            wchar_t Name[256];
            wcsncpy_s(Name, 256, Items[i].szName, _TRUNCATE);
            _wcslwr_s(Name, 256);
            if (wcsstr(Name, LuidTag) != NULL) {
                Found = TRUE;
                Sum += Items[i].FmtValue.doubleValue;
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, Items);
    return Found ? Sum : -1.0;
}

/* ---- cube (d3dtri's) ---- */

static const char CubeHlsl[] =
    "cbuffer C : register(b0) { float4x4 mvp; };\n"
    "struct V { float4 p : SV_Position; float4 c : COLOR; };\n"
    "V vs(float3 p : POSITION, float3 c : COLOR) { V o; o.p = mul(mvp, float4(p, 1.0)); o.c = float4(c, 1.0);"
    " return o; }\n"
    "float4 ps(V i) : SV_Target { return i.c; }\n";

typedef struct { float P[3], C[3]; } VERTEX;

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

/* ---- app state ---- */

static HWND MainWnd, TextWnd, CubeWnd;
static HFONT Font;
static BOOL Quit;
static wchar_t Static[1024];       /* the lines that do not change: adapter, device, first-frame check */
static UINT CubeW, CubeH;

static ID3D11Device *Dev;
static ID3D11DeviceContext *Ctx;
static IDXGISwapChain *Sc;
static ID3D11Texture2D *Bb, *Ds;
static ID3D11RenderTargetView *Rtv;
static ID3D11DepthStencilView *Dsv;
static ID3D11VertexShader *Vs;
static ID3D11PixelShader *Ps;
static ID3D11Buffer *Vb, *Ib, *Cb;
static ID3D11InputLayout *Il;

static LRESULT CALLBACK MainProc(HWND W, UINT M, WPARAM Wp, LPARAM Lp)
{
    if (M == WM_CLOSE || M == WM_DESTROY) {
        Quit = TRUE;
        PostQuitMessage(0);
        return 0;
    }
    if (M == WM_CTLCOLORSTATIC) {
        SetBkColor((HDC)Wp, RGB(255, 255, 255));
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }
    return DefWindowProcW(W, M, Wp, Lp);
}

static LRESULT CALLBACK CubeProc(HWND W, UINT M, WPARAM Wp, LPARAM Lp)
{
    if (M == WM_ERASEBKGND)
        return 1;
    return DefWindowProcW(W, M, Wp, Lp);
}

static void Fail(const wchar_t *What, HRESULT Hr)
{
    wchar_t Msg[512];

    swprintf(Msg, 512, L"%s failed (0x%08lx).\n\nThe GPU path is not working in this session.", What,
             (unsigned long)Hr);
    MessageBoxW(MainWnd, Msg, L"GPU Check", MB_ICONERROR);
    ExitProcess(2);
}

#define TRY(hr, what)                  \
    do {                               \
        HRESULT hr_ = (hr);            \
        if (FAILED(hr_))               \
            Fail(what, hr_);           \
    } while (0)

/* The Mali adapter, or (none present) the first one - the panel then says the GPU is not in use. */
static IDXGIAdapter1 *PickAdapter(DXGI_ADAPTER_DESC1 *Out)
{
    IDXGIFactory1 *Fac = NULL;
    IDXGIAdapter1 *A = NULL, *Pick = NULL;

    TRY(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&Fac), L"CreateDXGIFactory1");
    for (UINT i = 0; IDXGIFactory1_EnumAdapters1(Fac, i, &A) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 D;
        IDXGIAdapter1_GetDesc1(A, &D);
        if (Pick == NULL || wcsstr(D.Description, L"Mali") != NULL) {
            if (Pick != NULL)
                IDXGIAdapter1_Release(Pick);
            Pick = A;
            *Out = D;
            if (wcsstr(D.Description, L"Mali") != NULL)
                break;
        } else {
            IDXGIAdapter1_Release(A);
        }
    }
    IDXGIFactory1_Release(Fac);
    if (Pick == NULL)
        Fail(L"Finding a display adapter", E_FAIL);
    return Pick;
}

static void CreateCube(IDXGIAdapter1 *Adapter, D3D_FEATURE_LEVEL *Got)
{
    static const VERTEX Verts[8] = {
        { { -1, -1, -1 }, { 1, 0, 0 } }, { { 1, -1, -1 }, { 0, 1, 0 } }, { { 1, 1, -1 }, { 0, 0, 1 } },
        { { -1, 1, -1 }, { 1, 1, 0 } },  { { -1, -1, 1 }, { 1, 0, 1 } }, { { 1, -1, 1 }, { 0, 1, 1 } },
        { { 1, 1, 1 }, { 1, 1, 1 } },    { { -1, 1, 1 }, { 0.2f, 0.2f, 0.2f } },
    };
    static const USHORT Idx[36] = { 0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                                    3, 6, 2, 3, 7, 6, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5 };
    D3D_FEATURE_LEVEL Fl[2] = { D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    IDXGIDevice *Xd = NULL;
    IDXGIAdapter *Xa = NULL;
    IDXGIFactory *Xf = NULL;
    DXGI_SWAP_CHAIN_DESC Scd = { 0 };
    D3D11_TEXTURE2D_DESC Td = { 0 };
    ID3DBlob *Vsb = NULL, *Psb = NULL, *Err = NULL;
    D3D11_BUFFER_DESC Bd = { 0 };
    D3D11_SUBRESOURCE_DATA Init = { 0 };
    D3D11_INPUT_ELEMENT_DESC Ied[2] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    TRY(D3D11CreateDevice((IDXGIAdapter *)Adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                          Fl, 2, D3D11_SDK_VERSION, &Dev, Got, &Ctx), L"Creating the Direct3D device");
    TRY(ID3D11Device_QueryInterface(Dev, &IID_IDXGIDevice, (void **)&Xd), L"QueryInterface(IDXGIDevice)");
    TRY(IDXGIDevice_GetAdapter(Xd, &Xa), L"GetAdapter");
    TRY(IDXGIAdapter_GetParent(Xa, &IID_IDXGIFactory, (void **)&Xf), L"GetParent(IDXGIFactory)");
    Scd.BufferDesc.Width = CubeW;
    Scd.BufferDesc.Height = CubeH;
    Scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    Scd.SampleDesc.Count = 1;
    Scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    Scd.BufferCount = 2;
    Scd.OutputWindow = CubeWnd;
    Scd.Windowed = TRUE;
    Scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;   /* the model soak s2 ran for 300 s */
    TRY(IDXGIFactory_CreateSwapChain(Xf, (IUnknown *)Dev, &Scd, &Sc), L"Creating the swap chain");
    IDXGIFactory_MakeWindowAssociation(Xf, CubeWnd, DXGI_MWA_NO_ALT_ENTER);
    TRY(IDXGISwapChain_GetBuffer(Sc, 0, &IID_ID3D11Texture2D, (void **)&Bb), L"GetBuffer");
    TRY(ID3D11Device_CreateRenderTargetView(Dev, (ID3D11Resource *)Bb, NULL, &Rtv), L"CreateRenderTargetView");
    Td.Width = CubeW;
    Td.Height = CubeH;
    Td.MipLevels = Td.ArraySize = 1;
    Td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    Td.SampleDesc.Count = 1;
    Td.Usage = D3D11_USAGE_DEFAULT;
    Td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    TRY(ID3D11Device_CreateTexture2D(Dev, &Td, NULL, &Ds), L"Creating the depth buffer");
    TRY(ID3D11Device_CreateDepthStencilView(Dev, (ID3D11Resource *)Ds, NULL, &Dsv), L"CreateDepthStencilView");

    TRY(D3DCompile(CubeHlsl, sizeof(CubeHlsl) - 1, "gpucheck", NULL, NULL, "vs", "vs_4_0", 0, 0, &Vsb, &Err),
        L"Compiling the vertex shader");
    TRY(D3DCompile(CubeHlsl, sizeof(CubeHlsl) - 1, "gpucheck", NULL, NULL, "ps", "ps_4_0", 0, 0, &Psb, &Err),
        L"Compiling the pixel shader");
    TRY(ID3D11Device_CreateVertexShader(Dev, ID3D10Blob_GetBufferPointer(Vsb), ID3D10Blob_GetBufferSize(Vsb), NULL,
                                        &Vs), L"CreateVertexShader");
    TRY(ID3D11Device_CreatePixelShader(Dev, ID3D10Blob_GetBufferPointer(Psb), ID3D10Blob_GetBufferSize(Psb), NULL,
                                       &Ps), L"CreatePixelShader");
    TRY(ID3D11Device_CreateInputLayout(Dev, Ied, 2, ID3D10Blob_GetBufferPointer(Vsb), ID3D10Blob_GetBufferSize(Vsb),
                                       &Il), L"CreateInputLayout");
    Bd.Usage = D3D11_USAGE_DEFAULT;
    Bd.ByteWidth = sizeof(Verts);
    Bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    Init.pSysMem = Verts;
    TRY(ID3D11Device_CreateBuffer(Dev, &Bd, &Init, &Vb), L"CreateBuffer(vertices)");
    Bd.ByteWidth = sizeof(Idx);
    Bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    Init.pSysMem = Idx;
    TRY(ID3D11Device_CreateBuffer(Dev, &Bd, &Init, &Ib), L"CreateBuffer(indices)");
    Bd.ByteWidth = 64;
    Bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    TRY(ID3D11Device_CreateBuffer(Dev, &Bd, NULL, &Cb), L"CreateBuffer(constants)");
}

static const float Clear[4] = { 0.25f, 0.45f, 0.85f, 1.0f };

static HRESULT DrawFrame(float T)
{
    UINT Stride = sizeof(VERTEX), Offset = 0;
    D3D11_VIEWPORT Vp = { 0.0f, 0.0f, (float)CubeW, (float)CubeH, 0.0f, 1.0f };
    float Mvp[16];

    CubeMvp(Mvp, T, (float)CubeW / (float)CubeH);
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
    return S_OK;
}

/* Centre pixel of the back buffer after the first frame, through a staging copy; 0 when it could not be read. */
static UINT CentrePixel(UINT *Corner)
{
    D3D11_TEXTURE2D_DESC Sd;
    ID3D11Texture2D *St = NULL;
    D3D11_MAPPED_SUBRESOURCE M;
    UINT C = 0;

    ID3D11Texture2D_GetDesc(Bb, &Sd);
    Sd.Usage = D3D11_USAGE_STAGING;
    Sd.BindFlags = 0;
    Sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Sd.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(Dev, &Sd, NULL, &St)))
        return 0;
    ID3D11DeviceContext_CopyResource(Ctx, (ID3D11Resource *)St, (ID3D11Resource *)Bb);
    if (SUCCEEDED(ID3D11DeviceContext_Map(Ctx, (ID3D11Resource *)St, 0, D3D11_MAP_READ, 0, &M))) {
        C = *(const UINT *)((const BYTE *)M.pData + (CubeH / 2) * M.RowPitch + (CubeW / 2) * 4);
        *Corner = *(const UINT *)((const BYTE *)M.pData + 2 * M.RowPitch + 2 * 4);
        ID3D11DeviceContext_Unmap(Ctx, (ID3D11Resource *)St, 0);
    }
    ID3D11Texture2D_Release(St);
    return C;
}

int WINAPI wWinMain(HINSTANCE Inst, HINSTANCE Prev, PWSTR Cmd, int Show)
{
    WNDCLASSW Wc = { 0 };
    RECT Work, Cl;
    DXGI_ADAPTER_DESC1 Ad = { 0 };
    IDXGIAdapter1 *Adapter;
    D3D_FEATURE_LEVEL Got = 0;
    BOOL IsMali, IsSoftware, HaveStats;
    MW_ESCAPE_GPU_INFO_DATA Gi = { 0 };
    MW_ESCAPE_STATS_DATA S0 = { 0 }, Prev1 = { 0 };
    LARGE_INTEGER Freq, T0, Now, Sec;
    ULONG Frames = 0, SecFrames = 0, PresentFails = 0;
    double Fps = 0.0;
    int TextH, Pad;

    UNREFERENCED_PARAMETER(Prev);
    UNREFERENCED_PARAMETER(Cmd);

    SystemParametersInfoW(SPI_GETWORKAREA, 0, &Work, 0);
    Wc.lpfnWndProc = MainProc;
    Wc.hInstance = Inst;
    Wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    Wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    Wc.lpszClassName = L"GpuCheck";
    RegisterClassW(&Wc);
    Wc.lpfnWndProc = CubeProc;
    Wc.hbrBackground = NULL;
    Wc.lpszClassName = L"GpuCheckCube";
    RegisterClassW(&Wc);

    /* Fixed size (no resize border, no maximise): the cube's swap chain is sized once and never resized. */
    MainWnd = CreateWindowExW(0, L"GpuCheck", L"GPU Check", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                              Work.left, Work.top, Work.right - Work.left, Work.bottom - Work.top, NULL, NULL, Inst,
                              NULL);
    GetClientRect(MainWnd, &Cl);
    Pad = 8;
    TextH = (Cl.bottom * 45) / 100;
    Font = CreateFontW(-((Cl.right < Cl.bottom ? Cl.right : Cl.bottom) / 30), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                       L"Segoe UI");
    TextWnd = CreateWindowExW(0, L"STATIC", L"Starting...", WS_CHILD | WS_VISIBLE | SS_LEFT, Pad, Pad,
                              Cl.right - 2 * Pad, TextH - Pad, MainWnd, NULL, Inst, NULL);
    SendMessageW(TextWnd, WM_SETFONT, (WPARAM)Font, TRUE);
    CubeW = (UINT)(Cl.right & ~1);
    CubeH = (UINT)((Cl.bottom - TextH) & ~1);
    CubeWnd = CreateWindowExW(0, L"GpuCheckCube", NULL, WS_CHILD | WS_VISIBLE, 0, TextH, (int)CubeW, (int)CubeH,
                              MainWnd, NULL, Inst, NULL);
    ShowWindow(MainWnd, Show);
    UpdateWindow(MainWnd);

    Adapter = PickAdapter(&Ad);
    IsMali = wcsstr(Ad.Description, L"Mali") != NULL;
    IsSoftware = (Ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || wcsstr(Ad.Description, L"Basic Render") != NULL;
    CreateCube(Adapter, &Got);
    if (IsMali)   /* the private escapes are the Mali KMD's ABI: never sent to another vendor's driver */
        KmtOpen(Ad.AdapterLuid);
    PdhOpen(Ad.AdapterLuid);
    Gi.Code = MW_ESCAPE_GPU_INFO;
    if (!KmtEscape(&Gi, sizeof(Gi)))
        Gi.Count = 0;
    S0.Code = MW_ESCAPE_STATS;
    HaveStats = KmtEscape(&S0, sizeof(S0));
    Prev1 = S0;

    {
        UINT Corner = 0, Centre;
        const wchar_t *Verdict;
        wchar_t IdS[64];

        if (Gi.Count != 0)
            swprintf(IdS, 64, L"0x%08llx", (unsigned long long)Gi.Value[0]);
        else
            wcscpy_s(IdS, 64, L"not available");
        DrawFrame(0.0f);
        Centre = CentrePixel(&Corner);
        IDXGISwapChain_Present(Sc, 0, 0);
        if (Centre == 0 && Corner == 0)
            Verdict = L"could not read the frame back";
        else if (Centre != Corner)
            Verdict = L"yes - the cube is in the frame";
        else
            Verdict = L"NO - the frame is only background";
        swprintf(Static, 1024,
                 L"Adapter: %s\n"
                 L"Type: %s\n"
                 L"Direct3D 11 device: feature level %u_%u\n"
                 L"Mali GPU_ID: %s\n"
                 L"First frame drawn: %s\n",
                 Ad.Description,
                 IsMali && !IsSoftware ? L"HARDWARE GPU (Mali driver)"
                                       : (IsSoftware ? L"SOFTWARE (CPU) - the GPU is not used"
                                                     : L"not the Mali adapter"),
                 (unsigned)(Got >> 12), (unsigned)((Got >> 8) & 0xF),
                 IdS,
                 Verdict);
    }

    QueryPerformanceFrequency(&Freq);
    QueryPerformanceCounter(&T0);
    Sec = T0;
    while (!Quit) {
        MSG Msg;
        float T;

        while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE)) {
            if (Msg.message == WM_QUIT)
                Quit = TRUE;
            TranslateMessage(&Msg);
            DispatchMessageW(&Msg);
        }
        if (Quit)
            break;
        if (IsIconic(MainWnd)) {
            Sleep(100);
            continue;
        }
        QueryPerformanceCounter(&Now);
        T = (float)(Now.QuadPart - T0.QuadPart) / (float)Freq.QuadPart;
        DrawFrame(T);
        if (FAILED(IDXGISwapChain_Present(Sc, 0, 0)))
            PresentFails++;
        Frames++;
        SecFrames++;
        if ((ULONG64)(Now.QuadPart - Sec.QuadPart) >= (ULONG64)Freq.QuadPart) {
            double Dt = (double)(Now.QuadPart - Sec.QuadPart) / (double)Freq.QuadPart;
            double Util = PdhMali3d();
            MW_ESCAPE_STATS_DATA S = { 0 };
            wchar_t Text[2048], UtilS[64], StatsS[512];

            Fps = SecFrames / Dt;
            SecFrames = 0;
            Sec = Now;
            if (Util >= 0.0)
                swprintf(UtilS, 64, L"%.0f %%", Util);
            else
                wcscpy_s(UtilS, 64, L"not available");
            S.Code = MW_ESCAPE_STATS;
            if (HaveStats && KmtEscape(&S, sizeof(S))) {
                swprintf(StatsS, 512,
                         L"Mali jobs completed by the GPU: %lu per second\n"
                         L"   since this app started: %lu, since boot: %lu\n"
                         L"Failed jobs: %lu   Timed out: %lu   GPU resets: %lu\n",
                         (unsigned long)((S.JobsDone - Prev1.JobsDone) / (Dt > 0.5 ? Dt : 1.0)),
                         (unsigned long)(S.JobsDone - S0.JobsDone), (unsigned long)S.JobsDone,
                         (unsigned long)S.JobsFailed, (unsigned long)S.JobsTimedOut, (unsigned long)S.Resets);
                Prev1 = S;
            } else {
                wcscpy_s(StatsS, 512, L"Mali job counters: not available\n");
            }
            swprintf(Text, 2048,
                     L"%s"
                     L"Cube frame rate: %.1f fps (%lu frames, %lu present errors)\n"
                     L"Windows GPU counter, 3D engine busy: %s\n"
                     L"%s",
                     Static, Fps, Frames, PresentFails, UtilS, StatsS);
            SetWindowTextW(TextWnd, Text);
        }
    }
    return 0;
}
