/* libapplegfx-vulkan — SPIR-V capability scan for the device-capability gate (see caps.h). */
#include <stdlib.h>
#include <stdbool.h>
#include "instance.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static const char *const k_names[LAGFX_VK_CAP_COUNT] = { "int64", "int16", "int8", "float16", "storage8", "storage16", "varptr_sb", "varptr", "spirv14" };

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

static bool spv_reaches_narrow(const spv_ty_t *t, const uint32_t *w, const size_t *sm_at, uint32_t id, unsigned depth, uint32_t *w16, uint32_t *w8) {
    if (id >= SPV_MAXID || depth > 8u) return false;
    const spv_ty_t *x = &t[id];
    switch (x->kind) {
    case 1: case 2: if (x->width == 16) *w16 = 1; if (x->width == 8) *w8 = 1; return x->width == 16 || x->width == 8;
    case 4: case 6: return spv_reaches_narrow(t, w, sm_at, x->a, depth + 1, w16, w8);
    case 3: {
        size_t at = sm_at[id];
        if (!at) return false;
        uint32_t len = w[at] >> 16;
        bool hit = false;
        for (uint32_t k = 2; k < len; k++) hit |= spv_reaches_narrow(t, w, sm_at, w[at + k], depth + 1, w16, w8);
        return hit;
    }
    default: return false;
    }
}

static uint32_t spv_used_caps(const uint32_t *w, size_t n) {
    uint32_t m = 0;
    spv_ty_t *t = calloc(SPV_MAXID, sizeof *t);
    size_t *at = calloc(SPV_MAXID, sizeof *at);
    if (!t || !at) { free(t); free(at); return 0; }
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
        if (!len || i + len > n) break;
        switch (op) {
        case 21: if (len >= 4 && w[i + 1] < SPV_MAXID) {   /* OpTypeInt id width sign */
                t[w[i + 1]] = (spv_ty_t){ 1, (uint8_t)w[i + 2], 0, 0 };
                if (w[i + 2] == 8) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT8);
                if (w[i + 2] == 16) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT16);
                if (w[i + 2] == 64) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT64);
            } break;
        case 22: if (len >= 3 && w[i + 1] < SPV_MAXID) {   /* OpTypeFloat id width */
                t[w[i + 1]] = (spv_ty_t){ 2, (uint8_t)w[i + 2], 0, 0 };
                if (w[i + 2] == 16) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_FLOAT16);
            } break;
        case 23: if (len >= 4 && w[i + 1] < SPV_MAXID) t[w[i + 1]] = (spv_ty_t){ 6, 0, w[i + 2], 0 }; break;   /* vector */
        case 28: case 29: if (len >= 3 && w[i + 1] < SPV_MAXID) t[w[i + 1]] = (spv_ty_t){ 4, 0, w[i + 2], 0 }; break;   /* array / runtime array */
        case 30: if (len >= 2 && w[i + 1] < SPV_MAXID) { t[w[i + 1]] = (spv_ty_t){ 3, 0, 0, 0 }; at[w[i + 1]] = i; } break;   /* struct */
        case 32: if (len >= 4 && w[i + 1] < SPV_MAXID) t[w[i + 1]] = (spv_ty_t){ 5, 0, w[i + 3], w[i + 2] }; break;   /* pointer */
        default: break;
        }
        i += len;
    }
    for (uint32_t id = 0; id < SPV_MAXID; id++) {
        if (t[id].kind != 5) continue;
        uint32_t w16 = 0, w8 = 0;
        uint32_t sc = t[id].b;
        if (sc == 12u || sc == 2u || sc == 9u || sc == 1u || sc == 3u) {   /* StorageBuffer, Uniform, PushConstant, Input, Output */
            (void)spv_reaches_narrow(t, w, at, t[id].a, 0, &w16, &w8);
            if (w16) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_STORAGE16);
            if (w8 && sc != 1u && sc != 3u) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_STORAGE8);
        }
    }
    free(t); free(at);
    return m;
}

uint32_t lagfx_spv_required_caps(const void *spv, size_t bytes) {
    if (!spv || bytes < 20u || (bytes & 3u)) return 0;
    const uint32_t *w = spv;
    size_t n = bytes / 4u;
    if (w[0] != 0x07230203u) return 0;
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
            case 4448: case 4449: case 4450: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_STORAGE8); break;
            case 4433: case 4434: case 4435: case 4436: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_STORAGE16); break;
            case 4441: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_VARPTR_SB); break;
            case 4442: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_VARPTR); break;
            default: break;
            }
        } else if (op != 17u && i > 5u && op >= 71u) {   /* capabilities come first in a module: past the first decoration / type / function we are done */
            break;
        }
        i += len;
    }
    return m | spv_used_caps(w, n);
}

const char *lagfx_vk_caps_line(const struct lagfx_vk_state *vk, char *buf, size_t n) {
    char en[160] = "", ms[160] = "";
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
