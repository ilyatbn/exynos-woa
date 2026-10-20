/*
 * d3dtess - G34 F11.10 hull / domain shader check (notes/g34-fl11.md): tessellated patches drawn on the Mali D3D11
 * UMD and on WARP at feature level 11_0 (the process sets the UMD's D3D10UMD_FL11_0=1 diagnosis switch for itself;
 * "-native" does not), 64x64 RGBA8 target, compared pixel by pixel. The pixel shader colours by a non-linear function
 * of the domain location computed per tessellated vertex, so the picture depends on the tessellation pattern.
 *   tri_int      tri domain, integer partitioning, constant factors from a constant buffer, pass-through control points
 *   tri_frac     tri domain, fractional_odd, factors 2.3 / 3.6 / 4.1, inside 3.3
 *   quad_int     quad domain, integer, control-point phase that offsets the points
 *   quad_feven   quad domain, fractional_even, factors from an instanced (4x) fork phase reading the control points
 *   iso          isoline domain (lines), density 4, detail 6 (analytic check: WARP's isolines are not repeatable)
 *   plainline    a plain line list (no hull / domain shader), to separate line rasterization from tessellation
 *   pc_custom    tri domain, a custom patch constant (colour) read in the domain shader
 * Usage: d3dtess [-native] [test ...]. Exit 0 = every selected test matches (<= 0.5 % pixels off by more than 3/255).
 * Build: build-d3dtess.cmd.
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <d3d11.h>
#include <d3dcompiler.h>

static const char g_vs[] =
    "struct V { float3 p : POSITION; };\n"
    "struct O { float3 p : CPOS; };\n"
    "O main(V v) { O o; o.p = v.p; return o; }\n";

static const char g_vs_plain[] =
    "struct O { float4 pos : SV_Position; float3 c : COLOR0; };\n"
    "O main(float3 p : POSITION, uint id : SV_VertexID) { O o; o.pos = float4(p.xy, 0.5, 1);\n"
    "  o.c = float3(id * 0.12, 1 - id * 0.1, 0.5); return o; }\n";

static const char g_ps[] =
    "float4 main(float4 pos : SV_Position, float3 c : COLOR0) : SV_Target { return float4(c, 1); }\n";

struct Test {
    const char *name;
    int cps;              /* control points per patch */
    int npatch;
    const char *hs, *ds;
    bool lines;
};

#define TRI_HS_COMMON \
    "cbuffer F : register(b0) { float4 edge; float4 inside; };\n" \
    "struct C { float3 p : CPOS; };\n" \
    "struct PC { float e[3] : SV_TessFactor; float i : SV_InsideTessFactor; };\n" \
    "PC pcf(InputPatch<C, 3> ip, uint id : SV_PrimitiveID) {\n" \
    "  PC o; o.e[0] = edge.x; o.e[1] = edge.y; o.e[2] = edge.z; o.i = inside.x; return o; }\n"

#define TRI_DS \
    "struct C { float3 p : CPOS; };\n" \
    "struct PC { float e[3] : SV_TessFactor; float i : SV_InsideTessFactor; };\n" \
    "struct O { float4 pos : SV_Position; float3 c : COLOR0; };\n" \
    "[domain(\"tri\")] O main(PC pc, float3 uvw : SV_DomainLocation, const OutputPatch<C, 3> cp) {\n" \
    "  O o; float3 p = cp[0].p * uvw.x + cp[1].p * uvw.y + cp[2].p * uvw.z;\n" \
    "  o.pos = float4(p.xy, 0.5, 1);\n" \
    "  o.c = frac(uvw * float3(3.7, 5.3, 7.1)); return o; }\n"

static const Test g_tests[] = {
    { "tri_int", 3, 2,
      TRI_HS_COMMON
      "[domain(\"tri\")] [partitioning(\"integer\")] [outputtopology(\"triangle_cw\")] [outputcontrolpoints(3)]\n"
      "[patchconstantfunc(\"pcf\")] C main(InputPatch<C, 3> ip, uint i : SV_OutputControlPointID) { return ip[i]; }\n",
      TRI_DS, false },
    { "tri_frac", 3, 2,
      TRI_HS_COMMON
      "[domain(\"tri\")] [partitioning(\"fractional_odd\")] [outputtopology(\"triangle_cw\")] [outputcontrolpoints(3)]\n"
      "[patchconstantfunc(\"pcf\")] C main(InputPatch<C, 3> ip, uint i : SV_OutputControlPointID) { return ip[i]; }\n",
      TRI_DS, false },
    { "quad_int", 4, 1,
      "cbuffer F : register(b0) { float4 edge; float4 inside; };\n"
      "struct C { float3 p : CPOS; };\n"
      "struct PC { float e[4] : SV_TessFactor; float i[2] : SV_InsideTessFactor; };\n"
      "PC pcf(InputPatch<C, 4> ip) { PC o; o.e[0] = edge.x; o.e[1] = edge.y; o.e[2] = edge.z; o.e[3] = edge.w;\n"
      "  o.i[0] = inside.x; o.i[1] = inside.y; return o; }\n"
      "[domain(\"quad\")] [partitioning(\"integer\")] [outputtopology(\"triangle_ccw\")] [outputcontrolpoints(4)]\n"
      "[patchconstantfunc(\"pcf\")] C main(InputPatch<C, 4> ip, uint i : SV_OutputControlPointID) {\n"
      "  C o; o.p = ip[i].p * 0.9 + float3(0.05 * i, -0.03 * i, 0); return o; }\n",
      "struct C { float3 p : CPOS; };\n"
      "struct PC { float e[4] : SV_TessFactor; float i[2] : SV_InsideTessFactor; };\n"
      "struct O { float4 pos : SV_Position; float3 c : COLOR0; };\n"
      "[domain(\"quad\")] O main(PC pc, float2 uv : SV_DomainLocation, const OutputPatch<C, 4> cp) {\n"
      "  O o; float3 a = lerp(cp[0].p, cp[1].p, uv.x), b = lerp(cp[3].p, cp[2].p, uv.x);\n"
      "  float3 p = lerp(a, b, uv.y); o.pos = float4(p.xy, 0.5, 1);\n"
      "  o.c = float3(frac(uv * float2(4.3, 6.7)), frac(uv.x * uv.y * 9.0)); return o; }\n", false },
    { "quad_feven", 4, 1,
      "struct C { float3 p : CPOS; };\n"
      "struct PC { float e[4] : SV_TessFactor; float i[2] : SV_InsideTessFactor; };\n"
      "PC pcf(InputPatch<C, 4> ip) { PC o;\n"
      "  [unroll] for (int k = 0; k < 4; k++) o.e[k] = 2.2 + length(ip[k].p.xy - ip[(k + 1) & 3].p.xy) * 3.1;\n"
      "  o.i[0] = 3.4; o.i[1] = 4.6; return o; }\n"
      "[domain(\"quad\")] [partitioning(\"fractional_even\")] [outputtopology(\"triangle_cw\")]\n"
      "[outputcontrolpoints(4)] [patchconstantfunc(\"pcf\")]\n"
      "C main(InputPatch<C, 4> ip, uint i : SV_OutputControlPointID) { return ip[i]; }\n",
      "struct C { float3 p : CPOS; };\n"
      "struct PC { float e[4] : SV_TessFactor; float i[2] : SV_InsideTessFactor; };\n"
      "struct O { float4 pos : SV_Position; float3 c : COLOR0; };\n"
      "[domain(\"quad\")] O main(PC pc, float2 uv : SV_DomainLocation, const OutputPatch<C, 4> cp) {\n"
      "  O o; float3 a = lerp(cp[0].p, cp[1].p, uv.x), b = lerp(cp[3].p, cp[2].p, uv.x);\n"
      "  float3 p = lerp(a, b, uv.y); o.pos = float4(p.xy, 0.5, 1);\n"
      "  o.c = float3(frac(uv * float2(5.1, 3.9)), 0.5); return o; }\n", false },
    { "iso", 2, 1,
      "struct C { float3 p : CPOS; };\n"
      "struct PC { float e[2] : SV_TessFactor; };\n"
      "PC pcf() { PC o; o.e[0] = 4; o.e[1] = 6; return o; }\n"
      "[domain(\"isoline\")] [partitioning(\"integer\")] [outputtopology(\"line\")] [outputcontrolpoints(2)]\n"
      "[patchconstantfunc(\"pcf\")] C main(InputPatch<C, 2> ip, uint i : SV_OutputControlPointID) { return ip[i]; }\n",
      "struct C { float3 p : CPOS; };\n"
      "struct PC { float e[2] : SV_TessFactor; };\n"
      "struct O { float4 pos : SV_Position; float3 c : COLOR0; };\n"
      "[domain(\"isoline\")] O main(PC pc, float2 uv : SV_DomainLocation, const OutputPatch<C, 2> cp) {\n"
      "  O o; float3 p = lerp(cp[0].p, cp[1].p, uv.x); p.y += uv.y * 1.2 - 0.6;\n"
      "  o.pos = float4(p.xy, 0.5, 1); o.c = float3(uv, 1); return o; }\n", true },
    { "plainline", 2, 4, NULL, NULL, true },   /* no tessellation: a line list through a plain VS (rasterization A/B) */
    { "pc_custom", 3, 2,
      "struct C { float3 p : CPOS; };\n"
      "struct PC { float e[3] : SV_TessFactor; float i : SV_InsideTessFactor; float3 col : PCOL; };\n"
      "PC pcf(InputPatch<C, 3> ip, uint id : SV_PrimitiveID) {\n"
      "  PC o; o.e[0] = 2; o.e[1] = 3; o.e[2] = 4; o.i = 3; o.col = float3(id * 0.5, 0.25, frac(ip[0].p.x * 3)); return o; }\n"
      "[domain(\"tri\")] [partitioning(\"integer\")] [outputtopology(\"triangle_cw\")] [outputcontrolpoints(3)]\n"
      "[patchconstantfunc(\"pcf\")] C main(InputPatch<C, 3> ip, uint i : SV_OutputControlPointID) { return ip[i]; }\n",
      "struct C { float3 p : CPOS; };\n"
      "struct PC { float e[3] : SV_TessFactor; float i : SV_InsideTessFactor; float3 col : PCOL; };\n"
      "struct O { float4 pos : SV_Position; float3 c : COLOR0; };\n"
      "[domain(\"tri\")] O main(PC pc, float3 uvw : SV_DomainLocation, const OutputPatch<C, 3> cp) {\n"
      "  O o; float3 p = cp[0].p * uvw.x + cp[1].p * uvw.y + cp[2].p * uvw.z;\n"
      "  o.pos = float4(p.xy, 0.5, 1); o.c = pc.col * (0.5 + 0.5 * frac(uvw.y * 4.0)); return o; }\n", false },
};
static const int NTEST = sizeof(g_tests) / sizeof(g_tests[0]);

static ID3DBlob *Compile(const char *src, const char *target)
{
    ID3DBlob *b = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), target, NULL, NULL, "main", target, 0, 0, &b, &err);
    if (FAILED(hr)) {
        printf("D3DCompile %s 0x%08lx %s\n", target, (unsigned long)hr, err ? (const char *)err->GetBufferPointer() : "");
        return NULL;
    }
    return b;
}

static bool Render(ID3D11Device *dev, ID3D11DeviceContext *ctx, const Test *t, ID3DBlob *vsb, ID3DBlob *psb,
                   ID3DBlob *hsb, ID3DBlob *dsb, UINT *out)
{
    HRESULT hr;
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3D11HullShader *hs = NULL;
    ID3D11DomainShader *ds = NULL;
    ID3D11InputLayout *il = NULL;
    hr = dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), NULL, &vs);
    if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), NULL, &ps);
    if (SUCCEEDED(hr) && hsb) hr = dev->CreateHullShader(hsb->GetBufferPointer(), hsb->GetBufferSize(), NULL, &hs);
    if (SUCCEEDED(hr) && dsb) hr = dev->CreateDomainShader(dsb->GetBufferPointer(), dsb->GetBufferSize(), NULL, &ds);
    D3D11_INPUT_ELEMENT_DESC ie = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    if (SUCCEEDED(hr)) hr = dev->CreateInputLayout(&ie, 1, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il);
    if (FAILED(hr)) { printf("    shaders 0x%08lx\n", (unsigned long)hr); return false; }

    float v[2 * 4 * 3];
    int nv = t->cps * t->npatch;
    if (t->cps == 3) {
        float tri[18] = { -0.9f, -0.8f, 0, -0.6f, 0.9f, 0, 0.3f, -0.7f, 0,   0.0f, 0.95f, 0, 0.92f, 0.5f, 0, 0.6f, -0.9f, 0 };
        memcpy(v, tri, sizeof tri);
    } else if (t->cps == 4) {
        float q[12] = { -0.85f, -0.8f, 0, 0.8f, -0.9f, 0, 0.9f, 0.85f, 0, -0.75f, 0.8f, 0 };
        memcpy(v, q, sizeof q);
    } else if (!t->hs) {
        float l[24] = { -0.9f, -0.5f, 0, 0.9f, -0.4f, 0, -0.8f, 0.3f, 0, 0.7f, 0.6f, 0,
                        -0.9f, 0.8f, 0, 0.9f, 0.85f, 0, -0.2f, -0.9f, 0, 0.3f, 0.9f, 0 };
        memcpy(v, l, sizeof l);
    } else {
        float l[6] = { -0.9f, 0.0f, 0, 0.9f, 0.1f, 0 };
        memcpy(v, l, sizeof l);
    }
    D3D11_BUFFER_DESC vbd = { (UINT)(nv * 12), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA vsd = { v, 0, 0 };
    ID3D11Buffer *vb = NULL, *cb = NULL;
    hr = dev->CreateBuffer(&vbd, &vsd, &vb);
    float cbd[8] = { 3, 3, 3, 3, 3, 3, 0, 0 };
    if (!strcmp(t->name, "tri_frac")) { cbd[0] = 2.3f; cbd[1] = 3.6f; cbd[2] = 4.1f; cbd[4] = 3.3f; }
    if (!strcmp(t->name, "quad_int")) { cbd[0] = 2; cbd[1] = 3; cbd[2] = 4; cbd[3] = 5; cbd[4] = 3; cbd[5] = 4; }
    D3D11_BUFFER_DESC cbdsc = { 32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA csd = { cbd, 0, 0 };
    if (SUCCEEDED(hr)) hr = dev->CreateBuffer(&cbdsc, &csd, &cb);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 64; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *rt = NULL, *st = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    if (SUCCEEDED(hr)) hr = dev->CreateTexture2D(&td, NULL, &rt);
    if (SUCCEEDED(hr)) hr = dev->CreateRenderTargetView(rt, NULL, &rtv);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr)) hr = dev->CreateTexture2D(&td, NULL, &st);
    if (FAILED(hr)) { printf("    resources 0x%08lx\n", (unsigned long)hr); return false; }

    float clear[4] = { 0, 0, 0, 1 };
    ctx->ClearRenderTargetView(rtv, clear);
    D3D11_VIEWPORT vp = { 0, 0, 64, 64, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ID3D11RasterizerState *rs = NULL;
    dev->CreateRasterizerState(&rd, &rs);
    ctx->RSSetState(rs);
    UINT stride = 12, off = 0;
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
    ctx->IASetInputLayout(il);
    ctx->IASetPrimitiveTopology(t->hs ? (D3D11_PRIMITIVE_TOPOLOGY)(D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + t->cps - 1)
                                      : D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    ctx->VSSetShader(vs, NULL, 0);
    ctx->HSSetShader(hs, NULL, 0);
    ctx->HSSetConstantBuffers(0, 1, &cb);
    ctx->DSSetShader(ds, NULL, 0);
    ctx->PSSetShader(ps, NULL, 0);
    ctx->OMSetRenderTargets(1, &rtv, NULL);
    ctx->Draw(nv, 0);
    ctx->HSSetShader(NULL, NULL, 0);
    ctx->DSSetShader(NULL, NULL, 0);
    ctx->CopyResource(st, rt);
    D3D11_MAPPED_SUBRESOURCE m;
    hr = ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) { printf("    Map 0x%08lx\n", (unsigned long)hr); return false; }
    for (UINT y = 0; y < 64; y++)
        memcpy(out + y * 64, (const BYTE *)m.pData + y * m.RowPitch, 256);
    ctx->Unmap(st, 0);
    rs->Release(); st->Release(); rtv->Release(); rt->Release(); cb->Release(); vb->Release();
    il->Release(); if (ds) ds->Release(); if (hs) hs->Release(); ps->Release(); vs->Release();
    return true;
}

static bool Open(bool warp, IDXGIAdapter *ad, ID3D11Device **dev, ID3D11DeviceContext **ctx)
{
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0, got = (D3D_FEATURE_LEVEL)0;
    HRESULT hr = D3D11CreateDevice(warp ? NULL : ad, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
                                   &fl, 1, D3D11_SDK_VERSION, dev, &got, ctx);
    printf("%s: D3D11CreateDevice(FL 11_0) 0x%08lx FL 0x%x\n", warp ? "WARP" : "Mali", (unsigned long)hr, got);
    return SUCCEEDED(hr);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int first_sel = 1;
    if (argc > 1 && !strcmp(argv[1], "-dis")) {   /* print fxc's disassembly of every test's HS and DS */
        for (int i = 0; i < NTEST; i++) {
            const char *srcs[2] = { g_tests[i].hs, g_tests[i].ds }, *tg[2] = { "hs_5_0", "ds_5_0" };
            for (int k = 0; k < 2 && srcs[0]; k++) {
                ID3DBlob *b = Compile(srcs[k], tg[k]), *d = NULL;
                if (b && SUCCEEDED(D3DDisassemble(b->GetBufferPointer(), b->GetBufferSize(), 0, NULL, &d)))
                    printf("==== %s %s\n%s\n", g_tests[i].name, tg[k], (const char *)d->GetBufferPointer());
            }
        }
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "-native"))
        first_sel = 2;
    else {
        SetEnvironmentVariableA("D3D10UMD_FL11_0", "1");
        _putenv("D3D10UMD_FL11_0=1");
    }
    ID3DBlob *vsb = Compile(g_vs, "vs_5_0"), *psb = Compile(g_ps, "ps_5_0"), *vsplain = Compile(g_vs_plain, "vs_5_0");
    if (!vsb || !psb || !vsplain)
        return 1;
    IDXGIFactory1 *fac = NULL;
    IDXGIAdapter1 *ad = NULL, *pick = NULL;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac);
    for (UINT i = 0; fac && fac->EnumAdapters1(i, &ad) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        ad->GetDesc1(&d);
        if (!pick && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId != 0x1414)
            pick = ad;
        else
            ad->Release();
    }
    ID3D11Device *md = NULL, *wd = NULL;
    ID3D11DeviceContext *mc = NULL, *wc = NULL;
    if (!pick || !Open(false, pick, &md, &mc) || !Open(true, NULL, &wd, &wc))
        return 2;
    int fails = 0, run = 0;
    static UINT om[64 * 64], ow[64 * 64];
    for (int i = 0; i < NTEST; i++) {
        const Test *t = &g_tests[i];
        bool sel = argc <= first_sel;
        for (int a = first_sel; a < argc; a++)
            sel |= !strcmp(argv[a], t->name);
        if (!sel)
            continue;
        run++;
        ID3DBlob *hsb = t->hs ? Compile(t->hs, "hs_5_0") : NULL, *dsb = t->ds ? Compile(t->ds, "ds_5_0") : NULL;
        if (t->hs && (!hsb || !dsb)) { fails++; continue; }
        ID3DBlob *tvs = t->hs ? vsb : vsplain;
        printf("%-11s running...\n", t->name);
        memset(om, 0xCD, sizeof om);
        memset(ow, 0xAB, sizeof ow);
        bool okw = Render(wd, wc, t, tvs, psb, hsb, dsb, ow), okm = Render(md, mc, t, tvs, psb, hsb, dsb, om);
        UINT bad = 0, lit = 0, first = 0;
        if (okm && okw)
            for (UINT k = 0; k < 64 * 64; k++) {
                if ((ow[k] & 0xffffff) != 0)
                    lit++;
                for (int c = 0; c < 3; c++) {
                    int d = (int)((om[k] >> (8 * c)) & 255) - (int)((ow[k] >> (8 * c)) & 255);
                    if (d > 3 || d < -3) {
                        if (!bad++)
                            first = k;
                        break;
                    }
                }
            }
        bool ok = okm && okw && bad * 200 <= 64 * 64;
        if (!strcmp(t->name, "iso") && okm) {
            /* WARP's isoline output is not repeatable (56 / 0 lit pixels in consecutive runs of the same input), so
             * the Mali picture is checked analytically: line k (v = k / 4, density 4) runs from x -0.9 to 0.9 with
             * y = 0.1 u + 1.2 v - 0.6; every column 6..57 must have a lit pixel within one row of each line, and no
             * pixel farther than two rows from every line may be lit */
            UINT miss = 0, stray = 0;
            for (int x = 0; x < 64; x++) {
                float px = (x + 0.5f) / 32.0f - 1.0f, u = (px + 0.9f) / 1.8f;
                float ys[4];
                for (int k = 0; k < 4; k++)
                    ys[k] = (1.0f - (0.1f * u + 1.2f * k / 4.0f - 0.6f)) * 32.0f - 0.5f;
                for (int k = 0; k < 4 && x >= 6 && x <= 57; k++) {
                    bool hit = false;
                    for (int dy = -1; dy <= 1; dy++) {
                        int y = (int)(ys[k] + 0.5f) + dy;
                        if (y >= 0 && y < 64 && (om[y * 64 + x] & 0xffffff))
                            hit = true;
                    }
                    miss += !hit;
                }
                for (int y = 0; y < 64; y++) {
                    if (!(om[y * 64 + x] & 0xffffff))
                        continue;
                    bool close_to = false;
                    for (int k = 0; k < 4; k++)
                        close_to |= fabsf(y - ys[k]) <= 2.0f;
                    stray += !close_to;
                }
            }
            ok = !miss && !stray;
            printf("%-11s %s: analytic check, %u line pixels missing, %u stray lit pixels\n", t->name, ok ? "ok" : "DIFF",
                   miss, stray);
            fails += !ok;
            continue;
        }
        printf("%-11s %s: %u of 4096 pixels differ (WARP lit %u)", t->name, ok ? "ok" : "DIFF", bad, lit);
        if (bad)
            printf(", first (%u,%u) Mali %08x WARP %08x", first % 64, first / 64, om[first], ow[first]);
        if (!okm || !okw)
            printf(" (did not run on %s)", !okm ? "Mali" : "WARP");
        printf("\n");
        fails += !ok;
        if (hsb) hsb->Release();
        if (dsb) dsb->Release();
    }
    printf("%d of %d tests differ -> %s\n", fails, run, fails ? "FAIL" : "PASS");
    return fails ? 3 : 0;
}
