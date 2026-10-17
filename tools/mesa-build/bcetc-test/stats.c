/* stats - which EAC tables win for BC-derived data (G26 encoder tuning): BC3/BC4-like 8-level blocks, soft endpoints. */
#define BCETC_STANDALONE 1
#define BCETC_STATS 1
#include "BcEtc.c"
#include <stdio.h>
int main(void)
{
   unsigned r = 7;
   for (int kind = 0; kind < 3; kind++) {
      for (int b = 0; b < 200000; b++) {
         uint8_t blk[8];
         for (int i = 0; i < 8; i++) { r = r * 1103515245u + 12345u; blk[i] = (uint8_t)(r >> 16); }
         int a1 = blk[0] + (int)(blk[1] % 49) - 24;   /* soft: endpoints within 24 */
         if (b & 1) blk[1] = (uint8_t)(a1 < 0 ? 0 : a1 > 255 ? 255 : a1);   /* half soft, half random */
         int lev[8]; uint8_t lof[16], out[8];
         bc_alpha_pal(blk, kind, lev, lof);
         eac_from_levels(lev, lof, kind, out);
      }
      printf("kind %d:", kind);
      for (int t = 0; t < 16; t++) printf(" %u", bcetc_stats_table[kind][t]);
      printf("\n");
   }
   return 0;
}
