/** @file
  Beyond0LteTouchPowerDxe - switch on the touch IC rails through Samsung's ACPM before Windows starts
  (notes/m12-acpm.md, notes/m12-touch.md run 9).

  On the Exynos 9820 the S2MPS19 PMIC is reachable from the AP only through the APM core (ACPM
  firmware): the kernel's drivers/mfd/s2mps19_core.c calls exynos_acpm_read_reg / _update_reg, which
  queue a 16-byte command on an ACPM IPC channel in APM SRAM and ring the APM through a mailbox.
  This driver re-implements exactly that path, polling only, and at ReadyToBoot:
    1. reads L19CTRL (LDO19M, tsp_io 1.8 V) and L27CTRL (LDO27M, tsp_avdd 3.0 V) and logs them;
    2. for each whose enable field [7:6] is not 0b11, issues FUNC_UPDATE(value 0xC0, mask 0xC0) -
       the kernel's own enable path (s2mps19_regulator.c:120-127 s2m_enable -> update_reg), so the
       voltage bits [5:0] are kept by the APM - then reads the register back and logs it;
    3. waits the sec_ts power-on settle time.
  Every wait is bounded; any failure is logged and the handler returns (boot continues).

  Protocol source: LineageOS android_kernel_samsung_exynos9820, branch lineage-22.2:
    drivers/soc/samsung/acpm/acpm_ipc.c / acpm_ipc.h / acpm_mfd.c / fw_header/framework.h,
    include/soc/samsung/acpm_mfd.h / acpm_ipc_ctrl.h, drivers/mfd/s2mps19_core.c,
    include/linux/mfd/samsung/s2mps19-regulator.h. File:line citations below refer to that tree.

  NOT VALIDATED ON HARDWARE (stage Guided). A successful build says nothing about the APM's answer.
**/

#include <Uefi.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Guid/EventGroup.h>
#include <Platform/Exynos9820.h>

//
// 0 = read-only run: log L19CTRL / L27CTRL and the ACPM channel layout, never write the PMIC.
//
#ifndef BEYOND0LTE_TOUCH_POWER_WRITE
#define BEYOND0LTE_TOUCH_POWER_WRITE  1
#endif

//
// ---------------------------------------------------------------------------------------------
// Mailbox registers, offsets from EXYNOS_ACPM_IPC_MBOX_BASE (acpm_ipc.h:80-89).
// ---------------------------------------------------------------------------------------------
//
#define ACPM_MBOX_INTGR0   0x0008u   // acpm_ipc.h:80  AP -> APM interrupt generate
#define ACPM_MBOX_INTMR1   0x0024u   // acpm_ipc.h:87  APM -> AP mask (read for the log only)
#define ACPM_MBOX_INTCR1   0x0020u   // acpm_ipc.h:86  APM -> AP pending clear
#define ACPM_MBOX_INTGR1   0x001Cu   // acpm_ipc.h:85  APM -> AP generate (check_response re-raise)
#define ACPM_MBOX_INTSR1   0x0028u   // acpm_ipc.h:88  APM -> AP status (polled, acpm_ipc.c:573)

//
// struct acpm_framework (framework.h:37-81, CONFIG_EXYNOS_ACPM: every pointer is a u32 SRAM offset),
// at EXYNOS_ACPM_SRAM_BASE + EXYNOS_ACPM_INITDATA_OFFSET (acpm_ipc.c:808).
//
#define ACPM_FW_NUM_PLUGINS       0x04u   // framework.h:43
#define ACPM_FW_IPC_CHANNELS      0x08u   // framework.h:47  u32 offset of struct ipc_channel[]
#define ACPM_FW_NUM_IPC_CHANNELS  0x0Cu   // framework.h:49
#define ACPM_FW_IPC_AP_MAX        0x18u   // framework.h:52  channel_init: num_channels (acpm_ipc.c:715)
#define ACPM_FW_BUILD_INFO        0x60u   // framework.h:70  struct build_info (common.h:45-48)
#define ACPM_FW_BUILD_INFO_LEN    50u     // common.h:46-47  char build_version[25], build_time[25]

//
// struct ipc_channel (framework.h:123-130) with struct channel_info (framework.h:97-111) inline:
// 18 x u32 = 0x48 bytes. The APM's "rx" queue is the AP's TX queue and vice versa
// (acpm_ipc.c:727-740).
//
#define ACPM_CH_STRIDE        0x48u
#define ACPM_CH_ID            0x00u   // framework.h:124
#define ACPM_CH_TYPE          0x0Cu   // framework.h:127
#define ACPM_CH_RX_REAR       0x10u   // framework.h:98   -> AP tx_ch.rear  (acpm_ipc.c:736)
#define ACPM_CH_RX_FRONT      0x14u   // framework.h:99   -> AP tx_ch.front (acpm_ipc.c:737)
#define ACPM_CH_RX_BASE       0x18u   // framework.h:100  -> AP tx_ch.base  (acpm_ipc.c:738)
#define ACPM_CH_TX_REAR       0x28u   // framework.h:104  -> AP rx_ch.rear  (acpm_ipc.c:730)
#define ACPM_CH_TX_FRONT      0x2Cu   // framework.h:105  -> AP rx_ch.front (acpm_ipc.c:731)
#define ACPM_CH_TX_BASE       0x30u   // framework.h:106  -> AP rx_ch.base  (acpm_ipc.c:732)
#define ACPM_CH_Q_LEN         0x34u   // framework.h:107  (acpm_ipc.c:729/735)
#define ACPM_CH_Q_ELEM_SIZE   0x38u   // framework.h:108  bytes per element (acpm_ipc.c:728/734)
#define ACPM_CH_AP_POLL       0x44u   // framework.h:129  (acpm_ipc.c:722)
#define ACPM_TYPE_QUEUE       1u      // framework.h:240  enum shard_buffer_type

#define ACPM_IPC_SEQ_SHIFT    16u     // acpm_ipc_ctrl.h:24 ACPM_IPC_PROTOCOL_SEQ_NUM
#define ACPM_IPC_SEQ_MASK     0x3Fu   // acpm_ipc.c:301/548

//
// Timeouts. acpm_ipc.h:95 IPC_TIMEOUT = 15 ms for the queue-full wait (acpm_ipc.c:531). The reply wait
// in the kernel is 15 ms with up to 6 retries, then an emergency WDT reset (acpm_ipc.c:568-621); here it
// is a flat 100 ms and then a logged failure. Poll step 10 us = the kernel's udelay(10) (acpm_ipc.c:595).
//
#define ACPM_POLL_STEP_US         10u
#define ACPM_QUEUE_FULL_WAIT_US   15000u
#define ACPM_REPLY_WAIT_US        100000u

//
// ---------------------------------------------------------------------------------------------
// ACPM MFD command words (include/soc/samsung/acpm_mfd.h:5-56, drivers/soc/samsung/acpm/acpm_mfd.c)
//   cmd[0] = (type & 0xF) << 8 | reg | (channel & 0xF) << 12      (acpm_mfd.c:28-29)
//   cmd[1] = func | val << 8 | mask << 16                          (acpm_mfd.c:30/121/211-213)
//   reply cmd[1]: dest = (w >> 8) & 0xFF, ret = (w >> 24) & 0xFF   (acpm_mfd.c:43-44)
// ---------------------------------------------------------------------------------------------
//
#define MFD_SHIFT_REG         0u      // acpm_mfd.h:6
#define MFD_SHIFT_TYPE        8u      // acpm_mfd.h:7   MASK_TYPE 0xF
#define MFD_SHIFT_CHANNEL     12u     // acpm_mfd.h:16  MASK_CHANNEL 0xF
#define MFD_SHIFT_VAL         8u      // acpm_mfd.h:9/11/12 DEST / WRITE_VAL / UPDATE_VAL
#define MFD_SHIFT_MASK        16u     // acpm_mfd.h:13  UPDATE_MASK
#define MFD_SHIFT_RETURN      24u     // acpm_mfd.h:14
#define MFD_FUNC_READ         0u      // acpm_mfd.h:51  enum mfd_func
#define MFD_FUNC_UPDATE       2u      // acpm_mfd.h:53

//
// Which ACPM IPC channel the PMIC client uses: the "acpm-ipc-channel" property of its DT node
// (acpm_ipc.c:233-236, client node = s2mps19mfd, s2mps19_core.c:266).
// VERIFIED 2026-10-11 on the live tree (TWRP): xxd /proc/device-tree/speedy@15970000/s2mps19mfd@00/
// acpm-ipc-channel = 00000002 (the stock dtbo's fragment@70 value too).
// The channel table in APM SRAM must also contain an entry with this id or nothing is sent.
//
#define ACPM_PMIC_IPC_CHANNEL  2u

//
// S2MPS19 addressing through ACPM: speedy channel 0 (s2mps19_core.c:55, stock dtbo acpm-speedy-channel 0),
// type = i2c->addr = I2C_ADDR_PMIC 0x01 (s2mps19_core.c:41/487).
//
#define S2MPS19_SPEEDY_CHANNEL     0u      // s2mps19_core.c:55
#define S2MPS19_I2C_ADDR_PMIC      0x01u   // s2mps19_core.c:41
#define S2MPS19_PMIC_REG_L19CTRL   0x58u   // s2mps19-regulator.h:114  LDO19M tsp_io
#define S2MPS19_PMIC_REG_L27CTRL   0x60u   // s2mps19-regulator.h:122  LDO27M tsp_avdd
#define S2MPS19_ENABLE_SHIFT       6u      // s2mps19-regulator.h:220
#define S2MPS19_ENABLE_MASK        (0x3u << S2MPS19_ENABLE_SHIFT)   // s2mps19-regulator.h:221
#define S2MPS19_ENABLE_ON          (0x3u << S2MPS19_ENABLE_SHIFT)   // SEC_OPMODE_ON -> 0x3 (s2mps19_regulator.c:99-100)

//
// Settle delays from the kernel's touch power-on (drivers/input/touchscreen/sec_ts/y771_d/sec_ts.c):
// sec_ts_power(on) enables dvdd (tsp_io), sec_ts_delay(1), then avdd (sec_ts.c:2072-2090); the probe then
// waits sec_ts_delay(70) (sec_ts.c:2722-2724). Both exceed S2MPS19_ENABLE_TIME_LDO = 128 us
// (s2mps19-regulator.h:225).
//
#define TOUCH_IO_TO_AVDD_DELAY_US  1000u
#define TOUCH_POWER_SETTLE_US      70000u

typedef struct {
  UINT32  Index;       // position in the SRAM ipc_channel[] table
  UINT32  Id;
  UINT32  Type;
  UINT32  ApPoll;
  UINT32  Len;         // q_len
  UINT32  Size;        // q_elem_size (bytes)
  UINTN   TxRear;      // AP -> APM queue (APM's rx_*)
  UINTN   TxFront;
  UINTN   TxBase;
  UINTN   RxRear;      // APM -> AP queue (APM's tx_*)
  UINTN   RxFront;
  UINTN   RxBase;
} ACPM_CHANNEL;

STATIC UINT32   mSeqNum;            // acpm_ipc_ch.seq_num, 1..63 (acpm_ipc.c:545-546)
STATIC BOOLEAN  mDone;
STATIC BOOLEAN  mIpcBroken;         // after any IPC failure no further command is sent

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
  Log the ACPM build string from initdata (a cheap check that initdata-base points at a live framework).
**/
STATIC
VOID
LogAcpmBuildInfo (
  IN UINTN  Init
  )
{
  CHAR8   Text[ACPM_FW_BUILD_INFO_LEN + 4];
  UINTN   Word;
  UINTN   Byte;
  UINT32  Value;

  for (Word = 0; Word < sizeof (Text) / 4; Word++) {
    Value = MmioRead32 (Init + ACPM_FW_BUILD_INFO + Word * 4);
    for (Byte = 0; Byte < 4; Byte++) {
      CHAR8  C = (CHAR8)(Value >> (Byte * 8));
      Text[Word * 4 + Byte] = ((C >= 0x20) && (C < 0x7F)) ? C : '.';
    }
  }
  Text[25 - 1]                     = ' ';    // build_version[25] terminator
  Text[ACPM_FW_BUILD_INFO_LEN]     = '\0';
  DEBUG ((DEBUG_ERROR, "TouchPower: ACPM build_info '%a'\n", Text));
}

/**
  channel_init (acpm_ipc.c:709-755) for one channel + acpm_ipc_request_channel (acpm_ipc.c:223-261):
  walk the ipc_ap_max entries of the SRAM ipc_channel[] table and pick the one whose id matches.
**/
STATIC
EFI_STATUS
FindChannel (
  IN  UINT32        WantedId,
  OUT ACPM_CHANNEL  *Ch
  )
{
  UINTN   Init;
  UINT32  TableOff;
  UINT32  ApMax;
  UINT32  Index;
  UINTN   Entry;

  Init     = SramAddr (EXYNOS_ACPM_INITDATA_OFFSET);   // acpm_ipc.c:808
  TableOff = MmioRead32 (Init + ACPM_FW_IPC_CHANNELS);
  ApMax    = MmioRead32 (Init + ACPM_FW_IPC_AP_MAX);

  DEBUG ((
    DEBUG_ERROR,
    "TouchPower: ACPM initdata @%lx num_plugins %u ipc_channels 0x%x num_ipc_channels %u ipc_ap_max %u\n",
    (UINT64)Init,
    MmioRead32 (Init + ACPM_FW_NUM_PLUGINS),
    TableOff,
    MmioRead32 (Init + ACPM_FW_NUM_IPC_CHANNELS),
    ApMax
    ));
  LogAcpmBuildInfo (Init);

  if ((ApMax == 0) || (ApMax > 64) || !SramOffsetOk (TableOff, (UINT64)ApMax * ACPM_CH_STRIDE)) {
    DEBUG ((DEBUG_ERROR, "TouchPower: ACPM channel table implausible - not touching ACPM\n"));
    return EFI_NOT_FOUND;
  }

  for (Index = 0; Index < ApMax; Index++) {
    Entry = SramAddr (TableOff) + (UINTN)Index * ACPM_CH_STRIDE;
    if (MmioRead32 (Entry + ACPM_CH_ID) != WantedId) {
      continue;
    }

    Ch->Index  = Index;
    Ch->Id     = WantedId;
    Ch->Type   = MmioRead32 (Entry + ACPM_CH_TYPE);
    Ch->ApPoll = MmioRead32 (Entry + ACPM_CH_AP_POLL);
    Ch->Len    = MmioRead32 (Entry + ACPM_CH_Q_LEN);
    Ch->Size   = MmioRead32 (Entry + ACPM_CH_Q_ELEM_SIZE);

    {
      UINT32  TxRearOff  = MmioRead32 (Entry + ACPM_CH_RX_REAR);
      UINT32  TxFrontOff = MmioRead32 (Entry + ACPM_CH_RX_FRONT);
      UINT32  TxBaseOff  = MmioRead32 (Entry + ACPM_CH_RX_BASE);
      UINT32  RxRearOff  = MmioRead32 (Entry + ACPM_CH_TX_REAR);
      UINT32  RxFrontOff = MmioRead32 (Entry + ACPM_CH_TX_FRONT);
      UINT32  RxBaseOff  = MmioRead32 (Entry + ACPM_CH_TX_BASE);

      DEBUG ((
        DEBUG_ERROR,
        "TouchPower: ACPM ch id %u (idx %u) type %u ap_poll %u q_len %u elem %u "
        "ap-tx rear/front/base 0x%x/0x%x/0x%x ap-rx 0x%x/0x%x/0x%x\n",
        WantedId, Index, Ch->Type, Ch->ApPoll, Ch->Len, Ch->Size,
        TxRearOff, TxFrontOff, TxBaseOff, RxRearOff, RxFrontOff, RxBaseOff
        ));

      //
      // The kernel indexes acpm_ipc->channel[] by id (acpm_ipc.c:240 returns the id, :518 indexes by it),
      // i.e. it assumes id == index. Only logged here: the lookup above is by id.
      //
      if (Index != WantedId) {
        DEBUG ((DEBUG_ERROR, "TouchPower: note - channel id %u sits at table index %u\n", WantedId, Index));
      }

      if ((Ch->Type != ACPM_TYPE_QUEUE) || (Ch->Len < 2) || (Ch->Len > 256) ||
          (Ch->Size < 8) || (Ch->Size > 64) || ((Ch->Size & 3) != 0) ||
          !SramOffsetOk (TxRearOff, 4) || !SramOffsetOk (TxFrontOff, 4) ||
          !SramOffsetOk (RxRearOff, 4) || !SramOffsetOk (RxFrontOff, 4) ||
          !SramOffsetOk (TxBaseOff, (UINT64)Ch->Size * Ch->Len) ||
          !SramOffsetOk (RxBaseOff, (UINT64)Ch->Size * Ch->Len))
      {
        DEBUG ((DEBUG_ERROR, "TouchPower: ACPM channel %u layout implausible - not touching ACPM\n", WantedId));
        return EFI_UNSUPPORTED;
      }

      Ch->TxRear  = SramAddr (TxRearOff);
      Ch->TxFront = SramAddr (TxFrontOff);
      Ch->TxBase  = SramAddr (TxBaseOff);
      Ch->RxRear  = SramAddr (RxRearOff);
      Ch->RxFront = SramAddr (RxFrontOff);
      Ch->RxBase  = SramAddr (RxBaseOff);
    }

    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_ERROR, "TouchPower: ACPM channel id %u not in the %u AP channels\n", WantedId, ApMax));
  return EFI_NOT_FOUND;
}

/**
  check_response (acpm_ipc.c:282-345): find the reply whose seq matches, copy it out, dequeue it.

  @retval TRUE   reply found and dequeued (Reply[] filled)
  @retval FALSE  not there (yet), or the queue indices are out of range (*Broken set)
**/
STATIC
BOOLEAN
CheckResponse (
  IN  ACPM_CHANNEL  *Ch,
  IN  UINT32        Seq,
  OUT UINT32        *Reply,
  OUT BOOLEAN       *Broken
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
      // acpm_ipc.c:304 memcpy_align_4(cfg->cmd, ...) - the caller's cmd[] is 4 words
      for (W = 0; W < 4; W++) {
        Reply[W] = (W * 4 < Ch->Size) ? MmioRead32 (Elem + W * 4) : 0;
      }

      // acpm_ipc.c:314-317: move the element at rear into the hole at i
      if (I != Rear) {
        UINTN  RearElem = Ch->RxBase + (UINTN)Ch->Size * Rear;

        for (W = 0; W < Ch->Size / 4; W++) {
          MmioWrite32 (Elem + W * 4, MmioRead32 (RearElem + W * 4));
        }
      }

      // acpm_ipc.c:323-334
      Rear = (Rear + 1) % Ch->Len;
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
  Pick the next sequence number (acpm_ipc.c:545-546: 1..63, wraps to 1), skipping any value still
  present in the AP RX queue. The skip is NOT in the kernel - the kernel starts from a clean probe;
  here sboot may have left unconsumed replies whose seq would satisfy check_response falsely.
**/
STATIC
UINT32
NextSeq (
  IN ACPM_CHANNEL  *Ch
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
    DEBUG ((DEBUG_ERROR, "TouchPower: AP RX queue not empty (seq mask 0x%lx) - using seq %u\n", Pending, mSeqNum));
  }

  return mSeqNum;
}

/**
  __acpm_ipc_send_data (acpm_ipc.c:504-628), polling, response = true, indirection = false.

  @param[in,out] Cmd  4 command words in; the 4 reply words out.
**/
STATIC
EFI_STATUS
AcpmSend (
  IN     ACPM_CHANNEL  *Ch,
  IN OUT UINT32        *Cmd
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

  if (mIpcBroken) {
    return EFI_ABORTED;
  }

  // acpm_ipc.c:522-528
  Front = MmioRead32 (Ch->TxFront);
  Rear  = MmioRead32 (Ch->TxRear);
  if ((Front >= Ch->Len) || (Rear >= Ch->Len)) {
    DEBUG ((DEBUG_ERROR, "TouchPower: ACPM tx indices out of range (front %u rear %u len %u)\n", Front, Rear, Ch->Len));
    mIpcBroken = TRUE;
    return EFI_DEVICE_ERROR;
  }

  Next = Front + 1;
  if (Next >= Ch->Len) {
    Next = 0;
  }

  // acpm_ipc.c:531 buffer full check, UNTIL_EQUAL with IPC_TIMEOUT
  for (Waited = 0; Next == MmioRead32 (Ch->TxRear); Waited += ACPM_POLL_STEP_US) {
    if (Waited >= ACPM_QUEUE_FULL_WAIT_US) {
      DEBUG ((DEBUG_ERROR, "TouchPower: ACPM tx queue full (front %u rear %u) - timeout\n", Front, Rear));
      mIpcBroken = TRUE;
      return EFI_TIMEOUT;
    }
    gBS->Stall (ACPM_POLL_STEP_US);
  }

  // acpm_ipc.c:545-548
  Seq     = NextSeq (Ch);
  Cmd[0] |= (Seq & ACPM_IPC_SEQ_MASK) << ACPM_IPC_SEQ_SHIFT;
  for (W = 0; W < 4; W++) {
    Sent[W] = Cmd[W];
  }

  // acpm_ipc.c:550-551 memcpy_align_4(tx_base + size * front, cmd, size); words past cmd[3] are zero
  for (W = 0; W < Ch->Size / 4; W++) {
    MmioWrite32 (Ch->TxBase + (UINTN)Ch->Size * Front + W * 4, (W < 4) ? Cmd[W] : 0);
  }

  // acpm_ipc.c:564 writel(tmp_index, front); :566 apm_interrupt_gen -> acpm_ipc.c:430
  MmioWrite32 (Ch->TxFront, Next);
  MboxWrite (ACPM_MBOX_INTGR0, (1u << Ch->Id) << 16);

  // acpm_ipc.c:573-574: wait for INTSR1 bit AND a matching reply
  Broken = FALSE;
  for (Waited = 0; ; Waited += ACPM_POLL_STEP_US) {
    if (((MboxRead (ACPM_MBOX_INTSR1) & (1u << Ch->Id)) != 0) && CheckResponse (Ch, Seq, Cmd, &Broken)) {
      break;
    }
    if (Broken) {
      DEBUG ((DEBUG_ERROR, "TouchPower: ACPM rx indices out of range\n"));
      mIpcBroken = TRUE;
      return EFI_DEVICE_ERROR;
    }
    if (Waited >= ACPM_REPLY_WAIT_US) {
      // acpm_ipc.c:599-601: one last look regardless of INTSR1
      if (CheckResponse (Ch, Seq, Cmd, &Broken)) {
        break;
      }
      DEBUG ((
        DEBUG_ERROR,
        "TouchPower: ACPM no reply (seq %u cmd %08x %08x) INTSR1 %08x rx rear/front %u/%u tx rear/front %u/%u\n",
        Seq, Sent[0], Sent[1], MboxRead (ACPM_MBOX_INTSR1),
        MmioRead32 (Ch->RxRear), MmioRead32 (Ch->RxFront), MmioRead32 (Ch->TxRear), MmioRead32 (Ch->TxFront)
        ));
      mIpcBroken = TRUE;
      return EFI_TIMEOUT;
    }
    gBS->Stall (ACPM_POLL_STEP_US);
  }

  DEBUG ((
    DEBUG_ERROR,
    "TouchPower: ACPM seq %u sent %08x %08x -> reply %08x %08x %08x %08x (%u us)\n",
    Seq, Sent[0], Sent[1], Cmd[0], Cmd[1], Cmd[2], Cmd[3], Waited
    ));
  return EFI_SUCCESS;
}

STATIC
UINT32
PmicCmd0 (
  IN UINT8  Reg
  )
{
  // acpm_mfd.c:28-29 set_protocol(type, TYPE) | set_protocol(reg, REG) | set_protocol(channel, CHANNEL)
  return ((S2MPS19_I2C_ADDR_PMIC & 0xFu) << MFD_SHIFT_TYPE) | ((UINT32)Reg << MFD_SHIFT_REG) |
         ((S2MPS19_SPEEDY_CHANNEL & 0xFu) << MFD_SHIFT_CHANNEL);
}

/**
  exynos_acpm_read_reg (acpm_mfd.c:19-61).
**/
STATIC
EFI_STATUS
PmicRead (
  IN  ACPM_CHANNEL  *Ch,
  IN  UINT8         Reg,
  OUT UINT8         *Value
  )
{
  UINT32      Cmd[4];
  EFI_STATUS  Status;
  UINT32      Ret;

  Cmd[0] = PmicCmd0 (Reg);
  Cmd[1] = MFD_FUNC_READ;          // acpm_mfd.c:30
  Cmd[2] = 0;
  Cmd[3] = 0;

  Status = AcpmSend (Ch, Cmd);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Value = (UINT8)(Cmd[1] >> MFD_SHIFT_VAL);           // acpm_mfd.c:43 DEST
  Ret    = (Cmd[1] >> MFD_SHIFT_RETURN) & 0xFFu;       // acpm_mfd.c:44 RETURN
  if (Ret != 0) {
    DEBUG ((DEBUG_ERROR, "TouchPower: PMIC read 0x%02x - APM speedy_rx fail, ret %u\n", Reg, Ret));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  exynos_acpm_update_reg (acpm_mfd.c:200-243): the APM does (old & ~Mask) | (Val & Mask).
**/
STATIC
EFI_STATUS
PmicUpdate (
  IN ACPM_CHANNEL  *Ch,
  IN UINT8         Reg,
  IN UINT8         Val,
  IN UINT8         Mask
  )
{
  UINT32      Cmd[4];
  EFI_STATUS  Status;
  UINT32      Ret;

  Cmd[0] = PmicCmd0 (Reg);
  Cmd[1] = MFD_FUNC_UPDATE | ((UINT32)Val << MFD_SHIFT_VAL) | ((UINT32)Mask << MFD_SHIFT_MASK);   // acpm_mfd.c:211-213
  Cmd[2] = 0;
  Cmd[3] = 0;

  Status = AcpmSend (Ch, Cmd);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Ret = (Cmd[1] >> MFD_SHIFT_RETURN) & 0xFFu;         // acpm_mfd.c:226 (1 = speedy_rx, 2 = speedy_tx fail)
  if (Ret != 0) {
    DEBUG ((DEBUG_ERROR, "TouchPower: PMIC update 0x%02x - APM speedy fail, ret %u\n", Reg, Ret));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Read one LDO control register, enable it if its [7:6] field is not 0b11, read it back.

  @retval TRUE  an enable command was sent and the read-back shows 0b11
**/
STATIC
BOOLEAN
EnsureLdoOn (
  IN ACPM_CHANNEL  *Ch,
  IN UINT8         Reg,
  IN CONST CHAR8   *Name
  )
{
  UINT8       Before;
  UINT8       After;
  EFI_STATUS  Status;

  Status = PmicRead (Ch, Reg, &Before);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "TouchPower: %a (0x%02x) read failed: %r\n", Name, Reg, Status));
    return FALSE;
  }

  DEBUG ((
    DEBUG_ERROR,
    "TouchPower: %a (0x%02x) = 0x%02x  enable[7:6] = %u  vsel[5:0] = 0x%02x\n",
    Name, Reg, Before, (Before & S2MPS19_ENABLE_MASK) >> S2MPS19_ENABLE_SHIFT, Before & 0x3F
    ));

  if ((Before & S2MPS19_ENABLE_MASK) == S2MPS19_ENABLE_ON) {
    DEBUG ((DEBUG_ERROR, "TouchPower: %a already on (0b11) - no write\n", Name));
    return FALSE;
  }

 #if BEYOND0LTE_TOUCH_POWER_WRITE
  Status = PmicUpdate (Ch, Reg, S2MPS19_ENABLE_ON, S2MPS19_ENABLE_MASK);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "TouchPower: %a enable failed: %r\n", Name, Status));
    return FALSE;
  }

  Status = PmicRead (Ch, Reg, &After);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "TouchPower: %a read-back failed: %r\n", Name, Status));
    return FALSE;
  }

  DEBUG ((
    DEBUG_ERROR,
    "TouchPower: %a (0x%02x) after enable = 0x%02x  enable[7:6] = %u%a\n",
    Name, Reg, After, (After & S2MPS19_ENABLE_MASK) >> S2MPS19_ENABLE_SHIFT,
    ((After & 0x3F) != (Before & 0x3F)) ? "  VSEL CHANGED!" : ""
    ));
  return (After & S2MPS19_ENABLE_MASK) == S2MPS19_ENABLE_ON;
 #else
  (VOID)After;
  DEBUG ((DEBUG_ERROR, "TouchPower: %a off - read-only build, no write\n", Name));
  return FALSE;
 #endif
}

STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  ACPM_CHANNEL  Ch;
  BOOLEAN       Enabled;

  if (mDone) {
    return;            // ReadyToBoot can be signalled more than once
  }
  mDone = TRUE;

  DEBUG ((
    DEBUG_ERROR,
    "TouchPower: ReadyToBoot - mbox INTMR1 %08x INTSR1 %08x (write %a)\n",
    MboxRead (ACPM_MBOX_INTMR1), MboxRead (ACPM_MBOX_INTSR1),
    BEYOND0LTE_TOUCH_POWER_WRITE ? "on" : "off"
    ));

  if (EFI_ERROR (FindChannel (ACPM_PMIC_IPC_CHANNEL, &Ch))) {
    DEBUG ((DEBUG_ERROR, "TouchPower: done (no ACPM channel)\n"));
    return;
  }

  // sec_ts_power order: dvdd = tsp_io (LDO19M) first, 1 ms, then avdd = tsp_avdd (LDO27M) (sec_ts.c:2072-2090)
  Enabled = EnsureLdoOn (&Ch, S2MPS19_PMIC_REG_L19CTRL, "L19CTRL tsp_io");
  if (Enabled) {
    gBS->Stall (TOUCH_IO_TO_AVDD_DELAY_US);
  }

  if (EnsureLdoOn (&Ch, S2MPS19_PMIC_REG_L27CTRL, "L27CTRL tsp_avdd")) {
    Enabled = TRUE;
  }

  if (Enabled) {
    gBS->Stall (TOUCH_POWER_SETTLE_US);   // sec_ts.c:2724 sec_ts_delay(70)
    DEBUG ((DEBUG_ERROR, "TouchPower: settled %u us after enable\n", TOUCH_POWER_SETTLE_US));
  }

  DEBUG ((DEBUG_ERROR, "TouchPower: done%a\n", mIpcBroken ? " (ACPM IPC failed)" : ""));
}

EFI_STATUS
EFIAPI
Beyond0LteTouchPowerEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_EVENT  Event;

  //
  // TPL_NOTIFY: dispatched before Beyond0LteBootLogDxe's TPL_CALLBACK ReadyToBoot flush, so this
  // driver's lines are in the same boot's \UEFILOG.TXT.
  //
  return gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, OnReadyToBoot, NULL, &gEfiEventReadyToBootGuid, &Event);
}
