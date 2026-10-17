/*
 * mfcdec.c - decode an H.264 Annex-B file on the S10e's hardware decoder (Exynos9820Mfc IOCTLs, D10 V3,
 * notes/d10-mfc-decode-flow.md). Prints one line per displayed picture with FNV-1a 32 hashes of the CROPPED luma
 * and NV12 CbCr planes (compare with tools/mfc/mkclip.py, which hashes libavcodec's output the same way), then
 * the throughput. Optionally writes the cropped NV12 pictures to a file.
 *
 *   mfcdec <in.h264> [out.nv12] [-q]      (-q: summary only)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "..\..\..\drivers\Exynos9820Mfc\MfcIoctl.h"

#pragma comment(lib, "cfgmgr32.lib")

#define OUT_CAPACITY  (sizeof(MFC_DECODE_OUT) + 4096u * 2304u * 3u / 2u)

static unsigned int Fnv(const unsigned char *p, unsigned int w, unsigned int rows, unsigned int stride)
{
    unsigned int h = 0x811C9DC5u, r, c;

    for (r = 0; r < rows; r++)
        for (c = 0; c < w; c++)
            h = (h ^ p[r * stride + c]) * 0x01000193u;
    return h;
}

static HANDLE OpenMfc(void)
{
    WCHAR  list[1024];
    HANDLE h;

    if (CM_Get_Device_Interface_ListW((LPGUID)&GUID_DEVINTERFACE_EXYNOS9820_MFC, NULL, list, ARRAYSIZE(list),
                                      CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || list[0] == 0)
    {
        fprintf(stderr, "no Exynos9820Mfc device interface\n");
        return INVALID_HANDLE_VALUE;
    }

    h = CreateFileW(list, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        fprintf(stderr, "open %ls: %lu\n", list, GetLastError());
    return h;
}

static unsigned char *ReadAll(const char *path, unsigned int *size)
{
    FILE          *f = fopen(path, "rb");
    unsigned char *buf;
    long           n;

    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (unsigned char *)malloc(n > 0 ? n : 1);
    if (buf != NULL && fread(buf, 1, n, f) != (size_t)n)
    {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = (unsigned int)n;
    return buf;
}

int main(int argc, char **argv)
{
    const char     *outPath = NULL;
    int             quiet = 0, i;
    unsigned int    size, offset = 0, frames = 0, errors = 0, calls = 0;
    unsigned int    outLen = OUT_CAPACITY;     // after the header: exactly header + one picture (fewer pages to lock)
    unsigned int    maxAu = 0;                 // largest Consumed so far -> window = max(1 MiB, 4 x maxAu)
    unsigned char  *stream;
    unsigned char  *outBuf;
    FILE           *out = NULL;
    HANDLE          dev;
    MFC_OPEN_IN     open = { MFC_IOCTL_VERSION, MFC_CODEC_H264 };
    DWORD           got;
    LARGE_INTEGER   freq, t0, t1;
    unsigned long long hwUs = 0;

    if (argc < 2)
    {
        fprintf(stderr, "usage: mfcdec <in.h264> [out.nv12] [-q] [-order]\n");
        return 2;
    }

    for (i = 2; i < argc; i++)
    {
        if (strcmp(argv[i], "-q") == 0)
            quiet = 1;
        else if (strcmp(argv[i], "-order") == 0)
            open.Flags = MFC_OPEN_DECODE_ORDER;   // decode order (G17 V6b1, driver 1.0.0.7+)
        else
            outPath = argv[i];
    }

    stream = ReadAll(argv[1], &size);
    outBuf = (unsigned char *)malloc(OUT_CAPACITY);
    if (stream == NULL || outBuf == NULL)
    {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }

    if (outPath != NULL && (out = fopen(outPath, "wb")) == NULL)
    {
        fprintf(stderr, "cannot write %s\n", outPath);
        return 1;
    }

    dev = OpenMfc();
    if (dev == INVALID_HANDLE_VALUE)
        return 1;
    if (!DeviceIoControl(dev, IOCTL_MFC_OPEN, &open, sizeof(open), NULL, 0, &got, NULL))
    {
        fprintf(stderr, "IOCTL_MFC_OPEN: %lu\n", GetLastError());
        return 1;
    }

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    for (;;)
    {
        unsigned int          window = size - offset;
        const MFC_DECODE_OUT *o = (const MFC_DECODE_OUT *)outBuf;

        {
            unsigned int cap = maxAu * 4u > (1u << 20) ? maxAu * 4u : (1u << 20);
            if (cap > MFC_MAX_WINDOW)
                cap = MFC_MAX_WINDOW;
            if (window > cap)
                window = cap;
        }
        if (window <= 4)   // stuffing only: drain
            window = 0;
        calls++;
        if (!DeviceIoControl(dev, IOCTL_MFC_DECODE, window ? stream + offset : NULL, window, outBuf, outLen,
                             &got, NULL))
        {
            fprintf(stderr, "IOCTL_MFC_DECODE at %u (window %u): %lu\n", offset, window, GetLastError());
            break;
        }

        hwUs += o->HwMicroseconds;
        if (o->Flags & MFC_DEC_HEADER)
            outLen = (unsigned int)sizeof(MFC_DECODE_OUT) + o->Width * o->Height * 3u / 2u;
        if (o->Flags & MFC_DEC_HEADER)
            printf("header: %ux%u crop l%u t%u r%u b%u, consumed %u\n", o->Width, o->Height, o->CropLeft, o->CropTop,
                   o->CropRight, o->CropBottom, o->Consumed);
        if (o->Flags & MFC_DEC_ERROR)
        {
            errors++;
            printf("error: R2H %u err 0x%x status 0x%x at %u, consumed %u\n", o->R2h, o->Error, o->DisplayStatus,
                   offset, o->Consumed);
        }

        if (o->Flags & MFC_DEC_FRAME)
        {
            const unsigned char *y  = (const unsigned char *)(o + 1);
            const unsigned char *uv = y + o->Width * o->Height;
            unsigned int         cw = o->Width - o->CropLeft - o->CropRight;
            unsigned int         ch = o->Height - o->CropTop - o->CropBottom;
            const unsigned char *cy = y + o->CropTop * o->Width + o->CropLeft;
            const unsigned char *cu = uv + (o->CropTop / 2) * o->Width + (o->CropLeft & ~1u);

            if (!quiet)
                printf("frame %u: type %u %s%u us, luma fnv 0x%08x chroma fnv 0x%08x\n", frames, o->FrameType,
                       (o->Flags & MFC_DEC_WARNING) ? "WARN " : "", o->HwMicroseconds, Fnv(cy, cw, ch, o->Width),
                       Fnv(cu, cw, ch / 2, o->Width));
            if (out != NULL)
            {
                unsigned int r;
                for (r = 0; r < ch; r++)
                    fwrite(cy + r * o->Width, 1, cw, out);
                for (r = 0; r < ch / 2; r++)
                    fwrite(cu + r * o->Width, 1, cw, out);
            }
            frames++;
        }

        if (o->Flags & MFC_DEC_EOS)
            break;
        if (!(o->Flags & MFC_DEC_NOT_CONSUMED))
        {
            offset += o->Consumed;
            if (o->Consumed > maxAu && o->Consumed < window)
                maxAu = o->Consumed;
        }
        if (window == 0 && !(o->Flags & (MFC_DEC_FRAME | MFC_DEC_EOS)))
        {
            fprintf(stderr, "drain made no progress\n");
            break;
        }
        if (calls > 100000)
            break;
    }

    QueryPerformanceCounter(&t1);
    {
        double s = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
        printf("done: %u frames, %u errors, %u calls, %.3f s wall = %.1f fps, hw wait %.1f ms total (%.2f ms/frame)\n",
               frames, errors, calls, s, s > 0 ? frames / s : 0.0, hwUs / 1000.0,
               frames ? hwUs / 1000.0 / frames : 0.0);
    }

    DeviceIoControl(dev, IOCTL_MFC_CLOSE, NULL, 0, NULL, 0, &got, NULL);
    CloseHandle(dev);
    if (out != NULL)
        fclose(out);
    return errors ? 3 : 0;
}
