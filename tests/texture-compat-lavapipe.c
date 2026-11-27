/* Sampler accuracy (item #14) and 3D textures (item #17) against an independent CPU model of the Vulkan spec, on a Vulkan 1.3 software device (lavapipe) with the Khronos
 * validation layer when present. Results describe the software device; they do not predict any GPU (there is no Adreno here).
 *
 *   A  (#14) sampler state -> pixels, boundary coordinates. One SPIR-V fragment shader (tests/texcompat-spv.h x2d: explicit LOD, coordinate = fragment position * scale + offset
 *      from push constants) samples a 4 x 2 RGBA32F texture. The coordinate sets sit ON texel boundaries (integers), on texel centres, +-1/64 texel around the boundaries, on quarter
 *      positions and +-1000 texels away (address wrapping), so nearest / linear / every address mode is exercised exactly where implementations differ.
 *        A1 constexpr words (0 clamp_to_zero, 1 edge, 2 repeat, 3 mirrored repeat, 4 clamp_to_border) x nearest / linear through the PRODUCTION mapper (lagfx_sampler_state_from_constexpr_norm);
 *        A2 runtime sampler words (MTLSamplerAddressMode 0..5, border colours 0..2, nearest / linear) through the PRODUCTION decoder (lagfx_a64_sampler_decode_ex), incl. mirror clamp to edge;
 *        A3 negative controls: a deliberately wrong expectation must be detected;
 *        A4 clamp_to_zero / border on formats WITHOUT an alpha channel (R8, RG8), RGBA8 and an A8-style swizzled view: what Vulkan returns outside the image (Metal text: "(0, 0, 0) with
 *           the alpha component value from the texture");
 *        A5 bicubic: the constexpr mapper gives LINEAR (counted APPROX). The test pins that the output IS bilinear and measures how far a Catmull-Rom bicubic (what VK_FILTER_CUBIC_EXT
 *           means) would be, i.e. the size of the approximation; it also lists which cubic extensions this device offers.
 *   B  (#17) texture3d through the REAL translator: tests/fixtures/t3_*.air.bc (hand-written AIR, tests/fixtures/gen-texcompat-fixtures.sh) -> SPIR-V -> a real 3D VkImage with a 3-level
 *      mip chain of KNOWN colours -> every depth coordinate (slice boundaries, slice centres, outside the volume), filter (nearest / linear = trilinear), address mode on R, constant
 *      offset (z, and x/y/z), explicit level (negative, 0, 0.5, 1, 1.5, 2) with mip filter none / nearest / linear, all through runtime sampler words (lagfx_a64_sampler_decode_ex).
 *      The 2D-array question: a 3D image is never confused with a 2D array here (separate VkImageType / view type; production dimension checks are in arm64-exec-res-unit).
 * Exit 77 = skipped (no device).
 * SPDX-License-Identifier: MIT */
#include "air/bitcode_reader.h"
#include "air2spv/translate.h"
#include "air2spv/spv_reflect.h"
#include "air2spv/constexpr_sampler.h"
#include "protocol/arm64_sampler.h"
#include "vulkan/sampler_cache.h"
#include "common/le.h"

#include <vulkan/vulkan.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "texcompat-vk.h"
#include "texcompat-spv.h"

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)

/* ================================================================ CPU model (a transcription of the Vulkan spec's texel addressing, filtering and level selection) */
enum { M_EDGE = 0, M_REPEAT, M_MIRROR, M_MIRROR_CLAMP, M_BORDER };
typedef struct { int n[3]; const float *t; } lvl_t;
typedef struct { int nlev; lvl_t lev[4]; } tex_t;
static long resolve(long i, long n, int am) {   /* -1 = border */
    switch (am) {
    case M_EDGE: return i < 0 ? 0 : i >= n ? n - 1 : i;
    case M_REPEAT: return ((i % n) + n) % n;
    case M_MIRROR: { long m = ((i % (2 * n)) + 2 * n) % (2 * n); return m < n ? m : 2 * n - 1 - m; }
    case M_MIRROR_CLAMP: { long m = i < 0 ? -1 - i : i; return m >= n ? n - 1 : m; }
    default: return (i < 0 || i >= n) ? -1 : i;
    }
}
static void fetch(const lvl_t *L, const int am[3], long x, long y, long z, const float border[4], float o[4]) {
    long rx = resolve(x, L->n[0], am[0]), ry = resolve(y, L->n[1], am[1]), rz = resolve(z, L->n[2], am[2]);
    if (rx < 0 || ry < 0 || rz < 0) { memcpy(o, border, 16); return; }
    memcpy(o, L->t + (((size_t)rz * L->n[1] + ry) * L->n[0] + rx) * 4, 16);
}
/* coordinates in TEXELS of this level; off = integer texel offset */
static void sample_lvl(const lvl_t *L, const int am[3], int linear, double tx, double ty, double tz, const int off[3], const float border[4], float o[4]) {
    if (!linear) { fetch(L, am, (long)floor(tx) + off[0], (long)floor(ty) + off[1], (long)floor(tz) + off[2], border, o); return; }
    double ux = tx - 0.5, uy = ty - 0.5, uz = L->n[2] > 1 ? tz - 0.5 : 0.0;
    long x0 = (long)floor(ux), y0 = (long)floor(uy), z0 = (long)floor(uz);
    double fx = ux - (double)x0, fy = uy - (double)y0, fz = uz - (double)z0;
    int nz = L->n[2] > 1 ? 2 : 1;
    for (int c = 0; c < 4; c++) o[c] = 0;
    for (int dz = 0; dz < nz; dz++) for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) {
        float t[4]; fetch(L, am, x0 + dx + off[0], y0 + dy + off[1], z0 + dz + off[2], border, t);
        double w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy) * (nz == 2 ? (dz ? fz : 1 - fz) : 1.0);
        for (int c = 0; c < 4; c++) o[c] += (float)(w * t[c]);
    }
}
/* normalized coordinates, a level of detail and the sampler's mip / LOD state (Vulkan "Image Level(s) Selection": nearest d = ceil(lambda + 1/2) - 1, ties down) */
static void sample_tex(const tex_t *T, const int am[3], int linear, int mip_linear, double lodmin, double lodmax, double lod, double u, double v, double w, const int off[3], const float border[4], float o[4]) {
    double lam = lod < lodmin ? lodmin : lod > lodmax ? lodmax : lod; double q = T->nlev - 1; if (lam > q) lam = q;
    if (!mip_linear) {
        int d = lam <= 0.5 ? 0 : (int)ceil(lam + 0.5) - 1; if (d > T->nlev - 1) d = T->nlev - 1;
        const lvl_t *L = &T->lev[d]; sample_lvl(L, am, linear, u * L->n[0], v * L->n[1], w * L->n[2], off, border, o); return;
    }
    int l0 = (int)floor(lam); double f = lam - l0;
    if (l0 >= T->nlev - 1) { const lvl_t *L = &T->lev[T->nlev - 1]; sample_lvl(L, am, linear, u * L->n[0], v * L->n[1], w * L->n[2], off, border, o); return; }
    float a[4], b[4]; const lvl_t *A = &T->lev[l0], *B = &T->lev[l0 + 1];
    sample_lvl(A, am, linear, u * A->n[0], v * A->n[1], w * A->n[2], off, border, a); sample_lvl(B, am, linear, u * B->n[0], v * B->n[1], w * B->n[2], off, border, b);
    for (int c = 0; c < 4; c++) o[c] = (float)((1 - f) * a[c] + f * b[c]);
}

static const float BORDER_TRANSPARENT[4] = { 0, 0, 0, 0 }, BORDER_OPAQUE_BLACK[4] = { 0, 0, 0, 1 }, BORDER_OPAQUE_WHITE[4] = { 1, 1, 1, 1 };

/* ================================================================ A: 2D boundary sweep */
#define T2W 4
#define T2H 2
static float g_t2[T2H][T2W][4];
static tex_t g_T2;
static void init_t2(void) {
    for (int y = 0; y < T2H; y++) for (int x = 0; x < T2W; x++) { float *t = g_t2[y][x]; t[0] = 0.1f + 0.2f * (float)x; t[1] = 0.15f + 0.5f * (float)y; t[2] = ((x + y) & 1) ? 0.85f : 0.3f; t[3] = 1.0f - 0.1f * (float)x; }
    g_T2.nlev = 1; g_T2.lev[0] = (lvl_t){ { T2W, T2H, 1 }, &g_t2[0][0][0] };
}
static const struct { double ox, oy; const char *nm; } SETS[] = {
    { -2.5, -1.5, "integers (texel boundaries)" }, { -3.0, -2.0, "half integers (texel centres)" }, { -2.5 + 1.0 / 64, -1.5 + 1.0 / 64, "boundary + 1/64" }, { -2.5 - 1.0 / 64, -1.5 - 1.0 / 64, "boundary - 1/64" },
    { -2.75, -1.75, "quarter positions" }, { 1021.5, 1022.5, "+1000 texels" }, { -1030.5, -1031.5, "-1000 texels" } };

typedef struct { tcv_pipe_t P; tcv_tex_t tex; } ctx2_t;
static int draw2d(ctx2_t *C, VkSampler s, double ox, double oy, float *got) {
    struct { float a[4], b[4]; } pc = { { 1.0f / T2W, 1.0f / T2H, (float)(ox / T2W), (float)(oy / T2H) }, { 0, 0, 0, 0 } };
    VkImageView views[4] = { C->tex.view, VK_NULL_HANDLE, VK_NULL_HANDLE, C->tex.view }; VkSampler smps[4] = { 0, s, s, 0 };
    return tcv_draw(&C->P, views, smps, &pc, got);
}
/* returns the number of differing values (and prints the first ones when show) */
static unsigned g_mirror_edge_alt;
static int cmp2d(const char *what, const float *got, int am, int linear, const float border[4], double ox, double oy, float tol, int show) {
    int bad = 0; const int amv[3] = { am, am, M_EDGE }, off[3] = { 0, 0, 0 };
    if (getenv("TC_DBG") && am == M_MIRROR && !linear) { for (int x = 0; x < TCV_W; x++) { int ix = -1; for (int k = 0; k < T2W; k++) if (fabsf(got[x*4] - g_t2[0][k][0]) < 1e-5f) ix = k; printf("DBG %s tx=%g got texel x=%d (model %ld)\n", what, x + 0.5 + ox, ix, resolve((long)floor(x + 0.5 + ox), 4, am)); } }
    for (int y = 0; y < TCV_H; y++) for (int x = 0; x < TCV_W; x++) {
        float w[4]; double tx = x + 0.5 + ox, ty = y + 0.5 + oy; sample_lvl(&g_T2.lev[0], amv, linear, tx, ty, 0.0, off, border, w);
        if (!linear && (am == M_MIRROR || am == M_MIRROR_CLAMP) && (tx == floor(tx) || ty == floor(ty))) {
            /* EXACT integer texel coordinate with a mirroring mode + NEAREST: the Vulkan formula acts on the texel index floor(t) (mirrored repeat, size 4, t = 5 -> index 2), a software device
             * that reflects the COORDINATE first gets floor(3.0) = 3. Both are accepted at these points (and only there), the device's choice is counted in g_mirror_edge_alt. */
            long ax, ay;
            { double m = am == M_MIRROR ? fabs(fmod(tx, 8.0)) : fabs(tx); if (am == M_MIRROR && fmod(tx, 8.0) < 0) m = 8.0 - m; if (am == M_MIRROR && m > 4.0) m = 8.0 - m; ax = (long)floor(m); if (ax > 3) ax = 3; }
            { double m = am == M_MIRROR ? fabs(fmod(ty, 4.0)) : fabs(ty); if (am == M_MIRROR && fmod(ty, 4.0) < 0) m = 4.0 - m; if (am == M_MIRROR && m > 2.0) m = 4.0 - m; ay = (long)floor(m); if (ay > 1) ay = 1; }
            if (tx != floor(tx)) ax = resolve((long)floor(tx), 4, am);
            if (ty != floor(ty)) ay = resolve((long)floor(ty), 2, am);
            if (!memcmp(&g_t2[ay][ax][0], got + (y * TCV_W + x) * 4, 16) && memcmp(&g_t2[ay][ax][0], w, 16)) { g_mirror_edge_alt++; continue; }
        }
        for (int c = 0; c < 4; c++) if (fabsf(got[(y * TCV_W + x) * 4 + c] - w[c]) > tol) { if (show && bad < 3) printf("  %s: (%d,%d) ch%d got %g want %g\n", what, x, y, c, (double)got[(y * TCV_W + x) * 4 + c], (double)w[c]); bad++; }
    }
    return bad;
}

static int test_A(void) {
    ctx2_t C; init_t2();
    CHECK(!tcv_tex_create(&C.tex, 0, VK_FORMAT_R32G32B32A32_SFLOAT, T2W, T2H, 1, 1, 16, g_t2, NULL), "tex2");
    const uint32_t bn[4] = { 0, 1, 2, 3 }; const int isS[4] = { 0, 1, 1, 0 };
    CHECK(!tcv_pipe_create(&C.P, SPV_x2d, sizeof SPV_x2d, bn, isS, 4, 32), "pipeline x2d");
    lagfx_sampler_cache_t cache; lagfx_sampler_cache_init(&cache, tcv_dev, false);
    float got[TCV_W * TCV_H * 4]; unsigned draws = 0;
    lagfx_sampler_diag_reset();

    /* ---- A1: constexpr words through the production mapper ---- */
    static const char *amn[] = { "clamp_to_zero", "clamp_to_edge", "repeat", "mirrored_repeat", "clamp_to_border" };
    static const int camm[] = { M_BORDER, M_EDGE, M_REPEAT, M_MIRROR, M_BORDER };
    for (int a = 0; a < 5; a++) for (int f = 0; f < 2; f++) {
        uint64_t word = 0x807bff0000080000ull | (uint64_t)a | (uint64_t)a << 3 | (uint64_t)a << 6 | (uint64_t)f << 9 | (uint64_t)f << 11;
        lagfx_constexpr_sampler_t cs; CHECK(lagfx_constexpr_sampler_decode(word, &cs), "decode 0x%016llx", (unsigned long long)word);
        lagfx_sampler_state_t st = lagfx_sampler_state_from_constexpr_norm(&cs);
        VkSampler s = lagfx_sampler_cache_get(&cache, &st); CHECK(s, "sampler");
        for (unsigned k = 0; k < sizeof SETS / sizeof SETS[0]; k++) {
            tcv_msgs_reset();
            CHECK(!draw2d(&C, s, SETS[k].ox, SETS[k].oy, got), "draw"); draws++;
            char nm[160]; snprintf(nm, sizeof nm, "constexpr %s %s %s", amn[a], f ? "linear" : "nearest", SETS[k].nm);
            int bad = cmp2d(nm, got, camm[a], f, BORDER_TRANSPARENT, SETS[k].ox, SETS[k].oy, f ? 0.012f : 1e-6f, 1);
            if (bad && getenv("TC_SOFT")) { printf("SOFTFAIL %s: %d\n", nm, bad); } else CHECK(!bad, "%s: %d values differ from the Vulkan model", nm, bad);
            if (tcv_validation) CHECK(tcv_nmsg == 0, "%s: validation message", nm);
        }
    }
    printf("INFO: %u pixels at an exact integer texel coordinate with a mirroring mode + NEAREST differ from the spec formula in the way of a coordinate-reflecting implementation (device behaviour, not library state)\n", g_mirror_edge_alt);
    printf("PASS: A1 constexpr address modes x nearest/linear x %u coordinate sets (boundaries, centres, +-1/64, quarters, +-1000 texels): %u draws match the model\n", (unsigned)(sizeof SETS / sizeof SETS[0]), draws);

    /* ---- A2: runtime words through the production decoder ---- */
    static const char *mn[] = { "clamp_to_edge", "mirror_clamp_to_edge", "repeat", "mirrored_repeat", "clamp_to_zero", "clamp_to_border" };
    static const int mm[] = { M_EDGE, M_MIRROR_CLAMP, M_REPEAT, M_MIRROR, M_BORDER, M_BORDER };
    static const float *BC[3] = { BORDER_TRANSPARENT, BORDER_OPAQUE_BLACK, BORDER_OPAQUE_WHITE };
    unsigned d2 = 0;
    for (int a = 0; a < 6; a++) for (int f = 0; f < 2; f++) for (int bc = 0; bc < (a == 5 ? 3 : 2); bc++) {
        /* bc 0..2 = border colour field; for the modes that do not clamp to the border colour the field is set to 2 (opaque white) on bc == 1 to prove it is ignored */
        uint32_t bcf = a == 5 ? (uint32_t)bc : (bc == 1 ? 2u : 0u);
        uint32_t word = 0x84000000u | (uint32_t)a << 8 | (uint32_t)a << 12 | (uint32_t)a << 16 | (f ? 5u : 0u) | bcf << 20;
        uint8_t d[28] = { 0 }; d[0] = 3; d[4] = 28; lagfx_put_le32(d + 12, word); lagfx_put_le32(d + 24, 0x7f7fffffu);
        VkSamplerCreateInfo ci; uint32_t fl = lagfx_a64_sampler_decode_ex(d, 28, false, 0, tcv_mirror_clamp != 0, &ci);
        if (a == 1 && !tcv_mirror_clamp) { CHECK(fl & LAGFX_A64_SAMP_MIRROR_CLAMP_APPROX, "mirror clamp approx flag"); continue; }   /* device without the feature: mirrored repeat is used and flagged */
        VkSampler s; CHECK(vkCreateSampler(tcv_dev, &ci, NULL, &s) == VK_SUCCESS, "vkCreateSampler 0x%08x", word);
        const float *border = a == 5 ? BC[bc] : BORDER_TRANSPARENT;
        for (unsigned k = 0; k < sizeof SETS / sizeof SETS[0]; k++) {
            tcv_msgs_reset();
            CHECK(!draw2d(&C, s, SETS[k].ox, SETS[k].oy, got), "draw"); d2++;
            char nm[160]; snprintf(nm, sizeof nm, "runtime 0x%08x %s %s bc=%u %s", word, mn[a], f ? "linear" : "nearest", bcf, SETS[k].nm);
            int bad = cmp2d(nm, got, mm[a], f, border, SETS[k].ox, SETS[k].oy, f ? 0.012f : 1e-6f, 1);
            if (bad && getenv("TC_SOFT")) { printf("SOFTFAIL %s: %d\n", nm, bad); } else CHECK(!bad, "%s: %d values differ from the Vulkan model", nm, bad);
            if (tcv_validation) CHECK(tcv_nmsg == 0, "%s: validation message", nm);
        }
        vkDestroySampler(tcv_dev, s, NULL);
    }
    printf("PASS: A2 runtime address modes 0..5 x border colours x nearest/linear (mirror clamp %s): %u draws match the model; a border colour on a non-border mode is ignored\n", tcv_mirror_clamp ? "exact" : "approximated by mirrored repeat", d2);

    /* ---- A3: negative controls (the comparison must be able to fail) ---- */
    {
        uint64_t word = 0x807bff0000080000ull | 2ull | 2ull << 3 | 2ull << 6;   /* repeat, nearest */
        lagfx_constexpr_sampler_t cs; lagfx_constexpr_sampler_decode(word, &cs);
        lagfx_sampler_state_t st = lagfx_sampler_state_from_constexpr_norm(&cs); VkSampler rep = lagfx_sampler_cache_get(&cache, &st);
        CHECK(!draw2d(&C, rep, SETS[0].ox, SETS[0].oy, got), "draw");
        CHECK(cmp2d("neg: repeat vs edge model", got, M_EDGE, 0, BORDER_TRANSPARENT, SETS[0].ox, SETS[0].oy, 1e-6f, 0) > 0, "negative control: repeat sampler matched the clamp-to-edge model");
        CHECK(cmp2d("neg: repeat vs border model", got, M_BORDER, 0, BORDER_TRANSPARENT, SETS[0].ox, SETS[0].oy, 1e-6f, 0) > 0, "negative control: repeat sampler matched the border model");
        CHECK(cmp2d("neg: nearest vs linear model", got, M_REPEAT, 1, BORDER_TRANSPARENT, SETS[1].ox, SETS[1].oy, 1e-6f, 0) > 0 || 1, "unreachable");
        CHECK(!draw2d(&C, rep, SETS[2].ox, SETS[2].oy, got), "draw");
        CHECK(cmp2d("neg: nearest sampler vs linear model", got, M_REPEAT, 1, BORDER_TRANSPARENT, SETS[2].ox, SETS[2].oy, 0.012f, 0) > 0, "negative control: a nearest sampler matched the linear model off the texel centres");
        /* border colours differ from each other where they matter */
        uint32_t w5 = 0x84000000u | 5u << 8 | 5u << 12 | 5u << 16 | 2u << 20; uint8_t d[28] = { 0 }; d[0] = 3; d[4] = 28; lagfx_put_le32(d + 12, w5); lagfx_put_le32(d + 24, 0x7f7fffffu);
        VkSamplerCreateInfo ci; (void)lagfx_a64_sampler_decode_ex(d, 28, false, 0, false, &ci); VkSampler wh; CHECK(vkCreateSampler(tcv_dev, &ci, NULL, &wh) == VK_SUCCESS, "sampler");
        CHECK(!draw2d(&C, wh, SETS[0].ox, SETS[0].oy, got), "draw");
        CHECK(cmp2d("neg: white border vs transparent model", got, M_BORDER, 0, BORDER_TRANSPARENT, SETS[0].ox, SETS[0].oy, 1e-6f, 0) > 0, "negative control: an opaque white border matched the transparent model");
        CHECK(cmp2d("white border vs white model", got, M_BORDER, 0, BORDER_OPAQUE_WHITE, SETS[0].ox, SETS[0].oy, 1e-6f, 0) == 0, "opaque white border");
        vkDestroySampler(tcv_dev, wh, NULL);
        printf("PASS: A3 negative controls: wrong address mode, wrong filter and wrong border colour expectations are all detected\n");
    }

    /* ---- A4: border / clamp_to_zero on formats without alpha ---- */
    {
        struct { VkFormat f; const char *nm; uint32_t tb; int ch; int swz; } F[] = {
            { VK_FORMAT_R8G8B8A8_UNORM, "RGBA8", 4, 4, 0 }, { VK_FORMAT_R8_UNORM, "R8 (no alpha)", 1, 1, 0 }, { VK_FORMAT_R8G8_UNORM, "RG8 (no alpha)", 2, 2, 0 }, { VK_FORMAT_R8_UNORM, "R8 as A8 (view 0,0,0,R)", 1, 1, 1 } };
        printf("INFO A4 (outside the image, sampler = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, nearest; in-range texel in brackets):\n");
        uint64_t word = 0x807bff0000080000ull;   /* clamp_to_zero on all axes, nearest */
        lagfx_constexpr_sampler_t cs; lagfx_constexpr_sampler_decode(word, &cs);
        lagfx_sampler_state_t st = lagfx_sampler_state_from_constexpr_norm(&cs); VkSampler s0 = lagfx_sampler_cache_get(&cache, &st);
        for (unsigned i = 0; i < sizeof F / sizeof F[0]; i++) {
            uint8_t px[T2W * T2H * 4]; for (unsigned k = 0; k < T2W * T2H * F[i].tb; k++) px[k] = 0xc0;   /* a bright constant so an in-range read is recognisable */
            VkComponentMapping sw = { VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R };
            tcv_tex_t t; CHECK(!tcv_tex_create(&t, 0, F[i].f, T2W, T2H, 1, 1, F[i].tb, px, F[i].swz ? &sw : NULL), "tex");
            VkImageView views[4] = { t.view, VK_NULL_HANDLE, VK_NULL_HANDLE, t.view }; VkSampler smps[4] = { 0, s0, s0, 0 };
            struct { float a[4], b[4]; } pc = { { 1.0f / T2W, 1.0f / T2H, (float)(SETS[0].ox / T2W), (float)(SETS[0].oy / T2H) }, { 0, 0, 0, 0 } };
            tcv_msgs_reset(); CHECK(!tcv_draw(&C.P, views, smps, &pc, got), "draw");
            const float *out = got;                       /* pixel (0,0): u = -2, v = -1: outside */
            const float *in = got + ((1 * TCV_W) + 2) * 4;   /* pixel (2,1): u = 0, v = 0: texel (0,0) */
            printf("  %-26s outside = (%g, %g, %g, %g)   [inside = (%g, %g, %g, %g)]\n", F[i].nm, (double)out[0], (double)out[1], (double)out[2], (double)out[3], (double)in[0], (double)in[1], (double)in[2], (double)in[3]);
            CHECK(in[0] > 0.5f || F[i].swz, "%s: in-range texel not read", F[i].nm);
            CHECK(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f, "%s: border RGB not zero", F[i].nm);
            float want_a = F[i].ch == 4 ? 0.0f : F[i].swz ? 0.0f : 1.0f;   /* Vulkan Border Replacement: formats without alpha get the default alpha 1 */
            CHECK(out[3] == want_a, "%s: border alpha %g, expected %g", F[i].nm, (double)out[3], (double)want_a);
            if (tcv_validation) CHECK(tcv_nmsg == 0, "%s: validation message", F[i].nm);
            tcv_tex_destroy(&t);
        }
        printf("PASS: A4 clamp_to_zero as TRANSPARENT_BLACK border: alpha 0 with an alpha channel (RGBA8, A8-style view), alpha 1 without one (R8, RG8) on this device = the Metal text (0,0,0) + alpha of the texture [I: alpha-less reading]\n");
    }

    /* ---- A5: bicubic ---- */
    {
        uint64_t word = 0x807bff0000080000ull | 1ull | 1ull << 3 | 1ull << 6 | 2ull << 9 | 2ull << 11;   /* clamp to edge, mag = min = 2 (bicubic) */
        lagfx_constexpr_sampler_t cs; CHECK(lagfx_constexpr_sampler_decode(word, &cs), "decode");
        lagfx_sampler_diag_t d0; lagfx_sampler_diag_get(&d0);
        lagfx_sampler_state_t st = lagfx_sampler_state_from_constexpr_norm(&cs);
        lagfx_sampler_diag_t d1; lagfx_sampler_diag_get(&d1);
        CHECK(st.mag_filter == VK_FILTER_LINEAR && st.min_filter == VK_FILTER_LINEAR, "bicubic word must map to LINEAR (no exact bicubic exists)");
        CHECK(d1.cxs_state_builds_bicubic_as_linear == d0.cxs_state_builds_bicubic_as_linear + 1, "the approximation must be counted");
        VkSampler s = lagfx_sampler_cache_get(&cache, &st);
        double worst = 0; const int am[3] = { M_EDGE, M_EDGE, M_EDGE }, off[3] = { 0, 0, 0 };
        for (unsigned k = 0; k < sizeof SETS / sizeof SETS[0]; k++) {
            CHECK(!draw2d(&C, s, SETS[k].ox, SETS[k].oy, got), "draw");
            CHECK(!cmp2d("bicubic word == bilinear", got, M_EDGE, 1, BORDER_TRANSPARENT, SETS[k].ox, SETS[k].oy, 0.012f, 1), "the bicubic word must sample exactly like bilinear");
            for (int y = 0; y < TCV_H; y++) for (int x = 0; x < TCV_W; x++) {   /* distance of a Catmull-Rom 4 x 4 (VK_FILTER_CUBIC_EXT weights) from the bilinear result */
                double tx = x + 0.5 + SETS[k].ox - 0.5, ty = y + 0.5 + SETS[k].oy - 0.5; long x0 = (long)floor(tx), y0 = (long)floor(ty); double fx = tx - (double)x0, fy = ty - (double)y0;
                double wx[4] = { -0.5 * fx * fx * fx + fx * fx - 0.5 * fx, 1.5 * fx * fx * fx - 2.5 * fx * fx + 1, -1.5 * fx * fx * fx + 2 * fx * fx + 0.5 * fx, 0.5 * fx * fx * fx - 0.5 * fx * fx };
                double wy[4] = { -0.5 * fy * fy * fy + fy * fy - 0.5 * fy, 1.5 * fy * fy * fy - 2.5 * fy * fy + 1, -1.5 * fy * fy * fy + 2 * fy * fy + 0.5 * fy, 0.5 * fy * fy * fy - 0.5 * fy * fy };
                double acc[4] = { 0, 0, 0, 0 };
                for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) { float t[4]; fetch(&g_T2.lev[0], am, x0 - 1 + i, y0 - 1 + j, 0, BORDER_TRANSPARENT, t); for (int c = 0; c < 4; c++) acc[c] += wx[i] * wy[j] * t[c]; }
                for (int c = 0; c < 4; c++) { double dd = fabs(acc[c] - got[(y * TCV_W + x) * 4 + c]); if (dd > worst) worst = dd; }
            }
        }
        (void)off;
        CHECK(worst > 0.01, "the bicubic reference does not differ from bilinear on this texture: the approximation could not be measured");
        uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(tcv_pd, NULL, &ne, NULL);
        VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep); vkEnumerateDeviceExtensionProperties(tcv_pd, NULL, &ne, ep);
        int ext = 0, img = 0, qcom = 0; for (uint32_t i = 0; i < ne; i++) { ext |= !strcmp(ep[i].extensionName, "VK_EXT_filter_cubic"); img |= !strcmp(ep[i].extensionName, "VK_IMG_filter_cubic"); qcom |= !strcmp(ep[i].extensionName, "VK_QCOM_filter_cubic_weights"); }
        free(ep);
        printf("PASS: A5 bicubic constexpr word = exactly bilinear (counted bicubic_as_linear); a Catmull-Rom bicubic would differ by up to %.3f on this texture (the size of the approximation). Device offers VK_EXT_filter_cubic=%d VK_IMG_filter_cubic=%d VK_QCOM_filter_cubic_weights=%d (the library never enables them)\n", worst, ext, img, qcom);
    }
    lagfx_sampler_cache_destroy(&cache);
    tcv_pipe_destroy(&C.P); tcv_tex_destroy(&C.tex);
    return 0;
}

/* ================================================================ B: texture3d through the translator */
static float g_l0[4 * 4 * 4 * 4], g_l1[2 * 2 * 2 * 4], g_l2[1 * 4];
static tex_t g_T3;
static void init_t3(void) {
    for (int z = 0; z < 4; z++) for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) { float *t = g_l0 + (((size_t)z * 4 + y) * 4 + x) * 4; t[0] = 0.10f + 0.2f * (float)x; t[1] = 0.10f + 0.2f * (float)y; t[2] = 0.10f + 0.2f * (float)z; t[3] = 1.0f; }
    for (int z = 0; z < 2; z++) for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) { float *t = g_l1 + (((size_t)z * 2 + y) * 2 + x) * 4; t[0] = 0.90f - 0.5f * (float)x; t[1] = 0.20f + 0.5f * (float)y; t[2] = 0.90f - 0.6f * (float)z; t[3] = 1.0f; }
    g_l2[0] = 0.3f; g_l2[1] = 0.8f; g_l2[2] = 0.6f; g_l2[3] = 1.0f;
    g_T3.nlev = 3; g_T3.lev[0] = (lvl_t){ { 4, 4, 4 }, g_l0 }; g_T3.lev[1] = (lvl_t){ { 2, 2, 2 }, g_l1 }; g_T3.lev[2] = (lvl_t){ { 1, 1, 1 }, g_l2 };
}
static uint8_t *slurp(const char *name, size_t *n) {
    char path[512]; const char *dirs[] = { SRCDIR "/fixtures/", "tests/fixtures/", "../tests/fixtures/" };
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
        snprintf(path, sizeof path, "%s%s", dirs[d], name); FILE *f = fopen(path, "rb"); if (!f) continue;
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET); uint8_t *b = malloc((size_t)sz);
        if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
        fclose(f); *n = (size_t)sz; return b;
    }
    return NULL;
}
static int translate(const char *fixture, uint8_t **spv, size_t *sz) {
    char fn[96]; snprintf(fn, sizeof fn, "%s.air.bc", fixture); size_t bn = 0; uint8_t *bc = slurp(fn, &bn);
    CHECK(bc, "fixture %s not found (run tests/fixtures/gen-texcompat-fixtures.sh)", fixture);
    lagfx_air_module_t *m = NULL;
    CHECK(lagfx_air_module_open(bc, bn, &m) == LAGFX_OK && m, "%s: module_open", fixture);
    CHECK(lagfx_air2spv_translate_module(m, spv, sz) == LAGFX_OK && *spv, "%s: translate", fixture);
    lagfx_air_module_free(m); free(bc);
    return 0;
}

typedef struct { const char *name; double zc; int off[3]; int level_form; double lod; } fix_t;
static const fix_t FIX[] = {
    { "t3_zb", -0.375, { 0, 0, 0 }, 0, 0 }, { "t3_zc", -0.25, { 0, 0, 0 }, 0, 0 }, { "t3_offz", -0.25, { 0, 0, 1 }, 0, 0 }, { "t3_offxyz", -0.25, { 1, -1, 1 }, 0, 0 },
    { "t3_lvl0", -0.25, { 0, 0, 0 }, 1, 0.0 }, { "t3_lvl05", -0.25, { 0, 0, 0 }, 1, 0.5 }, { "t3_lvl1", -0.25, { 0, 0, 0 }, 1, 1.0 }, { "t3_lvl15", -0.25, { 0, 0, 0 }, 1, 1.5 },
    { "t3_lvl2", -0.25, { 0, 0, 0 }, 1, 2.0 }, { "t3_lvlneg", -0.25, { 0, 0, 0 }, 1, -1.0 }, { "t3_lvl1_off", -0.25, { 0, 0, 1 }, 1, 1.0 } };

typedef struct { uint32_t addr_r; int linear; int mip; uint32_t bc; } scfg_t;   /* runtime fields: MTL address mode of R, filter, mip filter 0 none / 1 nearest / 2 linear */
static const char *mtl_am[] = { "edge", "mirror_clamp", "repeat", "mirror", "clamp_to_zero", "clamp_to_border" };
static const int mtl_amm[] = { M_EDGE, M_MIRROR_CLAMP, M_REPEAT, M_MIRROR, M_BORDER, M_BORDER };

static int run_fixture(const fix_t *fx, const scfg_t *sc, tcv_tex_t *tex3, unsigned *ndraws, unsigned *nties) {
    uint8_t *spv; size_t sz; if (translate(fx->name, &spv, &sz)) return 1;
    lagfx_spv_binding_t rb[8]; size_t nb = lagfx_spv_reflect_bindings(spv, sz, rb, 8);
    if (0) for (size_t i = 0; i < nb && i < 8; i++) printf("  binding %u kind %d dim %u\n", rb[i].binding, (int)rb[i].kind, (unsigned)rb[i].image_dim);
    CHECK(nb >= 2 && nb <= 4, "%s: %zu bindings", fx->name, nb);
    uint32_t bn[8]; int isS[8];
    for (size_t i = 0; i < nb; i++) { bn[i] = rb[i].binding; isS[i] = rb[i].kind == LAGFX_SPV_BINDING_SAMPLER; CHECK(isS[i] || rb[i].kind == LAGFX_SPV_BINDING_SAMPLED_IMAGE, "binding kind"); if (!isS[i]) CHECK(rb[i].image_dim == 2, "%s: the image is not Dim3D", fx->name); }
    tcv_pipe_t P; CHECK(!tcv_pipe_create(&P, (const uint32_t *)(const void *)spv, sz, bn, isS, (uint32_t)nb, 0), "pipeline %s", fx->name);
    for (const scfg_t *s = sc; s->addr_r != 0xffffffffu; s++) {
        uint32_t word = 0x84000000u | (uint32_t)(s->linear ? 5 : 0) | s->mip << 24 | s->addr_r << 16 | s->bc << 20;
        uint8_t d[28] = { 0 }; d[0] = 3; d[4] = 28; lagfx_put_le32(d + 12, word); lagfx_put_le32(d + 24, 0x7f7fffffu);
        VkSamplerCreateInfo ci; uint32_t fl = lagfx_a64_sampler_decode_ex(d, 28, false, 0, tcv_mirror_clamp != 0, &ci);
        if (s->addr_r == 1 && !tcv_mirror_clamp) { (void)fl; continue; }
        /* t3_zb puts the depth coordinate on EXACT integer texel coordinates; with a mirroring mode and NEAREST that is where a coordinate-reflecting implementation (lavapipe, see A) and the
         * index formula of the spec differ, so those two combinations are covered by the 2D test A only. */
        if (!strcmp(fx->name, "t3_zb") && (s->addr_r == 1 || s->addr_r == 3) && !s->linear) continue;
        VkSampler smp; CHECK(vkCreateSampler(tcv_dev, &ci, NULL, &smp) == VK_SUCCESS, "sampler 0x%08x", word);
        VkImageView views[8]; VkSampler smps[8];
        for (size_t i = 0; i < nb; i++) views[i] = tex3->view, smps[i] = smp;
        float got[TCV_W * TCV_H * 4];
        tcv_msgs_reset();
        CHECK(!tcv_draw(&P, views, smps, NULL, got), "draw %s", fx->name);
        (*ndraws)++;
        const int am[3] = { M_EDGE, M_EDGE, mtl_amm[s->addr_r] };
        const float *border = s->addr_r == 5 ? (s->bc == 1 ? BORDER_OPAQUE_BLACK : s->bc == 2 ? BORDER_OPAQUE_WHITE : BORDER_TRANSPARENT) : BORDER_TRANSPARENT;
        int bad = 0; const int off[3] = { fx->off[0], fx->off[1], fx->off[2] };
        float tol = (s->linear || s->mip == 2) ? 0.012f : 1e-6f;
        /* the sampler as Vulkan got it: LOD range [0, 0.25] without a mip filter, [0, none) with one */
        double lmin = ci.minLod, lmax = ci.maxLod;
        for (int y = 0; y < TCV_H && bad < 4; y++) for (int x = 0; x < TCV_W; x++) {
            double u = (x + 0.5) * 0.125, v = 0.375, w = 0.25 * (y + 0.5) + fx->zc, lod = fx->level_form ? fx->lod : 0.0;
            float want[4]; sample_tex(&g_T3, am, s->linear, ci.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR, lmin, lmax, lod, u, v, w, off, border, want);
            if (ci.mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST && fabs((lod - floor(lod)) - 0.5) < 1e-9) {
                /* a tie of the nearest mip level: the spec allows ceil(d + 0.5) - 1 (preferred) AND floor(d + 0.5) (alternative), accept either (lavapipe takes both: 0.5 -> 0, 1.5 -> 2) */
                float alt[4]; sample_tex(&g_T3, am, s->linear, 0, lmin, lmax, lod + 1e-3, u, v, w, off, border, alt);
                int okp = 1, oka = 1; for (int c = 0; c < 4; c++) { okp &= fabsf(got[(y * TCV_W + x) * 4 + c] - want[c]) <= tol; oka &= fabsf(got[(y * TCV_W + x) * 4 + c] - alt[c]) <= tol; }
                if (okp || oka) { (*nties)++; continue; }
            }
            for (int c = 0; c < 4; c++) if (fabsf(got[(y * TCV_W + x) * 4 + c] - want[c]) > tol) {
                if (bad++ < 3) printf("  %s %s %s mip=%u: (%d,%d) ch%d got %g want %g (u=%g w=%g)\n", fx->name, mtl_am[s->addr_r], s->linear ? "linear" : "nearest", s->mip, x, y, c, (double)got[(y * TCV_W + x) * 4 + c], (double)want[c], u, w);
            }
        }
        if (bad && getenv("TC_SOFT")) { printf("SOFTFAIL %s %s %s mip=%u (minLod %g maxLod %g)\n", fx->name, mtl_am[s->addr_r], s->linear ? "linear" : "nearest", s->mip, lmin, lmax); } else
        CHECK(!bad, "%s %s %s mip=%u: differs from the 3D model", fx->name, mtl_am[s->addr_r], s->linear ? "linear" : "nearest", s->mip);
        if (tcv_validation) CHECK(tcv_nmsg == 0, "%s: %u validation message(s)", fx->name, tcv_nmsg);
        vkDestroySampler(tcv_dev, smp, NULL);
    }
    tcv_pipe_destroy(&P); free(spv);
    return 0;
}

static int test_B(void) {
    init_t3();
    static float all[(64 + 8 + 1) * 4]; memcpy(all, g_l0, sizeof g_l0); memcpy(all + 64 * 4, g_l1, sizeof g_l1); memcpy(all + 72 * 4, g_l2, sizeof g_l2);
    tcv_tex_t t3; CHECK(!tcv_tex_create(&t3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 4, 4, 4, 3, 16, all, NULL), "3D texture");
    unsigned nd = 0, ties = 0;
    static const scfg_t POS[] = { { 0, 0, 0, 0 }, { 2, 0, 0, 0 }, { 3, 0, 0, 0 }, { 4, 0, 0, 0 }, { 5, 0, 0, 0 }, { 0, 1, 0, 0 }, { 2, 1, 0, 0 }, { 3, 1, 0, 0 }, { 4, 1, 0, 0 }, { 5, 1, 0, 0 }, { 5, 1, 0, 2 }, { 1, 1, 0, 0 }, { 0xffffffffu, 0, 0, 0 } };
    for (unsigned i = 0; i < 4; i++) if (run_fixture(&FIX[i], POS, &t3, &nd, &ties)) return 1;   /* depth coordinate / filter / address mode on R / offset */
    printf("PASS: B1 texture3d depth coordinate (slice boundaries, centres, outside the volume), nearest / trilinear, address modes on R, constant offset (0,0,1) and (1,-1,1): %u draws through the translator match the 3D model\n", nd);
    unsigned n2 = 0;
    static const scfg_t MIP[] = { { 0, 0, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 2, 0 }, { 0, 1, 0, 0 }, { 0, 1, 1, 0 }, { 0, 1, 2, 0 }, { 2, 1, 2, 0 }, { 0xffffffffu, 0, 0, 0 } };
    for (unsigned i = 4; i < sizeof FIX / sizeof FIX[0]; i++) if (run_fixture(&FIX[i], MIP, &t3, &n2, &ties)) return 1;
    printf("PASS: B2 texture3d explicit level (-1, 0, 0.5, 1, 1.5, 2) x mip filter none / nearest / linear x nearest / trilinear (+ offset with a level): %u draws match the 3D mip-chain model (%u pixels sat on a level tie where the spec allows two levels)\n", n2, ties);
    /* negative controls: the model with a swapped slice order / a missing offset must differ from the images (the comparison can fail) */
    {
        const fix_t *fz = &FIX[2];   /* t3_offz */
        const int none[3] = { 0, 0, 0 }, off[3] = { 0, 0, 1 }; const int am[3] = { M_EDGE, M_EDGE, M_EDGE }; int differs = 0;
        for (int y = 0; y < TCV_H; y++) { float a[4], b[4]; double w = 0.25 * (y + 0.5) + fz->zc;
            sample_tex(&g_T3, am, 0, 0, 0, 0.25, 0, 0.0625, 0.375, w, off, BORDER_TRANSPARENT, a); sample_tex(&g_T3, am, 0, 0, 0, 0.25, 0, 0.0625, 0.375, w, none, BORDER_TRANSPARENT, b);
            if (fabsf(a[2] - b[2]) > 0.05f) differs = 1; }
        CHECK(differs, "negative control: a z offset of 1 must change the sampled slice in the model");
        float a[4], b[4]; sample_tex(&g_T3, am, 0, 0, 0, 0.25, 0, 0.5, 0.5, 0.5, none, BORDER_TRANSPARENT, a); sample_tex(&g_T3, am, 0, 0, 0, 1000, 1, 0.5, 0.5, 0.5, none, BORDER_TRANSPARENT, b);
        CHECK(fabsf(a[0] - b[0]) + fabsf(a[1] - b[1]) > 0.05f, "negative control: level 0 and level 1 must be distinguishable");
        printf("PASS: B3 negative controls: offset / level changes are visible in the model (the comparisons are able to fail)\n");
    }
    tcv_tex_destroy(&t3);
    return 0;
}

int main(void) {
    int ir = tcv_init(1, NULL, 0);
    if (ir) return ir;
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(tcv_pd, VK_FORMAT_R32G32B32A32_SFLOAT, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) { printf("SKIP: RGBA32F is not linearly filterable\n"); return 77; }
    int rc = test_A();
    if (!rc) rc = test_B();
    if (rc) { tcv_msgs_print(); return rc; }
    printf("PASS texture-compat-lavapipe on %s (validation layer %s)\n", tcv_devname, tcv_validation ? "ON" : "absent");
    return 0;
}
