/* Writes one DIAGNOSTIC SPIR-V variant of the pipeline repro's matrix (src/vulkan/spv_variants.c) for a given module, so the variant can be fed to spirv-val / spirv-dis or
 * to another Vulkan implementation. usage: spv-variant <in.spv> <e|f|g|h|i|j> <out.spv>. Exit: 0 = written, 3 = the variant does not apply to this module (nothing written),
 * 1 = malformed input / I/O error, 2 = usage. Never part of the normal translation path. SPDX-License-Identifier: MIT */
#include "vulkan/spv_variants.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 4 || strlen(argv[2]) != 1) { fprintf(stderr, "usage: %s <in.spv> <e|f|g|h|i|j> <out.spv>\n  e 64-bit dynamic index -> 32-bit | f drop unused capability decls | g OpUndef composites -> null\n  h OpUndef struct/pointer -> null (only) | i drop unreferenced OpUndef | j OpUndef struct -> null (only)\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *in = n > 0 && n % 4 == 0 ? malloc((size_t)n) : NULL;
    if (!in || fread(in, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "cannot read %s as SPIR-V words\n", argv[1]); fclose(f); free(in); return 1; }
    fclose(f);
    int (*fn)(const uint32_t *, size_t, uint32_t **, size_t *, char *, size_t) = NULL;
    switch (argv[2][0]) {
        case 'e': fn = lagfx_spv_variant_index32; break;
        case 'f': fn = lagfx_spv_variant_strip_caps; break;
        case 'g': fn = lagfx_spv_variant_undef_composites; break;
        case 'h': fn = lagfx_spv_variant_undef_struct_ptr; break;
        case 'i': fn = lagfx_spv_variant_drop_dead_undef; break;
        case 'j': fn = lagfx_spv_variant_undef_struct; break;
        default: fprintf(stderr, "unknown variant '%s'\n", argv[2]); free(in); return 2;
    }
    uint32_t *o = NULL; size_t on = 0; char info[400] = "";
    int r = fn(in, (size_t)n / 4, &o, &on, info, sizeof info);
    free(in);
    if (r < 0) { fprintf(stderr, "malformed module\n"); return 1; }
    if (r == 0 || !o) { printf("variant %s: %s (nothing written)\n", argv[2], info); return 3; }
    FILE *w = fopen(argv[3], "wb");
    if (!w || fwrite(o, 4, on, w) != on) { fprintf(stderr, "cannot write %s\n", argv[3]); if (w) fclose(w); free(o); return 1; }
    fclose(w); free(o);
    printf("variant %s: %d change(s): %s\n", argv[2], r, info);
    return 0;
}
