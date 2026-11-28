/* libapplegfx-vulkan — SPIR-V capability scan for the device-capability gate (see caps.h). */
#include <stdlib.h>
#include <stdbool.h>
#include "instance.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static const char *const k_names[LAGFX_VK_CAP_COUNT] = { "int64", "int16", "int8", "float16", "sb8", "ub8", "pc8", "sb16", "ub16", "pc16", "io16", "varptr_sb", "varptr", "spirv14", "subgroup", "sg_shuffle", "sg_shuffle_rel", "demote", "atomic64_buf", "atomic64_shared", "atomic_f32_buf", "atomic_f32_buf_add", "atomic_f32_shared", "atomic_f32_shared_add", "atomic_f32_image" };

const lagfx_vk_atomic_feat_t *lagfx_vk_atomic_feat_inject = NULL;

uint32_t lagfx_vk_atomic_caps(const lagfx_vk_atomic_feat_t *f) {
    uint32_t m = 0;
    if (!f) return 0;
    if (f->shader_int64 && f->buffer_int64_atomics) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC64_BUF);       /* device memory 64-bit atomics do not need the shared feature */
    if (f->shader_int64 && f->shared_int64_atomics) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC64_SHARED);    /* threadgroup memory 64-bit atomics do not need the buffer feature */
    if (f->af_ext) {
        if (f->buffer_f32) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_BUF);
        if (f->buffer_f32_add) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_BUF_ADD);
        if (f->shared_f32) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_SHARED);
        if (f->shared_f32_add) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_SHARED_ADD);
        if (f->image_f32) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_IMAGE);
    }
    return m;
}

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
static bool spv_used_caps(const uint32_t *w, size_t n, bool decl_i64atomics, bool decl_f32add, uint32_t *out) {
    uint32_t m = 0;
    *out = 0;
    if (n < 5u) return false;
    uint32_t bound = w[3];
    if (!bound || bound > SPV_BOUND_LIMIT) return false;
    spv_ty_t *t = calloc(bound, sizeof *t);
    size_t *at = calloc(bound, sizeof *at);
    uint32_t *pty = calloc(bound, sizeof *pty);   /* result id -> result type id, for the ids that can be an atomic's pointer (OpVariable, access chains, function parameters, phi / select / copy of pointers) */
    if (!t || !at || !pty) { free(t); free(at); free(pty); return false; }
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
        case 59: case 65: case 66: case 67: case 70: case 57: case 245: case 169: case 83: case 60: case 61:   /* OpVariable, [InBounds][Ptr]AccessChain, FunctionParameter, Phi, Select, CopyObject, ImageTexelPointer, Load: <type> <id> ... */
            if (len >= 3u && w[i + 1] < bound && w[i + 2] < bound) pty[w[i + 2]] = w[i + 1];
            break;
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
    /* Atomic uses, attributed to the storage class of the atomic's pointer (the pointee type says 64-bit integer / 32-bit float). A pointer whose class cannot be determined is charged to every class. */
    if (!fail) {
        bool unk_ptr = false, attributed64 = false, attributed_f = false;
        const uint32_t B64 = LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC64_BUF), S64 = LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC64_SHARED);
        for (size_t i = 5; i < n;) {
            uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
            if (!len || i + len > n) break;
            if ((op >= 227u && op <= 242u) || op == 6035u) {
                const bool store = op == 228u;
                uint32_t pid = store ? (len >= 2u ? w[i + 1] : 0u) : (len >= 4u ? w[i + 3] : 0u);
                uint32_t rty = (!store && len >= 3u && w[i + 1] < bound) ? w[i + 1] : 0u;    /* result type: the value type of every atomic but the store */
                uint32_t sc = ~0u, pointee = 0u;
                if (pid && pid < bound && pty[pid] && pty[pid] < bound && t[pty[pid]].kind == 5) { sc = t[pty[pid]].b; pointee = t[pty[pid]].a; }
                const spv_ty_t *vt = (pointee && pointee < bound) ? &t[pointee] : (rty ? &t[rty] : NULL);
                if (sc == ~0u) unk_ptr = true;
                if (vt && vt->kind == 1 && vt->width == 64) {
                    attributed64 = true;
                    if (sc == 12u) m |= B64; else if (sc == 4u) m |= S64; else m |= B64 | S64;
                } else if (vt && vt->kind == 2 && vt->width == 32) {
                    const bool add = op == 6035u;
                    attributed_f = true;
                    if (sc == 12u) m |= LAGFX_VK_CAP_BIT(add ? LAGFX_VK_CAP_ATOMIC_F32_BUF_ADD : LAGFX_VK_CAP_ATOMIC_F32_BUF);
                    else if (sc == 4u) m |= LAGFX_VK_CAP_BIT(add ? LAGFX_VK_CAP_ATOMIC_F32_SHARED_ADD : LAGFX_VK_CAP_ATOMIC_F32_SHARED);
                    else if (sc == 11u) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_IMAGE);
                    else m |= add ? (LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_BUF_ADD) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_SHARED_ADD) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_IMAGE))
                                  : (LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_BUF) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_SHARED) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_IMAGE));
                }
            }
            i += len;
        }
        /* a declared atomic capability that no use explains (a store through an unanalysable pointer, a hand-patched module): every class of it is required */
        if (decl_i64atomics && (!attributed64 || unk_ptr)) m |= B64 | S64;
        if (decl_f32add && (!attributed_f || unk_ptr)) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_BUF_ADD) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_SHARED_ADD) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_IMAGE);
    }
    free(t); free(at); free(pty);
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
    bool decl_i64atomics = false, decl_f32add = false;
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
            case 5379: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_DEMOTE); break;   /* DemoteToHelperInvocation <- shaderDemoteToHelperInvocation */
            case 12: decl_i64atomics = true; break;                                                              /* Int64Atomics: the class (buffer / shared) comes from the uses, see spv_used_caps */
            case 6033: decl_f32add = true; break;                                                                /* AtomicFloat32AddEXT: likewise */
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
    if (!spv_used_caps(w, n, decl_i64atomics, decl_f32add, &used)) return false;
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
    if (!spv_used_caps(w, n, false, false, &used)) return false;   /* unanalysable: do not pretend the module needs nothing */
    /* what is ALREADY declared: scan OpCapability words directly */
    uint32_t declared_ids[64]; unsigned nd = 0;
    size_t after_caps = 5; bool has_ext8 = false, has_ext16 = false, has_extd = false;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
        if (!len || i + len > n) break;
        if (op == 17u) { if (nd >= 64u) return false; declared_ids[nd++] = w[i + 1]; after_caps = i + len; }
        else if (op == 10u) { if (len >= 2u && !strncmp((const char *)&w[i + 1], "SPV_KHR_8bit_storage", 20)) has_ext8 = true; if (len >= 2u && !strncmp((const char *)&w[i + 1], "SPV_KHR_16bit_storage", 21)) has_ext16 = true; }
        else if (op != 10u) break;   /* capabilities and extensions come first; the first other instruction ends the preamble */
        if (op == 10u && len >= 2u && !strncmp((const char *)&w[i + 1], "SPV_EXT_demote_to_helper_invocation", 35)) has_extd = true;
        i += len;
    }
    /* OpDemoteToHelperInvocation (5380) anywhere in the body needs Capability DemoteToHelperInvocation (5379) and, below SPIR-V 1.6, its extension (S06: air.discard_fragment) */
    bool uses_demote = false;
    for (size_t i = 5; i < n;) { uint32_t len = w[i] >> 16; if (!len || i + len > n) break; if ((w[i] & 0xffffu) == 5380u) uses_demote = true; i += len; }
    bool has_demote_cap = false;
    for (unsigned d = 0; d < nd; d++) if (declared_ids[d] == 5379u) has_demote_cap = true;
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
    if (uses_demote && !has_demote_cap) add[na++] = 5379u;
    bool extd = uses_demote && !has_extd && w[1] < 0x00010600u;
    if (!na && !ext && !ext16 && !extd) return true;
    size_t extra = (size_t)na * 2u + (ext ? 7u : 0u) + (ext16 ? 7u : 0u) + (extd ? 10u : 0u);
    uint32_t *nw = malloc((n + extra) * 4u);
    if (!nw) return false;
    memcpy(nw, w, after_caps * 4u);
    size_t o = after_caps;
    for (unsigned a = 0; a < na; a++) { nw[o++] = (2u << 16) | 17u; nw[o++] = add[a]; }
    if (ext) { nw[o++] = (7u << 16) | 10u; char name[24] = "SPV_KHR_8bit_storage"; memcpy(&nw[o], name, 24); o += 6; }
    if (ext16) { nw[o++] = (7u << 16) | 10u; char name[24] = "SPV_KHR_16bit_storage"; memcpy(&nw[o], name, 24); o += 6; }
    if (extd) { nw[o++] = (10u << 16) | 10u; char name[36] = "SPV_EXT_demote_to_helper_invocation"; memcpy(&nw[o], name, 36); o += 9; }
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

void lagfx_spv_int64_report(const void *spv, size_t bytes, char *out, size_t out_n) {
    const uint32_t *w = spv; const size_t n = bytes / 4u;
    if (!out || !out_n) return;
    out[0] = 0;
    if (!w || n < 6u || w[0] != 0x07230203u) { snprintf(out, out_n, "not a SPIR-V module"); return; }
    const uint32_t bound = w[3];
    if (bound == 0 || bound > (1u << 22)) { snprintf(out, out_n, "bad id bound"); return; }
    uint8_t *is64 = calloc(bound, 1);   /* id is an OpTypeInt 64 (or a vector / pointee-free type of one is not tracked: scalars and vectors of it) */
    uint8_t *val64 = calloc(bound, 1);  /* id is a value whose type is 64-bit int */
    if (!is64 || !val64) { free(is64); free(val64); snprintf(out, out_n, "oom"); return; }
    unsigned tu = 0, ts = 0, cst = 0, conv = 0, arith = 0, bits = 0, mem = 0, idx = 0, other = 0;
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 5; i < n;) {
            const uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
            if (!len || i + len > n) break;
            if (pass == 0) {
                if (op == 21u && len >= 4u && w[i + 2] == 64u && w[i + 1] < bound) { is64[w[i + 1]] = 1; if (w[i + 3]) ts++; else tu++; }
                if (op == 23u && len >= 4u && w[i + 1] < bound && w[i + 2] < bound && is64[w[i + 2]]) is64[w[i + 1]] = 1;   /* OpTypeVector */
            } else if (len >= 3u && w[i + 1] < bound && is64[w[i + 1]] && w[i + 2] < bound) {   /* result type is 64-bit int: w[i+2] is the result id */
                val64[w[i + 2]] = 1;
                if (op == 43u || op == 44u || op == 46u) cst++;
                else if (op == 113u || op == 114u || op == 115u || op == 116u || op == 117u || op == 120u) conv++;
                else if ((op >= 128u && op <= 141u)) arith++;
                else if ((op >= 194u && op <= 199u) || op == 200u || op == 201u || op == 202u) bits++;
                else if (op == 61u) mem++;
                else other++;
            }
            if (pass == 1 && (op == 65u || op == 66u || op == 67u || op == 68u) && len >= 5u)   /* OpAccessChain / InBounds / PtrAccessChain / InBoundsPtrAccessChain: indices from word 4 on */
                for (size_t k = 4; k < len; k++) { const uint32_t id = w[i + k]; if (id < bound && val64[id]) idx++; }
            i += len;
        }
    snprintf(out, out_n, "i64 types(u=%u s=%u) consts=%u convert=%u arith=%u bitops=%u loads=%u other=%u access_chain_indices=%u", tu, ts, cst, conv, arith, bits, mem, other, idx);
    free(is64); free(val64);
}
