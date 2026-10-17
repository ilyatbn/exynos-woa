/*
 * bcetc_test - PC test of the UMD's BC -> ETC2 / EAC block encoders (Mesa d3d10umd BcEtc.c, G26,
 * notes/g26-bc-etc2.md): encodes solid, gradient, edge, noise and punch-through blocks, decodes them with a reference
 * decoder ported from Mesa (src/mesa/main/texcompress_etc.c: individual / differential / punch-through RGB, EAC alpha8 /
 * R11 / signed R11) and prints PSNR per case; fails if a transparent texel is not transparent (or the reverse), if the
 * decoder lands in a T / H / planar block, or if a case is below its PSNR floor.
 * Build: build-bcetc-test.cmd <mesa tree>. SPDX-License-Identifier: MIT
 */
#define BCETC_STANDALONE 1
#include "BcEtc.c"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static const int ref_etc1[8][4] = {
   {2, 8, -2, -8}, {5, 17, -5, -17}, {9, 29, -9, -29}, {13, 42, -13, -42},
   {18, 60, -18, -60}, {24, 80, -24, -80}, {33, 106, -33, -106}, {47, 183, -47, -183},
};
static const int ref_nonopaque[8][4] = {
   {0, 8, 0, -8}, {0, 17, 0, -17}, {0, 29, 0, -29}, {0, 42, 0, -42},
   {0, 60, 0, -60}, {0, 80, 0, -80}, {0, 106, 0, -106}, {0, 183, 0, -183},
};

static int bad_mode;

/* Mesa etc2_rgb8_parse_block + etc2_rgb8_fetch_texel, individual / differential only. out: 16 RGBA, row-major. */
static void
ref_rgb(const uint8_t *src, int punch, uint8_t out[64])
{
   static const int lookup[8] = {0, 1, 2, 3, -4, -3, -2, -1};
   int diffbit = (src[3] >> 1) & 1, opaque = 1;
   if (punch) {
      opaque = diffbit;
      diffbit = 1;
   }
   int base[2][3];
   if (!diffbit) {
      for (int c = 0; c < 3; c++) {
         base[0][c] = (src[c] & 0xf0) | (src[c] >> 4);
         base[1][c] = ((src[c] & 0xf) << 4) | (src[c] & 0xf);
      }
   } else {
      for (int c = 0; c < 3; c++) {
         int sum = (src[c] >> 3) + lookup[src[c] & 7];
         if (sum < 0 || sum > 31)
            bad_mode++;   /* T / H / planar: the encoder must never make this */
         base[0][c] = (src[c] & 0xf8) | (src[c] >> 5);
         int lo = sum & 31;
         base[1][c] = (lo << 3) | (lo >> 2);
      }
   }
   const int *tab[2];
   int t0 = (src[3] >> 5) & 7, t1 = (src[3] >> 2) & 7;
   tab[0] = (!punch || opaque) ? ref_etc1[t0] : ref_nonopaque[t0];
   tab[1] = (!punch || opaque) ? ref_etc1[t1] : ref_nonopaque[t1];
   int flip = src[3] & 1;
   uint32_t w = ((uint32_t)src[4] << 24) | (src[5] << 16) | (src[6] << 8) | src[7];
   for (int y = 0; y < 4; y++) {
      for (int x = 0; x < 4; x++) {
         int bit = y + x * 4;
         int idx = ((w >> (15 + bit)) & 2) | ((w >> bit) & 1);
         uint8_t *d = out + (y * 4 + x) * 4;
         if (punch && !opaque && idx == 2) {
            d[0] = d[1] = d[2] = d[3] = 0;
            continue;
         }
         int blk = flip ? (y >= 2) : (x >= 2);
         for (int c = 0; c < 3; c++)
            d[c] = (uint8_t)clampi(base[blk][c] + tab[blk][idx], 0, 255);
         d[3] = 255;
      }
   }
}

/* Mesa etc2_alpha8 / r11 / signed_r11 fetch. */
static void
ref_eac(const uint8_t *src, int kind, int out[16])
{
   int base = kind == 2 ? (int)(int8_t)src[0] : src[0];
   if (kind == 2 && base == -128)
      base = -127;
   int mult = src[1] >> 4, t = src[1] & 15;
   uint64_t w = 0;
   for (int i = 0; i < 6; i++)
      w = (w << 8) | src[2 + i];
   for (int y = 0; y < 4; y++) {
      for (int x = 0; x < 4; x++) {
         int idx = (int)((w >> (((3 - y) + (3 - x) * 4) * 3)) & 7);
         int m = eac_tab[t][idx];
         int v;
         if (kind == 0)
            v = clampi(base + m * mult, 0, 255);
         else if (kind == 1)
            v = clampi(base * 8 + 4 + (mult ? m * mult * 8 : m), 0, 2047);
         else
            v = clampi(base * 8 + (mult ? m * mult * 8 : m), -1023, 1023);
         out[y * 4 + x] = v;
      }
   }
}

static unsigned rng = 12345;
static int
rnd(int n)
{
   rng = rng * 1103515245u + 12345u;
   return (int)((rng >> 16) % (unsigned)n);
}

static double
psnr(double sse, double n, double peak)
{
   return sse <= 0 ? 99.0 : 10.0 * log10(peak * peak * n / sse);
}

static int failures;

static void
rgb_case(const char *name, int punch, int allow_ind, int blocks, double floor_db,
         void (*gen)(uint8_t rgba[64], int b))
{
   double sse = 0, n = 0;
   int trans_bad = 0;
   for (int b = 0; b < blocks; b++) {
      uint8_t in[64], enc[8], out[64];
      gen(in, b);
      bcetc_encode_rgb(in, punch != 0, allow_ind != 0, enc);
      ref_rgb(enc, punch, out);
      for (int i = 0; i < 16; i++) {
         int ta = punch && in[i * 4 + 3] < 128, tb = out[i * 4 + 3] == 0;
         if (ta != tb)
            trans_bad++;
         if (ta)
            continue;
         for (int c = 0; c < 3; c++) {
            double d = (double)in[i * 4 + c] - out[i * 4 + c];
            sse += d * d;
         }
         n += 3;
      }
   }
   double db = psnr(sse, n, 255);
   int ok = db >= floor_db && !trans_bad;
   failures += !ok;
   printf("%-26s %6.2f dB (floor %.0f)%s%s\n", name, db, floor_db, trans_bad ? "  TRANSPARENCY MISMATCH" : "",
          ok ? "" : "  FAIL");
}

static void
eac_case(const char *name, int kind, int blocks, double floor_db, void (*gen)(int v[16], int b, int kind))
{
   double sse = 0, n = 0, peak = kind == 0 ? 255 : kind == 1 ? 2047 : 2046;
   for (int b = 0; b < blocks; b++) {
      int in[16], out[16];
      uint8_t enc[8];
      gen(in, b, kind);
      bcetc_encode_eac(in, kind, enc);
      ref_eac(enc, kind, out);
      for (int i = 0; i < 16; i++) {
         double d = (double)in[i] - out[i];
         sse += d * d;
         n++;
      }
   }
   double db = psnr(sse, n, peak);
   int ok = db >= floor_db;
   failures += !ok;
   printf("%-26s %6.2f dB (floor %.0f)%s\n", name, db, floor_db, ok ? "" : "  FAIL");
}

/* --- generators --- */
static void
g_solid(uint8_t p[64], int b)
{
   int r = rnd(256), g = rnd(256), bl = rnd(256);
   (void)b;
   for (int i = 0; i < 16; i++) {
      p[i * 4] = (uint8_t)r; p[i * 4 + 1] = (uint8_t)g; p[i * 4 + 2] = (uint8_t)bl; p[i * 4 + 3] = 255;
   }
}
static void
g_grad(uint8_t p[64], int b)
{
   int r = rnd(200), g = rnd(200), bl = rnd(200), dx = rnd(16), dy = rnd(16);
   (void)b;
   for (int y = 0; y < 4; y++)
      for (int x = 0; x < 4; x++) {
         int i = y * 4 + x, s = x * dx + y * dy;
         p[i * 4] = (uint8_t)clampi(r + s, 0, 255);
         p[i * 4 + 1] = (uint8_t)clampi(g + s / 2, 0, 255);
         p[i * 4 + 2] = (uint8_t)clampi(bl + s / 3, 0, 255);
         p[i * 4 + 3] = 255;
      }
}
/* a BC1-like block: every texel one of 4 colours on a line between two endpoints (what decoded BC1 looks like) */
static void
g_bc1like(uint8_t p[64], int b)
{
   int c0[3] = {rnd(256), rnd(256), rnd(256)}, c1[3] = {rnd(256), rnd(256), rnd(256)};
   (void)b;
   for (int i = 0; i < 16; i++) {
      int k = rnd(4);
      for (int c = 0; c < 3; c++)
         p[i * 4 + c] = (uint8_t)((c0[c] * (3 - k) + c1[c] * k) / 3);
      p[i * 4 + 3] = 255;
   }
}
static void
g_bc1like_soft(uint8_t p[64], int b)   /* endpoints close together: the common case in real textures */
{
   int c0[3] = {rnd(224), rnd(224), rnd(224)}, c1[3];
   (void)b;
   for (int c = 0; c < 3; c++)
      c1[c] = c0[c] + rnd(32);
   for (int i = 0; i < 16; i++) {
      int k = rnd(4);
      for (int c = 0; c < 3; c++)
         p[i * 4 + c] = (uint8_t)((c0[c] * (3 - k) + c1[c] * k) / 3);
      p[i * 4 + 3] = 255;
   }
}
static void
g_punch(uint8_t p[64], int b)
{
   g_bc1like_soft(p, b);
   for (int i = 0; i < 16; i++)
      if (rnd(3) == 0) {
         p[i * 4] = p[i * 4 + 1] = p[i * 4 + 2] = 0;
         p[i * 4 + 3] = 0;
      }
}
static void
e_smooth(int v[16], int b, int kind)
{
   int lo = kind == 2 ? -1023 : 0, hi = kind == 0 ? 255 : kind == 1 ? 2047 : 1023;
   int base = lo + rnd(hi - lo + 1), d = rnd(kind == 0 ? 12 : 96) - (kind == 0 ? 6 : 48);
   (void)b;
   for (int y = 0; y < 4; y++)
      for (int x = 0; x < 4; x++)
         v[y * 4 + x] = clampi(base + (x + y) * d, lo, hi);
}
static void
e_bc4like(int v[16], int b, int kind)   /* 8 levels between two endpoints, as BC4 decodes */
{
   int lo = kind == 2 ? -1023 : 0, hi = kind == 0 ? 255 : kind == 1 ? 2047 : 1023;
   int a = lo + rnd(hi - lo + 1), c = lo + rnd(hi - lo + 1);
   (void)b;
   for (int i = 0; i < 16; i++) {
      int k = rnd(8);
      v[i] = (a * (7 - k) + c * k) / 7;
   }
}
static void
e_flat(int v[16], int b, int kind)
{
   int lo = kind == 2 ? -1023 : 0, hi = kind == 0 ? 255 : kind == 1 ? 2047 : 1023;
   int x = lo + rnd(hi - lo + 1);
   (void)b;
   for (int i = 0; i < 16; i++)
      v[i] = x;
}

int
main(void)
{
   const int N = 20000;
   rgb_case("rgb solid", 0, 1, N, 38, g_solid);
   rgb_case("rgb gradient", 0, 1, N, 34, g_grad);
   rgb_case("rgb bc1-like soft", 0, 1, N, 34, g_bc1like_soft);
   rgb_case("rgb bc1-like hard", 0, 1, N, 17, g_bc1like);   /* random far-apart endpoints: the ETC1-mode limit (T/H modes: follow-up) */
   rgb_case("rgb8a1 opaque soft", 1, 0, N, 33, g_bc1like_soft);
   rgb_case("rgb8a1 punch-through", 1, 0, N, 30, g_punch);
   eac_case("eac alpha flat", 0, N, 99, e_flat);
   eac_case("eac alpha smooth", 0, N, 40, e_smooth);
   eac_case("eac alpha bc4-like", 0, N, 26, e_bc4like);
   eac_case("r11 unsigned flat", 1, N, 60, e_flat);
   eac_case("r11 unsigned smooth", 1, N, 40, e_smooth);
   eac_case("r11 unsigned bc4-like", 1, N, 26, e_bc4like);
   eac_case("r11 signed smooth", 2, N, 40, e_smooth);
   eac_case("r11 signed bc4-like", 2, N, 26, e_bc4like);
   printf("T/H/planar blocks produced: %d\n", bad_mode);
   failures += bad_mode != 0;
   printf("%s\n", failures ? "BCETC_TEST FAIL" : "BCETC_TEST PASS");
   return failures ? 1 : 0;
}
