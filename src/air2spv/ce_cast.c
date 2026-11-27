/*
 * libapplegfx-vulkan - constant-expression casts of scalar constants (see ce_cast.h)
 * src/air2spv/ce_cast.c
 *
 * SPDX-License-Identifier: MIT
 */
#include "ce_cast.h"

#include <math.h>
#include <string.h>

uint64_t lagfx_ce_mask(uint32_t w) { return w >= 64u ? ~0ull : ((1ull << w) - 1ull); }

/* round-to-nearest-even float32 -> binary16 bit pattern */
uint32_t lagfx_f32_to_f16(float f) {
    uint32_t x; memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u, exp = (x >> 23) & 0xFFu, man = x & 0x7FFFFFu;
    if (exp == 0xFFu) return sign | 0x7C00u | (man ? (0x200u | (man >> 13)) : 0u);   /* inf / NaN (quiet) */
    int e = (int)exp - 127 + 15;
    if (e >= 0x1F) return sign | 0x7C00u;                                            /* overflow -> inf */
    if (e <= 0) {                                                                    /* subnormal / zero */
        if (e < -10) return sign;
        man |= 0x800000u;
        uint32_t shift = (uint32_t)(14 - e);
        uint32_t half = man >> shift, rem = man & ((1u << shift) - 1u), mid = 1u << (shift - 1u);
        if (rem > mid || (rem == mid && (half & 1u))) half++;
        return sign | half;
    }
    uint32_t half = ((uint32_t)e << 10) | (man >> 13), rem = man & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u))) half++;                    /* may carry into the exponent: correct (-> inf at the top) */
    return sign | half;
}
float lagfx_f16_to_f32(uint32_t h) {
    uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu, x;
    if (exp == 0u) {
        if (man == 0u) x = sign;
        else { int e = -1; do { e++; man <<= 1; } while (!(man & 0x400u)); x = sign | ((uint32_t)(127 - 15 - e) << 23) | ((man & 0x3FFu) << 13); }
    } else if (exp == 0x1Fu) x = sign | 0x7F800000u | (man << 13);
    else x = sign | ((exp - 15u + 127u) << 23) | (man << 13);
    float f; memcpy(&f, &x, 4); return f;
}

/* One CE_CAST of a known source: the folded value. Returns false when the cast is not foldable. dst_* describe the destination type. */
bool lagfx_ce_fold(uint32_t opc, const lagfx_ce_val_t *s, bool dst_f, bool dst_p, uint32_t dw, lagfx_ce_val_t *o) {
    memset(o, 0, sizeof *o);
    o->width = dw; o->is_float = dst_f; o->ok = true;
    if (s->undef) { o->undef = true; return true; }
    uint32_t sw = s->width;
    switch (opc) {
        case 0:  /* trunc */  if (s->is_float || dst_f || dst_p || s->is_ptr_null || dw >= sw) return false; o->bits = s->bits & lagfx_ce_mask(dw); return true;
        case 1:  /* zext  */  if (s->is_float || dst_f || dst_p || s->is_ptr_null || dw <= sw) return false; o->bits = s->bits & lagfx_ce_mask(sw); return true;
        case 2: {/* sext  */  if (s->is_float || dst_f || dst_p || s->is_ptr_null || dw <= sw) return false;
            uint64_t v = s->bits & lagfx_ce_mask(sw); if (v & (1ull << (sw - 1u))) v |= ~lagfx_ce_mask(sw);
            o->bits = v & lagfx_ce_mask(dw); return true; }
        case 3: case 4: {   /* fptoui / fptosi */
            if (!s->is_float || dst_f || dst_p || dw == 1u) return false;
            double d; { uint32_t b = (uint32_t)s->bits; float f; memcpy(&f, &b, 4); d = sw == 16u ? (double)lagfx_f16_to_f32(b) : (double)f; }
            if (d != d) { o->undef = true; return true; }                       /* NaN -> poison */
            d = trunc(d);
            if (opc == 3) { if (d < 0.0 || d >= ldexp(1.0, (int)dw)) { o->undef = true; return true; } o->bits = (uint64_t)d; }
            else { if (d < -ldexp(1.0, (int)dw - 1) || d >= ldexp(1.0, (int)dw - 1)) { o->undef = true; return true; } o->bits = (uint64_t)(int64_t)d & lagfx_ce_mask(dw); }
            return true; }
        case 5: case 6: {   /* uitofp / sitofp */
            if (s->is_float || !dst_f || s->is_ptr_null || sw == 1u) return false;
            float f;
            if (opc == 5) f = (float)(uint64_t)(s->bits & lagfx_ce_mask(sw));
            else { uint64_t v = s->bits & lagfx_ce_mask(sw); if (v & (1ull << (sw - 1u))) v |= ~lagfx_ce_mask(sw); f = (float)(int64_t)v; }
            if (dw == 32u) { uint32_t b; memcpy(&b, &f, 4); o->bits = b; } else o->bits = lagfx_f32_to_f16(f);
            return true; }
        case 7: case 8: {   /* fptrunc / fpext (half <-> float) */
            if (!s->is_float || !dst_f) return false;
            if (opc == 7 && !(sw == 32u && dw == 16u)) return false;
            if (opc == 8 && !(sw == 16u && dw == 32u)) return false;
            if (opc == 7) { uint32_t b = (uint32_t)s->bits; float f; memcpy(&f, &b, 4); o->bits = lagfx_f32_to_f16(f); }
            else { float f = lagfx_f16_to_f32((uint32_t)s->bits); uint32_t b; memcpy(&b, &f, 4); o->bits = b; }
            return true; }
        case 9:  /* ptrtoint: only the null pointer has a known address */
            if (!s->is_ptr_null || dst_f || dst_p) return false; o->bits = 0; return true;
        case 10: /* inttoptr: only 0 -> null */
            if (s->is_float || s->is_ptr_null || !dst_p || (s->bits & lagfx_ce_mask(sw)) != 0u) return false; o->is_ptr_null = true; o->bits = 0; return true;
        case 11: /* bitcast */
            if (dst_p || s->is_ptr_null) { if (dst_p && s->is_ptr_null) { o->is_ptr_null = true; return true; } return false; }
            if (sw != dw) return false;
            o->bits = s->bits & lagfx_ce_mask(dw); return true;   /* int<->float of equal width, int->int, float->float: the bit pattern is the value */
        case 12: /* addrspacecast: null stays null (the same address) */
            if (dst_p && s->is_ptr_null) { o->is_ptr_null = true; return true; }
            return false;
        default: return false;
    }
}
