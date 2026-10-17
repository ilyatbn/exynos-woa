/*
 * d3dmpvps - G17 / D10 T4 (notes/g17-dxva-mfc.md): mpv's own dumb-mode shaders (HLSL from its log, vo/gpu/d3d11,
 * SPIRV-Cross output) replayed on a 256x256 target. Inputs as mpv creates them: t1 luma R8_UNORM, t2 chroma R8G8_UNORM,
 * t0 dither R16G16B16A16_FLOAT 16x16, linear samplers, cbuffer b0 with an identity colour matrix. Clear red, draw a
 * full-target quad, read the center: grey-ish = the draw wrote; red = it wrote nothing (mpv's black window).
 *
 *   d3dmpvps [warp] [ps40] [noflip] [greenps] [posfirst]   (posfirst: SV_Position declared first in the VS output; ps40: ps_4_0/vs_4_0 instead of 4_1; noflip: mpv's VS without its
 *                                                y flip; greenps: a constant-green PS with mpv's inputs)
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <d3d11.h>
#include <d3dcompiler.h>

static const char kVs[] =
    "static float4 gl_Position; static float2 _18; static float2 _27; static float2 _28; static float2 _30; static float2 _31;\n"
    "struct SPIRV_Cross_Input { float2 _18 : TEXCOORD0; float2 _28 : TEXCOORD1; float2 _31 : TEXCOORD2; };\n"
    "struct SPIRV_Cross_Output { float2 _27 : TEXCOORD1; float2 _30 : TEXCOORD2; float4 gl_Position : SV_Position; };\n"
    "void vert_main() { gl_Position = float4(_18, 1.0f, 1.0f); _27 = _28; _30 = _31; gl_Position.y = -gl_Position.y; }\n"
    "SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input) { _18 = stage_input._18; _28 = stage_input._28;\n"
    "  _31 = stage_input._31; vert_main(); SPIRV_Cross_Output stage_output; stage_output.gl_Position = gl_Position;\n"
    "  stage_output._27 = _27; stage_output._30 = _30; return stage_output; }\n";

static const char kPs[] =
    "static float4 _169;\n"
    "cbuffer _47_49 : register(b0) {\n"
    "  row_major float3x3 _49_m0 : packoffset(c0); float3 _49_m1 : packoffset(c3); float3 _49_m2 : packoffset(c4);\n"
    "  float3 _49_m3 : packoffset(c5); float2 _49_m4 : packoffset(c6); row_major float2x2 _49_m5 : packoffset(c7);\n"
    "  float2 _49_m6 : packoffset(c9); float2 _49_m7 : packoffset(c9.z); float2 _49_m8 : packoffset(c10);\n"
    "  row_major float2x2 _49_m9 : packoffset(c11); float2 _49_m10 : packoffset(c13); float2 _49_m11 : packoffset(c13.z); };\n"
    "Texture2D<float4> _16 : register(t1); SamplerState __16_sampler : register(s1);\n"
    "Texture2D<float4> _29 : register(t2); SamplerState __29_sampler : register(s2);\n"
    "Texture2D<float4> _113 : register(t0); SamplerState __113_sampler : register(s0);\n"
    "static float4 gl_FragCoord; static float2 _20; static float2 _31; static float4 _132;\n"
    "struct SPIRV_Cross_Input { float2 _20 : TEXCOORD1; float2 _31 : TEXCOORD2; float4 gl_FragCoord : SV_Position; };\n"
    "struct SPIRV_Cross_Output { float4 _132 : SV_Target0; };\n"
    "void frag_main() { float4 _135; _135.x = _16.Sample(__16_sampler, _20).x;\n"
    "  float2 _35 = _29.Sample(__29_sampler, _31).xy * 1.0f; _135.y = _35.x; _135.z = _35.y;\n"
    "  float3 _66 = mul(_135.xyz, _49_m0) + _49_m1; float _68 = _66.x; float4 _164 = float4(_68, _66.yz, 1.0f);\n"
    "  _164.x = _68; _164.y = _66.y; _164.z = _66.z;\n"
    "  _132 = (floor(((_164 * 255.0f) + _113.Sample(__113_sampler, (float2(gl_FragCoord.x, gl_FragCoord.y) * 1.0f) * 0.015625f.xx).x.xxxx) + 0.0001220703125f.xxxx) * 1.0f) * 0.0039215688593685626983642578125f.xxxx; }\n"
    "SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input) { gl_FragCoord = stage_input.gl_FragCoord;\n"
    "  gl_FragCoord.w = 1.0 / gl_FragCoord.w; _20 = stage_input._20; _31 = stage_input._31; frag_main();\n"
    "  SPIRV_Cross_Output stage_output; stage_output._132 = _132; return stage_output; }\n";

static const char kGreenPs[] =
    "struct SPIRV_Cross_Input { float2 _20 : TEXCOORD1; float2 _31 : TEXCOORD2; float4 gl_FragCoord : SV_Position; };\n"
    "float4 main(SPIRV_Cross_Input i) : SV_Target0 { return float4(0, 1, 0, 1); }\n";

static ID3D11ShaderResourceView *Tex(ID3D11Device *dev, DXGI_FORMAT f, UINT w, UINT h, const void *texel, UINT bpp)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = td.ArraySize = 1; td.Format = f; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    unsigned char *data = new unsigned char[w * h * bpp];
    for (UINT i = 0; i < w * h; i++)
        memcpy(data + i * bpp, texel, bpp);
    D3D11_SUBRESOURCE_DATA sd = { data, w * bpp, 0 };
    ID3D11Texture2D *t = NULL;
    ID3D11ShaderResourceView *v = NULL;
    HRESULT hr = dev->CreateTexture2D(&td, &sd, &t);
    if (SUCCEEDED(hr))
        hr = dev->CreateShaderResourceView(t, NULL, &v);
    printf("texture fmt %u %ux%u: 0x%08lx\n", (unsigned)f, w, h, hr);
    delete[] data;
    return v;
}

int main(int argc, char **argv)
{
    bool warp = false, sm40 = false, noflip = false, greenps = false, posfirst = false;
    for (int a = 1; a < argc; a++) {
        warp = warp || !strcmp(argv[a], "warp");
        sm40 = sm40 || !strcmp(argv[a], "ps40");
        noflip = noflip || !strcmp(argv[a], "noflip");
        greenps = greenps || !strcmp(argv[a], "greenps");
        posfirst = posfirst || !strcmp(argv[a], "posfirst");
    }
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 }, got;
    HRESULT hr = D3D11CreateDevice(NULL, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, NULL, 0, fl, 2,
                                   D3D11_SDK_VERSION, &dev, &got, &ctx);
    printf("device 0x%08lx FL 0x%x %s\n", hr, got, warp ? "(WARP)" : "");
    if (FAILED(hr))
        return 1;
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    const char *vt = sm40 ? "vs_4_0" : "vs_4_1", *pt = sm40 ? "ps_4_0" : "ps_4_1";
    char vsrc[sizeof(kVs)];
    memcpy(vsrc, kVs, sizeof(kVs));
    if (noflip) {
        char *f = strstr(vsrc, "gl_Position.y = -gl_Position.y;");
        if (f)
            memset(f, ' ', strlen("gl_Position.y = -gl_Position.y;"));
    }
    if (posfirst) {   /* same members, SV_Position moved to the front of the output struct */
        const char *a = "struct SPIRV_Cross_Output { float2 _27 : TEXCOORD1; float2 _30 : TEXCOORD2; float4 gl_Position : SV_Position; };";
        const char *b = "struct SPIRV_Cross_Output { float4 gl_Position : SV_Position; float2 _27 : TEXCOORD1; float2 _30 : TEXCOORD2; };";
        char *f = strstr(vsrc, a);
        if (f)
            memcpy(f, b, strlen(b));
        else
            printf("posfirst: pattern not found\n");
    }
    const char *psrc = greenps ? kGreenPs : kPs;
    printf("variant:%s%s%s\n", noflip ? " noflip" : "", greenps ? " greenps" : "", posfirst ? " posfirst" : "");
    if (FAILED(D3DCompile(vsrc, strlen(vsrc), "mpv.vs", NULL, NULL, "main", vt, 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(psrc, strlen(psrc), "mpv.ps", NULL, NULL, "main", pt, 0, 0, &psb, &err))) {
        printf("compile failed: %s\n", err ? (const char *)err->GetBufferPointer() : "?");
        return 1;
    }
    printf("compiled %s / %s\n", vt, pt);
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    printf("CreateVertexShader 0x%08lx\n", dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), NULL, &vs));
    printf("CreatePixelShader 0x%08lx\n", dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), NULL, &ps));
    D3D11_INPUT_ELEMENT_DESC el[3] = {
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11InputLayout *il = NULL;
    printf("CreateInputLayout 0x%08lx\n", dev->CreateInputLayout(el, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il));

    const unsigned char luma = 128, chroma[2] = { 128, 128 };
    const unsigned short dither[4] = { 0x3800, 0x3800, 0x3800, 0x3c00 };   /* half 0.5, 0.5, 0.5, 1.0 */
    ID3D11ShaderResourceView *srv[3] = {
        Tex(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, 16, 16, dither, 8),
        Tex(dev, DXGI_FORMAT_R8_UNORM, 64, 64, &luma, 1),
        Tex(dev, DXGI_FORMAT_R8G8_UNORM, 32, 32, chroma, 2),
    };
    D3D11_SAMPLER_DESC sdsc = {};
    sdsc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sdsc.AddressU = sdsc.AddressV = sdsc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sdsc.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState *smp = NULL;
    dev->CreateSamplerState(&sdsc, &smp);
    ID3D11SamplerState *smps[3] = { smp, smp, smp };

    float cbdata[14 * 4] = {};
    cbdata[0] = 1; cbdata[5] = 1; cbdata[10] = 1;   /* row_major float3x3 in c0..c2: identity */
    D3D11_BUFFER_DESC cbd = { sizeof(cbdata), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA cbs = { cbdata, 0, 0 };
    ID3D11Buffer *cb = NULL;
    dev->CreateBuffer(&cbd, &cbs, &cb);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 256; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *rt = NULL, *st = NULL;
    dev->CreateTexture2D(&td, NULL, &rt);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    dev->CreateTexture2D(&td, NULL, &st);
    ID3D11RenderTargetView *rtv = NULL;
    dev->CreateRenderTargetView(rt, NULL, &rtv);

    struct V { float x, y, u0, v0, u1, v1; };
    const V quad[6] = { { -1, -1, 0, 1, 0, 1 }, { -1, 1, 0, 0, 0, 0 }, { 1, 1, 1, 0, 1, 0 },
                        { -1, -1, 0, 1, 0, 1 }, { 1, 1, 1, 0, 1, 0 }, { 1, -1, 1, 1, 1, 1 } };
    D3D11_BUFFER_DESC vbd = { sizeof(quad), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA vbs = { quad, 0, 0 };
    ID3D11Buffer *vb = NULL;
    dev->CreateBuffer(&vbd, &vbs, &vb);

    const float red[4] = { 1, 0, 0, 1 };
    ctx->ClearRenderTargetView(rtv, red);
    UINT stride = sizeof(V), off = 0;
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
    ctx->IASetInputLayout(il);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, NULL, 0);
    ctx->PSSetShader(ps, NULL, 0);
    ctx->PSSetShaderResources(0, 3, srv);
    ctx->PSSetSamplers(0, 3, smps);
    ctx->PSSetConstantBuffers(0, 1, &cb);
    D3D11_VIEWPORT vp = { 0, 0, 256, 256, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    /* mpv's rasterizer state: no culling (its VS flips y, which reverses the winding), scissor on, full target */
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE; rd.ScissorEnable = TRUE;
    ID3D11RasterizerState *rs = NULL;
    dev->CreateRasterizerState(&rd, &rs);
    D3D11_RECT sr = { 0, 0, 256, 256 };
    ctx->RSSetState(rs);
    ctx->RSSetScissorRects(1, &sr);
    ctx->OMSetRenderTargets(1, &rtv, NULL);
    ctx->Draw(6, 0);
    ctx->CopyResource(st, rt);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m)))
        return 1;
    UINT px = *(const UINT *)((const char *)m.pData + 128 * m.RowPitch + 128 * 4);
    ctx->Unmap(st, 0);
    bool red_left = (px & 0x00ffffffu) == 0x000000ffu;
    printf("center 0x%08x -> %s\n", px, red_left ? "FAIL (nothing written)" : "written");
    return red_left ? 2 : 0;
}
