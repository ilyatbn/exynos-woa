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
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#pragma warning(push)
#pragma warning(disable : 4201)     /* nameless unions in the SDK headers (C mode) */
#include <dxgi.h>
#include <d3d11.h>
#include <d3d10.h>
#include <d3dkmthk.h>
#pragma warning(pop)

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
    close.hAdapter = open.hAdapter;
    D3DKMTCloseAdapter(&close);
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
    printf("d3dprobe: CreateDXGIFactory1 hr 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr))
        return 0;
    for (UINT i = 0; IDXGIFactory1_EnumAdapters1(f, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        IDXGIOutput *o = NULL;
        UINT outputs = 0;

        if (FAILED(IDXGIAdapter1_GetDesc1(a, &d))) {
            IDXGIAdapter1_Release(a);
            continue;
        }
        while (IDXGIAdapter1_EnumOutputs(a, outputs, &o) != DXGI_ERROR_NOT_FOUND) {
            IDXGIOutput_Release(o);
            outputs++;
        }
        printf("adapter %u: %ls ven %04x dev %04x flags 0x%x dedicated %llu MB shared %llu MB outputs %u luid %08lx:%08lx\n",
               i, d.Description, d.VendorId, d.DeviceId, d.Flags,
               (unsigned long long)d.DedicatedVideoMemory >> 20, (unsigned long long)d.SharedSystemMemory >> 20,
               outputs, (unsigned long)d.AdapterLuid.HighPart, d.AdapterLuid.LowPart);
        KmtInfo(d.AdapterLuid);
        TryD3D11((IDXGIAdapter *)a, 0, "plain");
        TryD3D11((IDXGIAdapter *)a, D3D11_CREATE_DEVICE_BGRA_SUPPORT, "bgra");
        TryD3D10((IDXGIAdapter *)a);
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
    printf("d3dprobe: done\n");
    return 0;
}
