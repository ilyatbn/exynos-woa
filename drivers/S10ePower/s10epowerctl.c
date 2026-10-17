/*
 * s10epowerctl - S10ePower test tool (notes/clocks-dvfs.md).
 *   s10epowerctl state                 temperatures, granted / capped / requested clocks, counters
 *   s10epowerctl set little|mid|g3d K  request K kHz (rounded down to the ladder, capped by the thermal guard)
 *   s10epowerctl set mif K             MIF: K >= 2093000 -> 2093000, else 421000 (the two verified rates, no cap)
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "S10ePowerIoctl.h"

static void Print(const S10EPOWER_STATE *s, DWORD got)
{
    printf("allow_send %u guard %u | temp MID %u LITTLE %u G3D %u (reads %u fails %u)\n", s->AllowSend, s->GuardLevel,
           s->TempC[1], s->TempC[2], s->TempC[3], s->TempReads, s->TempFails);
    printf("LITTLE cur %u cap %u req %u | MID cur %u cap %u req %u | G3D cur %u cap %u req %u kHz\n", s->CurKhz[0],
           s->CapKhz[0], s->ReqKhz[0], s->CurKhz[1], s->CapKhz[1], s->ReqKhz[1], s->CurKhz[2], s->CapKhz[2],
           s->ReqKhz[2]);
    printf("sends %u fails %u timeouts %u\n", s->Sends, s->SendFails, s->Timeouts);
    if (got >= sizeof(*s))
        printf("MIF cur %u req %u kHz (0 = not requested since the driver loaded)\n", s->MifCurKhz, s->MifReqKhz);
    else
        printf("MIF: driver version %u has no MIF control\n", s->Version);
}

int main(int argc, char **argv)
{
    HANDLE h = CreateFileW(S10EPOWER_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    S10EPOWER_STATE s;
    DWORD got = 0;
    memset(&s, 0, sizeof(s));
    if (h == INVALID_HANDLE_VALUE) {
        printf("cannot open S10ePower (%lu)\n", GetLastError());
        return 1;
    }
    if (argc >= 4 && !strcmp(argv[1], "set")) {
        S10EPOWER_SET_FREQ f;
        f.Domain = !strcmp(argv[2], "little") ? S10EPOWER_DOM_LITTLE : !strcmp(argv[2], "mid") ? S10EPOWER_DOM_MID
                 : !strcmp(argv[2], "g3d") ? S10EPOWER_DOM_G3D : !strcmp(argv[2], "mif") ? S10EPOWER_DOM_MIF : 0xFFFF;
        f.Khz = (unsigned int)strtoul(argv[3], NULL, 10);
        if (!DeviceIoControl(h, IOCTL_S10EPOWER_SET_FREQ, &f, sizeof(f), &s, sizeof(s), &got, NULL)) {
            printf("set failed (%lu)\n", GetLastError());
            return 2;
        }
    } else if (!DeviceIoControl(h, IOCTL_S10EPOWER_GET_STATE, NULL, 0, &s, sizeof(s), &got, NULL)) {
        printf("state failed (%lu)\n", GetLastError());
        return 3;
    }
    Print(&s, got);
    return 0;
}
