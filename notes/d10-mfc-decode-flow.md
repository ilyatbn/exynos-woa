# D10 V-W2: MFC v13 H.264 decode, plain (non-NAL-Q) command flow

Source: LineageOS android_kernel_samsung_exynos9820 lineage-22.2, drivers/media/platform/exynos/mfc/ (reference only,
GPL - no code copied; register offsets / command numbers are facts). Research 2026-10-15 (subagent read of mfc_cmd.c,
mfc_reg_api.c, mfc_isr.c, mfc_run.c, mfc_utils.c, mfc_macros.h, mfc_regs.h, exynos9820-mfc.dtsi).
`L:` = files already used for R3/R4, `K:` = second fetch.

## Corrections to the earlier plan
- No FRAME_START: v13 decodes with **NAL_START = 5**, drains with **LAST_FRAME = 9** (L:mfc_regs.h:502,506).
- DPB addresses are NOT given at INIT_BUFFERS: the vendor always runs dynamic DPB (INIT_BUFFER_OPTIONS bit3); each
  output buffer address is written per NAL_START.

## 0. Mechanics
- Issue: RISC2HOST_CMD(0x1104)=0, HOST2RISC_CMD(0x1100)=cmd, HOST2RISC_INT(0x0044)=1 (L:mfc_hw_reg_api.c:56-85).
- Result: reason = RISC2HOST_CMD & 0x1FFFF; err = ERROR_CODE(0xF074) ([15:0] error, [31:16] warning).
- Clear AFTER reading results: RISC2HOST_CMD=0, RISC2HOST_INT(0x003C)=0.
- Any wait also accepts ERR_RET (32). FRAME_DONE wait also accepts 14 FIELD_DONE, 9 COMPLETE_SEQ, 15 SLICE_DONE.
- 0xF000..0xFFFC zeroed only at reset; written values persist across commands.
- Per instance: only INSTANCE_ID(0xF008)=inst_no before SEQ_HEADER / INIT_BUFFERS / NAL_START / LAST_FRAME /
  DPB_FLUSH / CLOSE. SHARED_MEM_ADDR never written in plain mode.

## 1. Sequence
A. OPEN_INSTANCE H2R 2 -> R2H 2: CODEC_CONTROL(0xF038) &= ~0xE; CODEC_TYPE(0xF00C)=0x00010000 (H.264 dec 0 |
   CLEAR_CTX_MEM bit16); CONTEXT_MEM_ADDR(0xF014)=inst buf, CONTEXT_MEM_SIZE(0xF018)=0x200000; D_CRC_CTRL(0xF0B0)=0.
   inst_no = RET_INSTANCE_ID(0xF070).
B. SEQ_HEADER H2R 3 -> R2H 3: stream regs D_STREAM_DATA_SIZE(0xF5D0)=valid bytes from offset,
   D_CPB_BUFFER_ADDR(0xF5B0)=PA (offset not added), D_CPB_BUFFER_SIZE(0xF5B4)=ALIGN(size,512),
   D_CPB_BUFFER_OFFSET(0xF5C0)=offset; data <= cpb_size-512. D_DEC_OPTIONS(0xF0B4)=0xA400;
   D_FORCE_PIXEL_VAL(0xF0C8)=0x8020000; PIXEL_FORMAT(0xF020)=0x10 (NV12, P010_decoding DT flag in bit4);
   D_SEI_ENABLE(0xF0C4)=0x34 (0x74 if fw date >= 0x180709); D_DISPLAY_DELAY not written (default -1); INSTANCE_ID.
   Read back: width D_DISPLAY_FRAME_WIDTH(0xF600), height (0xF604), D_MIN_NUM_DPB(0xF0F0),
   D_MIN_SCRATCH_BUFFER_SIZE(0xF108), D_MIN_FIRST/SECOND_PLANE_DPB_SIZE(0xF0F4/0xF0F8), D_MIN_NUM_MV(0xF100),
   crop D_DISPLAY_CROP_INFO1(0xF61C) left|right<<16, INFO2(0xF620) top|bottom<<16, D_DECODED_NAL_SIZE(0xF664)
   consumed (0<consumed<used -> resubmit same buffer with offset). ERR 102 HEADER_NOT_FOUND -> drop buffer, retry.
C. INIT_BUFFERS H2R 4 -> R2H 4: PIXEL_FORMAT again; D_NUM_DPB(0xF130)=N (>= min, <= 32);
   D_FIRST_PLANE_DPB_SIZE(0xF144)/STRIDE(0xF138), D_SECOND_PLANE_DPB_SIZE(0xF148)/STRIDE(0xF13C);
   D_SCRATCH_BUFFER_ADDR(0xF560)=codec buf, D_SCRATCH_BUFFER_SIZE(0xF564)=ALIGN(scratch,256);
   D_MV_BUFFER_SIZE(0xF150)=mv_size; D_INIT_BUFFER_OPTIONS(0xF154)=0x8; D_NUM_MV(0xF134)=mv_count;
   D_MV_BUFFER0+4i (0xF460..) = after scratch, each ALIGN16 then +mv_size; INSTANCE_ID. err != 0 = fail.
D. NAL_START H2R 5 -> R2H 13 (or 14/15/9/32): stream regs as B; pick idx not in dynamic_used;
   available |= 1<<idx; re-write plane sizes; D_FIRST_PLANE_DPB0+4idx (0xF160..)=luma,
   D_SECOND_PLANE_DPB0+4idx (0xF260..)=chroma; D_PICTURE_TAG(0xF5C8) optional (-> 0xF674);
   D_NAL_START_OPTIONS(0xF5AC) bit3=0; D_DYNAMIC_DPB_FLAG_LOWER(0xF5D8)=1<<idx, UPPER(0xF5D4)=0;
   D_AVAILABLE_DPB_FLAG_LOWER(0xF5BC)=available, UPPER(0xF5B8)=0; D_SLICE_IF_ENABLE(0xF5C4)=0;
   DEC_TIMEOUT_VALUE(0xF03C)=200000000; INSTANCE_ID; cmd 5 (or 9 at EOS).
   Result: disp = D_DISPLAY_STATUS(0xF608)&7: 0 decode only, 1 decode+display, 2 display only (input NOT consumed),
   3 empty (drain done). Decoded buffer = D_DECODED_LUMA_ADDR(0xF648). Display: D_DISPLAY_LUMA_ADDR(0xF60C),
   CHROMA(0xF610), type D_DISPLAY_FRAME_TYPE(0xF618)&7 (0 not coded -> skip, 1 I, 2 P, 3 B);
   dynamic_used = D_USED_DPB_FLAG_LOWER(0xF724); clear displayed idx from available.
   Consumed D_DECODED_NAL_SIZE: >0 and remaining >4 and no error -> refeed same buffer at offset+consumed.
   D_DISPLAY_STATUS [5:4] res change, bit9/bit10 fatal (DPB/scratch change).
E. EOS: LAST_FRAME (9) with size 0, free DPB each time, until disp == 3.
F. CLOSE_INSTANCE H2R 6 -> R2H 6 (INSTANCE_ID first). DPB_FLUSH H2R 10 -> R2H 10 optional.

## 2. Sizes (8-bit NV12, two planes)
- plane = ALIGN(w,16)*ALIGN(h,16); luma = plane+256, chroma = plane/2+256, each max'ed with the D_MIN_* read-back.
- stride luma = chroma = ALIGN(w,16).
- mv_size = ALIGN(WIDTH_MB(w) * (((HEIGHT_MB(h)+1)/2)*2) * 64 + 1024, 32).
- codec buffer = ALIGN(scratch,256) + mv_count*mv_size.
- 1920x1080: luma 2,089,216, chroma 1,044,736, mv 523,264.
- Address alignment of CPB/DPB: not in source -> UNVERIFIED; use 4 KiB (contiguous allocations are page aligned).

## 3. Errors
- ERR_RET while running: 160..222 = warning (handle frame normally), others drop the source.
- Fatal: 33 NO_AVAILABLE_DPB, 57 INSUFFICIENT_DPB_SIZE, 58 INSUFFICIENT_NUM_DPB, 60 INSUFFICIENT_MV_BUF_SIZE,
  62 INSUFFICIENT_SCRATCH. Also 61 NULL_SCRATCH, 102 HEADER_NOT_FOUND, 104 SEQUENCE_HEADER, 140 TIMEOUT.

## 4. v13 specifics
- No MFC_VER_MAJOR >= 13 branches; only check MFC_VERSION == 0x1300 after SYS_INIT. 9820 features come from DT:
  nal_q on (skipped here), static_info_dec, hdr10_plus (fw date >= 0x180709), black_bar, P010_decoding=1.
- Our firmware FW_VERSION 0x45210720 -> date field 0x210720 >= 0x180709 -> vendor would send D_SEI_ENABLE 0x74.

## V-W2 plan (Windows driver, self-test first)
1. Embedded test stream: a tiny H.264 Annex-B clip generated on the PC (ffmpeg/x264, e.g. 64x64 or 128x96,
   a few frames, baseline). Ship as a file next to mfc_fw.bin (generated, not a blob from the phone).
2. Driver self-test after SYS_INIT, polled (no ISR dependency): OPEN -> SEQ_HEADER -> INIT_BUFFERS -> NAL_START xN ->
   LAST_FRAME drain -> CLOSE; log each R2H, err, width/height/min DPB, and a checksum of each displayed luma plane.
3. Verify against the same checksum computed on the PC (ffmpeg -f rawvideo nv12 of the clip).

## Progress log
- 2026-10-15 14:40 research done (above). Next: generate the test clip.
- 14:50 test vector: tools/mfc/mkh264pcm.py (no encoder on the PC; I_PCM IDR + all-P_Skip P + second I_PCM IDR,
  deblocking off -> output known bit-exactly). 128x96: 37101 bytes. Expected (FNV-1a 32, luma W x H / NV12 CbCr):
  f0 0xd3990385/0x1eb1b40e, f1 same, f2 0xcabb3add/0x050d99fd. Cross-checked with libavcodec (PyAV 19.0.1 in a
  scratch venv): identical hashes, picture types I, P, I.
- 14:48 driver self-test committed (V-W2 self-test, chroma page-align fix 3a264fd, DriverVer
  1.0.0.2 d9b1c39). Device run vidc2 (b94fcbfb, BTPROBE switches as vidc1 + VIDC gate b96228a): package +
  mfc_test.h264 on /cache/S10EDIAG, extra-mfc.cmd, kdrun go -> scratchpad kd-vidc2.log. Booted 14:48:51.
- 14:49 vidc2: KD connected; installed V-W1 driver loaded at boot (fw at PA 0xef700000, SYS_INIT, ISR 1) => VIDC gate OK on device (word [13] set, device present). Waiting for the 1.0.0.2 install by extra-mfc.cmd.
- **14:50:45 vidc2 BUGCHECK 0x122 (WHEA, HalWheaHandleSei = async SError)** when extra-mfc.cmd's pnputil swapped the
  driver: CPU5 was in the OLD V-W1 Exynos9820Mfc ReleaseHardware (+0x10a8, via _PnpRemoveDevice), the SError surfaced
  on CPU3 in user mode. V-W1 release = RISC_ON 0 then immediately MfcReset (1024 register clears + MFC_RESET 0x1FFF)
  with the firmware running. Vendor never does that: shutdown (mfc.c:1666 mfc_risc_off) polls MFC_BUS_STATUS
  (0x7018) == 0 up to MFC_BW_TIMEOUT 500 ms, then RISC_ON 0; deinit (mfc_run.c:173) is only MFC_OFF (0x7120) = 1.
  Bus reset helper (mfc_stop_bus): BUS_RESET_CTRL (0x7110) = 1, wait bit1. The self-test itself never ran (old
  driver unloading). Fix: release = vendor risc_off (bus idle wait) + MFC_OFF 1, no reset; free buffers after.
  NOTE: the V-W1 driver is still the installed one on the phone - any further swap may hit this again.
- 14:55 TWRP: NTFS dirty after the bugcheck (ntfs-3g ro only, no fsck) -> in-Windows rename swap: extra-mfcswap.cmd with 1.0.0.3 (a6ce0f3, sys 13489b6e) + mfc_test.h264; kd-vidc3.log. Pass 1 swaps + clean restart, pass 2 = self-test boot.
- **14:55 vidc3 pass 1: the self-test RAN** (PnP completed the interrupted 1.0.0.2 install at boot, no PnP remove,
  no crash): "37101 B stream, buffer PA 0xeeed8000" -> "OPEN_INSTANCE R2H 32 (ERR_RET) err 0xa inst 65535".
  Error 10 is not named in mfc_regs.h. Suspect: the test buffer (separate allocation, PA 0xeeed8000) lies BELOW the
  RISC base (fw at 0xef700000). Vendor layout (mfc_buf.c:520-530, mfc.c:1773-1786): fw_buf = firmware_code 1 MiB +
  dev_ctx 32 KiB, common context at fw_daddr + 1 MiB (we used +4 MiB); every other buffer comes from the same
  iovmm allocator after the fw. Upstream s5p-mfc required buffers within a window ABOVE the fw base (recalled, not
  verified for v13). Fix (1.0.0.4): one allocation, fw at 0, common ctx at +1 MiB (vendor), decode-test area at
  +8 MiB inside the same buffer.
- 15:0x vidc3 pass 2 (1.0.0.3 swapped in by rename): same OPEN ERR 10 (expected, same layout); no crash across the clean restart (no PnP remove). 1.0.0.4 built (5da99ab, sys 1F1778A6): buffers above the base.
- **15:01 V-W2 PASSED (vidc4, Exynos9820Mfc 1.0.0.4 5da99ab, swapped from TWRP):** buffers above the base fixed
  ERR 10. KD: OPEN_INSTANCE R2H 2 inst 0; SEQ_HEADER R2H 3 128x96 min dpb 3 scratch 0x2100 plane 0x3040/0x1840
  mv 1 consumed 13; INIT_BUFFERS R2H 4 err 0 (dpb 5, luma 0x3100, chroma 0x1900, stride 128, mv 0x1000);
  NAL_START x3 R2H 13 status 0x81 (decode+display), types I/P/I, consumed 18544 / 9 / 18535;
  frame hashes f0 0xd3990385/0x1eb1b40e, f1 same, f2 0xcabb3add/0x050d99fd = EXACTLY the generator / libavcodec
  values; LAST_FRAME R2H 13 status 0x3 (drained); CLOSE_INSTANCE R2H 6; ISR test: ISR ran 1 time, last R2H 6.
  => **the MFC hardware decodes H.264 bit-exactly from Windows.** Observations: firmware min plane sizes
  0x3040/0x1840 = plane + 0x40 (our +256 rule covers it); status bit7 set with display (0x81) - meaning not
  looked up. Start codes in the stream at 0 (SPS), 12 (PPS), 20 (IDR), 18556 (P): SEQ_HEADER consumed 13 and
  the first NAL_START 18544 (13 + 18544 = 18557) - consumption ends 1 byte into the next 4-byte start code,
  and the offset refeed works.
  Layout rule (recorded): every MFC buffer must lie above the RISC base (below it -> OPEN_INSTANCE ERR_RET 10).
- 15:03 phone back in TWRP after vidc4 (no crash). Starting V3 (user-mode IOCTL decode): MfcIoctl.h, session engine Decode.c.
- 15:15 V3 driver half committed ffe96ee (1.0.0.5, sys 8B42879D): Decode.c session engine (self-test + IOCTLs share
  it), MfcIoctl.h (OPEN / DECODE window -> packed NV12 / CLOSE, one session per handle), commands complete via the
  ISR/DPC + KEVENT once D0 is up (polled before), queue sequential at PASSIVE. Review: allocation is 16 + 112 + 1 =
  129 MiB (the commit message's "144 MiB" is wrong); self-test session excludes the 1 MiB staging tail (no DPB
  overlap); window limit = CPB 4 MiB - 512 on both sides. Known edge: a DPC arriving late from a TIMED-OUT command
  could complete the next command early (only after a timeout) - follow-up if timeouts ever show up.
- 15:12 vidc5: TWRP-deployed 1.0.0.5 (sys 8B42879D) + C:\S10eTmp\mfc {mfcdec.exe, c720/c1080/cbase.h264}, stay.flg set (Windows stays up for ssh). kd-vidc5.log.
