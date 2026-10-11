// Beyond0LteAudioPower.h - the hand-off between Beyond0LteTouchPowerDxe (the only ACPM/PMIC writer) and
// Beyond0LteAudioProbeDxe for D8a milestone 1b (notes/d8-audio-m1.md §5.2).
//
// The CS47L93 codec's DCVDD (1.2 V) is S2MPS19 LDO18M, which sboot leaves OFF (boot #88: L18CTRL 0x28).
// Only TouchPowerDxe talks to the APM, and only at ReadyToBoot. So:
//   * AudioProbeDxe (DXE dispatch) asserts the codec reset and registers a TPL_NOTIFY event in the group
//     below instead of probing the codec right away;
//   * TouchPowerDxe's ReadyToBoot handler (TPL_NOTIFY) switches LDO18M on and then signals the group once,
//     in every path (rail on, refused, or no ACPM) - the codec probe logs whatever it finds;
//   * DxeCore queues the group's notify at TPL_NOTIFY and runs it in the same TPL_NOTIFY dispatch loop,
//     i.e. before BootLogDxe's TPL_CALLBACK UEFILOG flush (MdeModulePkg/Core/Dxe/Event/Event.c:153-200,235).
//
// Included by both drivers (TouchPowerDxe through a relative path; Beyond0LtePkg has no .dec).

#ifndef BEYOND0LTE_AUDIO_POWER_H_
#define BEYOND0LTE_AUDIO_POWER_H_

// 1 = TouchPowerDxe enables LDO18M (a PMIC write through ACPM) and AudioProbeDxe defers the codec probe
//     until after it. 0 = milestone-1 behaviour: L18CTRL read only, codec probed at DXE dispatch.
#ifndef BEYOND0LTE_AUDIO_DCVDD
#define BEYOND0LTE_AUDIO_DCVDD  1
#endif

// Event group "codec DCVDD step finished" (random GUID, this project only).
#define BEYOND0LTE_CODEC_POWER_EVENT_GUID \
  { 0xfd6b0be6, 0xc838, 0x4907, { 0xac, 0xe9, 0xe4, 0x59, 0x86, 0x8c, 0x68, 0x26 } }

#endif
