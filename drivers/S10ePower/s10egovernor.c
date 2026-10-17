/*
 * s10egovernor - CPU/GPU clock governor service for the S10e (notes/clocks-dvfs.md C3). Runs as the S10eGovernor
 * service (LocalSystem) and drives S10ePower:
 *   - screen off (Windows display state off, or the panel darkened by the power button - MaliG76Wddm's
 *     PanelDisplayPower trace value) -> every domain at its minimum;
 *   - touch (S6SY761 touch driver's \BaseNamedObjects\S10eTouchActivity event) -> boost rates for BoostMs after the
 *     last touch, so the frame answering a touch is rendered fast;
 *   - otherwise load tracking per cluster every 50 ms: target = 1.25 x busiest-core utilisation x ceiling, the next
 *     ladder step at or above it; up at once, down only after DownSamples quiet samples (hysteresis).
 * Ceilings / boost rates come from HKLM\SYSTEM\CurrentControlSet\Services\S10eGovernor\Parameters (REG_DWORD kHz:
 * MaxLittle, MaxMid, MaxG3d, BoostLittle, BoostMid, BoostG3d, BoostMs, DownSamples); defaults = the UEFI boot rates,
 * i.e. the governor only LOWERS clocks until the max-clock tests (C5) set higher ceilings. The S10ePower thermal guard
 * caps every request anyway.
 *   s10egovernor            run as the service (SCM)
 *   s10egovernor console    run in the console (testing), prints each change
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pdh.h>
#include "S10ePowerIoctl.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "pdh.lib")

/* GPU busy: Windows' own "GPU Engine" counters (the scheduler's per-engine running time, as in Task Manager):
 * the busiest 3D engine instance in percent, sampled every 4th loop (200 ms). -1 = counter unavailable. */
static PDH_HQUERY gGpuQuery;
static PDH_HCOUNTER gGpuCounter;

static void GpuInit(void)
{
    if (PdhOpenQueryW(NULL, 0, &gGpuQuery) != ERROR_SUCCESS ||
        PdhAddEnglishCounterW(gGpuQuery, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &gGpuCounter) !=
            ERROR_SUCCESS) {
        gGpuQuery = NULL;
        return;
    }
    PdhCollectQueryData(gGpuQuery);
}

static double GpuBusy(void)
{
    static BYTE buf[16384];
    DWORD bytes = sizeof(buf), n = 0, i;
    double best = 0;
    PDH_FMT_COUNTERVALUE_ITEM_W *items = (PDH_FMT_COUNTERVALUE_ITEM_W *)buf;
    if (!gGpuQuery || PdhCollectQueryData(gGpuQuery) != ERROR_SUCCESS)
        return -1;
    if (PdhGetFormattedCounterArrayW(gGpuCounter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &n, items) != ERROR_SUCCESS)
        return -1;
    for (i = 0; i < n; i++)
        if (items[i].FmtValue.CStatus == ERROR_SUCCESS && items[i].FmtValue.doubleValue > best)
            best = items[i].FmtValue.doubleValue;
    return best / 100.0;
}

static const GUID kConsoleDisplayState = { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };
                                         /* GUID_CONSOLE_DISPLAY_STATE (winnt.h) */

static const unsigned kLadderLittle[] = { 442000, 546000, 650000, 806000, 949000, 1053000, 1157000, 1300000, 1456000,
                                          1586000, 1742000, 1846000, 1950000 };
static const unsigned kLadderMid[] = { 507000, 650000, 754000, 845000, 962000, 1066000, 1222000, 1404000, 1508000,
                                       1586000, 1690000, 1794000, 1898000, 2002000, 2106000, 2210000, 2314000 };
static const unsigned kLadderG3d[] = { 156000, 200000, 260000, 325000, 377000, 433000, 572000, 650000, 702000 };

typedef struct {
    const char *Name;
    unsigned Dom, Max, Boost, Cur, Quiet;
    const unsigned *Ladder;
    unsigned Count;
} DOMAIN;

static DOMAIN gDom[3] = {
    { "LITTLE", S10EPOWER_DOM_LITTLE, 1456000, 1456000, 0, 0, kLadderLittle, ARRAYSIZE(kLadderLittle) },
    { "MID", S10EPOWER_DOM_MID, 1794000, 1794000, 0, 0, kLadderMid, ARRAYSIZE(kLadderMid) },
    { "G3D", S10EPOWER_DOM_G3D, 260000, 260000, 0, 0, kLadderG3d, ARRAYSIZE(kLadderG3d) },
};
static unsigned gBoostMs = 300, gDownSamples = 6;
static volatile LONG gDisplayOn = 1;
static HANDLE gStop, gPower = INVALID_HANDLE_VALUE;
static SERVICE_STATUS_HANDLE gSs;
static SERVICE_STATUS gStatus;
static BOOL gConsole;
static unsigned gChanges, gTouches, gScreenOffs;

typedef NTSTATUS(NTAPI *NtQSI)(ULONG, PVOID, ULONG, PULONG);

static unsigned Reg(HKEY k, const char *name, unsigned def)
{
    DWORD v = def, sz = sizeof(v), type = 0;
    if (k && RegQueryValueExA(k, name, NULL, &type, (BYTE *)&v, &sz) == ERROR_SUCCESS && type == REG_DWORD)
        return v;
    return def;
}

static void LoadConfig(void)
{
    HKEY k = NULL;
    RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\S10eGovernor\\Parameters", 0, KEY_READ, &k);
    gDom[0].Max = Reg(k, "MaxLittle", 1456000); gDom[0].Boost = Reg(k, "BoostLittle", gDom[0].Max);
    gDom[1].Max = Reg(k, "MaxMid", 1794000);    gDom[1].Boost = Reg(k, "BoostMid", gDom[1].Max);
    gDom[2].Max = Reg(k, "MaxG3d", 260000);     gDom[2].Boost = Reg(k, "BoostG3d", gDom[2].Max);
    gBoostMs = Reg(k, "BoostMs", 300);
    gDownSamples = Reg(k, "DownSamples", 6);
    if (k)
        RegCloseKey(k);
}

static void Stat(const char *name, unsigned v)
{
    HKEY k;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\S10eGovernor\\Parameters", 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
        RegCloseKey(k);
    }
}

/* the panel darkened by the power button (s10ekeys blank) is not a Windows display-off: read MaliG76Wddm's trace */
static BOOL PanelLit(void)
{
    HKEY k;
    DWORD v = 0x101, sz = sizeof(v), type = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SYSTEM\\CurrentControlSet\\Enum\\ACPI\\SAMS9828\\0\\Device Parameters\\MaliG76Wddm", 0, KEY_READ,
                      &k) == ERROR_SUCCESS) {
        RegQueryValueExA(k, "PanelDisplayPower", NULL, &type, (BYTE *)&v, &sz);
        RegCloseKey(k);
    }
    return (v & 0x100) != 0 || (v & 1) == 0;   /* 0x100 = on; a failed DCS write (bit0 0) leaves it as it was */
}

/* the lowest ladder step >= Khz, clamped to [ladder min, Max] */
static unsigned StepAtLeast(const DOMAIN *d, unsigned khz)
{
    unsigned i, best = d->Ladder[0];
    for (i = 0; i < d->Count; i++) {
        if (d->Ladder[i] > d->Max)
            break;
        best = d->Ladder[i];
        if (d->Ladder[i] >= khz)
            break;
    }
    return best;
}

static void Set(DOMAIN *d, unsigned khz)
{
    S10EPOWER_SET_FREQ f;
    S10EPOWER_STATE s;
    DWORD got;
    if (khz == d->Cur)
        return;
    f.Domain = d->Dom;
    f.Khz = khz;
    if (DeviceIoControl(gPower, IOCTL_S10EPOWER_SET_FREQ, &f, sizeof(f), &s, sizeof(s), &got, NULL)) {
        d->Cur = khz;
        gChanges++;
        if (gConsole)
            printf("%lu %s -> %u kHz (granted %u)\n", GetTickCount(), d->Name, khz,
                   s.CurKhz[d == &gDom[0] ? 0 : d == &gDom[1] ? 1 : 2]);
    }
}

static DWORD WINAPI CtlHandler(DWORD ctl, DWORD type, LPVOID data, LPVOID ctx)
{
    (void)ctx;
    switch (ctl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        gStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(gSs, &gStatus);
        SetEvent(gStop);
        return NO_ERROR;
    case SERVICE_CONTROL_POWEREVENT:
        if (type == PBT_POWERSETTINGCHANGE) {
            POWERBROADCAST_SETTING *p = (POWERBROADCAST_SETTING *)data;
            if (IsEqualGUID(&p->PowerSetting, &kConsoleDisplayState) && p->DataLength >= sizeof(DWORD))
                InterlockedExchange(&gDisplayOn, *(DWORD *)p->Data != 0);   /* 0 off, 1 on, 2 dimmed */
        }
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

static void Run(void)
{
    NtQSI qsi = (NtQSI)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation");
    SYSTEM_INFO si;
    SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION prev[8], now[8];
    HANDLE touch = NULL;
    ULONGLONG lastTouch = 0, lastStat = 0;
    unsigned ncpu, i, loops = 0;
    double gpuBusy = -1;

    GetSystemInfo(&si);
    ncpu = si.dwNumberOfProcessors > 8 ? 8 : si.dwNumberOfProcessors;
    LoadConfig();
    for (i = 0; i < 3; i++)
        gDom[i].Cur = gDom[i].Max;   /* what UEFI left (the boot rates) */
    gPower = CreateFileW(S10EPOWER_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (gPower == INVALID_HANDLE_VALUE) {
        Stat("LastError", GetLastError());
        return;
    }
    qsi(8 /* SystemProcessorPerformanceInformation */, prev, sizeof(prev[0]) * ncpu, NULL);
    GpuInit();
    Stat("GpuCounter", gGpuQuery != NULL);
    for (;;) {
        DWORD w;
        BOOL screen, boost;
        ULONGLONG t;
        if (touch == NULL)
            touch = OpenEventW(SYNCHRONIZE, FALSE, L"Global\\S10eTouchActivity");
        {
            HANDLE hs[2] = { gStop, touch };
            w = WaitForMultipleObjects(touch ? 2 : 1, hs, FALSE, 50);
        }
        if (w == WAIT_OBJECT_0)
            break;
        t = GetTickCount64();
        if ((++loops & 3) == 0)
            gpuBusy = GpuBusy();
        if (w == WAIT_OBJECT_0 + 1) {
            lastTouch = t;
            gTouches++;
        }
        screen = gDisplayOn && PanelLit();
        boost = screen && lastTouch && t - lastTouch < gBoostMs;
        if (!screen) {
            for (i = 0; i < 3; i++)
                Set(&gDom[i], gDom[i].Ladder[0]);
            if (gDom[0].Quiet != 0xFFFFFFFF)
                gScreenOffs++;
            gDom[0].Quiet = 0xFFFFFFFF;   /* marks "off" for the counter */
        } else {
            double busy[2] = { 0, 0 };
            if (gDom[0].Quiet == 0xFFFFFFFF)
                gDom[0].Quiet = 0;
            qsi(8, now, sizeof(now[0]) * ncpu, NULL);
            for (i = 0; i < ncpu; i++) {
                double total = (double)(now[i].KernelTime.QuadPart - prev[i].KernelTime.QuadPart) +
                               (double)(now[i].UserTime.QuadPart - prev[i].UserTime.QuadPart);   /* kernel incl. idle */
                double idle = (double)(now[i].IdleTime.QuadPart - prev[i].IdleTime.QuadPart);
                double b = total > 0 ? 1.0 - idle / total : 0;
                unsigned c = i < 4 ? 0 : 1;   /* cpu0-3 A55 LITTLE, cpu4-5 A75 MID */
                if (b > busy[c])
                    busy[c] = b;
            }
            memcpy(prev, now, sizeof(now[0]) * ncpu);
            for (i = 0; i < 3; i++) {
                DOMAIN *d = &gDom[i];
                unsigned target;
                if (boost) {
                    target = StepAtLeast(d, d->Boost);
                } else if (i < 2) {
                    target = StepAtLeast(d, (unsigned)(busy[i] * 1.25 * d->Max));
                } else if (gpuBusy >= 0) {
                    /* GPU: load tracking on the busiest 3D engine (1.25 x busy x ceiling), floor = half the ceiling
                     * while the screen is on so composition never starts from the minimum */
                    unsigned want = (unsigned)(gpuBusy * 1.25 * d->Max);
                    target = StepAtLeast(d, want > d->Max / 2 ? want : d->Max / 2);
                } else {
                    /* no GPU counter: keep the ceiling; half-ceiling step after 5 s without touch + idle CPUs */
                    BOOL idle = (!lastTouch || t - lastTouch > 5000) && busy[0] < 0.10 && busy[1] < 0.10;
                    target = StepAtLeast(d, idle ? d->Max / 2 : d->Max);
                }
                if (target >= d->Cur) {
                    d->Quiet = 0;
                    Set(d, target);
                } else if (++d->Quiet >= gDownSamples) {
                    d->Quiet = 0;
                    Set(d, target);
                }
            }
        }
        if (t - lastStat > 10000) {
            lastStat = t;
            Stat("Changes", gChanges); Stat("Touches", gTouches); Stat("ScreenOffs", gScreenOffs);
            Stat("CurLittle", gDom[0].Cur); Stat("CurMid", gDom[1].Cur); Stat("CurG3d", gDom[2].Cur);
            Stat("TouchEvent", touch != NULL);
            Stat("GpuBusyPct", gpuBusy < 0 ? 0xFFFFFFFF : (unsigned)(gpuBusy * 100));
        }
    }
    /* leaving: back to the boot rates so nothing stays at a minimum without a governor */
    for (i = 0; i < 3; i++)
        Set(&gDom[i], gDom[i].Max);
    CloseHandle(gPower);
}

static void WINAPI ServiceMain(DWORD argc, LPWSTR *argv)
{
    (void)argc; (void)argv;
    gSs = RegisterServiceCtrlHandlerExW(L"S10eGovernor", CtlHandler, NULL);
    if (!gSs)
        return;
    gStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    gStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_POWEREVENT;
    gStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(gSs, &gStatus);
    RegisterPowerSettingNotification(gSs, &kConsoleDisplayState, DEVICE_NOTIFY_SERVICE_HANDLE);
    Run();
    gStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(gSs, &gStatus);
}

int main(int argc, char **argv)
{
    static SERVICE_TABLE_ENTRYW table[] = { { L"S10eGovernor", ServiceMain }, { NULL, NULL } };
    gStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (argc > 1 && !strcmp(argv[1], "console")) {
        gConsole = TRUE;
        printf("s10egovernor console mode (Ctrl+C to stop)\n");
        Run();
        return 0;
    }
    return StartServiceCtrlDispatcherW(table) ? 0 : 1;
}
