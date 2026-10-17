/*
 * mftest.cpp - decode a video file through Media Foundation's Source Reader (the path Windows apps take) and print
 * per-frame FNV-1a 32 hashes of the NV12 luma / CbCr planes, the same lines tools/mfc/mfcdec and mkclip.py print
 * (D10 V4, notes/d10-mfc-decode-flow.md). Also prints which decoder MF picked (its MFT friendly name).
 *
 *   mftest <file.mp4> [-enum | -blockms] [-d3d11] [-q]
 *
 * -d3d11 (G17 V6b, notes/g17-dxva-mfc.md): the Source Reader gets a DXGI device manager on a D3D11 device of the first
 * hardware adapter (the Mali), so the decoder may use D3D11 DXVA - the path players take. Frames come back as D3D11
 * textures; ConvertToContiguousBuffer reads them back through the driver (copy to staging + map).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <mferror.h>
#include <stdio.h>
#include <string.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

/** -d3d11: a VIDEO_SUPPORT device on the first non-software adapter, multithread protected, in a DXGI device manager. **/
static IMFDXGIDeviceManager *CreateD3DManager(void)
{
    IDXGIFactory1 *fac = nullptr;
    IDXGIAdapter1 *ad = nullptr, *pick = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))))
        return nullptr;
    for (UINT i = 0; !pick && fac->EnumAdapters1(i, &ad) == S_OK; i++)
    {
        DXGI_ADAPTER_DESC1 d;
        ad->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId != 0x1414)
        {
            pick = ad;
            printf("d3d11 adapter: %ls\n", d.Description);
        }
        else
            ad->Release();
    }
    fac->Release();
    if (!pick)
        return nullptr;
    ID3D11Device     *dev = nullptr;
    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 }, got = {};
    HRESULT hr = D3D11CreateDevice(pick, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 3,
                                   D3D11_SDK_VERSION, &dev, &got, nullptr);
    pick->Release();
    printf("d3d11 device 0x%08lx FL 0x%x\n", hr, got);
    if (FAILED(hr))
        return nullptr;
    ID3D10Multithread *mt = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&mt))))
    {
        mt->SetMultithreadProtected(TRUE);   // MF uses the device from its own threads
        mt->Release();
    }
    UINT                  token = 0;
    IMFDXGIDeviceManager *mgr = nullptr;
    hr = MFCreateDXGIDeviceManager(&token, &mgr);
    if (SUCCEEDED(hr))
        hr = mgr->ResetDevice(dev, token);
    dev->Release();
    printf("dxgi device manager 0x%08lx\n", hr);
    if (FAILED(hr) && mgr)
    {
        mgr->Release();
        mgr = nullptr;
    }
    return mgr;
}

static unsigned int Fnv(const BYTE *p, UINT32 w, UINT32 rows, UINT32 stride)
{
    unsigned int h = 0x811C9DC5u;
    for (UINT32 r = 0; r < rows; r++)
        for (UINT32 c = 0; c < w; c++)
            h = (h ^ p[r * stride + c]) * 0x01000193u;
    return h;
}

static void PrintDecoder(IMFSourceReader *reader)
{
    IMFSourceReaderEx *ex = nullptr;
    if (FAILED(reader->QueryInterface(IID_PPV_ARGS(&ex))))
        return;
    for (DWORD i = 0;; i++)
    {
        GUID          cat = {};
        IMFTransform *t = nullptr;
        if (FAILED(ex->GetTransformForStream((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &cat, &t)))
            break;
        IMFAttributes *a = nullptr;
        WCHAR          name[128] = L"?";
        if (SUCCEEDED(t->GetAttributes(&a)) && a)
        {
            a->GetString(MFT_FRIENDLY_NAME_Attribute, name, ARRAYSIZE(name), nullptr);
            a->Release();
        }
        printf("transform %lu: %s, name %ls\n", i, cat == MFT_CATEGORY_VIDEO_DECODER ? "video decoder" : "other", name);
        t->Release();
    }
    ex->Release();
}

/** -enum: MFTEnumEx order for H.264 decoders, then activate each and try the file's native type on it. **/
static int Enumerate(const wchar_t *file)
{
    MFT_REGISTER_TYPE_INFO in = { MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate          **acts = nullptr;
    UINT32                 n = 0;
    IMFSourceReader       *reader = nullptr;
    IMFMediaType          *native = nullptr;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT |
                           MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, nullptr,
                           &acts, &n);
    printf("MFTEnumEx 0x%08lx: %u decoder(s)\n", hr, n);
    IMFPluginControl *pc = nullptr;
    if (SUCCEEDED(MFGetPluginControl(&pc)))
    {
        CLSID   pref = {};
        HRESULT g = pc->GetPreferredClsid(MF_Plugin_Type_MFT, L"{34363248-0000-0010-8000-00AA00389B71}", &pref);
        printf("plugin control preferred H.264: 0x%08lx {%08lX-...}\n", g, pref.Data1);
        for (DWORD i = 0; i < 200; i++)
        {
            LPWSTR sel = nullptr;
            if (FAILED(pc->GetPreferredClsidByIndex(MF_Plugin_Type_MFT, i, &sel, &pref)))
            {
                printf("plugin control: %lu preferred MFT entries\n", i);
                break;
            }
            if (sel && wcsstr(sel, L"34363248"))
                printf("  entry %lu: %ls -> {%08lX-...}\n", i, sel, pref.Data1);
            CoTaskMemFree(sel);
        }
        pc->Release();
    }
    if (SUCCEEDED(MFCreateSourceReaderFromURL(file, nullptr, &reader)))
        reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native);
    for (UINT32 i = 0; i < n; i++)
    {
        WCHAR  name[128] = L"?";
        GUID   clsid = {};
        UINT32 flags = 0;
        acts[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, ARRAYSIZE(name), nullptr);
        acts[i]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid);
        acts[i]->GetUINT32(MF_TRANSFORM_FLAGS_Attribute, &flags);
        IMFTransform *t = nullptr;
        HRESULT       a = acts[i]->ActivateObject(IID_PPV_ARGS(&t));
        HRESULT       s = E_FAIL;
        if (SUCCEEDED(a) && native)
            s = t->SetInputType(0, native, 0);
        printf("  %u: %ls clsid {%08lX-...} flags 0x%x activate 0x%08lx SetInputType 0x%08lx\n", i, name, clsid.Data1,
               flags, a, s);
        if (t)
            t->Release();
        acts[i]->ShutdownObject();
        acts[i]->Release();
    }
    if (native)
    {
        GUID   sub = {};
        UINT32 w = 0, h = 0, blob = 0;
        native->GetGUID(MF_MT_SUBTYPE, &sub);
        MFGetAttributeSize(native, MF_MT_FRAME_SIZE, &w, &h);
        native->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &blob);
        printf("native type: subtype {%08lX-...} %ux%u sequence header %u B\n", sub.Data1, w, h, blob);
        native->Release();
    }
    if (reader)
        reader->Release();
    CoTaskMemFree(acts);
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: mftest <file.mp4> [-q]\n");
        return 2;
    }
    bool quiet = argc > 2 && wcscmp(argv[argc - 1], L"-q") == 0;
    bool d3d = false;
    UINT32 rows = 0;   // -rows N: hash only the first N picture rows (the visible part of a padded 1088 picture)
    UINT32 cols = 0;   // -cols N: hash only the first N columns (the visible part of a padded 864 picture)
    for (int i = 2; i < argc; i++)
    {
        d3d = d3d || wcscmp(argv[i], L"-d3d11") == 0;
        if (wcscmp(argv[i], L"-rows") == 0 && i + 1 < argc)
            rows = (UINT32)_wtoi(argv[i + 1]);
        if (wcscmp(argv[i], L"-cols") == 0 && i + 1 < argc)
            cols = (UINT32)_wtoi(argv[i + 1]) & ~1u;   // even: CbCr pairs
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    if (argc > 2 && wcscmp(argv[2], L"-enum") == 0)
        return Enumerate(argv[1]);
    if (argc > 2 && wcscmp(argv[2], L"-blockms") == 0)
    {
        // process-local: take the Microsoft H264 Video Decoder MFT out of this process's enumeration
        static const CLSID msH264 = { 0x62ce7e72, 0x4c71, 0x4d20, { 0xb1, 0x5d, 0x45, 0x28, 0x31, 0xa8, 0x7d, 0x9d } };
        IMFPluginControl  *pc = nullptr;
        if (SUCCEEDED(MFGetPluginControl(&pc)))
        {
            printf("blocked Microsoft H.264 decoder in this process: 0x%08lx\n",
                   pc->SetDisabled(MF_Plugin_Type_MFT, msH264, TRUE));
            pc->Release();
        }
    }

    IMFAttributes   *attr = nullptr;
    IMFSourceReader *reader = nullptr;
    IMFMediaType    *type = nullptr;
    IMFDXGIDeviceManager *mgr = d3d ? CreateD3DManager() : nullptr;
    if (d3d && !mgr)
        return 1;
    HRESULT          hr = MFCreateAttributes(&attr, 2);
    if (SUCCEEDED(hr)) hr = attr->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);   // the decoder's own NV12
    if (SUCCEEDED(hr) && mgr) hr = attr->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, mgr);
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromURL(argv[1], attr, &reader);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (SUCCEEDED(hr)) hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type);
    if (FAILED(hr))
    {
        fprintf(stderr, "source reader setup failed 0x%08lx\n", hr);
        return 1;
    }
    PrintDecoder(reader);

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    unsigned int frames = 0;
    for (;;)
    {
        DWORD      stream = 0, flags = 0;
        LONGLONG   ts = 0;
        IMFSample *s = nullptr;
        hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &ts, &s);
        if (FAILED(hr))
        {
            fprintf(stderr, "ReadSample 0x%08lx\n", hr);
            break;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
        if (!s)
            continue;
        IMFMediaType *cur = nullptr;
        UINT32        w = 0, h = 0;
        reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
        MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
        UINT32 stride = MFGetAttributeUINT32(cur, MF_MT_DEFAULT_STRIDE, w);
        cur->Release();
        IMFMediaBuffer *b = nullptr;
        BYTE           *p = nullptr;
        DWORD           len = 0;
        if (d3d && frames == 0)
        {
            IMFMediaBuffer *b0 = nullptr;
            IMFDXGIBuffer  *db = nullptr;
            if (SUCCEEDED(s->GetBufferByIndex(0, &b0)) && SUCCEEDED(b0->QueryInterface(IID_PPV_ARGS(&db))))
            {
                ID3D11Texture2D *tex = nullptr;
                UINT             sub = 0;
                db->GetSubresourceIndex(&sub);
                if (SUCCEEDED(db->GetResource(IID_PPV_ARGS(&tex))))
                {
                    D3D11_TEXTURE2D_DESC td;
                    tex->GetDesc(&td);
                    printf("output texture %ux%u fmt %u array %u bind 0x%x misc 0x%x, subresource %u\n", td.Width,
                           td.Height, td.Format, td.ArraySize, td.BindFlags, td.MiscFlags, sub);
                    tex->Release();
                }
                db->Release();
            }
            else
                printf("output is not a DXGI buffer (decoder used system memory)\n");
            if (b0) b0->Release();
        }
        if (SUCCEEDED(s->ConvertToContiguousBuffer(&b)) && SUCCEEDED(b->Lock(&p, nullptr, &len)))
        {
            UINT32 hr2 = (rows && rows < h) ? rows : h;   // hashed rows; the CbCr plane still starts after h rows
            UINT32 wc = (cols && cols < w) ? cols : w;    // hashed columns (bytes per row, both planes)
            if (!quiet)
                printf("frame %u: ts %lld %ux%u luma fnv 0x%08x chroma fnv 0x%08x\n", frames, ts, wc, hr2,
                       Fnv(p, wc, hr2, stride), Fnv(p + stride * h, wc, hr2 / 2, stride));
            b->Unlock();
        }
        if (b) b->Release();
        s->Release();
        frames++;
    }
    QueryPerformanceCounter(&t1);
    double sec = (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
    printf("done: %u frames, %.3f s = %.1f fps\n", frames, sec, sec > 0 ? frames / sec : 0.0);
    reader->Release();
    type->Release();
    attr->Release();
    if (mgr) mgr->Release();
    MFShutdown();
    return 0;
}
