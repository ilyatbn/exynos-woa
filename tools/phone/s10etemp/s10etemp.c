/*
 * s10etemp.c - read the Exynos 9820 TMU temperatures from Windows through S10eDiag (notes/thermal-clocks.md).
 * Two TMU blocks (TOP 0x10090000, SUB 0x100A0000), 11 sensors each: current code at +0x84 + (s/2)*4 (low half = even
 * sensor), per-sensor trim TRIMINFO(s) at s*4: bits[8:0] = code at 25 C, bits[17:9] = code at 85 C. Two-point
 * conversion T = 25 + (code - c25) * 60 / (c85 - c25) - checked on the device: the RISE7 threshold code decodes to
 * exactly the DT 115 C hot trip. Zones = DT sensor masks (stock dt.dtb): TOP BIG 0x7c0, MID 0x20, LITTLE 0x10;
 * SUB G3D 0x21e, ISP 0x100. Read-only.
 *
 *   s10etemp [interval_s count]       one line per sample: zone maxima in C (+ per-sensor with -v)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "..\..\..\drivers\S10eDiag\S10eDiagIoctl.h"

#define TMU_TOP     0x10090000ULL
#define TMU_SUB     0x100A0000ULL
#define TMU_SENSORS 11

typedef struct { const char *name; int block; unsigned mask; } ZONE;
static const ZONE kZones[] = {
    { "BIG", 0, 0x7c0 }, { "MID", 0, 0x020 }, { "LITTLE", 0, 0x010 }, { "G3D", 1, 0x21e }, { "ISP", 1, 0x100 },
};

static HANDLE gDiag;

static int ReadRegs(unsigned long long addr, unsigned long *out, unsigned long bytes)
{
    S10EDIAG_READ_IN in = { addr, bytes, S10EDIAG_READ_MMIO };
    DWORD            got = 0;
    return DeviceIoControl(gDiag, IOCTL_S10EDIAG_READ_PHYS, &in, sizeof(in), out, bytes, &got, NULL) && got == bytes;
}

/* tenths of a degree C for every sensor of one block; returns 0 on a read failure */
static int ReadBlock(unsigned long long base, int t10[TMU_SENSORS])
{
    unsigned long trim[TMU_SENSORS], cur[6];
    int           s;

    if (!ReadRegs(base, trim, sizeof(trim)) || !ReadRegs(base + 0x84, cur, sizeof(cur)))
        return 0;
    for (s = 0; s < TMU_SENSORS; s++)
    {
        int c25  = (int)(trim[s] & 0x1ff);
        int c85  = (int)((trim[s] >> 9) & 0x1ff);
        int code = (int)((cur[s / 2] >> ((s & 1) ? 16 : 0)) & 0x1ff);
        t10[s] = (c85 > c25) ? 250 + (code - c25) * 600 / (c85 - c25) : -9999;
    }
    return 1;
}

int main(int argc, char **argv)
{
    int interval = argc > 2 ? atoi(argv[1]) : 0, count = argc > 2 ? atoi(argv[2]) : 1, verbose = 0, i;

    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "-v") == 0)
            verbose = 1;
    gDiag = CreateFileW(S10EDIAG_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (gDiag == INVALID_HANDLE_VALUE)
    {
        printf("cannot open S10eDiag (%lu) - run elevated\n", GetLastError());
        return 1;
    }
    for (i = 0; i < count; i++)
    {
        int        t[2][TMU_SENSORS], z, s;
        SYSTEMTIME st;

        if (!ReadBlock(TMU_TOP, t[0]) || !ReadBlock(TMU_SUB, t[1]))
        {
            printf("read failed (%lu)\n", GetLastError());
            return 1;
        }
        GetLocalTime(&st);
        printf("%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
        for (z = 0; z < (int)(sizeof(kZones) / sizeof(kZones[0])); z++)
        {
            int max = -9999;
            for (s = 0; s < TMU_SENSORS; s++)
                if ((kZones[z].mask >> s) & 1u && t[kZones[z].block][s] > max)
                    max = t[kZones[z].block][s];
            printf("  %s %d.%d", kZones[z].name, max / 10, abs(max % 10));
        }
        printf(" C\n");
        if (verbose)
            for (z = 0; z < 2; z++)
            {
                printf("  %s:", z ? "SUB" : "TOP");
                for (s = 0; s < TMU_SENSORS; s++)
                    printf(" %d.%d", t[z][s] / 10, abs(t[z][s] % 10));
                printf("\n");
            }
        fflush(stdout);
        if (i + 1 < count)
            Sleep((DWORD)interval * 1000u);
    }
    CloseHandle(gDiag);
    return 0;
}
