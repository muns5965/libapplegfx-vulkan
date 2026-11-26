/*
 * libapplegfx-vulkan — air.sample_texture_3d translation regression
 * tests/air2spv-texture3d-translate-unit.c
 *
 * SPDX-License-Identifier: MIT
 *
 * air.sample_texture_3d.<vty>(tex, samp, float3 coord, ...) must lower to a Dim3D image (OpTypeImage 3D, Sampled=1), an OpTypeSampledImage of it and an
 * OpImageSampleImplicitLod with a 3-component coordinate. Before, the 3D texture was declared as a 2D image and sampled through a 2D slice (the Z
 * component silently dropped).
 *
 * Fixture: uber_a2xghfc_231501.air.bc, the fragment function A2Xghfc of the 20261126-231501 device dump (library hash 35024e60e414abf4). It takes
 * `texture3d<half> color_cube0 / color_cube1 / lut_tex` entry arguments and calls UberShader::sample_3d_lut (a helper with a texture3d parameter) which
 * contains the only air.sample_texture_3d call of the dumps. The 2D samples of the same module must stay Dim2D.
 */
#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

enum { OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23, OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLED_IMAGE = 27, OP_SAMPLE_IMPLICIT = 87, OP_SAMPLE_EXPLICIT = 88,
       OP_SAMPLED_IMAGE = 86 };

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *b = malloc((size_t)sz);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
    fclose(f); *n = (size_t)sz; return b;
}

#define MAXID 1u << 16
typedef struct { uint32_t op, a, b, c, type; } rec_t;   /* per result id: its op, first operands, result type */

int main(void) {
    const char *dirs[] = { "tests/fixtures/", "../tests/fixtures/", SRCDIR "/fixtures/", NULL };
    uint8_t *air = NULL; size_t air_len = 0; char path[512];
    for (int i = 0; dirs[i] && !air; i++) { snprintf(path, sizeof path, "%suber_a2xghfc_231501.air.bc", dirs[i]); air = slurp(path, &air_len); }
    if (!air) { printf("FAIL: fixture not found\n"); return 1; }
    lagfx_air_module_t *m = NULL;
    if (lagfx_air_module_open(air, air_len, &m) != LAGFX_OK || !m) { printf("FAIL: module open\n"); return 1; }
    uint8_t *spv = NULL; size_t sz = 0;
    if (lagfx_air2spv_translate_module(m, &spv, &sz) != LAGFX_OK || !spv) { printf("FAIL: translate\n"); return 1; }

    const uint32_t *w = (const uint32_t *)(const void *)spv;
    size_t nw = sz / 4u, bound = w[3];
    rec_t *r = calloc(bound + 1u, sizeof *r);
    uint32_t img2d = 0, img3d = 0, simg3d = 0;
    int n_img2d_samp = 0, n_img3d_samp = 0, n_3d_vec3 = 0, n_3d_unresolved = 0, rc = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 5; i < nw;) {
            uint32_t op = w[i] & 0xffffu, wc = w[i] >> 16;
            if (!wc || i + wc > nw) break;
            const uint32_t *o = w + i + 1u;
            if (pass == 0) {
                switch (op) {
                    case OP_TYPE_FLOAT: r[o[0]] = (rec_t){ op, o[1], 0, 0, 0 }; break;
                    case OP_TYPE_VECTOR: r[o[0]] = (rec_t){ op, o[1], o[2], 0, 0 }; break;
                    case OP_TYPE_IMAGE:
                        r[o[0]] = (rec_t){ op, o[1], o[2], 0, 0 };
                        if (o[2] == 1 && o[6] == 1) img2d = o[0];   /* Dim2D, Sampled=1 */
                        if (o[2] == 2 && o[6] == 1) img3d = o[0];   /* Dim3D, Sampled=1 */
                        break;
                    case OP_TYPE_SAMPLED_IMAGE: r[o[0]] = (rec_t){ op, o[1], 0, 0, 0 }; if (o[1] == img3d && img3d) simg3d = o[0]; break;
                    case OP_SAMPLED_IMAGE: r[o[1]] = (rec_t){ op, 0, 0, 0, o[0] }; break;
                    default:   /* every other op that carries <result type, result id> */
                        if (wc >= 3 && (op == 1 || op == 12 || op == 55 || op == 57 || (op >= 43 && op <= 52) || op == 61 || (op >= 77 && op <= 83) ||
                                        (op >= 109 && op <= 245 && op != 224 && op != 225 && op != 228)))
                            r[o[1]] = (rec_t){ op, 0, 0, 0, o[0] };
                        break;
                }
            } else if (op == OP_SAMPLE_IMPLICIT || op == OP_SAMPLE_EXPLICIT) {
                uint32_t si = r[o[2]].type, coord = o[3];
                uint32_t sit = si;   /* the OpTypeSampledImage id */
                if (sit && r[sit].op == OP_TYPE_SAMPLED_IMAGE && r[sit].a == img3d && img3d) {
                    n_img3d_samp++;
                    uint32_t ct = r[coord].type;
                    if (!ct) n_3d_unresolved++;
                    else if (r[ct].op == OP_TYPE_VECTOR && r[ct].b == 3u) n_3d_vec3++;
                    else { printf("FAIL: a Dim3D sample has a coordinate that is not a 3-component vector (type %%%u)\n", ct); rc = 1; }
                } else if (sit && r[sit].op == OP_TYPE_SAMPLED_IMAGE) n_img2d_samp++;
            }
            i += wc;
        }
    }
    printf("OpTypeImage Dim3D=%s, OpTypeSampledImage(3D)=%s, 3D samples=%d (vec3 coord %d, coord type not tracked %d), 2D samples=%d\n",
           img3d ? "yes" : "no", simg3d ? "yes" : "no", n_img3d_samp, n_3d_vec3, n_3d_unresolved, n_img2d_samp);
    if (!img3d || !simg3d) { printf("FAIL: no Dim3D sampled image type (texture3d still declared as a 2D image)\n"); rc = 1; }
    if (n_img3d_samp < 1) { printf("FAIL: no OpImageSample on a Dim3D sampled image (sample_texture_3d fell back to a 2D slice / undef)\n"); rc = 1; }
    if (n_3d_vec3 < 1 || n_3d_unresolved) { printf("FAIL: expected every Dim3D sample coordinate to be a tracked vec3\n"); rc = 1; }
    if (!img2d || n_img2d_samp < 1) { printf("FAIL: the module's texture2d samples must stay Dim2D\n"); rc = 1; }
    if (rc == 0) printf("PASS: air.sample_texture_3d lowers to a Dim3D image sample\n");
    free(r); free(spv); lagfx_air_module_free(m); free(air);
    return rc;
}
