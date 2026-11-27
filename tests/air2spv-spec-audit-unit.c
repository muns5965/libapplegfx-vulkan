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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

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

int main(void) {
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
    if (g_fail) { printf("air2spv-spec-audit-unit: %d failure(s)\n", g_fail); return 1; }
    printf("air2spv-spec-audit-unit: OK\n");
    return 0;
}
