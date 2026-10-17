/*
 * d3dvbring - G17 / D10 T4 (notes/g17-dxva-mfc.md): mpv's vertex streaming as a minimal repro. mpv (ra_d3d11.c
 * vbuf_upload / renderpass_run_raster) keeps ONE dynamic vertex buffer as a ring: each draw maps it with
 * WRITE_NO_OVERWRITE at the next offset (WRITE_DISCARD + offset 0 when it wraps), binds it with
 * IASetVertexBuffers(stride, offset) and calls Draw(n, 0); attributes are TEXCOORDn R32G32_FLOAT. On the Mali UMD its
 * video quad produced no fragments. Here: clear a 256x256 target red, draw a green full-target quad through each
 * pattern, read the center pixel back. PASS = green.
 *
 *   d3dvbring [warp] [shared] [scissor] [far]   (far: the vertex shader writes z = w = 1, as mpv's does)
 *     (scissor: rasterizer ScissorEnable + a full-target scissor rect, as mpv)
 *     (shared: the render target is created D3D11_RESOURCE_MISC_SHARED, like a flip-model
 *                               swap chain buffer - our UMD backs it with a shared kernel allocation)
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <d3d11.h>
#include <d3dcompiler.h>

static const char kHlslFar[] =
    "struct VI { float2 pos : TEXCOORD0; float2 uv : TEXCOORD1; };\n"
    "struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VO vs(VI i) { VO o; o.pos = float4(i.pos, 1, 1); o.uv = i.uv; return o; }\n"
    "float4 ps(VO i) : SV_Target { return float4(0, 1, 0, 1); }\n";

static const char kHlsl[] =
    "struct VI { float2 pos : TEXCOORD0; float2 uv : TEXCOORD1; };\n"
    "struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VO vs(VI i) { VO o; o.pos = float4(i.pos, 0, 1); o.uv = i.uv; return o; }\n"
    "float4 ps(VO i) : SV_Target { return float4(0, 1, 0, 1); }\n";

struct V { float x, y, u, v; };

int main(int argc, char **argv)
{
    bool warp = false, shared = false, scissor = false, farz = false;
    for (int a = 1; a < argc; a++) {
        warp = warp || !strcmp(argv[a], "warp");
        shared = shared || !strcmp(argv[a], "shared");
        scissor = scissor || !strcmp(argv[a], "scissor");
        farz = farz || !strcmp(argv[a], "far");
    }
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 }, got;
    HRESULT hr = D3D11CreateDevice(NULL, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, NULL, 0, fl, 2,
                                   D3D11_SDK_VERSION, &dev, &got, &ctx);
    printf("device 0x%08lx FL 0x%x %s%s\n", hr, got, warp ? "(WARP)" : "", shared ? " shared target" : "");
    if (FAILED(hr))
        return 1;
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    const char *src = farz ? kHlslFar : kHlsl;
    size_t srclen = strlen(src);
    if (farz)
        printf("vertex shader writes z = w = 1\n");
    if (FAILED(D3DCompile(src, srclen, NULL, NULL, NULL, "vs", "vs_4_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(src, srclen, NULL, NULL, NULL, "ps", "ps_4_0", 0, 0, &psb, &err))) {
        printf("compile failed: %s\n", err ? (const char *)err->GetBufferPointer() : "?");
        return 1;
    }
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), NULL, &vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), NULL, &ps);
    D3D11_INPUT_ELEMENT_DESC el[2] = {
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11InputLayout *il = NULL;
    dev->CreateInputLayout(el, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 256; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = shared ? D3D11_RESOURCE_MISC_SHARED : 0;
    ID3D11Texture2D *rt = NULL, *st = NULL;
    dev->CreateTexture2D(&td, NULL, &rt);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    dev->CreateTexture2D(&td, NULL, &st);
    ID3D11RenderTargetView *rtv = NULL;
    dev->CreateRenderTargetView(rt, NULL, &rtv);

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 64 * 1024; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer *vb = NULL;
    dev->CreateBuffer(&bd, NULL, &vb);

    /* a full-target quad as a 6-vertex triangle list (mpv draws triangle lists) */
    const V quad[6] = { { -1, -1, 0, 1 }, { -1, 1, 0, 0 }, { 1, 1, 1, 0 }, { -1, -1, 0, 1 }, { 1, 1, 1, 0 }, { 1, -1, 1, 1 } };
    struct Case { const char *name; UINT offset; D3D11_MAP map; } cases[] = {
        { "discard, offset 0", 0, D3D11_MAP_WRITE_DISCARD },
        { "no-overwrite, offset 96", 96, D3D11_MAP_WRITE_NO_OVERWRITE },
        { "no-overwrite, offset 4096", 4096, D3D11_MAP_WRITE_NO_OVERWRITE },
        { "no-overwrite, offset 192", 192, D3D11_MAP_WRITE_NO_OVERWRITE },
        { "discard, offset 0 again", 0, D3D11_MAP_WRITE_DISCARD },
    };
    int fails = 0;
    D3D11_VIEWPORT vp = { 0, 0, 256, 256, 0, 1 };
    ID3D11RasterizerState *rs = NULL;
    if (scissor) {
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        rd.ScissorEnable = TRUE;
        dev->CreateRasterizerState(&rd, &rs);
        printf("scissor enabled, rect 0,0-256,256\n");
    }
    for (auto &c : cases) {
        D3D11_MAPPED_SUBRESOURCE m;
        hr = ctx->Map(vb, 0, c.map, 0, &m);
        if (FAILED(hr)) {
            printf("%-28s Map 0x%08lx\n", c.name, hr);
            fails++;
            continue;
        }
        memcpy((char *)m.pData + c.offset, quad, sizeof(quad));
        ctx->Unmap(vb, 0);
        const float red[4] = { 1, 0, 0, 1 };
        ctx->ClearRenderTargetView(rtv, red);
        UINT stride = sizeof(V), off = c.offset;
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
        ctx->IASetInputLayout(il);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, NULL, 0);
        ctx->PSSetShader(ps, NULL, 0);
        ctx->RSSetViewports(1, &vp);
        if (rs) {
            D3D11_RECT sr = { 0, 0, 256, 256 };
            ctx->RSSetState(rs);
            ctx->RSSetScissorRects(1, &sr);
        }
        ctx->OMSetRenderTargets(1, &rtv, NULL);
        ctx->Draw(6, 0);
        ctx->CopyResource(st, rt);
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            UINT px = *(const UINT *)((const char *)m.pData + 128 * m.RowPitch + 128 * 4);
            ctx->Unmap(st, 0);
            bool ok = px == 0xff00ff00u;
            fails += !ok;
            printf("%-28s center 0x%08x %s\n", c.name, px, ok ? "PASS" : "FAIL");
        }
    }
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 2 : 0;
}
