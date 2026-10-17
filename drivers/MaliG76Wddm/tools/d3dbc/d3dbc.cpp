/*
 * d3dbc - G23 BCn emulation check (notes/g23-bc-emulation.md): BC1-BC5 textures on the Mali D3D11 UMD (which stores
 * them uncompressed, decoded at upload) against WARP (native BCn). For each format the same deterministic pseudo-random
 * block data (every bit pattern is a valid BC1-BC5 block) goes through the three upload paths:
 *   init   IMMUTABLE 12x12, 3 mips (12, 6, 3: edge blocks of odd sizes), initial data
 *   update DEFAULT 12x12, 3 mips, UpdateSubresource per mip
 *   map    STAGING 8x8 Map WRITE, CopyResource into a DEFAULT texture
 * Every texel of every mip is read back with Texture2D.Load into a 16x16 RGBA32F target and compared, within one step of
 * the 8-bit format the emulation stores into (1/200 for UNORM, 1/117 for SNORM and sRGB).
 * Exit 0 = all match. Build: build-d3dbc.cmd (x86 - the 32-bit UMD is the one games use - or ARM64).
 * G26 (notes/g26-bc-etc2.md): "d3dbc lossy" checks the ETC2 / EAC re-compression instead (lossy by design): per case
 * the PSNR against WARP over the format's channels, for two data kinds - "random" (every bit random: far-apart
 * endpoints, the worst case for any re-encoder; floor 10 dB: only catches broken layouts - random BC2 alpha is noise no 8-level EAC block fits, 12 dB at a 3x3 mip) and "soft" (endpoints close together, as in real
 * textures; floor 30 dB) - and BC1's 1-bit alpha exactly (a texel transparent on WARP must be transparent on Mali).
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <d3d11.h>
#include <d3dcompiler.h>

static const char *g_ps =
    "Texture2D<float4> t : register(t0);\n"
    "cbuffer c : register(b0) { int mip; int3 pad; };\n"
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "  return t.Load(int3((int)pos.x, (int)pos.y, mip));\n"
    "}\n";
static const char *g_vs =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 p = float2((id << 1) & 2, id & 2);\n"
    "  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n";

struct Fmt { DXGI_FORMAT f; const char *name; UINT block; };
static const Fmt g_fmts[] = {
    { DXGI_FORMAT_BC1_UNORM, "BC1_UNORM", 8 },      { DXGI_FORMAT_BC1_UNORM_SRGB, "BC1_UNORM_SRGB", 8 },
    { DXGI_FORMAT_BC2_UNORM, "BC2_UNORM", 16 },     { DXGI_FORMAT_BC3_UNORM, "BC3_UNORM", 16 },
    { DXGI_FORMAT_BC3_UNORM_SRGB, "BC3_UNORM_SRGB", 16 },
    { DXGI_FORMAT_BC4_UNORM, "BC4_UNORM", 8 },      { DXGI_FORMAT_BC4_SNORM, "BC4_SNORM", 8 },
    { DXGI_FORMAT_BC5_UNORM, "BC5_UNORM", 16 },     { DXGI_FORMAT_BC5_SNORM, "BC5_SNORM", 16 },
};
static const int NF = sizeof(g_fmts) / sizeof(g_fmts[0]);
static UINT umax1(UINT v) { return v ? v : 1; }
enum { PATH_INIT, PATH_UPDATE, PATH_MAP, NPATH };
static const char *g_paths[NPATH] = { "init", "update", "map" };

static bool g_soft;   /* G26: soft data (endpoints close together) */

/* block data for a w x h mip: rows of ceil(w/4) blocks; bcn = 1..5 (the BC number of the format) */
static void Fill(unsigned char *d, UINT bytes, UINT seed, int bcn)
{
    UINT x = seed * 2654435761u + 12345u;
    for (UINT i = 0; i < bytes; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        d[i] = (unsigned char)(x >> 8);
    }
    if (!g_soft)
        return;
    /* G26 soft: the second endpoint of each block near the first. RGB565 colour endpoints (BC1 bytes 0-3, BC2 / BC3
     * bytes 8-11): colour1 = colour0 with only low bits of R, G and B flipped (both colour orders occur, so BC1 keeps
     * some 3-colour blocks with transparent texels); 8-bit endpoints (BC3 alpha, BC4, both halves of BC5): within 24.
     * BC2's explicit 4-bit alpha stays random. */
    UINT block = (bcn == 1 || bcn == 4) ? 8 : 16;
    for (UINT b = 0; b + block <= bytes; b += block) {
        for (UINT half = 0; half < block; half += 8) {
            unsigned char *h = d + b + half;
            bool colour = bcn == 1 || ((bcn == 2 || bcn == 3) && half == 8);
            bool explicit_alpha = bcn == 2 && half == 0;
            if (colour) {
                unsigned c0 = h[0] | (h[1] << 8);
                unsigned c1 = c0 ^ (h[2] & 0x0861);   /* low bit of R (11), low two of G (5, 6), low of B (0) */
                h[2] = (unsigned char)c1;
                h[3] = (unsigned char)(c1 >> 8);
            } else if (!explicit_alpha) {
                int a0 = h[0], a1 = a0 + (int)(h[1] % 49) - 24;
                h[1] = (unsigned char)(a1 < 0 ? 0 : a1 > 255 ? 255 : a1);
            }
        }
    }
}

struct Dev {
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11Buffer *cb;
    ID3D11Texture2D *rt, *st;
    ID3D11RenderTargetView *rtv;
};

static bool Open(Dev *d, bool warp, IDXGIAdapter *ad, ID3DBlob *vsb, ID3DBlob *psb)
{
    memset(d, 0, sizeof(*d));
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_1, got;
    HRESULT hr = D3D11CreateDevice(warp ? NULL : ad, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
                                   &fl, 1, D3D11_SDK_VERSION, &d->dev, &got, &d->ctx);
    printf("%s: D3D11CreateDevice 0x%08lx FL 0x%x\n", warp ? "WARP" : "Mali", (unsigned long)hr, got);
    if (FAILED(hr))
        return false;
    hr = d->dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), NULL, &d->vs);
    if (SUCCEEDED(hr))
        hr = d->dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), NULL, &d->ps);
    D3D11_BUFFER_DESC bd = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    if (SUCCEEDED(hr))
        hr = d->dev->CreateBuffer(&bd, NULL, &d->cb);
    D3D11_TEXTURE2D_DESC rd = {};
    rd.Width = rd.Height = 16; rd.MipLevels = rd.ArraySize = 1; rd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    rd.SampleDesc.Count = 1; rd.Usage = D3D11_USAGE_DEFAULT; rd.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (SUCCEEDED(hr))
        hr = d->dev->CreateTexture2D(&rd, NULL, &d->rt);
    rd.Usage = D3D11_USAGE_STAGING; rd.BindFlags = 0; rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr))
        hr = d->dev->CreateTexture2D(&rd, NULL, &d->st);
    if (SUCCEEDED(hr))
        hr = d->dev->CreateRenderTargetView(d->rt, NULL, &d->rtv);
    if (FAILED(hr))
        printf("  setup 0x%08lx\n", (unsigned long)hr);
    return SUCCEEDED(hr);
}

/* Create the texture of format f via path p with the seeded data; returns NULL (and prints) on failure. BC mip 0 must
 * be a multiple of 4 (D3D rule): 12x12 with 3 mips (12, 6, 3 - edge blocks); the map path is a STAGING 8x8 texture
 * (dynamic BC textures are refused) mapped for write and copied into a DEFAULT one (also checks CopyResource). */
static ID3D11Texture2D *Make(Dev *d, const Fmt *f, int p, UINT *w, UINT *h, UINT *mips)
{
    *w = *h = p == PATH_MAP ? 8 : 12;
    *mips = p == PATH_MAP ? 1 : 3;
    unsigned char data[3][512];
    UINT pitch[3], rows[3];
    for (UINT m = 0; m < *mips; m++) {
        UINT mw = umax1(*w >> m), mh = umax1(*h >> m);
        pitch[m] = (mw + 3) / 4 * f->block;
        rows[m] = (mh + 3) / 4;
        Fill(data[m], pitch[m] * rows[m], (UINT)f->f * 31 + m * 7 + (UINT)p, f->name[2] - '0');
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = *w; td.Height = *h; td.MipLevels = *mips; td.ArraySize = 1; td.Format = f->f;
    td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.Usage = p == PATH_INIT ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA init[3] = { { data[0], pitch[0], 0 }, { data[1], pitch[1], 0 }, { data[2], pitch[2], 0 } };
    ID3D11Texture2D *t = NULL;
    HRESULT hr = d->dev->CreateTexture2D(&td, p == PATH_INIT ? init : NULL, &t);
    if (FAILED(hr)) {
        printf("  %s %s: CreateTexture2D 0x%08lx\n", f->name, g_paths[p], (unsigned long)hr);
        return NULL;
    }
    if (p == PATH_UPDATE) {
        for (UINT m = 0; m < *mips; m++)
            d->ctx->UpdateSubresource(t, m, NULL, data[m], pitch[m], 0);
    } else if (p == PATH_MAP) {
        D3D11_TEXTURE2D_DESC sd = td;
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Texture2D *st = NULL;
        hr = d->dev->CreateTexture2D(&sd, NULL, &st);
        D3D11_MAPPED_SUBRESOURCE map = {};
        if (SUCCEEDED(hr))
            hr = d->ctx->Map(st, 0, D3D11_MAP_WRITE, 0, &map);
        if (FAILED(hr)) {
            printf("  %s map: staging/Map 0x%08lx\n", f->name, (unsigned long)hr);
            if (st) st->Release();
            t->Release();
            return NULL;
        }
        for (UINT r = 0; r < rows[0]; r++)
            memcpy((unsigned char *)map.pData + r * map.RowPitch, data[0] + r * pitch[0], pitch[0]);
        d->ctx->Unmap(st, 0);
        d->ctx->CopyResource(t, st);
        st->Release();
    }
    return t;
}

/* Read mip m of t into out[16][16][4] (texels outside the mip left at -9). */
static bool Read(Dev *d, ID3D11Texture2D *t, UINT m, float out[16][16][4])
{
    ID3D11ShaderResourceView *srv = NULL;
    if (FAILED(d->dev->CreateShaderResourceView(t, NULL, &srv)))
        return false;
    int cbv[4] = { (int)m, 0, 0, 0 };
    d->ctx->UpdateSubresource(d->cb, 0, NULL, cbv, 0, 0);
    float z[4] = { -9, -9, -9, -9 };
    d->ctx->ClearRenderTargetView(d->rtv, z);
    d->ctx->OMSetRenderTargets(1, &d->rtv, NULL);
    D3D11_VIEWPORT vp = { 0, 0, 16, 16, 0, 1 };
    d->ctx->RSSetViewports(1, &vp);
    d->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d->ctx->VSSetShader(d->vs, NULL, 0);
    d->ctx->PSSetShader(d->ps, NULL, 0);
    d->ctx->PSSetShaderResources(0, 1, &srv);
    d->ctx->PSSetConstantBuffers(0, 1, &d->cb);
    d->ctx->Draw(3, 0);
    d->ctx->CopyResource(d->st, d->rt);
    D3D11_MAPPED_SUBRESOURCE map;
    HRESULT hr = d->ctx->Map(d->st, 0, D3D11_MAP_READ, 0, &map);
    if (SUCCEEDED(hr)) {
        for (int y = 0; y < 16; y++)
            memcpy(out[y], (unsigned char *)map.pData + y * map.RowPitch, 16 * 4 * sizeof(float));
        d->ctx->Unmap(d->st, 0);
    }
    ID3D11ShaderResourceView *none = NULL;
    d->ctx->PSSetShaderResources(0, 1, &none);
    srv->Release();
    return SUCCEEDED(hr);
}

int main(int argc, char **argv)
{
    bool lossy = argc > 1 && !strcmp(argv[1], "lossy");   /* G26 */
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    HRESULT hr = D3DCompile(g_vs, strlen(g_vs), "vs", NULL, NULL, "main", "vs_4_0", 0, 0, &vsb, &err);
    if (SUCCEEDED(hr))
        hr = D3DCompile(g_ps, strlen(g_ps), "ps", NULL, NULL, "main", "ps_4_0", 0, 0, &psb, &err);
    if (FAILED(hr)) {
        printf("D3DCompile 0x%08lx %s\n", (unsigned long)hr, err ? (const char *)err->GetBufferPointer() : "");
        return 1;
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
    if (!pick || !Open(&mali, false, pick, vsb, psb) || !Open(&warp, true, NULL, vsb, psb))
        return 2;
    int fails = 0, cases = 0;
    for (int kind = 0; kind < (lossy ? 2 : 1); kind++) {
    g_soft = kind == 1;
    if (lossy)
        printf("--- %s data (PSNR floor %d dB)\n", g_soft ? "soft" : "random", g_soft ? 30 : 10);
    for (int i = 0; i < NF; i++) {
        UINT caps = 0;
        mali.dev->CheckFormatSupport(g_fmts[i].f, &caps);
        printf("%-15s Mali CheckFormatSupport 0x%x\n", g_fmts[i].name, caps);
        for (int p = 0; p < NPATH; p++) {
            UINT w, h, mips;
            ID3D11Texture2D *tm = Make(&mali, &g_fmts[i], p, &w, &h, &mips);
            ID3D11Texture2D *tw = Make(&warp, &g_fmts[i], p, &w, &h, &mips);
            for (UINT m = 0; m < mips; m++) {
                cases++;
                float om[16][16][4], ow[16][16][4];
                if (!tm || !tw || !Read(&mali, tm, m, om) || !Read(&warp, tw, m, ow)) {
                    printf("  %-15s %-6s mip %u: FAILED to run\n", g_fmts[i].name, g_paths[p], m);
                    fails++;
                    continue;
                }
                UINT mw = umax1(w >> m), mh = umax1(h >> m), bad = 0;
                /* one step of the 8-bit storage the emulation decodes into: 1/255 UNORM; 1/127 SNORM; sRGB values are
                 * stored encoded and converted at sampling, so an 8-bit rounding of the decode shows as up to ~2 steps */
                bool coarse = strstr(g_fmts[i].name, "SNORM") || strstr(g_fmts[i].name, "SRGB");
                float tol = coarse ? 1.0f / 117 : 1.0f / 200;
                float worst = 0;
                int wx = 0, wy = 0;
                for (UINT y = 0; y < mh; y++)
                    for (UINT x = 0; x < mw; x++)
                        for (int c = 0; c < 4; c++) {
                            float dlt = fabsf(om[y][x][c] - ow[y][x][c]);
                            if (dlt > worst) { worst = dlt; wx = (int)x; wy = (int)y; }
                            if (dlt > tol)
                                bad++;
                        }
                if (lossy) {
                    /* G26: PSNR over the format's channels (BC4 R, BC5 RG, else RGBA); peak 1 (UNORM) or 2 (SNORM) */
                    int bcn = g_fmts[i].name[2] - '0', nch = bcn == 4 ? 1 : bcn == 5 ? 2 : 4;
                    double peak = strstr(g_fmts[i].name, "SNORM") ? 2.0 : 1.0, sse = 0, n = 0;
                    int trans_bad = 0;
                    for (UINT y = 0; y < mh; y++)
                        for (UINT x = 0; x < mw; x++) {
                            for (int c = 0; c < nch; c++) {
                                double dd = (double)om[y][x][c] - ow[y][x][c];
                                sse += dd * dd;
                                n++;
                            }
                            if (bcn == 1 && (ow[y][x][3] < 0.5f) != (om[y][x][3] < 0.5f))
                                trans_bad++;
                        }
                    double db = sse > 0 ? 10.0 * log10(peak * peak * n / sse) : 99.0;
                    bool lbad = db < (g_soft ? 30.0 : 10.0) || trans_bad;
                    printf("  %-15s %-6s mip %u (%ux%u): %s, PSNR %.2f dB, worst diff %.4f%s\n", g_fmts[i].name,
                           g_paths[p], m, mw, mh, lbad ? "LOW" : "ok", db, worst,
                           trans_bad ? ", 1-bit alpha MISMATCH" : "");
                    fails += lbad;
                    continue;
                }
                printf("  %-15s %-6s mip %u (%ux%u): %s, worst diff %.4f at (%d,%d) Mali (%.3f %.3f %.3f %.3f) WARP "
                       "(%.3f %.3f %.3f %.3f)\n", g_fmts[i].name, g_paths[p], m, mw, mh, bad ? "DIFF" : "ok", worst, wx,
                       wy, om[wy][wx][0], om[wy][wx][1], om[wy][wx][2], om[wy][wx][3], ow[wy][wx][0], ow[wy][wx][1],
                       ow[wy][wx][2], ow[wy][wx][3]);
                fails += bad != 0;
            }
            if (tm) tm->Release();
            if (tw) tw->Release();
        }
    }
    }
    printf("%d of %d cases differ -> %s\n", fails, cases, fails ? "FAIL" : "PASS");
    return fails ? 3 : 0;
}
