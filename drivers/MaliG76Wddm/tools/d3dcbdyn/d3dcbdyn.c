/*
 * d3dcbdyn - DYNAMIC constant buffer updated with Map(WRITE_DISCARD) between draws (the pattern of dgVoodoo / NFSU2,
 * ~1000 times per frame) - regression test for the UMD's constant-buffer renaming (notes/perf-threaded-context.md
 * T6). A 64x64 target, 4 vertical strips; for strip i: Map(DISCARD) the CB, write colour i, Unmap, VSSetConstantBuffers
 * is NOT repeated (the binding must follow the new contents), scissor to the strip, draw a full-target quad whose
 * pixel shader outputs the CB colour. Then the same with the CB bound AFTER the update (rebind each strip).
 * Readback: PASS = every strip has its colour.
 *   d3dcbdyn [warp]
 * SPDX-License-Identifier: GPL-2.0
 */
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <d3d11.h>
#include <d3dcompiler.h>

static const char *g_vs =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 p = float2((id & 1) ? 1.0 : -1.0, (id & 2) ? -1.0 : 1.0);\n"
    "  return float4(p, 0.0, 1.0);\n"
    "}\n";
static const char *g_ps =
    "cbuffer C : register(b0) { float4 col; float4 pad[15]; };\n"
    "float4 main() : SV_Target { return col; }\n";

static ID3DBlob *Compile(const char *src, const char *target)
{
    ID3DBlob *b = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), NULL, NULL, NULL, "main", target, 0, 0, &b, &err);
    if (FAILED(hr)) {
        printf("D3DCompile %s hr 0x%08lx %s\n", target, hr, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        return NULL;
    }
    return b;
}

static const float g_cols[4][4] = { {1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1} };
static const UINT32 g_want[4] = { 0xff0000ff, 0xff00ff00, 0xffff0000, 0xff00ffff };   /* RGBA8 in memory: ABGR */

static int Pass(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *rt, ID3D11Buffer *cb, int rebind,
                const char *name)
{
    ID3D11Texture2D *st;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(rt, &td);
    for (int i = 0; i < 4; i++) {
        D3D11_MAPPED_SUBRESOURCE m;
        HRESULT hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        if (FAILED(hr)) { printf("Map hr 0x%08lx\n", hr); return 0; }
        memset(m.pData, 0, 256);
        memcpy(m.pData, g_cols[i], sizeof g_cols[i]);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb, 0);
        if (rebind || i == 0)
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb);
        D3D11_RECT r = { i * 16, 0, i * 16 + 16, 64 };
        ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &r);
        ID3D11DeviceContext_Draw(ctx, 4, 0);
    }
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &st))) return 0;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)rt);
    D3D11_MAPPED_SUBRESOURCE m;
    int ok = 1;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        for (int i = 0; i < 4; i++) {
            UINT32 px = ((const UINT32 *)((const BYTE *)m.pData + 32 * m.RowPitch))[i * 16 + 8];
            printf("%-16s strip %d 0x%08x (want 0x%08x) %s\n", name, i, px, g_want[i], px == g_want[i] ? "PASS" : "FAIL");
            ok &= px == g_want[i];
        }
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    } else {
        ok = 0;
    }
    ID3D11Texture2D_Release(st);
    return ok;
}

int main(int argc, char **argv)
{
    int warp = argc > 1 && !strcmp(argv[1], "warp");
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(NULL, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                   D3D11_SDK_VERSION, &dev, &fl, &ctx);
    printf("D3D11CreateDevice hr 0x%08lx FL 0x%x\n", hr, fl);
    if (FAILED(hr)) return 1;

    D3D11_TEXTURE2D_DESC td = { 64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET, 0, 0 };
    ID3D11Texture2D *rt;
    ID3D11RenderTargetView *rtv;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt))) return 2;
    if (FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv))) return 2;
    D3D11_BUFFER_DESC bd = { 256, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    ID3D11Buffer *cb;
    if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &cb))) return 3;
    ID3DBlob *vsb = Compile(g_vs, "vs_4_0"), *psb = Compile(g_ps, "ps_4_0");
    if (!vsb || !psb) return 4;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps);
    D3D11_RASTERIZER_DESC rd = { D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0.0f, 0.0f, TRUE, TRUE, FALSE, FALSE };
    ID3D11RasterizerState *rs;
    ID3D11Device_CreateRasterizerState(dev, &rd, &rs);

    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    D3D11_VIEWPORT vp = { 0, 0, 64, 64, 0, 1 };
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(ctx, rs);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);

    const float clear[4] = { 0, 0, 0, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
    int ok = Pass(dev, ctx, rt, cb, 0, "bound-once");
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
    ok &= Pass(dev, ctx, rt, cb, 1, "rebind-each");
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 5;
}
