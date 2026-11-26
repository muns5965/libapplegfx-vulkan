/* libapplegfx-vulkan — SPIR-V capability scan for the device-capability gate (see caps.h). */
#include <stdlib.h>
#include <stdbool.h>
#include "instance.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static const char *const k_names[LAGFX_VK_CAP_COUNT] = { "int64", "int16", "int8", "float16", "sb8", "ub8", "pc8", "sb16", "ub16", "pc16", "io16", "varptr_sb", "varptr", "spirv14", "subgroup", "sg_shuffle", "sg_shuffle_rel" };

const char *lagfx_vk_cap_name(unsigned cap) { return cap < LAGFX_VK_CAP_COUNT ? k_names[cap] : "?"; }

unsigned lagfx_vk_caps_parse(const char *csv) {
    unsigned m = 0;
    while (csv && *csv) {
        const char *e = csv;
        while (*e && *e != ',') e++;
        size_t n = (size_t)(e - csv);
        for (unsigned i = 0; i < LAGFX_VK_CAP_COUNT; i++) if (strlen(k_names[i]) == n && !strncmp(csv, k_names[i], n)) m |= LAGFX_VK_CAP_BIT(i);
        csv = *e ? e + 1 : e;
    }
    return m;
}

/* Capabilities a module NEEDS judging by what it actually uses, whatever it
 * declares (a translated module may use a 16/8-bit type or 16/8-bit
 * StorageBuffer member without declaring the matching capability, which a
 * driver may refuse with a bare VK_ERROR_UNKNOWN at pipeline creation):
 *   OpTypeFloat 16 → float16, OpTypeInt 8/16/64 → int8/int16/int64,
 *   a 16/8-bit scalar reachable inside a StorageBuffer/Uniform/PushConstant
 *   pointee → storage16 / storage8,
 *   a 16-bit type behind an Input/Output pointer → storage16 (the
 *   StorageInputOutput16 requirement; the device flag is storage16). */
#define SPV_MAXID 65536u
typedef struct { uint8_t kind, width; uint32_t a, b; } spv_ty_t;   /* kind: 1 int 2 float 3 struct 4 array 5 pointer 6 vec; a = elem/pointee, b = storage class */

static bool spv_reaches_narrow(const spv_ty_t *t, const uint32_t *w, const size_t *sm_at, uint32_t bound, uint32_t id, unsigned depth, uint32_t *w16, uint32_t *w8, bool *fail) {
    if (id >= bound || depth > 16u) { *fail = true; return false; }   /* unanalysable → the caller fails closed */
    const spv_ty_t *x = &t[id];
    switch (x->kind) {
    case 1: case 2: if (x->width == 16) *w16 = 1; if (x->width == 8) *w8 = 1; return x->width == 16 || x->width == 8;
    case 4: case 6: return spv_reaches_narrow(t, w, sm_at, bound, x->a, depth + 1, w16, w8, fail);
    case 3: {
        size_t at = sm_at[id];
        if (!at) return false;
        uint32_t len = w[at] >> 16;
        bool hit = false;
        for (uint32_t k = 2; k < len; k++) hit |= spv_reaches_narrow(t, w, sm_at, bound, w[at + k], depth + 1, w16, w8, fail);
        return hit;
    }
    default: return false;
    }
}

/* false = the module could not be analysed (allocation failure, id bound too large, ids outside the bound, truncated stream, types nested deeper than the
 * limit): the caller must treat that as "needs everything", never as "needs nothing". */
#define SPV_BOUND_LIMIT (1u << 22)
static bool spv_used_caps(const uint32_t *w, size_t n, uint32_t *out) {
    uint32_t m = 0;
    *out = 0;
    if (n < 5u) return false;
    uint32_t bound = w[3];
    if (!bound || bound > SPV_BOUND_LIMIT) return false;
    spv_ty_t *t = calloc(bound, sizeof *t);
    size_t *at = calloc(bound, sizeof *at);
    if (!t || !at) { free(t); free(at); return false; }
    bool fail = false;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
        if (!len || i + len > n) { fail = true; break; }
        bool ty = op == 21 || op == 22 || op == 23 || op == 24 || op == 28 || op == 29 || op == 30 || op == 32;
        if (ty && (len < 2u || w[i + 1] >= bound)) { fail = true; break; }
        switch (op) {
        case 21: if (len >= 4) {   /* OpTypeInt id width sign */
                t[w[i + 1]] = (spv_ty_t){ 1, (uint8_t)w[i + 2], 0, 0 };
                if (w[i + 2] == 8) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT8);
                if (w[i + 2] == 16) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT16);
                if (w[i + 2] == 64) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT64);
            } else fail = true; break;
        case 22: if (len >= 3) {   /* OpTypeFloat id width */
                t[w[i + 1]] = (spv_ty_t){ 2, (uint8_t)w[i + 2], 0, 0 };
                if (w[i + 2] == 16) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_FLOAT16);
            } else fail = true; break;
        case 23: case 24: if (len >= 4) t[w[i + 1]] = (spv_ty_t){ 6, 0, w[i + 2], 0 }; else fail = true; break;   /* vector / matrix (column type) */
        case 28: case 29: if (len >= 3) t[w[i + 1]] = (spv_ty_t){ 4, 0, w[i + 2], 0 }; else fail = true; break;   /* array / runtime array */
        case 30: t[w[i + 1]] = (spv_ty_t){ 3, 0, 0, 0 }; at[w[i + 1]] = i; break;   /* struct */
        case 32: if (len >= 4) t[w[i + 1]] = (spv_ty_t){ 5, 0, w[i + 3], w[i + 2] }; else fail = true; break;   /* pointer */
        default: break;
        }
        if (fail) break;
        i += len;
    }
    for (uint32_t id = 0; !fail && id < bound; id++) {
        if (t[id].kind != 5) continue;
        uint32_t w16 = 0, w8 = 0;
        uint32_t sc = t[id].b;
        if (sc == 12u || sc == 2u || sc == 9u || sc == 1u || sc == 3u) {   /* StorageBuffer, Uniform, PushConstant, Input, Output */
            (void)spv_reaches_narrow(t, w, at, bound, t[id].a, 0, &w16, &w8, &fail);
            if (sc == 12u) { if (w16) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SB16); if (w8) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SB8); }
            else if (sc == 2u) { if (w16) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_UB16); if (w8) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_UB8); }
            else if (sc == 9u) { if (w16) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_PC16); if (w8) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_PC8); }
            else if (w16) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_IO16);
        }
    }
    free(t); free(at);
    if (fail) return false;
    *out = m;
    return true;
}

bool lagfx_spv_required_caps_ex(const void *spv, size_t bytes, uint32_t *mask) {
    *mask = LAGFX_VK_CAP_ALL;   /* until proven otherwise */
    if (!spv || bytes < 20u || (bytes & 3u)) return false;
    const uint32_t *w = spv;
    size_t n = bytes / 4u;
    if (w[0] != 0x07230203u) return false;
    uint32_t m = 0;
    if (w[1] > 0x00010300u) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SPIRV14);
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
        if (!len || i + len > n) break;
        if (op == 17u && len >= 2u) {   /* OpCapability */
            switch (w[i + 1]) {
            case 11: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT64); break;
            case 22: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT16); break;
            case 39: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT8); break;
            case 9:  m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_FLOAT16); break;
            case 4448: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SB8); break;
            case 4449: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_UB8); break;
            case 4450: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_PC8); break;
            case 4433: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SB16); break;
            case 4434: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_UB16); break;
            case 4435: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_PC16); break;
            case 4436: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_IO16); break;
            case 4441: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_VARPTR_SB); break;
            case 4442: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_VARPTR); break;
            case 61: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SUBGROUP); break;                                        /* GroupNonUniform */
            case 65: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SUBGROUP) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SUBGROUP_SHUFFLE); break; /* GroupNonUniformShuffle */
            case 66: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SUBGROUP) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SUBGROUP_SHUFFLE_RELATIVE); break; /* GroupNonUniformShuffleRelative */
            default: break;
            }
        } else if (op != 17u && i > 5u && op >= 71u) {   /* capabilities come first in a module: past the first decoration / type / function we are done */
            break;
        }
        i += len;
    }
    uint32_t used;
    if (!spv_used_caps(w, n, &used)) return false;
    *mask = m | used;
    return true;
}

uint32_t lagfx_spv_required_caps(const void *spv, size_t bytes) {
    uint32_t m;
    return lagfx_spv_required_caps_ex(spv, bytes, &m) ? m : 0;   /* legacy view: 0 = not analysable; the pipeline gate uses _ex and fails closed */
}

const char *lagfx_vk_caps_line(const struct lagfx_vk_state *vk, char *buf, size_t n) {
    char en[240] = "", ms[240] = "";
    size_t eo = 0, mo = 0;
    for (unsigned i = 0; vk && i < LAGFX_VK_CAP_COUNT; i++) {
        if (vk->caps_enabled & LAGFX_VK_CAP_BIT(i)) eo += (size_t)snprintf(en + eo, sizeof en - eo, "%s%s", eo ? "," : "", k_names[i]);
        else mo += (size_t)snprintf(ms + mo, sizeof ms - mo, "%s%s", mo ? "," : "", k_names[i]);
    }
    snprintf(buf, n, "g7-gpu-caps: enabled=[%s] missing=[%s] (shaders needing a missing capability are not built; their draws are skipped, stub_missing_cap_<name>)", en, ms);
    return buf;
}

void lagfx_vk_caps_inject_missing(struct lagfx_vk_state *vk, uint32_t mask) {
    if (!vk) return;
    vk->caps_enabled &= ~mask;
    vk->caps_missing |= mask & LAGFX_VK_CAP_ALL;
}

/* ---- declaring what a module uses ----------------------------------------------------------------------------------------------------------------- */
bool lagfx_spv_declare_used_caps(uint8_t **spv, size_t *bytes) {
    if (!spv || !*spv || !bytes || *bytes < 20u || (*bytes & 3u)) return true;
    const uint32_t *w = (const uint32_t *)*spv;
    size_t n = *bytes / 4u;
    if (w[0] != 0x07230203u) return true;
    static const struct { unsigned cap; uint32_t id; bool is8; } k[] = {
        { LAGFX_VK_CAP_SB16, 4433, false }, { LAGFX_VK_CAP_UB16, 4434, false }, { LAGFX_VK_CAP_PC16, 4435, false }, { LAGFX_VK_CAP_IO16, 4436, false },
        { LAGFX_VK_CAP_SB8, 4448, true }, { LAGFX_VK_CAP_UB8, 4449, true }, { LAGFX_VK_CAP_PC8, 4450, true },
    };
    uint32_t used;
    if (!spv_used_caps(w, n, &used)) return false;   /* unanalysable: do not pretend the module needs nothing */
    /* what is ALREADY declared: scan OpCapability words directly */
    uint32_t declared_ids[64]; unsigned nd = 0;
    size_t after_caps = 5; bool has_ext8 = false, has_ext16 = false;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
        if (!len || i + len > n) break;
        if (op == 17u) { if (nd >= 64u) return false; declared_ids[nd++] = w[i + 1]; after_caps = i + len; }
        else if (op == 10u) { if (len >= 2u && !strncmp((const char *)&w[i + 1], "SPV_KHR_8bit_storage", 20)) has_ext8 = true; if (len >= 2u && !strncmp((const char *)&w[i + 1], "SPV_KHR_16bit_storage", 21)) has_ext16 = true; }
        else if (op != 10u) break;   /* capabilities and extensions come first; the first other instruction ends the preamble */
        i += len;
    }
    uint32_t add[8]; unsigned na = 0; bool need8 = false, need16 = false;
    for (unsigned j = 0; j < sizeof k / sizeof k[0]; j++) {
        if (!(used & LAGFX_VK_CAP_BIT(k[j].cap))) continue;
        bool dup = false;
        for (unsigned d = 0; d < nd; d++) if (declared_ids[d] == k[j].id) dup = true;
        if (dup) continue;
        add[na++] = k[j].id;
        if (k[j].is8) need8 = true; else need16 = true;
    }
    bool ext = need8 && !has_ext8 && w[1] < 0x00010500u;
    bool ext16 = need16 && !has_ext16 && w[1] < 0x00010300u;   /* 16-bit storage is core from SPIR-V 1.3 */
    if (!na && !ext && !ext16) return true;
    size_t extra = (size_t)na * 2u + (ext ? 7u : 0u) + (ext16 ? 7u : 0u);
    uint32_t *nw = malloc((n + extra) * 4u);
    if (!nw) return false;
    memcpy(nw, w, after_caps * 4u);
    size_t o = after_caps;
    for (unsigned a = 0; a < na; a++) { nw[o++] = (2u << 16) | 17u; nw[o++] = add[a]; }
    if (ext) { nw[o++] = (7u << 16) | 10u; char name[24] = "SPV_KHR_8bit_storage"; memcpy(&nw[o], name, 24); o += 6; }
    if (ext16) { nw[o++] = (7u << 16) | 10u; char name[24] = "SPV_KHR_16bit_storage"; memcpy(&nw[o], name, 24); o += 6; }
    memcpy(nw + o, w + after_caps, (n - after_caps) * 4u);
    free(*spv);
    *spv = (uint8_t *)nw;
    *bytes = (n + extra) * 4u;
    return true;
}

bool lagfx_vk_can_pin_subgroup32(const struct lagfx_vk_state *vk) {
    if (!vk) return false;
    if (vk->sg_size == 32u && vk->sg_min <= 32u && vk->sg_max >= 32u && vk->sg_min == vk->sg_max) return true;   /* fixed 32 (a range makes the driver free to choose: pin it) */
    return vk->sg_control && vk->sg_min <= 32u && vk->sg_max >= 32u;
}
