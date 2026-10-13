// SPDX-License-Identifier: GPL-2.0
/*
 * MaliG76Wddm's PRAM trace: every DDI entry and exit, the registry trace values and a 1 Hz heartbeat, written as
 * text lines into reserved RAM that the firmware saves as \WINPRAM.BIN on the next boot - so a boot that hangs
 * (the full-adapter black screen, notes/gpu-full-adapter-hang.md) or bugchecks still leaves its last DDIs behind.
 * The registry trace cannot do that: its writes were lost by the force reset after the hang.
 *
 * Where: BEYOND0LTE_WINPRAM_BASE 0x8FE00000 (firmware/Platform/Beyond0LtePkg/Library/Beyond0LteFbSerialPortLib/
 * Beyond0LteBootLog.h:36, reserved RAM, a firmware choice) + 0x4000, 16 KiB. The firmware saves the first
 * 0x10000 bytes (BEYOND0LTE_WINPRAM_DUMP, :38) and zeroes them after the save (Beyond0LteBootLogDxe.c:557-592).
 * Inside those 64 KiB, +0x0000..0x3FFF is Exynos9820Ufs's ring, +0x8000..0x8FFF its early marks, +0xA000..0xDFFF
 * S10eDiag's mark ring, +0xF000 the progress words; +0x4000..0x7FFF is used by nothing else (inventory in the
 * notes). A slice of our own instead of S10eDiag's ring: its lock is private to S10eDiag, and an atomic on an index
 * word in a non-cacheable mapping is not something ARMv8 guarantees.
 *
 * Layout: 256 lines of 64 bytes, the S10eDiag line format "<uptime ms, 10 digits> <text>" padded with spaces and
 * ended by '\n', so `tr -c '[:print:]\n' '\n'` on the dump reads it:
 *   line 0       header: "MALIG76T", then ULONG next rolling line (+8), next first-call line (+12), sequence (+16)
 *   line 1       heartbeat, rewritten in place every second
 *   lines 2-111  first calls: the first entry and exit of every DDI, in call order (kept when the ring wraps)
 *   lines 112-255 rolling: every DDI entry/exit and registry trace value; the header says which line is next
 * Each text starts with a 6-hex-digit sequence number, so the rolling part sorts after a wrap.
 *
 * Any IRQL: an interlocked increment on our own (cached) counters picks the line, then plain byte stores into the
 * non-cached mapping - no lock, no allocation, no formatting library (the ISR, SubmitCommand at DISPATCH and
 * SetVidPnSourceAddress up to DIRQL all write here). Concurrent writers get different lines.
 */
#include "MaliG76Wddm.h"

#define PRAM_PA             0x8FE04000ULL   /* BEYOND0LTE_WINPRAM_BASE + 0x4000 (free slice, see above) */
#define PRAM_SIZE           0x4000u
#define PRAM_LINE           64u
#define PRAM_LINES          (PRAM_SIZE / PRAM_LINE)
#define PRAM_HB_LINE        1u
#define PRAM_FIRST_START    2u
#define PRAM_FIRST_COUNT    109u
#define PRAM_STATS_LINE     (PRAM_FIRST_START + PRAM_FIRST_COUNT)   /* g40: counters, rewritten in place */
#define PRAM_ROLL_START     (PRAM_STATS_LINE + 1u)
#define PRAM_ROLL_COUNT     (PRAM_LINES - PRAM_ROLL_START)

C_ASSERT(PRAM_ROLL_START + PRAM_ROLL_COUNT == PRAM_LINES);
C_ASSERT(PRAM_ROLL_COUNT >= 64);

static const CHAR PramMagic[8] = { 'M', 'A', 'L', 'I', 'G', '7', '6', 'T' };
static volatile UCHAR *Pram;
static volatile LONG PramSeq, PramFirst, PramRoll;

/* A line under construction in ordinary memory, copied out byte by byte. */
typedef struct {
    CHAR B[PRAM_LINE];
    ULONG N;
} PRAM_TEXT;

static void Ch(PRAM_TEXT *T, CHAR C)
{
    if (T->N < PRAM_LINE - 1)
        T->B[T->N++] = (C >= 0x20 && C < 0x7F) ? C : '.';
}

/* Room left on the line after keeping Keep characters for what follows (a " <8 hex digits>" value). */
static ULONG Room(const PRAM_TEXT *T, ULONG Keep)
{
    return (T->N + Keep < PRAM_LINE - 1) ? PRAM_LINE - 1 - Keep - T->N : 0;
}

static void Str(PRAM_TEXT *T, const CHAR *S, ULONG Keep)
{
    for (ULONG n = Room(T, Keep); S != NULL && *S != '\0' && n > 0; n--)
        Ch(T, *S++);
}

static void WStr(PRAM_TEXT *T, PCWSTR S, ULONG Keep)
{
    for (ULONG n = Room(T, Keep); S != NULL && *S != L'\0' && n > 0; n--, S++)
        Ch(T, (*S < 0x80) ? (CHAR)*S : '?');
}

static void Hex(PRAM_TEXT *T, ULONG V, ULONG Digits)
{
    static const CHAR H[] = "0123456789abcdef";
    for (ULONG i = Digits; i-- > 0;)
        Ch(T, H[(V >> (i * 4)) & 0xF]);
}

/* "<uptime ms, 10 digits> " and, unless Seq is ~0u, "<sequence, 6 hex digits> ". */
static void Begin(PRAM_TEXT *T, ULONG Seq)
{
    ULONG64 ms = KeQueryInterruptTime() / 10000ULL;
    T->N = 0;
    for (ULONG i = 0; i < 10; i++) {
        T->B[9 - i] = (CHAR)('0' + ms % 10);
        ms /= 10;
    }
    T->N = 10;
    Ch(T, ' ');
    if (Seq != ~0u) {
        Hex(T, Seq, 6);
        Ch(T, ' ');
    }
}

static void Store(ULONG Line, PRAM_TEXT *T)
{
    volatile UCHAR *p = Pram + (SIZE_T)Line * PRAM_LINE;
    ULONG i;
    for (i = 0; i < T->N; i++)
        p[i] = (UCHAR)T->B[i];
    for (; i < PRAM_LINE - 1; i++)
        p[i] = ' ';
    p[PRAM_LINE - 1] = '\n';
}

static void Header(void)
{
    *(volatile ULONG *)(Pram + 8) = PRAM_ROLL_START + (ULONG)PramRoll % PRAM_ROLL_COUNT;
    *(volatile ULONG *)(Pram + 12) = PRAM_FIRST_START + min((ULONG)PramFirst, PRAM_FIRST_COUNT);
    *(volatile ULONG *)(Pram + 16) = (ULONG)PramSeq;
}

/* Write T (Begin already called with Seq) to a first-call line if First and one is left, else to the ring. */
static void Emit(PRAM_TEXT *T, BOOLEAN First)
{
    ULONG line;
    if (First) {
        LONG n = InterlockedIncrement(&PramFirst) - 1;
        if ((ULONG)n < PRAM_FIRST_COUNT) {
            Store(PRAM_FIRST_START + (ULONG)n, T);
            Header();
            KeMemoryBarrier();
            return;
        }
        InterlockedExchange(&PramFirst, (LONG)PRAM_FIRST_COUNT);   /* no overflow after 2^31 calls */
    }
    line = PRAM_ROLL_START + ((ULONG)InterlockedIncrement(&PramRoll) - 1) % PRAM_ROLL_COUNT;
    Store(line, T);
    Header();
    KeMemoryBarrier();
}

/* DriverEntry (PASSIVE). Enable 0 leaves the trace off (Parameters\PramTrace = 0). A previous load of this boot
 * (or a boot whose dump did not zero the slice) is continued, not erased: a LOAD line separates them. */
void MwPramInit(ULONG Enable, ULONG Mode, ULONG Build)
{
    PHYSICAL_ADDRESS pa;
    PRAM_TEXT t;
    BOOLEAN ours = TRUE;

    if (!Enable || Pram != NULL)
        return;
    pa.QuadPart = (LONGLONG)PRAM_PA;
    Pram = (volatile UCHAR *)MmMapIoSpaceEx(pa, PRAM_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (Pram == NULL)
        return;
    for (ULONG i = 0; i < 8; i++)
        ours = ours && Pram[i] == (UCHAR)PramMagic[i];
    if (ours) {
        ULONG roll = *(volatile ULONG *)(Pram + 8), first = *(volatile ULONG *)(Pram + 12);
        PramSeq = (LONG)*(volatile ULONG *)(Pram + 16);
        PramRoll = (roll >= PRAM_ROLL_START && roll < PRAM_LINES) ? (LONG)(roll - PRAM_ROLL_START) : 0;
        PramFirst = (first >= PRAM_FIRST_START && first <= PRAM_FIRST_START + PRAM_FIRST_COUNT)
                        ? (LONG)(first - PRAM_FIRST_START) : 0;
    } else {
        for (ULONG i = 0; i < PRAM_SIZE; i++)
            Pram[i] = (i % PRAM_LINE == PRAM_LINE - 1) ? '\n' : ' ';
        for (ULONG i = 0; i < 8; i++)
            Pram[i] = (UCHAR)PramMagic[i];
        PramSeq = PramFirst = PramRoll = 0;
    }
    Header();
    Begin(&t, (ULONG)InterlockedIncrement(&PramSeq));
    Str(&t, "LOAD mode ", 0);
    Hex(&t, Mode, 1);
    Str(&t, " build ", 0);
    Hex(&t, Build, 8);
    Str(&t, ours ? " cont" : " new", 0);
    Emit(&t, TRUE);
}

void MwPramFree(void)
{
    volatile UCHAR *p = Pram;
    if (p == NULL)
        return;
    MwPramText(TRUE, "UNLOAD", NULL, 0, FALSE);
    Pram = NULL;
    KeMemoryBarrier();
    MmUnmapIoSpace((PVOID)p, PRAM_SIZE);
}

/* One line: "<Tag><Name> <Value 8 hex>" (Value only if HasValue). Tag is ASCII, Name wide (may be NULL). */
void MwPramText(BOOLEAN First, const CHAR *Tag, PCWSTR Name, ULONG Value, BOOLEAN HasValue)
{
    PRAM_TEXT t;
    if (Pram == NULL)
        return;
    Begin(&t, (ULONG)InterlockedIncrement(&PramSeq));
    /* A long name is cut, never the value after it (e.g. Order_GetStandardAllocationDriverData). */
    Str(&t, Tag, HasValue ? 9 : 0);
    WStr(&t, Name, HasValue ? 9 : 0);
    if (HasValue) {
        Ch(&t, ' ');
        Hex(&t, Value, 8);
    }
    Emit(&t, First);
}

/* A DDI entry ('>') or exit ('<' + status). First: the DDI's first call (also kept in the first-call lines). */
void MwPramDdi(BOOLEAN First, BOOLEAN Exit, const CHAR *Name, NTSTATUS Status)
{
    PRAM_TEXT t;
    if (Pram == NULL)
        return;
    Begin(&t, (ULONG)InterlockedIncrement(&PramSeq));
    Ch(&t, Exit ? '<' : '>');
    Str(&t, Name, Exit ? 9 : 0);    /* StopDeviceAndReleasePostDisplayOwnership is 40 characters */
    if (Exit) {
        Ch(&t, ' ');
        Hex(&t, (ULONG)Status, 8);
    }
    Emit(&t, First);
}

/* The heartbeat line (rewritten in place, no sequence number): "HB i<isr> d<dpc> v<vsync> s<submits> p<presents>
 * f<fence>", 6 hex digits each (low 24 bits) - 61 characters. If its uptime stops advancing before the phone was
 * reset, the 1 Hz timer DPC stopped running. */
/* The stats line (rewritten in place like the heartbeat): "ST o<opens> u<refused std types mask> l<latches>
 * k<present skips> b<present blts>", 6 hex digits each. g40full: per-event lines into the first-call region or the
 * ring kept getting lost (the first-call region persists across boots and fills; the ring wraps in ~20 s). */
void MwPramStats(ULONG Opens, ULONG Refused, ULONG Latches, ULONG Skips, ULONG Blts)
{
    PRAM_TEXT t;
    if (Pram == NULL)
        return;
    Begin(&t, ~0u);
    Str(&t, "ST o", 0);
    Hex(&t, Opens, 6);
    Str(&t, " u", 0);
    Hex(&t, Refused, 6);
    Str(&t, " l", 0);
    Hex(&t, Latches, 6);
    Str(&t, " k", 0);
    Hex(&t, Skips, 6);
    Str(&t, " b", 0);
    Hex(&t, Blts, 6);
    Store(PRAM_STATS_LINE, &t);
    KeMemoryBarrier();
}

void MwPramHeartbeat(ULONG Isr, ULONG Dpc, ULONG Vsync, ULONG Submits, ULONG Presents, ULONG Fence)
{
    PRAM_TEXT t;
    if (Pram == NULL)
        return;
    Begin(&t, ~0u);
    Str(&t, "HB i", 0);
    Hex(&t, Isr, 6);
    Str(&t, " d", 0);
    Hex(&t, Dpc, 6);
    Str(&t, " v", 0);
    Hex(&t, Vsync, 6);
    Str(&t, " s", 0);
    Hex(&t, Submits, 6);
    Str(&t, " p", 0);
    Hex(&t, Presents, 6);
    Str(&t, " f", 0);
    Hex(&t, Fence, 6);
    Store(PRAM_HB_LINE, &t);
    KeMemoryBarrier();
}
