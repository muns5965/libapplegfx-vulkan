/*
 * libapplegfx-vulkan — write rendered pixels into a guest display surface. See surface_writeback.h.
 * SPDX-License-Identifier: MIT
 */
#include "surface_writeback.h"
#include "../common/le.h"
#include "../common/log.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static lagfx_status_t decode_impl(const uint8_t *desc, size_t len, lagfx_guest_surface_t *out, bool allow_rgha) {
    if (!desc || !out || len < LAGFX_SURF_DESC_BYTES) return LAGFX_ERR_INVALID_ARG;
    uint32_t list = lagfx_le32(desc), fourcc = lagfx_le32(desc + 4), first = lagfx_le32(desc + 0x60);
    uint64_t alloc = lagfx_le64(desc + 0x18), sw = lagfx_le64(desc + 0x20), sh = lagfx_le64(desc + 0x28), row = lagfx_le64(desc + 0x30);
    uint32_t bpp = fourcc == LAGFX_FOURCC_BGRA ? 4u : (allow_rgha && fourcc == LAGFX_FOURCC_RGHA) ? 8u : 0u;
    if (!bpp) { LAGFX_WARN("guest surface: fourcc %08x refused (%s)", fourcc, allow_rgha ? "only BGRA / RGhA" : "only BGRA"); return LAGFX_ERR_INVALID_ARG; }
    if (!(list & 1u) || !sw || !sh || sw > 8192 || sh > 8192 || row < sw * bpp || row > 0x10000 || alloc > 64ull * 1024 * 1024) return LAGFX_ERR_INVALID_ARG;
    uint64_t length = (alloc + first + 0x3fffull) & ~0x3fffull, npages = length >> LAGFX_SURF_PAGE_SHIFT;
    if (!npages || npages > 4096) return LAGFX_ERR_INVALID_ARG;
    if ((uint64_t)first + row * (sh - 1) + sw * bpp > (npages << LAGFX_SURF_PAGE_SHIFT)) return LAGFX_ERR_INVALID_ARG;
    out->fourcc = fourcc; out->width = (uint32_t)sw; out->height = (uint32_t)sh; out->first_offset = first; out->npages = (uint32_t)npages; out->bpp = bpp;
    out->row_bytes = row; out->alloc = alloc; out->page_list_gpa = (uint64_t)(list & ~1u) << 12;
    return LAGFX_OK;
}
lagfx_status_t lagfx_guest_surface_decode(const uint8_t *desc, size_t len, lagfx_guest_surface_t *out) { return decode_impl(desc, len, out, false); }
lagfx_status_t lagfx_guest_surface_decode_any(const uint8_t *desc, size_t len, lagfx_guest_surface_t *out) { return decode_impl(desc, len, out, true); }

lagfx_status_t lagfx_guest_surface_write_bgra(const lagfx_shell_callbacks_t *shell, const lagfx_guest_surface_t *s,
                                              const uint8_t *src, uint32_t w, uint32_t h, size_t src_stride) {
    if (!shell || !shell->read_memory || !shell->write_memory || !s || !src || !w || !h || src_stride < (size_t)w * 4u) return LAGFX_ERR_INVALID_ARG;
    if (w > s->width) w = s->width;
    if (h > s->height) h = s->height;
    uint32_t *pages = malloc((size_t)s->npages * 4u);
    if (!pages) return LAGFX_ERR_OUT_OF_MEMORY;
    lagfx_status_t st = LAGFX_ERR_BACKEND;
    if (!shell->read_memory(shell->opaque, s->page_list_gpa, (uint64_t)s->npages * 4u, pages)) goto out;
    /* check every page the pixels touch before writing anything */
    uint64_t last = (uint64_t)s->first_offset + s->row_bytes * (h - 1u) + (uint64_t)w * 4u - 1u;
    for (uint64_t pg = s->first_offset >> LAGFX_SURF_PAGE_SHIFT; pg <= (last >> LAGFX_SURF_PAGE_SHIFT); pg++)
        if (pg >= s->npages || !(lagfx_le32((const uint8_t *)&pages[pg]) & 1u)) { LAGFX_WARN("guest surface: page %llu missing; nothing written", (unsigned long long)pg); goto out; }
    for (uint32_t y = 0; y < h; y++) {
        uint64_t off = (uint64_t)s->first_offset + (uint64_t)y * s->row_bytes, n = (uint64_t)w * 4u;
        const uint8_t *p = src + (size_t)y * src_stride;
        while (n) {
            uint64_t pg = off >> LAGFX_SURF_PAGE_SHIFT, o = off & 0x3fffu, c = 0x4000u - o;
            if (c > n) c = n;
            uint64_t gpa = ((uint64_t)(lagfx_le32((const uint8_t *)&pages[pg]) & ~1u) << 12) + o;
            if (!shell->write_memory(shell->opaque, gpa, c, p)) { LAGFX_WARN("guest surface: write at 0x%llx failed (row %u)", (unsigned long long)gpa, y); st = LAGFX_ERR_BACKEND; goto out; }
            p += c; off += c; n -= c;
        }
    }
    st = LAGFX_OK;
out:
    free(pages);
    return st;
}

lagfx_status_t lagfx_guest_surface_xfer(const lagfx_shell_callbacks_t *shell, const lagfx_guest_surface_t *s, uint8_t *buf, uint32_t w, uint32_t h, size_t stride,
                                        bool to_guest, lagfx_guest_surface_xfer_stats_t *st) {
    lagfx_guest_surface_xfer_stats_t zero = { 0, 0, 0 };
    if (!st) st = &zero;
    if (!shell || !shell->read_memory || (to_guest && !shell->write_memory) || !s || !buf || !w || !h || !s->npages || s->npages > 4096u || (s->bpp != 4u && s->bpp != 8u) || stride < (size_t)w * s->bpp) return LAGFX_ERR_INVALID_ARG;
    if (w > s->width) w = s->width;
    if (h > s->height) h = s->height;
    uint32_t *pages = malloc((size_t)s->npages * 4u);
    uint8_t *bad = calloc(s->npages, 1);   /* pages already counted as skipped */
    if (!pages || !bad) { free(pages); free(bad); return LAGFX_ERR_OUT_OF_MEMORY; }
    lagfx_status_t res = LAGFX_ERR_BACKEND;
    if (!shell->read_memory(shell->opaque, s->page_list_gpa, (uint64_t)s->npages * 4u, pages)) goto out;
    const uint64_t total = (uint64_t)s->npages << LAGFX_SURF_PAGE_SHIFT;
    for (uint32_t y = 0; y < h; y++) {
        uint64_t off = (uint64_t)s->first_offset + (uint64_t)y * s->row_bytes, n = (uint64_t)w * s->bpp;
        uint8_t *p = buf + (size_t)y * stride;
        while (n) {
            uint64_t pg = off >> LAGFX_SURF_PAGE_SHIFT, o = off & 0x3fffu, c = 0x4000u - o;
            if (c > n) c = n;
            bool ok = off < total && off + c <= total && pg < s->npages;
            uint32_t ent = ok ? lagfx_le32((const uint8_t *)&pages[pg]) : 0;
            ok = ok && (ent & 1u);
            if (ok) {
                uint64_t gpa = ((uint64_t)(ent & ~1u) << 12) + o;
                ok = to_guest ? shell->write_memory(shell->opaque, gpa, c, p) : shell->read_memory(shell->opaque, gpa, c, p);
            }
            if (ok) st->bytes += c;
            else {
                if (!to_guest) memset(p, 0, (size_t)c);
                st->chunks_skipped++;
                if (pg < s->npages && !bad[pg]) bad[pg] = 1, st->pages_skipped++;
                else if (pg >= s->npages) st->pages_skipped++;
            }
            p += c; off += c; n -= c;
        }
    }
    res = LAGFX_OK;
out:
    free(pages); free(bad);
    return res;
}

static uint64_t wbv_hash(const uint8_t *b, uint64_t px, uint32_t bpp) {
    uint64_t h = 1469598103934665603ull;
    if (bpp == 8u) { for (uint64_t i = 0; i < px; i++) { uint64_t v = lagfx_le64(b + i * 8u); h = (h ^ v) * 1099511628211ull; } return h; }   /* RGhA: one step per whole LE u64 pixel (NOT comparable with Henge's pvgpix, which hashes converted BGRA) */
    for (uint64_t i = 0; i < px; i++) { uint32_t v = lagfx_le32(b + i * 4u); h = (h ^ v) * 1099511628211ull; }
    return h;
}
/* binary16 class of one channel: exponent 31 = Inf/NaN; sign bit with a non-zero magnitude = negative (-0.0 is not counted); above one = magnitude > 0x3c00 and finite. */
static bool half_nonfinite(uint16_t h) { return (h & 0x7c00u) == 0x7c00u; }
static bool half_negative(uint16_t h) { return (h & 0x8000u) && (h & 0x7fffu); }
static bool half_above_one(uint16_t h) { return !(h & 0x8000u) && (h & 0x7fffu) > 0x3c00u && !half_nonfinite(h); }
lagfx_status_t lagfx_guest_surface_verify(const lagfx_shell_callbacks_t *shell, const lagfx_guest_surface_t *s, const uint8_t *buf, uint32_t w, uint32_t h, size_t stride, lagfx_wbv_result_t *r) {
    if (!shell || !s || !buf || !r || !w || !h || (s->bpp != 4u && s->bpp != 8u) || stride < (size_t)w * s->bpp) return LAGFX_ERR_INVALID_ARG;
    const uint32_t bpp = s->bpp;
    if (w > s->width) w = s->width;
    if (h > s->height) h = s->height;
    memset(r, 0, sizeof *r);
    r->w = w, r->h = h, r->bpp = bpp, r->first_x = r->first_y = -1, r->pixels = (uint64_t)w * h;
    uint8_t *g = malloc((size_t)w * h * bpp), *tight = malloc((size_t)w * h * bpp);
    if (!g || !tight) { free(g); free(tight); return LAGFX_ERR_OUT_OF_MEMORY; }
    lagfx_guest_surface_xfer_stats_t st = { 0, 0, 0 }, st2 = { 0, 0, 0 };
    lagfx_status_t res = lagfx_guest_surface_xfer(shell, s, g, w, h, (size_t)w * bpp, false, &st);
    if (res != LAGFX_OK) goto out;
    for (uint32_t y = 0; y < h; y++) memcpy(tight + (size_t)y * w * bpp, buf + (size_t)y * stride, (size_t)w * bpp);   /* valid pixels only: no row padding */
    r->hash_buf = wbv_hash(tight, r->pixels, bpp), r->hash_guest = wbv_hash(g, r->pixels, bpp), r->pages_skipped = st.pages_skipped;
    for (uint64_t i = 0; i < r->pixels; i++) {
        const uint8_t *a = tight + i * bpp, *b = g + i * bpp;
        if (bpp == 8u) {   /* RGhA: raw 16-bit channels r, g, b, a (LE); compared bit for bit, so NaN payloads and -0.0 count too */
            uint16_t c[4];
            for (int k = 0; k < 4; k++) c[k] = (uint16_t)(a[2 * k] | a[2 * k + 1] << 8);
            if (half_nonfinite(c[0]) || half_nonfinite(c[1]) || half_nonfinite(c[2]) || half_nonfinite(c[3])) r->half_nonfinite++;
            if (half_negative(c[0]) || half_negative(c[1]) || half_negative(c[2]) || half_negative(c[3])) r->half_negative++;
            if (half_above_one(c[0]) || half_above_one(c[1]) || half_above_one(c[2]) || half_above_one(c[3])) r->half_above_one++;
        }
        bool rgb = bpp == 8u ? memcmp(a, b, 6) != 0 : (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]);
        bool alpha = bpp == 8u ? (a[6] != b[6] || a[7] != b[7]) : a[3] != b[3];
        if (!rgb && !alpha) continue;
        if (rgb) r->rgb_mismatch++; else r->alpha_only_mismatch++;
        if (r->first_x < 0) r->first_x = (int32_t)(i % w), r->first_y = (int32_t)(i / w);
    }
    res = lagfx_guest_surface_xfer(shell, s, g, w, h, (size_t)w * bpp, false, &st2);   /* second read into the same buffer: is the guest side stable? */
    if (res != LAGFX_OK) goto out;
    r->hash_guest2 = wbv_hash(g, r->pixels, bpp), r->stable = r->hash_guest2 == r->hash_guest;
out:
    free(g); free(tight);
    return res;
}
bool lagfx_wbv_enabled(void) { const char *e = getenv("LAGFX_WB_VERIFY"); return e && e[0] == '1' && !e[1]; }
uint64_t lagfx_wbv_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
bool lagfx_wbv_admit(lagfx_wbv_gate_t *g, uint64_t now_ns) {
    if (!g || g->done >= LAGFX_WBV_MAX_COMPARES) return false;
    if (g->done && now_ns - g->last_ns < LAGFX_WBV_MIN_GAP_NS) return false;
    g->done++, g->last_ns = now_ns;
    return true;
}

#ifdef LAGFX_HAVE_VULKAN
lagfx_status_t lagfx_vk_render_target_to_guest_surface(struct lagfx_vk_state *vk, lagfx_vk_render_target_t *rt,
                                                       const lagfx_shell_callbacks_t *shell, const uint8_t *desc, size_t desc_len) {
    if (!vk || !rt || !shell) return LAGFX_ERR_INVALID_ARG;
    if (rt->format != VK_FORMAT_B8G8R8A8_UNORM && rt->format != VK_FORMAT_R8G8B8A8_UNORM) return LAGFX_ERR_INVALID_ARG;
    lagfx_guest_surface_t gs;
    lagfx_status_t st = lagfx_guest_surface_decode(desc, desc_len, &gs);
    if (st != LAGFX_OK) return st;
    size_t bytes = (size_t)rt->width * rt->height * 4u, stride = 0;
    uint8_t *px = malloc(bytes);
    if (!px) return LAGFX_ERR_OUT_OF_MEMORY;
    st = lagfx_vk_render_target_readback(vk, rt, px, bytes, &stride);
    if (st == LAGFX_OK) {
        if (rt->format == VK_FORMAT_R8G8B8A8_UNORM)
            for (size_t i = 0; i + 3 < bytes; i += 4) { uint8_t t = px[i]; px[i] = px[i + 2]; px[i + 2] = t; }
        if (rt->width != gs.width || rt->height != gs.height)
            LAGFX_WARN("guest surface: target %ux%u vs surface %ux%u: top-left overlap written, no scaling", rt->width, rt->height, gs.width, gs.height);
        st = lagfx_guest_surface_write_bgra(shell, &gs, px, rt->width, rt->height, stride);
    }
    free(px);
    return st;
}
#endif
