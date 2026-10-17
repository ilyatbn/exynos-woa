/* bench - time the BcEtc block encoders on the PC (G26): one 1024x1024 texture with mips = 87381 blocks each. */
#define BCETC_STANDALONE 1
#include "BcEtc.c"
#include <stdio.h>
#include <windows.h>
int main(void)
{
   enum { N = 87381 };
   static uint8_t px[N][64];
   static int av[N][16];
   unsigned r = 1;
   for (int b = 0; b < N; b++) {
      int c0[3], c1[3];
      for (int c = 0; c < 3; c++) { r = r * 1103515245u + 12345u; c0[c] = (r >> 16) % 224; c1[c] = c0[c] + (int)((r >> 8) % 32); }
      for (int i = 0; i < 16; i++) {
         r = r * 1103515245u + 12345u;
         int k = (r >> 16) % 4;
         for (int c = 0; c < 3; c++) px[b][i * 4 + c] = (uint8_t)((c0[c] * (3 - k) + c1[c] * k) / 3);
         px[b][i * 4 + 3] = 255;
         av[b][i] = 100 + (int)((r >> 4) % 40);
      }
   }
   LARGE_INTEGER f, t0, t1, t2, t3;
   QueryPerformanceFrequency(&f);
   uint8_t out[8];
   unsigned sink = 0;
   QueryPerformanceCounter(&t0);
   for (int b = 0; b < N; b++) { bcetc_encode_rgb(px[b], false, true, out); sink += out[3]; }
   QueryPerformanceCounter(&t1);
   for (int b = 0; b < N; b++) { bcetc_encode_rgb(px[b], true, false, out); sink += out[3]; }
   QueryPerformanceCounter(&t2);
   for (int b = 0; b < N; b++) { bcetc_encode_eac(av[b], 0, out); sink += out[1]; }
   QueryPerformanceCounter(&t3);
   double ms = 1000.0 / f.QuadPart;
   printf("per 1024x1024+mips (87381 blocks): rgb %.1f ms, rgb8a1 %.1f ms, eac %.1f ms (sink %u)\n",
          (t1.QuadPart - t0.QuadPart) * ms, (t2.QuadPart - t1.QuadPart) * ms, (t3.QuadPart - t2.QuadPart) * ms, sink);
   /* full transcode of soft BC data, 1024x1024 + mips, per format */
   static uint8_t bcsrc[1400000], etcdst[1400000];
   const enum pipe_format fm[3] = {PIPE_FORMAT_DXT1_RGBA, PIPE_FORMAT_DXT5_RGBA, PIPE_FORMAT_RGTC2_UNORM};
   const char *nm[3] = {"BC1", "BC3", "BC5"};
   for (int f = 0; f < 3; f++) {
      unsigned bs = f ? 16 : 8;
      for (unsigned i = 0; i < sizeof bcsrc; i++) { r = r * 1103515245u + 12345u; bcsrc[i] = (uint8_t)(r >> 16); }
      for (unsigned b = 0; b + bs <= sizeof bcsrc; b += bs) {   /* soft: close endpoints */
         uint8_t *c = bcsrc + b + (f == 1 ? 8 : 0);
         if (f != 2) { unsigned c0 = c[0] | (c[1] << 8), c1 = c0 ^ (c[2] & 0x0861); c[2] = (uint8_t)c1; c[3] = (uint8_t)(c1 >> 8); }
         if (f >= 1) { int a1 = bcsrc[b] + (int)(bcsrc[b + 1] % 49) - 24; bcsrc[b + 1] = (uint8_t)(a1 < 0 ? 0 : a1 > 255 ? 255 : a1); }
      }
      QueryPerformanceCounter(&t0);
      size_t off = 0;
      for (unsigned m = 0, mw = 1024; m < 11; m++, mw = mw > 1 ? mw / 2 : 1) {
         unsigned bw = (mw + 3) / 4;
         bcetc_transcode(fm[f], bcetc_target(fm[f]), etcdst + off, bw * bs, bcsrc + off, bw * bs, mw, mw);
         off += (size_t)bw * bw * bs;
      }
      QueryPerformanceCounter(&t1);
      printf("transcode %s 1024x1024+mips: %.1f ms\n", nm[f], (t1.QuadPart - t0.QuadPart) * ms);
   }
   return 0;
}
