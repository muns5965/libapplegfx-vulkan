/* spv-ssa — applies the opt-in SSA promotion (src/vulkan/spv_ssa.c) to a SPIR-V file and prints the structure counters. usage: spv-ssa <in.spv> <out.spv> [mode 1..4 [max-phi-incoming]]
 * mode 1 = promotion only, 2 = + dead pure values, 3 = + unused Input loads, 4 = + array/struct locals (the LAGFX_SPV_SSA values). Exit 0 changed, 1 nothing to do, 2 error. SPDX-License-Identifier: MIT */
#include "vulkan/spv_ssa.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <in.spv> <out.spv> [mode 1..4 [max-phi-incoming]]\n", argv[0]); return 2; }
    unsigned mode = argc > 3 ? (unsigned)atoi(argv[3]) : 2u;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *w = malloc((size_t)n);
    if (!w || n % 4 || fread(w, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    unsigned fl = mode <= 1 ? 0u : mode == 2 ? LSSA_DCE : mode == 3 ? (LSSA_DCE | LSSA_DCE_INPUT) : (LSSA_DCE | LSSA_AGG);
    if (argc > 4 && atoi(argv[4]) > 0) fl |= LSSA_MAXPHI(atoi(argv[4]));
    uint32_t *o = NULL;
    size_t on = 0;
    lagfx_ssa_stats_t st;
    char why[256];
    int r = lagfx_spv_ssa_promote(w, (size_t)n / 4, fl, &o, &on, &st, why, sizeof why);
    printf("spv-ssa: rc=%d mode=%u vars=%u promoted=%u escaped=%u agg-skipped=%u wide-skipped=%u other-skipped=%u phis=%u (placed %u trivial %u dead %u) undefs=%u loads-removed=%u stores-removed=%u decl-removed=%u dce-removed=%u decos-dropped=%u inst %u->%u bytes %u->%u %s\n",
           r, mode, st.vars, st.promoted, st.escaped, st.skipped_agg, st.skipped_wide, st.skipped_type, st.phis_kept, st.phis_placed, st.phis_trivial, st.phis_dead, st.undefs, st.loads_removed, st.stores_removed, st.decl_removed,
           st.dce_removed, st.decos_dropped, st.inst_before, st.inst_after, st.words_before * 4, st.words_after * 4, r == 0 ? why : "");
    if (r != 1) return r == 0 ? 1 : 2;
    FILE *g = fopen(argv[2], "wb");
    if (!g || fwrite(o, 4, on, g) != on) return 2;
    fclose(g);
    return 0;
}
