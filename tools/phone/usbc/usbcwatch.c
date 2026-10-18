/*
 * usbcwatch - G18 H1b (notes/g18-hdmi-dp-alt.md): watch the max77705 USB-C/PD block while a USB-C hub / HDMI dongle
 * is plugged. Reads registers FIRST..FIRST+COUNT-1 through S10eBattery's IOCTL_S10E_USBC_READ every INTERVAL ms for
 * SECONDS and prints the first full dump, then one line per register change (time, reg, old -> new). Read-only.
 * Note: reading 0x02..0x05 clears those interrupt latches, so a latched bit shows up exactly once.
 * usage: usbcwatch [SECONDS=60] [INTERVAL_MS=100] [FIRST=0] [COUNT=128]. SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <batclass.h>
#include <stdio.h>
#include <stdlib.h>

#define IOCTL_S10E_USBC_READ CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8A0, METHOD_BUFFERED, FILE_READ_ACCESS)
typedef struct { UCHAR Reg; UCHAR Length; } S10E_USBC_READ_IN;

static HANDLE OpenBattery(void)
{
    HDEVINFO h = SetupDiGetClassDevsW(&GUID_DEVICE_BATTERY, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA d = { sizeof(d) };
    HANDLE f = INVALID_HANDLE_VALUE;
    for (DWORD i = 0; f == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(h, NULL, &GUID_DEVICE_BATTERY, i, &d); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(h, &d, NULL, 0, &need, NULL);
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)calloc(1, need);
        det->cbSize = sizeof(*det);
        if (SetupDiGetDeviceInterfaceDetailW(h, &d, det, need, NULL, NULL)) {
            f = CreateFileW(det->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (f != INVALID_HANDLE_VALUE) {
                UCHAR probe;
                S10E_USBC_READ_IN in = { 0, 1 };
                DWORD got = 0;
                if (!DeviceIoControl(f, IOCTL_S10E_USBC_READ, &in, sizeof(in), &probe, 1, &got, NULL)) {
                    printf("battery %ls: IOCTL error %lu (not S10eBattery 1.0.0.6?)\n", det->DevicePath, GetLastError());
                    CloseHandle(f);
                    f = INVALID_HANDLE_VALUE;
                }
            }
        }
        free(det);
    }
    SetupDiDestroyDeviceInfoList(h);
    return f;
}

static BOOL ReadRegs(HANDLE f, UCHAR first, UCHAR count, UCHAR *out)
{
    S10E_USBC_READ_IN in = { first, count };
    DWORD got = 0;
    return DeviceIoControl(f, IOCTL_S10E_USBC_READ, &in, sizeof(in), out, count, &got, NULL) && got == count;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int seconds = argc > 1 ? atoi(argv[1]) : 60, interval = argc > 2 ? atoi(argv[2]) : 100;
    UCHAR first = (UCHAR)(argc > 3 ? strtoul(argv[3], NULL, 0) : 0), count = (UCHAR)(argc > 4 ? strtoul(argv[4], NULL, 0) : 128);
    if (count == 0 || count > 128 || first + count > 256) { printf("bad range\n"); return 1; }
    HANDLE f = OpenBattery();
    if (f == INVALID_HANDLE_VALUE) { printf("no S10eBattery device with the USB-C IOCTL\n"); return 2; }
    UCHAR prev[128], cur[128];
    if (!ReadRegs(f, first, count, prev)) { printf("first read failed %lu\n", GetLastError()); return 3; }
    printf("t=0.000 initial dump 0x%02X..0x%02X:\n", first, first + count - 1);
    for (int i = 0; i < count; i++) printf("%s%02X%s", i % 16 ? " " : "  ", prev[i], i % 16 == 15 || i == count - 1 ? "\n" : "");
    ULONGLONG t0 = GetTickCount64();
    unsigned reads = 1, fails = 0;
    while (GetTickCount64() - t0 < (ULONGLONG)seconds * 1000) {
        Sleep(interval);
        if (!ReadRegs(f, first, count, cur)) { fails++; continue; }
        reads++;
        double t = (GetTickCount64() - t0) / 1000.0;
        for (int i = 0; i < count; i++)
            if (cur[i] != prev[i]) printf("t=%.3f reg 0x%02X: %02X -> %02X\n", t, first + i, prev[i], cur[i]);
        memcpy(prev, cur, count);
    }
    printf("done: %u reads, %u failed\n", reads, fails);
    CloseHandle(f);
    return 0;
}
