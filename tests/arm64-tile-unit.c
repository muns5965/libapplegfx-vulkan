/* ARM64 tile shaders (tag 0x33 tile pipeline, binds 0x9d / 0xa1, dispatches 0x9b / 0xa2) executed on lavapipe (NOT verified on a hardware GPU).
 * The kernel is the real Apple tile_simd_blur_15 (tests/fixtures), wrapped in a synthetic MTLB and referenced by a synthetic serialized tile pipeline. The
 * synthetic command buffer: render pass over an RGBA16F target cleared to a sentinel, set tile pipeline, bind buffers + texture, dispatch. Checked pixel by
 * pixel against a CPU model of the kernel: whole-target dispatch, region dispatches (the second one proves the pass was suspended and resumed with LOAD ops: the
 * first one's pixels survive), the fail-closed paths (no 32-lane subgroup, unbound texture, wrong attachment format). SKIPs without fixtures / an ICD.
 * SPDX-License-Identifier: MIT */
#include "dispatchers/channel_compute_dispatcher.h"
#include "dispatchers/ring_common.h"
#include "protocol/state.h"
#include "protocol/arm64_exec_decode.h"
#include "protocol/arm64_exec_run.h"
#include "protocol/arm64_objects.h"
#include "protocol/arm64_pipeline.h"
#include "protocol/arm64_tile.h"
#include "protocol/arm64_completion.h"
#include "vulkan/caps.h"
#include "device.h"
#include "common/le.h"
#include "libapplegfx-vulkan.h"
#ifdef LAGFX_HAVE_VULKAN
#include "vulkan/instance.h"
#include <vulkan/vulkan.h>
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_SIZE   (2u << 20)
#define SHARED_PFN 1u
#define TABLE_PFN  2u
#define RING_PFN0  4u
#define ROOT_PFN   16u
#define TOP_PFN    17u
#define LEAF_PFN   18u
#define DATA_PFN0  32u
#define NVPN       24u

static void pfree(lagfx_protocol_t *p) { lagfx_arm64_cq_shutdown(p, 1000); free(p); }
static uint8_t *g_mem;
static bool rd(void *o, uint64_t gpa, uint64_t len, void *dst) { (void)o; if (gpa + len > MEM_SIZE) return false; memcpy(dst, g_mem + gpa, len); return true; }
static bool wr(void *o, uint64_t gpa, uint64_t len, const void *src) { (void)o; if (gpa + len > MEM_SIZE) return false; memcpy(g_mem + gpa, src, len); return true; }
static void irq(void *o, uint32_t v) { (void)o; (void)v; }
static lagfx_task_t *cb_create_task(void *o, uint64_t s, void **p) { (void)o; (void)s; (void)p; return NULL; }
static void cb_destroy_task(void *o, lagfx_task_t *t) { (void)o; (void)t; }
static bool cb_map(void *a, lagfx_task_t *t, uint64_t o, const lagfx_physical_range_t *r, size_t c, bool ro) { (void)a; (void)t; (void)o; (void)r; (void)c; (void)ro; return true; }
static bool cb_unmap(void *a, lagfx_task_t *t, uint64_t o, uint64_t l) { (void)a; (void)t; (void)o; (void)l; return true; }
static void put32(uint64_t gpa, uint32_t v) { memcpy(g_mem + gpa, &v, 4); }
static uint32_t g_wp;
static lagfx_device_t *g_dev;

static lagfx_device_t *make_device(void) {
    lagfx_device_descriptor_t d;
    memset(&d, 0, sizeof d);
    d.shell.opaque = (void *)0xfeedu;
    d.shell.create_task = cb_create_task; d.shell.destroy_task = cb_destroy_task; d.shell.map_memory = cb_map; d.shell.unmap_memory = cb_unmap;
    d.shell.read_memory = rd; d.shell.write_memory = wr; d.shell.raise_interrupt = irq;
    d.thread_count = 2;
    char *err = NULL;
    return lagfx_device_new(&d, &err);
}
static void submit(lagfx_protocol_t *p, uint16_t op, uint16_t arg8, const uint32_t *words, uint32_t nwords) {
    uint32_t len = 12u + nwords * 4u;
    uint64_t ring = (uint64_t)RING_PFN0 << 14;
    uint8_t cmd[2048]; memset(cmd, 0, sizeof cmd);
    memcpy(cmd, &op, 2); memcpy(cmd + 2, &arg8, 2); memcpy(cmd + 4, &len, 4);
    uint32_t stamp = g_wp + 1u; memcpy(cmd + 8, &stamp, 4);
    if (nwords) memcpy(cmd + 12, words, nwords * 4u);
    memcpy(g_mem + ring + (g_wp % 0x10000u), cmd, len);
    g_wp += len;
    put32(((uint64_t)SHARED_PFN << 14) + LAGFX_RING_FIFO_DESC_BASE_OFFSET + 0x00, g_wp);
    p->current_chan_id = 1;
    channel_compute_dispatcher_drain(p, 1);
}
static lagfx_protocol_t *mk(uint32_t mode) {
    memset(g_mem, 0, MEM_SIZE); g_wp = 0;
    lagfx_protocol_t *p = calloc(1, sizeof *p);
    p->magic = LAGFX_PROTOCOL_MAGIC; p->dev = g_dev;
    p->ring_page_shift = 14; p->protocol_mode = mode;
    p->ring_shared_page_pfn = SHARED_PFN;
    uint64_t d = ((uint64_t)SHARED_PFN << 14) + LAGFX_RING_FIFO_DESC_BASE_OFFSET;
    put32(d + 0x0c, 1); put32(d + 0x10, TABLE_PFN);
    for (uint32_t i = 0; i < 4; i++) put32(((uint64_t)TABLE_PFN << 14) + 4u * i, RING_PFN0 + i);
    put32((uint64_t)ROOT_PFN << 14, TOP_PFN); put32(((uint64_t)ROOT_PFN << 14) + 4, 2);
    put32((uint64_t)TOP_PFN << 14, LEAF_PFN);
    for (uint32_t v = 0; v < NVPN; v++) put32(((uint64_t)LEAF_PFN << 14) + 4u * v, DATA_PFN0 + v);
    return p;
}
static void va_write(uint64_t va, const void *src, uint32_t n) {
    const uint8_t *s = src;
    while (n) {
        uint32_t vpn = (uint32_t)(va >> 14), in = (uint32_t)(va & 0x3fff), c = 0x4000 - in < n ? 0x4000 - in : n;
        assert(vpn < NVPN);
        memcpy(g_mem + ((uint64_t)(DATA_PFN0 + vpn) << 14) + in, s, c);
        va += c; s += c; n -= c;
    }
}
static lagfx_task_entry_t *def_task(lagfx_protocol_t *p, uint32_t task) {
    lagfx_task_entry_t *t = lagfx_protocol_alloc_task_slot(p);
    assert(t);
    t->id = task; t->live = true; t->root_page_pfn = ROOT_PFN;
    t->lists_set = true; t->objects_addr = 0x100; t->objects_len = 0;
    return t;
}

#ifndef LAGFX_TEST_FIXTURE_DIR
#define LAGFX_TEST_FIXTURE_DIR "tests/fixtures"
#endif
#define VA_PAY 0x4000u
#define VA_CMD 0x8000u
#define VPN_LIB 6u        /* the kernel library (one 16 KiB page holds the 5.4 KB bitcode + container) */
#define VPN_SRC 8u
#define VPN_UBO 9u
#define VPN_WBUF 10u
#define REF_SRC 1u
#define REF_DST 2u
#define REF_UBO 3u
#define REF_WBUF 4u
#define REF_FN 5u
#define REF_TILE 6u
#define REF_DST4 7u       /* an RGBA8 target: the imageblock storage image does not match (fail closed) */

static void entry(uint32_t ref, uint32_t type, uint32_t size, uint64_t addr) {
    uint32_t e[3] = { type | size << 8, (uint32_t)addr, (uint32_t)(addr >> 32) };
    va_write(0x100u + ref * 12u, e, 12);
}
static void put_buffer_object(uint32_t ref, uint32_t pay_off, uint64_t len, uint32_t vpage) {
    uint8_t h[16] = {0};
    memcpy(h, &len, 8); memcpy(h + 8, &vpage, 4);
    va_write(VA_PAY + pay_off, h, 16);
    entry(ref, LAGFX_A64_OBJ_BUFFER, 16, VA_PAY + pay_off);
}
/* a 2D texture object (same wire layout as the other ARM64 tests); pixel format = MTLPixelFormat, bytes per pixel bpp, guest pages at vpage */
static void put_texture(uint32_t ref, uint32_t pay_off, uint32_t w, uint32_t h, uint32_t mtl_fmt, uint32_t bpp, uint32_t vpage) {
    uint8_t t[200]; memset(t, 0, sizeof t);
    uint64_t tlen = (uint64_t)w * h * bpp; memcpy(t, &tlen, 8);
    memcpy(t + 8, &vpage, 4);
    t[12] = 1;
    { uint8_t *mp = t + 12 + 0x18; uint64_t row = (uint64_t)w * bpp; uint32_t md = 1; memcpy(mp + 0x10, &row, 8); memcpy(mp + 0x18, &w, 4); memcpy(mp + 0x1c, &h, 4); memcpy(mp + 0x20, &md, 4); }
    uint32_t rec = 12 + 0x18 + 36, tag = 0x14;
    memcpy(t + rec, &tag, 4);
    uint8_t *d = t + rec + 12;
    d[0] = 2; d[2] = (uint8_t)mtl_fmt;
    uint32_t one = 1;
    memcpy(d + 4, &w, 4); memcpy(d + 8, &h, 4); memcpy(d + 12, &one, 4);
    d[16] = 1; d[18] = 1;
    uint32_t tsize = rec + 12 + 28;
    va_write(VA_PAY + pay_off, t, tsize);
    entry(ref, LAGFX_A64_OBJ_TEXTURE, tsize, VA_PAY + pay_off);
}
static uint8_t *slurp(const char *name, size_t *n) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", LAGFX_TEST_FIXTURE_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n);
    if (!b || fread(b, 1, *n, f) != *n) { fclose(f); free(b); return NULL; }
    fclose(f);
    return b;
}
/* a one-function MTLB around real AIR bitcode (TYPE byte: 0 vertex, 1 fragment, 2 kernel) */
static uint8_t *synth_mtlb(const uint8_t *bc, size_t bclen, const char *name, uint8_t stage, size_t *out) {
    size_t cap = 0x200 + bclen + 16;
    uint8_t *b = calloc(1, cap);
    memcpy(b, "MTLB", 4);
    uint64_t fet = 0x60, base = 0x200;
    memcpy(b + 0x18, &fet, 8); memcpy(b + 0x48, &base, 8);
    memcpy(b + base, bc, bclen);
    size_t p = fet;
    uint32_t one = 1; memcpy(b + p, &one, 4); p += 4;
    size_t es = p; p += 4;
    uint16_t l;
    size_t nl = strlen(name) + 1;
    memcpy(b + p, "NAME", 4); l = (uint16_t)nl; memcpy(b + p + 4, &l, 2); memcpy(b + p + 6, name, nl); p += 6 + nl;
    memcpy(b + p, "TYPE", 4); l = 1; memcpy(b + p + 4, &l, 2); b[p + 6] = stage; p += 7;
    uint64_t z = 0;
    memcpy(b + p, "OFFT", 4); l = 24; memcpy(b + p + 4, &l, 2); memcpy(b + p + 6, &z, 8); memcpy(b + p + 14, &z, 8); memcpy(b + p + 22, &z, 8); p += 30;
    uint64_t sz = bclen;
    memcpy(b + p, "MDSZ", 4); l = 8; memcpy(b + p + 4, &l, 2); memcpy(b + p + 6, &sz, 8); p += 14;
    memcpy(b + p, "ENDT", 4); p += 4;
    uint32_t esz = (uint32_t)(p - es); memcpy(b + es, &esz, 4);
    *out = cap;
    return b;
}
static void put_function(uint32_t ref, uint32_t pay_off, uint32_t vpn, const uint8_t *lib, size_t len) {
    va_write((uint64_t)vpn << 14, lib, (uint32_t)len);
    uint8_t h[20] = {0};
    uint64_t va = (uint64_t)vpn << 14; uint32_t l = (uint32_t)len, tag = 1, sz = 20;
    memcpy(h, &va, 8); memcpy(h + 8, &l, 4); memcpy(h + 12, &tag, 4); memcpy(h + 16, &sz, 4);
    va_write(VA_PAY + pay_off, h, 20);
    entry(ref, LAGFX_A64_OBJ_FUNCTION, 20, VA_PAY + pay_off);
}
/* a serialized TILE pipeline: {0x33, size, ref, length, tokens}; the kernel is named by a `01 04 <function ref>` token (as in the device dump) */
static void put_tile_pipeline(uint32_t ref, uint32_t pay_off, uint32_t fn) {
    uint8_t b[64]; memset(b, 0, sizeof b);
    uint32_t o = 16;
    b[o++] = 0x03; b[o++] = 0x00; b[o++] = 0x04; uint32_t v = 0x13; memcpy(b + o, &v, 4); o += 4;   /* two properties the dump carries (meaning unknown, ignored) */
    b[o++] = 0x03; b[o++] = 0x04; v = 0x40; memcpy(b + o, &v, 4); o += 4;
    b[o++] = 0x01; b[o++] = 0x04; memcpy(b + o, &fn, 4); o += 4;
    uint32_t tag = 0x33, len = o - 16;
    memcpy(b, &tag, 4); memcpy(b + 4, &o, 4); memcpy(b + 8, &ref, 4); memcpy(b + 12, &len, 4);
    va_write(VA_PAY + pay_off, b, o);
    entry(ref, LAGFX_A64_OBJ_SERIALIZED, o, VA_PAY + pay_off);
}

static uint8_t g_cb[4096];
static uint32_t g_cbn;
static void cb_reset(void) { g_cbn = 8; memset(g_cb, 0, sizeof g_cb); }
static void cb_cmd(uint32_t tag, const void *payload, uint32_t n) {
    uint32_t sz = 8 + n;
    memcpy(g_cb + g_cbn, &tag, 4); memcpy(g_cb + g_cbn + 4, &sz, 4);
    if (n) memcpy(g_cb + g_cbn + 8, payload, n);
    g_cbn += sz;
}
static void cb_pass(uint32_t ref, uint16_t load, const double clear[4]) {
    uint8_t r[584]; memset(r, 0, sizeof r);
    uint8_t *c = r + 0x4c;
    memcpy(c, &ref, 4); memcpy(c + 20, &load, 2);
    uint16_t store = 1; memcpy(c + 22, &store, 2);
    memcpy(c + 0x1c, clear, 32);
    cb_cmd(26, r, sizeof r);
}
static void cb_pipeline(uint32_t ref) { cb_cmd(116, &ref, 4); }
static void cb_tile_buf(uint32_t index, uint32_t ref, uint64_t off) { uint8_t r[8 + 12]; uint32_t one = 1; memcpy(r, &index, 4); memcpy(r + 4, &one, 4); memcpy(r + 8, &ref, 4); memcpy(r + 12, &off, 8); cb_cmd(157, r, sizeof r); }
static void cb_tile_tex(uint32_t index, uint32_t ref) { uint8_t r[12]; uint32_t one = 1; memcpy(r, &index, 4); memcpy(r + 4, &one, 4); memcpy(r + 8, &ref, 4); cb_cmd(161, r, sizeof r); }
static void cb_dispatch_all(uint64_t w, uint64_t h) { uint64_t v[3] = { w, h, 1 }; cb_cmd(155, v, sizeof v); }
static void cb_dispatch_region(uint64_t w, uint64_t h, uint64_t ox, uint64_t oy, uint64_t rw, uint64_t rh) {
    uint64_t v[9] = { w, h, 1, ox, oy, 0, rw, rh, 1 };
    uint8_t r[76]; memset(r, 0, sizeof r); memcpy(r, v, sizeof v);
    cb_cmd(162, r, sizeof r);
}
static uint32_t cb_finish(void) {
    uint32_t seglen = g_cbn, info = 0;
    memcpy(g_cb, &seglen, 4); memcpy(g_cb + 4, &info, 4);
    va_write(VA_CMD, g_cb, g_cbn);
    return g_cbn;
}
static void exec_refs(lagfx_protocol_t *p, uint32_t task, uint32_t len) {
    static const uint32_t refs[] = { REF_SRC, REF_DST, REF_UBO, REF_WBUF, REF_DST4 };
    static const uint32_t flags[] = { 1, 0, 1, 1, 0 };
    uint32_t w[3 + 2 * 5 + 4], n = 0;
    w[n++] = task; w[n++] = 5; w[n++] = 1;
    for (uint32_t i = 0; i < 5; i++) { w[n++] = refs[i]; w[n++] = flags[i]; }
    uint64_t off = VA_CMD, l = len;
    memcpy(w + n, &off, 8); n += 2; memcpy(w + n, &l, 8); n += 2;
    submit(p, 0x43, 0, w, n);
}

static uint16_t f2h(float f) {
    union { float f; uint32_t u; } v = { f };
    uint32_t s = (v.u >> 16) & 0x8000u; int e = (int)((v.u >> 23) & 0xff) - 127 + 15; uint32_t m = v.u & 0x7fffffu;
    if (f == 0.0f) return (uint16_t)s;
    return (uint16_t)(s | ((uint32_t)e << 10) | (m >> 13));
}
static float h2f(uint16_t h) {
    uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 0x1fu, m = h & 0x3ffu; float r;
    if (e == 0) r = (float)m / 1024.0f / 16384.0f;
    else { r = 1.0f + (float)m / 1024.0f; for (int k = (int)e - 15; k > 0; k--) r *= 2.0f; for (int k = (int)e - 15; k < 0; k++) r *= 0.5f; }
    return s ? -r : r;
}
static float texv(uint32_t x, uint32_t y, int ch) {
    switch (ch) { case 0: return (float)((x * 3 + y * 5) % 16); case 1: return (float)((x + y * 7) % 16); case 2: return (float)((x * y) % 16); default: return (float)((x + y) % 16); }
}

#ifdef LAGFX_HAVE_VULKAN
#define TILE(p) ((p)->diag2.exec.tile)
#define RUNC(p) ((p)->diag2.exec.run)
#define SENT 3.0f

static uint32_t g_S, g_W, g_H, g_TW;
static float g_w[15];

static void build_objects(const uint8_t *lib, size_t libn, bool horizontal) {
    put_function(REF_FN, 0x400, VPN_LIB, lib, libn);
    put_tile_pipeline(REF_TILE, 0x500, REF_FN);
    put_texture(REF_SRC, 0x000, g_TW, g_H, 115, 8, VPN_SRC);
    put_texture(REF_DST, 0x100, g_W, g_H, 115, 8, 0);
    put_texture(REF_DST4, 0x200, g_W, g_H, 70, 4, 0);   /* RGBA8Unorm */
    put_buffer_object(REF_UBO, 0x300, 0x4000, VPN_UBO);
    put_buffer_object(REF_WBUF, 0x320, 0x4000, VPN_WBUF);
    uint16_t *tp = calloc((size_t)g_TW * g_H * 4, 2);
    for (uint32_t y = 0; y < g_H; y++) for (uint32_t x = 0; x < g_TW; x++) for (int ch = 0; ch < 4; ch++) tp[((size_t)y * g_TW + x) * 4 + ch] = f2h(texv(x, y, ch));
    va_write((uint64_t)VPN_SRC << 14, tp, g_TW * g_H * 8);
    free(tp);
    uint16_t u[8] = { 0, 0, (uint16_t)(horizontal ? g_S : 0), (uint16_t)(horizontal ? 0 : g_S), horizontal, f2h(0.5f), 0, 0 };   /* { src_offset, fill_offset, horizontal, edr_scale } */
    va_write((uint64_t)VPN_UBO << 14, u, sizeof u);
    uint16_t w16[16] = {0};
    for (int i = 0; i < 15; i++) { g_w[i] = ((uint32_t)i <= g_S) ? (float)(i % 4 + 1) / 8.0f : 0.0f; w16[i] = f2h(g_w[i]); }
    va_write((uint64_t)VPN_WBUF << 14, w16, sizeof w16);
}
/* the expected value of the horizontal kernel at (x, y), the same model as tests/air2spv-compute-lavapipe.c */
static float model(uint32_t x, uint32_t y, int ch) {
    float acc = 0;
    for (uint32_t i = 0; i <= g_S && i < 15; i++) acc += g_w[i] * texv(x + i, y, ch);
    return ch < 3 ? acc * 0.5f : acc;
}
static lagfx_protocol_t *new_task(lagfx_task_entry_t **t, const uint8_t *lib, size_t libn) {
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    *t = def_task(p, 1);
    build_objects(lib, libn, true);
    return p;
}
static unsigned compare(lagfx_protocol_t *p, lagfx_task_entry_t *t, uint32_t ref, bool (*computed)(uint32_t, uint32_t), unsigned *n_computed, unsigned *n_kept) {
    uint16_t *img = malloc((size_t)g_W * g_H * 8);
    uint32_t w, h;
    assert(lagfx_a64_exec_readback_texture(p, t, ref, img, (size_t)g_W * g_H * 8, &w, &h) && w == g_W && h == g_H);
    unsigned bad = 0; *n_computed = *n_kept = 0;
    for (uint32_t y = 0; y < g_H; y++) for (uint32_t x = 0; x < g_W; x++) {
        bool c = computed(x, y);
        for (int ch = 0; ch < 4; ch++) {
            float got = h2f(img[((size_t)y * g_W + x) * 4 + ch]), want = c ? model(x, y, ch) : SENT;
            if (got != want) { if (bad++ < 6) printf("  mismatch (%u,%u).%d %s: got %g expected %g\n", x, y, ch, c ? "computed" : "kept", got, want); }
        }
        if (c) (*n_computed)++; else (*n_kept)++;
    }
    free(img);
    return bad;
}
static bool all_px(uint32_t x, uint32_t y) { (void)x; (void)y; return true; }
static bool no_px(uint32_t x, uint32_t y) { (void)x; (void)y; return false; }
static bool right_half_and_top_left(uint32_t x, uint32_t y) { return x >= g_S || y < 4; }

static void tile_pass_clear(void) { static const double sent[4] = { SENT, SENT, SENT, SENT }; cb_reset(); cb_pass(REF_DST, 2, sent); }
static void tile_bind_all(void) { cb_pipeline(REF_TILE); cb_tile_buf(0, REF_UBO, 0); cb_tile_buf(1, REF_WBUF, 0); cb_tile_tex(0, REF_SRC); }

static void test_tile(const uint8_t *lib, size_t libn) {
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    struct lagfx_vk_state *vk = g_dev->vk;
    g_S = lagfx_vk_can_pin_subgroup32(vk) ? 32u : vk->sg_size;
    if (g_S < 4 || g_S > 64) { printf("SKIP: subgroup size %u not handled by this test\n", g_S); return; }
    g_W = 2 * g_S, g_H = 8, g_TW = 3 * g_S + 4;
    unsigned nc, nk;

    /* (1) fail closed: the device's subgroup is not 32 wide and cannot be pinned -> the kernel (simd lane mapping) is not run, the attachment keeps its clear */
    bool native = g_S == 32u;
    if (!native) {
        unsetenv("LAGFX_TILE_ANY_SUBGROUP");
        lagfx_task_entry_t *t; lagfx_protocol_t *p = new_task(&t, lib, libn);
        tile_pass_clear(); tile_bind_all(); cb_dispatch_all(g_S, 4);
        exec_refs(p, 1, cb_finish());
        assert(p->diag2.exec.pipe.resolved == 1 && TILE(p).pipes == 1 && TILE(p).skip_no_sg32 == 1 && TILE(p).dispatches == 0 && RUNC(p).submit_failed == 0);
        assert(RUNC(p).tag_partial[155] == 1 && compare(p, t, REF_DST, no_px, &nc, &nk) == 0);
        lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); pfree(p);
        puts("PASS: no 32-lane subgroup and no pinning -> tile dispatch skipped (counted), attachment untouched");
        setenv("LAGFX_TILE_ANY_SUBGROUP", "1", 1);   /* the rest runs the kernel with this device's own subgroup size (workgroup = (S, 4, 1), weights beyond tap S are 0) */
    }

    /* (2) whole-target dispatch: every pixel computed */
    {
        lagfx_task_entry_t *t; lagfx_protocol_t *p = new_task(&t, lib, libn);
        tile_pass_clear(); tile_bind_all(); cb_dispatch_all(g_S, 4);
        exec_refs(p, 1, cb_finish());
        printf("INFO: tile whole: pipes=%llu dispatches=%llu groups=%llu sg_pinned=%llu override=%llu skip[rt=%llu unbound=%llu desc=%llu pipe_build=%llu] resume_failed=%llu\n", (unsigned long long)TILE(p).pipes,
               (unsigned long long)TILE(p).dispatches, (unsigned long long)TILE(p).groups, (unsigned long long)TILE(p).sg_pinned, (unsigned long long)TILE(p).sg_override, (unsigned long long)TILE(p).skip_rt,
               (unsigned long long)TILE(p).skip_unbound, (unsigned long long)TILE(p).skip_desc, (unsigned long long)TILE(p).skip_pipe_build, (unsigned long long)TILE(p).resume_failed);
        assert(TILE(p).pipes == 1 && TILE(p).dispatches == 1 && TILE(p).whole == 1 && TILE(p).groups == 4u && TILE(p).resume_failed == 0 && RUNC(p).submit_failed == 0);
        assert(TILE(p).binds_buf == 2 && TILE(p).binds_tex == 1 && RUNC(p).tag_done[155] == 1 && RUNC(p).tag_done[157] == 2 && RUNC(p).tag_done[161] == 1);
        unsigned bad = compare(p, t, REF_DST, all_px, &nc, &nk);
        printf("tile whole-target dispatch: %u pixels, %u channel mismatches\n", nc, bad);
        assert(bad == 0);
        lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); pfree(p);
        puts("PASS: tile_simd_blur_15 through tags 0x33 / 0x9d / 0xa1 / 0x9b on lavapipe: pass suspended, compute dispatched, pass resumed, every pixel equals the CPU model");
    }

    /* (3) two region dispatches in ONE pass: right half (origin (S, 0)), then the top-left quadrant. The first result must survive the second suspend / resume (LOAD ops);
     * the bottom-left quadrant is never dispatched and keeps the clear colour */
    {
        lagfx_task_entry_t *t; lagfx_protocol_t *p = new_task(&t, lib, libn);
        tile_pass_clear(); tile_bind_all();
        cb_dispatch_region(g_S, 4, g_S, 0, g_S, g_H);
        cb_dispatch_region(g_S, 4, 0, 0, g_S, 4);
        exec_refs(p, 1, cb_finish());
        assert(TILE(p).dispatches == 2 && TILE(p).region == 2 && TILE(p).groups == 2u + 1u && TILE(p).resume_failed == 0 && RUNC(p).tag_done[162] == 2 && RUNC(p).submit_failed == 0);
        unsigned bad = compare(p, t, REF_DST, right_half_and_top_left, &nc, &nk);
        printf("tile region dispatches: %u computed + %u kept pixels, %u channel mismatches\n", nc, nk, bad);
        assert(bad == 0 && nk == g_S * 4u);
        lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); pfree(p);
        puts("PASS: tile region dispatch (tag 0xa2): WorkgroupId base shifted to the region origin, attachment contents preserved across suspend / resume, untouched region keeps its clear");
    }

    /* (4) fail closed: unbound texture (not set), unbound buffer, and an attachment that is not Rgba16f */
    {
        lagfx_task_entry_t *t; lagfx_protocol_t *p = new_task(&t, lib, libn);
        tile_pass_clear(); cb_pipeline(REF_TILE); cb_tile_buf(0, REF_UBO, 0); cb_tile_buf(1, REF_WBUF, 0); cb_dispatch_all(g_S, 4);   /* no texture */
        exec_refs(p, 1, cb_finish());
        assert(TILE(p).skip_unbound == 1 && TILE(p).dispatches == 0 && RUNC(p).tag_partial[155] == 1);
        assert(compare(p, t, REF_DST, no_px, &nc, &nk) == 0);
        static const double sent[4] = { SENT, SENT, SENT, SENT };
        cb_reset(); cb_pass(REF_DST4, 2, sent); tile_bind_all(); cb_dispatch_all(g_S, 4);
        exec_refs(p, 1, cb_finish());
        assert(TILE(p).skip_rt == 1 && TILE(p).dispatches == 0);
        cb_reset(); cb_pass(REF_DST, 2, sent); cb_dispatch_all(g_S, 4);    /* no pipeline set in this pass */
        exec_refs(p, 1, cb_finish());
        assert(TILE(p).skip_no_pipe == 1);
        lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); pfree(p);
        puts("PASS: tile dispatch fails closed: unbound texture, non-Rgba16f attachment, no pipeline set (all counted, pass untouched)");
    }
}
#endif

int main(void) {
    g_mem = calloc(1, MEM_SIZE);
    g_dev = make_device();
    assert(g_dev);
#ifdef LAGFX_HAVE_VULKAN
    size_t bn = 0, ln = 0;
    uint8_t *bc = slurp("tile_simd_blur_15.air.bc", &bn);
    if (!bc) { puts("SKIP: fixture tile_simd_blur_15.air.bc missing"); lagfx_device_free(g_dev); free(g_mem); return 77; }
    uint8_t *lib = synth_mtlb(bc, bn, "tile_simd_blur_15", 2, &ln);
    if (!g_dev->vk || !g_dev->vk->initialized) { puts("SKIP: no Vulkan device"); free(lib); free(bc); lagfx_device_free(g_dev); free(g_mem); return 77; }
    struct lagfx_vk_state *vk = g_dev->vk;
    uint32_t need = LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SUBGROUP) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SUBGROUP_SHUFFLE);
    if ((vk->caps_enabled & need) != need) { puts("SKIP: device has no compute subgroup shuffle"); free(lib); free(bc); lagfx_device_free(g_dev); free(g_mem); return 77; }
    test_tile(lib, ln);
    free(lib); free(bc);
#else
    puts("SKIP: built without Vulkan");
#endif
    lagfx_device_free(g_dev);
    free(g_mem);
    return 0;
}
