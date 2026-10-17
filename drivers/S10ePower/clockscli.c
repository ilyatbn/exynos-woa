/*
 * clockscli - (console; the GUI is clocks.c) live per-core CPU clocks and the GPU clock on the S10e (user 2026-10-17: "a tool showing current/max clock
 * speeds per core"). Windows' Task Manager shows firmware boot values (base speed = the MID cluster's 1794 MHz boot
 * rate) and its own estimate, not the real rates the governor sets through ACPM, so this measures:
 *   - each core: a ~2 ms burst of dependent single-cycle integer ops on a thread pinned to it (ops/ns ~= GHz, the
 *     S10ePerf method); short enough not to raise the load the governor sees;
 *   - the GPU: MaliG76Wddm's MW_ESCAPE_CLOCK (GPU cycle counter over QPC) - works without admin rights;
 *   - what the governor asked for (its registry stats, every 10 s) and, run as administrator, ACPM's granted rates
 *     and the thermal readings from S10ePower.
 * Table maxima: LITTLE (A55) 1950 MHz, MID (A75) 2400 MHz (DT; needs MIF 2093 MHz), GPU 702 MHz. The two BIG (M4)
 * cores are not running under Windows.
 *
 *   clockscli [refresh seconds=1] [count=0 (forever)]
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <d3dkmthk.h>
#include "maliwddm_abi.h"
#include "S10ePowerIoctl.h"

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

static unsigned long long volatile gSink;

static double Now(void)
{
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart / (double)f.QuadPart;
}

/* GHz of the core this thread runs on: 8 dependent XOR/ADD per iteration (no reassociation) */
static double AddChain(unsigned long long it)
{
    unsigned long long a = 1, b = 3, i;
    double t0 = Now(), t1;
    for (i = 0; i < it; i++) {
        a ^= b; a += i; a ^= b; a += i; a ^= b; a += i; a ^= b; a += i;
    }
    t1 = Now();
    gSink = a;
    return (double)(it * 8ull) / ((t1 - t0) * 1e9);
}

static BOOL GpuRead(D3DKMT_HANDLE a, MW_ESCAPE_CLOCK_DATA *c)
{
    D3DKMT_ESCAPE e = { 0 };
    ZeroMemory(c, sizeof(*c));
    c->Code = MW_ESCAPE_CLOCK;
    e.hAdapter = a;
    e.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    e.pPrivateDriverData = c;
    e.PrivateDriverDataSize = sizeof(*c);
    return D3DKMTEscape(&e) >= 0 && c->Valid;
}

static D3DKMT_HANDLE FindMali(void)
{
    D3DKMT_ADAPTERINFO info[16];
    D3DKMT_ENUMADAPTERS2 en = { 0 };
    MW_ESCAPE_CLOCK_DATA c;
    en.NumAdapters = 16;
    en.pAdapters = info;
    if (D3DKMTEnumAdapters2(&en) < 0)
        return 0;
    for (ULONG i = 0; i < en.NumAdapters; i++)
        if (GpuRead(info[i].hAdapter, &c))
            return info[i].hAdapter;
    return 0;
}

static DWORD RegDw(const char *name)
{
    HKEY k;
    DWORD v = 0, sz = sizeof(v), type = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\S10eGovernor\\Parameters", 0,
                      KEY_READ, &k) == ERROR_SUCCESS) {
        RegQueryValueExA(k, name, NULL, &type, (BYTE *)&v, &sz);
        RegCloseKey(k);
    }
    return v;
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 1, count = argc > 2 ? atoi(argv[2]) : 0;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    DWORD ncpu = si.dwNumberOfProcessors > 32 ? 32 : si.dwNumberOfProcessors;
    D3DKMT_HANDLE mali = FindMali();
    HANDLE pw = CreateFileW(S10EPOWER_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SetConsoleTitleA("S10e clocks");
    for (int n = 0; count == 0 || n < count; n++) {
        MW_ESCAPE_CLOCK_DATA g0 = { 0 }, g1 = { 0 };
        BOOL g = mali && GpuRead(mali, &g0);
        double ghz[32];
        for (DWORD c = 0; c < ncpu; c++) {
            SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << c);
            Sleep(0);
            AddChain(50000);                      /* settle on the core */
            ghz[c] = AddChain(500000);            /* ~2 ms at 2 GHz */
        }
        SetThreadAffinityMask(GetCurrentThread(), ~(DWORD_PTR)0);
        Sleep((DWORD)secs * 1000);
        g = g && GpuRead(mali, &g1) && g1.Qpc > g0.Qpc && g1.Cycles >= g0.Cycles;
        if (count != 1)
            printf("\x1b[2J\x1b[H");             /* clear the console (VT sequences, Windows 10+) */
        printf("S10e clocks (measured)          now        max\n");
        for (DWORD c = 0; c < ncpu; c++) {
            BOOL mid = c >= 4;
            printf("  cpu%lu  %-14s %6.0f MHz   %4u MHz\n", c, mid ? "A75 (fast)" : "A55 (small)", ghz[c] * 1000.0,
                   mid ? 2400u : 1950u);
        }
        if (g)
            printf("  GPU   Mali-G76 MP12  %6.0f MHz    702 MHz\n",
                   (double)(g1.Cycles - g0.Cycles) * (double)g1.QpcFreq / (double)(g1.Qpc - g0.Qpc) / 1e6);
        else
            printf("  GPU   Mali-G76 MP12      (no clock reading)\n");
        printf("governor requested: small %lu, fast %lu, GPU %lu, memory %lu MHz (updated every 10 s)\n",
               RegDw("CurLittle") / 1000, RegDw("CurMid") / 1000, RegDw("CurG3d") / 1000, RegDw("CurMif") / 1000);
        if (pw != INVALID_HANDLE_VALUE) {
            S10EPOWER_STATE s;
            DWORD got = 0;
            memset(&s, 0, sizeof(s));
            if (DeviceIoControl(pw, IOCTL_S10EPOWER_GET_STATE, NULL, 0, &s, sizeof(s), &got, NULL))
                printf("ACPM granted: small %u, fast %u, GPU %u MHz | temps: fast %u C, small %u C, GPU %u C\n",
                       s.CurKhz[0] / 1000, s.CurKhz[1] / 1000, s.CurKhz[2] / 1000, s.TempC[1], s.TempC[2],
                       s.TempC[3]);
        } else {
            printf("(run as administrator for ACPM granted rates and temperatures)\n");
        }
        printf("BIG cores (M4 x2): not running under Windows. Windows' \"base speed\" = a firmware boot value.\n");
        fflush(stdout);
    }
    return 0;
}
