/*
 * libapplegfx-vulkan -- spec-conformance regression tests found by the spec audit (docs/spec-audit.md)
 * tests/air2spv-spec-audit-unit.c
 * SPDX-License-Identifier: MIT
 *
 * Fixtures tests/fixtures/spec_*.ll are HAND-WRITTEN IR (llvm-as), not Apple output.
 *  1. LLVM `srem` (C / MSL integer %, sign of the DIVIDEND) must become OpSRem; OpSMod takes the sign of the divisor (SPIR-V 3.56.x, OpSRem / OpSMod).
 *  2. A struct { <3 x float>, float } under the AIR data layout (v96:128:128) has the second member at byte 16 (LLVM alloc size of <3 x float> is 16; checked with
 *     opt instcombine on the same data layout): the Block's Offset decorations must say 16, not 12.
 *  3. [[instance_id]] is Vulkan InstanceIndex (BuiltIn 43), not a second VertexIndex (BuiltIn 42).
 *  6. air.round: halves away from zero (Trunc + select), not GLSL Round.
 *  5. air.discard_fragment: OpKill (split block) by default, OpDemoteToHelperInvocation + capability + extension when enabled; both counted.
 *  4. int -> int air.convert extends by the SOURCE signedness (OpSConvert for a signed source, OpUConvert for an unsigned one); same width is OpBitcast.
 */
#include "air2spv/translate.h"
#include "air2spv/translate_function.h"
#include "air/bitcode_reader.h"
#include "vulkan/caps.h"
#include "vulkan/spv_variants.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static int g_fail;
#define EXPECT(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static uint8_t *slurp(const char *name, size_t *n) {
    static const char *dirs[] = { SRCDIR "/fixtures/", "tests/fixtures/", "../tests/fixtures/" };
    char path[512];
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
        snprintf(path, sizeof path, "%s%s", dirs[d], name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *b = malloc((size_t)sz);
        if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
        fclose(f); *n = (size_t)sz; return b;
    }
    return NULL;
}

static uint32_t *translate(const char *fixture, bool frag_with_keys, size_t *nw) {
    size_t bn = 0; uint8_t *bc = slurp(fixture, &bn);
    if (!bc) { printf("FAIL: fixture %s not found\n", fixture); g_fail++; return NULL; }
    lagfx_air_module_t *m = NULL; uint8_t *spv = NULL; size_t sz = 0;
    if (lagfx_air_module_open(bc, bn, &m) != LAGFX_OK || !m) { printf("FAIL: %s: module_open\n", fixture); g_fail++; free(bc); return NULL; }
    lagfx_stage_keys_t vso; memset(&vso, 0, sizeof vso);   /* an empty paired-vertex key list: gives the fragment its own argument keys */
    lagfx_status_t st = lagfx_air2spv_translate_module_ex(m, frag_with_keys ? &vso : NULL, &spv, &sz);
    if (st != LAGFX_OK || !spv) { printf("FAIL: %s: translate st=%d\n", fixture, (int)st); g_fail++; free(bc); return NULL; }
    free(bc);
    if (getenv("SPEC_DUMP")) { char o[256]; snprintf(o, sizeof o, "%s/%s.spv", getenv("SPEC_DUMP"), fixture); FILE *f = fopen(o, "wb"); if (f) { fwrite(spv, 1, sz, f); fclose(f); } }
    *nw = sz / 4u;
    return (uint32_t *)spv;
}

static int count_op(const uint32_t *w, size_t nw, uint32_t opcode) {
    int n = 0;
    for (size_t i = 5; i < nw;) { uint32_t wc = w[i] >> 16; if (!wc) break; if ((w[i] & 0xffffu) == opcode) n++; i += wc; }
    return n;
}

static bool has_cap(const uint32_t *w, size_t nw, uint32_t cap) {
    for (size_t i = 5; i < nw;) { uint32_t wc = w[i] >> 16; if (!wc) break; if ((w[i] & 0xffffu) == 17u && w[i + 1] == cap) return true; i += wc; }
    return false;
}
static bool has_ext(const uint32_t *w, size_t nw, const char *name) {
    for (size_t i = 5; i < nw;) { uint32_t wc = w[i] >> 16; if (!wc) break; if ((w[i] & 0xffffu) == 10u && !strncmp((const char *)&w[i + 1], name, strlen(name) + 1u)) return true; i += wc; }
    return false;
}


/* order of the integer width conversions of a module: U = OpUConvert, S = OpSConvert, B = OpBitcast (a same-width air.convert) */
static void conv_seq(const uint32_t *w, size_t nw, char *o, size_t cap) {
    size_t n = 0;
    for (size_t i = 5; i < nw && n + 1 < cap;) { uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu; if (!wc) break; if (op == 113u) o[n++] = 'U'; else if (op == 114u) o[n++] = 'S'; else if (op == 124u) o[n++] = 'B'; i += wc; }
    o[n] = 0;
}
/* spirv-val (vulkan1.3) when the tool is installed: 1 = accepted, 0 = not available, -1 = rejected */
static int spirv_val(const uint32_t *w, size_t nw) {
    if (system("command -v spirv-val >/dev/null 2>&1") != 0) return 0;
    char path[] = "/tmp/lagfx_specaudit_XXXXXX";
    int fd = mkstemp(path); if (fd < 0) return 0;
    FILE *f = fdopen(fd, "wb"); if (!f) return 0;
    fwrite(w, 4, nw, f); fclose(f);
    char cmd[300]; snprintf(cmd, sizeof cmd, "spirv-val --target-env vulkan1.3 %s >/dev/null 2>&1", path);
    int rc = system(cmd); unlink(path);
    return rc == 0 ? 1 : -1;
}

int main(void) {
    unsetenv("LAGFX_SPV_NANMAX");   /* the default translation must not depend on the caller's environment */
    size_t nw;
    uint32_t *w = translate("spec_srem_fragment.air.bc", false, &nw);
    if (w) {
        EXPECT(count_op(w, nw, 138u) == 1, "srem: OpSRem count %d, want 1", count_op(w, nw, 138u));
        EXPECT(count_op(w, nw, 139u) == 0, "srem: OpSMod count %d, want 0 (SMod takes the sign of the divisor)", count_op(w, nw, 139u));
        free(w);
    }
    {   /* the module's type table holds %S = { <3 x float>, float }: its layout under the AIR data layout is size 32, align 16 (member 1 at byte 16) */
        size_t bn = 0; uint8_t *bc = slurp("spec_vec3_layout_fragment.air.bc", &bn);
        lagfx_air_module_t *m = NULL;
        EXPECT(bc && lagfx_air_module_open(bc, bn, &m) == LAGFX_OK && m, "vec3 layout: module_open");
        if (m) {
            uint32_t nt = 0; const lagfx_air_type_t *ts = lagfx_air_module_types(m, &nt);
            int found = 0;
            for (uint32_t t = 0; t < nt; t++) {
                if ((ts[t].kind != LAGFX_AIR_TYPE_STRUCT_NAMED && ts[t].kind != LAGFX_AIR_TYPE_STRUCT_ANON) || ts[t].num_op != 3u) continue;
                uint32_t f0 = ts[t].op[1];
                if (f0 >= nt || ts[f0].kind != LAGFX_AIR_TYPE_VECTOR || ts[f0].op[0] != 3u) continue;
                uint32_t sz = 0, al = 0, vsz = 0, val = 0;
                found = 1;
                EXPECT(lagfx_air2spv_type_layout(m, t, &sz, &al), "vec3 layout: layout query failed");
                EXPECT(lagfx_air2spv_type_layout(m, f0, &vsz, &val) && vsz == 16u && val == 16u, "vec3 layout: <3 x float> size %u align %u, want 16/16 (LLVM alloc size)", vsz, val);
                EXPECT(sz == 32u && al == 16u, "vec3 layout: struct { <3 x float>, float } size %u align %u, want 32/16", sz, al);
            }
            EXPECT(found, "vec3 layout: struct type not found in the module");
        }
        free(bc);
    }
    w = translate("spec_instance_vertex.air.bc", false, &nw);
    if (w) {
        int vid = 0, iid = 0;
        for (size_t i = 5; i < nw;) { uint32_t wc = w[i] >> 16; if (!wc) break; if ((w[i] & 0xffffu) == 71u && wc == 4u && w[i + 2] == 11u) { vid += w[i + 3] == 42u; iid += w[i + 3] == 43u; } i += wc; }
        EXPECT(vid == 1, "instance_id: VertexIndex builtins %d, want 1", vid);
        EXPECT(iid == 1, "instance_id: InstanceIndex builtins %d, want 1", iid);
        free(w);
    }
    w = translate("spec_convert_int_fragment.air.bc", false, &nw);   /* 4. int -> int air.convert: widening follows the SOURCE signedness (S07) */
    if (w) {
        lagfx_conv_totals_t ct; lagfx_air2spv_conv_totals(&ct);
        EXPECT(count_op(w, nw, 114u) == 1, "convert int: OpSConvert count %d, want 1 (signed i16 source sign-extends)", count_op(w, nw, 114u));
        EXPECT(count_op(w, nw, 113u) == 1, "convert int: OpUConvert count %d, want 1 (unsigned i16 source zero-extends)", count_op(w, nw, 113u));
        EXPECT(count_op(w, nw, 124u) == 1, "convert int: OpBitcast count %d, want 1 (same-width i32 -> i32)", count_op(w, nw, 124u));
        EXPECT(ct.int_int >= 3u && ct.int_int_unhandled == 0u, "convert int: lowered %llu unhandled %llu", (unsigned long long)ct.int_int, (unsigned long long)ct.int_int_unhandled);
        free(w);
    }
    w = translate("spec_round_half_vec_fragment.air.bc", false, &nw);   /* 6. air.round on <3 x half>: emulation (OpSelect + ordered >= 0.5), valid module (spirv-val run by hand) */
    if (w) {
        EXPECT(count_op(w, nw, 169u) == 1 && count_op(w, nw, 190u) == 1, "round: OpSelect %d OpFOrdGreaterThanEqual %d, want 1 / 1", count_op(w, nw, 169u), count_op(w, nw, 190u));
        free(w);
    }
    /* 5. air.discard_fragment (S06): OpKill in a split block by default, OpDemoteToHelperInvocation (+ capability + extension) when the device has the feature */
    {
        static const char *const fx[2] = { "spec_discard_linear.air.bc", "spec_discard_branch.air.bc" };
        for (int f = 0; f < 2; f++) {
            lagfx_discard_totals_t t0, t1;
            lagfx_air2spv_set_discard_demote(false); lagfx_air2spv_discard_totals(&t0);
            w = translate(fx[f], false, &nw);
            lagfx_air2spv_discard_totals(&t1);
            if (w) {
                EXPECT(count_op(w, nw, 252u) == 1 && count_op(w, nw, 5380u) == 0, "%s kill mode: OpKill %d OpDemote %d, want 1 / 0", fx[f], count_op(w, nw, 252u), count_op(w, nw, 5380u));
                EXPECT(count_op(w, nw, 17u) >= 1 && !has_cap(w, nw, 5379u), "%s kill mode: DemoteToHelperInvocation must not be declared", fx[f]);
                EXPECT(t1.kill - t0.kill == 1u && t1.demote == t0.demote && t1.dropped == t0.dropped, "%s kill mode: counters", fx[f]);
                free(w);
            }
            lagfx_air2spv_set_discard_demote(true); lagfx_air2spv_discard_totals(&t0);
            w = translate(fx[f], false, &nw);
            lagfx_air2spv_discard_totals(&t1);
            if (w) {
                EXPECT(count_op(w, nw, 5380u) == 1 && count_op(w, nw, 252u) == 0, "%s demote mode: OpDemote %d OpKill %d, want 1 / 0", fx[f], count_op(w, nw, 5380u), count_op(w, nw, 252u));
                EXPECT(has_cap(w, nw, 5379u), "%s demote mode: Capability DemoteToHelperInvocation not declared", fx[f]);
                EXPECT(has_ext(w, nw, "SPV_EXT_demote_to_helper_invocation"), "%s demote mode: SPV_EXT_demote_to_helper_invocation not declared", fx[f]);
                EXPECT(t1.demote - t0.demote == 1u && t1.kill == t0.kill, "%s demote mode: counters", fx[f]);
                uint32_t req = 0; EXPECT(lagfx_spv_required_caps_ex(w, nw * 4u, &req) && (req & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_DEMOTE)), "%s demote mode: required-caps scan misses the demote capability", fx[f]);
                free(w);
            }
        }
        lagfx_air2spv_set_discard_demote(false);
    }
    /* S07 matrix (fixtures gen-convert-fixtures.sh, executed on lavapipe by air2spv-int-intrinsics-lavapipe): the extension follows the SOURCE signedness, same width = OpBitcast.
     * Sequence = every width conversion of the module in order, including the zext / sext / trunc casts of the fixture (they are OpUConvert / OpSConvert too). */
    {
        static const struct { const char *fx; const char *seq; } k[] = {
            /* a: s8<-s32 S, s32<-s8 S, u8<-u32 U, u32<-u8 U, s16<-s32 S, u32<-s16 S (old: U), u16<-u32 U, s32<-u16 U (old: S) */
            { "rt_convert_a_fragment.air.bc", "SSUUSSUU" },
            /* b: u32<-s32 B (old: U, invalid same width), sext S, s16<-s64 S, s32<-s16 S | zext U, s16<-u64 U (old: S), s32<-s16 S */
            { "rt_convert_b_fragment.air.bc", "BSSSUUS" },
            /* c: s16x2<-s32x2 S, u32x2<-s16x2 S (old: U), s32x2<-u16x2 U (old: S) */
            { "rt_convert_c_fragment.air.bc", "SSU" },
        };
        for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
            w = translate(k[i].fx, false, &nw);
            if (!w) continue;
            char sq[64]; conv_seq(w, nw, sq, sizeof sq);
            EXPECT(!strcmp(sq, k[i].seq), "%s: width-conversion sequence \"%s\", want \"%s\"", k[i].fx, sq, k[i].seq);
            EXPECT(spirv_val(w, nw) >= 0, "%s: spirv-val rejects the module (an OpUConvert / OpSConvert of equal widths is invalid)", k[i].fx);
            free(w);
        }
    }
    /* D2: the NaN min / max candidate is OPT-IN. Default: FMin 37 / FMax 40 / FClamp 43 only; forced on: the same instructions as NMin 79 / NMax 80 / NClamp 81; the repro-ladder rewrite
     * (lagfx_spv_variant_nanmax) of the DEFAULT module is byte-identical to the forced-on translation; the default stays byte-identical whatever was done before */
    {
        static const char *const fx[2] = { "rt_nanmax_a_fragment.air.bc", "rt_nanmax_b_fragment.air.bc" };
        for (int f = 0; f < 2; f++) {
            size_t n0, n1, n2, n3;
            lagfx_air2spv_set_nanmax(-1); uint32_t *d0 = translate(fx[f], false, &n0);
            lagfx_air2spv_set_nanmax(1);  uint32_t *on = translate(fx[f], false, &n1);
            lagfx_air2spv_set_nanmax(0);  uint32_t *d1 = translate(fx[f], false, &n2);
            lagfx_air2spv_set_nanmax(-1);
            uint32_t *rw = NULL; size_t nr = 0; char info[200] = "";
            int nch = d0 ? lagfx_spv_variant_nanmax(d0, n0, &rw, &nr, info, sizeof info) : -1;
            n3 = nr;
            if (d0 && on && d1) {
                EXPECT(n0 == n2 && !memcmp(d0, d1, n0 * 4u), "%s: default translation changed after a forced-on one (not byte-identical)", fx[f]);
                int ext_d = 0, ext_n = 0; (void)ext_d; (void)ext_n;
                EXPECT(nch == 4, "%s: lagfx_spv_variant_nanmax changed %d, want 4 (%s)", fx[f], nch, info);
                EXPECT(rw && n1 == n3 && !memcmp(on, rw, n1 * 4u), "%s: translator opt-in (LAGFX_SPV_NANMAX) and the SPIR-V rewrite variant differ", fx[f]);
                EXPECT(n0 == n1 && memcmp(d0, on, n0 * 4u) != 0, "%s: forced-on translation equals the default (nothing switched)", fx[f]);
                EXPECT(spirv_val(on, n1) >= 0, "%s: NMin/NMax/NClamp module rejected by spirv-val", fx[f]);
                /* instruction numbers of the OpExtInst words that differ */
                int d37 = 0, d40 = 0, d43 = 0, n79 = 0, n80 = 0, n81 = 0;
                for (size_t i = 5; i < n0;) { uint32_t wc = d0[i] >> 16; if (!wc) break; if ((d0[i] & 0xffffu) == 12u) { uint32_t in0 = d0[i + 4], in1 = on[i + 4]; d37 += in0 == 37u; d40 += in0 == 40u; d43 += in0 == 43u; n79 += in1 == 79u; n80 += in1 == 80u; n81 += in1 == 81u; } i += wc; }
                if (f == 0) EXPECT(d40 == 2 && d37 == 2 && n80 == 2 && n79 == 2 && d43 == 0, "%s: FMax %d FMin %d -> NMax %d NMin %d, want 2 / 2 -> 2 / 2", fx[f], d40, d37, n80, n79);
                else EXPECT(d43 == 4 && n81 == 4 && d40 == 0 && d37 == 0, "%s: FClamp %d -> NClamp %d, want 4 (3 clamp + 1 saturate) -> 4", fx[f], d43, n81);
            }
            free(d0); free(on); free(d1); free(rw);
        }
    }
    if (g_fail) { printf("air2spv-spec-audit-unit: %d failure(s)\n", g_fail); return 1; }
    printf("air2spv-spec-audit-unit: OK\n");
    return 0;
}
