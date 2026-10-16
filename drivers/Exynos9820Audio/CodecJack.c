/*
 * CodecJack.c - 3.5 mm jack presence on the CS47L93 (notes/d8-jack-windows.md J4, research notes/d8-jack-research.md Q4).
 *
 * Stock detection = JD2 + micd-clamp on HPOUT2 (stock dtbo cirrus,micd-clamp-mode 8; extcon-madera.c). Only presence
 * is needed here (no mic / impedance / moisture). Polled on a passive-level WDF timer - one-shot, re-armed by its own
 * callback (WDF_TIMER_CONFIG: "The time period must be zero if WdfTimerCreate sets the execution level to
 * WdfExecutionLevelPassive"; a periodic one failed with C00000BB on the phone). The codec IRQ line (gpa2-2,
 * IRQ1_MASK_7 bits 0x10/0x20) is for later.
 *   0x1A06 INTERRUPT_DEBOUNCE_7 |= 0x0015 (MICD_CLAMP_DB | JD2_DB | JD1_DB)
 *   0x02C6 MICD_CLAMP_CONTROL [3:0] = 8 (JD1L_JD2L)
 *   0x02D3 JACK_DETECT_ANALOGUE |= 0x0003 (JD1_ENA | JD2_ENA)
 *   present <=> (0x1886 IRQ1_RAW_STATUS_7 & 0x0010) == 0 (madera_jack_present)
 * Needs the 32 kHz clock (CodecSetup: 0x100 CLK_32K_ENA | MCLK2).
 */
#include "Exynos9820Audio.h"

#define R_INTERRUPT_DEBOUNCE_7   0x1A06u
#define   DEBOUNCE7_JD_MICD        0x0015u
#define R_MICD_CLAMP_CONTROL     0x02C6u
#define   MICD_CLAMP_MODE_MASK     0x000Fu
#define   MICD_CLAMP_JD1L_JD2L     0x0008u
#define R_JACK_DETECT_ANALOGUE   0x02D3u
#define   JD1_JD2_ENA              0x0003u
#define R_IRQ1_RAW_STATUS_7      0x1886u
#define   MICD_CLAMP_STS           0x0010u

#define JACK_POLL_MS             500u

static EVT_WDF_TIMER CodecJackTimer;

static VOID CodecJackPoll(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_z_ PCSTR Why)
{
    USHORT   Raw7 = 0;
    NTSTATUS Status = CodecRead(Dev, R_IRQ1_RAW_STATUS_7, &Raw7);
    BOOLEAN  Present = (Raw7 & MICD_CLAMP_STS) == 0;

    if (!NT_SUCCESS(Status)) {
        return;
    }
    if (!Dev->JackKnown || Present != Dev->JackPresent) {
        Dev->JackPresent = Present;
        Dev->JackKnown   = TRUE;
        AudLog(Dev->Log, "jack: %s (%s, IRQ1_RAW_STATUS_7 %04X)\r\n", Present ? "PLUGGED" : "unplugged", Why, Raw7);
        AudLogFlush(Dev->Log);
        AudJackNotify(Dev);   /* J4b: Windows re-reads both jack presences and moves the default output */
    }
}

static VOID CodecJackTimer(_In_ WDFTIMER Timer)
{
    PAUD_DEVICE_CONTEXT Dev = AudGetDeviceContext((WDFDEVICE)WdfTimerGetParentObject(Timer));

    if (Dev->CodecUp && !Dev->JackStopping) {
        CodecJackPoll(Dev, "poll");
    }
    if (!Dev->JackStopping) {
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(JACK_POLL_MS));
    }
}

/* Detection on + first read + periodic poll. Codec must be up (CodecInit) with the 32 kHz clock running. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS CodecJackStart(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ WDFDEVICE Device)
{
    WDF_TIMER_CONFIG      Cfg;
    WDF_OBJECT_ATTRIBUTES Attr;
    NTSTATUS              Status;

    PAGED_CODE();

    if (!Dev->CodecUp) {
        return STATUS_DEVICE_NOT_READY;
    }
    AudLog(Dev->Log, "jack: start\r\n"); AudLogFlush(Dev->Log);
    (VOID)CodecUpdate(Dev, R_INTERRUPT_DEBOUNCE_7, DEBOUNCE7_JD_MICD, DEBOUNCE7_JD_MICD);
    (VOID)CodecUpdate(Dev, R_MICD_CLAMP_CONTROL, MICD_CLAMP_MODE_MASK, MICD_CLAMP_JD1L_JD2L);
    (VOID)CodecUpdate(Dev, R_JACK_DETECT_ANALOGUE, JD1_JD2_ENA, JD1_JD2_ENA);
    KeStallExecutionProcessor(2000);
    AudLog(Dev->Log, "jack: detection enabled\r\n"); AudLogFlush(Dev->Log);
    Dev->JackKnown = FALSE;
    CodecJackPoll(Dev, "start");

    if (Dev->JackTimer == NULL) {
        WDF_TIMER_CONFIG_INIT(&Cfg, CodecJackTimer);   /* one-shot: passive timers cannot be periodic */
        Cfg.AutomaticSerialization = FALSE;   /* passive-level timer on a device without a passive execution level */
        WDF_OBJECT_ATTRIBUTES_INIT(&Attr);
        Attr.ParentObject   = Device;
        Attr.ExecutionLevel = WdfExecutionLevelPassive;
        Status = WdfTimerCreate(&Cfg, &Attr, &Dev->JackTimer);
        if (!NT_SUCCESS(Status)) {
            AudLog(Dev->Log, "jack: timer create %08lX\r\n", (ULONG)Status);
            return Status;
        }
    }
    AudLog(Dev->Log, "jack: polling every %u ms\r\n", JACK_POLL_MS); AudLogFlush(Dev->Log);
    Dev->JackStopping = FALSE;
    WdfTimerStart(Dev->JackTimer, WDF_REL_TIMEOUT_IN_MS(JACK_POLL_MS));
    return STATUS_SUCCESS;
}

/* Stop polling (waits for a running callback). Not from the timer callback. */
_IRQL_requires_(PASSIVE_LEVEL)
VOID CodecJackStop(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    PAGED_CODE();

    Dev->JackStopping = TRUE;                 /* the callback stops re-arming */
    if (Dev->JackTimer != NULL) {
        WdfTimerStop(Dev->JackTimer, TRUE);   /* waits for a running callback */
    }
    Dev->JackKnown = FALSE;
}
