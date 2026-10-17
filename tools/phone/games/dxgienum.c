/*
 * dxgienum.exe - list DXGI adapters and their outputs (desktop coordinates, attached, rotation) as this process sees
 * them. Games bring-up (notes/games-nfsu2.md): dgVoodoo reported "No any output device is found" on our adapter.
 * Built for ARM64 and x86 (a 32-bit game sees DXGI through WOW64). Documented API only.
 * SPDX-License-Identifier: GPL-2.0
 */
#define COBJMACROS
#include <windows.h>
#include <dxgi.h>
#include <stdio.h>

int main(void)
{
    IDXGIFactory1 *f = NULL;
    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f);
    printf("CreateDXGIFactory1 hr 0x%08lx (%u-bit process)\n", (unsigned long)hr, (unsigned)(sizeof(void *) * 8));
    if (FAILED(hr))
        return 1;
    IDXGIAdapter1 *a;
    for (UINT i = 0; IDXGIFactory1_EnumAdapters1(f, i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        IDXGIAdapter1_GetDesc1(a, &d);
        printf("adapter %u: %ls vendor 0x%04x device 0x%04x flags 0x%x dedicated %llu MB shared %llu MB luid %08lx:%08lx\n",
               i, d.Description, d.VendorId, d.DeviceId, d.Flags, (unsigned long long)(d.DedicatedVideoMemory >> 20),
               (unsigned long long)(d.SharedSystemMemory >> 20), (unsigned long)d.AdapterLuid.HighPart,
               (unsigned long)d.AdapterLuid.LowPart);
        IDXGIOutput *o;
        UINT n = 0;
        for (UINT j = 0; (hr = IDXGIAdapter1_EnumOutputs(a, j, &o)) == S_OK; j++, n++) {
            DXGI_OUTPUT_DESC od;
            IDXGIOutput_GetDesc(o, &od);
            printf("  output %u: %ls attached %d rect %ld,%ld-%ld,%ld rotation %d\n", j, od.DeviceName,
                   od.AttachedToDesktop, od.DesktopCoordinates.left, od.DesktopCoordinates.top,
                   od.DesktopCoordinates.right, od.DesktopCoordinates.bottom, (int)od.Rotation);
            static const struct { DXGI_FORMAT f; const char *n; } fmts[] = {
                { DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM" }, { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "R8G8B8A8_UNORM_SRGB" },
                { DXGI_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM" }, { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, "B8G8R8A8_UNORM_SRGB" },
                { DXGI_FORMAT_B8G8R8X8_UNORM, "B8G8R8X8_UNORM" }, { DXGI_FORMAT_R10G10B10A2_UNORM, "R10G10B10A2_UNORM" },
                { DXGI_FORMAT_B5G6R5_UNORM, "B5G6R5_UNORM" },
            };
            for (UINT k = 0; k < sizeof(fmts) / sizeof(fmts[0]); k++) {
                UINT modes = 0;
                HRESULT mh = IDXGIOutput_GetDisplayModeList(o, fmts[k].f, DXGI_ENUM_MODES_SCALING, &modes, NULL);
                printf("    GetDisplayModeList(%s) hr 0x%08lx modes %u", fmts[k].n, (unsigned long)mh, modes);
                if (SUCCEEDED(mh) && modes) {
                    DXGI_MODE_DESC md[16];
                    UINT m = modes < 16 ? modes : 16;
                    if (SUCCEEDED(IDXGIOutput_GetDisplayModeList(o, fmts[k].f, DXGI_ENUM_MODES_SCALING, &m, md)))
                        for (UINT q = 0; q < m; q++)
                            printf(" | %ux%u %u/%u sc%d", md[q].Width, md[q].Height, md[q].RefreshRate.Numerator,
                                   md[q].RefreshRate.Denominator, (int)md[q].Scaling);
                }
                printf("\n");
            }
            IDXGIOutput_Release(o);
        }
        printf("  %u output(s), last EnumOutputs hr 0x%08lx\n", n, (unsigned long)hr);
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
    return 0;
}
