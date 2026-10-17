# G19 - lower (scaled) desktop resolutions, e.g. 720x1520 on the 1080x2280 panel

User 2026-10-17: "yes to attempting to add a lower 720p resolution scale to see if that helps later in games".

## Facts so far
- The panel (S6E3FA7, command-mode DSI) runs only at its native 1080x2280; MaliG76Wddm exposes ONE source mode and
  one target mode (display.c header: "one source, one target, one mode, identity transforms").
- Present path today: DWM's 1080x2280 primary -> UMD d3d10_scanout_present GPU blit into the scan-out BO (1:1), or
  the rotated path (landscape), or the KMD CPU copy (DoPresent). A smaller source needs a stretch in that blit.
- Candidate mode: 720x1520 (exactly 2/3 of 1080x2280, same 19:9 aspect).

## Plan
1. MS Learn: how a WDDM driver offers a source mode smaller than the target (VidPN present path scaling,
   D3DKMDT_VPPS_*, source/target mode sets in EnumVidPnCofuncModality / IsSupportedVidPn / CommitVidPn).
2. KMD display.c: add the scaled source mode(s) + path scaling support.
3. UMD d3d10_gdi.c: stretch blit (LINEAR) when the primary is smaller than the scan-out; KMD DoPresent fallback.
4. Test with nogpu.flg recovery ready (a VidPN mistake can leave the display black at boot).

## Progress log
