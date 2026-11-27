/* SPDX-License-Identifier: MIT */
/*
 * qp_spv.h - tiny SPIR-V emitter for the VK_QCOM_tile_shading device probe (tools/qcom-tile-probe).
 *
 * Header-only, no Vulkan dependency, no external assembler needed on the phone: the probe generates its own modules at run time.
 * Token values (checked against SPIRV-Registry extensions/QCOM/SPV_QCOM_tile_shading.asciidoc, revision 1, and the Khronos
 * VK_QCOM_tile_shading proposal):
 *   Capability TileShadingQCOM = 4495, StorageClass TileAttachmentQCOM = 4491,
 *   ExecutionMode NonCoherentTileAttachmentReadQCOM = 4489 (fragment only, not emitted here), TileShadingRateQCOM = 4490 (GLCompute, three literals),
 *   BuiltIn TileOffsetQCOM = 4492 (uvec2), TileDimensionQCOM = 4493 (uvec3), TileApronSizeQCOM = 4494 (uvec2), OpExtension "SPV_QCOM_tile_shading".
 * The core SPIR-V tokens are from the SPIR-V 1.3 specification (Khronos, public).
 *
 * "Twin" mode: the same module with every tile-shading token replaced by a core one (no capability / extension / execution mode, the tile
 * attachment is a UniformConstant storage image, TileOffset/TileDimension become WorkgroupId/NumWorkgroups uvec3 inputs). spirv-val of the
 * installed SPIRV-Tools does not know capability 4495, so the twin is what offline validation runs on: it has the identical structure
 * (types, control flow, group operations, push constants) and differs in exactly those tokens.
 */
#ifndef QP_SPV_H
#define QP_SPV_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define QP_CAP_TILE_SHADING 4495u
#define QP_SC_TILE_ATTACHMENT 4491u
#define QP_EM_TILE_SHADING_RATE 4490u
#define QP_EM_NONCOHERENT_TILE_READ 4489u
#define QP_BI_TILE_OFFSET 4492u
#define QP_BI_TILE_DIMENSION 4493u
#define QP_BI_TILE_APRON 4494u

/* SPIR-V ImageFormat operands used */
enum { QP_IF_UNKNOWN = 0, QP_IF_RGBA32F = 1, QP_IF_RGBA16F = 2, QP_IF_RGBA8 = 4, QP_IF_RGB10A2 = 11 };

typedef enum { QK_DUMP = 0, QK_PATTERN, QK_RMW, QK_SIMD } qp_kkind;

/* One compute kernel variant. All kernels share one push-constant block of 8 uints:
 *   [0..3] area  x0 y0 x1 y1   (pixels outside are never written; the render area)
 *   [4..7] region x0 y0 x1 y1  (second clip; = area except in the region-limit case of E6) */
typedef struct {
    const char *name;
    qp_kkind kind;
    int tile;            /* 1: image is a TileAttachmentQCOM variable and the pixel comes from TileOffsetQCOM; 0: plain storage image + GlobalInvocationId */
    int ifmt;            /* QP_IF_* */
    int arrayed;         /* tile image declared Arrayed, coordinate uvec3(x,y,0) */
    int rel;             /* tile only: coordinate = pixel - TileOffset (tile-relative convention) */
    uint32_t ls_x, ls_y; /* LocalSize (0 = no LocalSize execution mode) */
    uint32_t rate_x, rate_y; /* tile only: TileShadingRateQCOM (rate_z = 1); 0 = not emitted. Pixel = TileOffset + GlobalInvocationId * rate + (dx,dy) */
    int pat_div;         /* QK_PATTERN: 15 (unorm formats) or 16 (float formats; exact) */
    uint32_t logical;    /* QK_SIMD: logical simdgroup width (power of two, <= 64) */
} qp_kspec;

typedef struct { uint32_t *w; uint32_t n, cap; } qp_wb;

typedef struct {
    qp_wb caps, exts, mem, entry, modes, annot, types, body;
    uint32_t next_id;
    uint32_t t_void, t_bool, t_f32, t_u32, t_i32, t_v2u, t_v3u, t_v4f;
    struct { uint32_t ty, val, id; } k[96]; int nk;
    struct { uint32_t sc, ty, id; } p[24]; int np;
    uint32_t iface[8]; int niface;
} qp_b;

static void qp_put(qp_wb *b, uint32_t v) {
    if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 256; b->w = (uint32_t *)realloc(b->w, b->cap * 4); }
    b->w[b->n++] = v;
}
static void qp_ins(qp_wb *b, uint32_t op, uint32_t na, const uint32_t *a) {
    qp_put(b, ((na + 1) << 16) | op);
    for (uint32_t i = 0; i < na; i++) qp_put(b, a[i]);
}
#define QP_EMIT(buf, op, ...) do { uint32_t a_[] = { __VA_ARGS__ }; qp_ins((buf), (op), (uint32_t)(sizeof a_ / 4), a_); } while (0)

static uint32_t qp_nid(qp_b *b) { return b->next_id++; }

/* literal string -> words (nul padded) */
static uint32_t qp_str_words(const char *s, uint32_t *out) {
    uint32_t n = (uint32_t)strlen(s) + 1, nw = (n + 3) / 4;
    for (uint32_t i = 0; i < nw; i++) out[i] = 0;
    memcpy(out, s, n);
    return nw;
}

/* ---- types / constants (appended to the types section on first use, so definitions always precede uses) ---- */
static uint32_t qp_t_void(qp_b *b) { if (!b->t_void) { b->t_void = qp_nid(b); QP_EMIT(&b->types, 19, b->t_void); } return b->t_void; }
static uint32_t qp_t_bool(qp_b *b) { if (!b->t_bool) { b->t_bool = qp_nid(b); QP_EMIT(&b->types, 20, b->t_bool); } return b->t_bool; }
static uint32_t qp_t_f32(qp_b *b) { if (!b->t_f32) { b->t_f32 = qp_nid(b); QP_EMIT(&b->types, 22, b->t_f32, 32); } return b->t_f32; }
static uint32_t qp_t_u32(qp_b *b) { if (!b->t_u32) { b->t_u32 = qp_nid(b); QP_EMIT(&b->types, 21, b->t_u32, 32, 0); } return b->t_u32; }
static uint32_t qp_t_i32(qp_b *b) { if (!b->t_i32) { b->t_i32 = qp_nid(b); QP_EMIT(&b->types, 21, b->t_i32, 32, 1); } return b->t_i32; }
static uint32_t qp_t_v2u(qp_b *b) { if (!b->t_v2u) { uint32_t u = qp_t_u32(b); b->t_v2u = qp_nid(b); QP_EMIT(&b->types, 23, b->t_v2u, u, 2); } return b->t_v2u; }
static uint32_t qp_t_v3u(qp_b *b) { if (!b->t_v3u) { uint32_t u = qp_t_u32(b); b->t_v3u = qp_nid(b); QP_EMIT(&b->types, 23, b->t_v3u, u, 3); } return b->t_v3u; }
static uint32_t qp_t_v4f(qp_b *b) { if (!b->t_v4f) { uint32_t f = qp_t_f32(b); b->t_v4f = qp_nid(b); QP_EMIT(&b->types, 23, b->t_v4f, f, 4); } return b->t_v4f; }
static uint32_t qp_t_ptr(qp_b *b, uint32_t sc, uint32_t ty) {
    for (int i = 0; i < b->np; i++) if (b->p[i].sc == sc && b->p[i].ty == ty) return b->p[i].id;
    uint32_t id = qp_nid(b); QP_EMIT(&b->types, 32, id, sc, ty);
    b->p[b->np].sc = sc; b->p[b->np].ty = ty; b->p[b->np].id = id; b->np++;
    return id;
}
static uint32_t qp_k32(qp_b *b, uint32_t ty, uint32_t val) {
    for (int i = 0; i < b->nk; i++) if (b->k[i].ty == ty && b->k[i].val == val) return b->k[i].id;
    uint32_t id = qp_nid(b); QP_EMIT(&b->types, 43, ty, id, val);
    b->k[b->nk].ty = ty; b->k[b->nk].val = val; b->k[b->nk].id = id; b->nk++;
    return id;
}
static uint32_t qp_cu(qp_b *b, uint32_t v) { return qp_k32(b, qp_t_u32(b), v); }
static uint32_t qp_ci(qp_b *b, int32_t v) { return qp_k32(b, qp_t_i32(b), (uint32_t)v); }
static uint32_t qp_cf(qp_b *b, float f) { uint32_t u; memcpy(&u, &f, 4); return qp_k32(b, qp_t_f32(b), u); }

/* ---- instructions (body section) ---- */
static uint32_t qp_i1(qp_b *b, uint32_t op, uint32_t ty, uint32_t x) { uint32_t r = qp_nid(b); QP_EMIT(&b->body, op, ty, r, x); return r; }
static uint32_t qp_i2(qp_b *b, uint32_t op, uint32_t ty, uint32_t x, uint32_t y) { uint32_t r = qp_nid(b); QP_EMIT(&b->body, op, ty, r, x, y); return r; }
static uint32_t qp_i3(qp_b *b, uint32_t op, uint32_t ty, uint32_t x, uint32_t y, uint32_t z) { uint32_t r = qp_nid(b); QP_EMIT(&b->body, op, ty, r, x, y, z); return r; }
static uint32_t qp_uadd(qp_b *b, uint32_t x, uint32_t y) { return qp_i2(b, 128, qp_t_u32(b), x, y); }
static uint32_t qp_usub(qp_b *b, uint32_t x, uint32_t y) { return qp_i2(b, 130, qp_t_u32(b), x, y); }
static uint32_t qp_umul(qp_b *b, uint32_t x, uint32_t y) { return qp_i2(b, 132, qp_t_u32(b), x, y); }
static uint32_t qp_umod(qp_b *b, uint32_t x, uint32_t y) { return qp_i2(b, 137, qp_t_u32(b), x, y); }
static uint32_t qp_uand(qp_b *b, uint32_t x, uint32_t y) { return qp_i2(b, 199, qp_t_u32(b), x, y); }
static uint32_t qp_ushr(qp_b *b, uint32_t x, uint32_t y) { return qp_i2(b, 194, qp_t_u32(b), x, y); }
static uint32_t qp_fadd(qp_b *b, uint32_t x, uint32_t y) { return qp_i2(b, 129, qp_t_f32(b), x, y); }
static uint32_t qp_fmul(qp_b *b, uint32_t x, uint32_t y) { return qp_i2(b, 133, qp_t_f32(b), x, y); }
static uint32_t qp_u2f(qp_b *b, uint32_t x) { return qp_i1(b, 112, qp_t_f32(b), x); }
static uint32_t qp_ult(qp_b *b, uint32_t x, uint32_t y) { return qp_i2(b, 176, qp_t_bool(b), x, y); }
static uint32_t qp_uge(qp_b *b, uint32_t x, uint32_t y) { return qp_i2(b, 174, qp_t_bool(b), x, y); }
static uint32_t qp_land(qp_b *b, uint32_t x, uint32_t y) { return qp_i2(b, 167, qp_t_bool(b), x, y); }
static uint32_t qp_extract(qp_b *b, uint32_t ty, uint32_t v, uint32_t idx) { uint32_t r = qp_nid(b); QP_EMIT(&b->body, 81, ty, r, v, idx); return r; }
static uint32_t qp_load(qp_b *b, uint32_t ty, uint32_t ptr) { return qp_i1(b, 61, ty, ptr); }
static uint32_t qp_v4(qp_b *b, uint32_t x, uint32_t y, uint32_t z, uint32_t w) { uint32_t r = qp_nid(b); QP_EMIT(&b->body, 80, qp_t_v4f(b), r, x, y, z, w); return r; }

typedef struct { uint32_t then_l, merge_l; } qp_if;
static qp_if qp_if_begin(qp_b *b, uint32_t cond) {
    qp_if c; c.then_l = qp_nid(b); c.merge_l = qp_nid(b);
    QP_EMIT(&b->body, 247, c.merge_l, 0);
    QP_EMIT(&b->body, 250, cond, c.then_l, c.merge_l);
    QP_EMIT(&b->body, 248, c.then_l);
    return c;
}
static void qp_if_end(qp_b *b, qp_if c) { QP_EMIT(&b->body, 249, c.merge_l); QP_EMIT(&b->body, 248, c.merge_l); }

static uint32_t qp_global_var(qp_b *b, uint32_t sc, uint32_t ty) {
    uint32_t pt = qp_t_ptr(b, sc, ty), id = qp_nid(b);
    QP_EMIT(&b->types, 59, pt, id, sc);
    return id;
}
static uint32_t qp_builtin_var(qp_b *b, uint32_t builtin, uint32_t ty) {
    uint32_t id = qp_global_var(b, 1 /*Input*/, ty);
    QP_EMIT(&b->annot, 71, id, 11 /*BuiltIn*/, builtin);
    b->iface[b->niface++] = id;
    return id;
}

/* module finish: header + sections -> malloc'd words */
static uint32_t *qp_finish(qp_b *b, uint32_t *nwords) {
    uint32_t n = 5 + b->caps.n + b->exts.n + b->mem.n + b->entry.n + b->modes.n + b->annot.n + b->types.n + b->body.n;
    uint32_t *o = (uint32_t *)malloc(n * 4), p = 0;
    o[p++] = 0x07230203u; o[p++] = 0x00010300u; o[p++] = 0; o[p++] = b->next_id; o[p++] = 0;
    qp_wb *s[] = { &b->caps, &b->exts, &b->mem, &b->entry, &b->modes, &b->annot, &b->types, &b->body };
    for (int i = 0; i < 8; i++) { memcpy(o + p, s[i]->w, s[i]->n * 4); p += s[i]->n; free(s[i]->w); }
    *nwords = n;
    return o;
}

static void qp_entry(qp_b *b, uint32_t model, uint32_t fn) {
    uint32_t a[16], p = 0;
    a[p++] = model; a[p++] = fn; p += qp_str_words("main", a + p);
    for (int i = 0; i < b->niface; i++) a[p++] = b->iface[i];
    qp_ins(&b->entry, 15, p, a);
}

static void qp_head(qp_b *b) {
    b->next_id = 1;
    QP_EMIT(&b->mem, 14, 0 /*Logical*/, 1 /*GLSL450*/);
}

/* ---- compute kernels ---- */
static uint32_t *qp_gen_kernel(const qp_kspec *k, int twin, uint32_t *nwords) {
    qp_b B; memset(&B, 0, sizeof B); qp_b *b = &B; qp_head(b);
    const int T = k->tile;            /* the kernel addresses a tile attachment */
    const int R = T && !twin;         /* real tile-shading tokens */
    const int simd = (k->kind == QK_SIMD);

    QP_EMIT(&b->caps, 17, 1 /*Shader*/);
    if (R) { QP_EMIT(&b->caps, 17, QP_CAP_TILE_SHADING); }
    if (k->ifmt == QP_IF_UNKNOWN) { QP_EMIT(&b->caps, 17, 56 /*StorageImageWriteWithoutFormat*/); if (k->kind == QK_RMW) QP_EMIT(&b->caps, 17, 55 /*ReadWithoutFormat*/); }
    if (k->ifmt == QP_IF_RGB10A2) { QP_EMIT(&b->caps, 17, 49 /*StorageImageExtendedFormats*/); }
    if (simd) { QP_EMIT(&b->caps, 17, 61 /*GroupNonUniform*/); QP_EMIT(&b->caps, 17, 65 /*GroupNonUniformShuffle*/); }
    if (R) { uint32_t w[8]; uint32_t nw = qp_str_words("SPV_QCOM_tile_shading", w); qp_ins(&b->exts, 10, nw, w); }

    uint32_t tv = qp_t_void(b), tf = qp_t_f32(b), tu = qp_t_u32(b), tb = qp_t_bool(b), tv2 = qp_t_v2u(b), tv3 = qp_t_v3u(b);
    (void)tb;
    /* image */
    uint32_t timg = qp_nid(b);
    QP_EMIT(&b->types, 25, timg, tf, 1 /*2D*/, 0, (uint32_t)(k->arrayed ? 1 : 0), 0, 2 /*storage*/, (uint32_t)k->ifmt);
    uint32_t vimg = qp_global_var(b, R ? QP_SC_TILE_ATTACHMENT : 0 /*UniformConstant*/, timg);
    QP_EMIT(&b->annot, 71, vimg, 34 /*DescriptorSet*/, 0);
    QP_EMIT(&b->annot, 71, vimg, 33 /*Binding*/, 0);
    /* push constants: struct of 8 uints */
    uint32_t tpc = qp_nid(b);
    QP_EMIT(&b->types, 30, tpc, tu, tu, tu, tu, tu, tu, tu, tu);
    QP_EMIT(&b->annot, 71, tpc, 2 /*Block*/);
    for (uint32_t i = 0; i < 8; i++) QP_EMIT(&b->annot, 72, tpc, i, 35 /*Offset*/, i * 4);
    uint32_t vpc = qp_global_var(b, 9 /*PushConstant*/, tpc);
    uint32_t pc_u = qp_t_ptr(b, 9, tu);

    uint32_t vlid = 0, vwid = 0, vgid = 0, vtoff = 0, vtdim = 0, vlane = 0, vsgs = 0;
    int need_gid = (!T) || (T && k->rate_x);
    int need_lw = T && !k->rate_x;
    if (simd) need_lw = 1, need_gid = !T;
    if (need_lw) { vlid = qp_builtin_var(b, 27, tv3); vwid = qp_builtin_var(b, 26, tv3); }
    if (need_gid && !(simd && T)) vgid = qp_builtin_var(b, 28, tv3);
    if (T) {
        if (R) vtoff = qp_builtin_var(b, QP_BI_TILE_OFFSET, tv2);
        else vtoff = vwid ? vwid : qp_builtin_var(b, 26 /*twin: WorkgroupId (shared when the kernel already reads it)*/, tv3);
        vtdim = qp_builtin_var(b, R ? QP_BI_TILE_DIMENSION : 24 /*twin: NumWorkgroups*/, tv3);
    }
    if (simd) { vlane = qp_builtin_var(b, 41, tu); vsgs = qp_builtin_var(b, 36, tu); }

    uint32_t fnty = qp_nid(b); QP_EMIT(&b->types, 33, fnty, tv);
    uint32_t fn = qp_nid(b), lab = qp_nid(b);
    QP_EMIT(&b->body, 54, tv, fn, 0, fnty);
    QP_EMIT(&b->body, 248, lab);

    uint32_t pcv[8];
    for (uint32_t i = 0; i < 8; i++) { uint32_t ptr = qp_i2(b, 65, pc_u, vpc, qp_cu(b, i)); pcv[i] = qp_load(b, tu, ptr); }
    uint32_t img = qp_load(b, timg, vimg);

    uint32_t ox = 0, oy = 0, dx = 0, dy = 0, tx1 = 0, ty1 = 0;
    if (T) {
        uint32_t off = qp_load(b, R ? tv2 : tv3, vtoff), dim = qp_load(b, tv3, vtdim);
        ox = qp_extract(b, tu, off, 0); oy = qp_extract(b, tu, off, 1);
        dx = qp_extract(b, tu, dim, 0); dy = qp_extract(b, tu, dim, 1);
        tx1 = qp_uadd(b, ox, dx); ty1 = qp_uadd(b, oy, dy);
    }

    /* pixel list */
    uint32_t qxs[16], qys[16]; int npx = 0;
    uint32_t simd_x = 0, simd_y = 0;
    if (simd) {
        if (T) {
            uint32_t lid = qp_load(b, tv3, vlid), wid = qp_load(b, tv3, vwid);
            uint32_t lx = qp_extract(b, tu, lid, 0), wx = qp_extract(b, tu, wid, 0), wy = qp_extract(b, tu, wid, 1);
            simd_x = qp_uadd(b, qp_uadd(b, ox, qp_umul(b, wx, qp_cu(b, 64))), lx);
            simd_y = qp_uadd(b, oy, wy);
        } else {
            uint32_t gid = qp_load(b, tv3, vgid);
            simd_x = qp_extract(b, tu, gid, 0); simd_y = qp_extract(b, tu, gid, 1);
        }
        qxs[0] = simd_x; qys[0] = simd_y; npx = 1;
    } else if (T && k->rate_x) {
        uint32_t gid = qp_load(b, tv3, vgid);
        uint32_t gx = qp_extract(b, tu, gid, 0), gy = qp_extract(b, tu, gid, 1);
        uint32_t bx = qp_uadd(b, ox, qp_umul(b, gx, qp_cu(b, k->rate_x))), by = qp_uadd(b, oy, qp_umul(b, gy, qp_cu(b, k->rate_y)));
        for (uint32_t yy = 0; yy < k->rate_y; yy++) for (uint32_t xx = 0; xx < k->rate_x && npx < 16; xx++) {
            qxs[npx] = xx ? qp_uadd(b, bx, qp_cu(b, xx)) : bx; qys[npx] = yy ? qp_uadd(b, by, qp_cu(b, yy)) : by; npx++;
        }
    } else if (T) {
        uint32_t lid = qp_load(b, tv3, vlid), wid = qp_load(b, tv3, vwid);
        uint32_t lx = qp_extract(b, tu, lid, 0), ly = qp_extract(b, tu, lid, 1), wx = qp_extract(b, tu, wid, 0), wy = qp_extract(b, tu, wid, 1);
        qxs[0] = qp_uadd(b, qp_uadd(b, ox, qp_umul(b, wx, qp_cu(b, k->ls_x))), lx);
        qys[0] = qp_uadd(b, qp_uadd(b, oy, qp_umul(b, wy, qp_cu(b, k->ls_y))), ly);
        npx = 1;
    } else {
        uint32_t gid = qp_load(b, tv3, vgid);
        qxs[0] = qp_extract(b, tu, gid, 0); qys[0] = qp_extract(b, tu, gid, 1); npx = 1;
    }

    /* SIMD kernel: the group operations run before (outside) the guard, so every lane takes part even if its pixel is clipped */
    uint32_t sv_sum = 0, sv_up = 0, sv_lane = 0, sv_sgs = 0;
    if (simd) {
        uint32_t t1 = qp_umul(b, simd_x, qp_cu(b, 3)), t2 = qp_umul(b, simd_y, qp_cu(b, 5));
        uint32_t m = qp_umod(b, qp_uadd(b, t1, t2), qp_cu(b, 61));
        uint32_t v0 = qp_u2f(b, qp_uadd(b, m, qp_cu(b, 1)));
        uint32_t v = v0, scope = qp_cu(b, 3 /*Subgroup*/);
        for (uint32_t d = k->logical / 2; d >= 1; d /= 2) {
            uint32_t sh = qp_i3(b, 346 /*GroupNonUniformShuffleXor*/, tf, scope, v, qp_cu(b, d));
            v = qp_fadd(b, v, sh);
        }
        sv_sum = v;
        sv_lane = qp_load(b, tu, vlane); sv_sgs = qp_load(b, tu, vsgs);
        uint32_t lm = qp_uand(b, sv_lane, qp_cu(b, k->logical - 1));
        uint32_t isz = qp_i2(b, 170 /*IEqual*/, qp_t_bool(b), lm, qp_cu(b, 0));
        uint32_t lm1 = qp_usub(b, sv_lane, qp_cu(b, 1));
        uint32_t tgt = qp_i3(b, 169 /*Select*/, tu, isz, sv_lane, lm1);
        sv_up = qp_i3(b, 345 /*GroupNonUniformShuffle*/, tf, scope, v0, tgt);
    }

    for (int pi = 0; pi < npx; pi++) {
        uint32_t qx = qxs[pi], qy = qys[pi];
        uint32_t c = qp_ult(b, qx, pcv[2]);
        c = qp_land(b, c, qp_ult(b, qy, pcv[3]));
        c = qp_land(b, c, qp_uge(b, qx, pcv[0]));
        c = qp_land(b, c, qp_uge(b, qy, pcv[1]));
        c = qp_land(b, c, qp_uge(b, qx, pcv[4]));
        c = qp_land(b, c, qp_uge(b, qy, pcv[5]));
        c = qp_land(b, c, qp_ult(b, qx, pcv[6]));
        c = qp_land(b, c, qp_ult(b, qy, pcv[7]));
        if (T) { c = qp_land(b, c, qp_ult(b, qx, tx1)); c = qp_land(b, c, qp_ult(b, qy, ty1)); }
        qp_if g = qp_if_begin(b, c);
        uint32_t sx = qx, sy = qy;
        if (R && k->rel) { sx = qp_usub(b, qx, ox); sy = qp_usub(b, qy, oy); }
        uint32_t coord;
        if (k->arrayed) { coord = qp_nid(b); QP_EMIT(&b->body, 80, tv3, coord, sx, sy, qp_cu(b, 0)); }
        else { coord = qp_nid(b); QP_EMIT(&b->body, 80, tv2, coord, sx, sy); }
        uint32_t val;
        if (k->kind == QK_DUMP) {
            val = qp_v4(b, qp_u2f(b, ox), qp_u2f(b, oy), qp_u2f(b, dx), qp_u2f(b, dy));
        } else if (k->kind == QK_PATTERN) {
            float sc = (k->pat_div == 16) ? 0.0625f : (1.0f / 15.0f);
            uint32_t n1 = qp_uand(b, qx, qp_cu(b, 15)), n2 = qp_uand(b, qy, qp_cu(b, 15)), n3 = qp_uand(b, qp_ushr(b, qx, qp_cu(b, 4)), qp_cu(b, 15));
            uint32_t s = qp_cf(b, sc);
            val = qp_v4(b, qp_fmul(b, qp_u2f(b, n1), s), qp_fmul(b, qp_u2f(b, n2), s), qp_fmul(b, qp_u2f(b, n3), s), qp_cf(b, 1.0f));
        } else if (k->kind == QK_RMW) {
            uint32_t in = qp_nid(b); QP_EMIT(&b->body, 98, qp_t_v4f(b), in, img, coord);
            uint32_t r = qp_extract(b, tf, in, 0), gch = qp_extract(b, tf, in, 1), bch = qp_extract(b, tf, in, 2), a = qp_extract(b, tf, in, 3);
            uint32_t o0 = qp_fadd(b, qp_fmul(b, gch, qp_cf(b, 0.5f)), qp_cf(b, 0.125f));
            uint32_t o1 = qp_fadd(b, qp_fmul(b, r, qp_cf(b, 0.5f)), qp_cf(b, 0.25f));
            val = qp_v4(b, o0, o1, bch, a);
        } else { /* QK_SIMD */
            val = qp_v4(b, sv_sum, sv_up, qp_u2f(b, sv_lane), qp_u2f(b, sv_sgs));
        }
        QP_EMIT(&b->body, 99, img, coord, val);
        qp_if_end(b, g);
    }
    qp_ins(&b->body, 253, 0, NULL);
    qp_ins(&b->body, 56, 0, NULL);

    qp_entry(b, 5 /*GLCompute*/, fn);
    if (k->ls_x) QP_EMIT(&b->modes, 16, fn, 17 /*LocalSize*/, k->ls_x, k->ls_y, 1);
    else if (twin) QP_EMIT(&b->modes, 16, fn, 17, 8, 8, 1);   /* twin only: the validator wants a LocalSize; the real no-LocalSize module is the [N] case of E5 V4 */
    if (R && k->rate_x) QP_EMIT(&b->modes, 16, fn, QP_EM_TILE_SHADING_RATE, k->rate_x, k->rate_y, 1);
    return qp_finish(b, nwords);
}

/* ---- graphics: fullscreen triangle + a coordinate pattern fragment shader (push constant: float blue) ---- */
static uint32_t *qp_gen_vert(uint32_t *nwords) {
    qp_b B; memset(&B, 0, sizeof B); qp_b *b = &B; qp_head(b);
    QP_EMIT(&b->caps, 17, 1);
    uint32_t tv = qp_t_void(b), tf = qp_t_f32(b), ti = qp_t_i32(b), tv4 = qp_t_v4f(b);
    uint32_t vidx = qp_builtin_var(b, 42, ti);
    uint32_t vpos = qp_global_var(b, 3 /*Output*/, tv4);
    QP_EMIT(&b->annot, 71, vpos, 11, 0 /*Position*/);
    b->iface[b->niface++] = vpos;
    uint32_t fnty = qp_nid(b); QP_EMIT(&b->types, 33, fnty, tv);
    uint32_t fn = qp_nid(b), lab = qp_nid(b);
    QP_EMIT(&b->body, 54, tv, fn, 0, fnty);
    QP_EMIT(&b->body, 248, lab);
    uint32_t idx = qp_load(b, ti, vidx);
    uint32_t a = qp_i2(b, 199, ti, qp_i2(b, 196 /*ShiftLeftLogical*/, ti, idx, qp_ci(b, 1)), qp_ci(b, 2));
    uint32_t c = qp_i2(b, 199, ti, idx, qp_ci(b, 2));
    uint32_t fa = qp_i1(b, 111 /*ConvertSToF*/, tf, a), fc = qp_i1(b, 111, tf, c);
    uint32_t x = qp_fadd(b, qp_fmul(b, fa, qp_cf(b, 2.0f)), qp_cf(b, -1.0f));
    uint32_t y = qp_fadd(b, qp_fmul(b, fc, qp_cf(b, 2.0f)), qp_cf(b, -1.0f));
    uint32_t pos = qp_v4(b, x, y, qp_cf(b, 0.0f), qp_cf(b, 1.0f));
    QP_EMIT(&b->body, 62, vpos, pos);
    qp_ins(&b->body, 253, 0, NULL); qp_ins(&b->body, 56, 0, NULL);
    qp_entry(b, 0 /*Vertex*/, fn);
    return qp_finish(b, nwords);
}

static uint32_t *qp_gen_frag(uint32_t *nwords) {
    qp_b B; memset(&B, 0, sizeof B); qp_b *b = &B; qp_head(b);
    QP_EMIT(&b->caps, 17, 1);
    uint32_t tv = qp_t_void(b), tf = qp_t_f32(b), tu = qp_t_u32(b), tv4 = qp_t_v4f(b);
    uint32_t vfc = qp_builtin_var(b, 15 /*FragCoord*/, tv4);
    uint32_t vout = qp_global_var(b, 3, tv4);
    QP_EMIT(&b->annot, 71, vout, 30 /*Location*/, 0);
    b->iface[b->niface++] = vout;
    uint32_t tpc = qp_nid(b); QP_EMIT(&b->types, 30, tpc, tf);
    QP_EMIT(&b->annot, 71, tpc, 2); QP_EMIT(&b->annot, 72, tpc, 0, 35, 0);
    uint32_t vpc = qp_global_var(b, 9, tpc), pcf = qp_t_ptr(b, 9, tf);
    uint32_t fnty = qp_nid(b); QP_EMIT(&b->types, 33, fnty, tv);
    uint32_t fn = qp_nid(b), lab = qp_nid(b);
    QP_EMIT(&b->body, 54, tv, fn, 0, fnty);
    QP_EMIT(&b->body, 248, lab);
    uint32_t fc = qp_load(b, tv4, vfc);
    uint32_t ux = qp_i1(b, 109 /*ConvertFToU*/, tu, qp_extract(b, tf, fc, 0)), uy = qp_i1(b, 109, tu, qp_extract(b, tf, fc, 1));
    uint32_t s = qp_cf(b, 0.0625f);
    uint32_t r = qp_fmul(b, qp_u2f(b, qp_uand(b, ux, qp_cu(b, 15))), s), g = qp_fmul(b, qp_u2f(b, qp_uand(b, uy, qp_cu(b, 15))), s);
    uint32_t pb = qp_load(b, tf, qp_i2(b, 65, pcf, vpc, qp_cu(b, 0)));
    uint32_t o = qp_v4(b, r, g, pb, qp_cf(b, 1.0f));
    QP_EMIT(&b->body, 62, vout, o);
    qp_ins(&b->body, 253, 0, NULL); qp_ins(&b->body, 56, 0, NULL);
    qp_entry(b, 4 /*Fragment*/, fn);
    QP_EMIT(&b->modes, 16, fn, 7 /*OriginUpperLeft*/);
    return qp_finish(b, nwords);
}

#endif
