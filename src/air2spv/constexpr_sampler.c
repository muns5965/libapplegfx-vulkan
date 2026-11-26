/*
 * libapplegfx-vulkan — constexpr sampler state extraction from AIR
 * src/air2spv/constexpr_sampler.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 */

#include "air2spv/constexpr_sampler.h"

#include <string.h>

float lagfx_half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) & 1u;
    uint32_t exp  = (uint32_t)(h >> 10) & 0x1Fu;
    uint32_t man  = (uint32_t)h & 0x3FFu;
    float v;
    if (exp == 0u) {
        v = (float)man * (1.0f / 16777216.0f);          /* subnormal: man * 2^-24 */
    } else if (exp == 31u) {
        union { uint32_t u; float f; } c;
        c.u = 0x7F800000u | (man << 13);                 /* inf / nan */
        return sign ? -c.f : c.f;
    } else {
        v = (1.0f + (float)man * (1.0f / 1024.0f));
        int e = (int)exp - 15;
        while (e > 0) { v *= 2.0f; e--; }
        while (e < 0) { v *= 0.5f; e++; }
    }
    return sign ? -v : v;
}

bool lagfx_constexpr_sampler_decode(uint64_t raw, lagfx_constexpr_sampler_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->raw       = raw;
    out->raw_low24 = (uint32_t)(raw & 0xFFFFFFu);
    uint16_t hmin = (uint16_t)((raw >> 24) & 0xFFFFu);
    uint16_t hmax = (uint16_t)((raw >> 40) & 0xFFFFu);
    /* Layout check: unknown high bits must be clear, both clamps must be
     * finite non-negative halves in order, and the bit-63 one-word marker set — a
     * word that does not look like the observed ones is not interpreted. */
    if ((raw >> 56) & 0x7Fu) return false;
    if (!((raw >> 63) & 1u)) return false;      /* one-word-form marker */
    if ((hmin & 0x8000u) || (hmax & 0x8000u)) return false;
    if (((hmin >> 10) & 0x1Fu) == 31u || ((hmax >> 10) & 0x1Fu) == 31u) return false;
    float lo = lagfx_half_to_float(hmin), hi = lagfx_half_to_float(hmax);
    if (lo > hi) return false;
    out->valid      = true;
    out->normalized = !((raw >> 15) & 1u);
    for (int i = 0; i < 3; i++) out->addr[i] = (uint8_t)((raw >> (3 * i)) & 7u);
    out->mag_filter    = (uint8_t)((raw >> 9) & 3u);
    out->min_filter    = (uint8_t)((raw >> 11) & 3u);
    out->mip_filter    = (uint8_t)((raw >> 13) & 3u);
    out->compare_func  = (uint8_t)((raw >> 16) & 0xFu);
    out->max_aniso     = (uint8_t)(((raw >> 20) & 0xFu) + 1u);
    out->lod_min    = lo;
    out->lod_max    = hi;
    return true;
}

static const lagfx_air_metadata_t *md_by_id(const lagfx_air_module_t *m, uint32_t id) {
    uint32_t ns = 0, nm = 0;
    (void)lagfx_air_module_metadata_strings(m, &ns);
    const lagfx_air_metadata_t *md = lagfx_air_module_metadata(m, &nm);
    if (!md || id < ns || id - ns >= nm) return NULL;
    return &md[id - ns];
}

static bool module_const_i64(const lagfx_air_module_t *m, uint32_t value_id, uint64_t *out) {
    uint32_t nc = 0;
    const lagfx_air_constant_t *c = lagfx_air_module_constants(m, &nc);
    uint32_t base = lagfx_air_module_num_globalvars(m);
    uint32_t nf = 0;
    (void)lagfx_air_module_functions(m, &nf);
    base += nf;
    if (!c || value_id < base || value_id - base >= nc) return false;
    const lagfx_air_constant_t *k = &c[value_id - base];
    if (k->kind != LAGFX_AIR_CONST_INTEGER) return false;
    *out = (uint64_t)k->payload.i64;
    return true;
}

uint32_t lagfx_air_extract_constexpr_samplers(const lagfx_air_module_t *m,
                                              lagfx_constexpr_sampler_t *out,
                                              uint32_t max) {
    if (!m || !out || max == 0u) return 0u;
    const lagfx_air_metadata_t *nn = lagfx_air_module_named_metadata(m, "air.sampler_states");
    if (!nn) return 0u;
    uint32_t n = 0;
    uint32_t ng = lagfx_air_module_num_globalvars(m);
    for (uint32_t i = 0; i < nn->num_operands && n < max; i++) {
        const lagfx_air_metadata_t *node = md_by_id(m, nn->operands[i]);
        if (!node || node->kind != LAGFX_AIR_MD_NODE || node->num_operands < 2u) continue;
        const char *tag = node->operands[0]
                              ? lagfx_air_module_metadata_string_by_id(m, node->operands[0] - 1u) : NULL;
        if (!tag || strcmp(tag, "air.sampler_state") != 0) continue;
        if (!node->operands[1]) continue;
        const lagfx_air_metadata_t *val = md_by_id(m, node->operands[1] - 1u);
        if (!val || val->kind != LAGFX_AIR_MD_VALUE || val->num_operands < 2u) continue;
        uint32_t gid = val->operands[1];
        lagfx_air_globalvar_t gv;
        if (gid >= ng || !lagfx_air_module_globalvar(m, gid, &gv)) continue;
        if ((gv.flags & 1u) == 0u || (gv.flags >> 2) != 2u || gv.initid == 0u) continue;
        uint64_t raw;
        if (!module_const_i64(m, gv.initid - 1u, &raw)) continue;
        (void)lagfx_constexpr_sampler_decode(raw, &out[n]);
        out[n].global_index = gid;
        n++;
    }
    return n;
}

int32_t lagfx_air_sampler_operand_global(const lagfx_air_module_t *m,
                                         const lagfx_air_function_body_t *body,
                                         uint32_t first_local_value_id,
                                         uint32_t abs_value_id) {
    if (!m) return -1;
    uint32_t ng = lagfx_air_module_num_globalvars(m), nf = 0, nmc = 0;
    (void)lagfx_air_module_functions(m, &nf);
    const lagfx_air_constant_t *mc = lagfx_air_module_constants(m, &nmc);
    uint32_t mbase = ng + nf;
    uint32_t nlc = 0;
    const lagfx_air_constant_t *lc = body ? lagfx_air_function_body_local_constants(body, &nlc) : NULL;
    uint32_t v = abs_value_id;
    for (int depth = 0; depth < 4; depth++) {
        if (v < ng) return (int32_t)v;
        const lagfx_air_constant_t *k = NULL;
        const void *ops = NULL;
        if (lc && v >= first_local_value_id && v - first_local_value_id < nlc) {
            k = &lc[v - first_local_value_id];
            ops = lagfx_air_function_body_payload_ptr(body, k->payload.bytes.offset);
        } else if (mc && v >= mbase && v - mbase < nmc) {
            k = &mc[v - mbase];
            ops = k->payload.bytes.offset ? lagfx_air_module_string(m, k->payload.bytes.offset) : NULL;
        }
        if (!k || k->kind != LAGFX_AIR_CONST_UNKNOWN || k->raw_code != 11u /* CE_CAST */ ||
            !ops || k->payload.bytes.len < 3u * sizeof(uint32_t))
            return -1;
        v = ((const uint32_t *)ops)[2];   /* [opcode, src type, src value id (absolute)] */
    }
    return -1;
}
