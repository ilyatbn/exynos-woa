// SPDX-License-Identifier: GPL-2.0
/*
 * m5test: M5.2 step (b)'s acceptance test. Runs M1's WRITE_VALUE job on the
 * Mali through maliwddm using only the D3DKMT thunks (what a user-mode
 * driver's runtime calls end up as): a device, a context on node 0, one
 * allocation holding the job and its target, a render with one command, and
 * a lock that waits for the GPU before reading the value back.
 *
 * Exit: 0 the GPU wrote the value; 1 no maliwddm adapter; 2 a step failed.
 *
 * The thunks are resolved at run time from win32u (NtGdiDdDDI*, the system
 * calls themselves), falling back to gdi32 (D3DKMT*): WinPE's gdi32 forwards
 * every D3DKMT export to ext-ms-win-dx-d3dkmt-dxcore, and WinPE has no
 * dxcore, so a static gdi32 import never even loaded there
 * (STATUS_DLL_NOT_FOUND in every PE run).
 *
 * Build: cl /W4 /I include tools\windows\m5test.c
 *
 * MaliG76Wddm (Galaxy S10e, Mali-G76, M16 G11): derived from wddm-mali-g52 tools/windows/m5test.c (commit
 * 16f7e7c6, GPL-2.0, Copyright gahingwoo). G76: the adapter must report a G76 GPU_ID ((id >> 16) & 0xFF0F ==
 * 0x7201; GPU_INFO Value[0] = id >> 16) with its GPU half up (Valid != 0); the job still runs on JS1.
 * Build here: build-m5test.cmd (ARM64). Exit codes unchanged; 3 = D3DKMT thunks not found.
 */
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <stdio.h>
#include <string.h>
#include "maliwddm_abi.h"

typedef NTSTATUS (APIENTRY *KMT_FN)(void *);
static KMT_FN pCreateDevice, pDestroyDevice, pCreateContext, pDestroyContext, pCreateAllocation,
              pDestroyAllocation, pLock, pUnlock, pRender, pEscape, pEnumAdapters2;

static KMT_FN resolve(const char *name)
{
    char nt[96];
    HMODULE w = LoadLibraryA("win32u.dll");
    HMODULE g = LoadLibraryA("gdi32.dll");
    FARPROC f = NULL;

    sprintf_s(nt, sizeof(nt), "NtGdiDdDDI%s", name);
    if (w != NULL)
        f = GetProcAddress(w, nt);
    if (f == NULL && g != NULL) {
        sprintf_s(nt, sizeof(nt), "D3DKMT%s", name);
        f = GetProcAddress(g, nt);
    }
    if (f == NULL)
        printf("cannot resolve %s\n", name);
    return (KMT_FN)(void *)f;
}

static int resolve_all(void)
{
    pCreateDevice = resolve("CreateDevice");
    pDestroyDevice = resolve("DestroyDevice");
    pCreateContext = resolve("CreateContext");
    pDestroyContext = resolve("DestroyContext");
    pCreateAllocation = resolve("CreateAllocation");
    pDestroyAllocation = resolve("DestroyAllocation");
    pLock = resolve("Lock");
    pUnlock = resolve("Unlock");
    pRender = resolve("Render");
    pEscape = resolve("Escape");
    pEnumAdapters2 = resolve("EnumAdapters2");
    return pCreateDevice && pDestroyDevice && pCreateContext && pDestroyContext && pCreateAllocation &&
           pDestroyAllocation && pLock && pUnlock && pRender && pEscape && pEnumAdapters2;
}

#define JOB_BYTES       4096
#define TARGET_OFFSET   1024
#define MAGIC           0xC0FFEE52u

static D3DKMT_HANDLE g_adapter;
static void print_stats(D3DKMT_HANDLE adapter, const char *when);

static int fail(const char *what, NTSTATUS st)
{
    printf("FAIL: %s: 0x%08lx\n", what, (unsigned long)st);
    if (g_adapter != 0)
        print_stats(g_adapter, "at the failure");
    return 2;
}

/* M1's job: a WRITE_VALUE header at +0, its payload at +32. */
static void write_job(volatile UINT32 *job, UINT64 target, UINT32 value)
{
    memset((void *)job, 0, 64);
    job[4] = 1u | (2u << 1) | (1u << 16);   /* is_64b, WRITE_VALUE, index 1 */
    job[8] = (UINT32)target;
    job[9] = (UINT32)(target >> 32);
    job[10] = 6;                             /* immediate 32 */
    job[12] = value;
}

static NTSTATUS escape(D3DKMT_HANDLE adapter, D3DKMT_HANDLE device, void *data, UINT size)
{
    D3DKMT_ESCAPE e = {0};
    e.hAdapter = adapter;
    e.hDevice = device;
    e.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    e.pPrivateDriverData = data;
    e.PrivateDriverDataSize = size;
    return pEscape(&e);
}

/* The adapter that answers our GPU-info escape with a G76's GPU_ID and its GPU half up (G76). */
static D3DKMT_HANDLE find_adapter(void)
{
    D3DKMT_ENUMADAPTERS2 en = {0};
    D3DKMT_ADAPTERINFO info[16];

    en.NumAdapters = 16;
    en.pAdapters = info;
    if (pEnumAdapters2(&en) < 0)
        return 0;
    for (ULONG i = 0; i < en.NumAdapters; i++) {
        MW_ESCAPE_GPU_INFO_DATA g = {0};
        g.Code = MW_ESCAPE_GPU_INFO;
        NTSTATUS st = escape(info[i].hAdapter, 0, &g, sizeof(g));
        printf("adapter %lu (luid %08lx:%08lx): escape 0x%08lx, %u params, GPU_ID 0x%llx\n",
               i, info[i].AdapterLuid.HighPart, info[i].AdapterLuid.LowPart, (unsigned long)st,
               g.Count, (unsigned long long)g.Value[0]);
        if (st >= 0 && g.Count != 0 && g.Valid != 0 && (g.Value[0] & 0xFF0F) == 0x7201)
            return info[i].hAdapter;
        if (st >= 0 && g.Count != 0)
            printf("  answers GPU_INFO, but valid 0x%llx id 0x%llx: GPU half not up, or not a G76\n",
                   (unsigned long long)g.Valid, (unsigned long long)g.Value[0]);
    }
    return 0;
}

static void print_stats(D3DKMT_HANDLE adapter, const char *when)
{
    MW_ESCAPE_STATS_DATA s = {0};
    s.Code = MW_ESCAPE_STATS;
    NTSTATUS st = escape(adapter, 0, &s, sizeof(s));
    if (st < 0) {
        printf("stats %s: escape 0x%08lx\n", when, (unsigned long)st);
        return;
    }
    printf("stats %s: gpu %u renders %u presents %u submits %u fence %u/%u queued %u running %u\n"
           "  jobs done %u failed %u timedout %u js 0x%x fault 0x%x @0x%llx irqs %u resets %u\n",
           when, s.GpuUp, s.Renders, s.Presents, s.Submits, s.LastCompletedFence, s.LastSubmittedFence,
           s.Queued, s.Running, s.JobsDone, s.JobsFailed, s.JobsTimedOut, s.LastJsStatus,
           s.LastFaultStatus, (unsigned long long)s.LastFaultAddress, s.Irqs, s.Resets);
    printf("  allocations %u (last at 0x%llx); aperture maps %u (fails %u, last 0x%llx x%u pages, 0x%08x), unmaps %u\n"
           "  paging ops by type:", s.Allocations, (unsigned long long)s.LastAllocVa, s.Maps, s.MapFails,
           (unsigned long long)s.LastMapVa, s.LastMapPages, s.LastMapStatus, s.Unmaps);
    for (int i = 0; i < 16; i++)
        if (s.PagingOps[i] != 0)
            printf(" [%d]=%u", i, s.PagingOps[i]);
    printf("\n  patches %u: last with %u allocations, %u patch locations; allocation 0 in segment %u at 0x%llx\n",
           s.Patches, s.PatchAllocations, s.PatchPatchLocations, s.PatchSegment0,
           (unsigned long long)s.PatchAddress0);
    printf("  presents: %u blts, %u fills, %u skipped\n", s.PresentBlts, s.PresentFills, s.PresentSkips);
}

int main(void)
{
    D3DKMT_HANDLE adapter;
    D3DKMT_CREATEDEVICE dev = {0};
    D3DKMT_CREATECONTEXT ctx = {0};
    D3DKMT_CREATEALLOCATION ca = {0};
    D3DDDI_ALLOCATIONINFO ai = {0};
    MW_ALLOCATION_INFO priv = {0};
    D3DKMT_LOCK lock = {0};
    D3DKMT_UNLOCK unlock = {0};
    D3DKMT_RENDER render = {0};
    MW_ESCAPE_ALLOC_INFO_DATA where = {0};
    NTSTATUS st;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!resolve_all())
        return 3;
    adapter = find_adapter();
    if (adapter == 0) {
        printf("no MaliG76Wddm adapter with the GPU half up (Parameters\\GpuEnable 1?)\n");
        return 1;
    }

    g_adapter = adapter;
    print_stats(adapter, "at start");
    dev.hAdapter = adapter;
    if ((st = pCreateDevice(&dev)) < 0)
        return fail("CreateDevice", st);
    ctx.hDevice = dev.hDevice;
    ctx.NodeOrdinal = 0;
    ctx.EngineAffinity = 1;
    ctx.ClientHint = D3DKMT_CLIENTHINT_DX10;
    if ((st = pCreateContext(&ctx)) < 0)
        return fail("CreateContext", st);
    printf("device 0x%x context 0x%x, command buffer %u bytes, %u allocation slots\n",
           dev.hDevice, ctx.hContext, ctx.CommandBufferSize, ctx.AllocationListSize);

    priv.Version = MW_ABI_VERSION;
    priv.Flags = MW_ALLOC_NOEXEC;
    priv.Size = JOB_BYTES;
    ai.pPrivateDriverData = &priv;
    ai.PrivateDriverDataSize = sizeof(priv);
    ca.hDevice = dev.hDevice;
    ca.NumAllocations = 1;
    ca.pAllocationInfo = &ai;
    if ((st = pCreateAllocation(&ca)) < 0)
        return fail("CreateAllocation", st);

    where.Code = MW_ESCAPE_ALLOC_INFO;
    where.hAllocation = ai.hAllocation;
    if ((st = escape(adapter, dev.hDevice, &where, sizeof(where))) < 0)
        return fail("Escape(ALLOC_INFO)", st);
    printf("allocation 0x%x at GPU 0x%llx, %llu bytes\n", ai.hAllocation,
           (unsigned long long)where.GpuVa, (unsigned long long)where.Size);
    if (where.GpuVa == 0)
        return fail("allocation has no GPU address (GPU not up?)", 0);

    lock.hDevice = dev.hDevice;
    lock.hAllocation = ai.hAllocation;
    if ((st = pLock(&lock)) < 0)
        return fail("Lock", st);
    volatile UINT32 *cpu = (volatile UINT32 *)lock.pData;
    memset((void *)cpu, 0, JOB_BYTES);
    write_job(cpu, where.GpuVa + TARGET_OFFSET, MAGIC);
    unlock.hDevice = dev.hDevice;
    unlock.NumAllocations = 1;
    unlock.phAllocations = &ai.hAllocation;
    if ((st = pUnlock(&unlock)) < 0)
        return fail("Unlock", st);

    /* One command, one allocation it writes. */
    MW_COMMAND *cmd = (MW_COMMAND *)ctx.pCommandBuffer;
    cmd->Type = MW_CMD_JOB;
    cmd->Slot = 1;
    cmd->Jc = where.GpuVa;
    memset(&ctx.pAllocationList[0], 0, sizeof(ctx.pAllocationList[0]));
    ctx.pAllocationList[0].hAllocation = ai.hAllocation;
    ctx.pAllocationList[0].WriteOperation = 1;
    render.hContext = ctx.hContext;
    render.CommandLength = sizeof(*cmd);
    render.AllocationCount = 1;
    render.PatchLocationCount = 0;
    if ((st = pRender(&render)) < 0)
        return fail("Render", st);
    printf("rendered\n");

    /* Locking waits for the GPU to finish with the allocation. */
    ULONGLONG t0 = GetTickCount64();
    if ((st = pLock(&lock)) < 0)
        return fail("Lock after render", st);
    cpu = (volatile UINT32 *)lock.pData;
    UINT32 got = cpu[TARGET_OFFSET / 4];
    UINT32 status = cpu[0];
    (void)pUnlock(&unlock);
    print_stats(adapter, "after the job");
    printf("after %llu ms: target 0x%08x (want 0x%08x), job header status word 0x%08x\n",
           GetTickCount64() - t0, got, MAGIC, status);

    {
        D3DKMT_DESTROYALLOCATION da = {0};
        D3DKMT_DESTROYCONTEXT dc = {0};
        D3DKMT_DESTROYDEVICE dd = {0};
        da.hDevice = dev.hDevice;
        da.phAllocationList = &ai.hAllocation;
        da.AllocationCount = 1;
        (void)pDestroyAllocation(&da);
        dc.hContext = ctx.hContext;
        (void)pDestroyContext(&dc);
        dd.hDevice = dev.hDevice;
        (void)pDestroyDevice(&dd);
    }

    if (got != MAGIC) {
        printf("FAIL: the GPU did not write the value\n");
        return 2;
    }
    printf("PASS: the Mali ran a job submitted through WDDM\n");
    return 0;
}
