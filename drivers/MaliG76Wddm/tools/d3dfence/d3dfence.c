/*
 * d3dfence - G15 S4 (notes/gpu-wddm2-design.md): does the Mali adapter give out monitored fences?
 *  1. D3D11 device on the first non-Microsoft hardware adapter; ID3D11Device5 QI; CreateFence; Signal from the
 *     immediate context (ID3D11DeviceContext4) and wait for it.
 *  2. D3DKMT on the same adapter (by LUID): CreateDevice + CreateSynchronizationObject2(D3DDDI_MONITORED_FENCE).
 * Windows Terminal dies in d3d11!WaitTrackedFenceImpl because DXGI's tracked fence (d3d11 CreateFence) is NULL;
 * this separates "the kernel refuses monitored fences" from "the D3D11 runtime does not offer them on this UMD".
 * Exit 0 always (a report). Build: build-d3dfence.cmd.
 * SPDX-License-Identifier: GPL-2.0
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d3dkmthk.h>

typedef NTSTATUS (APIENTRY *PFN_OPENLUID)(D3DKMT_OPENADAPTERFROMLUID *);
typedef NTSTATUS (APIENTRY *PFN_CREATEDEV)(D3DKMT_CREATEDEVICE *);
typedef NTSTATUS (APIENTRY *PFN_SYNC2)(D3DKMT_CREATESYNCHRONIZATIONOBJECT2 *);
typedef NTSTATUS (APIENTRY *PFN_QAI)(D3DKMT_QUERYADAPTERINFO *);

int main(void)
{
    IDXGIFactory1 *fac = NULL;
    IDXGIAdapter1 *ad = NULL, *pick = NULL;
    DXGI_ADAPTER_DESC1 desc, pdesc = {0};
    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&fac);

    printf("CreateDXGIFactory1 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr))
        return 0;
    for (UINT i = 0; IDXGIFactory1_EnumAdapters1(fac, i, &ad) == S_OK; i++) {
        IDXGIAdapter1_GetDesc1(ad, &desc);
        printf("adapter %u: %ls vendor 0x%04x device 0x%04x flags 0x%x luid %08lx:%08lx\n", i, desc.Description,
               desc.VendorId, desc.DeviceId, desc.Flags, (unsigned long)desc.AdapterLuid.HighPart,
               (unsigned long)desc.AdapterLuid.LowPart);
        if (!pick && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.VendorId != 0x1414) {
            pick = ad;
            pdesc = desc;
        } else {
            IDXGIAdapter1_Release(ad);
        }
    }
    if (!pick) {
        printf("no hardware adapter\n");
        return 0;
    }

    D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                D3D_FEATURE_LEVEL_10_0 }, got = 0;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    hr = D3D11CreateDevice((IDXGIAdapter *)pick, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, fls, 4, D3D11_SDK_VERSION, &dev,
                           &got, &ctx);
    printf("D3D11CreateDevice 0x%08lx FL 0x%x\n", (unsigned long)hr, got);
    if (SUCCEEDED(hr)) {
        ID3D11Device5 *d5 = NULL;
        hr = ID3D11Device_QueryInterface(dev, &IID_ID3D11Device5, (void **)&d5);
        printf("QI ID3D11Device5 0x%08lx\n", (unsigned long)hr);
        D3D11_FEATURE_DATA_THREADING th = {0};
        ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_THREADING, &th, sizeof(th));
        printf("threading concurrent %d cmdlists %d\n", th.DriverConcurrentCreates, th.DriverCommandLists);
        if (d5) {
            ID3D11Fence *f = NULL;
            hr = ID3D11Device5_CreateFence(d5, 0, D3D11_FENCE_FLAG_NONE, &IID_ID3D11Fence, (void **)&f);
            printf("CreateFence 0x%08lx fence %p\n", (unsigned long)hr, (void *)f);
            if (f) {
                ID3D11DeviceContext4 *c4 = NULL;
                hr = ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11DeviceContext4, (void **)&c4);
                printf("QI ID3D11DeviceContext4 0x%08lx\n", (unsigned long)hr);
                if (c4) {
                    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
                    hr = ID3D11DeviceContext4_Signal(c4, f, 1);
                    printf("Signal(1) 0x%08lx\n", (unsigned long)hr);
                    ID3D11DeviceContext_Flush(ctx);
                    hr = ID3D11Fence_SetEventOnCompletion(f, 1, ev);
                    DWORD w = WaitForSingleObject(ev, 3000);
                    printf("SetEventOnCompletion 0x%08lx wait %lu completed %llu\n", (unsigned long)hr, w,
                           (unsigned long long)ID3D11Fence_GetCompletedValue(f));
                    ID3D11DeviceContext4_Release(c4);
                }
                ID3D11Fence_Release(f);
            }
            /* DXGI's tracked fence for a DWM-bound swap chain is shared with DWM: try a shared fence too. */
            static const D3D11_FENCE_FLAG sflags[] = { D3D11_FENCE_FLAG_SHARED, D3D11_FENCE_FLAG_SHARED_CROSS_ADAPTER };
            for (int k = 0; k < 2; k++) {
                ID3D11Fence *sf = NULL;
                hr = ID3D11Device5_CreateFence(d5, 0, sflags[k], &IID_ID3D11Fence, (void **)&sf);
                printf("CreateFence(flags 0x%x) 0x%08lx fence %p\n", sflags[k], (unsigned long)hr, (void *)sf);
                if (sf) {
                    HANDLE sh = NULL;
                    hr = ID3D11Fence_CreateSharedHandle(sf, NULL, GENERIC_ALL, NULL, &sh);
                    printf("  CreateSharedHandle 0x%08lx handle %p\n", (unsigned long)hr, sh);
                    if (sh) {
                        ID3D11Fence *of = NULL;
                        hr = ID3D11Device5_OpenSharedFence(d5, sh, &IID_ID3D11Fence, (void **)&of);
                        printf("  OpenSharedFence 0x%08lx fence %p\n", (unsigned long)hr, (void *)of);
                        if (of)
                            ID3D11Fence_Release(of);
                        CloseHandle(sh);
                    }
                    ID3D11Fence_Release(sf);
                }
            }
            D3D11_FEATURE_DATA_D3D11_OPTIONS3 o3 = {0};
            hr = ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_D3D11_OPTIONS3, &o3, sizeof(o3));
            printf("OPTIONS3 0x%08lx VPAndRTArrayIndexFromAnyShaderFeedingRasterizer %d\n", (unsigned long)hr,
                   o3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer);
            D3D11_FEATURE_DATA_D3D11_OPTIONS5 o5 = {0};
            hr = ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_D3D11_OPTIONS5, &o5, sizeof(o5));
            printf("OPTIONS5 0x%08lx SharedResourceTier %d\n", (unsigned long)hr, o5.SharedResourceTier);
            ID3D11Device5_Release(d5);
        }
    }

    HMODULE g = LoadLibraryW(L"gdi32.dll");
    PFN_OPENLUID openluid = (PFN_OPENLUID)GetProcAddress(g, "D3DKMTOpenAdapterFromLuid");
    PFN_CREATEDEV createdev = (PFN_CREATEDEV)GetProcAddress(g, "D3DKMTCreateDevice");
    PFN_SYNC2 sync2 = (PFN_SYNC2)GetProcAddress(g, "D3DKMTCreateSynchronizationObject2");
    PFN_QAI qai = (PFN_QAI)GetProcAddress(g, "D3DKMTQueryAdapterInfo");
    if (openluid && createdev && sync2) {
        D3DKMT_OPENADAPTERFROMLUID ol = {0};
        ol.AdapterLuid = pdesc.AdapterLuid;
        NTSTATUS st = openluid(&ol);
        printf("D3DKMTOpenAdapterFromLuid 0x%08lx\n", (unsigned long)st);
        if (st >= 0 && qai) {
            D3DKMT_WDDM_2_0_CAPS c20 = {0};
            D3DKMT_QUERYADAPTERINFO q = {0};
            q.hAdapter = ol.hAdapter;
            q.Type = KMTQAITYPE_WDDM_2_0_CAPS;
            q.pPrivateDriverData = &c20;
            q.PrivateDriverDataSize = sizeof(c20);
            st = qai(&q);
            printf("QAI WDDM_2_0_CAPS 0x%08lx value 0x%x\n", (unsigned long)st, c20.Value);
            D3DKMT_DRIVERVERSION dv = 0;
            q.Type = KMTQAITYPE_DRIVERVERSION;
            q.pPrivateDriverData = &dv;
            q.PrivateDriverDataSize = sizeof(dv);
            st = qai(&q);
            printf("QAI DRIVERVERSION 0x%08lx value 0x%x\n", (unsigned long)st, (unsigned)dv);
        }
        if (st >= 0) {
            D3DKMT_CREATEDEVICE cd = {0};
            cd.hAdapter = ol.hAdapter;
            st = createdev(&cd);
            printf("D3DKMTCreateDevice 0x%08lx\n", (unsigned long)st);
            if (st >= 0) {
                D3DKMT_CREATESYNCHRONIZATIONOBJECT2 so = {0};
                so.hDevice = cd.hDevice;
                so.Info.Type = D3DDDI_MONITORED_FENCE;
                so.Info.MonitoredFence.InitialFenceValue = 0;
                st = sync2(&so);
                printf("CreateSynchronizationObject2(MONITORED_FENCE) 0x%08lx handle 0x%x cpuva %p gpuva 0x%llx\n",
                       (unsigned long)st, so.hSyncObject, so.Info.MonitoredFence.FenceValueCPUVirtualAddress,
                       (unsigned long long)so.Info.MonitoredFence.FenceValueGPUVirtualAddress);
            }
        }
    }
    return 0;
}
