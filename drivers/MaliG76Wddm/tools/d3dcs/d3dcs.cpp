/*
 * d3dcs - G24 DirectCompute 4.x check (notes/g24-compute-cs4x.md): cs_4_0 compute shaders on the Mali D3D11 UMD
 * against WARP, both at feature level 10_1. Each test runs the same shader on both devices and compares the UAV buffer
 * word for word:
 *   raw        ByteAddressBuffer Load / RWByteAddressBuffer Store, SV_DispatchThreadID
 *   raw2       Load2 / Store2 with a swizzle
 *   structured StructuredBuffer / RWStructuredBuffer of {float, uint, float2}, SV_GroupID
 *   groupshared groupshared reduction with GroupMemoryBarrierWithGroupSync, SV_GroupIndex
 *   grid2d     Dispatch(2,3,1) of 8x8 groups: SV_DispatchThreadID / SV_GroupThreadID / SV_GroupID / SV_GroupIndex
 *   texcb      Texture2D.Load + a constant buffer
 *   clear      ClearUnorderedAccessViewUint, then a shader that writes only the even words
 * Exit 0 = every test matches. Build: build-d3dcs.cmd (x86 for the 32-bit UMD games use, or arm64).
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <stdio.h>
#include <d3d11.h>
#include <d3dcompiler.h>

enum InKind { IN_NONE, IN_RAW, IN_STRUCT, IN_TEX };
enum OutKind { OUT_RAW, OUT_STRUCT };

struct Test {
    const char *name;
    const char *hlsl;
    InKind in;
    UINT in_words, in_stride;
    OutKind out;
    UINT out_words, out_stride;
    UINT gx, gy, gz;
    bool clear;
};

static const Test g_tests[] = {
    { "raw",
      "ByteAddressBuffer src : register(t0);\n"
      "RWByteAddressBuffer dst : register(u0);\n"
      "[numthreads(64,1,1)] void main(uint3 tid : SV_DispatchThreadID) {\n"
      "  uint v = src.Load(tid.x * 4);\n"
      "  dst.Store(tid.x * 4, v * 3u + tid.x);\n"
      "}\n", IN_RAW, 256, 0, OUT_RAW, 256, 0, 4, 1, 1, false },
    { "raw2",
      "ByteAddressBuffer src : register(t0);\n"
      "RWByteAddressBuffer dst : register(u0);\n"
      "[numthreads(64,1,1)] void main(uint3 tid : SV_DispatchThreadID) {\n"
      "  uint2 v = src.Load2(tid.x * 8);\n"
      "  dst.Store2(tid.x * 8, v.yx + uint2(1, 2));\n"
      "}\n", IN_RAW, 256, 0, OUT_RAW, 256, 0, 2, 1, 1, false },
    { "structured",
      "struct S { float a; uint b; float2 c; };\n"
      "StructuredBuffer<S> src : register(t0);\n"
      "RWStructuredBuffer<S> dst : register(u0);\n"
      "[numthreads(32,1,1)] void main(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID) {\n"
      "  S s = src[tid.x];\n"
      "  s.a = s.a * 2.0; s.b = s.b + gid.x * 1000u; s.c = s.c.yx;\n"
      "  dst[tid.x] = s;\n"
      "}\n", IN_STRUCT, 64 * 4, 16, OUT_STRUCT, 64 * 4, 16, 2, 1, 1, false },
    { "groupshared",
      "ByteAddressBuffer src : register(t0);\n"
      "RWByteAddressBuffer dst : register(u0);\n"
      "groupshared uint sh[64];\n"
      "[numthreads(64,1,1)] void main(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex,\n"
      "                              uint3 gid : SV_GroupID) {\n"
      "  sh[gi] = src.Load(tid.x * 4);\n"
      "  GroupMemoryBarrierWithGroupSync();\n"
      "  if (gi == 0) { uint s = 0; for (uint i = 0; i < 64; i++) s += sh[i]; dst.Store(gid.x * 4, s); }\n"
      "}\n", IN_RAW, 256, 0, OUT_RAW, 4, 0, 4, 1, 1, false },
    { "grid2d",
      "RWByteAddressBuffer dst : register(u0);\n"
      "[numthreads(8,8,1)] void main(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID,\n"
      "                            uint gi : SV_GroupIndex, uint3 gt : SV_GroupThreadID) {\n"
      "  uint idx = (gid.y * 2 + gid.x) * 64 + gi;\n"
      "  dst.Store(idx * 4, (tid.x << 24) | (tid.y << 16) | (gt.x << 8) | gt.y);\n"
      "}\n", IN_NONE, 0, 0, OUT_RAW, 384, 0, 2, 3, 1, false },
    { "texcb",
      "Texture2D<float4> tex : register(t0);\n"
      "cbuffer C : register(b0) { uint w; uint mul; uint2 pad; };\n"
      "RWByteAddressBuffer dst : register(u0);\n"
      "[numthreads(16,1,1)] void main(uint3 tid : SV_DispatchThreadID) {\n"
      "  float4 c = tex.Load(int3(tid.x % w, tid.x / w, 0));\n"
      "  dst.Store(tid.x * 4, (uint)(c.x * 255.0 + 0.5) * mul + (uint)(c.w * 255.0 + 0.5));\n"
      "}\n", IN_TEX, 16, 0, OUT_RAW, 16, 0, 1, 1, 1, false },
    { "clear",
      "RWByteAddressBuffer dst : register(u0);\n"
      "[numthreads(32,1,1)] void main(uint3 tid : SV_DispatchThreadID) {\n"
      "  dst.Store(tid.x * 8, tid.x * 11u);\n"
      "}\n", IN_NONE, 0, 0, OUT_RAW, 64, 0, 1, 1, 1, true },
};
static const int NTEST = sizeof(g_tests) / sizeof(g_tests[0]);

static UINT g_in[1024];

static bool RunTest(ID3D11Device *dev, ID3D11DeviceContext *ctx, const Test *t, ID3DBlob *code, UINT *out)
{
    HRESULT hr;
    ID3D11ComputeShader *cs = NULL;
    hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), NULL, &cs);
    if (FAILED(hr)) { printf("    CreateComputeShader 0x%08lx\n", (unsigned long)hr); return false; }

    ID3D11ShaderResourceView *srv = NULL;
    ID3D11Buffer *inbuf = NULL, *cb = NULL;
    ID3D11Texture2D *tex = NULL;
    if (t->in == IN_RAW || t->in == IN_STRUCT) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = t->in_words * 4;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = t->in == IN_RAW ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS : D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = t->in == IN_STRUCT ? t->in_stride : 0;
        D3D11_SUBRESOURCE_DATA sd = { g_in, 0, 0 };
        hr = dev->CreateBuffer(&bd, &sd, &inbuf);
        D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
        if (t->in == IN_RAW) {
            vd.Format = DXGI_FORMAT_R32_TYPELESS;
            vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            vd.BufferEx.NumElements = t->in_words;
            vd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        } else {
            vd.Format = DXGI_FORMAT_UNKNOWN;
            vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            vd.Buffer.NumElements = t->in_words * 4 / t->in_stride;
        }
        if (SUCCEEDED(hr))
            hr = dev->CreateShaderResourceView(inbuf, &vd, &srv);
        if (FAILED(hr)) { printf("    input buffer/SRV 0x%08lx\n", (unsigned long)hr); return false; }
    } else if (t->in == IN_TEX) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = 4; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd = { g_in, 16, 0 };
        hr = dev->CreateTexture2D(&td, &sd, &tex);
        if (SUCCEEDED(hr))
            hr = dev->CreateShaderResourceView(tex, NULL, &srv);
        UINT cbd[4] = { 4, 3, 0, 0 };
        D3D11_BUFFER_DESC bd = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
        D3D11_SUBRESOURCE_DATA cd = { cbd, 0, 0 };
        if (SUCCEEDED(hr))
            hr = dev->CreateBuffer(&bd, &cd, &cb);
        if (FAILED(hr)) { printf("    texture/cbuffer 0x%08lx\n", (unsigned long)hr); return false; }
    }

    D3D11_BUFFER_DESC od = {};
    od.ByteWidth = t->out_words * 4;
    od.Usage = D3D11_USAGE_DEFAULT;
    od.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    od.MiscFlags = t->out == OUT_RAW ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS : D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    od.StructureByteStride = t->out == OUT_STRUCT ? t->out_stride : 0;
    ID3D11Buffer *outbuf = NULL, *st = NULL;
    ID3D11UnorderedAccessView *uav = NULL;
    hr = dev->CreateBuffer(&od, NULL, &outbuf);
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    if (t->out == OUT_RAW) {
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.Buffer.NumElements = t->out_words;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    } else {
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.Buffer.NumElements = t->out_words * 4 / t->out_stride;
    }
    if (SUCCEEDED(hr))
        hr = dev->CreateUnorderedAccessView(outbuf, &ud, &uav);
    od.Usage = D3D11_USAGE_STAGING; od.BindFlags = 0; od.MiscFlags = 0; od.StructureByteStride = 0;
    od.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr))
        hr = dev->CreateBuffer(&od, NULL, &st);
    if (FAILED(hr)) { printf("    output buffer/UAV 0x%08lx\n", (unsigned long)hr); return false; }

    UINT zero[4] = { 0, 0, 0, 0 }, dead[4] = { 0xDEADBEEF, 0, 0, 0 };
    ctx->ClearUnorderedAccessViewUint(uav, t->clear ? dead : zero);
    ctx->CSSetShader(cs, NULL, 0);
    if (srv)
        ctx->CSSetShaderResources(0, 1, &srv);
    if (cb)
        ctx->CSSetConstantBuffers(0, 1, &cb);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, NULL);
    ctx->Dispatch(t->gx, t->gy, t->gz);
    ID3D11UnorderedAccessView *nouav = NULL;
    ctx->CSSetUnorderedAccessViews(0, 1, &nouav, NULL);
    ctx->CopyResource(st, outbuf);
    D3D11_MAPPED_SUBRESOURCE map;
    hr = ctx->Map(st, 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr)) { printf("    Map 0x%08lx\n", (unsigned long)hr); return false; }
    memcpy(out, map.pData, t->out_words * 4);
    ctx->Unmap(st, 0);
    if (srv) srv->Release();
    if (inbuf) inbuf->Release();
    if (cb) cb->Release();
    if (tex) tex->Release();
    uav->Release(); outbuf->Release(); st->Release(); cs->Release();
    return true;
}

static bool Open(bool warp, IDXGIAdapter *ad, ID3D11Device **dev, ID3D11DeviceContext **ctx)
{
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_1, got;
    HRESULT hr = D3D11CreateDevice(warp ? NULL : ad, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
                                   &fl, 1, D3D11_SDK_VERSION, dev, &got, ctx);
    D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS o = {};
    if (SUCCEEDED(hr))
        (*dev)->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &o, sizeof(o));
    printf("%s: D3D11CreateDevice 0x%08lx FL 0x%x, ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x %d\n",
           warp ? "WARP" : "Mali", (unsigned long)hr, got, o.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x);
    return SUCCEEDED(hr) && o.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x;
}

int main()
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* a GPU hang must not lose the lines before it */
    for (UINT i = 0; i < 1024; i++)
        g_in[i] = i * 7u + 1u;
    for (UINT i = 0; i < 64; i++) {   /* the structured input: {float a, uint b, float2 c} */
        float a = 0.5f * (float)i, c0 = 1.0f + (float)i, c1 = -2.0f * (float)i;
        memcpy(&g_in[256 + i * 4 + 0], &a, 4);
        g_in[256 + i * 4 + 1] = i * 3u;
        memcpy(&g_in[256 + i * 4 + 2], &c0, 4);
        memcpy(&g_in[256 + i * 4 + 3], &c1, 4);
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
    ID3D11Device *md = NULL, *wd = NULL;
    ID3D11DeviceContext *mc = NULL, *wc = NULL;
    if (!pick || !Open(false, pick, &md, &mc) || !Open(true, NULL, &wd, &wc))
        return 2;
    int fails = 0;
    static UINT om[1024], ow[1024];
    for (int i = 0; i < NTEST; i++) {
        const Test *t = &g_tests[i];
        ID3DBlob *code = NULL, *err = NULL;
        HRESULT hr = D3DCompile(t->hlsl, strlen(t->hlsl), t->name, NULL, NULL, "main", "cs_4_0", 0, 0, &code, &err);
        if (FAILED(hr)) {
            printf("%-12s D3DCompile 0x%08lx %s\n", t->name, (unsigned long)hr,
                   err ? (const char *)err->GetBufferPointer() : "");
            fails++;
            continue;
        }
        /* the structured test reads the struct array placed at g_in[256] */
        if (t->in == IN_STRUCT)
            memmove(g_in, g_in + 256, 64 * 16);
        memset(om, 0xCD, sizeof om);
        memset(ow, 0xAB, sizeof ow);
        printf("%-12s running...\n", t->name);
        bool okm = RunTest(md, mc, t, code, om), okw = RunTest(wd, wc, t, code, ow);
        if (t->in == IN_STRUCT)
            for (UINT k = 0; k < 256; k++)
                g_in[k] = k * 7u + 1u;
        UINT bad = 0, first = 0;
        if (okm && okw)
            for (UINT k = 0; k < t->out_words; k++)
                if (om[k] != ow[k] && !bad++)
                    first = k;
        bool ok = okm && okw && !bad;
        printf("%-12s %s", t->name, ok ? "ok" : "DIFF");
        if (okm && okw && bad)
            printf(" (%u of %u words; first [%u] Mali 0x%08x WARP 0x%08x)", bad, t->out_words, first, om[first],
                   ow[first]);
        if (!okm || !okw)
            printf(" (did not run on %s)", !okm ? "Mali" : "WARP");
        printf("  sample [0..3] Mali %08x %08x %08x %08x\n", om[0], om[1], om[2], om[3]);
        fails += !ok;
        code->Release();
    }
    printf("%d of %d tests differ -> %s\n", fails, NTEST, fails ? "FAIL" : "PASS");
    return fails ? 3 : 0;
}
