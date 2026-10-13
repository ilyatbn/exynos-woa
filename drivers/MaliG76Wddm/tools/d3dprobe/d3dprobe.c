// SPDX-License-Identifier: GPL-2.0
/*
 * d3dprobe.exe - what the Direct3D runtimes make of each adapter (M16 G13, notes/gpu-full-adapter-hang.md): DWM needs
 * a D3D11 device on the display adapter, and the full MaliG76Wddm adapter's UMD (maliumd.dll = Mesa d3d10umd) is
 * built with SUPPORT_D3D10_1 0 / SUPPORT_D3D11 0 (mesa src/gallium/frontends/d3d10umd/State.h:39-40). For every
 * DXGI adapter this prints: the description; the UMD file the kernel names for DX9/DX10/DX11 (D3DKMT
 * KMTQAITYPE_UMDRIVERNAME); the WDDM version (KMTQAITYPE_DRIVERVERSION); and the HRESULT + feature level of
 * D3D11CreateDevice (with and without BGRA support, as DWM asks) and D3D10CreateDevice on it. Run with
 * MALIKM_TRACE=1 to get the Mesa shim's own trace on stderr (it runs inside this process).
 * Exit 0 always (it is a report). Build: build-d3dprobe.cmd.
 * Robustness (run g15fullb stopped after CreateDXGIFactory1 - the output loop below waited for NOT_FOUND only, so
 * any other EnumOutputs error spun forever): every step prints before it runs, every runtime call is inside SEH,
 * and a watchdog thread ends the process after 90 s with a "TIMEOUT in <step>" line.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#pragma warning(push)
#pragma warning(disable : 4201)     /* nameless unions in the SDK headers (C mode) */
#include <dxgi1_2.h>
#include <d3d11.h>
#include <d3d10.h>
#include <d3dkmthk.h>
#pragma warning(pop)

static volatile const char *Step = "start";

static void Mark(const char *S)
{
    Step = S;
    printf("  .. %s\n", S);
    fflush(stdout);
}

static DWORD WINAPI Watchdog(LPVOID P)
{
    UNREFERENCED_PARAMETER(P);
    Sleep(90000);
    printf("d3dprobe: TIMEOUT in %s\n", (const char *)Step);
    fflush(stdout);
    ExitProcess(3);
}

static const char *FlName(D3D_FEATURE_LEVEL F)
{
    switch (F) {
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    case D3D_FEATURE_LEVEL_10_1: return "10_1";
    case D3D_FEATURE_LEVEL_10_0: return "10_0";
    case D3D_FEATURE_LEVEL_9_3: return "9_3";
    case D3D_FEATURE_LEVEL_9_2: return "9_2";
    case D3D_FEATURE_LEVEL_9_1: return "9_1";
    default: return "?";
    }
}

static void KmtInfo(LUID Luid)
{
    D3DKMT_OPENADAPTERFROMLUID open = {0};
    D3DKMT_CLOSEADAPTER close = {0};
    static const struct { KMTUMDVERSION V; const char *N; } v[] = {
        { KMTUMDVERSION_DX9, "DX9" }, { KMTUMDVERSION_DX10, "DX10" }, { KMTUMDVERSION_DX11, "DX11" },
    };
    NTSTATUS st;

    open.AdapterLuid = Luid;
    st = D3DKMTOpenAdapterFromLuid(&open);
    if (st != 0) {
        printf("  D3DKMTOpenAdapterFromLuid 0x%08lx\n", (unsigned long)st);
        return;
    }
    for (int i = 0; i < 3; i++) {
        D3DKMT_UMDFILENAMEINFO u = {0};
        D3DKMT_QUERYADAPTERINFO q = {0};
        u.Version = v[i].V;
        q.hAdapter = open.hAdapter;
        q.Type = KMTQAITYPE_UMDRIVERNAME;
        q.pPrivateDriverData = &u;
        q.PrivateDriverDataSize = sizeof(u);
        st = D3DKMTQueryAdapterInfo(&q);
        printf("  UMD %-4s 0x%08lx %ls\n", v[i].N, (unsigned long)st, st == 0 ? u.UmdFileName : L"");
    }
    {
        D3DKMT_DRIVERVERSION dv = 0;
        D3DKMT_QUERYADAPTERINFO q = {0};
        q.hAdapter = open.hAdapter;
        q.Type = KMTQAITYPE_DRIVERVERSION;
        q.pPrivateDriverData = &dv;
        q.PrivateDriverDataSize = sizeof(dv);
        st = D3DKMTQueryAdapterInfo(&q);
        printf("  WDDM driver version 0x%08lx status 0x%08lx\n", (unsigned long)dv, (unsigned long)st);
    }
    {
        D3DKMT_ADAPTERTYPE t = {0};
        D3DKMT_QUERYADAPTERINFO q = {0};
        q.hAdapter = open.hAdapter;
        q.Type = KMTQAITYPE_ADAPTERTYPE;
        q.pPrivateDriverData = &t;
        q.PrivateDriverDataSize = sizeof(t);
        st = D3DKMTQueryAdapterInfo(&q);
        printf("  adapter type 0x%08x status 0x%08lx: render %u display %u software %u post %u\n", t.Value,
               (unsigned long)st, t.RenderSupported, t.DisplaySupported, t.SoftwareDevice, t.PostDevice);
    }
    close.hAdapter = open.hAdapter;
    D3DKMTCloseAdapter(&close);
}

/* g47 (notes/gpu-full-adapter-hang.md): LogonUI's XAML fail-fasts on E_INVALIDARG from DComp surface creation.
 * DComp surfaces need resource flags the docs list as "not supported until Direct3D 11.1" (SHARED_NTHANDLE, GUARDED)
 * and our UMD has only the D3D10/10.1 DDI. Create a 256x256 B8G8R8A8 render-target/shader texture with each flag
 * set and print the HRESULT (the Basic Render Driver adapter is the control), plus the device's D3D11.1 options. */
static void TryTextures(ID3D11Device *Dev)
{
    static const struct { UINT Misc; const char *N; } t[] = {
        { 0, "none" },
        { D3D11_RESOURCE_MISC_SHARED, "SHARED" },
        { D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, "KEYEDMUTEX" },
        { D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE, "SHARED|NTHANDLE" },
        { D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE, "KEYEDMUTEX|NTHANDLE" },
        { D3D11_RESOURCE_MISC_GUARDED, "GUARDED" },
        { D3D11_RESOURCE_MISC_GDI_COMPATIBLE, "GDI_COMPATIBLE" },
        { D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_DISPLAYABLE,
          "SHARED|NT|DISPLAYABLE" },
    };
    D3D11_FEATURE_DATA_D3D11_OPTIONS o;
    D3D11_FEATURE_DATA_THREADING th;

    if (SUCCEEDED(ID3D11Device_CheckFeatureSupport(Dev, D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof(o))))
        printf("    options: ExtendedResourceSharing %d OutputMergerLogicOp %d ClearView %d CopyWithOverlap %d\n",
               o.ExtendedResourceSharing, o.OutputMergerLogicOp, o.ClearView, o.CopyWithOverlap);
    else
        printf("    options: D3D11_OPTIONS query failed\n");
    if (SUCCEEDED(ID3D11Device_CheckFeatureSupport(Dev, D3D11_FEATURE_THREADING, &th, sizeof(th))))
        printf("    threading: concurrent creates %d command lists %d\n", th.DriverConcurrentCreates, th.DriverCommandLists);
    for (UINT i = 0; i < ARRAYSIZE(t); i++) {
        D3D11_TEXTURE2D_DESC d = { 0 };
        ID3D11Texture2D *tex = NULL;
        HRESULT hr;

        d.Width = 256;
        d.Height = 256;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = t[i].Misc;
        Mark(t[i].N);
        hr = ID3D11Device_CreateTexture2D(Dev, &d, NULL, &tex);
        printf("    texture BGRA 256 %-20s hr 0x%08lx\n", t[i].N, (unsigned long)hr);
        /* g50: composition buffers come back with an NT shared handle, and the UMD backs shared resources with no
         * kernel allocation - can the runtime make a shared handle for our shared textures at all? */
        if (tex != NULL && (t[i].Misc & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
            IDXGIResource1 *r1 = NULL;
            HANDLE h = NULL;
            HRESULT hq = ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource1, (void **)&r1);
            if (SUCCEEDED(hq)) {
                Mark("CreateSharedHandle");
                hq = IDXGIResource1_CreateSharedHandle(r1, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                       NULL, &h);
                IDXGIResource1_Release(r1);
            }
            printf("      CreateSharedHandle hr 0x%08lx handle %p\n", (unsigned long)hq, h);
            if (h != NULL)
                CloseHandle(h);
        } else if (tex != NULL && (t[i].Misc & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX))) {
            IDXGIResource *r = NULL;
            HANDLE h = NULL;
            HRESULT hq = ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource, (void **)&r);
            if (SUCCEEDED(hq)) {
                Mark("GetSharedHandle");
                hq = IDXGIResource_GetSharedHandle(r, &h);
                IDXGIResource_Release(r);
            }
            printf("      GetSharedHandle hr 0x%08lx handle %p\n", (unsigned long)hq, h);
        }
        if (tex != NULL)
            ID3D11Texture2D_Release(tex);
    }
}

/* g49: dcomp!TokenFactory::CreateCompositionBuffer QIs the D3D11 device for this undocumented interface and calls
 * slot 12 = d3d11!CDevice::CreateCompositionBuffer(UINT w, UINT h, DXGI_FORMAT, int, UINT, UINT, REFIID, void **,
 * void **) (signature from d3d11.pdb); in LogonUI it returned E_INVALIDARG. Called here with dcomp's arguments
 * (CAtlasSurfacePool::CreateDeviceResources: int 0, flags 0x10 for R8/R8G8 else 0x30, last UINT 1 for B8G8R8A8/X8). */
static const GUID IID_CompositionBufferFactory =
    { 0x26c5dc23, 0xe49c, 0x4b0a, { 0x8f, 0x79, 0xe7, 0xb1, 0xac, 0x80, 0x4d, 0x32 } };
typedef HRESULT (STDMETHODCALLTYPE *PFN_CREATECOMPOSITIONBUFFER)(void *This, UINT W, UINT H, DXGI_FORMAT F, int I,
                                                                 UINT Flags, UINT Bgra, REFIID Riid, void **Out1,
                                                                 void **Out2);

static void TryCompositionBuffers(ID3D11Device *Dev)
{
    static const struct { DXGI_FORMAT F; UINT Flags; UINT Bgra; const char *N; } t[] = {
        { DXGI_FORMAT_B8G8R8A8_UNORM, 0x30, 1, "B8G8R8A8" },
        { DXGI_FORMAT_R8G8B8A8_UNORM, 0x30, 0, "R8G8B8A8" },
        { DXGI_FORMAT_R8_UNORM, 0x10, 0, "R8" },
        { DXGI_FORMAT_R8G8_UNORM, 0x10, 0, "R8G8" },
        { DXGI_FORMAT_R16G16B16A16_FLOAT, 0x30, 0, "R16G16B16A16F" },
    };
    D3D11_FEATURE_DATA_DISPLAYABLE disp = { 0 };
    void **obj = NULL;
    HRESULT hr;

    hr = ID3D11Device_CheckFeatureSupport(Dev, D3D11_FEATURE_DISPLAYABLE, &disp, sizeof(disp));
    printf("    displayable: hr 0x%08lx DisplayableTexture %d SharedResourceTier %d\n", (unsigned long)hr,
           disp.DisplayableTexture, (int)disp.SharedResourceTier);
    hr = ID3D11Device_QueryInterface(Dev, &IID_CompositionBufferFactory, (void **)&obj);
    printf("    QI {26c5dc23} hr 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr) || obj == NULL)
        return;
    for (UINT i = 0; i < ARRAYSIZE(t); i++) {
        PFN_CREATECOMPOSITIONBUFFER Create = (PFN_CREATECOMPOSITIONBUFFER)((void **)*obj)[12];
        IUnknown *buf = NULL;
        void *out2 = NULL;

        Mark(t[i].N);
        __try {
            hr = Create(obj, 256, 256, t[i].F, 0, t[i].Flags, t[i].Bgra, &IID_IUnknown, (void **)&buf, &out2);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            hr = (HRESULT)GetExceptionCode();
        }
        printf("    composition buffer 256 %-14s flags 0x%02x hr 0x%08lx out1 %p out2 %p\n", t[i].N, t[i].Flags,
               (unsigned long)hr, (void *)buf, out2);
        /* Neither output is released: releasing out1 as an IUnknown faulted on the PC (x64 AMD/WARP), so the
         * outputs' types are not what the riid suggests. The probe exits soon after; the leak is harmless. */
    }
    ((IUnknown *)obj)->lpVtbl->Release((IUnknown *)obj);
}

static void TryD3D11(IDXGIAdapter *A, UINT Flags, const char *What)
{
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_1,
    };
    ID3D11Device *dev = NULL;
    D3D_FEATURE_LEVEL got = 0;
    HRESULT hr;

    fflush(stdout);
    hr = D3D11CreateDevice(A, D3D_DRIVER_TYPE_UNKNOWN, NULL, Flags, levels, ARRAYSIZE(levels),
                           D3D11_SDK_VERSION, &dev, &got, NULL);
    if (hr == E_INVALIDARG)   /* runtimes without 11_1 refuse the whole list */
        hr = D3D11CreateDevice(A, D3D_DRIVER_TYPE_UNKNOWN, NULL, Flags, levels + 1, ARRAYSIZE(levels) - 1,
                               D3D11_SDK_VERSION, &dev, &got, NULL);
    printf("  D3D11CreateDevice %-6s hr 0x%08lx level %s\n", What, (unsigned long)hr, SUCCEEDED(hr) ? FlName(got) : "-");
    if (dev != NULL && (Flags & D3D11_CREATE_DEVICE_BGRA_SUPPORT)) {
        TryTextures(dev);
        TryCompositionBuffers(dev);
    }
    if (dev != NULL)
        ID3D11Device_Release(dev);
}

static void TryD3D10(IDXGIAdapter *A)
{
    ID3D10Device *dev = NULL;
    HRESULT hr;

    fflush(stdout);
    hr = D3D10CreateDevice(A, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_SDK_VERSION, &dev);
    printf("  D3D10CreateDevice        hr 0x%08lx\n", (unsigned long)hr);
    if (dev != NULL)
        ID3D10Device_Release(dev);
}

int main(void)
{
    IDXGIFactory1 *f = NULL;
    IDXGIAdapter1 *a = NULL;
    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f);

    setvbuf(stdout, NULL, _IONBF, 0);
    CloseHandle(CreateThread(NULL, 0, Watchdog, NULL, 0, NULL));
    printf("d3dprobe: CreateDXGIFactory1 hr 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr))
        return 0;
    for (UINT i = 0; IDXGIFactory1_EnumAdapters1(f, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        IDXGIOutput *o = NULL;
        UINT outputs = 0;

        Mark("GetDesc1");
        if (FAILED(IDXGIAdapter1_GetDesc1(a, &d))) {
            IDXGIAdapter1_Release(a);
            continue;
        }
        Mark("EnumOutputs");
        for (;;) {
            HRESULT ho = IDXGIAdapter1_EnumOutputs(a, outputs, &o);
            if (FAILED(ho)) {
                if (ho != DXGI_ERROR_NOT_FOUND)
                    printf("  EnumOutputs(%u) hr 0x%08lx\n", outputs, (unsigned long)ho);
                break;
            }
            {
                DXGI_OUTPUT_DESC od;
                if (SUCCEEDED(IDXGIOutput_GetDesc(o, &od)))
                    printf("  output %u: %ls attached %d rect %ld,%ld-%ld,%ld rotation %d\n", outputs, od.DeviceName,
                           od.AttachedToDesktop, od.DesktopCoordinates.left, od.DesktopCoordinates.top,
                           od.DesktopCoordinates.right, od.DesktopCoordinates.bottom, (int)od.Rotation);
            }
            IDXGIOutput_Release(o);
            if (++outputs >= 16)
                break;
        }
        printf("adapter %u: %ls ven %04x dev %04x flags 0x%x dedicated %llu MB shared %llu MB outputs %u luid %08lx:%08lx\n",
               i, d.Description, d.VendorId, d.DeviceId, d.Flags,
               (unsigned long long)d.DedicatedVideoMemory >> 20, (unsigned long long)d.SharedSystemMemory >> 20,
               outputs, (unsigned long)d.AdapterLuid.HighPart, d.AdapterLuid.LowPart);
        __try {
            Mark("D3DKMT");
            KmtInfo(d.AdapterLuid);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            printf("  EXCEPTION 0x%08lx in D3DKMT\n", (unsigned long)GetExceptionCode());
        }
        __try {
            Mark("D3D11CreateDevice plain");
            TryD3D11((IDXGIAdapter *)a, 0, "plain");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            printf("  EXCEPTION 0x%08lx in D3D11 plain\n", (unsigned long)GetExceptionCode());
        }
        __try {
            Mark("D3D11CreateDevice bgra");
            TryD3D11((IDXGIAdapter *)a, D3D11_CREATE_DEVICE_BGRA_SUPPORT, "bgra");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            printf("  EXCEPTION 0x%08lx in D3D11 bgra\n", (unsigned long)GetExceptionCode());
        }
        __try {
            Mark("D3D10CreateDevice");
            TryD3D10((IDXGIAdapter *)a);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            printf("  EXCEPTION 0x%08lx in D3D10\n", (unsigned long)GetExceptionCode());
        }
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
    printf("d3dprobe: done\n");
    return 0;
}
