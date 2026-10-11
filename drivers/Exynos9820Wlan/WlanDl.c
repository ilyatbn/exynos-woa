/*  WlanDl.c - S10e BCM4375 WLAN, milestone 2: firmware download + ARM CR4 start (KMDF, no NDIS).

    The sequence is Samsung's bcmdhd PCIe "download state" flow for a CR4 dongle, re-implemented from the
    facts listed (file:line) in notes/m12-wifi-m2.md; constants live in Exynos9820Wlan.h. Order:

      1. map BAR0 (register window) + BAR2 (4 MB TCM window); refuse if bus mastering is on
      2. BAR0 window -> ChipCommon, cross-check chipid, walk the EROM (dhd ai_scan rules)
      3. PCIE2 BAR1-window sizing WAR (dhd + fmac both do it)
      4. optional: module id from the OTP CIS (dhd dhd_parse_board_information_bcm) -> nvram file
      5. read firmware + nvram from \SystemRoot\System32\drivers\bcm4375\, nvram processed like fmac
      6. BAR1 window = 0, ARM CR4 reset+halt, TCM size from the CR4 bank registers
      7. zero the last RAM word, firmware -> RAM base, nvram (+ length token) -> top of RAM, random seed
         below it, reset instruction -> backplane 0; every write read back
      8. ARM out of reset (halt cleared), poll the last RAM word for the pcie_shared pointer (5 s)
      9. read pcie_shared + the firmware console; on timeout / bad pointer the ARM is halted again
     10. firmware up: hand the mapped chip to the milestone-3 callback (WlanMsg.c), if one was given

    This file never sets PCI bus master enable, interrupts or rings. WlanMsg.c (milestone 3) does, inside
    the callback, and clears bus mastering + halts the ARM again before it returns.
*/

#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include "WlanDl.h"

#define BCM_MAX_CORES   40

typedef struct _BCM_CORE {
    ULONG   Id;
    ULONG   Mfg;
    ULONG   Rev;
    ULONG   Base;
    ULONG   Wrap[3];
} BCM_CORE, *PBCM_CORE;

typedef struct _BCM_CHIP {
    volatile UCHAR *Cfg;        /* EP config space (CFG0 window) */
    volatile UCHAR *Bar0;       /* first 4 KB = backplane window */
    volatile UCHAR *Tcm;        /* BAR2, backplane 0 .. 4 MB with BAR1_WIN = 0 */
    ULONG           CurWin;
    ULONG           WinFaults;
    PWLAN_REPORT    R;
    BCM_CORE        Core[BCM_MAX_CORES];
    ULONG           NumCores;
    ULONG           EromPtr;    /* scan cursor (backplane address) */
    ULONG           EromLimit;
    BOOLEAN         EromBad;
} BCM_CHIP, *PBCM_CHIP;

/* ---------------------------------------------------------------- config + backplane access */

static ULONG
CfgRd(_In_ PBCM_CHIP C, _In_ ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(C->Cfg + Off));
}

static VOID
CfgWr(_In_ PBCM_CHIP C, _In_ ULONG Off, _In_ ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(C->Cfg + Off), Val);
}

/* fmac pcie.c brcmf_pcie_buscore_prep_addr + select_core: write, read back, rewrite once. */
static BOOLEAN
BpSetWin(_Inout_ PBCM_CHIP C, _In_ ULONG Win)
{
    ULONG V;

    if (C->CurWin == Win) {
        return TRUE;
    }
    CfgWr(C, BCM_CFG_BAR0_WIN, Win);
    V = CfgRd(C, BCM_CFG_BAR0_WIN);
    if (V != Win) {
        CfgWr(C, BCM_CFG_BAR0_WIN, Win);
        V = CfgRd(C, BCM_CFG_BAR0_WIN);
    }
    if (V != Win) {
        if (C->WinFaults++ < 4) {
            ReportLine(C->R, "[dl] BAR0 window 0x%08lX did not stick (reads 0x%08lX)\r\n", Win, V);
        }
        C->CurWin = 0xFFFFFFFFUL;
        return FALSE;
    }
    C->CurWin = Win;
    return TRUE;
}

static ULONG
BpRd(_Inout_ PBCM_CHIP C, _In_ ULONG Addr)
{
    if (!BpSetWin(C, Addr & ~(BCM_BAR0_WIN_SIZE - 1))) {
        return 0xFFFFFFFFUL;
    }
    return READ_REGISTER_ULONG((volatile ULONG *)(C->Bar0 + (Addr & (BCM_BAR0_WIN_SIZE - 1))));
}

static VOID
BpWr(_Inout_ PBCM_CHIP C, _In_ ULONG Addr, _In_ ULONG Val)
{
    if (!BpSetWin(C, Addr & ~(BCM_BAR0_WIN_SIZE - 1))) {
        return;
    }
    WRITE_REGISTER_ULONG((volatile ULONG *)(C->Bar0 + (Addr & (BCM_BAR0_WIN_SIZE - 1))), Val);
}

/* TCM through BAR2: offset == backplane address (BAR1_WIN = 0). Callers bound-check against the
   4 MB window before calling. */
static ULONG
TcmRd32(_In_ PBCM_CHIP C, _In_ ULONG Addr)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(C->Tcm + Addr));
}

static VOID
TcmWr32(_In_ PBCM_CHIP C, _In_ ULONG Addr, _In_ ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(C->Tcm + Addr), Val);
}

/* Write Len bytes at Addr (4-byte aligned), 32-bit accesses then byte accesses for the tail
   (dhd dhdpcie_bus_membytes). Returns the number of 32-bit/byte units that read back wrong. */
static ULONG
TcmWriteVerify(_In_ PBCM_CHIP C, _In_ ULONG Addr, _In_reads_bytes_(Len) const UCHAR *Data, _In_ ULONG Len)
{
    ULONG I;
    ULONG Bad = 0;
    ULONG Words = Len / 4;

    for (I = 0; I < Words; I++) {
        ULONG V = (ULONG)Data[4 * I] | ((ULONG)Data[4 * I + 1] << 8) | ((ULONG)Data[4 * I + 2] << 16) |
                  ((ULONG)Data[4 * I + 3] << 24);
        TcmWr32(C, Addr + 4 * I, V);
    }
    for (I = 4 * Words; I < Len; I++) {
        WRITE_REGISTER_UCHAR((volatile UCHAR *)(C->Tcm + Addr + I), Data[I]);
    }
    KeMemoryBarrier();
    for (I = 0; I < Words; I++) {
        ULONG V = (ULONG)Data[4 * I] | ((ULONG)Data[4 * I + 1] << 8) | ((ULONG)Data[4 * I + 2] << 16) |
                  ((ULONG)Data[4 * I + 3] << 24);
        if (TcmRd32(C, Addr + 4 * I) != V) {
            Bad++;
        }
    }
    for (I = 4 * Words; I < Len; I++) {
        if (READ_REGISTER_UCHAR((volatile UCHAR *)(C->Tcm + Addr + I)) != Data[I]) {
            Bad++;
        }
    }
    return Bad;
}

/* ---------------------------------------------------------------- EROM scan (dhd ai_scan) */

/* dhd get_erom_ent: Mask 0 = next word as is; else skip invalid entries until (ent & Mask) == Match,
   stopping at the END marker or the scan limit. */
static ULONG
EromEnt(_Inout_ PBCM_CHIP C, _In_ ULONG Mask, _In_ ULONG Match)
{
    ULONG Ent;

    for (;;) {
        if (C->EromPtr >= C->EromLimit) {
            C->EromBad = TRUE;
            return ER_END | ER_VALID;
        }
        Ent = BpRd(C, C->EromPtr);
        C->EromPtr += 4;
        if (Mask == 0) {
            return Ent;
        }
        if ((Ent & ER_VALID) == 0) {
            continue;
        }
        if (Ent == (ER_END | ER_VALID)) {
            return Ent;
        }
        if ((Ent & Mask) == Match) {
            return Ent;
        }
    }
}

/* dhd get_asd: one address descriptor of slave port Sp and type St, or 0 (pushed back) if the next
   valid entry is not one. */
static ULONG
EromAsd(_Inout_ PBCM_CHIP C, _In_ ULONG Sp, _In_ ULONG St, _Out_ PULONG AddrL, _Out_ PULONG SizeL)
{
    ULONG Asd;
    ULONG Sz;

    *AddrL = 0;
    *SizeL = 0;
    Asd = EromEnt(C, ER_VALID, ER_VALID);
    if (((Asd & ER_TAG1) != ER_ADD) || (((Asd & AD_SP_MASK) >> AD_SP_SHIFT) != Sp) || ((Asd & AD_ST_MASK) != St)) {
        C->EromPtr -= 4;   /* not ours: push it back */
        return 0;
    }
    *AddrL = Asd & AD_ADDR_MASK;
    if ((Asd & AD_AG32) != 0) {
        (VOID)EromEnt(C, 0, 0);                    /* address high: always 0 on this 32-bit backplane */
    }
    Sz = Asd & AD_SZ_MASK;
    if (Sz == AD_SZ_SZD) {
        ULONG Szd = EromEnt(C, 0, 0);

        *SizeL = Szd & SD_SZ_MASK;
        if ((Szd & SD_SG32) != 0) {
            (VOID)EromEnt(C, 0, 0);
        }
    } else {
        *SizeL = AD_SZ_BASE << (Sz >> AD_SZ_SHIFT);
    }
    return Asd;
}

static BOOLEAN
WrapperOnlyCore(_In_ ULONG Cid)
{
    return Cid == CORE_ID_PMU || Cid == CORE_ID_GCI || Cid == CORE_ID_SR || Cid == CORE_ID_HUB ||
           Cid == CORE_ID_OOBR || Cid == CORE_ID_NS_CCB;
}

static VOID
EromScan(_Inout_ PBCM_CHIP C, _In_ ULONG EromPtr)
{
    C->EromPtr   = EromPtr;
    C->EromLimit = EromPtr + ER_REMAPCONTROL;
    C->NumCores  = 0;

    while (C->NumCores < BCM_MAX_CORES && !C->EromBad) {
        ULONG Cia, Cib, Cid, Mfg, Rev, Nmw, Nsw, Nmp, Nsp, I, Asd, AddrL, SizeL;
        BOOLEAN Bridge = FALSE;
        PBCM_CORE K;

        Cia = EromEnt(C, ER_TAG, ER_CI);
        if (Cia == (ER_END | ER_VALID)) {
            break;
        }
        Cib = EromEnt(C, 0, 0);
        if ((Cib & ER_TAG) != ER_CI) {
            ReportLine(C->R, "[dl] EROM: CIB 0x%08lX not a component - scan stopped\r\n", Cib);
            C->EromBad = TRUE;
            break;
        }
        Cid = (Cia & CIA_CID_MASK) >> CIA_CID_SHIFT;
        Mfg = (Cia & CIA_MFG_MASK) >> CIA_MFG_SHIFT;
        Rev = (Cib & CIB_REV_MASK) >> CIB_REV_SHIFT;
        Nmw = (Cib & CIB_NMW_MASK) >> CIB_NMW_SHIFT;
        Nsw = (Cib & CIB_NSW_MASK) >> CIB_NSW_SHIFT;
        Nmp = (Cib & CIB_NMP_MASK) >> CIB_NMP_SHIFT;
        Nsp = (Cib & CIB_NSP_MASK) >> CIB_NSP_SHIFT;

        /* aiutils.c:234-245 */
        if ((Mfg == MFG_ID_ARM && Cid == CORE_ID_DEF_AI_COMP) || Nsp == 0) {
            continue;
        }
        /* aiutils.c:247-263: wrapper-less components are only kept for a few ids */
        if ((Nmw + Nsw) == 0 && !WrapperOnlyCore(Cid)) {
            continue;
        }

        K = &C->Core[C->NumCores];
        RtlZeroMemory(K, sizeof(*K));
        K->Id  = Cid;
        K->Mfg = Mfg;
        K->Rev = Rev;

        for (I = 0; I < Nmp; I++) {
            ULONG Mpd = EromEnt(C, ER_VALID, ER_VALID);

            if ((Mpd & ER_TAG) != ER_MP) {
                ReportLine(C->R, "[dl] EROM: core 0x%03lX master port 0x%08lX bad\r\n", Cid, Mpd);
                C->EromBad = TRUE;
                break;
            }
        }
        if (C->EromBad) {
            break;
        }

        /* first slave address = core base; a bridge instead marks a core we skip */
        Asd = EromAsd(C, 0, AD_ST_SLAVE, &AddrL, &SizeL);
        if (Asd == 0) {
            Asd = EromAsd(C, 0, AD_ST_BRIDGE, &AddrL, &SizeL);
            if (Asd != 0) {
                Bridge = TRUE;
            } else {
                ReportLine(C->R, "[dl] EROM: core 0x%03lX has no slave/bridge address\r\n", Cid);
                C->EromBad = TRUE;
                break;
            }
        }
        K->Base = AddrL;

        /* consume the remaining slave/bridge descriptors of every port (aiutils.c:300-340) */
        for (I = 0; I < Nsp; I++) {
            ULONG A2, S2;

            while (EromAsd(C, I, AD_ST_SLAVE, &A2, &S2) != 0 || EromAsd(C, I, AD_ST_BRIDGE, &A2, &S2) != 0) {
            }
        }

        /* master wrappers, then slave wrappers (aiutils.c:345-405) */
        for (I = 0; I < Nmw; I++) {
            if (EromAsd(C, I, AD_ST_MWRAP, &AddrL, &SizeL) == 0) {
                ReportLine(C->R, "[dl] EROM: core 0x%03lX master wrapper %lu missing\r\n", Cid, I);
                C->EromBad = TRUE;
                break;
            }
            if (I < 3) {
                K->Wrap[I] = AddrL;
            }
        }
        for (I = 0; I < Nsw && !C->EromBad; I++) {
            ULONG Sp = I + ((Nsp > 1) ? 1 : 0);

            if (EromAsd(C, Sp, AD_ST_SWRAP, &AddrL, &SizeL) == 0 &&
                EromAsd(C, I, AD_ST_SWRAP, &AddrL, &SizeL) == 0) {
                ReportLine(C->R, "[dl] EROM: core 0x%03lX slave wrapper %lu missing\r\n", Cid, I);
                C->EromBad = TRUE;
                break;
            }
            if (Nmw == 0 && I < 3) {
                K->Wrap[I] = AddrL;
            }
        }
        if (C->EromBad) {
            break;
        }
        if (!Bridge) {
            C->NumCores++;
        }
    }
}

static PBCM_CORE
FindCore(_In_ PBCM_CHIP C, _In_ ULONG Id)
{
    ULONG I;

    for (I = 0; I < C->NumCores; I++) {
        if (C->Core[I].Id == Id) {
            return &C->Core[I];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- core reset (dhd _ai_core_reset) */

static VOID
WrapReset(_Inout_ PBCM_CHIP C, _In_ ULONG Wrap, _In_ ULONG Bits, _In_ ULONG ResetBits)
{
    ULONG I;

    for (I = 0; I < 300 && (BpRd(C, Wrap + AI_RESETSTATUS) & 0xFF) != 0; I++) {
        KeStallExecutionProcessor(1);
    }
    BpWr(C, Wrap + AI_RESETCTRL, AIRC_RESET);
    KeStallExecutionProcessor(10);
    BpWr(C, Wrap + AI_IOCTRL, Bits | ResetBits | SICF_FGC | SICF_CLOCK_EN);
    (VOID)BpRd(C, Wrap + AI_IOCTRL);
    for (I = 0; I < 10; I++) {
        BpWr(C, Wrap + AI_RESETCTRL, 0);
        KeStallExecutionProcessor(1);
        if ((BpRd(C, Wrap + AI_RESETCTRL) & AIRC_RESET) == 0) {
            break;
        }
    }
    BpWr(C, Wrap + AI_IOCTRL, Bits | SICF_CLOCK_EN);
    (VOID)BpRd(C, Wrap + AI_IOCTRL);
    KeStallExecutionProcessor(1);
}

/* dhd ai_core_reset: secondary wrappers first, then the primary one. */
static VOID
CoreReset(_Inout_ PBCM_CHIP C, _In_ const ULONG Wrap[3], _In_ ULONG Bits, _In_ ULONG ResetBits)
{
    if (Wrap[2] != 0) {
        WrapReset(C, Wrap[2], Bits, ResetBits);
    }
    if (Wrap[1] != 0) {
        WrapReset(C, Wrap[1], Bits, ResetBits);
    }
    if (Wrap[0] != 0) {
        WrapReset(C, Wrap[0], Bits, ResetBits);
    }
}

/* ---------------------------------------------------------------- blobs */

NTSTATUS
WlanLoadBlob(_In_z_ PCWSTR Name, _Outptr_result_bytebuffer_(*Len) PUCHAR *Buf, _Out_ PULONG Len, _In_ ULONG Max)
{
    WCHAR                     Path[160];
    UNICODE_STRING            U;
    OBJECT_ATTRIBUTES         Oa;
    IO_STATUS_BLOCK           Iosb;
    HANDLE                    File;
    FILE_STANDARD_INFORMATION Info;
    NTSTATUS                  Status;
    PUCHAR                    B;

    *Buf = NULL;
    *Len = 0;
    Status = RtlStringCchPrintfW(Path, ARRAYSIZE(Path), L"%s%s", WLAN_BLOB_DIR, Name);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    RtlInitUnicodeString(&U, Path);
    InitializeObjectAttributes(&Oa, &U, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateFile(&File, GENERIC_READ | SYNCHRONIZE, &Oa, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                          FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = ZwQueryInformationFile(File, &Iosb, &Info, sizeof(Info), FileStandardInformation);
    if (NT_SUCCESS(Status) && (Info.EndOfFile.QuadPart == 0 || Info.EndOfFile.QuadPart > Max)) {
        Status = STATUS_FILE_TOO_LARGE;
    }
    if (NT_SUCCESS(Status)) {
        B = (PUCHAR)ExAllocatePool2(POOL_FLAG_PAGED, (SIZE_T)Info.EndOfFile.QuadPart + 8, 'lDlW');
        if (B == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        } else {
            Status = ZwReadFile(File, NULL, NULL, NULL, &Iosb, B, (ULONG)Info.EndOfFile.QuadPart, NULL, NULL);
            if (NT_SUCCESS(Status) && Iosb.Information == (ULONG_PTR)Info.EndOfFile.QuadPart) {
                *Buf = B;
                *Len = (ULONG)Info.EndOfFile.QuadPart;
            } else {
                ExFreePoolWithTag(B, 'lDlW');
                if (NT_SUCCESS(Status)) {
                    Status = STATUS_END_OF_FILE;
                }
            }
        }
    }
    ZwClose(File);
    return Status;
}

static BOOLEAN
IsNvramChar(_In_ UCHAR Ch)
{
    return Ch != '#' && Ch >= 0x20 && Ch < 0x7F;   /* fmac is_nvram_char */
}

/* fmac brcmf_fw_nvram_strip (IDLE/KEY/VALUE/COMMENT): entries "key=value\0", '#' starts a comment, a value
   ends at the first non-printable or '#'; boardrev=0xff appended if absent; padded to roundup(len+1, 4);
   length token (~n << 16 | n, n = padded/4) appended. Out must hold In's length + 32. */
static ULONG
NvramStrip(_In_reads_bytes_(Len) const UCHAR *In, _In_ ULONG Len, _Out_writes_bytes_(OutMax) PUCHAR Out,
           _In_ ULONG OutMax, _Out_ PULONG Token)
{
    enum { ST_IDLE, ST_KEY, ST_VALUE, ST_COMMENT } St = ST_IDLE;
    ULONG   Pos, Entry = 0, O = 0, Padded, N;
    BOOLEAN BoardRev = FALSE;

    *Token = 0;
    for (Pos = 0; Pos < Len; Pos++) {
        UCHAR Ch = In[Pos];

        switch (St) {
        case ST_IDLE:
            if (Ch == '#') {
                St = ST_COMMENT;
            } else if (IsNvramChar(Ch) && Ch != ' ') {
                Entry = Pos;
                St    = ST_KEY;
            }
            break;
        case ST_KEY:
            if (Ch == '=') {
                St = ST_VALUE;
            } else if (!IsNvramChar(Ch) || Ch == ' ') {
                St = (Ch == '\n') ? ST_IDLE : ST_COMMENT;   /* '=' expected: invalid key, skip the line */
            }
            break;
        case ST_VALUE:
            if (!IsNvramChar(Ch)) {
                ULONG EntLen = Pos - Entry;

                if (O + EntLen + 1 < OutMax) {
                    if (EntLen >= 9 && RtlCompareMemory(In + Entry, "boardrev=", 9) == 9) {
                        BoardRev = TRUE;
                    }
                    RtlCopyMemory(Out + O, In + Entry, EntLen);
                    O += EntLen;
                    Out[O++] = 0;
                }
                St = (Ch == '#') ? ST_COMMENT : ST_IDLE;
            }
            break;
        default:   /* ST_COMMENT */
            if (Ch == '\n') {
                St = ST_IDLE;
            }
            break;
        }
    }
    if (!BoardRev && O + sizeof(NVRAM_DEFAULT_BOARDREV) < OutMax) {
        RtlCopyMemory(Out + O, NVRAM_DEFAULT_BOARDREV, sizeof(NVRAM_DEFAULT_BOARDREV));   /* incl. NUL */
        O += sizeof(NVRAM_DEFAULT_BOARDREV);
    }
    Padded = (O + 1 + 3) & ~3UL;
    if (Padded + 4 > OutMax) {
        return 0;
    }
    while (O < Padded) {
        Out[O++] = 0;
    }
    N      = Padded / 4;
    *Token = ((~N) << 16) | (N & 0xFFFF);
    Out[O++] = (UCHAR)(*Token);
    Out[O++] = (UCHAR)(*Token >> 8);
    Out[O++] = (UCHAR)(*Token >> 16);
    Out[O++] = (UCHAR)(*Token >> 24);
    return O;
}

/* dhd dhd_parse_board_information_bcm: CIS tuples {0x80, len, tag, data...} in the OTP shadow; tag 0x81 =
   module vendor id (2 bytes). Returns (vid0 << 8) | vid1, 0 if absent. */
static ULONG
OtpVendor(_Inout_ PBCM_CHIP C)
{
    UCHAR Cis[CIS_TUPLE_END - CIS_TUPLE_START + 1 + 3];
    ULONG I, Len = CIS_TUPLE_END - CIS_TUPLE_START + 1;

    for (I = 0; I < Len; I += 4) {
        ULONG W = BpRd(C, CIS_TUPLE_START + I);

        Cis[I] = (UCHAR)W;
        Cis[I + 1] = (UCHAR)(W >> 8);
        Cis[I + 2] = (UCHAR)(W >> 16);
        Cis[I + 3] = (UCHAR)(W >> 24);
    }
    ReportLine(C->R, "[dl] OTP CIS 0x%08lX: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
               CIS_TUPLE_START, Cis[0], Cis[1], Cis[2], Cis[3], Cis[4], Cis[5], Cis[6], Cis[7], Cis[8], Cis[9],
               Cis[10], Cis[11]);
    for (I = 0; I + 4 < Len;) {
        UCHAR TLen;

        if (Cis[I] != CIS_TUPLE_TAG_START) {
            I++;
            continue;
        }
        TLen = Cis[I + 1];
        if (TLen >= 3 && Cis[I + 2] == CIS_TUPLE_TAG_VENDOR && I + 4 < Len) {
            return ((ULONG)Cis[I + 3] << 8) | Cis[I + 4];
        }
        I += 2 + (TLen ? TLen : 1);
    }
    return 0;
}

/* ---------------------------------------------------------------- the run */

static VOID
RandomFill(_Out_writes_bytes_(Len) PUCHAR Buf, _In_ ULONG Len)
{
    /* xorshift32 seeded from the performance counter and interrupt time: the seed only has to differ per
       boot (dhd uses get_random_bytes; the firmware mixes it into its own RNG). */
    ULONG Seed = (ULONG)KeQueryPerformanceCounter(NULL).QuadPart ^ (ULONG)KeQueryInterruptTime() ^ 0x9E3779B9UL;
    ULONG I;

    for (I = 0; I < Len; I += 4) {
        ULONG V;

        Seed ^= Seed << 13;
        Seed ^= Seed >> 17;
        Seed ^= Seed << 5;
        V = Seed;

        Buf[I] = (UCHAR)V;
        if (I + 1 < Len) Buf[I + 1] = (UCHAR)(V >> 8);
        if (I + 2 < Len) Buf[I + 2] = (UCHAR)(V >> 16);
        if (I + 3 < Len) Buf[I + 3] = (UCHAR)(V >> 24);
    }
}

/* Dump the firmware console ring. *FromIdx == 0xFFFFFFFF: the last CONS_DUMP_MAX bytes; else only what the
   firmware wrote since that index (capped at CONS_DUMP_MAX). *FromIdx is set to the current index. */
static VOID
ConsoleDump(_Inout_ PBCM_CHIP C, _In_ ULONG RamBase, _In_ ULONG RamEnd, _In_ ULONG ConsAddr, _Inout_ PULONG FromIdx)
{
    ULONG Buf, Size, Idx, Start, I, N;
    CHAR  Line[132];
    ULONG L = 0;

    if (ConsAddr < RamBase || ConsAddr + 0x14 > RamEnd) {
        ReportLine(C->R, "[dl] console 0x%08lX outside RAM\r\n", ConsAddr);
        return;
    }
    Buf  = TcmRd32(C, ConsAddr + CONS_LOG_BUF);
    Size = TcmRd32(C, ConsAddr + CONS_LOG_BUF_SIZE);
    Idx  = TcmRd32(C, ConsAddr + CONS_LOG_IDX);
    ReportLine(C->R, "[dl] console buf 0x%08lX size %lu idx %lu\r\n", Buf, Size, Idx);
    if (Buf < RamBase || Size == 0 || Size > 0x10000 || Buf + Size > RamEnd || Idx >= Size) {
        return;
    }
    if (*FromIdx < Size) {
        N = (Idx + Size - *FromIdx) % Size;   /* bytes written since the last dump (a full wrap reads as 0) */
        if (N > CONS_DUMP_MAX) {
            N = CONS_DUMP_MAX;
        }
    } else {
        N = (Size < CONS_DUMP_MAX) ? Size : CONS_DUMP_MAX;
    }
    *FromIdx = Idx;
    Start = (Idx + Size - N) % Size;   /* the last N bytes of the ring, oldest first */
    ReportLine(C->R, "[dl] ---- firmware console (last %lu bytes) ----\r\n", N);
    for (I = 0; I < N; I++) {
        UCHAR Ch = READ_REGISTER_UCHAR((volatile UCHAR *)(C->Tcm + Buf + ((Start + I) % Size)));

        if (Ch == '\n' || L >= sizeof(Line) - 4) {
            Line[L] = 0;
            if (L != 0) {
                ReportLine(C->R, "  %s\r\n", Line);
            }
            L = 0;
            if (Ch == '\n') {
                continue;
            }
        }
        if (Ch >= 0x20 && Ch < 0x7F) {
            Line[L++] = (CHAR)Ch;
        }
    }
    if (L != 0) {
        Line[L] = 0;
        ReportLine(C->R, "  %s\r\n", Line);
    }
    ReportLine(C->R, "[dl] ---- end console ----\r\n");
}

/* ---------------------------------------------------------------- milestone-3 bus interface */

_Use_decl_annotations_
ULONG
WlanBusBpRd(PWLAN_BUS Bus, ULONG Addr)
{
    return BpRd((PBCM_CHIP)Bus->Chip, Addr);
}

_Use_decl_annotations_
VOID
WlanBusBpWr(PWLAN_BUS Bus, ULONG Addr, ULONG Val)
{
    BpWr((PBCM_CHIP)Bus->Chip, Addr, Val);
}

_Use_decl_annotations_
VOID
WlanBusArmHalt(PWLAN_BUS Bus)
{
    CoreReset((PBCM_CHIP)Bus->Chip, Bus->Cr4Wrap, SICF_CPUHALT, SICF_CPUHALT);
    Bus->ArmHalted = TRUE;
}

_Use_decl_annotations_
VOID
WlanBusConsoleDump(PWLAN_REPORT R, PWLAN_BUS Bus)
{
    PBCM_CHIP C = (PBCM_CHIP)Bus->Chip;

    C->R = R;
    ConsoleDump(C, Bus->RamBase, Bus->RamEnd, Bus->ConsoleAddr, &Bus->ConsoleIdx);
}

_Use_decl_annotations_
VOID
WlanDlRun(PWLAN_REPORT R, volatile UCHAR *Cfg, ULONG Bar0Lo, ULONG Bar2Lo, ULONG Command,
          const WLAN_DL_CONFIG *Config, PWLAN_DL_RESULT Out, WLAN_FW_UP_FN *FwUp, PVOID FwUpCtx)
{
    BCM_CHIP        *C = NULL;
    PHYSICAL_ADDRESS Pa;
    PBCM_CORE        Cr4, Pcie, Gci, Pmu;
    ULONG            ChipId, EromPtr, Cap, Nb, I, RamSize = 0, RamBase = BCM4375_RAM_BASE, RamEnd, Bad;
    PUCHAR           Fw = NULL, NvRaw = NULL, Nv = NULL;
    ULONG            FwLen = 0, NvRawLen = 0, NvLen = 0, Token = 0, VarAddr, Shared = 0, Ms;
    PCWSTR           NvName, FwName;
    UCHAR            Seed[BCM_ENTROPY_HOST_NBYTES];
    NTSTATUS         Status;
    ULONG            ConsIdx = 0xFFFFFFFFUL;
    WLAN_BUS         Bus;

    RtlZeroMemory(Out, sizeof(*Out));
    Out->DlResult = DL_BACKPLANE;
    ReportLine(R, "---- milestone 2: firmware download ----\r\n");

    if (!Config->Download) {
        ReportLine(R, "[dl] skipped (Parameters\\Download = 0)\r\n");
        Out->DlResult = DL_SKIPPED;
        return;
    }
    if ((Command & EP_CFG_COMMAND_BME) != 0) {
        ReportLine(R, "DL %u: bus mastering is ON (command 0x%08lX) - refusing (milestone 2 is no-DMA)\r\n",
                   DL_BME_SET, Command);
        Out->DlResult = DL_BME_SET;
        return;
    }
    if (Bar2Lo < WLAN_MEM_WINDOW_BASE || (ULONGLONG)Bar2Lo + WLAN_BAR2_SIZE > WLAN_MEM_WINDOW_BASE + WLAN_MEM_WINDOW_SIZE ||
        (Bar2Lo & (WLAN_BAR2_SIZE - 1)) != 0) {
        ReportLine(R, "DL %u: BAR2 0x%08lX not a 4 MB window inside the MEM window\r\n", DL_BACKPLANE, Bar2Lo);
        return;
    }

    C = (BCM_CHIP *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*C), 'cDlW');
    if (C == NULL) {
        return;
    }
    C->Cfg    = Cfg;
    C->R      = R;
    C->CurWin = 0xFFFFFFFFUL;
    Pa.QuadPart = Bar0Lo;
    C->Bar0 = (volatile UCHAR *)MmMapIoSpaceEx(Pa, WLAN_BAR0_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    Pa.QuadPart = Bar2Lo;
    C->Tcm  = (volatile UCHAR *)MmMapIoSpaceEx(Pa, WLAN_BAR2_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (C->Bar0 == NULL || C->Tcm == NULL) {
        ReportLine(R, "DL %u: mapping BAR0/BAR2 failed\r\n", DL_BACKPLANE);
        goto Done;
    }

    /* 2. ChipCommon + EROM */
    ChipId = BpRd(C, SI_ENUM_BASE);
    ReportLine(R, "[dl] backplane chipid 0x%08lX (type %lu)\r\n", ChipId, ChipId >> CC_CHIPID_TYPE_SHIFT);
    if ((ChipId & CC_CHIPID_MASK) != BCM4375_CHIP_ID || (ChipId >> CC_CHIPID_TYPE_SHIFT) != 1) {
        ReportLine(R, "DL %u: backplane chipid is not a BCM4375 AI chip\r\n", DL_BACKPLANE);
        goto Done;
    }
    EromPtr = BpRd(C, SI_ENUM_BASE + CC_EROMPTR_REG);
    EromScan(C, EromPtr);
    ReportLine(R, "[dl] EROM @0x%08lX: %lu cores%s\r\n", EromPtr, C->NumCores, C->EromBad ? " (scan stopped early)" : "");
    for (I = 0; I < C->NumCores; I++) {
        ReportLine(R, "  core 0x%03lX rev %3lu base 0x%08lX wrap 0x%08lX 0x%08lX\r\n", C->Core[I].Id, C->Core[I].Rev,
                   C->Core[I].Base, C->Core[I].Wrap[0], C->Core[I].Wrap[1]);
    }
    ReportWriteFile(R);
    Cr4  = FindCore(C, CORE_ID_ARMCR4);
    Pcie = FindCore(C, CORE_ID_PCIE2);
    Gci  = FindCore(C, CORE_ID_GCI);
    Pmu  = FindCore(C, CORE_ID_PMU);
    if (Cr4 == NULL || Cr4->Wrap[0] == 0 || Pcie == NULL) {
        ReportLine(R, "DL %u: no ARM CR4 (with wrapper) / PCIE2 core in the EROM\r\n", DL_BACKPLANE);
        goto Done;
    }
    Out->Cr4Rev  = Cr4->Rev;
    Out->PcieRev = Pcie->Rev;
    RtlCopyMemory(Out->Cr4Wrap, Cr4->Wrap, sizeof(Out->Cr4Wrap));

    /* 3. PCIE2 BAR1 window sizing WAR */
    BpWr(C, Pcie->Base + PCIE2_CONFIGADDR, PCIE2_BAR1_SIZE_WAR_REG);
    I = BpRd(C, Pcie->Base + PCIE2_CONFIGDATA);
    BpWr(C, Pcie->Base + PCIE2_CONFIGDATA, I);

    /* 4. module id from OTP */
    if (Config->OtpProbe && Gci != NULL && Pmu != NULL && Gci->Base == OTP_GCI_CORE_BASE && Pmu->Base == OTP_PMU_CORE_BASE) {
        BpWr(C, OTP_CTRL1_ADDR, OTP_CTRL1_VALUE);
        BpWr(C, PMU_MIN_RES_MASK_ADDR, PMU_MIN_RES_MASK_VALUE);
        KeStallExecutionProcessor(1000);
        Out->OtpVid = OtpVendor(C);
    } else {
        ReportLine(R, "[dl] OTP probe not done (OtpProbe %lu, GCI 0x%08lX, PMU 0x%08lX)\r\n", Config->OtpProbe,
                   Gci ? Gci->Base : 0, Pmu ? Pmu->Base : 0);
    }
    if (Config->NvramFile[0] != 0) {
        NvName = Config->NvramFile;
    } else if (Out->OtpVid == 0x4322) {
        NvName = WLAN_NVRAM_MURATA_1RH_ES43;
    } else if (Out->OtpVid == 0x5133 || Out->OtpVid == 0x5333) {
        NvName = WLAN_NVRAM_SEMCO_CS01;
    } else {
        NvName = WLAN_NVRAM_DEFAULT;
    }
    FwName = (Config->FirmwareFile[0] != 0) ? Config->FirmwareFile : WLAN_FW_DEFAULT;
    (VOID)RtlStringCchCopyW(Out->NvramUsed, ARRAYSIZE(Out->NvramUsed), NvName);
    ReportLine(R, "[dl] module vid 0x%04lX -> nvram %ls, firmware %ls\r\n", Out->OtpVid, NvName, FwName);

    /* 5. blobs */
    Out->DlResult = DL_BLOB;
    Status = WlanLoadBlob(FwName, &Fw, &FwLen, WLAN_FW_MAX_SIZE);
    if (!NT_SUCCESS(Status)) {
        ReportLine(R, "DL %u: firmware %ls: 0x%08lX\r\n", DL_BLOB, FwName, Status);
        goto Done;
    }
    Status = WlanLoadBlob(NvName, &NvRaw, &NvRawLen, NVRAM_MAX_SIZE);
    if (!NT_SUCCESS(Status)) {
        ReportLine(R, "DL %u: nvram %ls: 0x%08lX\r\n", DL_BLOB, NvName, Status);
        goto Done;
    }
    Nv = (PUCHAR)ExAllocatePool2(POOL_FLAG_PAGED, NvRawLen + 64, 'nDlW');
    NvLen = (Nv != NULL) ? NvramStrip(NvRaw, NvRawLen, Nv, NvRawLen + 64, &Token) : 0;
    if (NvLen == 0) {
        ReportLine(R, "DL %u: nvram processing failed\r\n", DL_BLOB);
        goto Done;
    }
    Out->FwSize = FwLen;
    Out->NvramLen = NvLen;
    Out->NvramToken = Token;
    ReportLine(R, "[dl] firmware %lu B (reset instr 0x%08lX), nvram %lu B raw -> %lu B, token 0x%08lX\r\n",
               FwLen, *(ULONG UNALIGNED *)Fw, NvRawLen, NvLen, Token);

    /* 6. halt the ARM, size the TCM */
    Out->DlResult = DL_SIZE;
    CfgWr(C, BCM_CFG_BAR1_WIN, 0);
    CoreReset(C, Cr4->Wrap, SICF_CPUHALT, SICF_CPUHALT);
    Cap = BpRd(C, Cr4->Base + ARMCR4_CAP);
    Nb  = (Cap & ARMCR4_TCBANB_MASK) + ((Cap & ARMCR4_TCBBNB_MASK) >> ARMCR4_TCBBNB_SHIFT);
    for (I = 0; I < Nb && I < 64; I++) {
        ULONG Info;

        BpWr(C, Cr4->Base + ARMCR4_BANKIDX, I);
        Info = BpRd(C, Cr4->Base + ARMCR4_BANKINFO);
        RamSize += ((Info & ARMCR4_BSZ_MASK) + 1) * (((Info & ARMCR4_BUNITSZ_MASK) != 0) ? ARMCR4_BSZ_1K : ARMCR4_BSZ_8K);
    }
    Out->RamSize = RamSize;
    RamEnd = RamBase + RamSize;
    ReportLine(R, "[dl] ARM CR4 rev %lu halted; cap 0x%08lX, %lu banks -> RAM 0x%08lX..0x%08lX (%lu KB)\r\n",
               Cr4->Rev, Cap, Nb, RamBase, RamEnd, RamSize / 1024);
    if (RamSize < FwLen + NvLen + BCM_ENTROPY_HOST_NBYTES + 0x1000 || RamEnd > WLAN_BAR2_SIZE) {
        ReportLine(R, "DL %u: RAM does not fit (needs fw+nvram+seed inside the 4 MB BAR2 window)\r\n", DL_SIZE);
        goto Halt;
    }

    /* 7. image */
    Out->DlResult = DL_VERIFY;
    TcmWr32(C, RamEnd - 4, 0);
    Bad = TcmWriteVerify(C, RamBase, Fw, FwLen);
    VarAddr = RamEnd - NvLen;
    Bad += TcmWriteVerify(C, VarAddr, Nv, NvLen);
    RandomFill(Seed, sizeof(Seed));
    TcmWr32(C, VarAddr - 8, BCM_ENTROPY_HOST_NBYTES);
    TcmWr32(C, VarAddr - 4, BCM_NVRAM_RNG_SIGNATURE);
    Bad += TcmWriteVerify(C, VarAddr - 8 - BCM_ENTROPY_HOST_NBYTES, Seed, sizeof(Seed));
    TcmWr32(C, 0, *(ULONG UNALIGNED *)Fw);
    if (TcmRd32(C, 0) != *(ULONG UNALIGNED *)Fw) {
        Bad++;
    }
    ReportLine(R, "[dl] image written: fw @0x%08lX, nvram @0x%08lX, seed @0x%08lX, last word 0x%08lX; %lu mismatches\r\n",
               RamBase, VarAddr, VarAddr - 8 - BCM_ENTROPY_HOST_NBYTES, TcmRd32(C, RamEnd - 4), Bad);
    ReportWriteFile(R);
    if (Bad != 0) {
        ReportLine(R, "DL %u: TCM read-back mismatch - ARM left halted\r\n", DL_VERIFY);
        goto Halt;
    }

    /* 8. run */
    Out->DlResult = DL_TIMEOUT;
    CoreReset(C, Cr4->Wrap, 0, 0);
    Out->ArmReleased = TRUE;
    ReportLine(R, "[dl] ARM released, waiting for pcie_shared (%u ms)\r\n", FW_UP_TIMEOUT_MS);
    ReportWriteFile(R);
    for (Ms = 0; Ms < FW_UP_TIMEOUT_MS; Ms += FW_UP_POLL_MS) {
        LARGE_INTEGER Delay;

        Shared = TcmRd32(C, RamEnd - 4);
        if (Shared != 0 && Shared != Token) {
            break;
        }
        Delay.QuadPart = -10000LL * FW_UP_POLL_MS;
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
    if (Shared == 0 || Shared == Token) {
        ReportLine(R, "DL %u: no pcie_shared after %u ms (last word 0x%08lX)\r\n", DL_TIMEOUT, FW_UP_TIMEOUT_MS, Shared);
        goto Halt;
    }

    /* 9. pcie_shared + console */
    Out->SharedAddr = Shared;
    ReportLine(R, "[dl] pcie_shared at 0x%08lX after %lu ms\r\n", Shared, Ms);
    if (Shared < RamBase || Shared + 0x34 > RamEnd) {
        ReportLine(R, "DL %u: pcie_shared pointer outside RAM\r\n", DL_BAD_SHARED);
        Out->DlResult = DL_BAD_SHARED;
        goto Halt;
    }
    Out->SharedFlags = TcmRd32(C, Shared + SH_FLAGS);
    Out->ConsoleAddr = TcmRd32(C, Shared + SH_CONSOLE_ADDR);
    ReportLine(R, "[dl] shared flags 0x%08lX (version %lu%s%s) console 0x%08lX trap 0x%08lX assert line %lu fwid 0x%08lX\r\n",
               Out->SharedFlags, Out->SharedFlags & PCIE_SHARED_VERSION_MASK,
               (Out->SharedFlags & PCIE_SHARED_TRAP) ? ", TRAP" : "", (Out->SharedFlags & PCIE_SHARED_ASSERT) ? ", ASSERT" : "",
               Out->ConsoleAddr, TcmRd32(C, Shared + SH_TRAP_ADDR), TcmRd32(C, Shared + SH_ASSERT_LINE),
               TcmRd32(C, Shared + SH_FWID));
    ConsoleDump(C, RamBase, RamEnd, Out->ConsoleAddr, &ConsIdx);
    if ((Out->SharedFlags & PCIE_SHARED_VERSION_MASK) < PCIE_SHARED_MIN_VERSION ||
        (Out->SharedFlags & PCIE_SHARED_VERSION_MASK) > PCIE_SHARED_MAX_VERSION ||
        (Out->SharedFlags & (PCIE_SHARED_TRAP | PCIE_SHARED_ASSERT)) != 0) {
        ReportLine(R, "DL %u: pcie_shared version/flags not usable\r\n", DL_BAD_SHARED);
        Out->DlResult = DL_BAD_SHARED;
        goto Done;   /* firmware ran: keep it running so the console stays readable */
    }
    Out->DlResult = DL_OK;
    ReportLine(R, "DL 0: BCM4375 firmware is running (pcie_shared v%lu)\r\n", Out->SharedFlags & PCIE_SHARED_VERSION_MASK);
    ReportWriteFile(R);

    /* Milestone 3 (WlanMsg.c) runs while BAR0/BAR2 are still mapped; it leaves BME off and, unless
       Parameters\Msgbuf = 0, the ARM halted. */
    if (FwUp != NULL) {
        RtlZeroMemory(&Bus, sizeof(Bus));
        Bus.Chip         = C;
        Bus.Cfg          = Cfg;
        Bus.Tcm          = C->Tcm;
        Bus.RamBase      = RamBase;
        Bus.RamEnd       = RamEnd;
        Bus.SharedAddr   = Shared;
        Bus.SharedFlags  = Out->SharedFlags;
        Bus.ConsoleAddr  = Out->ConsoleAddr;
        Bus.ConsoleIdx   = ConsIdx;
        Bus.PcieCoreBase = Pcie->Base;
        Bus.PcieRev      = Pcie->Rev;
        Bus.Command      = Command;
        RtlCopyMemory(Bus.Cr4Wrap, Cr4->Wrap, sizeof(Bus.Cr4Wrap));
        FwUp(R, &Bus, FwUpCtx);
        C->R = R;
        if (Bus.ArmHalted) {
            Out->ArmReleased = FALSE;
        }
    }
    goto Done;

Halt:
    CoreReset(C, Cr4->Wrap, SICF_CPUHALT, SICF_CPUHALT);
    Out->ArmReleased = FALSE;
    ReportLine(R, "[dl] ARM halted again\r\n");

Done:
    if (C != NULL) {
        if (C->Bar0 != NULL) {
            MmUnmapIoSpace((PVOID)C->Bar0, WLAN_BAR0_SIZE);
        }
        if (C->Tcm != NULL) {
            MmUnmapIoSpace((PVOID)C->Tcm, WLAN_BAR2_SIZE);
        }
        ExFreePoolWithTag(C, 'cDlW');
    }
    if (Fw != NULL) {
        ExFreePoolWithTag(Fw, 'lDlW');
    }
    if (NvRaw != NULL) {
        ExFreePoolWithTag(NvRaw, 'lDlW');
    }
    if (Nv != NULL) {
        ExFreePoolWithTag(Nv, 'nDlW');
    }
    ReportWriteFile(R);
}
