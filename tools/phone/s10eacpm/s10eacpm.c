/*
 * s10eacpm.c - ACPM DVFS FREQ_REQ from Windows user mode through S10eDiag MMIO peek/poke (notes/thermal-clocks.md).
 * A port of the device-validated UEFI ACPM IPC client (Beyond0LteTouchPowerDxe / Beyond0LteGpuProbeDxe GpuAcpm.c,
 * itself a transliteration of the kernel's acpm_ipc.c - citations there). Channel 0 = DVFS requests (VERIFIED live
 * DT clock-controller@1a240000 acpm-ipc-channel 0). Domains: 2 LITTLE, 3 MID, 4 BIG, 10 G3D (notes/m15-perf.md,
 * notes/gpu-m16-g3don.md). Polling only; ONE process at a time must use channel 0 (no locking across processes).
 *
 *   s10eacpm freq <domain> <khz>      FREQ_REQ {domain, khz, 0, 0}, print the reply
 *   s10eacpm chan [id]                print the channel layout (default 0)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "..\..\..\drivers\S10eDiag\S10eDiagIoctl.h"
#include "s10eacpm.h"

#define MBOX_BASE      0x15900000ULL   // Exynos9820.h EXYNOS_ACPM_IPC_MBOX_BASE (VERIFIED)
#define SRAM_BASE      0x02039000ULL   // EXYNOS_ACPM_SRAM_BASE (VERIFIED)
#define SRAM_SIZE      0x30000u        // EXYNOS_ACPM_SRAM_SIZE (VERIFIED)
#define INITDATA       0x7000u         // EXYNOS_ACPM_INITDATA_OFFSET (VERIFIED)
#define MBOX_INTGR0    0x0008u
#define MBOX_INTCR1    0x0020u
#define MBOX_INTGR1    0x001Cu
#define MBOX_INTSR1    0x0028u
#define FW_IPC_CHANNELS 0x08u
#define FW_IPC_AP_MAX   0x18u
#define CH_STRIDE      0x48u
#define CH_ID          0x00u
#define CH_TYPE        0x0Cu
#define CH_RX_REAR     0x10u
#define CH_RX_FRONT    0x14u
#define CH_RX_BASE     0x18u
#define CH_TX_REAR     0x28u
#define CH_TX_FRONT    0x2Cu
#define CH_TX_BASE     0x30u
#define CH_Q_LEN       0x34u
#define CH_Q_ELEM_SIZE 0x38u
#define CH_AP_POLL     0x44u
#define TYPE_QUEUE     1u
#define SEQ_SHIFT      16u
#define SEQ_MASK       0x3Fu
#define REPLY_WAIT_MS  100u

static HANDLE   gDiag = INVALID_HANDLE_VALUE;
static unsigned gSeq;

static unsigned long Rd(unsigned long long a)
{
    S10EDIAG_READ_IN in = { a, 4, S10EDIAG_READ_MMIO };
    unsigned long    v = 0xFFFFFFFFu;
    DWORD            got;
    DeviceIoControl(gDiag, IOCTL_S10EDIAG_READ_PHYS, &in, sizeof(in), &v, 4, &got, NULL);
    return v;
}

static void Wr(unsigned long long a, unsigned long v)
{
    S10EDIAG_WRITE32_IN in = { a, v, 0 };
    DWORD               got;
    DeviceIoControl(gDiag, IOCTL_S10EDIAG_WRITE32, &in, sizeof(in), NULL, 0, &got, NULL);
}

static int SramOk(unsigned long long off, unsigned long long len)
{
    return (off & 3) == 0 && len <= SRAM_SIZE && off <= SRAM_SIZE - len;
}

int AcpmOpen(void)
{
    if (gDiag == INVALID_HANDLE_VALUE)
        gDiag = CreateFileW(S10EDIAG_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    return gDiag != INVALID_HANDLE_VALUE;
}

int AcpmFindChannel(unsigned id, ACPM_CHANNEL *ch, int verbose)
{
    unsigned long long init = SRAM_BASE + INITDATA, entry;
    unsigned long      table = Rd(init + FW_IPC_CHANNELS), apmax = Rd(init + FW_IPC_AP_MAX), i;

    if (apmax == 0 || apmax > 64 || !SramOk(table, (unsigned long long)apmax * CH_STRIDE))
    {
        printf("ACPM channel table implausible (ipc_channels 0x%lx ap_max %lu)\n", table, apmax);
        return 0;
    }
    for (i = 0; i < apmax; i++)
    {
        unsigned long txr, txf, txb, rxr, rxf, rxb, type;
        entry = SRAM_BASE + table + (unsigned long long)i * CH_STRIDE;
        if (Rd(entry + CH_ID) != id)
            continue;
        type = Rd(entry + CH_TYPE);
        ch->Id = id;
        ch->Len = Rd(entry + CH_Q_LEN);
        ch->Size = Rd(entry + CH_Q_ELEM_SIZE);
        txr = Rd(entry + CH_RX_REAR);  txf = Rd(entry + CH_RX_FRONT); txb = Rd(entry + CH_RX_BASE);
        rxr = Rd(entry + CH_TX_REAR);  rxf = Rd(entry + CH_TX_FRONT); rxb = Rd(entry + CH_TX_BASE);
        if (verbose)
            printf("ACPM ch %u (idx %lu) type %lu ap_poll %lu q_len %lu elem %lu\n", id, i, type, Rd(entry + CH_AP_POLL),
                   ch->Len, ch->Size);
        if (type != TYPE_QUEUE || ch->Len < 2 || ch->Len > 256 || ch->Size < 16 || ch->Size > 64 || (ch->Size & 3) ||
            !SramOk(txr, 4) || !SramOk(txf, 4) || !SramOk(rxr, 4) || !SramOk(rxf, 4) ||
            !SramOk(txb, (unsigned long long)ch->Size * ch->Len) || !SramOk(rxb, (unsigned long long)ch->Size * ch->Len))
        {
            printf("ACPM channel %u layout implausible - not sending\n", id);
            return 0;
        }
        ch->TxRear = SRAM_BASE + txr; ch->TxFront = SRAM_BASE + txf; ch->TxBase = SRAM_BASE + txb;
        ch->RxRear = SRAM_BASE + rxr; ch->RxFront = SRAM_BASE + rxf; ch->RxBase = SRAM_BASE + rxb;
        return 1;
    }
    printf("ACPM channel %u not found\n", id);
    return 0;
}

static int CheckResponse(ACPM_CHANNEL *ch, unsigned seq, unsigned long *reply, int *broken)
{
    unsigned long front = Rd(ch->RxFront), rear = Rd(ch->RxRear), i, steps, w;

    if (front >= ch->Len || rear >= ch->Len)
    {
        *broken = 1;
        return 0;
    }
    for (i = rear, steps = 0; i != front && steps < ch->Len; steps++)
    {
        unsigned long long elem = ch->RxBase + (unsigned long long)ch->Size * i;
        if (((Rd(elem) >> SEQ_SHIFT) & SEQ_MASK) == seq)
        {
            for (w = 0; w < 4; w++)
                reply[w] = Rd(elem + w * 4);
            if (i != rear)
            {
                unsigned long long re = ch->RxBase + (unsigned long long)ch->Size * rear;
                for (w = 0; w < ch->Size / 4; w++)
                    Wr(elem + w * 4, Rd(re + w * 4));
            }
            rear = (rear + 1) % ch->Len;
            Wr(ch->RxRear, rear);
            if (rear == Rd(ch->RxFront))
            {
                Wr(MBOX_BASE + MBOX_INTCR1, 1u << ch->Id);
                if (rear != Rd(ch->RxFront))
                    Wr(MBOX_BASE + MBOX_INTGR1, 1u << ch->Id);
            }
            return 1;
        }
        i = (i + 1) % ch->Len;
    }
    return 0;
}

static unsigned NextSeq(ACPM_CHANNEL *ch)
{
    unsigned long long pending = 0;
    unsigned long      front = Rd(ch->RxFront), rear = Rd(ch->RxRear), steps, t;

    if (front < ch->Len && rear < ch->Len)
        for (steps = 0; rear != front && steps < ch->Len; steps++)
        {
            pending |= 1ULL << ((Rd(ch->RxBase + (unsigned long long)ch->Size * rear) >> SEQ_SHIFT) & SEQ_MASK);
            rear = (rear + 1) % ch->Len;
        }
    for (t = 0; t < 63; t++)
    {
        if (++gSeq >= 64)
            gSeq = 1;
        if (!(pending & (1ULL << gSeq)))
            break;
    }
    return gSeq;
}

int AcpmSend(ACPM_CHANNEL *ch, unsigned long cmd[4])
{
    unsigned long front = Rd(ch->TxFront), rear = Rd(ch->TxRear), next, w;
    unsigned      seq;
    ULONGLONG     t0;
    int           broken = 0;

    if (front >= ch->Len || rear >= ch->Len)
    {
        printf("ACPM tx indices out of range (front %lu rear %lu len %lu)\n", front, rear, ch->Len);
        return 0;
    }
    next = (front + 1) % ch->Len;
    t0 = GetTickCount64();
    while (next == Rd(ch->TxRear))
        if (GetTickCount64() - t0 > 15)
        {
            printf("ACPM tx queue full\n");
            return 0;
        }
    seq = NextSeq(ch);
    cmd[0] |= (seq & SEQ_MASK) << SEQ_SHIFT;
    for (w = 0; w < ch->Size / 4; w++)
        Wr(ch->TxBase + (unsigned long long)ch->Size * front + w * 4, w < 4 ? cmd[w] : 0);
    Wr(ch->TxFront, next);
    Wr(MBOX_BASE + MBOX_INTGR0, (1u << ch->Id) << 16);
    t0 = GetTickCount64();
    for (;;)
    {
        if ((Rd(MBOX_BASE + MBOX_INTSR1) & (1u << ch->Id)) && CheckResponse(ch, seq, cmd, &broken))
            return 1;
        if (broken)
        {
            printf("ACPM rx indices out of range\n");
            return 0;
        }
        if (GetTickCount64() - t0 > REPLY_WAIT_MS)
        {
            if (CheckResponse(ch, seq, cmd, &broken))
                return 1;
            printf("ACPM no reply (seq %u)\n", seq);
            return 0;
        }
    }
}

int AcpmFreqReq(unsigned domain, unsigned khz, unsigned long reply[4])
{
    ACPM_CHANNEL ch;
    if (!AcpmFindChannel(0, &ch, 0))
        return 0;
    reply[0] = domain;
    reply[1] = khz;
    reply[2] = 0;   // FREQ_REQ (acpm_dvfs.h:19)
    reply[3] = 0;
    return AcpmSend(&ch, reply);
}

#ifndef S10EACPM_NO_MAIN
int main(int argc, char **argv)
{
    unsigned long r[4];
    ACPM_CHANNEL  ch;

    if (!AcpmOpen())
    {
        printf("cannot open S10eDiag (%lu) - run elevated\n", GetLastError());
        return 1;
    }
    if (argc >= 4 && strcmp(argv[1], "freq") == 0)
    {
        unsigned d = (unsigned)strtoul(argv[2], NULL, 0), k = (unsigned)strtoul(argv[3], NULL, 0);
        if (!AcpmFreqReq(d, k, r))
            return 2;
        printf("FREQ_REQ domain %u %u kHz -> reply %08lx %08lx %08lx %08lx\n", d, k, r[0], r[1], r[2], r[3]);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "chan") == 0)
        return AcpmFindChannel(argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 0u, &ch, 1) ? 0 : 2;
    printf("usage: s10eacpm freq <domain> <khz> | s10eacpm chan [id]\n");
    return 2;
}
#endif
