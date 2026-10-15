/*
 * mftest.cpp - decode a video file through Media Foundation's Source Reader (the path Windows apps take) and print
 * per-frame FNV-1a 32 hashes of the NV12 luma / CbCr planes, the same lines tools/mfc/mfcdec and mkclip.py print
 * (D10 V4, notes/d10-mfc-decode-flow.md). Also prints which decoder MF picked (its MFT friendly name).
 *
 *   mftest <file.mp4> [-q]
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

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

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

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: mftest <file.mp4> [-q]\n");
        return 2;
    }
    bool quiet = argc > 2 && wcscmp(argv[2], L"-q") == 0;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    IMFAttributes   *attr = nullptr;
    IMFSourceReader *reader = nullptr;
    IMFMediaType    *type = nullptr;
    HRESULT          hr = MFCreateAttributes(&attr, 1);
    if (SUCCEEDED(hr)) hr = attr->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);   // the decoder's own NV12
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
        if (SUCCEEDED(s->ConvertToContiguousBuffer(&b)) && SUCCEEDED(b->Lock(&p, nullptr, &len)))
        {
            if (!quiet)
                printf("frame %u: ts %lld %ux%u luma fnv 0x%08x chroma fnv 0x%08x\n", frames, ts, w, h,
                       Fnv(p, w, h, stride), Fnv(p + stride * h, w, h / 2, stride));
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
    MFShutdown();
    return 0;
}
