/*
 * screenrec - record the phone screen for a remote viewer (notes/g27-x64-apps.md, "Remote viewing").
 *   screenrec.exe [-dup] SECONDS FPS SCALE DIR [LAUNCH [LAUNCHARGS]]
 * -dup: DXGI Desktop Duplication instead of GDI - GDI cannot capture a fullscreen D3D game (frames black). MS Learn
 * "Desktop Duplication API": the image is always B8G8R8A8_UNORM, "always in the un-rotated orientation, and the desktop
 * image is rotated within the surface" (DXGI_OUTDUPL_DESC.Rotation; rotated back here); ACCESS_LOST (mode change,
 * fullscreen switch) -> duplicate again. AcquireNextFrame returns only changed frames, so the last one is kept.
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
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdlib.h>

/* A screensaver that is already running (or starts later: restoring SPI_SETSCREENSAVEACTIVE at the end of the previous
 * run lets it start at once, the input-idle time having long passed) makes every BitBlt of the screen fail. Close it
 * when SPI_GETSCREENSAVERRUNNING says one runs: WM_CLOSE to the windows of the "Screen-saver" desktop, or to the
 * foreground window - but only if that window belongs to a *.scr process. */
static BOOL CALLBACK CloseScrWindow(HWND h, LPARAM)
{
    PostMessageW(h, WM_CLOSE, 0, 0);
    return TRUE;
}

static int CloseScreensaver(void)
{
    BOOL running = FALSE;
    if (!SystemParametersInfoW(SPI_GETSCREENSAVERRUNNING, 0, &running, 0) || !running)
        return 0;
    HDESK d = OpenDesktopW(L"Screen-saver", 0, FALSE, DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS);
    if (d) {
        EnumDesktopWindows(d, CloseScrWindow, 0);
        CloseDesktop(d);
        return 1;
    }
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    wchar_t img[MAX_PATH] = L"";
    DWORD n = MAX_PATH;
    if (p) {
        QueryFullProcessImageNameW(p, 0, img, &n);
        CloseHandle(p);
    }
    size_t len = wcslen(img);
    if (len > 4 && !_wcsicmp(img + len - 4, L".scr")) {
        PostMessageW(fg, WM_CLOSE, 0, 0);
        return 2;
    }
    return 3;   /* running, but no screensaver window found to close */
}

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

struct Dup {
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    IDXGIOutput1 *out;
    IDXGIOutputDuplication *dup;
    ID3D11Texture2D *stage;
    DXGI_OUTDUPL_DESC desc;
    BYTE *last;          /* CPU copy of the last frame (un-rotated, desc.ModeDesc size, BGRA) */
    UINT lastPitch, sw, sh;
    bool haveFrame;
};

static HRESULT DupOpen(Dup *d, FILE *log)
{
    if (d->dup) { d->dup->Release(); d->dup = NULL; }
    if (!d->dev) {
        D3D_FEATURE_LEVEL fl;
        HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &d->dev, &fl,
                                       &d->ctx);
        if (FAILED(hr)) return hr;
        IDXGIDevice *xd = NULL; IDXGIAdapter *ad = NULL; IDXGIOutput *o = NULL;
        d->dev->QueryInterface(IID_PPV_ARGS(&xd));
        xd->GetAdapter(&ad);
        hr = ad->EnumOutputs(0, &o);
        DXGI_ADAPTER_DESC ade; ad->GetDesc(&ade);
        fwprintf(log, L"dup: device FL 0x%x adapter '%s', output 0 0x%08lx\n", fl, ade.Description, (unsigned long)hr);
        if (SUCCEEDED(hr)) { o->QueryInterface(IID_PPV_ARGS(&d->out)); o->Release(); }
        ad->Release(); xd->Release();
        if (!d->out) return E_FAIL;
    }
    HRESULT hr = d->out->DuplicateOutput(d->dev, &d->dup);
    if (FAILED(hr)) return hr;
    d->dup->GetDesc(&d->desc);
    fwprintf(log, L"dup: %ux%u rotation %d\n", d->desc.ModeDesc.Width, d->desc.ModeDesc.Height, (int)d->desc.Rotation);
    /* the staging copy follows the acquired surface: it is un-rotated (here 720x1520 while ModeDesc says 1520x720) */
    d->haveFrame = false;
    return hr;
}

/* Take the newest frame (or keep the last one), rotate it upright and scale it into bits (sw x sh, top-down BGRA). */
static bool DupGrab(Dup *d, BYTE *bits, int sw, int sh, FILE *log)
{
    if (!d->dup && FAILED(DupOpen(d, log)))
        return false;
    DXGI_OUTDUPL_FRAME_INFO fi;
    IDXGIResource *res = NULL;
    HRESULT hr = d->dup->AcquireNextFrame(100, &fi, &res);
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        fwprintf(log, L"dup: access lost - duplicating again\n");
        if (FAILED(DupOpen(d, log))) return false;
        hr = d->dup->AcquireNextFrame(100, &fi, &res);
    }
    if (SUCCEEDED(hr)) {
        ID3D11Texture2D *tex = NULL;
        res->QueryInterface(IID_PPV_ARGS(&tex));
        D3D11_TEXTURE2D_DESC sd = {};
        if (tex) tex->GetDesc(&sd);
        fwprintf(log, L"dup: frame tex %ux%u fmt %d present %lld accumulated %u\n", sd.Width, sd.Height, (int)sd.Format,
                 fi.LastPresentTime.QuadPart, fi.AccumulatedFrames);
        if (tex && fi.LastPresentTime.QuadPart && (!d->stage || sd.Width != d->sw || sd.Height != d->sh)) {
            if (d->stage) d->stage->Release();
            D3D11_TEXTURE2D_DESC td = sd;
            td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1; td.SampleDesc.Quality = 0;
            td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
            HRESULT h2 = d->dev->CreateTexture2D(&td, NULL, &d->stage);
            free(d->last);
            d->sw = sd.Width; d->sh = sd.Height; d->lastPitch = sd.Width * 4;
            d->last = (BYTE *)calloc(sd.Height, d->lastPitch);
            fwprintf(log, L"dup: staging %ux%u 0x%08lx\n", sd.Width, sd.Height, (unsigned long)h2);
            if (FAILED(h2)) { d->stage = NULL; }
        }
        if (tex && d->stage && fi.LastPresentTime.QuadPart) {
            d->ctx->CopyResource(d->stage, tex);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(d->ctx->Map(d->stage, 0, D3D11_MAP_READ, 0, &m))) {
                unsigned long long sum = 0;
                for (UINT y = 0; y < d->sh; y++) {
                    memcpy(d->last + y * d->lastPitch, (BYTE *)m.pData + y * m.RowPitch, d->lastPitch);
                    for (UINT x = 0; x < d->lastPitch; x += 64)
                        sum += d->last[y * d->lastPitch + x];
                }
                d->ctx->Unmap(d->stage, 0);
                d->haveFrame = true;
                fwprintf(log, L"dup: copied, sampled byte sum %llu\n", sum);
            }
        }
        if (tex) tex->Release();
        res->Release();
        d->dup->ReleaseFrame();
    } else if (hr != DXGI_ERROR_WAIT_TIMEOUT) {
        fwprintf(log, L"dup: AcquireNextFrame 0x%08lx\n", (unsigned long)hr);
        return false;
    }
    if (!d->haveFrame)
        return false;
    int SW = (int)d->sw, SH = (int)d->sh;    /* un-rotated surface */
    bool swap = d->desc.Rotation == DXGI_MODE_ROTATION_ROTATE90 || d->desc.Rotation == DXGI_MODE_ROTATION_ROTATE270;
    int W = swap ? SH : SW, H = swap ? SW : SH;    /* desktop (upright) size */
    for (int oy = 0; oy < sh; oy++) {
        int dy = (int)((oy + 0.5) * H / sh);
        for (int ox = 0; ox < sw; ox++) {
            int dx = (int)((ox + 0.5) * W / sw), sx, sy;
            switch (d->desc.Rotation) {
            case DXGI_MODE_ROTATION_ROTATE90:  sx = dy;          sy = W - 1 - dx; break;
            case DXGI_MODE_ROTATION_ROTATE180: sx = W - 1 - dx;  sy = H - 1 - dy; break;
            case DXGI_MODE_ROTATION_ROTATE270: sx = H - 1 - dy;  sy = dx;         break;
            default:                           sx = dx;          sy = dy;         break;
            }
            ((UINT *)bits)[oy * sw + ox] = ((UINT *)(d->last + sy * d->lastPitch))[sx] | 0xff000000u;
        }
    }
    return true;
}

int wmain(int argc, wchar_t **argv)
{
    bool useDup = argc > 1 && !_wcsicmp(argv[1], L"-dup");
    if (useDup) { argc--; argv++; }
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
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = sw; bi.bmiHeader.biHeight = -sh;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    BYTE *bits = NULL;
    HBITMAP bmp = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    Dup dd = {};
    fwprintf(log, L"capture %s\n", useDup ? L"dxgi-duplication" : L"gdi");
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
        int ss = CloseScreensaver();
        if (ss) {
            fwprintf(log, L"screensaver was running (close path %d)\n", ss);
            Sleep(300);
        }
        BOOL got = useDup ? DupGrab(&dd, bits, sw, sh, log)
                          : StretchBlt(mem, 0, 0, sw, sh, scr, x, y, w, h, SRCCOPY | CAPTUREBLT);
        GdiFlush();
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
    if (dd.dup) dd.dup->Release();
    if (dd.stage) dd.stage->Release();
    if (dd.out) dd.out->Release();
    if (dd.ctx) dd.ctx->Release();
    if (dd.dev) dd.dev->Release();
    free(dd.last);
    SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, ssWas, NULL, 0);
    SetThreadExecutionState(ES_CONTINUOUS);
    fwprintf(log, L"end frames %d screensaver restored to %d\n", n, ssWas);
    fclose(log);
    return 0;
}
