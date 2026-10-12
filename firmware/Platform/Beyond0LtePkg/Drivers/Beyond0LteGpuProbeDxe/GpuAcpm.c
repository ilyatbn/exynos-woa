/** @file
  M16 G9: ACPM DVFS FREQ_REQ for the G3D (notes/gpu-m16-g3don.md H1-H3).

  The IPC client below is a COPY of the device-validated client in
  Beyond0LteTouchPowerDxe.c (FindChannel / CheckResponse / NextSeq / AcpmSend; FREQ_REQ on channel 0
  validated in m15 runs 31/32), with three changes only:
    - log prefix "GpuProbe: ACPM";
    - delays via TimerLib MicroSecondDelay (this driver's depex has no Metronome, so gBS->Stall is not
      guaranteed to wait - same reason as Beyond0LteParking.c);
    - no PMIC (MFD) helpers, no build-info dump.
  Kernel protocol citations (LineageOS android_kernel_samsung_exynos9820 lineage-22.2,
  drivers/soc/samsung/acpm/) are the ones in the TouchPower copy; they are kept on each step.

  NOT VALIDATED ON HARDWARE for the G3D domain (stage Guided).
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Platform/Exynos9820.h>

#include "GpuAcpm.h"

#define ACPM_MBOX_INTGR0   0x0008u   // acpm_ipc.h:80  AP -> APM interrupt generate
#define ACPM_MBOX_INTCR1   0x0020u   // acpm_ipc.h:86  APM -> AP pending clear
#define ACPM_MBOX_INTGR1   0x001Cu   // acpm_ipc.h:85  APM -> AP generate (check_response re-raise)
#define ACPM_MBOX_INTSR1   0x0028u   // acpm_ipc.h:88  APM -> AP status (polled, acpm_ipc.c:573)

#define ACPM_FW_IPC_CHANNELS  0x08u   // framework.h:47  u32 offset of struct ipc_channel[]
#define ACPM_FW_IPC_AP_MAX    0x18u   // framework.h:52  channel_init: num_channels (acpm_ipc.c:715)

#define ACPM_CH_STRIDE        0x48u
#define ACPM_CH_ID            0x00u   // framework.h:124
#define ACPM_CH_TYPE          0x0Cu   // framework.h:127
#define ACPM_CH_RX_REAR       0x10u   // framework.h:98   -> AP tx_ch.rear  (acpm_ipc.c:736)
#define ACPM_CH_RX_FRONT      0x14u   // framework.h:99   -> AP tx_ch.front (acpm_ipc.c:737)
#define ACPM_CH_RX_BASE       0x18u   // framework.h:100  -> AP tx_ch.base  (acpm_ipc.c:738)
#define ACPM_CH_TX_REAR       0x28u   // framework.h:104  -> AP rx_ch.rear  (acpm_ipc.c:730)
#define ACPM_CH_TX_FRONT      0x2Cu   // framework.h:105  -> AP rx_ch.front (acpm_ipc.c:731)
#define ACPM_CH_TX_BASE       0x30u   // framework.h:106  -> AP rx_ch.base  (acpm_ipc.c:732)
#define ACPM_CH_Q_LEN         0x34u   // framework.h:107
#define ACPM_CH_Q_ELEM_SIZE   0x38u   // framework.h:108
#define ACPM_CH_AP_POLL       0x44u   // framework.h:129
#define ACPM_TYPE_QUEUE       1u      // framework.h:240

#define ACPM_IPC_SEQ_SHIFT    16u     // acpm_ipc_ctrl.h:24
#define ACPM_IPC_SEQ_MASK     0x3Fu   // acpm_ipc.c:301/548

#define ACPM_POLL_STEP_US         10u       // acpm_ipc.c:595 udelay(10)
#define ACPM_QUEUE_FULL_WAIT_US   15000u    // acpm_ipc.h:95 IPC_TIMEOUT
#define ACPM_REPLY_WAIT_US        100000u   // TouchPower's flat 100 ms (kernel: 15 ms + retries, then WDT reset)

//
// VERIFIED live DT clock-controller@1a240000 acpm-ipc-channel = 0 (notes/m15-perf.md; TouchPower's
// ACPM_DVFS_IPC_CHANNEL). acpm_dvfs's channel 5 is the MIF-notify BUFFER channel, not the request path (H1).
//
#define ACPM_DVFS_IPC_CHANNEL  0u
#define ACPM_DVFS_FREQ_REQ     0u     // acpm_dvfs.h:19

typedef struct {
  UINT32  Id;
  UINT32  Len;
  UINT32  Size;
  UINTN   TxRear;
  UINTN   TxFront;
  UINTN   TxBase;
  UINTN   RxRear;
  UINTN   RxFront;
  UINTN   RxBase;
} GPU_ACPM_CHANNEL;

STATIC UINT32  mSeqNum;

STATIC
UINTN
SramAddr (
  IN UINT32  Offset
  )
{
  return (UINTN)EXYNOS_ACPM_SRAM_BASE + Offset;
}

STATIC
BOOLEAN
SramOffsetOk (
  IN UINT64  Offset,
  IN UINT64  Length
  )
{
  return ((Offset & 3) == 0) && (Length <= EXYNOS_ACPM_SRAM_SIZE) &&
         (Offset <= EXYNOS_ACPM_SRAM_SIZE - Length);
}

STATIC
UINT32
MboxRead (
  IN UINT32  Reg
  )
{
  return MmioRead32 ((UINTN)EXYNOS_ACPM_IPC_MBOX_BASE + Reg);
}

STATIC
VOID
MboxWrite (
  IN UINT32  Reg,
  IN UINT32  Value
  )
{
  MmioWrite32 ((UINTN)EXYNOS_ACPM_IPC_MBOX_BASE + Reg, Value);
}

/**
  channel_init (acpm_ipc.c:709-755) + acpm_ipc_request_channel (acpm_ipc.c:223-261), by id.
**/
STATIC
EFI_STATUS
FindChannel (
  IN  UINT32            WantedId,
  OUT GPU_ACPM_CHANNEL  *Ch
  )
{
  UINTN   Init;
  UINT32  TableOff;
  UINT32  ApMax;
  UINT32  Index;
  UINTN   Entry;
  UINT32  Type;
  UINT32  TxRearOff, TxFrontOff, TxBaseOff, RxRearOff, RxFrontOff, RxBaseOff;

  Init     = SramAddr (EXYNOS_ACPM_INITDATA_OFFSET);   // acpm_ipc.c:808
  TableOff = MmioRead32 (Init + ACPM_FW_IPC_CHANNELS);
  ApMax    = MmioRead32 (Init + ACPM_FW_IPC_AP_MAX);
  if ((ApMax == 0) || (ApMax > 64) || !SramOffsetOk (TableOff, (UINT64)ApMax * ACPM_CH_STRIDE)) {
    DEBUG ((DEBUG_ERROR, "GpuProbe: ACPM channel table implausible (ipc_channels 0x%x ap_max %u)\n", TableOff, ApMax));
    return EFI_NOT_FOUND;
  }

  for (Index = 0; Index < ApMax; Index++) {
    Entry = SramAddr (TableOff) + (UINTN)Index * ACPM_CH_STRIDE;
    if (MmioRead32 (Entry + ACPM_CH_ID) != WantedId) {
      continue;
    }

    Type       = MmioRead32 (Entry + ACPM_CH_TYPE);
    Ch->Id     = WantedId;
    Ch->Len    = MmioRead32 (Entry + ACPM_CH_Q_LEN);
    Ch->Size   = MmioRead32 (Entry + ACPM_CH_Q_ELEM_SIZE);
    TxRearOff  = MmioRead32 (Entry + ACPM_CH_RX_REAR);
    TxFrontOff = MmioRead32 (Entry + ACPM_CH_RX_FRONT);
    TxBaseOff  = MmioRead32 (Entry + ACPM_CH_RX_BASE);
    RxRearOff  = MmioRead32 (Entry + ACPM_CH_TX_REAR);
    RxFrontOff = MmioRead32 (Entry + ACPM_CH_TX_FRONT);
    RxBaseOff  = MmioRead32 (Entry + ACPM_CH_TX_BASE);

    DEBUG ((DEBUG_ERROR, "GpuProbe: ACPM ch id %u (idx %u) type %u ap_poll %u q_len %u elem %u\n",
            WantedId, Index, Type, MmioRead32 (Entry + ACPM_CH_AP_POLL), Ch->Len, Ch->Size));

    if ((Type != ACPM_TYPE_QUEUE) || (Ch->Len < 2) || (Ch->Len > 256) ||
        (Ch->Size < 16) || (Ch->Size > 64) || ((Ch->Size & 3) != 0) ||
        !SramOffsetOk (TxRearOff, 4) || !SramOffsetOk (TxFrontOff, 4) ||
        !SramOffsetOk (RxRearOff, 4) || !SramOffsetOk (RxFrontOff, 4) ||
        !SramOffsetOk (TxBaseOff, (UINT64)Ch->Size * Ch->Len) ||
        !SramOffsetOk (RxBaseOff, (UINT64)Ch->Size * Ch->Len))
    {
      DEBUG ((DEBUG_ERROR, "GpuProbe: ACPM channel %u layout implausible - not sending\n", WantedId));
      return EFI_UNSUPPORTED;
    }

    Ch->TxRear  = SramAddr (TxRearOff);
    Ch->TxFront = SramAddr (TxFrontOff);
    Ch->TxBase  = SramAddr (TxBaseOff);
    Ch->RxRear  = SramAddr (RxRearOff);
    Ch->RxFront = SramAddr (RxFrontOff);
    Ch->RxBase  = SramAddr (RxBaseOff);
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_ERROR, "GpuProbe: ACPM channel id %u not in the %u AP channels\n", WantedId, ApMax));
  return EFI_NOT_FOUND;
}

/**
  check_response (acpm_ipc.c:282-345).
**/
STATIC
BOOLEAN
CheckResponse (
  IN  GPU_ACPM_CHANNEL  *Ch,
  IN  UINT32            Seq,
  OUT UINT32            *Reply,
  OUT BOOLEAN           *Broken
  )
{
  UINT32  Front;
  UINT32  Rear;
  UINT32  I;
  UINT32  Steps;
  UINT32  W;

  Front = MmioRead32 (Ch->RxFront);
  Rear  = MmioRead32 (Ch->RxRear);
  if ((Front >= Ch->Len) || (Rear >= Ch->Len)) {
    *Broken = TRUE;
    return FALSE;
  }

  for (I = Rear, Steps = 0; (I != Front) && (Steps < Ch->Len); Steps++) {
    UINTN  Elem = Ch->RxBase + (UINTN)Ch->Size * I;

    if (((MmioRead32 (Elem) >> ACPM_IPC_SEQ_SHIFT) & ACPM_IPC_SEQ_MASK) == Seq) {
      for (W = 0; W < 4; W++) {
        Reply[W] = MmioRead32 (Elem + W * 4);      // Size >= 16 checked in FindChannel
      }

      if (I != Rear) {                              // acpm_ipc.c:314-317
        UINTN  RearElem = Ch->RxBase + (UINTN)Ch->Size * Rear;

        for (W = 0; W < Ch->Size / 4; W++) {
          MmioWrite32 (Elem + W * 4, MmioRead32 (RearElem + W * 4));
        }
      }

      Rear = (Rear + 1) % Ch->Len;                  // acpm_ipc.c:323-334
      MmioWrite32 (Ch->RxRear, Rear);
      Front = MmioRead32 (Ch->RxFront);
      if (Rear == Front) {
        MboxWrite (ACPM_MBOX_INTCR1, 1u << Ch->Id);
        if (Rear != MmioRead32 (Ch->RxFront)) {
          MboxWrite (ACPM_MBOX_INTGR1, 1u << Ch->Id);
        }
      }

      return TRUE;
    }

    I = (I + 1) % Ch->Len;
  }

  return FALSE;
}

/**
  Next seq 1..63 (acpm_ipc.c:545-546), skipping any seq still pending in the AP RX queue (TouchPower's
  deviation: sboot may have left unconsumed replies).
**/
STATIC
UINT32
NextSeq (
  IN GPU_ACPM_CHANNEL  *Ch
  )
{
  UINT64  Pending;
  UINT32  Front;
  UINT32  Rear;
  UINT32  Steps;
  UINT32  Try;

  Pending = 0;
  Front   = MmioRead32 (Ch->RxFront);
  Rear    = MmioRead32 (Ch->RxRear);
  if ((Front < Ch->Len) && (Rear < Ch->Len)) {
    for (Steps = 0; (Rear != Front) && (Steps < Ch->Len); Steps++) {
      Pending |= LShiftU64 (1, (MmioRead32 (Ch->RxBase + (UINTN)Ch->Size * Rear) >> ACPM_IPC_SEQ_SHIFT) &
                               ACPM_IPC_SEQ_MASK);
      Rear = (Rear + 1) % Ch->Len;
    }
  }

  for (Try = 0; Try < 63; Try++) {
    if (++mSeqNum >= 64) {
      mSeqNum = 1;
    }
    if ((Pending & LShiftU64 (1, mSeqNum)) == 0) {
      break;
    }
  }

  if (Pending != 0) {
    DEBUG ((DEBUG_ERROR, "GpuProbe: ACPM AP RX queue not empty (seq mask 0x%lx) - using seq %u\n", Pending, mSeqNum));
  }

  return mSeqNum;
}

/**
  __acpm_ipc_send_data (acpm_ipc.c:504-628), polling, response = true, indirection = false.
**/
STATIC
EFI_STATUS
AcpmSend (
  IN     GPU_ACPM_CHANNEL  *Ch,
  IN OUT UINT32            *Cmd
  )
{
  UINT32   Front;
  UINT32   Rear;
  UINT32   Next;
  UINT32   Seq;
  UINT32   W;
  UINT32   Waited;
  BOOLEAN  Broken;
  UINT32   Sent[4];

  Front = MmioRead32 (Ch->TxFront);                 // acpm_ipc.c:522-528
  Rear  = MmioRead32 (Ch->TxRear);
  if ((Front >= Ch->Len) || (Rear >= Ch->Len)) {
    DEBUG ((DEBUG_ERROR, "GpuProbe: ACPM tx indices out of range (front %u rear %u len %u)\n", Front, Rear, Ch->Len));
    return EFI_DEVICE_ERROR;
  }

  Next = Front + 1;
  if (Next >= Ch->Len) {
    Next = 0;
  }

  for (Waited = 0; Next == MmioRead32 (Ch->TxRear); Waited += ACPM_POLL_STEP_US) {   // acpm_ipc.c:531
    if (Waited >= ACPM_QUEUE_FULL_WAIT_US) {
      DEBUG ((DEBUG_ERROR, "GpuProbe: ACPM tx queue full (front %u rear %u) - timeout\n", Front, Rear));
      return EFI_TIMEOUT;
    }
    MicroSecondDelay (ACPM_POLL_STEP_US);
  }

  Seq     = NextSeq (Ch);                           // acpm_ipc.c:545-548
  Cmd[0] |= (Seq & ACPM_IPC_SEQ_MASK) << ACPM_IPC_SEQ_SHIFT;
  for (W = 0; W < 4; W++) {
    Sent[W] = Cmd[W];
  }

  for (W = 0; W < Ch->Size / 4; W++) {              // acpm_ipc.c:550-551
    MmioWrite32 (Ch->TxBase + (UINTN)Ch->Size * Front + W * 4, (W < 4) ? Cmd[W] : 0);
  }

  MmioWrite32 (Ch->TxFront, Next);                  // acpm_ipc.c:564
  MboxWrite (ACPM_MBOX_INTGR0, (1u << Ch->Id) << 16);   // acpm_ipc.c:430

  Broken = FALSE;
  for (Waited = 0; ; Waited += ACPM_POLL_STEP_US) { // acpm_ipc.c:573-574
    if (((MboxRead (ACPM_MBOX_INTSR1) & (1u << Ch->Id)) != 0) && CheckResponse (Ch, Seq, Cmd, &Broken)) {
      break;
    }
    if (Broken) {
      DEBUG ((DEBUG_ERROR, "GpuProbe: ACPM rx indices out of range\n"));
      return EFI_DEVICE_ERROR;
    }
    if (Waited >= ACPM_REPLY_WAIT_US) {
      if (CheckResponse (Ch, Seq, Cmd, &Broken)) {   // acpm_ipc.c:599-601
        break;
      }
      DEBUG ((DEBUG_ERROR, "GpuProbe: ACPM no reply (seq %u cmd %08x %08x) INTSR1 %08x rx rear/front %u/%u "
                           "tx rear/front %u/%u\n", Seq, Sent[0], Sent[1], MboxRead (ACPM_MBOX_INTSR1),
              MmioRead32 (Ch->RxRear), MmioRead32 (Ch->RxFront), MmioRead32 (Ch->TxRear), MmioRead32 (Ch->TxFront)));
      return EFI_TIMEOUT;
    }
    MicroSecondDelay (ACPM_POLL_STEP_US);
  }

  DEBUG ((DEBUG_ERROR, "GpuProbe: ACPM seq %u sent %08x %08x %08x %08x -> reply %08x %08x %08x %08x (%u us)\n",
          Seq, Sent[0], Sent[1], Sent[2], Sent[3], Cmd[0], Cmd[1], Cmd[2], Cmd[3], Waited));
  return EFI_SUCCESS;
}

EFI_STATUS
GpuAcpmDvfsFreqReq (
  IN  UINT32  DomainIdx,
  IN  UINT32  RateKhz,
  OUT UINT32  *Reply
  )
{
  GPU_ACPM_CHANNEL  Ch;
  EFI_STATUS        Status;

  Status = FindChannel (ACPM_DVFS_IPC_CHANNEL, &Ch);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // exynos_acpm_set_rate (cal-if/acpm_dvfs.c:23-47): {id, rate, FREQ_REQ, 0}, response = true
  Reply[0] = DomainIdx;
  Reply[1] = RateKhz;
  Reply[2] = ACPM_DVFS_FREQ_REQ;
  Reply[3] = 0;
  return AcpmSend (&Ch, Reply);
}
