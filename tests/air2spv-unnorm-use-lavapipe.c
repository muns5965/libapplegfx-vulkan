/* Unnormalized-coordinates samplers converted BY USE (item #15): lagfx_spv_unnorm_rewrite follows sample -> OpSampledImage -> OpLoad -> OpVariable -> Binding and rewrites only the
 * samples whose sampler is one of the unnormalized bindings of the draw.
 *
 *   Part 1 (pure, no device): every shader of tests/texcompat-spv.h through the rewriter: return code, report counters, the resulting instruction stream (opcode / image-operand mask),
 *          samplers that are NOT in the set stay untouched, unsupported uses (Dref, 3D image, ...) are refused with a count, unresolved samplers follow the policy argument. The
 *          rewritten module is also run through spirv-val when the tool exists (N otherwise).
 *   Part 2 (Vulkan 1.3 software device, Khronos validation layer when present): the same shaders drawn with REAL unnormalized VkSamplers (the captured runtime words 0x04000000 /
 *          0x04000005 through lagfx_a64_sampler_decode, plus a synthetic clamp-to-border one) against an independent CPU model of Vulkan's unnormalized sampling
 *          (texel = floor(u) + offset, edge / border addressing, bilinear on u - 0.5). Controls:
 *            positive: each use form (implicit, bias, const offset, bias + offset, explicit LOD, grad + offset) matches the model after the rewrite and produces NO validation message;
 *            negative: the UNREWRITTEN shader triggers VUID-vkCmdDraw-None-08610 (implicit LOD) / -08611 (bias or offset) on the same sampler; the model with the offset ignored differs
 *                      from the model with it (the test would notice a dropped offset); the legacy whole-module rewrite changes a NORMALIZED sampler's implicit-LOD sample (mip level) while
 *                      the per-use rewrite keeps it.
 * Everything here describes a software device (lavapipe); it says nothing about any GPU driver. Exit 77 = skipped (no device).
 * SPDX-License-Identifier: MIT */
#include "air2spv/spv_reflect.h"
#include "protocol/arm64_sampler.h"
#include "common/le.h"

#include <vulkan/vulkan.h>
#include <math.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "texcompat-vk.h"
#include "texcompat-spv.h"

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)
#define SPVLEN(a) (sizeof(a))

/* ------------------------------------------------------------------ stream inspection helpers */
static uint32_t count_op(const uint8_t *spv, size_t n, uint32_t op) { uint32_t c = 0; for (size_t i = 5; i < n / 4;) { uint32_t h = lagfx_le32(spv + i * 4), wc = h >> 16; if (!wc) break; if ((h & 0xffff) == op) c++; i += wc; } return c; }
/* image-operand mask of the first sample instruction with opcode `op` (0xffffffff when absent, 0 when it has none) */
static uint32_t first_mask(const uint8_t *spv, size_t n, uint32_t op) { for (size_t i = 5; i < n / 4;) { uint32_t h = lagfx_le32(spv + i * 4), wc = h >> 16; if (!wc) break; if ((h & 0xffff) == op) return wc > 5 ? lagfx_le32(spv + (i + 5) * 4) : 0; i += wc; } return 0xffffffffu; }

static int spirv_val(const uint8_t *spv, size_t n, const char *tag) {   /* 0 ok, 1 invalid, 2 tool missing */
    char path[128]; snprintf(path, sizeof path, "/tmp/lagfx-unnorm-use-%d-%s.spv", (int)getpid(), tag);
    FILE *f = fopen(path, "wb"); if (!f) return 2; fwrite(spv, 1, n, f); fclose(f);
    char cmd[256]; snprintf(cmd, sizeof cmd, "spirv-val --target-env vulkan1.0 %s >/dev/null 2>&1", path);
    int rc = system(cmd); remove(path);
    if (rc == -1) return 2;
    return WIFEXITED(rc) && WEXITSTATUS(rc) == 127 ? 2 : (rc == 0 ? 0 : 1);
}
static int g_val_missing;

typedef struct { const char *name; const uint8_t *spv; size_t n; } shader_t;
#define SH(x) { #x, (const uint8_t *)SPV_##x, sizeof SPV_##x }

static int run_one(const shader_t *s, const uint32_t *bind, uint32_t nb, int conv_unres, int want_rc, uint8_t **out, size_t *on, lagfx_spv_unnorm_report_t *rep) {
    int rc = lagfx_spv_unnorm_rewrite(s->spv, s->n, bind, nb, conv_unres, out, on, rep);
    CHECK(rc == want_rc, "%s bindings{%u..} unres=%d: rc %d, want %d", s->name, nb ? bind[0] : 0, conv_unres, rc, want_rc);
    if (rc == 1) {
        CHECK(*out && *on > 0 && rep->module_changed, "%s: rewritten module missing", s->name);
        int v = spirv_val(*out, *on, s->name);
        if (v == 2) g_val_missing = 1; else CHECK(v == 0, "%s: the rewritten module fails spirv-val", s->name);
    } else CHECK(!*out, "%s: module returned without a change", s->name);
    return 0;
}

static int part1(void) {
    static const shader_t SH_[] = { SH(u_impl), SH(u_bias), SH(u_off), SH(u_biasoff), SH(u_expl), SH(u_expl_off), SH(u_gradoff), SH(u_mixed), SH(u_unused), SH(u_dref), SH(u_img3d), SH(u_3dB_2dA), SH(u_unres) };
    const shader_t *S[16]; for (unsigned i = 0; i < sizeof SH_ / sizeof SH_[0]; i++) S[i] = &SH_[i];
    const shader_t *impl = S[0], *bias = S[1], *off = S[2], *biasoff = S[3], *expl = S[4], *expl_off = S[5], *gradoff = S[6], *mixed = S[7], *unused = S[8], *dref = S[9], *img3d = S[10], *m3d = S[11], *unres = S[12];
    const uint32_t A[] = { 1 }, B[] = { 2 }, AB[] = { 1, 2 }, NONE[] = { 7 };
    uint8_t *o; size_t on; lagfx_spv_unnorm_report_t r;

    if (run_one(impl, A, 1, 1, 1, &o, &on, &r)) return 1;
    CHECK(r.implicit_to_lod0 == 1 && r.uses_unnorm == 1 && count_op(o, on, 87) == 0 && count_op(o, on, 88) == 1 && first_mask(o, on, 88) == 0x2, "u_impl: implicit -> explicit Lod 0");
    free(o);
    if (run_one(impl, NONE, 1, 1, 0, &o, &on, &r)) return 1;   /* negative control: a binding that is not the sampler's: the module is untouched */
    CHECK(r.uses_unnorm == 0 && r.samples_total == 1, "u_impl with an unrelated binding must not count the sample");
    if (run_one(impl, B, 1, 1, 0, &o, &on, &r)) return 1;
    if (run_one(bias, A, 1, 1, 1, &o, &on, &r)) return 1;
    CHECK(r.bias_dropped == 1 && r.implicit_to_lod0 == 1 && first_mask(o, on, 88) == 0x2, "u_bias: Bias dropped, Lod 0");
    free(o);
    if (run_one(off, A, 1, 1, 1, &o, &on, &r)) return 1;
    CHECK(r.offset_folded == 1 && count_op(o, on, 129) >= 1 && count_op(o, on, 111) == 1 && first_mask(o, on, 88) == 0x2, "u_off: ConstOffset folded into the coordinate (OpConvertSToF + OpFAdd), mask = Lod only");
    free(o);
    if (run_one(biasoff, A, 1, 1, 1, &o, &on, &r)) return 1;
    CHECK(r.bias_dropped == 1 && r.offset_folded == 1 && first_mask(o, on, 88) == 0x2, "u_biasoff");
    free(o);
    if (run_one(expl, A, 1, 1, 0, &o, &on, &r)) return 1;   /* already valid: left as it is, only counted */
    CHECK(r.explicit_kept == 1 && r.implicit_to_lod0 == 0, "u_expl is kept");
    if (run_one(expl_off, A, 1, 1, 1, &o, &on, &r)) return 1;
    CHECK(r.offset_folded == 1 && r.explicit_kept == 1 && first_mask(o, on, 88) == 0x2, "u_expl_off: offset folded, Lod kept");
    free(o);
    if (run_one(gradoff, A, 1, 1, 1, &o, &on, &r)) return 1;
    CHECK(r.offset_folded == 1 && r.grad_kept == 1 && first_mask(o, on, 88) == 0x4, "u_gradoff: Grad kept, offset folded");
    free(o);
    /* per use: the sampler of the OTHER binding is not converted */
    if (run_one(mixed, A, 1, 1, 1, &o, &on, &r)) return 1;
    CHECK(r.samples_total == 2 && r.uses_unnorm == 1 && r.implicit_to_lod0 == 1 && count_op(o, on, 87) == 1 && count_op(o, on, 88) == 1, "u_mixed {A}: exactly one of the two samples rewritten");
    free(o);
    if (run_one(mixed, AB, 2, 1, 1, &o, &on, &r)) return 1;
    CHECK(r.uses_unnorm == 2 && count_op(o, on, 87) == 0 && count_op(o, on, 88) == 2, "u_mixed {A,B}: both rewritten");
    free(o);
    if (run_one(mixed, B, 1, 1, 1, &o, &on, &r)) return 1;
    CHECK(r.uses_unnorm == 1 && count_op(o, on, 87) == 1 && count_op(o, on, 88) == 1, "u_mixed {B}: only the B sample rewritten");
    free(o);
    if (run_one(unused, A, 1, 1, 0, &o, &on, &r)) return 1;   /* the false skip of the old code: a pixel sampler is bound but the shader never samples through it */
    CHECK(r.uses_unnorm == 0 && r.samples_total == 1, "u_unused: no use of the pixel sampler -> rc 0 (draw must not be skipped)");
    /* unsupported uses: refused with a count */
    if (run_one(dref, A, 1, 1, -1, &o, &on, &r)) return 1;
    CHECK(r.unsup_dref == 1, "u_dref counted");
    if (run_one(dref, B, 1, 1, 0, &o, &on, &r)) return 1;
    if (run_one(img3d, A, 1, 1, -1, &o, &on, &r)) return 1;
    CHECK(r.unsup_image_dim == 1, "u_img3d counted");
    if (run_one(img3d, B, 1, 1, 0, &o, &on, &r)) return 1;
    if (run_one(m3d, A, 1, 1, 1, &o, &on, &r)) return 1;   /* 2D through A is rewritten, the 3D sample through B is left alone */
    CHECK(r.implicit_to_lod0 == 1 && count_op(o, on, 87) == 1 && count_op(o, on, 88) == 1, "u_3dB_2dA {A}");
    free(o);
    if (run_one(m3d, B, 1, 1, -1, &o, &on, &r)) return 1;   /* B (3D) unnormalized: Vulkan forbids it */
    CHECK(r.unsup_image_dim == 1, "u_3dB_2dA {B} counted");
    /* unresolved sampler (function parameter): policy argument */
    if (run_one(unres, A, 1, 0, 0, &o, &on, &r)) return 1;
    CHECK(r.uses_unresolved == 1, "u_unres: counted as unresolved");
    if (run_one(unres, A, 1, 1, 1, &o, &on, &r)) return 1;
    CHECK(r.uses_unresolved == 1 && count_op(o, on, 87) == 0, "u_unres with convert_unresolved");
    free(o);
    /* analysis-only mode and bad input */
    CHECK(lagfx_spv_unnorm_rewrite(off->spv, off->n, A, 1, 1, NULL, NULL, &r) == 1 && r.offset_folded == 1, "analysis only (out == NULL)");
    CHECK(lagfx_spv_unnorm_rewrite(off->spv, off->n - 4, A, 1, 1, &o, &on, &r) == -1 || lagfx_spv_unnorm_rewrite(off->spv, 6, A, 1, 1, &o, &on, &r) == -1, "truncated module refused");
    CHECK(lagfx_spv_unnorm_rewrite(off->spv, off->n, NULL, 0, 1, &o, &on, &r) == 0, "no bindings -> nothing to do");
    printf("PASS: part 1: per-use rewrite (%u shaders, counters, instruction streams%s)\n", (unsigned)(sizeof SH_ / sizeof SH_[0]), g_val_missing ? "; spirv-val NOT available (N)" : ", spirv-val on every rewritten module");
    return 0;
}

/* ------------------------------------------------------------------ CPU model of Vulkan unnormalized sampling (independent of the code under test) */
#define TW 4
#define TH 4
static float g_tex[TH][TW][4];
static void texel(int x, int y, int border, float o[4]) {
    if (border && (x < 0 || y < 0 || x >= TW || y >= TH)) { o[0] = o[1] = o[2] = o[3] = 0; return; }
    x = x < 0 ? 0 : x >= TW ? TW - 1 : x; y = y < 0 ? 0 : y >= TH ? TH - 1 : y;
    memcpy(o, g_tex[y][x], 16);
}
/* u, v in texels (unnormalized); offset in texels */
static void model(int linear, int border, float u, float v, int ox, int oy, float o[4]) {
    if (!linear) { texel((int)floorf(u) + ox, (int)floorf(v) + oy, border, o); return; }
    float uu = u - 0.5f, vv = v - 0.5f; int x0 = (int)floorf(uu), y0 = (int)floorf(vv); float fx = uu - (float)x0, fy = vv - (float)y0;
    for (int c = 0; c < 4; c++) o[c] = 0;
    for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) {
        float t[4]; texel(x0 + dx + ox, y0 + dy + oy, border, t); float w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
        for (int c = 0; c < 4; c++) o[c] += w * t[c];
    }
}

static VkSampler mk_runtime_sampler(uint32_t state_word) {   /* the production decoder, 28-byte payload as the exec reads it */
    uint8_t d[28] = { 0 }; d[0] = 3; d[4] = 28; lagfx_put_le32(d + 12, state_word); lagfx_put_le32(d + 24, 0x7f7fffffu);
    VkSamplerCreateInfo ci; uint32_t fl = lagfx_a64_sampler_decode(d, 28, false, 0, &ci);
    if (!(fl & LAGFX_A64_SAMP_UNNORM_APPLIED)) { printf("FAIL: word 0x%08x did not decode as unnormalized\n", state_word); return VK_NULL_HANDLE; }
    VkSampler s = VK_NULL_HANDLE; vkCreateSampler(tcv_dev, &ci, NULL, &s); return s;
}

typedef struct { float a[4], b[4]; } push_t;

static int cmp_img(const char *what, const float *got, float (*want)(int, int, int), float tol) {
    int bad = 0;
    for (int y = 0; y < TCV_H; y++) for (int x = 0; x < TCV_W; x++) for (int c = 0; c < 4; c++) {
        float w = want(x, y, c), g = got[(y * TCV_W + x) * 4 + c];
        if (fabsf(g - w) > tol && bad++ < 3) printf("  %s: pixel (%d,%d) ch %d got %g want %g\n", what, x, y, c, (double)g, (double)w);
    }
    return bad;
}

typedef struct { int linear, border, ox, oy, mode; } cfg_t;   /* mode 0: out = model; 1: u_mixed */
static cfg_t g_cfg;
static float want_model(int x, int y, int c) {
    float o[4]; model(g_cfg.linear, g_cfg.border, (float)x + 0.5f - 1.75f, (float)y + 0.5f - 1.75f, g_cfg.ox, g_cfg.oy, o); return o[c];
}
static float want_mixed(int x, int y, int c) {
    float o[4]; model(g_cfg.linear, g_cfg.border, (float)x + 0.5f - 1.75f, (float)y + 0.5f - 1.75f, 0, 0, o);
    return c == 0 ? o[0] : c == 1 ? 0.75f : c == 2 ? o[1] : 0.0f;   /* R = A.r, G = B.r (mip level 1 = 0.75), B = A.g, A = B.g (0) */
}

static int part2(void) {
    for (int y = 0; y < TH; y++) for (int x = 0; x < TW; x++) { float *t = g_tex[y][x]; t[0] = 0.1f + 0.2f * (float)x; t[1] = 0.1f + 0.2f * (float)y; t[2] = ((x ^ y) & 1) ? 0.9f : 0.2f; t[3] = 1.0f; }
    tcv_tex_t t0, t1;
    CHECK(!tcv_tex_create(&t0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, TW, TH, 1, 1, 16, g_tex, NULL), "tex0");
    static float mip[(8 * 8 + 4 * 4) * 4];   /* tex1: level 0 8x8 r = 0.25, level 1 4x4 r = 0.75 */
    for (int i = 0; i < 8 * 8; i++) mip[i * 4] = 0.25f, mip[i * 4 + 3] = 1.0f;
    for (int i = 0; i < 4 * 4; i++) mip[(64 + i) * 4] = 0.75f, mip[(64 + i) * 4 + 3] = 1.0f;
    CHECK(!tcv_tex_create(&t1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8, 8, 1, 2, 16, mip, NULL), "tex1");
    VkSamplerCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT, .maxLod = VK_LOD_CLAMP_NONE };
    VkSampler smpB; TCV_VK(vkCreateSampler(tcv_dev, &bci, NULL, &smpB));
    VkSampler near_ = mk_runtime_sampler(0x04000000u), lin = mk_runtime_sampler(0x04000005u);   /* CAPTURED runtime words: unnormalized nearest / linear, clamp to edge */
    CHECK(near_ && lin, "decode");
    VkSamplerCreateInfo sbd = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, .unnormalizedCoordinates = VK_TRUE };   /* SYNTHETIC: pixel coordinates + clamp to border + linear */
    VkSampler lin_bd; TCV_VK(vkCreateSampler(tcv_dev, &sbd, NULL, &lin_bd));

    static const shader_t SH_[] = { SH(u_impl), SH(u_bias), SH(u_off), SH(u_biasoff), SH(u_expl), SH(u_expl_off), SH(u_gradoff) };
    static const int OFX[] = { 0, 0, 1, 1, 0, 1, 1 }, OFY[] = { 0, 0, -1, -1, 0, -1, -1 };
    const uint32_t bnum[4] = { 0, 1, 2, 3 }; const int isS[4] = { 0, 1, 1, 0 };
    const uint32_t A1[] = { 1 };
    struct { VkSampler s; int linear, border; const char *nm; } SS[3] = { { near_, 0, 0, "captured 0x04000000 nearest/edge" }, { lin, 1, 0, "captured 0x04000005 linear/edge" }, { lin_bd, 1, 1, "synthetic linear/border" } };
    push_t pc = { { 1, 1, -1.75f, -1.75f }, { 0, 0, 0, 0 } };
    float got[TCV_W * TCV_H * 4];
    unsigned neg_val_impl = 0, neg_val_off = 0;
    for (unsigned k = 0; k < sizeof SH_ / sizeof SH_[0]; k++) {
        const shader_t *s = &SH_[k];
        uint8_t *o = NULL; size_t on = 0; lagfx_spv_unnorm_report_t r;
        CHECK(lagfx_spv_unnorm_rewrite(s->spv, s->n, A1, 1, 1, &o, &on, &r) >= 0, "%s rewrite", s->name);
        const uint8_t *use = o ? o : s->spv; size_t usen = o ? on : s->n;   /* u_expl needs no rewrite */
        tcv_pipe_t P; CHECK(!tcv_pipe_create(&P, (const uint32_t *)(const void *)use, usen, bnum, isS, 4, sizeof pc), "pipeline %s", s->name);
        for (int si = 0; si < 3; si++) {
            VkImageView views[4] = { t0.view, VK_NULL_HANDLE, VK_NULL_HANDLE, t1.view }; VkSampler smps[4] = { 0, SS[si].s, smpB, 0 };
            tcv_msgs_reset();
            CHECK(!tcv_draw(&P, views, smps, &pc, got), "draw");
            g_cfg = (cfg_t){ SS[si].linear, SS[si].border, OFX[k], OFY[k], 0 };
            int bad = cmp_img(s->name, got, want_model, SS[si].linear ? 0.012f : 1e-6f);
            CHECK(!bad, "%s with %s: %d values differ from the Vulkan unnormalized model", s->name, SS[si].nm, bad);
            if (tcv_validation) { if (tcv_nmsg) tcv_msgs_print(); CHECK(tcv_nmsg == 0, "%s with %s: %u validation message(s) on the REWRITTEN shader", s->name, SS[si].nm, tcv_nmsg); }
        }
        /* a dropped offset must be visible in the model (the control that the test would notice a missing fold) */
        if (OFX[k] || OFY[k]) {
            float a[4], b[4]; int differs = 0;
            for (int y = 0; y < TCV_H && !differs; y++) for (int x = 0; x < TCV_W; x++) {
                model(0, 0, (float)x - 1.25f, (float)y - 1.25f, OFX[k], OFY[k], a); model(0, 0, (float)x - 1.25f, (float)y - 1.25f, 0, 0, b);
                if (fabsf(a[0] - b[0]) + fabsf(a[1] - b[1]) > 0.05f) { differs = 1; break; }
            }
            CHECK(differs, "%s: the offset has no effect in the model: the test could not notice a dropped offset", s->name);
        }
        tcv_pipe_destroy(&P);
        /* NEGATIVE CONTROL: the original (unrewritten) shader with the same unnormalized sampler */
        if (tcv_validation && (!strcmp(s->name, "u_impl") || !strcmp(s->name, "u_off") || !strcmp(s->name, "u_bias"))) {
            tcv_pipe_t Q; CHECK(!tcv_pipe_create(&Q, (const uint32_t *)(const void *)s->spv, s->n, bnum, isS, 4, sizeof pc), "pipeline orig");
            VkImageView views[4] = { t0.view, VK_NULL_HANDLE, VK_NULL_HANDLE, t1.view }; VkSampler smps[4] = { 0, near_, smpB, 0 };
            tcv_msgs_reset();
            (void)tcv_draw(&Q, views, smps, &pc, got);
            unsigned v10 = tcv_msgs_with("08610"), v11 = tcv_msgs_with("08611");
            if (!strcmp(s->name, "u_impl")) neg_val_impl = v10;
            if (!strcmp(s->name, "u_off")) neg_val_off = v11 + v10;
            if (!strcmp(s->name, "u_bias")) neg_val_off += v11;
            tcv_pipe_destroy(&Q);
        }
        free(o);
    }
    if (tcv_validation) {
        CHECK(neg_val_impl >= 1, "negative control: the unrewritten implicit-LOD shader with an unnormalized sampler produced no VUID-vkCmdDraw-None-08610");
        CHECK(neg_val_off >= 1, "negative control: the unrewritten bias / offset shaders produced no 08610 / 08611 VUID");
        printf("PASS: negative control: the UNREWRITTEN shaders are flagged by the validation layer (08610 x%u, bias/offset 08610|08611 x%u); the rewritten ones raise no message\n", neg_val_impl, neg_val_off);
    } else printf("N: Khronos validation layer not available: negative validation controls and the zero-message checks were NOT run\n");
    printf("PASS: part 2a: implicit / bias / const offset / bias+offset / explicit LOD / grad+offset forms through captured and synthetic unnormalized samplers match the Vulkan model on %s\n", tcv_devname);

    /* ---- per-use: a NORMALIZED sampler's implicit-LOD sample must keep its mip selection ---- */
    {
        const shader_t mixed = SH(u_mixed);
        uint8_t *o = NULL; size_t on = 0; lagfx_spv_unnorm_report_t r;
        CHECK(lagfx_spv_unnorm_rewrite(mixed.spv, mixed.n, A1, 1, 1, &o, &on, &r) == 1, "mixed rewrite");
        tcv_pipe_t P; CHECK(!tcv_pipe_create(&P, (const uint32_t *)(const void *)o, on, bnum, isS, 4, sizeof pc), "pipeline mixed");
        VkImageView views[4] = { t0.view, VK_NULL_HANDLE, VK_NULL_HANDLE, t1.view }; VkSampler smps[4] = { 0, lin, smpB, 0 };
        tcv_msgs_reset();
        CHECK(!tcv_draw(&P, views, smps, &pc, got), "draw mixed");
        g_cfg = (cfg_t){ 1, 0, 0, 0, 1 };
        CHECK(!cmp_img("u_mixed per-use", got, want_mixed, 0.012f), "per-use rewrite: the normalized sampler B must still select mip level 1 (0.75)");
        if (tcv_validation) CHECK(tcv_nmsg == 0, "u_mixed per-use: validation messages");
        tcv_pipe_destroy(&P); free(o);
        /* legacy whole-module rewrite (NEGATIVE CONTROL: the old behaviour) */
        uint8_t *lo = NULL; size_t ln = 0;
        CHECK(lagfx_spv_explicit_lod0(mixed.spv, mixed.n, &lo, &ln) == 1, "legacy rewrite");
        CHECK(!tcv_pipe_create(&P, (const uint32_t *)(const void *)lo, ln, bnum, isS, 4, sizeof pc), "pipeline legacy");
        CHECK(!tcv_draw(&P, views, smps, &pc, got), "draw legacy");
        int bad = cmp_img("legacy", got, want_mixed, 0.012f);
        CHECK(bad > 0, "negative control failed: the legacy whole-module rewrite should have changed B's mip selection (G != 0.75) but matched");
        printf("PASS: part 2b: per-use rewrite keeps the normalized sampler's implicit LOD (mip level 1); the legacy whole-module rewrite does not (negative control: %d channel values differ)\n", bad);
        tcv_pipe_destroy(&P); free(lo);
    }
    vkDestroySampler(tcv_dev, smpB, NULL); vkDestroySampler(tcv_dev, near_, NULL); vkDestroySampler(tcv_dev, lin, NULL); vkDestroySampler(tcv_dev, lin_bd, NULL);
    tcv_tex_destroy(&t0); tcv_tex_destroy(&t1);
    return 0;
}

int main(void) {
    int rc = part1();
    if (rc) return rc;
    int ir = tcv_init(1, NULL, 0);
    if (ir == 77) { printf("part 1 passed; part 2 skipped (no device)\n"); return 77; }
    rc = part2();
    if (rc) { tcv_msgs_print(); return rc; }
    printf("PASS air2spv-unnorm-use-lavapipe on %s (validation layer %s)\n", tcv_devname, tcv_validation ? "ON" : "absent");
    return 0;
}
