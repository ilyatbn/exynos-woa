/*
 * usbcaltmode - G18 H2 (notes/g18-h2-altmode-research.md section H, notes/g18-hdmi-prep.md P2): DP alt-mode entry on
 * the max77705 through S10eBattery 1.0.0.7's allowlisted IOCTL_S10E_USBC_OPCODE, then watch the alt-mode events until
 * the DP link is configured (pin assignment) and report HPD. Every opcode, reply byte and latch is logged.
 *
 * usage: usbcaltmode -dry               print the planned sequence and payloads, open nothing
 *        usbcaltmode [SECONDS=60] [-stop]  enable alt mode (0x55 0x03), follow the events for SECONDS,
 *                                      -stop: send 0x55 0x01 (alt mode stop) at the end
 * Writes only what the driver allows (0x55 / 0x48 Identity, Enter Mode, DP Configure / 0x4B). No VBUS, role or
 * charging opcode exists in this tool (research section G). SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <batclass.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IOCTL_S10E_USBC_READ   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8A0, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_S10E_USBC_OPCODE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8A1, METHOD_BUFFERED, FILE_WRITE_ACCESS)
typedef struct { UCHAR Reg; UCHAR Length; } S10E_USBC_READ_IN;
typedef struct { UCHAR Opcode; UCHAR DataLen; UCHAR ReadLen; UCHAR Data[32]; } S10E_USBC_OPCODE_IN;

/* research section C/G: the only SEND_VDM payloads (byte packing checked there) */
static const UCHAR kIdentity[5] = { 0x09, 0x01, 0x80, 0x00, 0xFF };
static const UCHAR kEnter[5]    = { 0x09, 0x04, 0x81, 0x01, 0xFF };
static const UCHAR kConfig[9]   = { 0x0A, 0x11, 0x81, 0x01, 0xFF, 0x06, 0x08 /* pin, replaced */, 0x00, 0x00 };

static HANDLE g_dev = INVALID_HANDLE_VALUE;
static ULONGLONG g_t0;

static double T(void) { return (GetTickCount64() - g_t0) / 1000.0; }

static void Hex(const char *what, const UCHAR *p, unsigned n)
{
    printf("t=%.3f %s:", T(), what);
    for (unsigned i = 0; i < n; i++) printf(" %02X", p[i]);
    printf("\n");
}

static HANDLE OpenBattery(void)
{
    HDEVINFO h = SetupDiGetClassDevsW(&GUID_DEVICE_BATTERY, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA d = { sizeof(d) };
    HANDLE f = INVALID_HANDLE_VALUE;
    for (DWORD i = 0; f == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(h, NULL, &GUID_DEVICE_BATTERY, i, &d); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(h, &d, NULL, 0, &need, NULL);
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)calloc(1, need);
        if (!det) break;
        det->cbSize = sizeof(*det);
        if (SetupDiGetDeviceInterfaceDetailW(h, &d, det, need, NULL, NULL)) {
            f = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, 0, NULL);
            if (f != INVALID_HANDLE_VALUE) {
                UCHAR probe;
                S10E_USBC_READ_IN in = { 0, 1 };
                DWORD got = 0;
                if (!DeviceIoControl(f, IOCTL_S10E_USBC_READ, &in, sizeof(in), &probe, 1, &got, NULL)) {
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

static BOOL Rd(UCHAR reg, UCHAR n, UCHAR *out)
{
    S10E_USBC_READ_IN in = { reg, n };
    DWORD got = 0;
    return DeviceIoControl(g_dev, IOCTL_S10E_USBC_READ, &in, sizeof(in), out, n, &got, NULL) && got == n;
}

/* one opcode; reply[0] should echo the opcode, reply[1..rlen] the payload */
static BOOL Opc(UCHAR op, const UCHAR *data, UCHAR n, UCHAR rlen, UCHAR *reply)
{
    S10E_USBC_OPCODE_IN in;
    DWORD got = 0;
    memset(&in, 0, sizeof(in));
    in.Opcode = op; in.DataLen = n; in.ReadLen = rlen;
    memcpy(in.Data, data, n);
    printf("t=%.3f OPC %02X len %u:", T(), op, n);
    for (unsigned i = 0; i < n; i++) printf(" %02X", data[i]);
    printf("\n");
    if (g_dev == INVALID_HANDLE_VALUE) return FALSE;   /* -dry */
    if (!DeviceIoControl(g_dev, IOCTL_S10E_USBC_OPCODE, &in, sizeof(in), reply, (DWORD)rlen + 1, &got, NULL)) {
        printf("t=%.3f   -> IOCTL error %lu\n", T(), GetLastError());
        return FALSE;
    }
    Hex("  reply", reply, got);
    if (got < 1 || reply[0] != op) printf("t=%.3f   (reply byte0 %02X != opcode %02X)\n", T(), got ? reply[0] : 0, op);
    return TRUE;
}

static char PinLetter(UCHAR one_hot)
{
    for (int i = 0; i < 6; i++) if (one_hot == (1u << i)) return "ABCDEF"[i];
    return '?';
}

static ULONG Le32(const UCHAR *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((ULONG)p[3] << 24); }

static void Status(const char *when)
{
    UCHAR s[4], m[4];
    if (!Rd(0x0A, 4, s) || !Rd(0x0E, 4, m)) { printf("t=%.3f %s: status read failed\n", T(), when); return; }
    printf("t=%.3f %s: CC_STATUS0 %02X (CCStat %u, CCPinStat %u) CC_STATUS1 %02X (Altmode %u, ConnStat %u)"
           " PD_STATUS0 %02X PD_STATUS1 %02X (%s) masks UIC %02X CC %02X PD %02X VDM %02X\n",
           T(), when, s[0], s[0] & 7, s[0] >> 6, s[1], s[1] & 1, (s[1] >> 1) & 1, s[2], s[3],
           (s[3] & 0x80) ? "DFP" : "UFP", m[0], m[1], m[2], m[3]);
}

/* research section C: multi-function preference D,B,F,C,E,A else C,E,A,D,B,F */
static UCHAR SelectPin(UCHAR pins, BOOL mfp)
{
    static const UCHAR pref_mfp[] = { 0x08, 0x02, 0x20, 0x04, 0x10, 0x01 };
    static const UCHAR pref[]     = { 0x04, 0x10, 0x01, 0x08, 0x02, 0x20 };
    const UCHAR *p = mfp ? pref_mfp : pref;
    for (int i = 0; i < 6; i++) if (pins & p[i]) return p[i];
    return 0;
}

static void PrintPlan(void)
{
    UCHAR r[33];
    UCHAR cfg[9];
    printf("planned sequence (research section H):\n");
    printf(" 0 read 0x0A-0x0D status, 0x0E-0x11 masks\n");
    Opc(0x55, (const UCHAR *)"\x03", 1, 1, r);
    printf("   then CC_STATUS1 bit0 Altmode should read 1\n");
    printf(" 3 only if SINK + DFP:\n");
    Opc(0x48, kIdentity, 5, 10, r);
    printf(" 4-7 on VDM_INT 0x05 b0/b1/b2/b3: read the response (0x4B step 01/02/03/04), after MODES send Enter Mode:\n");
    Opc(0x4B, (const UCHAR *)"\x03", 1, 11, r);
    Opc(0x48, kEnter, 5, 10, r);
    printf(" 8 on PD_INT 0x04 b2 DP_STATUS_UPDATE: 0x4B step 10, if Port_Connected send DP Configure (pin C shown):\n");
    memcpy(cfg, kConfig, 9); cfg[6] = 0x04;
    Opc(0x48, cfg, 9, 14, r);
    printf(" 9 on PD_INT b3 DP_CONFIGURE: 0x4B step 11 -> configured; 10 on PD_INT b4 ATTENTION: 0x4B step 06 -> HPD\n");
    printf("11 detach (CC_INT b0 + CCStat 0): forget; -stop sends 0x55 0x01 at the end\n");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    g_t0 = GetTickCount64();
    int seconds = 60;
    BOOL stop = FALSE;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-dry")) { PrintPlan(); return 0; }
        else if (!strcmp(argv[i], "-stop")) stop = TRUE;
        else seconds = atoi(argv[i]);
    }
    g_dev = OpenBattery();
    if (g_dev == INVALID_HANDLE_VALUE) { printf("no S10eBattery device with the USB-C IOCTLs (1.0.0.7 needed for -opcodes)\n"); return 2; }

    UCHAR r[33], lat[4], st[4];
    Status("before");
    Rd(0x02, 4, lat);   /* drop stale latches */
    Hex("stale latches 02-05", lat, 4);

    if (!Opc(0x55, (const UCHAR *)"\x03", 1, 1, r)) { printf("alt-mode enable failed\n"); return 3; }
    Sleep(50);
    Status("after 0x55 0x03");

    if (Rd(0x0A, 4, st) && (st[0] & 7) == 1 && (st[3] & 0x80))   /* research section F: SINK + DFP kicks Identity */
        Opc(0x48, kIdentity, 5, 10, r);

    UCHAR pins = 0, chosen = 0;
    BOOL configured = FALSE;
    ULONGLONG end = GetTickCount64() + (ULONGLONG)seconds * 1000;
    while (GetTickCount64() < end) {
        Sleep(50);
        if (!Rd(0x02, 4, lat)) continue;
        if (!(lat[0] | lat[1] | lat[2] | lat[3])) continue;
        Hex("latches UIC CC PD VDM", lat, 4);
        if ((lat[1] & 0x01) && Rd(0x0A, 1, st) && (st[0] & 7) == 0) {
            printf("t=%.3f detach (CCStat 0) - state forgotten\n", T());
            pins = chosen = 0; configured = FALSE;
        }
        if (lat[3] & 0x01 && Opc(0x4B, (const UCHAR *)"\x01", 1, 31, r))
            printf("t=%.3f   DISCOVER_ID: VID %04lX PID %04lX\n", T(), Le32(r + 8) & 0xFFFF, Le32(r + 16) >> 16);
        if (lat[3] & 0x02 && Opc(0x4B, (const UCHAR *)"\x02", 1, 31, r)) {
            BOOL dp = FALSE;
            for (int i = 8; i + 1 < 32; i += 2) if ((r[i] | (r[i + 1] << 8)) == 0xFF01) dp = TRUE;
            printf("t=%.3f   DISCOVER_SVIDS: DisplayPort SVID 0xFF01 %s\n", T(), dp ? "present" : "ABSENT");
        }
        if (lat[3] & 0x04 && Opc(0x4B, (const UCHAR *)"\x03", 1, 11, r)) {
            ULONG vdo = Le32(r + 8);
            BOOL receptacle = (vdo >> 6) & 1;
            pins = (UCHAR)(receptacle ? (vdo >> 16) : (vdo >> 8));
            printf("t=%.3f   DISCOVER_MODES: DP caps VDO %08lX port_cap %lu %s pins %02X\n", T(), vdo, vdo & 3,
                   receptacle ? "receptacle (UFP_D pins)" : "plug (DFP_D pins)", pins);
            if (pins && (vdo & 1))   /* Port_Capability must include UFP_D */
                Opc(0x48, kEnter, 5, 10, r);
            else
                printf("t=%.3f   no UFP_D capability / no pins - Enter Mode not sent\n", T());
        }
        if (lat[3] & 0x08 && Opc(0x4B, (const UCHAR *)"\x04", 1, 7, r))
            printf("t=%.3f   ENTER_MODE: VDM header %08lX cmd_type %lu (1 = ACK)\n", T(), Le32(r + 4), (Le32(r + 4) >> 6) & 3);
        if (lat[2] & 0x04 && Opc(0x4B, (const UCHAR *)"\x10", 1, 11, r)) {
            ULONG s = Le32(r + 8);
            printf("t=%.3f   DP_STATUS: VDO %08lX connected %lu MFP %lu HPD %lu IRQ %lu\n", T(), s, s & 3, (s >> 4) & 1,
                   (s >> 7) & 1, (s >> 8) & 1);
            if ((s & 3) && !configured && pins) {
                UCHAR cfg[9];
                chosen = SelectPin(pins, (s >> 4) & 1);
                memcpy(cfg, kConfig, 9);
                cfg[6] = chosen;
                if (chosen) Opc(0x48, cfg, 9, 14, r);
            }
        }
        if (lat[2] & 0x08 && Opc(0x4B, (const UCHAR *)"\x11", 1, 11, r)) {
            configured = TRUE;
            printf("t=%.3f   DP_CONFIGURE ACK: link configured, pin %c (%s)\n", T(),
                   PinLetter(chosen),
                   (chosen & 0x15) ? "4-lane DP" : "2-lane DP + USB3");
        }
        if (lat[2] & 0x10 && Opc(0x4B, (const UCHAR *)"\x06", 1, 11, r)) {
            ULONG s = Le32(r + 8);
            printf("t=%.3f   ATTENTION: VDO %08lX HPD %lu IRQ %lu\n", T(), s, (s >> 7) & 1, (s >> 8) & 1);
        }
    }
    Status("end");
    if (stop) Opc(0x55, (const UCHAR *)"\x01", 1, 1, r);
    CloseHandle(g_dev);
    printf("done: configured %d pin %02X\n", configured, chosen);
    return 0;
}
