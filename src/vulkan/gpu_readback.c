/*
 * libapplegfx-vulkan — independent GPU readback of a mapped display surface (F-2 diagnostic). See gpu_readback.h.
 * SPDX-License-Identifier: MIT
 */
#include "gpu_readback.h"
#include "../common/log.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RB_LINE_CAP      60u              /* at most this many g7-gpu-readback lines per process */
#define RB_FIRST_N       4u               /* the first candidates of a class are always sampled */
#define RB_DEFAULT_EVERY 16u
#define RB_GAP_NS        30000000000ull   /* a candidate this long after the last sample is sampled too */
#define RB_POISON        0xA5u

int lagfx_rb_test_hook;
static lagfx_rb_totals_t g_tot;
static lagfx_rb_verdict_t g_last;
static lagfx_rb_gate_t g_gate[2];          /* [0] 4-byte surfaces, [1] 8-byte (RGhA): the big display surface has its own budget */

const lagfx_rb_verdict_t *lagfx_rb_last_verdict(void) { return &g_last; }
const lagfx_rb_totals_t *lagfx_rb_totals(void) { return &g_tot; }
void lagfx_rb_test_reset(void) { memset(&g_tot, 0, sizeof g_tot); memset(&g_last, 0, sizeof g_last); memset(g_gate, 0, sizeof g_gate); lagfx_rb_test_hook = 0; }

bool lagfx_rb_enabled(void) {
    const char *a = getenv("LAGFX_WB_VERIFY"), *b = getenv("LAGFX_GPU_READBACK");
    return (a && a[0] == '1' && !a[1]) || (b && b[0] == '1' && !b[1]);
}

bool lagfx_rb_gate_admit(lagfx_rb_gate_t *g, uint64_t now_ns, uint32_t every) {
    if (!g) return false;
    if (!every) every = RB_DEFAULT_EVERY;
    g->seen++;
    bool take = g->seen <= RB_FIRST_N || (g->seen % every) == 0 || (g->taken && now_ns - g->last_ns >= RB_GAP_NS);
    if (take) g->taken++, g->last_ns = now_ns;
    return take;
}

uint32_t lagfx_rb_format_bpp(int32_t f) {
    if (f == 97) return 8;                     /* VK_FORMAT_R16G16B16A16_SFLOAT */
    if (f >= 37 && f <= 50) return 4;          /* VK_FORMAT_R8G8B8A8_* (37..43) and B8G8R8A8_* (44..50) */
    return 0;
}

/* ---- pure compare ---- */

static bool img_ok(const lagfx_rb_img_t *i, uint32_t w, uint32_t h, uint32_t bpp) {
    if (!i->p) return false;
    size_t row = (size_t)w * bpp;
    if (i->stride < row) return false;
    return i->len >= (size_t)(h - 1u) * i->stride + row;
}

static uint64_t px_load(const uint8_t *p, uint32_t bpp) {
    if (bpp == 8u) { uint64_t v; memcpy(&v, p, 8); return v; }
    uint32_t v; memcpy(&v, p, 4); return v;
}

static uint64_t hash_img(const lagfx_rb_img_t *i, uint32_t w, uint32_t h, uint32_t bpp) {
    uint64_t hh = 1469598103934665603ull;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *r = i->p + (size_t)y * i->stride;
        for (uint32_t x = 0; x < w; x++) hh = (hh ^ px_load(r + (size_t)x * bpp, bpp)) * 1099511628211ull;
    }
    return hh;
}

/* pixels that differ between two images; first differing pixel in *fx, *fy (left at -1 if none) */
static uint64_t diff_img(const lagfx_rb_img_t *a, const lagfx_rb_img_t *b, uint32_t w, uint32_t h, uint32_t bpp, int32_t *fx, int32_t *fy) {
    uint64_t n = 0;
    size_t row = (size_t)w * bpp;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *ra = a->p + (size_t)y * a->stride, *rb = b->p + (size_t)y * b->stride;
        if (!memcmp(ra, rb, row)) continue;
        for (uint32_t x = 0; x < w; x++) {
            if (memcmp(ra + (size_t)x * bpp, rb + (size_t)x * bpp, bpp)) {
                if (!n && fx && fy) *fx = (int32_t)x, *fy = (int32_t)y;
                n++;
            }
        }
    }
    return n;
}

static void classify_half(const lagfx_rb_img_t *a, uint32_t w, uint32_t h, lagfx_rb_cmp_t *o) {
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *r = a->p + (size_t)y * a->stride;
        for (uint32_t x = 0; x < w; x++) {
            unsigned nan = 0, inf = 0, neg = 0, gt1 = 0;
            for (unsigned c = 0; c < 4; c++) {
                uint16_t v; memcpy(&v, r + (size_t)x * 8u + c * 2u, 2);
                uint16_t mag = (uint16_t)(v & 0x7fffu);
                if (mag > 0x7c00u) nan = 1;
                else if (mag == 0x7c00u) inf = 1;
                else if (v & 0x8000u) { if (mag) neg = 1; }
                else if (mag > 0x3c00u) gt1 = 1;
            }
            o->nan_px += nan, o->inf_px += inf, o->neg_px += neg, o->gt1_px += gt1;
        }
    }
}

lagfx_rb_err_t lagfx_rb_compare(const lagfx_rb_img_t *a, const lagfx_rb_img_t *b, const lagfx_rb_img_t *c, uint32_t w, uint32_t h, uint32_t bpp, lagfx_rb_cmp_t *out) {
    if (!out) return LAGFX_RB_ERR_ARG;
    memset(out, 0, sizeof *out);
    out->ab_x = out->ab_y = out->bc_x = out->bc_y = -1;
    if (!a || !b || !w || !h || (bpp != 4u && bpp != 8u)) return LAGFX_RB_ERR_ARG;
    out->w = w, out->h = h, out->bpp = bpp, out->bytes = (uint64_t)w * h * bpp;
    if (!img_ok(a, w, h, bpp)) out->bad_img |= 1u;
    if (!img_ok(b, w, h, bpp)) out->bad_img |= 2u;
    if (c && !img_ok(c, w, h, bpp)) out->bad_img |= 4u;
    if (out->bad_img) return LAGFX_RB_ERR_STRIDE;
    out->ab_px = diff_img(a, b, w, h, bpp, &out->ab_x, &out->ab_y);
    out->hash_a = hash_img(a, w, h, bpp);
    out->hash_b = hash_img(b, w, h, bpp);
    if (c) {
        out->bc_px = diff_img(b, c, w, h, bpp, &out->bc_x, &out->bc_y);
        out->ac_px = diff_img(a, c, w, h, bpp, NULL, NULL);
        out->hash_c = hash_img(c, w, h, bpp);
    }
    if (bpp == 8u) out->half_stats = true, classify_half(a, w, h, out);
    return LAGFX_RB_OK;
}

/* ---- verdict line ---- */

size_t lagfx_rb_format_line(char *buf, size_t cap, const lagfx_rb_verdict_t *v, const lagfx_rb_totals_t *t) {
    if (!buf || !cap || !v || !t) return 0;
    const char *k = v->kind == LAGFX_RB_PASS ? "PASS" : v->kind == LAGFX_RB_FAIL ? "FAIL" : "SKIP";
    const lagfx_rb_cmp_t *c = &v->cmp;
    int n = snprintf(buf, cap,
        "g7-gpu-readback: verdict=%s reason=%s swap=%" PRIu64 " swap_fin=%" PRIu64 " ms=%u gen=%" PRIu64 " %ux%u fourcc=%08x bpp=%u pitch=%" PRIu64 " bytes=%" PRIu64
        " A_vs_B_mismatch_px=%" PRIu64 " B_vs_C_mismatch_px=%" PRIu64 " A_vs_C_mismatch_px=%" PRIu64 " first_AB=%d,%d first_BC=%d,%d"
        " hash_A=%016" PRIx64 " hash_B=%016" PRIx64 " hash_C=%016" PRIx64 " hash_algo=%s"
        " nan_px=%" PRIu64 " inf_px=%" PRIu64 " neg_px=%" PRIu64 " gt1_px=%" PRIu64 " half_stats=%d"
        " pages_skipped=%" PRIu64 " C_stable=%d src=vkCmdCopyImageToBuffer(no blit/shader/format change) layout=GENERAL(no transition) coherent=%d"
        " totals[candidates=%" PRIu64 " sampled=%" PRIu64 " pass=%" PRIu64 " fail=%" PRIu64 " skip=%" PRIu64 " abandoned=%" PRIu64 " not_sampled=%" PRIu64 " invalidates=%" PRIu64 "]",
        k, v->skip ? v->skip : "-", v->swap, v->swap_fin, v->mapping, v->gen, v->w, v->h, v->fourcc, v->bpp, v->pitch, c->bytes,
        c->ab_px, c->bc_px, c->ac_px, c->ab_x, c->ab_y, c->bc_x, c->bc_y,
        c->hash_a, c->hash_b, c->hash_c, v->bpp == 8u ? "u64_fnv1a_full" : "u32_fnv1a_full",
        c->nan_px, c->inf_px, c->neg_px, c->gt1_px, c->half_stats ? 1 : 0,
        v->pages_skipped, v->c_stable ? 1 : 0, v->coherent ? 1 : 0,
        t->candidates, t->sampled, t->pass, t->fail, t->skip, t->abandoned, t->not_sampled, t->invalidates);
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

#ifdef LAGFX_HAVE_VULKAN
static uint32_t every_n(void) {
    const char *e = getenv("LAGFX_GPU_READBACK_EVERY");
    long n = e ? strtol(e, NULL, 10) : 0;
    return n > 0 ? (uint32_t)n : RB_DEFAULT_EVERY;
}

static void emit(lagfx_rb_verdict_t *v) {
    v->valid = true;
    g_tot.sampled++;
    if (v->kind == LAGFX_RB_PASS) g_tot.pass++; else if (v->kind == LAGFX_RB_FAIL) g_tot.fail++; else g_tot.skip++;
    g_tot.lines++;
    g_last = *v;
    char line[1536];
    lagfx_rb_format_line(line, sizeof line, v, &g_tot);
    LAGFX_WARN("%s", line);
}

struct lagfx_rb_slot {
    VkBuffer buf; VkDeviceMemory mem; uint8_t *map; VkDeviceSize size; bool coherent;
    bool pending;
    uint64_t swap, gen;
    uint32_t mapping, w, h, bpp;
};

static uint32_t rb_mem_type(struct lagfx_vk_state *vk, uint32_t bits, bool *coherent) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(vk->phys_device, &mp);
    static const VkMemoryPropertyFlags want[4] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT };
    for (unsigned t = 0; t < 4; t++)
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want[t]) == want[t]) { *coherent = (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0; return i; }
    return UINT32_MAX;
}

static bool slot_buf(struct lagfx_vk_state *vk, lagfx_rb_slot_t *s, VkDeviceSize size) {
    if (s->buf && s->size >= size) return true;
    if (s->buf) { vkDestroyBuffer(vk->device, s->buf, NULL); vkFreeMemory(vk->device, s->mem, NULL); s->buf = VK_NULL_HANDLE, s->mem = VK_NULL_HANDLE, s->map = NULL; }
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (vkCreateBuffer(vk->device, &bi, NULL, &s->buf) != VK_SUCCESS) { s->buf = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements rq;
    vkGetBufferMemoryRequirements(vk->device, s->buf, &rq);
    bool coh = false;
    uint32_t mt = rb_mem_type(vk, rq.memoryTypeBits, &coh);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = mt };
    void *mp = NULL;
    if (mt == UINT32_MAX || vkAllocateMemory(vk->device, &ai, NULL, &s->mem) != VK_SUCCESS) { s->mem = VK_NULL_HANDLE; goto bad; }
    if (vkBindBufferMemory(vk->device, s->buf, s->mem, 0) != VK_SUCCESS || vkMapMemory(vk->device, s->mem, 0, VK_WHOLE_SIZE, 0, &mp) != VK_SUCCESS || !mp) { vkFreeMemory(vk->device, s->mem, NULL); s->mem = VK_NULL_HANDLE; goto bad; }
    s->map = mp, s->size = size, s->coherent = coh;
    return true;
bad:
    vkDestroyBuffer(vk->device, s->buf, NULL);
    s->buf = VK_NULL_HANDLE;
    return false;
}

void lagfx_rb_slot_free(struct lagfx_vk_state *vk, lagfx_rb_slot_t **slot) {
    if (!slot || !*slot) return;
    lagfx_rb_slot_t *s = *slot;
    if (vk && s->buf) vkDestroyBuffer(vk->device, s->buf, NULL);
    if (vk && s->mem) vkFreeMemory(vk->device, s->mem, NULL);   /* freeing unmaps */
    free(s);
    *slot = NULL;
}

static void emit_skip(uint32_t mapping, uint32_t w, uint32_t h, uint32_t bpp, uint64_t gen, uint64_t swap, const char *why) {
    lagfx_rb_verdict_t v; memset(&v, 0, sizeof v);
    v.kind = LAGFX_RB_SKIP, v.skip = why, v.swap = swap, v.swap_fin = swap, v.gen = gen, v.mapping = mapping, v.w = w, v.h = h, v.bpp = bpp, v.fourcc = 0;
    v.cmp.ab_x = v.cmp.ab_y = v.cmp.bc_x = v.cmp.bc_y = -1;
    emit(&v);
}

void lagfx_rb_record(struct lagfx_vk_state *vk, lagfx_rb_slot_t **slot, VkCommandBuffer cb, VkImage img, VkFormat fmt, VkImageUsageFlags usage,
                     uint32_t w, uint32_t h, uint32_t bpp, uint32_t mapping, uint64_t gen, uint64_t swap) {
    if (!lagfx_rb_enabled() || !slot) return;
    g_tot.candidates++;
    if (*slot && (*slot)->pending) { (*slot)->pending = false; g_tot.abandoned++; }   /* recorded earlier, the finish never ran (failed exec, dropped write-back) */
    if (!vk || cb == VK_NULL_HANDLE || img == VK_NULL_HANDLE) return;
    if (g_tot.lines >= RB_LINE_CAP || !lagfx_rb_gate_admit(&g_gate[bpp == 8u], lagfx_wbv_now_ns(), every_n())) { g_tot.not_sampled++; return; }
    uint32_t fb = lagfx_rb_format_bpp((int32_t)fmt);
    const char *skip = NULL;
    if (!(usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) skip = "no_transfer_src";
    else if (!fb) skip = "unknown_format";
    else if (fb != bpp) skip = "bpp_format_mismatch";
    else if (!w || !h || (uint64_t)w * h * bpp > (256ull << 20)) skip = "bad_extent";
    if (skip) { emit_skip(mapping, w, h, bpp, gen, swap, skip); return; }
    if (!*slot) *slot = calloc(1, sizeof **slot);
    lagfx_rb_slot_t *s = *slot;
    if (!s || !slot_buf(vk, s, (VkDeviceSize)w * h * bpp)) { emit_skip(mapping, w, h, bpp, gen, swap, "alloc_failed"); return; }
    memset(s->map, RB_POISON, (size_t)w * h * bpp);   /* a copy that never ran cannot match anything real */
    /* The image is in GENERAL (the library never moves it) and the caller issued a full barrier before its own staging copy, which also covers this later read. */
    VkBufferImageCopy r = { .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
                            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageOffset = { 0, 0, 0 }, .imageExtent = { w, h, 1 } };
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, s->buf, 1, &r);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    s->pending = true, s->swap = swap, s->gen = gen, s->mapping = mapping, s->w = w, s->h = h, s->bpp = bpp;
}

static void flip_bit(uint8_t *p, size_t n) { if (n) p[n / 2u] ^= 1u; }

void lagfx_rb_finish(struct lagfx_vk_state *vk, lagfx_rb_slot_t **slot, const lagfx_shell_callbacks_t *shell, const lagfx_guest_surface_t *gs,
                     const uint8_t *stage, uint64_t gen, uint64_t swap_now) {
    if (!slot || !*slot) return;
    lagfx_rb_slot_t *s = *slot;
    if (!s->pending || s->gen != gen) return;
    s->pending = false;
    lagfx_rb_verdict_t v; memset(&v, 0, sizeof v);
    v.swap = s->swap, v.swap_fin = swap_now, v.gen = gen, v.mapping = s->mapping, v.w = s->w, v.h = s->h, v.bpp = s->bpp, v.fourcc = gs ? gs->fourcc : 0, v.coherent = s->coherent;
    v.pitch = (uint64_t)s->w * s->bpp;
    v.cmp.ab_x = v.cmp.ab_y = v.cmp.bc_x = v.cmp.bc_y = -1;
    size_t n = (size_t)s->w * s->h * s->bpp;
    uint8_t *tmp_a = NULL, *tmp_b = NULL, *tmp_c = NULL, *tmp_c2 = NULL;
    if (!vk || !shell || !gs || !stage) { v.kind = LAGFX_RB_SKIP, v.skip = "no_inputs"; goto out; }
    if (gs->width != s->w || gs->height != s->h || gs->bpp != s->bpp) { v.kind = LAGFX_RB_SKIP, v.skip = "desc_changed"; goto out; }
    if (!s->coherent || lagfx_rb_test_hook == 5) {
        VkMappedMemoryRange mr = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = s->mem, .offset = 0, .size = VK_WHOLE_SIZE };
        g_tot.invalidates++;
        if (vkInvalidateMappedMemoryRanges(vk->device, 1, &mr) != VK_SUCCESS) { v.kind = LAGFX_RB_SKIP, v.skip = "invalidate_failed"; goto out; }
    }
    tmp_c = malloc(n);
    if (!tmp_c) { v.kind = LAGFX_RB_SKIP, v.skip = "oom"; goto out; }
    lagfx_guest_surface_xfer_stats_t xs; memset(&xs, 0, sizeof xs);
    if (lagfx_guest_surface_xfer(shell, gs, tmp_c, s->w, s->h, (size_t)s->w * s->bpp, false, &xs) != LAGFX_OK) { v.kind = LAGFX_RB_SKIP, v.skip = "guest_read_failed"; goto out; }
    v.pages_skipped = xs.pages_skipped;
    lagfx_rb_img_t a = { s->map, (size_t)s->w * s->bpp, n }, b = { stage, (size_t)s->w * s->bpp, n }, c = { tmp_c, (size_t)s->w * s->bpp, n };
    if (lagfx_rb_test_hook == 1 || lagfx_rb_test_hook == 2 || lagfx_rb_test_hook == 4) {   /* negative controls act on copies, never on the real staging / guest bytes */
        tmp_a = malloc(n), tmp_b = malloc(n);
        if (!tmp_a || !tmp_b) { v.kind = LAGFX_RB_SKIP, v.skip = "oom"; goto out; }
        memcpy(tmp_a, s->map, n), memcpy(tmp_b, stage, n);
        if (lagfx_rb_test_hook == 1) flip_bit(tmp_b, n);
        if (lagfx_rb_test_hook == 2) flip_bit(tmp_a, n);
        if (lagfx_rb_test_hook == 4) flip_bit(tmp_c, n);
        a.p = tmp_a, b.p = tmp_b;
    }
    if (lagfx_rb_test_hook == 3) b.stride = (size_t)s->w * s->bpp + 8u;   /* a wrong stride for B: must be detected, not read past the buffer */
    lagfx_rb_err_t e = lagfx_rb_compare(&a, &b, &c, s->w, s->h, s->bpp, &v.cmp);
    if (e != LAGFX_RB_OK) { v.kind = LAGFX_RB_FAIL, v.skip = e == LAGFX_RB_ERR_STRIDE ? "stride" : "bad_args"; goto out; }
    v.c_stable = true;
    if (v.cmp.bc_px) {   /* B != C: was the guest writing meanwhile? read C once more */
        tmp_c2 = malloc(n);
        lagfx_guest_surface_xfer_stats_t xs2; memset(&xs2, 0, sizeof xs2);
        if (tmp_c2 && lagfx_guest_surface_xfer(shell, gs, tmp_c2, s->w, s->h, (size_t)s->w * s->bpp, false, &xs2) == LAGFX_OK) {
            lagfx_rb_img_t c2 = { tmp_c2, (size_t)s->w * s->bpp, n };
            uint64_t hh = 1469598103934665603ull;
            for (size_t i = 0; i + (s->bpp) <= n; i += s->bpp) { uint64_t px = 0; memcpy(&px, c2.p + i, s->bpp); hh = (hh ^ px) * 1099511628211ull; }
            v.c_stable = hh == v.cmp.hash_c;
        }
    }
    if (v.cmp.ab_px) v.kind = LAGFX_RB_FAIL, v.skip = "A!=B";
    else if (v.pages_skipped) v.kind = LAGFX_RB_SKIP, v.skip = "guest_pages_skipped";
    else if (v.cmp.bc_px) v.kind = LAGFX_RB_FAIL, v.skip = "B!=C";
    else v.kind = LAGFX_RB_PASS;
out:
    free(tmp_a), free(tmp_b), free(tmp_c), free(tmp_c2);
    emit(&v);
}
#endif
