/*
 * d3dmrt - G32 (notes/games-unity.md): multiple render targets with mixed formats on the Mali D3D11 UMD vs WARP.
 * Dave the Diver's depth-of-field prefilter (URP) writes SV_Target0 = CoC to an R16_UNORM target and SV_Target1 =
 * colour to an R16G16B16A16_FLOAT view of a TYPELESS resource; on the Mali target 1 came back +inf everywhere
 * (frame capture, traces/games/framecap-day-black2.txt). One full-screen draw per case writes target 0 = 0.33 and
 * target 1 = (0.25, 0.5, 0.75, 1); the centre pixel of each target is read back and compared with WARP.
 * Prints one line per case and device, then "D3DMRT n/m match".
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static const char *g_vs =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 p = float2((id << 1) & 2, id & 2);\n"
    "  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n";
static const char *g_ps =
    "struct O { float c : SV_Target0; float4 col : SV_Target1; };\n"
    "O main() { O o; o.c = 0.33; o.col = float4(0.25, 0.5, 0.75, 1.0); return o; }\n";
static const char *g_ps4 =   /* target 0 written as float4 (for 4-channel target 0 formats) */
    "struct O { float4 c : SV_Target0; float4 col : SV_Target1; };\n"
    "O main() { O o; o.c = float4(0.33, 0.33, 0.33, 1); o.col = float4(0.25, 0.5, 0.75, 1.0); return o; }\n";

struct Case {
    const char *name;
    DXGI_FORMAT res0, view0;   /* DXGI_FORMAT_UNKNOWN = no target 0 */
    DXGI_FORMAT res1, view1;
    bool ps4;
};

static const Case g_cases[] = {
    {"r16unorm + rgba16f(typeless)", DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16B16A16_TYPELESS,
     DXGI_FORMAT_R16G16B16A16_FLOAT, false},
    {"r16unorm + rgba16f", DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
     DXGI_FORMAT_R16G16B16A16_FLOAT, false},
    {"(none) + rgba16f(typeless)", DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16G16B16A16_TYPELESS,
     DXGI_FORMAT_R16G16B16A16_FLOAT, false},
    {"rgba8 + rgba16f(typeless)", DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
     DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT, true},
    {"r16float + rgba16f", DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
     DXGI_FORMAT_R16G16B16A16_FLOAT, false},
    {"r16unorm + r11g11b10f", DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R11G11B10_FLOAT,
     DXGI_FORMAT_R11G11B10_FLOAT, false},
    {"r16unorm + rgba8", DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
     DXGI_FORMAT_R8G8B8A8_UNORM, false},
};

static float Half(unsigned short h)
{
    unsigned s = h >> 15, e = (h >> 10) & 31, m = h & 1023;
    if (e == 31)
        return m ? NAN : (s ? -INFINITY : INFINITY);
    float v = e ? ldexpf((float)(m | 1024), (int)e - 25) : ldexpf((float)m, -24);
    return s ? -v : v;
}

static float Small(unsigned bits, unsigned mbits)   /* unsigned 5-bit-exponent float (R11G11B10 channels) */
{
    unsigned e = bits >> mbits, m = bits & ((1u << mbits) - 1);
    if (e == 31)
        return m ? NAN : INFINITY;
    return e ? ldexpf((float)(m | (1u << mbits)), (int)e - 15 - (int)mbits) : ldexpf((float)m, -14 - (int)mbits);
}

/* the centre pixel of tex as up to 4 floats in the given (typed) format */
static int Read(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, DXGI_FORMAT fmt, float out[4])
{
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *st = NULL;
    if (FAILED(dev->CreateTexture2D(&d, NULL, &st)))
        return 0;
    ctx->CopyResource(st, tex);
    D3D11_MAPPED_SUBRESOURCE m;
    int n = 0;
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        const BYTE *p = (const BYTE *)m.pData + (d.Height / 2) * m.RowPitch;
        unsigned x = d.Width / 2;
        switch (fmt) {
        case DXGI_FORMAT_R16_UNORM: out[0] = ((const unsigned short *)p)[x] / 65535.0f; n = 1; break;
        case DXGI_FORMAT_R16_FLOAT: out[0] = Half(((const unsigned short *)p)[x]); n = 1; break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            for (int i = 0; i < 4; i++)
                out[i] = Half(((const unsigned short *)p)[x * 4 + i]);
            n = 4;
            break;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            for (int i = 0; i < 4; i++)
                out[i] = p[x * 4 + i] / 255.0f;
            n = 4;
            break;
        case DXGI_FORMAT_R11G11B10_FLOAT: {
            unsigned v = ((const unsigned *)p)[x];
            out[0] = Small(v & 0x7ff, 6);
            out[1] = Small((v >> 11) & 0x7ff, 6);
            out[2] = Small(v >> 22, 5);
            n = 3;
            break;
        }
        default: break;
        }
        ctx->Unmap(st, 0);
    }
    st->Release();
    return n;
}

struct Result {
    int n0, n1;
    float t0[4], t1[4];
    HRESULT hr;
};

static void Run(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3DBlob *vsb, ID3DBlob *psb, ID3DBlob *ps4b, const Case &c,
                Result *r)
{
    memset(r, 0, sizeof(*r));
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), NULL, &vs);
    ID3DBlob *pb = c.ps4 ? ps4b : psb;
    dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), NULL, &ps);
    ID3D11Texture2D *t[2] = {};
    ID3D11RenderTargetView *v[2] = {};
    const DXGI_FORMAT res[2] = {c.res0, c.res1}, view[2] = {c.view0, c.view1};
    for (int i = 0; i < 2; i++) {
        if (res[i] == DXGI_FORMAT_UNKNOWN)
            continue;
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = 64;
        d.Height = 64;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = res[i];
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        r->hr = dev->CreateTexture2D(&d, NULL, &t[i]);
        if (FAILED(r->hr))
            goto out;
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = view[i];
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        r->hr = dev->CreateRenderTargetView(t[i], &rd, &v[i]);
        if (FAILED(r->hr))
            goto out;
        const float zero[4] = {0, 0, 0, 0};
        ctx->ClearRenderTargetView(v[i], zero);
    }
    {
        ctx->OMSetRenderTargets(2, v, NULL);
        D3D11_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(NULL);
        ctx->VSSetShader(vs, NULL, 0);
        ctx->PSSetShader(ps, NULL, 0);
        ctx->Draw(3, 0);
        ID3D11RenderTargetView *none[2] = {};
        ctx->OMSetRenderTargets(2, none, NULL);
        if (t[0])
            r->n0 = Read(dev, ctx, t[0], view[0], r->t0);
        if (t[1])
            r->n1 = Read(dev, ctx, t[1], view[1], r->t1);
    }
out:
    for (int i = 0; i < 2; i++) {
        if (v[i]) v[i]->Release();
        if (t[i]) t[i]->Release();
    }
    if (vs) vs->Release();
    if (ps) ps->Release();
}

static bool Near(const float *a, const float *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (isnan(a[i]) || isnan(b[i]) || isinf(a[i]) || isinf(b[i]))
            return false;
        if (fabsf(a[i] - b[i]) > 0.01f)
            return false;
    }
    return true;
}

static void Print(const char *dev, const Case &c, const Result &r)
{
    printf("%-5s %-30s hr 0x%08lx t0", dev, c.name, (unsigned long)r.hr);
    for (int i = 0; i < r.n0; i++)
        printf(" %.3f", r.t0[i]);
    printf("  t1");
    for (int i = 0; i < r.n1; i++)
        printf(" %.3f", r.t1[i]);
    printf("\n");
}

int main()
{
    ID3DBlob *vsb = NULL, *psb = NULL, *ps4b = NULL, *err = NULL;
    if (FAILED(D3DCompile(g_vs, strlen(g_vs), "vs", NULL, NULL, "main", "vs_4_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(g_ps, strlen(g_ps), "ps", NULL, NULL, "main", "ps_4_0", 0, 0, &psb, &err)) ||
        FAILED(D3DCompile(g_ps4, strlen(g_ps4), "ps4", NULL, NULL, "main", "ps_4_0", 0, 0, &ps4b, &err))) {
        printf("D3DCompile failed: %s\n", err ? (const char *)err->GetBufferPointer() : "");
        return 1;
    }
    IDXGIFactory1 *f = NULL;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f);
    IDXGIAdapter1 *mali = NULL;
    for (UINT i = 0; f && f->EnumAdapters1(i, &mali) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        mali->GetDesc1(&d);
        if (wcsstr(d.Description, L"Mali"))
            break;
        mali->Release();
        mali = NULL;
    }
    ID3D11Device *dm = NULL, *dw = NULL;
    ID3D11DeviceContext *cm = NULL, *cw = NULL;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_1;
    HRESULT hm = mali ? D3D11CreateDevice(mali, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dm, NULL, &cm)
                      : E_FAIL;
    HRESULT hw = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dw, NULL, &cw);
    printf("Mali device 0x%08lx, WARP device 0x%08lx\n", (unsigned long)hm, (unsigned long)hw);
    if (FAILED(hm) || FAILED(hw))
        return 1;
    int match = 0, total = 0;
    for (const Case &c : g_cases) {
        Result rm, rw;
        Run(dm, cm, vsb, psb, ps4b, c, &rm);
        Run(dw, cw, vsb, psb, ps4b, c, &rw);
        Print("Mali", c, rm);
        Print("WARP", c, rw);
        bool ok = rm.n0 == rw.n0 && rm.n1 == rw.n1 && Near(rm.t0, rw.t0, rm.n0) && Near(rm.t1, rw.t1, rm.n1);
        printf("      -> %s\n", ok ? "match" : "DIFFER");
        match += ok;
        total++;
    }
    printf("D3DMRT %d/%d match\n", match, total);
    return match == total ? 0 : 2;
}
