/*
 * d3dso - G25.6 stream-output check (notes/g25-geometry-shaders.md): D3D10-style stream output without a geometry
 * shader (CreateGeometryShaderWithStreamOutput from the VERTEX shader's bytecode) on the Mali D3D11 UMD and on WARP;
 * the stream-output buffers are read back and compared float by float.
 *   all      SV_Position + COLOR captured, 32 bytes per vertex (9 vertices of a triangle list)
 *   subset   COLOR.zw then SV_Position.xy, 16 bytes per vertex (reordered components, a register subset)
 * (a two-buffer case was dropped: CreateGeometryShaderWithStreamOutput refused it on WARP too at FL 10_1)
 * Exit 0 = all match. Build: build-d3dso.cmd (x86 / arm64). SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <d3d11.h>
#include <d3dcompiler.h>

static const char *g_vs =
    "struct I { float2 p : POSITION; float4 c : COLOR; };\n"
    "struct O { float4 p : SV_Position; float4 c : COLOR; };\n"
    "O main(I i, uint id : SV_VertexID) { O o; o.p = float4(i.p, 0.25 * id, 1); o.c = i.c * 2; return o; }\n";

struct V { float x, y, r, g, b, a; };

struct Dev {
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    ID3D11VertexShader *vs;
    ID3D11InputLayout *il;
    ID3D11Buffer *vb;
};

static bool Open(Dev *d, bool warp, IDXGIAdapter *ad, ID3DBlob *vsb, const V *verts, UINT n)
{
    memset(d, 0, sizeof(*d));
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_1, got;
    HRESULT hr = D3D11CreateDevice(warp ? NULL : ad, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
                                   &fl, 1, D3D11_SDK_VERSION, &d->dev, &got, &d->ctx);
    printf("%s: D3D11CreateDevice 0x%08lx FL 0x%x\n", warp ? "WARP" : "Mali", (unsigned long)hr, got);
    if (FAILED(hr))
        return false;
    hr = d->dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), NULL, &d->vs);
    D3D11_INPUT_ELEMENT_DESC ie[2] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (SUCCEEDED(hr)) hr = d->dev->CreateInputLayout(ie, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &d->il);
    D3D11_BUFFER_DESC bd = { n * (UINT)sizeof(V), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { verts, 0, 0 };
    if (SUCCEEDED(hr)) hr = d->dev->CreateBuffer(&bd, &init, &d->vb);
    if (FAILED(hr))
        printf("  setup 0x%08lx\n", (unsigned long)hr);
    return SUCCEEDED(hr);
}

/* Stream out n vertices through the VS with decl into nbuf buffers of the given strides; read them into out[b]. */
static bool Run(Dev *d, ID3DBlob *vsb, const D3D11_SO_DECLARATION_ENTRY *decl, UINT ndecl, const UINT *strides, UINT nbuf,
                UINT n, float out[2][128])
{
    ID3D11GeometryShader *gs = NULL;
    HRESULT hr = d->dev->CreateGeometryShaderWithStreamOutput(vsb->GetBufferPointer(), vsb->GetBufferSize(), decl, ndecl,
                                                              strides, nbuf, 0 /* FL 10_x: stream 0 */, NULL, &gs);
    if (FAILED(hr)) {
        printf("  CreateGeometryShaderWithStreamOutput 0x%08lx\n", (unsigned long)hr);
        return false;
    }
    ID3D11Buffer *so[2] = {}, *st[2] = {};
    for (UINT b = 0; b < nbuf; b++) {
        D3D11_BUFFER_DESC bd = { 512, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, 0 };
        d->dev->CreateBuffer(&bd, NULL, &so[b]);
        D3D11_BUFFER_DESC sd = { 512, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 };
        d->dev->CreateBuffer(&sd, NULL, &st[b]);
    }
    UINT offs[2] = { 0, 0 }, vstride = sizeof(V), voff = 0;
    d->ctx->IASetVertexBuffers(0, 1, &d->vb, &vstride, &voff);
    d->ctx->IASetInputLayout(d->il);
    d->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d->ctx->VSSetShader(d->vs, NULL, 0);
    d->ctx->GSSetShader(gs, NULL, 0);
    d->ctx->PSSetShader(NULL, NULL, 0);
    d->ctx->SOSetTargets(nbuf, so, offs);
    d->ctx->Draw(n, 0);
    ID3D11Buffer *none[2] = {};
    d->ctx->SOSetTargets(2, none, offs);
    d->ctx->GSSetShader(NULL, NULL, 0);
    bool ok = true;
    for (UINT b = 0; b < nbuf; b++) {
        d->ctx->CopyResource(st[b], so[b]);
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(d->ctx->Map(st[b], 0, D3D11_MAP_READ, 0, &map))) {
            memcpy(out[b], map.pData, 512);
            d->ctx->Unmap(st[b], 0);
        } else {
            ok = false;
        }
        so[b]->Release();
        st[b]->Release();
    }
    gs->Release();
    return ok;
}

int main()
{
    setvbuf(stdout, NULL, _IONBF, 0);
    ID3DBlob *vsb = NULL, *err = NULL;
    if (FAILED(D3DCompile(g_vs, strlen(g_vs), "vs", NULL, NULL, "main", "vs_4_0", 0, 0, &vsb, &err))) {
        printf("D3DCompile %s\n", err ? (const char *)err->GetBufferPointer() : "");
        return 1;
    }
    V verts[9];
    for (int i = 0; i < 9; i++) {
        V v = { -0.8f + 0.2f * i, 0.5f - 0.1f * i, 0.1f * i, 0.05f * i, 1.0f - 0.1f * i, 0.5f };
        verts[i] = v;
    }
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
    if (!pick || !Open(&mali, false, pick, vsb, verts, 9) || !Open(&warp, true, NULL, vsb, verts, 9))
        return 2;

    D3D11_SO_DECLARATION_ENTRY all[2] = { { 0, "SV_POSITION", 0, 0, 4, 0 }, { 0, "COLOR", 0, 0, 4, 0 } };
    D3D11_SO_DECLARATION_ENTRY subset[2] = { { 0, "COLOR", 0, 2, 2, 0 }, { 0, "SV_POSITION", 0, 0, 2, 0 } };
    UINT s32[1] = { 32 }, s16[1] = { 16 };
    struct Case { const char *name; const D3D11_SO_DECLARATION_ENTRY *d; UINT nd; const UINT *st; UINT nb; UINT floats[2]; }
        cases[2] = {
            { "all", all, 2, s32, 1, { 72, 0 } },
            { "subset", subset, 2, s16, 1, { 36, 0 } },
        };
    int fails = 0;
    for (int c = 0; c < 2; c++) {
        static float om[2][128], ow[2][128];
        memset(om, 0, sizeof om);
        memset(ow, 0, sizeof ow);
        bool okm = Run(&mali, vsb, cases[c].d, cases[c].nd, cases[c].st, cases[c].nb, 9, om);
        bool okw = Run(&warp, vsb, cases[c].d, cases[c].nd, cases[c].st, cases[c].nb, 9, ow);
        if (!okm || !okw) {
            printf("%-8s FAILED to run\n", cases[c].name);
            fails++;
            continue;
        }
        int bad = 0, first = -1;
        for (UINT b = 0; b < cases[c].nb; b++)
            for (UINT i = 0; i < cases[c].floats[b]; i++)
                if (fabsf(om[b][i] - ow[b][i]) > 1e-5f) {
                    if (first < 0)
                        first = (int)(b * 1000 + i);
                    bad++;
                }
        printf("%-8s %s: %d floats differ%s", cases[c].name, bad ? "DIFF" : "ok", bad, bad ? "" : "\n");
        if (bad) {
            int b = first / 1000, i = first % 1000;
            printf(" (first buffer %d float %d: Mali %g WARP %g)\n", b, i, om[b][i], ow[b][i]);
        }
        fails += bad != 0;
    }
    printf("%d of 2 cases differ -> %s\n", fails, fails ? "FAIL" : "PASS");
    return fails ? 3 : 0;
}
