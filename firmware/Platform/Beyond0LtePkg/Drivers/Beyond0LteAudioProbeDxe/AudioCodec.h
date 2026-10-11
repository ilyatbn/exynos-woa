// AudioCodec.h - CS47L93 register access over spi_2, shared inside Beyond0LteAudioProbeDxe (D8a m2).
//
// Implemented in Beyond0LteAudioProbeDxe.c. Valid only while CodecProbe has the spi_2 bus up (USI mode,
// pins, controller init) - i.e. from inside CodecProbe, between "spi2 after init" and the bus release.
// Framing: madera regmap over SPI, 32-bit big-endian address (bit 31 = read flag 0x80 of the top byte),
// 16 pad bits, then a 16-bit value (registers < 0x3000) or a 32-bit value (the 32-bit regmap, >= 0x3000)
// (cs47l92-tables.c:2356-2362 / 2391-2405, regmap-spi.c:111). notes/d8-audio-m1.md §1.2, §6.1.

#ifndef AUDIO_CODEC_H_
#define AUDIO_CODEC_H_

#include <Uefi.h>

EFI_STATUS
CodecRead (
  IN  UINT32  Reg,
  OUT UINT16  *Value
  );

EFI_STATUS
CodecWrite (
  IN UINT32  Reg,
  IN UINT16  Value
  );

// regmap_update_bits: read, (old & ~Mask) | (Value & Mask), write. No write if the read fails.
EFI_STATUS
CodecUpdate (
  IN UINT32  Reg,
  IN UINT16  Mask,
  IN UINT16  Value
  );

EFI_STATUS
CodecRead32 (
  IN  UINT32  Reg,
  OUT UINT32  *Value
  );

EFI_STATUS
CodecWrite32 (
  IN UINT32  Reg,
  IN UINT32  Value
  );

#endif
