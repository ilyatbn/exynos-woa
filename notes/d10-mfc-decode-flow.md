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
