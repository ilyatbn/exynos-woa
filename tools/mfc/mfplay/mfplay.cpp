/*
 * mfplay.cpp - G17 V6c (notes/g17-dxva-mfc.md): play a video file with the Media Foundation Media Engine on a D3D11
 * device of the first hardware adapter (the Mali), in frame-server mode: every new frame is converted by the engine
 * into our BGRA texture with TransferVideoFrame - the decode + video processor path players use. Audio muted.
 * Prints the device, per-frame pixel hashes of the first frames (BGRA, read back through a staging copy), and the
 * frame rate.
 *
 *   mfplay <file.mp4> [-frames N] [-seconds S]
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

static HANDLE g_ended, g_loaded;
static volatile LONG g_error;

class Notify : public IMFMediaEngineNotify
{
    LONG m_ref = 1;

public:
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify))
        {
            *ppv = static_cast<IMFMediaEngineNotify *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0)
            delete this;
        return r;
    }
    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR param1, DWORD param2) override
    {
        switch (event)
        {
        case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
            SetEvent(g_loaded);
            break;
        case MF_MEDIA_ENGINE_EVENT_ENDED:
            SetEvent(g_ended);
            break;
        case MF_MEDIA_ENGINE_EVENT_ERROR:
            printf("engine error %u hr 0x%08lx\n", (unsigned)param1, (unsigned long)param2);
            InterlockedExchange(&g_error, 1);
            SetEvent(g_loaded);
            SetEvent(g_ended);
            break;
        }
        return S_OK;
    }
};

static unsigned int Fnv(const BYTE *p, UINT32 bytes, UINT32 rows, UINT32 stride)
{
    unsigned int h = 0x811C9DC5u;
    for (UINT32 r = 0; r < rows; r++)
        for (UINT32 c = 0; c < bytes; c++)
            h = (h ^ p[r * stride + c]) * 0x01000193u;
    return h;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: mfplay <file.mp4> [-frames N] [-seconds S]\n");
        return 2;
    }
    UINT maxFrames = 100000, seconds = 30;
    for (int i = 2; i + 1 < argc; i++)
    {
        if (wcscmp(argv[i], L"-frames") == 0)
            maxFrames = (UINT)_wtoi(argv[i + 1]);
        if (wcscmp(argv[i], L"-seconds") == 0)
            seconds = (UINT)_wtoi(argv[i + 1]);
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    g_ended = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_loaded = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    IDXGIFactory1 *fac = nullptr;
    IDXGIAdapter1 *ad = nullptr, *pick = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&fac));
    for (UINT i = 0; fac && !pick && fac->EnumAdapters1(i, &ad) == S_OK; i++)
    {
        DXGI_ADAPTER_DESC1 d;
        ad->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId != 0x1414)
        {
            pick = ad;
            printf("adapter: %ls\n", d.Description);
        }
        else
            ad->Release();
    }
    ID3D11Device        *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    D3D_FEATURE_LEVEL    fl[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 }, got = {};
    HRESULT hr = D3D11CreateDevice(pick, pick ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 3,
                                   D3D11_SDK_VERSION, &dev, &got, &ctx);
    printf("device 0x%08lx FL 0x%x\n", hr, got);
    if (FAILED(hr))
        return 1;
    ID3D10Multithread *mt = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&mt))))
    {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }
    UINT                  token = 0;
    IMFDXGIDeviceManager *mgr = nullptr;
    MFCreateDXGIDeviceManager(&token, &mgr);
    mgr->ResetDevice(dev, token);

    IMFMediaEngineClassFactory *cf = nullptr;
    IMFAttributes              *attr = nullptr;
    IMFMediaEngine             *eng = nullptr;
    Notify                     *notify = new Notify();
    hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cf));
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&attr, 3);
    if (SUCCEEDED(hr)) hr = attr->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, mgr);
    if (SUCCEEDED(hr)) hr = attr->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify);
    if (SUCCEEDED(hr)) hr = attr->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (SUCCEEDED(hr)) hr = cf->CreateInstance(0, attr, &eng);
    printf("media engine 0x%08lx\n", hr);
    if (FAILED(hr))
        return 1;
    eng->SetMuted(TRUE);   // quiet: video only
    BSTR url = SysAllocString(argv[1]);
    hr = eng->SetSource(url);
    SysFreeString(url);
    if (FAILED(hr) || WaitForSingleObject(g_loaded, 10000) != WAIT_OBJECT_0 || g_error)
    {
        printf("load failed 0x%08lx\n", hr);
        return 1;
    }
    DWORD w = 0, h = 0;
    eng->GetNativeVideoSize(&w, &h);
    printf("video %lux%lu\n", w, h);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *rt = nullptr, *st = nullptr;
    dev->CreateTexture2D(&td, nullptr, &rt);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    dev->CreateTexture2D(&td, nullptr, &st);

    hr = eng->Play();
    printf("play 0x%08lx\n", hr);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    UINT frames = 0, transferFail = 0;
    RECT r = { 0, 0, (LONG)w, (LONG)h };
    MFARGB black = { 0, 0, 0, 255 };
    while (frames < maxFrames && WaitForSingleObject(g_ended, 1) != WAIT_OBJECT_0)
    {
        QueryPerformanceCounter(&t1);
        if ((t1.QuadPart - t0.QuadPart) / f.QuadPart >= (LONGLONG)seconds)
            break;
        LONGLONG pts = 0;
        if (eng->OnVideoStreamTick(&pts) != S_OK)
            continue;
        hr = eng->TransferVideoFrame(rt, nullptr, &r, &black);
        if (FAILED(hr))
        {
            if (transferFail++ < 3)
                printf("TransferVideoFrame 0x%08lx\n", hr);
            continue;
        }
        if (frames < 5)
        {
            D3D11_MAPPED_SUBRESOURCE m;
            ctx->CopyResource(st, rt);
            if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m)))
            {
                const BYTE *p = (const BYTE *)m.pData;
                const BYTE *c = p + (h / 2) * m.RowPitch + (w / 2) * 4;
                printf("frame %u: pts %lld bgra fnv 0x%08x center %u,%u,%u\n", frames, pts,
                       Fnv(p, w * 4, h, m.RowPitch), c[0], c[1], c[2]);
                ctx->Unmap(st, 0);
            }
        }
        frames++;
    }
    QueryPerformanceCounter(&t1);
    double sec = (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
    printf("done: %u frames in %.2f s = %.1f fps, %u transfer failures, ended %d, error %ld\n", frames, sec,
           sec > 0 ? frames / sec : 0.0, transferFail, WaitForSingleObject(g_ended, 0) == WAIT_OBJECT_0, g_error);
    eng->Shutdown();
    eng->Release();
    return 0;
}
