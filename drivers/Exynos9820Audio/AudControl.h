/*
 * AudControl.h - speaker trim control from user mode (tools/S10eSpeakerTrim).
 *
 * A private KS property set on the render circuit (filter level). The app opens the circuit's KSCATEGORY_AUDIO
 * interface and sends IOCTL_KS_PROPERTY with KSPROPERTY { AUDPROPSETID_SpeakerTrim, AUD_PROPERTY_TRIM, GET|SET };
 * the value is AUD_TRIM. A SET clamps the fields, stores them in Parameters (RcvAtten, SpkAtten, RcvGain, SpkGain),
 * and applies them at once to the amps that are powered (AmpApplyTrim). Handled in AudEvtCircuitRequestPreprocess
 * before ACX sees the request (Control.c).
 */
#pragma once

/* AUDPROPSETID_SpeakerTrim = {6FEF068C-7388-4619-86D2-91FCA6A5E4AC} (defined in Control.c; same value in the app) */
#define AUDPROPSETID_SPEAKERTRIM_INIT { 0x6fef068c, 0x7388, 0x4619, { 0x86, 0xd2, 0x91, 0xfc, 0xa6, 0xa5, 0xe4, 0xac } }

#define AUD_PROPERTY_TRIM  0u

typedef struct _AUD_TRIM {
    ULONG RcvAtten;     /* earpiece digital attenuation, 1/8 dB steps (0..816) */
    ULONG SpkAtten;     /* loudspeaker digital attenuation, 1/8 dB steps (0..816) */
    ULONG RcvGain;      /* earpiece analog gain AMP_GAIN_CTRL [9:5] (0..17) */
    ULONG SpkGain;      /* loudspeaker analog gain (0..17) */
} AUD_TRIM, *PAUD_TRIM;
