/*
 * libapplegfx-vulkan — constexpr sampler extraction (unit)
 * tests/air2spv-constexpr-sampler-unit.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Part A: decode of the four sampler words observed in the captured Apple
 *         modules (literal values) and of layouts that must be rejected.
 * Part B: when LAGFX_SAMPLER_DUMP_DIR names a directory of extracted
 *         `*.air.bc` modules (the captured Apple shaders are not shipped
 *         in this repo), extract the samplers of every module and follow
 *         every air.sample_texture_* call's sampler operand to its global.
 *         Without the variable Part B is skipped (the test still passes).
 */

#include "air2spv/constexpr_sampler.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static const uint64_t k_words[4] = {
    0x807bff0000088a00ull, 0x807bff0000088000ull,
    0x807bff0000080092ull, 0x807bff0000080a49ull,
};

static void part_a(void) {
    CHECK(lagfx_half_to_float(0x7bffu) == 65504.0f);
    CHECK(lagfx_half_to_float(0x0000u) == 0.0f);
    CHECK(lagfx_half_to_float(0x3c00u) == 1.0f);
    CHECK(lagfx_half_to_float(0x0001u) > 0.0f && lagfx_half_to_float(0x0001u) < 1e-7f);
    for (int i = 0; i < 4; i++) {
        lagfx_constexpr_sampler_t s;
        CHECK(lagfx_constexpr_sampler_decode(k_words[i], &s));
        CHECK(s.valid);
        CHECK(s.compare_func == 8 && s.max_aniso == 1 && s.mip_filter == 0);
        CHECK(s.lod_min == 0.0f && s.lod_max == 65504.0f);
        CHECK(s.raw == k_words[i]);
        CHECK(s.raw_low24 == (uint32_t)(k_words[i] & 0xFFFFFFu));
    }
    lagfx_constexpr_sampler_t s;
    /* field decode of the four captured words (external layout, see header) */
    CHECK(lagfx_constexpr_sampler_decode(k_words[0], &s));
    CHECK(!s.normalized && s.mag_filter == 1 && s.min_filter == 1 && s.addr[0] == 0 && s.addr[1] == 0 && s.addr[2] == 0);
    CHECK(lagfx_constexpr_sampler_decode(k_words[1], &s));
    CHECK(!s.normalized && s.mag_filter == 0 && s.min_filter == 0 && s.addr[0] == 0);
    CHECK(lagfx_constexpr_sampler_decode(k_words[2], &s));
    CHECK(s.normalized && s.mag_filter == 0 && s.min_filter == 0 && s.addr[0] == 2 && s.addr[1] == 2 && s.addr[2] == 2);
    CHECK(lagfx_constexpr_sampler_decode(k_words[3], &s));
    CHECK(s.normalized && s.mag_filter == 1 && s.min_filter == 1 && s.addr[0] == 1 && s.addr[1] == 1 && s.addr[2] == 1);
    CHECK(!lagfx_constexpr_sampler_decode(0x007bff0000088a00ull, &s));   /* bit 63 clear */
    CHECK(!lagfx_constexpr_sampler_decode(0x817bff0000088a00ull, &s));   /* unknown bit 56 */
    CHECK(!lagfx_constexpr_sampler_decode(0x807c000000000000ull, &s));   /* max = +inf */
    CHECK(!lagfx_constexpr_sampler_decode(0x8001003c00000000ull, &s));   /* min(0x3c00>...) > max */
    CHECK(!lagfx_constexpr_sampler_decode(0, NULL));
}

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)l + 1);
    if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
    fclose(f); *n = (size_t)l; return b;
}

static int word_index(uint64_t w) {
    for (int i = 0; i < 4; i++) if (k_words[i] == w) return i;
    return -1;
}

static void part_b(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "cannot open %s\n", dir); g_fail++; return; }
    struct dirent *e;
    unsigned nmod = 0, nwith = 0, ncalls = 0, nres = 0, nunres = 0, nargsamp = 0;
    unsigned seen[4] = {0};
    while ((e = readdir(d))) {
        size_t nl = strlen(e->d_name);
        if (nl < 7 || strcmp(e->d_name + nl - 7, ".air.bc") != 0) continue;
        char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        size_t n; uint8_t *b = slurp(path, &n); if (!b) continue;
        lagfx_air_module_t *m = NULL;
        if (lagfx_air_module_open(b, n, &m) != LAGFX_OK) { free(b); continue; }
        nmod++;
        lagfx_constexpr_sampler_t sm[LAGFX_CONSTEXPR_SAMPLER_MAX];
        uint32_t ns = lagfx_air_extract_constexpr_samplers(m, sm, LAGFX_CONSTEXPR_SAMPLER_MAX);
        if (ns) nwith++;
        for (uint32_t i = 0; i < ns; i++) {
            int wi = word_index(sm[i].raw);
            CHECK(wi >= 0);
            if (wi >= 0) seen[wi]++;
            CHECK(sm[i].valid);
        }
        uint32_t nf, ng = lagfx_air_module_num_globalvars(m), nmc;
        const lagfx_air_function_t *fn = lagfx_air_module_functions(m, &nf);
        const lagfx_air_type_t *ts = lagfx_air_module_types(m, NULL);
        (void)lagfx_air_module_constants(m, &nmc);
        for (uint32_t fi = 0; fi < nf; fi++) {
            if (fn[fi].is_proto) continue;
            lagfx_air_function_body_t *bd = NULL;
            if (lagfx_air_function_body_open(m, fi, &bd) != LAGFX_OK) continue;
            uint32_t ni, nlc;
            const lagfx_air_inst_t *in = lagfx_air_function_body_instructions(bd, &ni);
            (void)lagfx_air_function_body_local_constants(bd, &nlc);
            uint32_t fty = fn[fi].type_index;
            uint32_t nargs = ts[fty].num_op >= 2u ? ts[fty].num_op - 2u : 0u;
            uint32_t first_local = ng + nf + nmc + nargs;
            uint32_t inst_num = first_local + nlc;
            for (uint32_t k = 0; k < ni; k++) {
                const lagfx_air_inst_t *x = &in[k];
                bool prod = true;
                switch (x->code) {
                    case LAGFX_AIR_INST_DECLAREBLOCKS: case LAGFX_AIR_INST_STORE:
                    case LAGFX_AIR_INST_STORE_OLD: case LAGFX_AIR_INST_RET:
                    case LAGFX_AIR_INST_BR: case LAGFX_AIR_INST_SWITCH:
                    case LAGFX_AIR_INST_UNREACHABLE: prod = false; break;
                    default: break;
                }
                if (x->code == LAGFX_AIR_INST_CALL && x->num_ops >= 4u) {
                    bool expl = (x->ops[1] >> 15) & 1u;
                    uint32_t calleei = 2u + (expl ? 1u : 0u);
                    uint32_t callee_abs = inst_num - (uint32_t)x->ops[calleei];
                    uint32_t cfn = callee_abs - ng;
                    const char *nm = (callee_abs >= ng && cfn < nf)
                                         ? lagfx_air_module_string(m, fn[cfn].name_offset) : NULL;
                    uint32_t fnty = expl ? (uint32_t)x->ops[2] : (callee_abs >= ng && cfn < nf ? fn[cfn].type_index : 0u);
                    prod = ts[fnty].kind == LAGFX_AIR_TYPE_FUNCTION && ts[fnty].num_op >= 2u &&
                           ts[ts[fnty].op[1]].kind != LAGFX_AIR_TYPE_VOID;
                    if (nm && strncmp(nm, "air.sample_texture_", 19) == 0 && x->num_ops > calleei + 2u) {
                        ncalls++;
                        uint32_t samp_abs = inst_num - (uint32_t)x->ops[calleei + 2u];
                        int32_t g = lagfx_air_sampler_operand_global(m, bd, first_local, samp_abs);
                        if (g >= 0) {
                            nres++;
                            int found = 0;
                            for (uint32_t i = 0; i < ns; i++) found |= sm[i].global_index == (uint32_t)g;
                            CHECK(found);
                        } else if (samp_abs < first_local && samp_abs >= ng + nf + nmc) {
                            nargsamp++;
                        } else {
                            nunres++;
                        }
                    }
                }
                if (prod) inst_num++;
            }
            lagfx_air_function_body_free(bd);
        }
        lagfx_air_module_free(m);
        free(b);
    }
    closedir(d);
    printf("dump: %u modules, %u with constexpr samplers, %u sample calls: %u -> constexpr global, "
           "%u -> sampler argument, %u unresolved\n", nmod, nwith, ncalls, nres, nargsamp, nunres);
    printf("dump: sampler words seen: ");
    for (int i = 0; i < 4; i++) printf("0x%016llx x%u  ", (unsigned long long)k_words[i], seen[i]);
    printf("\n");
    CHECK(nmod > 0);
    for (int i = 0; i < 4; i++) CHECK(seen[i] > 0);
    CHECK(nres > 0);
    CHECK(nunres == 0);
}

int main(void) {
    part_a();
    const char *dir = getenv("LAGFX_SAMPLER_DUMP_DIR");
    if (dir && *dir) part_b(dir);
    else fprintf(stderr, "LAGFX_SAMPLER_DUMP_DIR unset: dump part skipped\n");
    if (g_fail) { fprintf(stderr, "%d failure(s)\n", g_fail); return 1; }
    printf("constexpr sampler unit: ok\n");
    return 0;
}
