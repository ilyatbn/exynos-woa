/*
 * d3dsm41 - FL 10_1 task F2 (notes/gpu-umd-d3d11.md): the shader model 4.1 instructions on the Mali D3D11 UMD,
 * compared with WARP. One pixel shader (ps_4_1, compiled at run time with d3dcompiler_47) writes one test per pixel
 * of an 8x1 R32G32B32A32_FLOAT target:
 *   x=0 sampleinfo of a 4x texture        x=1..4 samplepos(0..3) of the 4x texture
 *   x=5 ld_ms sample 3 of the 4x texture (cleared to 0.25,0.5,0.75,1)
 *   x=6 gather4 (red) at the centre of a 2x2 R32_FLOAT texture {1,2,3,4}
 *   x=7 lod of a 64x64 mipped texture sampled with a 4x minification (ddx/ddy from the 8x1 target), computed in
 *       uniform control flow at the top (derivatives need the whole quad; undefined after divergent returns)
 * Prints both devices' values; exit 0 = every value matches WARP within 1e-3 (lod within 0.05).
 * Build: build-d3dsm41.cmd. SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <d3d11.h>
#include <d3dcompiler.h>

static const char *g_ps =
    "Texture2DMS<float4,4> ms : register(t0);\n"
    "Texture2D<float> g : register(t1);\n"
    "Texture2D<float4> m : register(t2);\n"
    "SamplerState pt : register(s0);\n"
    "SamplerState lin : register(s1);\n"
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "  float2 uv = float2(pos.x * 4.0 / 64.0, pos.y * 4.0 / 64.0);\n"
    "  float2 lod = float2(m.CalculateLevelOfDetail(lin, uv), m.CalculateLevelOfDetailUnclamped(lin, uv));\n"
    "  uint x = (uint)pos.x;\n"
    "  uint w, h, n; ms.GetDimensions(w, h, n);\n"
    "  if (x == 0) return float4(n, w, h, 0);\n"
    "  if (x <= 4) return float4(ms.GetSamplePosition(x - 1), 0, 0);\n"
    "  if (x == 5) return ms.Load(int2(1, 1), 3);\n"
    "  if (x == 6) return g.GatherRed(pt, float2(0.5, 0.5));\n"
    "  return float4(lod, 0, 0);\n"
    "}\n";
static const char *g_vs =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 p = float2((id << 1) & 2, id & 2);\n"
    "  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n";

static bool Run(bool warp, IDXGIAdapter *ad, ID3DBlob *vsb, ID3DBlob *psb, float out[8][4])
{
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_1, got;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    HRESULT hr = D3D11CreateDevice(warp ? NULL : ad, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
                                   &fl, 1, D3D11_SDK_VERSION, &dev, &got, &ctx);
    printf("%s: D3D11CreateDevice 0x%08lx FL 0x%x\n", warp ? "WARP" : "Mali", (unsigned long)hr, got);
    if (FAILED(hr))
        return false;
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    hr = dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), NULL, &vs);
    if (SUCCEEDED(hr))
        hr = dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), NULL, &ps);
    if (FAILED(hr)) {
        printf("  shader create 0x%08lx\n", (unsigned long)hr);
        return false;
    }
    /* t0: 4x RGBA32F 4x4, cleared */
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 4; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.SampleDesc.Count = 4; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *ms = NULL;
    hr = dev->CreateTexture2D(&td, NULL, &ms);
    if (FAILED(hr)) {
        printf("  4x RGBA32F texture 0x%08lx\n", (unsigned long)hr);
        return false;
    }
    ID3D11RenderTargetView *msrtv = NULL;
    dev->CreateRenderTargetView(ms, NULL, &msrtv);
    float cc[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
    ctx->ClearRenderTargetView(msrtv, cc);
    /* t1: 2x2 R32F {1,2,3,4} */
    float gv[4] = { 1, 2, 3, 4 };
    D3D11_TEXTURE2D_DESC gd = {};
    gd.Width = gd.Height = 2; gd.MipLevels = gd.ArraySize = 1; gd.Format = DXGI_FORMAT_R32_FLOAT;
    gd.SampleDesc.Count = 1; gd.Usage = D3D11_USAGE_IMMUTABLE; gd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA gs = { gv, 8, 0 };
    ID3D11Texture2D *gt = NULL;
    dev->CreateTexture2D(&gd, &gs, &gt);
    /* t2: 64x64 RGBA8 with a full mip chain (contents irrelevant for lod) */
    D3D11_TEXTURE2D_DESC md = {};
    md.Width = md.Height = 64; md.MipLevels = 7; md.ArraySize = 1; md.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    md.SampleDesc.Count = 1; md.Usage = D3D11_USAGE_DEFAULT; md.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *mt = NULL;
    dev->CreateTexture2D(&md, NULL, &mt);
    ID3D11ShaderResourceView *srv[3] = {};
    dev->CreateShaderResourceView(ms, NULL, &srv[0]);
    dev->CreateShaderResourceView(gt, NULL, &srv[1]);
    dev->CreateShaderResourceView(mt, NULL, &srv[2]);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState *smp[2] = {};
    dev->CreateSamplerState(&sd, &smp[0]);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    dev->CreateSamplerState(&sd, &smp[1]);
    /* target 8x1 RGBA32F + staging */
    D3D11_TEXTURE2D_DESC rd = {};
    rd.Width = 8; rd.Height = 1; rd.MipLevels = rd.ArraySize = 1; rd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    rd.SampleDesc.Count = 1; rd.Usage = D3D11_USAGE_DEFAULT; rd.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *rt = NULL, *st = NULL;
    dev->CreateTexture2D(&rd, NULL, &rt);
    rd.Usage = D3D11_USAGE_STAGING; rd.BindFlags = 0; rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    dev->CreateTexture2D(&rd, NULL, &st);
    ID3D11RenderTargetView *rtv = NULL;
    dev->CreateRenderTargetView(rt, NULL, &rtv);
    float z[4] = { -9, -9, -9, -9 };
    ctx->ClearRenderTargetView(rtv, z);
    ctx->OMSetRenderTargets(1, &rtv, NULL);
    D3D11_VIEWPORT vp = { 0, 0, 8, 1, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, NULL, 0);
    ctx->PSSetShader(ps, NULL, 0);
    ctx->PSSetShaderResources(0, 3, srv);
    ctx->PSSetSamplers(0, 2, smp);
    ctx->Draw(3, 0);
    ctx->CopyResource(st, rt);
    D3D11_MAPPED_SUBRESOURCE map;
    hr = ctx->Map(st, 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr))
        return false;
    memcpy(out, map.pData, 8 * 4 * sizeof(float));
    ctx->Unmap(st, 0);
    return true;
}

int main()
{
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    HRESULT hr = D3DCompile(g_vs, strlen(g_vs), "vs", NULL, NULL, "main", "vs_4_1", 0, 0, &vsb, &err);
    if (SUCCEEDED(hr))
        hr = D3DCompile(g_ps, strlen(g_ps), "ps", NULL, NULL, "main", "ps_4_1", 0, 0, &psb, &err);
    if (FAILED(hr)) {
        printf("D3DCompile 0x%08lx %s\n", (unsigned long)hr, err ? (const char *)err->GetBufferPointer() : "");
        return 1;
    }
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
    float mali[8][4], warp[8][4];
    if (!pick || !Run(false, pick, vsb, psb, mali) || !Run(true, NULL, vsb, psb, warp))
        return 2;
    static const char *names[8] = { "sampleinfo(n,w,h)", "samplepos 0", "samplepos 1", "samplepos 2", "samplepos 3",
                                    "ld_ms sample 3", "gather4 red", "lod (clamped, unclamped)" };
    int bad = 0;
    for (int x = 0; x < 8; x++) {
        float tol = x == 7 ? 0.05f : 1e-3f;
        bool ok = true;
        for (int c = 0; c < 4; c++)
            if (fabsf(mali[x][c] - warp[x][c]) > tol)
                ok = false;
        printf("%-26s Mali (%g, %g, %g, %g)  WARP (%g, %g, %g, %g)  %s\n", names[x], mali[x][0], mali[x][1],
               mali[x][2], mali[x][3], warp[x][0], warp[x][1], warp[x][2], warp[x][3], ok ? "ok" : "DIFF");
        bad += !ok;
    }
    printf("%d of 8 differ -> %s\n", bad, bad ? "FAIL" : "PASS");
    return bad ? 3 : 0;
}
