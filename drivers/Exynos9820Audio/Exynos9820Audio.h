/*
 * Exynos9820Audio.h - Galaxy S10e (Exynos 9820) loudspeaker render driver, ACX 1.1 (KMDF).
 *
 * Path: ABOX RDMA0 -> SPUS OUT0 -> SIFS1 -> UAIF1 (DSP-A, ABOX clock master) -> CS35L40 @0x40 (bottom speaker)
 * / @0x41 (earpiece) on hsi2c_38. notes/d8-audio-m3-abox.md, notes/d8-audio-m3-amp.md, notes/d8-audio-win-m1.md.
 *
 * Layers:
 *   Driver.c  - DriverEntry, PnP/power, _CRS contract, registry parameters.
 *   Render.c  - ACX render circuit "Speaker", host + bridge pins, formats, RT stream, DMA ring, timer clock.
 *   AboxHw.c  - ABOX block: read-only audit + AboxInit/AboxStart/AboxStop/AboxPosition (stage 1/2 hooks).
 *   AmpI2c.c  - hsi2c_38 polled master + CS35L40 sequences (AmpInit/AmpPowerUp/AmpMute/AmpPowerDown).
 *   RunLog.c  - text run log, C:\Windows\System32\LogFiles\Exynos9820Audio-run.txt.
 *
 * NOT VALIDATED ON HARDWARE (stage Guided).
 */

#pragma once

/* Include order = the ACX sample's Common/Private.h: ks.h needs windef.h/mmsystem.h first, acx.h needs wdf.h. */
#include <ntddk.h>
#include <windef.h>
#include <mmsystem.h>
#include <ks.h>
#include <ksmedia.h>
#include <wdf.h>
#include <acx.h>
#include <ntstrsafe.h>

#define AUD_POOL_TAG  'oduA'

/* ======================================================================
 * Physical windows. Every value is a copy of firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h
 * (line numbers at commit time); the driver refuses to start unless the _CRS matches (Dsdt.asl Device (AUD0)).
 * _CRS ORDER IS A BINDING CONTRACT: index = AUD_RES_*.
 * ====================================================================== */
#define AUD_ABOX_IFR_BASE      0x18C50000ULL   /* EXYNOS_ABOX_IFR_BASE    (VERIFIED, :737) */
#define AUD_ABOX_IFR_SIZE      0x10000u        /* EXYNOS_ABOX_IFR_SIZE    (VERIFIED, :738) */
#define AUD_ABOX_SYSREG_BASE   0x18C10000ULL   /* EXYNOS_ABOX_SYSREG_BASE (VERIFIED, :739) */
#define AUD_ABOX_SYSREG_SIZE   0x10000u        /* EXYNOS_ABOX_SYSREG_SIZE (VERIFIED, :740) */
#define AUD_CMU_AUD_BASE       0x18C00000ULL   /* EXYNOS_CMU_AUD_BASE     (SOURCE-ONLY, :757; read live at boot #96) */
#define AUD_CMU_AUD_SIZE       0x8000u         /* EXYNOS_CMU_AUD_SIZE     (SOURCE-ONLY, :758) */
#define AUD_GPIO_AUD_BASE      0x18C60000ULL   /* EXYNOS_PINCTRL_AUD_BASE (SOURCE-ONLY, :759; read live at boot #96) */
#define AUD_GPIO_AUD_SIZE      0x1000u         /* EXYNOS_PINCTRL_AUD_SIZE (SOURCE-ONLY, :760) */
#define AUD_HSI2C_BASE         0x109A0000ULL   /* EXYNOS_AUDIO_HSI2C_BASE (VERIFIED, :792) */
#define AUD_HSI2C_SIZE         0x1000u         /* EXYNOS_AUDIO_HSI2C_SIZE (VERIFIED, :793) */
#define AUD_ABOX_SRAM_BASE     0x18D00000ULL   /* EXYNOS_ABOX_SRAM_BASE   (VERIFIED, :741) */
#define AUD_ABOX_SRAM_SIZE     0x59000u        /* EXYNOS_ABOX_SRAM_SIZE   (VERIFIED, :742) */
#define AUD_PMU_AUD_PAGE_BASE  0x15861000ULL   /* EXYNOS_PMU_BASE 0x15860000 (VERIFIED, :280) + page of 0x1900..0x1920 */
#define AUD_PMU_AUD_PAGE_SIZE  0x1000u
#define AUD_PERIC1_GPIO_BASE   0x10830000ULL   /* EXYNOS_PERIC1_GPIO_BASE (VERIFIED, :798) */
#define AUD_PERIC1_GPIO_SIZE   0x1000u         /* EXYNOS_PERIC1_GPIO_SIZE (VERIFIED, :799) */

typedef enum _AUD_RES {
    AUD_RES_ABOX_IFR = 0,
    AUD_RES_ABOX_SYSREG,
    AUD_RES_CMU_AUD,
    AUD_RES_GPIO_AUD,
    AUD_RES_HSI2C,
    AUD_RES_ABOX_SRAM,
    AUD_RES_PMU_AUD,
    AUD_RES_PERIC1_GPIO,
    AUD_RES_COUNT
} AUD_RES;

/* PMU AUD registers, offsets inside the AUD_RES_PMU_AUD page (0x15861000). */
#define AUD_PMU_AUD_CONFIGURATION  0x900u   /* EXYNOS_PMU_AUD_CONFIGURATION_OFFSET 0x1900 (SOURCE-ONLY, :754) */
#define AUD_PMU_AUD_STATUS         0x904u   /* EXYNOS_PMU_AUD_STATUS_OFFSET 0x1904, bit0 = on (SOURCE-ONLY, :755) */
#define AUD_PMU_AUD_PAD            0x920u   /* EXYNOS_PMU_AUD_PAD_OFFSET 0x1920, bit11 = normal (SOURCE-ONLY, :756) */

/* PERIC1 pin banks (Exynos9820.h:688, :800-808, :811). */
#define AUD_GPIO_BANK_DAT      0x004u   /* EXYNOS_GPIO_BANK_DAT */
#define AUD_GPIO_GPH0_OFF      0x0E0u   /* EXYNOS_GPIO_GPH0_OFF: gph0-6 SCL, gph0-7 SDA */
#define AUD_GPIO_GPH1_OFF      0x100u   /* EXYNOS_GPIO_GPH1_OFF: gph1-1 amp R reset, gph1-2 amp L reset */
#define AUD_GPIO_FUNC_OUTPUT   0x1u     /* EXYNOS_GPIO_FUNC_OUTPUT */
#define AUD_GPIO_FUNC_HSI2C38  0x3u     /* EXYNOS_GPIO_FUNC_HSI2C38 */
#define AUD_HSI2C38_SCL_PIN    6u       /* EXYNOS_HSI2C38_SCL_PIN */
#define AUD_HSI2C38_SDA_PIN    7u       /* EXYNOS_HSI2C38_SDA_PIN */
#define AUD_AMP_R_I2C_ADDR     0x40u    /* EXYNOS_AMP_R_I2C_ADDR: "cs35l40-bot", loudspeaker */
#define AUD_AMP_L_I2C_ADDR     0x41u    /* EXYNOS_AMP_L_I2C_ADDR: "cs35l40-rcv", earpiece */
#define AUD_AMP_R_RESET_PIN    1u       /* EXYNOS_AMP_R_RESET_PIN (gph1-1, low = reset) */
#define AUD_AMP_L_RESET_PIN    2u       /* EXYNOS_AMP_L_RESET_PIN (gph1-2, low = reset) */
#define AUD_CS35L40_CHIP_ID    0x00035A40u  /* EXYNOS_CS35L40_CHIP_ID  (MTLREVID even) */
#define AUD_CS35L40R_CHIP_ID   0x00035B40u  /* EXYNOS_CS35L40R_CHIP_ID (MTLREVID odd) */

/* ======================================================================
 * Registry: HKLM\SYSTEM\CurrentControlSet\Services\Exynos9820Audio\Parameters (REG_DWORD).
 * Defaults (A8, user safety): every hardware stage OFF - a fresh install (or a package swap that drops the staged
 * values) gives a silent, QPC-clocked endpoint; the tester enables AmpInit / AmpEnable / AboxEnable explicitly.
 * ====================================================================== */
typedef struct _AUD_CONFIG {
    ULONG AmpInit;      /* 0 (default): 1 = bus up, amp reset release + ID read (UEFI m1b steps, device-proven) */
    ULONG AmpEnable;    /* 0 (default): 1 = configure + power the CS35L40 on stream start (boot #99 sequence) */
    ULONG AboxEnable;   /* 0 (default): 1 = ABOX stage 1 + RDMA0 on stream start (boot #99 sequence) */
    ULONG HwVolume;     /* 0 (default): volume + mute elements applied in software by the driver; 1 = on the amp registers */
    ULONG Stereo;       /* 0 (default): mono (L+R)/2 on the loudspeaker only; 1 = L on the earpiece amp (untested) */
    ULONG KsOverride;   /* 1 (default): the driver answers host-pin DATAINTERSECTION / PROPOSEDATAFORMAT (A7) and the
                           volume element's CHANNEL_CONFIG (A9) itself (ReqLog.c); 0 = ACX only */
    ULONG DefaultMode;  /* 1 (default, A10): host pin also offers AUDIO_SIGNALPROCESSINGMODE_DEFAULT; 0 = RAW only */
    ULONG RcvSlotReg;   /* diagnostics: SP_FRAME_RX_SLOT (0x4820) for the earpiece amp; 0xFFFFFFFF (default) = 0x100
                           (slot 0, amp notes F6). 1 = listen to slot 1 like the loudspeaker (swap test, 2026-10-16) */
} AUD_CONFIG, *PAUD_CONFIG;

/* ======================================================================
 * Run log (RunLog.c). Appending is legal up to DISPATCH_LEVEL; the file is (re)written only at PASSIVE_LEVEL.
 * ====================================================================== */
#define AUD_RUNLOG_FILE  L"\\SystemRoot\\System32\\LogFiles\\Exynos9820Audio-run.txt"
#define AUD_RUNLOG_MAX   (256u * 1024u)   /* A6: room for the KS request log (ReqLog.c) */

typedef struct _AUD_RUNLOG {
    KSPIN_LOCK Lock;
    ULONG      Used;
    ULONG      Dropped;     /* lines lost because the buffer was full */
    LONGLONG   Start;       /* KeQueryInterruptTime at creation, for the [t=] prefix */
    CHAR       Text[AUD_RUNLOG_MAX];
} AUD_RUNLOG, *PAUD_RUNLOG;

/* ======================================================================
 * Device context.
 * ====================================================================== */
typedef struct _AUD_AMP {
    UCHAR    Addr;
    ULONG    ResetPin;
    PCSTR    Name;
    BOOLEAN  Present;       /* DEVID matched */
    BOOLEAN  OtpDone;       /* OTP trim unpack ran (AmpOtpUnpack) - GLOBAL_EN is refused without it */
    BOOLEAN  Configured;    /* AmpConfigure table ran */
    BOOLEAN  Powered;       /* GLOBAL_EN = 1 */
    ULONG    RevId;
    ULONG    OtpId;
} AUD_AMP, *PAUD_AMP;

/* ReqLog.c: one KS request key and its running count (A7 log de-duplication). */
#define AUD_REQKEYS 96u
#define AUD_FMTHASHES 64u   /* ReqLog.c distinct proposed-format table (FmtHash) */
typedef struct _AUD_REQKEY {
    ULONG Data1;     /* set GUID Data1 */
    ULONG Id;
    ULONG ItemId;
    ULONG Count;
    UCHAR Type;      /* ACX_REQUEST_TYPE */
    UCHAR Verb;
    UCHAR Item;      /* ACX_ITEM_TYPE */
} AUD_REQKEY, *PAUD_REQKEY;

typedef struct _AUD_DEVICE_CONTEXT {
    WDFDEVICE   Device;
    AUD_CONFIG  Cfg;
    PAUD_RUNLOG Log;

    /* _CRS mappings, AUD_RES order. */
    PUCHAR      Va[AUD_RES_COUNT];
    ULONG       Len[AUD_RES_COUNT];
    BOOLEAN     Mapped;

    /* ABOX state (AboxHw.c). */
    BOOLEAN     AudOn;          /* PMU AUD_STATUS bit0 read 1 at PrepareHardware */
    BOOLEAN     AboxAlive;      /* IP_INDEX read "ABOX" */
    BOOLEAN     AboxReady;      /* AboxInit succeeded (stage 1 done) */
    BOOLEAN     AboxRunning;    /* AboxStart succeeded */
    ULONG       AboxSaved[16];  /* register values before the stage-1 writes, restored by AboxRelease */
    ULONG       AboxSavedCount;
    BOOLEAN     AboxDmaStuck;   /* RDMA0 progress bit never cleared after a stop: ring memory must be leaked */

    /* Amp state (AmpI2c.c). BusLock serialises every hsi2c_38 transfer (PASSIVE_LEVEL only). */
    WDFWAITLOCK BusLock;
    BOOLEAN     BusReady;
    ULONG       SavedGph0Con;
    ULONG       SavedUsiCon;
    BOOLEAN     BusSaved;
    AUD_AMP     Amp[2];         /* [0] = R/bot @0x40 (speaker), [1] = L/rcv @0x41 (earpiece) */

    /* Endpoint volume when Cfg.HwVolume = 1 (KS units: 1/65536 dB), applied on the amps. */
    LONG        VolumeLevel;
    ULONG       Muted;

    /* Endpoint volume when Cfg.HwVolume = 0 (default): the volume + mute elements still exist (sample topology; their
       absence is the suspected cause of the missing mix format, notes/d8-audio-win-a5.md) and are applied in
       software by AudDmaFillSlot while it expands client packets into the DMA ring. Gain Q30 per client channel; written at PASSIVE, read in the timer DPC
       (aligned 32-bit, single-copy atomic). The amps stay at their fixed level and are never muted by this path. */
    LONG        SwLevel[2];       /* KS units, per client channel */
    volatile LONG SwGainQ30[2];   /* 1.0 = 1 << 30 */
    volatile LONG SwMuted;

    ACXCIRCUIT  Render;
    BOOLEAN     CircuitAdded;
    ULONG       HostPinId;      /* ACX id of the streaming (host) pin, set when the circuit is built */
    LONG        SpeakerConfig;  /* ReqLog.c KSPROPERTY_AUDIO_CHANNEL_CONFIG (A9): KSAUDIO_SPEAKER_STEREO */
    ULONG       VolumeElementId;/* ACX id of the volume element (Render.c AudCreateElements) */

    volatile LONG ReqLogged;    /* ReqLog.c: KS request lines logged so far (capped) */
    volatile LONG FmtLogged;    /* ReqLog.c: distinct PROPOSEDATAFORMAT formats decoded so far */
    ULONG       FmtHashCount;   /* ReqLog.c: distinct proposed formats seen (FNV-1a of size + bytes), under ReqLock */
    ULONG       FmtHash[AUD_FMTHASHES];
    KSPIN_LOCK  ReqLock;        /* ReqKeys / ReqKeyCount */
    ULONG       ReqKeyCount;
    AUD_REQKEY  ReqKeys[AUD_REQKEYS];
} AUD_DEVICE_CONTEXT, *PAUD_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AUD_DEVICE_CONTEXT, AudGetDeviceContext)

/* Render circuit context: back pointer to the device context. */
typedef struct _AUD_CIRCUIT_CONTEXT {
    PAUD_DEVICE_CONTEXT Dev;
} AUD_CIRCUIT_CONTEXT, *PAUD_CIRCUIT_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AUD_CIRCUIT_CONTEXT, AudGetCircuitContext)

typedef struct _AUD_ELEMENT_CONTEXT {
    PAUD_DEVICE_CONTEXT Dev;
} AUD_ELEMENT_CONTEXT, *PAUD_ELEMENT_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AUD_ELEMENT_CONTEXT, AudGetElementContext)

/* ======================================================================
 * Stream (Render.c).
 * ====================================================================== */
#define AUD_MAX_PACKETS      2u          /* ACX RT packet count limit (sample MAX_PACKET_COUNT) */
#define AUD_DMA_ALIGN        16u         /* RDMA BUF_STR/END/OFFSET fields are 31:4 (notes F11, 2.1 layout; UNVERIFIED on 2.0.1) */
#define AUD_RATE             48000u
#define AUD_CHANNELS         2u          /* endpoint (client) channels */
#define AUD_HW_SLOTS         4u          /* UAIF1 frame: 4 x 32-bit slots, slot 1 = speaker @0x40, slot 0 = earpiece @0x41 */
#define AUD_HW_FRAME_BYTES   (AUD_HW_SLOTS * 4u)
#define AUD_HNS_PER_SEC      10000000ULL

typedef struct _AUD_STREAM_CONTEXT {
    PAUD_DEVICE_CONTEXT Dev;
    ACXSTREAM        Stream;
    ACXDATAFORMAT    Format;
    ULONG            BlockAlign;
    ULONG            BytesPerSec;
    ULONG            BitsPerSample;     /* container bits: 16 or 32 */

    /* Client packets (mapped to audiodg by ACX): one non-paged pool allocation + MDL per packet, exactly the ACX
       sample's StreamEngine.cpp AllocateRtPackets (A11; was one contiguous MmAllocateContiguousNodeMemory block). */
    PUCHAR           PacketBuf[AUD_MAX_PACKETS];
    ULONG            AllocPerPacket;    /* page-rounded packet size */
    PMDL             PacketMdl[AUD_MAX_PACKETS];
    ULONG            PacketCount;
    ULONG            PacketSize;
    ULONG            FirstPacketOffset;
    ULONG            FramesPerPacket;

    /* DMA ring the RDMA reads: non-cached, below 4 GiB, AUD_DMA_SLOTS periods of 4 x 32-bit frames. */
    PUCHAR           DmaVa;
    PHYSICAL_ADDRESS DmaPhys;
    ULONG            DmaBytes;
    ULONG            DmaSlotBytes;

    /* Packet clock: RDMA frame counter while HwStarted, QPC otherwise. */
    WDFTIMER         Timer;
    ACX_STREAM_STATE State;
    volatile LONG    CurrentPacket;
    ULONG            Periods;           /* packet boundaries since this Run */
    ULONGLONG        Position;          /* bytes heard since the stream started */
    ULONGLONG        StartTime;         /* HNS, QPC clock */
    ULONGLONG        StartPosition;
    LARGE_INTEGER    QpcFrequency;
    KSPIN_LOCK       HwLock;            /* HwFrames / HwLastPos (timer DPC vs position queries) */
    ULONGLONG        HwFrames;          /* RDMA frames since this Run */
    ULONG            HwLastPos;         /* last RDMA0 ring position (frames, AboxRingPosition) */
    ULONG            HwLastRaw;         /* last raw RDMA0 STATUS (log) */
    ULONG            HwBadPos;          /* samples outside the ring (ignored) */
    ULONG            Glitches;          /* timer passes that found more than one boundary */
    ULONG            LateWrites;
    ULONG            Traces;         /* run audB4: callback trace lines written (capped) */

    BOOLEAN          HwStarted;         /* AboxStart succeeded for this run (amps may still have failed) */
} AUD_STREAM_CONTEXT, *PAUD_STREAM_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AUD_STREAM_CONTEXT, AudGetStreamContext)

typedef struct _AUD_TIMER_CONTEXT {
    PAUD_STREAM_CONTEXT Stream;
} AUD_TIMER_CONTEXT, *PAUD_TIMER_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AUD_TIMER_CONTEXT, AudGetTimerContext)

/* ======================================================================
 * Prototypes.
 * ====================================================================== */

/* RunLog.c */
PAUD_RUNLOG AudLogCreate(VOID);
VOID AudLogFree(_In_opt_ PAUD_RUNLOG Log);
VOID AudLog(_In_opt_ PAUD_RUNLOG Log, _In_z_ _Printf_format_string_ PCSTR Fmt, ...);
_IRQL_requires_(PASSIVE_LEVEL) VOID AudLogFlush(_In_opt_ PAUD_RUNLOG Log);

/* Render.c */
const KSDATAFORMAT_WAVEFORMATEXTENSIBLE *AudHostFormat(_In_ ULONG Bits);
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS AudRenderCircuitCreate(_In_ WDFDEVICE Device, _In_ PAUD_DEVICE_CONTEXT Dev);
VOID AudSwVolumeInit(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ LONG LevelKs);

/* ReqLog.c */
EVT_ACX_OBJECT_PREPROCESS_REQUEST AudEvtCircuitRequestPreprocess;
EVT_ACX_OBJECT_PREPROCESS_REQUEST AudEvtStreamRequestPreprocess;

/* AboxHw.c */
_IRQL_requires_(PASSIVE_LEVEL) BOOLEAN AboxAudit(_Inout_ PAUD_DEVICE_CONTEXT Dev);
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS AboxInit(_Inout_ PAUD_DEVICE_CONTEXT Dev);
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS AboxStart(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ PHYSICAL_ADDRESS PhysAddr,
                                                  _In_ ULONG Bytes, _In_ ULONG PeriodBytes, _In_ ULONG Rate,
                                                  _In_ ULONG Channels, _In_ ULONG Bits);
_IRQL_requires_(PASSIVE_LEVEL) VOID AboxStop(_Inout_ PAUD_DEVICE_CONTEXT Dev);
_IRQL_requires_(PASSIVE_LEVEL) VOID AboxRelease(_Inout_ PAUD_DEVICE_CONTEXT Dev);
_IRQL_requires_max_(DISPATCH_LEVEL) NTSTATUS AboxRingPosition(_In_ PAUD_DEVICE_CONTEXT Dev, _Out_ PULONG Frames,
                                                              _Out_ PULONG RawStatus);

/* AmpI2c.c - all PASSIVE_LEVEL, BusLock taken inside. */
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS AmpInit(_Inout_ PAUD_DEVICE_CONTEXT Dev);
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS AmpPowerUp(_Inout_ PAUD_DEVICE_CONTEXT Dev);
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS AmpMute(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ BOOLEAN Mute);
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS AmpSetVolume(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ LONG LevelKs);
_IRQL_requires_(PASSIVE_LEVEL) VOID AmpPowerDown(_Inout_ PAUD_DEVICE_CONTEXT Dev);
_IRQL_requires_(PASSIVE_LEVEL) VOID AmpRelease(_Inout_ PAUD_DEVICE_CONTEXT Dev);
