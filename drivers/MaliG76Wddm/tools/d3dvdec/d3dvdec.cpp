/*
 * d3dvdec - G17 V6a/V6b test (notes/g17-dxva-mfc.md): the D3D11 video decoder path of the Mali UMD as an app sees it.
 * D3D11CreateDevice(VIDEO_SUPPORT) on the Mali, ID3D11VideoDevice: profiles, NV12 support, configs for 1920x1080,
 * CreateVideoDecoder, an NV12 BIND_DECODER texture + decoder output view. "warp" runs the same queries on WARP.
 * Exit 0 = every step succeeded. Build: build-d3dvdec.cmd. SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <stdio.h>
#include <d3d11.h>
#include <initguid.h>

DEFINE_GUID(kH264VldNoFgt, 0x1b81be68, 0xa0c7, 0x11d3, 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5);

static void PrintGuid(const GUID &g)
{
    printf("{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1],
           g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

int main(int argc, char **argv)
{
    bool warp = argc > 1 && !strcmp(argv[1], "warp");
    IDXGIFactory1 *fac = NULL;
    IDXGIAdapter1 *ad = NULL, *pick = NULL;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac);
    for (UINT i = 0; fac && fac->EnumAdapters1(i, &ad) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        ad->GetDesc1(&d);
        if (!pick && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId != 0x1414) {
            pick = ad;
            printf("adapter: %ls\n", d.Description);
        } else {
            ad->Release();
        }
    }
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 }, got;
    HRESULT hr = D3D11CreateDevice(warp ? NULL : pick, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, NULL,
                                   D3D11_CREATE_DEVICE_VIDEO_SUPPORT, fl, 2, D3D11_SDK_VERSION, &dev, &got, &ctx);
    printf("D3D11CreateDevice(VIDEO_SUPPORT) 0x%08lx FL 0x%x\n", hr, got);
    if (FAILED(hr))
        return 1;
    ID3D11VideoDevice *vd = NULL;
    hr = dev->QueryInterface(__uuidof(ID3D11VideoDevice), (void **)&vd);
    printf("QueryInterface(ID3D11VideoDevice) 0x%08lx\n", hr);
    if (FAILED(hr))
        return 2;
    UINT n = vd->GetVideoDecoderProfileCount(), i;
    bool hasH264 = false;
    printf("decoder profiles: %u\n", n);
    for (i = 0; i < n; i++) {
        GUID g;
        if (SUCCEEDED(vd->GetVideoDecoderProfile(i, &g))) {
            printf("  ");
            PrintGuid(g);
            if (IsEqualGUID(g, kH264VldNoFgt)) {
                printf("  H264_VLD_NOFGT");
                hasH264 = true;
            }
            printf("\n");
        }
    }
    if (!hasH264)
        return 3;
    BOOL nv12 = FALSE;
    hr = vd->CheckVideoDecoderFormat(&kH264VldNoFgt, DXGI_FORMAT_NV12, &nv12);
    printf("CheckVideoDecoderFormat(NV12) 0x%08lx -> %d\n", hr, nv12);
    D3D11_VIDEO_DECODER_DESC desc = { kH264VldNoFgt, 1920, 1080, DXGI_FORMAT_NV12 };
    UINT nc = 0;
    hr = vd->GetVideoDecoderConfigCount(&desc, &nc);
    printf("GetVideoDecoderConfigCount(1920x1080) 0x%08lx -> %u\n", hr, nc);
    D3D11_VIDEO_DECODER_CONFIG cfg = {};
    for (i = 0; i < nc; i++) {
        D3D11_VIDEO_DECODER_CONFIG c;
        if (SUCCEEDED(vd->GetVideoDecoderConfig(&desc, i, &c))) {
            printf("  config %u: BitstreamRaw %u DecoderSpecific 0x%x MinRT %u\n", i, c.ConfigBitstreamRaw,
                   c.ConfigDecoderSpecific, c.ConfigMinRenderTargetBuffCount);
            if (c.ConfigBitstreamRaw == 2 || (warp && cfg.ConfigBitstreamRaw == 0))
                cfg = c;
        }
    }
    if (nc == 0)
        return 4;
    ID3D11VideoDecoder *dec = NULL;
    hr = vd->CreateVideoDecoder(&desc, &cfg, &dec);
    printf("CreateVideoDecoder 0x%08lx\n", hr);
    if (FAILED(hr))
        return 5;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 1920; td.Height = 1080; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DECODER;
    ID3D11Texture2D *tex = NULL;
    UINT sup = 0;
    dev->CheckFormatSupport(DXGI_FORMAT_NV12, &sup);
    printf("CheckFormatSupport(NV12) 0x%08x\n", sup);
    hr = dev->CreateTexture2D(&td, NULL, &tex);
    printf("CreateTexture2D(NV12, BIND_DECODER) 0x%08lx\n", hr);
    if (FAILED(hr))
        return 6;
    D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC od = {};
    od.DecodeProfile = kH264VldNoFgt;
    od.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
    ID3D11VideoDecoderOutputView *ov = NULL;
    hr = vd->CreateVideoDecoderOutputView(tex, &od, &ov);
    printf("CreateVideoDecoderOutputView 0x%08lx\n", hr);
    if (FAILED(hr))
        return 7;
    printf("PASS\n");
    return 0;
}
