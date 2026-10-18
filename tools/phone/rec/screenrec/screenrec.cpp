/*
 * screenrec - record the phone screen for a remote viewer (notes/g27-x64-apps.md, "Remote viewing").
 *   screenrec.exe SECONDS FPS SCALE DIR [LAUNCH [LAUNCHARGS]]
 * Run in the interactive user session. DPI-aware GDI frames (BitBlt of the screen DC, StretchBlt to SCALE) every
 * 1/FPS s, JPEG-encoded with WIC into DIR\frame_NNNN.jpg; DIR\frames.txt gets the time of each frame. LAUNCH starts
 * right after the first frame. For the run: screensaver off (SPI_SETSCREENSAVEACTIVE, the previous value restored at
 * the end - MS Learn: SetThreadExecutionState "does not stop the screen saver from executing") and
 * SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED). Test tooling, not driver code.
 * Build: build-screenrec.cmd. SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>

static bool SaveJpeg(IWICImagingFactory *wic, HBITMAP bmp, const wchar_t *path)
{
    IWICBitmap *src = NULL;
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    IPropertyBag2 *props = NULL;
    bool ok = false;
    if (FAILED(wic->CreateBitmapFromHBITMAP(bmp, NULL, WICBitmapIgnoreAlpha, &src))) goto out;
    if (FAILED(wic->CreateStream(&st)) || FAILED(st->InitializeFromFilename(path, GENERIC_WRITE))) goto out;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatJpeg, NULL, &enc)) ||
        FAILED(enc->Initialize(st, WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&fr, &props)))
        goto out;
    {
        PROPBAG2 opt = {};
        opt.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.7f;
        props->Write(1, &opt, &v);
    }
    if (FAILED(fr->Initialize(props)) || FAILED(fr->WriteSource(src, NULL)) || FAILED(fr->Commit()) ||
        FAILED(enc->Commit()))
        goto out;
    ok = true;
out:
    if (props) props->Release();
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    if (src) src->Release();
    return ok;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 5) {
        fwprintf(stderr, L"usage: screenrec SECONDS FPS SCALE DIR [LAUNCH [LAUNCHARGS]]\n");
        return 1;
    }
    double seconds = _wtof(argv[1]), fps = _wtof(argv[2]), scale = _wtof(argv[3]);
    const wchar_t *dir = argv[4];
    const wchar_t *launch = argc > 5 ? argv[5] : NULL, *largs = argc > 6 ? argv[6] : NULL;
    if (fps <= 0 || scale <= 0 || scale > 1)
        return 1;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CreateDirectoryW(dir, NULL);
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s\\frames.txt", dir);
    FILE *log = _wfopen(path, L"w");
    if (!log)
        return 2;
    BOOL ssWas = FALSE;
    SystemParametersInfoW(SPI_GETSCREENSAVEACTIVE, 0, &ssWas, 0);
    SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, FALSE, NULL, 0);
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IWICImagingFactory *wic = NULL;
    CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));

    int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int sw = (int)(w * scale), sh = (int)(h * scale);
    fwprintf(log, L"start screen %dx%d -> %dx%d seconds %g fps %g screensaver-was %d launch '%s %s'\n", w, h, sw, sh,
             seconds, fps, ssWas, launch ? launch : L"", largs ? largs : L"");
    fflush(log);
    HDC scr = GetDC(NULL);
    HDC mem = CreateCompatibleDC(scr);
    HBITMAP bmp = CreateCompatibleBitmap(scr, sw, sh);
    HGDIOBJ old = SelectObject(mem, bmp);
    SetStretchBltMode(mem, HALFTONE);
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    int n = 0;
    for (;;) {
        QueryPerformanceCounter(&t);
        double ms = (t.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
        if (ms >= seconds * 1000)
            break;
        double due = n * 1000.0 / fps;
        if (due > ms) {
            Sleep((DWORD)(due - ms));
            QueryPerformanceCounter(&t);
            ms = (t.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
        }
        BOOL got = StretchBlt(mem, 0, 0, sw, sh, scr, x, y, w, h, SRCCOPY | CAPTUREBLT);
        SelectObject(mem, old);
        swprintf(path, MAX_PATH, L"%s\\frame_%04d.jpg", dir, n);
        bool saved = got && wic && SaveJpeg(wic, bmp, path);
        SelectObject(mem, bmp);
        fwprintf(log, saved ? L"%04d %7d ms\n" : L"%04d %7d ms capture failed (blt %d)\n", n, (int)ms, got);
        fflush(log);
        if (n == 0 && launch) {
            HINSTANCE r = ShellExecuteW(NULL, L"open", launch, largs, NULL, SW_SHOWNORMAL);
            fwprintf(log, L"launched %s -> %d\n", launch, (int)(INT_PTR)r);
            fflush(log);
        }
        n++;
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, scr);
    if (wic) wic->Release();
    SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, ssWas, NULL, 0);
    SetThreadExecutionState(ES_CONTINUOUS);
    fwprintf(log, L"end frames %d screensaver restored to %d\n", n, ssWas);
    fclose(log);
    return 0;
}
