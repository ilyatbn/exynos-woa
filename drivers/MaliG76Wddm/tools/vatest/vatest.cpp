/*
 * vatest - G-game (notes/g17-dxva-mfc.md): what costs a process its address space on our stack. A 32-bit game filled
 * its 4 GB with ~110 private 32 MB reservations (vaprobe), roughly one per GPU allocation. This counts the private
 * reserved-but-uncommitted regions of exactly 32 MB in its own address space while it creates N textures in three
 * ways: DEFAULT without data (GPU only), DEFAULT with initial data (an upload), DYNAMIC mapped once from the CPU.
 * A behavioural test instead of reading Windows internals (AGENTS rule 11).
 *
 *   vatest [N=40] [side=256]   (textures side x side RGBA8)
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <d3d11.h>
#include <dxgi1_2.h>

static void Count(const char *what)
{
    MEMORY_BASIC_INFORMATION m;
    ULONG_PTR p = 0;
    unsigned n32 = 0;
    SIZE_T resv = 0, comm = 0;
    ULONG_PTR lastBase = (ULONG_PTR)-1;
    SIZE_T groupSize = 0, groupComm = 0;
    for (;;) {
        if (VirtualQuery((LPCVOID)p, &m, sizeof(m)) != sizeof(m))
            break;
        if (m.State != MEM_FREE && m.Type == MEM_PRIVATE) {
            ULONG_PTR ab = (ULONG_PTR)m.AllocationBase;
            if (ab != lastBase) {
                if (groupSize == (32u << 20) && groupComm < (1u << 20))
                    n32++;
                lastBase = ab;
                groupSize = groupComm = 0;
            }
            groupSize += m.RegionSize;
            if (m.State == MEM_COMMIT) {
                groupComm += m.RegionSize;
                comm += m.RegionSize;
            } else {
                resv += m.RegionSize;
            }
        }
        ULONG_PTR next = (ULONG_PTR)m.BaseAddress + m.RegionSize;
        if (next <= p)
            break;
        p = next;
    }
    if (groupSize == (32u << 20) && groupComm < (1u << 20))
        n32++;
    printf("%-34s 32 MB empty reservations %4u, private reserved-only %8.1f MB, committed %7.1f MB\n", what, n32,
           resv / 1048576.0, comm / 1048576.0);
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 40;
    UINT side = argc > 2 ? (UINT)atoi(argv[2]) : 256;
    IDXGIFactory1 *fac = NULL;
    IDXGIAdapter1 *ad = NULL, *pick = NULL;
    DXGI_ADAPTER_DESC1 d;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac)))
        return 1;
    for (UINT i = 0; fac->EnumAdapters1(i, &ad) == S_OK; i++) {
        ad->GetDesc1(&d);
        if (!pick && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId != 0x1414)
            pick = ad;
        else
            ad->Release();
    }
    if (!pick)
        return 1;
    pick->GetDesc1(&d);
    printf("adapter: %ls (%u-bit process)\n", d.Description, (unsigned)(sizeof(void *) * 8));
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    Count("before device");
    if (FAILED(D3D11CreateDevice(pick, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, NULL, &ctx)))
        return 1;
    Count("device created");
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = side; td.Height = side; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D **t = (ID3D11Texture2D **)calloc(3 * n, sizeof(*t));
    for (int i = 0; i < n; i++)
        dev->CreateTexture2D(&td, NULL, &t[i]);
    ctx->Flush();
    char buf[64];
    snprintf(buf, sizeof buf, "+%d DEFAULT, no data", n);
    Count(buf);
    unsigned char *px = (unsigned char *)calloc((size_t)side * side, 4);
    D3D11_SUBRESOURCE_DATA sd = { px, side * 4, 0 };
    for (int i = 0; i < n; i++)
        dev->CreateTexture2D(&td, &sd, &t[n + i]);
    ctx->Flush();
    snprintf(buf, sizeof buf, "+%d DEFAULT with data", n);
    Count(buf);
    td.Usage = D3D11_USAGE_DYNAMIC; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    for (int i = 0; i < n; i++) {
        if (SUCCEEDED(dev->CreateTexture2D(&td, NULL, &t[2 * n + i]))) {
            D3D11_MAPPED_SUBRESOURCE ms;
            if (SUCCEEDED(ctx->Map(t[2 * n + i], 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
                ctx->Unmap(t[2 * n + i], 0);
        }
    }
    ctx->Flush();
    snprintf(buf, sizeof buf, "+%d DYNAMIC mapped", n);
    Count(buf);
    for (int i = 0; i < 3 * n; i++)
        if (t[i])
            t[i]->Release();
    ctx->Flush();
    Count("all released");
    return 0;
}
