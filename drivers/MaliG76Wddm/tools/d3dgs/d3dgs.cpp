/*
 * d3dgs - G25 geometry-shader check (notes/g25-geometry-shaders.md): the same draws through a GS on the Mali D3D11 UMD
 * (which emulates the geometry stage) and on WARP, rendered into a 64x64 RGBA8 target and compared pixel by pixel.
 *   sprites      16 points -> 4-vertex quads (triangle strip, size from a constant buffer) - dgVoodoo's point sprites
 *   cut          1 point -> two triangles in one strip separated by RestartStrip
 *   passthrough  2 triangles (triangle list in) -> the same triangles with inverted colour
 * A pixel differs when a channel is off by more than 2/255; a case passes with <= 1 % differing pixels (edge rounding).
 * Exit 0 = all pass. Build: build-d3dgs.cmd (x86 / arm64). SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <d3d11.h>
#include <d3dcompiler.h>

static const char *g_vs =
    "struct I { float2 p : POSITION; float4 c : COLOR; };\n"
    "struct O { float4 p : SV_Position; float4 c : COLOR; };\n"
    "O main(I i) { O o; o.p = float4(i.p, 0.5, 1); o.c = i.c; return o; }\n";
static const char *g_ps =
    "struct O { float4 p : SV_Position; float4 c : COLOR; };\n"
    "float4 main(O i) : SV_Target { return i.c; }\n";
static const char *g_gs_sprite =
    "struct O { float4 p : SV_Position; float4 c : COLOR; };\n"
    "cbuffer k : register(b0) { float s; float3 pad; };\n"
    "[maxvertexcount(4)] void main(point O i[1], inout TriangleStream<O> t) {\n"
    "  O o; o.c = i[0].c;\n"
    "  o.p = i[0].p + float4(-s,  s, 0, 0); t.Append(o);\n"
    "  o.p = i[0].p + float4( s,  s, 0, 0); t.Append(o);\n"
    "  o.p = i[0].p + float4(-s, -s, 0, 0); t.Append(o);\n"
    "  o.p = i[0].p + float4( s, -s, 0, 0); t.Append(o);\n"
    "}\n";
static const char *g_gs_cut =
    "struct O { float4 p : SV_Position; float4 c : COLOR; };\n"
    "[maxvertexcount(6)] void main(point O i[1], inout TriangleStream<O> t) {\n"
    "  O o; o.c = i[0].c;\n"
    "  o.p = i[0].p + float4(-0.9,  0.5, 0, 0); t.Append(o);\n"
    "  o.p = i[0].p + float4(-0.1,  0.5, 0, 0); t.Append(o);\n"
    "  o.p = i[0].p + float4(-0.5, -0.5, 0, 0); t.Append(o);\n"
    "  t.RestartStrip();\n"
    "  o.c = i[0].c.bgra;\n"
    "  o.p = i[0].p + float4( 0.1,  0.5, 0, 0); t.Append(o);\n"
    "  o.p = i[0].p + float4( 0.9,  0.5, 0, 0); t.Append(o);\n"
    "  o.p = i[0].p + float4( 0.5, -0.5, 0, 0); t.Append(o);\n"
    "}\n";
static const char *g_gs_pass =
    "struct O { float4 p : SV_Position; float4 c : COLOR; };\n"
    "[maxvertexcount(3)] void main(triangle O i[3], inout TriangleStream<O> t) {\n"
    "  for (int k = 0; k < 3; k++) { O o = i[k]; o.c = float4(1 - i[k].c.rgb, 1); t.Append(o); }\n"
    "}\n";

struct V { float x, y, r, g, b, a; };

struct Dev {
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    ID3D11Texture2D *rt, *st;
    ID3D11RenderTargetView *rtv;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11InputLayout *il;
    ID3D11Buffer *cb;
};

static ID3DBlob *Compile(const char *src, const char *target)
{
    ID3DBlob *b = NULL, *e = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), target, NULL, NULL, "main", target, 0, 0, &b, &e);
    if (FAILED(hr)) {
        printf("D3DCompile %s 0x%08lx %s\n", target, (unsigned long)hr, e ? (const char *)e->GetBufferPointer() : "");
        return NULL;
    }
    return b;
}

static bool Open(Dev *d, bool warp, IDXGIAdapter *ad, ID3DBlob *vsb, ID3DBlob *psb)
{
    memset(d, 0, sizeof(*d));
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_1, got;
    HRESULT hr = D3D11CreateDevice(warp ? NULL : ad, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
                                   &fl, 1, D3D11_SDK_VERSION, &d->dev, &got, &d->ctx);
    printf("%s: D3D11CreateDevice 0x%08lx FL 0x%x\n", warp ? "WARP" : "Mali", (unsigned long)hr, got);
    if (FAILED(hr))
        return false;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 64; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    hr = d->dev->CreateTexture2D(&td, NULL, &d->rt);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr)) hr = d->dev->CreateTexture2D(&td, NULL, &d->st);
    if (SUCCEEDED(hr)) hr = d->dev->CreateRenderTargetView(d->rt, NULL, &d->rtv);
    if (SUCCEEDED(hr)) hr = d->dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), NULL, &d->vs);
    if (SUCCEEDED(hr)) hr = d->dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), NULL, &d->ps);
    D3D11_INPUT_ELEMENT_DESC ie[2] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (SUCCEEDED(hr)) hr = d->dev->CreateInputLayout(ie, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &d->il);
    float k[4] = { 0.08f, 0, 0, 0 };
    D3D11_BUFFER_DESC bd = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { k, 0, 0 };
    if (SUCCEEDED(hr)) hr = d->dev->CreateBuffer(&bd, &init, &d->cb);
    if (FAILED(hr))
        printf("  setup 0x%08lx\n", (unsigned long)hr);
    return SUCCEEDED(hr);
}

/* Draw verts with topology through gs (compiled blob), read the 64x64 target into px. */
static bool Run(Dev *d, ID3DBlob *gsb, const V *verts, UINT n, D3D11_PRIMITIVE_TOPOLOGY topo, unsigned *px)
{
    ID3D11GeometryShader *gs = NULL;
    HRESULT hr = d->dev->CreateGeometryShader(gsb->GetBufferPointer(), gsb->GetBufferSize(), NULL, &gs);
    if (FAILED(hr)) {
        printf("  CreateGeometryShader 0x%08lx\n", (unsigned long)hr);
        return false;
    }
    D3D11_BUFFER_DESC bd = { n * (UINT)sizeof(V), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { verts, 0, 0 };
    ID3D11Buffer *vb = NULL;
    hr = d->dev->CreateBuffer(&bd, &init, &vb);
    if (FAILED(hr)) {
        gs->Release();
        return false;
    }
    float clear[4] = { 0, 0, 0, 1 };
    d->ctx->ClearRenderTargetView(d->rtv, clear);
    d->ctx->OMSetRenderTargets(1, &d->rtv, NULL);
    D3D11_VIEWPORT vp = { 0, 0, 64, 64, 0, 1 };
    d->ctx->RSSetViewports(1, &vp);
    UINT stride = sizeof(V), off = 0;
    d->ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
    d->ctx->IASetInputLayout(d->il);
    d->ctx->IASetPrimitiveTopology(topo);
    d->ctx->VSSetShader(d->vs, NULL, 0);
    d->ctx->GSSetShader(gs, NULL, 0);
    d->ctx->GSSetConstantBuffers(0, 1, &d->cb);
    d->ctx->PSSetShader(d->ps, NULL, 0);
    d->ctx->Draw(n, 0);
    d->ctx->GSSetShader(NULL, NULL, 0);
    d->ctx->CopyResource(d->st, d->rt);
    D3D11_MAPPED_SUBRESOURCE map;
    hr = d->ctx->Map(d->st, 0, D3D11_MAP_READ, 0, &map);
    if (SUCCEEDED(hr)) {
        for (int y = 0; y < 64; y++)
            memcpy(px + y * 64, (unsigned char *)map.pData + y * map.RowPitch, 64 * 4);
        d->ctx->Unmap(d->st, 0);
    }
    vb->Release();
    gs->Release();
    return SUCCEEDED(hr);
}

static int Diff(unsigned a, unsigned b)
{
    for (int c = 0; c < 4; c++) {
        int x = (a >> (8 * c)) & 255, y = (b >> (8 * c)) & 255;
        if (abs(x - y) > 2)
            return 1;
    }
    return 0;
}

int main()
{
    setvbuf(stdout, NULL, _IONBF, 0);
    ID3DBlob *vsb = Compile(g_vs, "vs_4_0"), *psb = Compile(g_ps, "ps_4_0");
    ID3DBlob *gsb[3] = { Compile(g_gs_sprite, "gs_4_0"), Compile(g_gs_cut, "gs_4_0"), Compile(g_gs_pass, "gs_4_0") };
    if (!vsb || !psb || !gsb[0] || !gsb[1] || !gsb[2])
        return 1;
    IDXGIFactory1 *fac = NULL;
    IDXGIAdapter1 *ad = NULL, *pick = NULL;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac);
    for (UINT i = 0; fac && fac->EnumAdapters1(i, &ad) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 dd;
        ad->GetDesc1(&dd);
        if (!pick && !(dd.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && dd.VendorId != 0x1414)
            pick = ad;
        else
            ad->Release();
    }
    Dev mali, warp;
    if (!pick || !Open(&mali, false, pick, vsb, psb) || !Open(&warp, true, NULL, vsb, psb))
        return 2;

    V pts[16], one = { 0, 0, 1, 0.5f, 0.25f, 1 }, tris[6];
    for (int i = 0; i < 16; i++) {
        V v = { -0.75f + 0.5f * (i % 4), -0.75f + 0.5f * (i / 4), (i & 1) ? 1.f : 0.2f, (i & 2) ? 1.f : 0.3f,
                (i & 4) ? 1.f : 0.4f, 1 };
        pts[i] = v;
    }
    V t[6] = { { -0.9f, -0.9f, 1, 0, 0, 1 }, { -0.9f, 0.9f, 0, 1, 0, 1 }, { 0.0f, -0.9f, 0, 0, 1, 1 },
               { 0.1f, 0.9f, 1, 1, 0, 1 }, { 0.9f, 0.9f, 0, 1, 1, 1 }, { 0.9f, -0.9f, 1, 0, 1, 1 } };
    memcpy(tris, t, sizeof t);
    struct Case { const char *name; const V *v; UINT n; D3D11_PRIMITIVE_TOPOLOGY topo; } cases[3] = {
        { "sprites", pts, 16, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST },
        { "cut", &one, 1, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST },
        { "passthrough", tris, 6, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST },
    };
    int fails = 0;
    static unsigned pm[64 * 64], pw[64 * 64];
    for (int c = 0; c < 3; c++) {
        bool okm = Run(&mali, gsb[c], cases[c].v, cases[c].n, cases[c].topo, pm);
        bool okw = Run(&warp, gsb[c], cases[c].v, cases[c].n, cases[c].topo, pw);
        if (!okm || !okw) {
            printf("%-12s FAILED to run\n", cases[c].name);
            fails++;
            continue;
        }
        int bad = 0, lit_m = 0, lit_w = 0;
        for (int i = 0; i < 64 * 64; i++) {
            bad += Diff(pm[i], pw[i]);
            lit_m += (pm[i] & 0xffffff) != 0;
            lit_w += (pw[i] & 0xffffff) != 0;
        }
        bool ok = bad * 100 <= 64 * 64;
        printf("%-12s %s: %d of 4096 pixels differ, lit Mali %d / WARP %d\n", cases[c].name, ok ? "ok" : "DIFF", bad,
               lit_m, lit_w);
        fails += !ok;
    }
    printf("%d of 3 cases differ -> %s\n", fails, fails ? "FAIL" : "PASS");
    return fails ? 3 : 0;
}
