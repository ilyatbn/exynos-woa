/*
 * s10egov.c - temperature-driven clock governor for the S10e under Windows (notes/thermal-clocks.md).
 * Reads the TMU over MMIO (same decode as tools/phone/s10etemp) every second and sets the LITTLE/MID CPU clusters and
 * the G3D through ACPM FREQ_REQ (tools/phone/s10eacpm) by temperature level: boost while cool, stepping down below
 * the DT trip points (CPU control 83 C, GPU control 88 C, alerts 95+), 4 C hysteresis. A failed temperature read
 * drops to the base clocks (the UEFI ones). The 115 C TMU hardware trip (armed, notes) stays the last line.
 * Runs as SYSTEM at boot (scheduled task); one instance (named mutex). Log: C:\S10eTmp\s10egov.log.
 *
 *   s10egov            run the governor
 *   s10egov base       request the base clocks once and exit
 *   s10egov status     print temperatures once
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <share.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "..\..\..\drivers\S10eDiag\S10eDiagIoctl.h"
#include "..\s10eacpm\s10eacpm.h"

#define TMU_TOP 0x10090000ULL
#define TMU_SUB 0x100A0000ULL
#define DOM_LITTLE 2u
#define DOM_MID    3u
#define DOM_G3D    10u
#define HYST       40     // tenths of C

typedef struct { unsigned little, mid; } CPU_LEVEL;
static const CPU_LEVEL kCpu[] = { { 1742000, 2106000 }, { 1456000, 1794000 }, { 1157000, 1404000 }, { 949000, 1066000 } };
static const int       kCpuUp[] = { 780, 850, 920 };        // enter level i+1 at this CPU zone max (tenths C)
static const unsigned  kGpu[] = { 260000, 260000, 200000, 156000 };   // boost held at 260 MHz while the 18:50 system hang is bisected (notes)
static const int       kGpuUp[] = { 800, 870, 940 };        // enter level i+1 at this G3D zone max
#define CPU_BASE 1
#define GPU_BASE 2

static HANDLE gDiag;
static FILE  *gLog;
static int    gCpuLevel = -1, gGpuLevel = -1;

static void Log(const char *fmt, ...)
{
    SYSTEMTIME st;
    va_list    ap;
    char       line[512];

    GetLocalTime(&st);
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%02u:%02u:%02u %s\n", st.wHour, st.wMinute, st.wSecond, line);
    if (gLog)
    {
        fprintf(gLog, "%04u-%02u-%02u %02u:%02u:%02u %s\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                st.wSecond, line);
        fflush(gLog);
    }
    fflush(stdout);
}

static int ReadRegs(unsigned long long a, unsigned long *out, unsigned long bytes)
{
    S10EDIAG_READ_IN in = { a, bytes, S10EDIAG_READ_MMIO };
    DWORD            got = 0;
    return DeviceIoControl(gDiag, IOCTL_S10EDIAG_READ_PHYS, &in, sizeof(in), out, bytes, &got, NULL) && got == bytes;
}

/* max temperature (tenths C) over the sensors in mask; -9999 on failure */
static int ZoneMax(unsigned long long base, unsigned mask)
{
    unsigned long trim[11], cur[6];
    int           s, max = -9999;

    if (!ReadRegs(base, trim, sizeof(trim)) || !ReadRegs(base + 0x84, cur, sizeof(cur)))
        return -9999;
    for (s = 0; s < 11; s++)
    {
        int c25 = (int)(trim[s] & 0x1ff), c85 = (int)((trim[s] >> 9) & 0x1ff);
        int code = (int)((cur[s / 2] >> ((s & 1) ? 16 : 0)) & 0x1ff), t;
        if (!((mask >> s) & 1u) || c85 <= c25)
            continue;
        t = 250 + (code - c25) * 600 / (c85 - c25);
        if (t > max)
            max = t;
    }
    return max;
}

/* level with hysteresis: go up as soon as the threshold is reached, come down only HYST below it */
static int Pick(int cur, int temp, const int *up, int n)
{
    int lvl = 0;
    while (lvl < n && temp >= up[lvl])
        lvl++;
    if (cur > lvl && temp > up[cur - 1] - HYST)
        lvl = cur;   // stay up until it has cooled by HYST
    return lvl;
}

static void SetCpu(int lvl, int t)
{
    unsigned long r[4];
    int           ok1, ok2;
    if (lvl == gCpuLevel)
        return;
    ok1 = AcpmFreqReq(DOM_LITTLE, kCpu[lvl].little, r);
    ok2 = AcpmFreqReq(DOM_MID, kCpu[lvl].mid, r);
    Log("CPU level %d -> %d (LITTLE %u, MID %u kHz) at %d.%d C%s", gCpuLevel, lvl, kCpu[lvl].little, kCpu[lvl].mid,
        t / 10, abs(t % 10), (ok1 && ok2) ? "" : " - ACPM FAILED");
    if (ok1 && ok2)
        gCpuLevel = lvl;
}

static void SetGpu(int lvl, int t)
{
    unsigned long r[4];
    int           ok;
    if (lvl == gGpuLevel)
        return;
    ok = AcpmFreqReq(DOM_G3D, kGpu[lvl], r);
    Log("GPU level %d -> %d (%u kHz) at %d.%d C%s", gGpuLevel, lvl, kGpu[lvl], t / 10, abs(t % 10), ok ? "" : " - ACPM FAILED");
    if (ok)
        gGpuLevel = lvl;
}

static BOOL WINAPI OnCtrl(DWORD type)
{
    (void)type;
    gCpuLevel = gGpuLevel = -1;
    SetCpu(CPU_BASE, 0);
    SetGpu(GPU_BASE, 0);
    Log("stopped - base clocks requested");
    ExitProcess(0);
}

int main(int argc, char **argv)
{
    unsigned tick = 0;

    gDiag = CreateFileW(S10EDIAG_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (gDiag == INVALID_HANDLE_VALUE || !AcpmOpen())
    {
        printf("cannot open S10eDiag (%lu) - run elevated\n", GetLastError());
        return 1;
    }
    if (argc > 1 && strcmp(argv[1], "status") == 0)
    {
        printf("BIG %d MID %d LITTLE %d G3D %d (tenths C)\n", ZoneMax(TMU_TOP, 0x7c0), ZoneMax(TMU_TOP, 0x20),
               ZoneMax(TMU_TOP, 0x10), ZoneMax(TMU_SUB, 0x21e));
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "base") == 0)
    {
        SetCpu(CPU_BASE, 0);
        SetGpu(GPU_BASE, 0);
        return 0;
    }
    if (CreateMutexW(NULL, TRUE, L"Global\\S10eGovernor") && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        printf("already running\n");
        return 1;
    }
    gLog = _fsopen("C:\\S10eTmp\\s10egov.log", "a", _SH_DENYNO);
    SetConsoleCtrlHandler(OnCtrl, TRUE);
    Log("start: CPU levels %u/%u .. %u/%u kHz, GPU %u .. %u kHz", kCpu[0].little, kCpu[0].mid, kCpu[3].little,
        kCpu[3].mid, kGpu[0], kGpu[3]);
    for (;; tick++)
    {
        int big = ZoneMax(TMU_TOP, 0x7c0), mid = ZoneMax(TMU_TOP, 0x20), lit = ZoneMax(TMU_TOP, 0x10);
        int g3d = ZoneMax(TMU_SUB, 0x21e);
        int cpu = max(big, max(mid, lit));

        if (cpu < -1000 || g3d < -1000 || cpu > 1300 || g3d > 1300)
        {
            Log("temperature read failed/implausible (cpu %d g3d %d) - base clocks", cpu, g3d);
            SetCpu(CPU_BASE, cpu);
            SetGpu(GPU_BASE, g3d);
        }
        else
        {
            SetCpu(Pick(gCpuLevel < 0 ? 0 : gCpuLevel, cpu, kCpuUp, 3), cpu);
            SetGpu(Pick(gGpuLevel < 0 ? 0 : gGpuLevel, g3d, kGpuUp, 3), g3d);
            if (tick % 30 == 0)
                Log("temps BIG %d.%d MID %d.%d LITTLE %d.%d G3D %d.%d C, CPU level %d, GPU level %d", big / 10,
                    abs(big % 10), mid / 10, abs(mid % 10), lit / 10, abs(lit % 10), g3d / 10, abs(g3d % 10),
                    gCpuLevel, gGpuLevel);
        }

        //
        // Silent-reset evidence (notes/thermal-clocks.md): the cluster-0 watchdog (0x10050000, 82 s, petted by
        // Exynos9820Ufs) and the RWD1 record (0xFED13D80, drivers/include/Rwd1Constants.h). Every 30 s, and every
        // second while WTCNT is below half (= petting stopped) - the last lines before a reset tell who stopped.
        //
        {
            unsigned long wdt[3], rwd1[8];
            if (ReadRegs(0x10050000ULL, wdt, sizeof(wdt)) && ReadRegs(0xFED13D80ULL, rwd1, sizeof(rwd1)) &&
                (tick % 30 == 0 || wdt[2] < 0xFFF5u / 2))
                Log("wdt con %08lx cnt %lu/65525, rwd1 %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx", wdt[0], wdt[2],
                    rwd1[0], rwd1[1], rwd1[2], rwd1[3], rwd1[4], rwd1[5], rwd1[6], rwd1[7]);
        }
        Sleep(1000);
    }
}
