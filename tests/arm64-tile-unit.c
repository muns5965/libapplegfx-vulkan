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
#include "protocol/diag.h"
#include <unistd.h>
#include "vulkan/caps.h"
#include "device.h"
#include "common/le.h"
#include "libapplegfx-vulkan.h"
#ifdef LAGFX_HAVE_VULKAN
#include "vulkan/instance.h"
#include <vulkan/vulkan.h>
#include "vulkan/format_caps.h"
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
#define REF_DSTB 8u       /* a BGRA8 target (MTLPixelFormat 80 = VK_FORMAT_B8G8R8A8_UNORM): the unformatted-image kernel variant */

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
    static const uint32_t refs[] = { REF_SRC, REF_DST, REF_UBO, REF_WBUF, REF_DST4, REF_DSTB };
    static const uint32_t flags[] = { 1, 0, 1, 1, 0, 0 };
    uint32_t w[3 + 2 * 6 + 4], n = 0;
    w[n++] = task; w[n++] = 6; w[n++] = 1;
    for (uint32_t i = 0; i < 6; i++) { w[n++] = refs[i]; w[n++] = flags[i]; }
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
static float g_texscale = 1.0f;   /* the BGRA8 test scales the source so the unorm8 result is not saturated (powers of two: exact in half) */
static float texv(uint32_t x, uint32_t y, int ch) {
    switch (ch) { case 0: return g_texscale * (float)((x * 3 + y * 5) % 16); case 1: return g_texscale * (float)((x + y * 7) % 16); case 2: return g_texscale * (float)((x * y) % 16); default: return g_texscale * (float)((x + y) % 16); }
}

#ifdef LAGFX_HAVE_VULKAN
#define TILE(p) ((p)->diag2.exec.tile)
#define RUNC(p) ((p)->diag2.exec.run)
#define SENT 3.0f

static uint32_t g_S, g_W, g_H, g_TW, g_TH;
static bool g_vert;
static float g_w[15];

static void build_objects(const uint8_t *lib, size_t libn, bool horizontal) {
    put_function(REF_FN, 0x400, VPN_LIB, lib, libn);
    put_tile_pipeline(REF_TILE, 0x500, REF_FN);
    put_texture(REF_SRC, 0x000, g_TW, g_TH, 115, 8, VPN_SRC);
    put_texture(REF_DST, 0x100, g_W, g_H, 115, 8, 0);
    put_texture(REF_DST4, 0x200, g_W, g_H, 70, 4, 0);   /* RGBA8Unorm */
    put_texture(REF_DSTB, 0x600, g_W, g_H, 80, 4, 0);   /* BGRA8Unorm */
    put_buffer_object(REF_UBO, 0x300, 0x4000, VPN_UBO);
    put_buffer_object(REF_WBUF, 0x320, 0x4000, VPN_WBUF);
    uint16_t *tp = calloc((size_t)g_TW * g_TH * 4, 2);
    for (uint32_t y = 0; y < g_TH; y++) for (uint32_t x = 0; x < g_TW; x++) for (int ch = 0; ch < 4; ch++) tp[((size_t)y * g_TW + x) * 4 + ch] = f2h(texv(x, y, ch));
    va_write((uint64_t)VPN_SRC << 14, tp, g_TW * g_TH * 8);
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
    for (uint32_t i = 0; i <= g_S && i < 15; i++) acc += g_w[i] * (g_vert ? texv(x, y + i, ch) : texv(x + i, y, ch));
    return ch < 3 ? acc * 0.5f : acc;
}
static lagfx_protocol_t *new_task(lagfx_task_entry_t **t, const uint8_t *lib, size_t libn) {
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    *t = def_task(p, 1);
    build_objects(lib, libn, !g_vert);
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

static bool vert_cols(uint32_t x, uint32_t y) { (void)y; return (x % g_S) < 4u; }   /* the 4-wide columns of the two tiles */

static void tile_pass_clear(void) { static const double sent[4] = { SENT, SENT, SENT, SENT }; cb_reset(); cb_pass(REF_DST, 2, sent); }
static void tile_bind_all(void) { cb_pipeline(REF_TILE); cb_tile_buf(0, REF_UBO, 0); cb_tile_buf(1, REF_WBUF, 0); cb_tile_tex(0, REF_SRC); }


/* injected device: B8G8R8A8 has no storage-image feature and refuses STORAGE usage unless the image is the MUTABLE / EXTENDED_USAGE surrogate */
static VkPhysicalDevice tile_inj_pd;
static void tile_inj_fp2(void *ctx, VkFormat f, VkFormatProperties2 *out) {
    (void)ctx; vkGetPhysicalDeviceFormatProperties2(tile_inj_pd, f, out);
    if (f == VK_FORMAT_B8G8R8A8_UNORM) out->formatProperties.optimalTilingFeatures &= ~(VkFormatFeatureFlags)VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
}
static VkResult tile_inj_ifp2(void *ctx, const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *out) {
    (void)ctx;
    if (info->format == VK_FORMAT_B8G8R8A8_UNORM && (info->usage & VK_IMAGE_USAGE_STORAGE_BIT) && !(info->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    return vkGetPhysicalDeviceImageFormatProperties2(tile_inj_pd, info, out);
}

static void test_tile(const uint8_t *lib, size_t libn, uint32_t force_w) {
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    struct lagfx_vk_state *vk = g_dev->vk;
    bool wide = vk->sg_size > 32u && !(vk->sg_size % 32u) && vk->sg_min == vk->sg_max;   /* a fixed 64 / 128 wide subgroup: the dispatcher emulates 32-lane simdgroups */
    g_S = force_w ? force_w : (lagfx_vk_can_pin_subgroup32(vk) || wide) ? 32u : vk->sg_size;
    if (force_w) { char e[16]; snprintf(e, sizeof e, "%u", force_w); setenv("LAGFX_TILE_SIMD_EMULATE", e, 1); }
    else unsetenv("LAGFX_TILE_SIMD_EMULATE");
    printf("INFO: tile test: device subgroup %u [%u..%u], logical simdgroup width %u (%s)\n", vk->sg_size, vk->sg_min, vk->sg_max, g_S, force_w ? "forced emulation hook" : wide ? "emulated on a wide subgroup" : g_S == 32u ? "native / pinned 32" : "device size");
    if (g_S < 4 || g_S > 64) { printf("SKIP: subgroup size %u not handled by this test\n", g_S); return; }
    g_vert = false;
    g_W = 2 * g_S, g_H = 8, g_TW = 3 * g_S + 4, g_TH = g_H;
    unsigned nc, nk;

    /* (1) fail closed: the device's subgroup is not 32 wide and cannot be pinned -> the kernel (simd lane mapping) is not run, the attachment keeps its clear */
    bool native = force_w || wide || g_S == 32u;
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
        {   /* VK_QCOM_tile_shading opt-in (default OFF): the dispatches ran as compute emulation, the extension was not enabled, and the diag dump carries the g7-gpu-qcomtile line with those counts */
            assert(p->diag2.exec.qcomtile.emul_dispatches == 2 && p->diag2.exec.qcomtile.emul_while_enabled == 0 && !g_dev->vk->qcomtile.enabled);
            char lp[] = "/tmp/tile-diag-XXXXXX"; int lfd = mkstemp(lp); assert(lfd >= 0);
            fflush(stderr); int sv = dup(2); assert(sv >= 0 && dup2(lfd, 2) >= 0);   /* the dump prints each line to stderr as well as to the log sink */
            p->diag2.t0_ns = 1; lagfx_diag_final(p);
            fflush(stderr); dup2(sv, 2); close(sv); close(lfd);
            FILE *lf = fopen(lp, "r"); char ln[4096]; bool found = false;
            while (lf && fgets(ln, sizeof ln, lf)) if (strstr(ln, "g7-gpu-qcomtile:") && strstr(ln, "use[emul_dispatches=2 emul_while_qcom_enabled=0 qcom_native_dispatches=0]") && strstr(ln, "chosen_path=compute_emulation")) found = true;
            if (lf) fclose(lf);
            unlink(lp);
            assert(found);
            puts("PASS: g7-gpu-qcomtile counted line in the diag dump (base line + use[emul_dispatches=2 emul_while_qcom_enabled=0 qcom_native_dispatches=0])");
        }
        lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); pfree(p);
        puts("PASS: tile region dispatch (tag 0xa2): WorkgroupId base shifted to the region origin, attachment contents preserved across suspend / resume, untouched region keeps its clear");
    }

    if (force_w || wide) {
        lagfx_task_entry_t *t; lagfx_protocol_t *p = new_task(&t, lib, libn);
        tile_pass_clear(); tile_bind_all(); cb_dispatch_all(g_S, 4);
        exec_refs(p, 1, cb_finish());
        assert(TILE(p).sg_emulated == 1 && TILE(p).sg_pinned == 0 && TILE(p).sg_override == 0 && TILE(p).skip_no_sg32 == 0);
        lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); pfree(p);
        puts("PASS: logical-simdgroup emulation is what ran (tile.sg_emulated counted, no pin / override / skip)");
    }

    /* (3b) the same kernel in its vertical mode (uniform horizontal = 0: the thread coordinates are swapped, the shuffle runs along y; src_offset = (0,0), fill_offset = (0,S)).
     * Read from the AIR: a tile (S,4) writes the 4-wide, S-tall pixel block at (WorkgroupId * (S,4)) + (ty, tx), each = sum_i w[i] * tex(x, y + i). One row of tiles is dispatched
     * as a region so every written pixel is inside the S-tall target; the rest keeps the clear. */
    {
        g_vert = true; g_W = 2 * g_S; g_H = g_S; g_TW = g_W; g_TH = 2 * g_S + 4;
        lagfx_task_entry_t *t; lagfx_protocol_t *p = new_task(&t, lib, libn);
        tile_pass_clear(); tile_bind_all();
        cb_dispatch_region(g_S, 4, 0, 0, g_W, 4);
        exec_refs(p, 1, cb_finish());
        assert(TILE(p).dispatches == 1 && TILE(p).region == 1 && TILE(p).groups == 2u && TILE(p).resume_failed == 0 && RUNC(p).submit_failed == 0);
        unsigned bad = compare(p, t, REF_DST, vert_cols, &nc, &nk);
        printf("tile vertical region dispatch: %u computed + %u kept pixels, %u channel mismatches\n", nc, nk, bad);
        assert(bad == 0 && nc == 8u * g_S);
        lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); pfree(p);
        g_vert = false; g_W = 2 * g_S, g_H = 8, g_TW = 3 * g_S + 4, g_TH = g_H;
        puts("PASS: vertical simd blur (swapped lanes, shuffle along y) equals the CPU model");
    }

    /* (3c) buffers bound at an offset that is not a multiple of minStorageBufferOffsetAlignment (Metal allows any 4-aligned one). Both kernel buffers are read-only per the
     * arg metadata, so they are bound through aligned host copies and the result still equals the CPU model; the geometry and the unaligned binds land in the diag arrays. */
    if (vk->phys_props.limits.minStorageBufferOffsetAlignment <= 4u) puts("SKIP: unaligned tile buffer offsets (the device accepts any 4-aligned storage buffer offset)");
    else {
        lagfx_task_entry_t *t; lagfx_protocol_t *p = new_task(&t, lib, libn);
        uint16_t u[8] = { 0, 0, (uint16_t)g_S, 0, 1, f2h(0.5f), 0, 0 }, w16[16] = {0};
        for (int i = 0; i < 15; i++) w16[i] = f2h(g_w[i]);
        va_write(((uint64_t)VPN_UBO << 14) + 4, u, sizeof u);      /* the same uniforms / weights, shifted by 4 bytes */
        va_write(((uint64_t)VPN_WBUF << 14) + 4, w16, sizeof w16);
        tile_pass_clear(); cb_pipeline(REF_TILE); cb_tile_buf(0, REF_UBO, 4); cb_tile_buf(1, REF_WBUF, 4); cb_tile_tex(0, REF_SRC);
        cb_dispatch_all(g_S, 4);
        exec_refs(p, 1, cb_finish());
        assert(TILE(p).dispatches == 1 && TILE(p).skip_unaligned == 0 && TILE(p).gathered == 2u && TILE(p).gather_failed == 0 && TILE(p).unaligned_written == 0);
        assert(TILE(p).n_unal == 2u && TILE(p).unal[0].off == 4u && !TILE(p).unal[0].written && TILE(p).unal[0].align == vk->phys_props.limits.minStorageBufferOffsetAlignment);
        assert(TILE(p).n_geom == 1u && !TILE(p).geom[0].region && TILE(p).geom[0].tw == g_S && TILE(p).geom[0].th == 4u && TILE(p).geom[0].tgt_w == g_W && TILE(p).geom[0].tgt_h == g_H);
        assert(compare(p, t, REF_DST, all_px, &nc, &nk) == 0);
        lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); pfree(p);
        puts("PASS: read-only tile buffers at offsets the device cannot bind are bound through aligned copies (result equals the CPU model); geometry + unaligned binds recorded");
    }

    /* (5) B8G8R8A8_UNORM attachment: the kernel variant with an unformatted storage-image imageblock. The source is scaled (1/256) so the unorm8 result is not saturated; the
     * attachment is cleared to four DIFFERENT logical channel values, so a swapped R / B (or any component mix-up) is visible: the expected bytes are in B, G, R, A memory order
     * and the kernel's channel k must land in logical component k. unorm8 rounding: +-1. On a device without without-format storage access for B8G8R8A8 the dispatch is skipped
     * and the cause counted (skip_rt_why 2 / 4 / 5): then only that is checked. */
    {
        const bool dev_nofmt = vk->have_storage_img_rw_nofmt;
        g_texscale = 1.0f / 256.0f;
        lagfx_task_entry_t *t; lagfx_protocol_t *p = new_task(&t, lib, libn);
        static const double clr[4] = { 0.2, 0.4, 0.6, 0.8 };
        cb_reset(); cb_pass(REF_DSTB, 2, clr); tile_bind_all(); cb_dispatch_all(g_S, 4);
        exec_refs(p, 1, cb_finish());
        if (TILE(p).dispatches == 1) {
            assert(dev_nofmt && TILE(p).skip_rt == 0 && TILE(p).bgra8_dispatches == 1 && TILE(p).bgra8_variants == 1 && TILE(p).bgra8_variant_failed == 0 && TILE(p).resume_failed == 0 && RUNC(p).submit_failed == 0);
            uint8_t *img = malloc((size_t)g_W * g_H * 4); uint32_t w, h;
            assert(lagfx_a64_exec_readback_texture(p, t, REF_DSTB, img, (size_t)g_W * g_H * 4, &w, &h) && w == g_W && h == g_H);
            unsigned bad = 0, sat = 0;
            for (uint32_t y = 0; y < g_H; y++) for (uint32_t x = 0; x < g_W; x++) for (int ch = 0; ch < 4; ch++) {
                static const int byte_of[4] = { 2, 1, 0, 3 };   /* logical R, G, B, A -> byte of a BGRA pixel */
                float m = model(x, y, ch); if (m > 1.0f) m = 1.0f, sat++;
                int want = (int)(m * 255.0f + 0.5f), got = img[((size_t)y * g_W + x) * 4 + byte_of[ch]];
                if (got < want - 1 || got > want + 1) { if (bad++ < 6) printf("  BGRA8 mismatch (%u,%u).%d: got %d expected %d\n", x, y, ch, got, want); }
            }
            printf("tile BGRA8 whole-target dispatch: %u saturated channel values, %u mismatches\n", sat, bad);
            assert(bad == 0 && sat == 0);
            free(img);
            /* a second dispatch in the same executor reuses the cached variant (no second translation) */
            cb_reset(); cb_pass(REF_DSTB, 2, clr); tile_bind_all(); cb_dispatch_region(g_S, 4, g_S, 0, g_S, g_H);
            exec_refs(p, 1, cb_finish());
            assert(TILE(p).dispatches == 2 && TILE(p).bgra8_dispatches == 2 && TILE(p).bgra8_variants == 1);
            puts("PASS: B8G8R8A8_UNORM imageblock: unformatted-storage kernel variant runs, channel order (R,G,B,A -> BGRA bytes) and unorm8 conversion equal the CPU model, variant cached");
        } else {
            assert(TILE(p).skip_rt == 1 && (TILE(p).skip_rt_why[4] + TILE(p).skip_rt_why[5] + TILE(p).skip_rt_why[2]) == 1 && TILE(p).bgra8_dispatches == 0 && TILE(p).bgra8_variants == 0);
            assert(TILE(p).skip_rt_first.valid && TILE(p).skip_rt_first.format == VK_FORMAT_B8G8R8A8_UNORM);
            printf("SKIP-EXEC: this device cannot run a BGRA8 imageblock (why[2]=%llu why[4]=%llu why[5]=%llu): refused with its own counter, attachment untouched\n",
                   (unsigned long long)TILE(p).skip_rt_why[2], (unsigned long long)TILE(p).skip_rt_why[4], (unsigned long long)TILE(p).skip_rt_why[5]);
        }
        lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); pfree(p);
        /* the device feature gate: with the without-format feature reported off, the BGRA8 attachment is refused with its own cause (4), never dispatched */
        const bool saved = vk->have_storage_img_rw_nofmt;
        vk->have_storage_img_rw_nofmt = false;
        p = new_task(&t, lib, libn);
        cb_reset(); cb_pass(REF_DSTB, 2, clr); tile_bind_all(); cb_dispatch_all(g_S, 4);
        exec_refs(p, 1, cb_finish());
        assert(TILE(p).dispatches == 0 && TILE(p).skip_rt == 1 && (TILE(p).skip_rt_why[4] == 1 || TILE(p).skip_rt_why[2] == 1) && TILE(p).bgra8_variants == 0);
        vk->have_storage_img_rw_nofmt = saved;
        lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); pfree(p);
        g_texscale = 1.0f;
        puts("PASS: BGRA8 imageblock without the device's unformatted storage access is refused with its own counter, nothing dispatched");
    }


    /* (5b) B8G8R8A8_UNORM attachment on a device WITHOUT storage-image / without-format access for that format (the Adreno 730 case, injected): the image is the MUTABLE |
     * EXTENDED_USAGE surrogate, the kernel is translated with an Rgba8 imageblock + red/blue swap, bound through the RGBA8 storage view. Same CPU model, same bytes. */
    {
        static VkPhysicalDevice s_pd;
        s_pd = vk->phys_device;
        static const lagfx_fmtcap_backend_t be = { .ctx = NULL, .format_props2 = tile_inj_fp2, .image_format_props2 = tile_inj_ifp2 };
        tile_inj_pd = s_pd;
        lagfx_fmtcap_test_backend = &be;
        const bool saved_nf = vk->have_storage_img_rw_nofmt;
        vk->have_storage_img_rw_nofmt = false;
        g_texscale = 1.0f / 256.0f;
        lagfx_task_entry_t *t; lagfx_protocol_t *p = new_task(&t, lib, libn);
        static const double clr[4] = { 0.2, 0.4, 0.6, 0.8 };
        cb_reset(); cb_pass(REF_DSTB, 2, clr); tile_bind_all(); cb_dispatch_all(g_S, 4);
        exec_refs(p, 1, cb_finish());
        assert(p->diag2.exec.fmt.st_surr >= 1u);   /* the attachment is a surrogate image */
        assert(TILE(p).dispatches == 1 && TILE(p).skip_rt == 0 && TILE(p).bgra8_surr_dispatches == 1 && TILE(p).bgra8_dispatches == 0 && TILE(p).bgra8_variants == 1 && TILE(p).bgra8_variant_failed == 0 && TILE(p).resume_failed == 0 && RUNC(p).submit_failed == 0);
        uint8_t *img = malloc((size_t)g_W * g_H * 4); uint32_t w, h;
        assert(lagfx_a64_exec_readback_texture(p, t, REF_DSTB, img, (size_t)g_W * g_H * 4, &w, &h) && w == g_W && h == g_H);
        unsigned bad = 0;
        for (uint32_t y = 0; y < g_H; y++) for (uint32_t x = 0; x < g_W; x++) for (int ch = 0; ch < 4; ch++) {
            static const int byte_of[4] = { 2, 1, 0, 3 };
            float m = model(x, y, ch); if (m > 1.0f) m = 1.0f;
            int want = (int)(m * 255.0f + 0.5f), got = img[((size_t)y * g_W + x) * 4 + byte_of[ch]];
            if (got < want - 1 || got > want + 1) { if (bad++ < 6) printf("  BGRA8 surrogate mismatch (%u,%u).%d: got %d expected %d\n", x, y, ch, got, want); }
        }
        printf("tile BGRA8 surrogate dispatch: %u mismatches\n", bad);
        assert(bad == 0);
        free(img);
        cb_reset(); cb_pass(REF_DSTB, 2, clr); tile_bind_all(); cb_dispatch_region(g_S, 4, g_S, 0, g_S, g_H);
        exec_refs(p, 1, cb_finish());
        assert(TILE(p).dispatches == 2 && TILE(p).bgra8_surr_dispatches == 2 && TILE(p).bgra8_variants == 1);
        lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); pfree(p);
        vk->have_storage_img_rw_nofmt = saved_nf;
        lagfx_fmtcap_test_backend = NULL;
        g_texscale = 1.0f;
        puts("PASS: B8G8R8A8 imageblock on a device with no BGRA8 storage: RGBA8 storage view + red/blue swap executes, bytes equal the CPU model, variant cached");
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
        assert(TILE(p).skip_rt_why[1] == 1 && TILE(p).skip_rt_first.valid && TILE(p).skip_rt_first.format != VK_FORMAT_R16G16B16A16_SFLOAT);   /* the cause is recorded: a non-Rgba16f attachment */
        cb_reset(); cb_pass(REF_DST, 2, sent); cb_dispatch_all(g_S, 4);    /* no pipeline set in this pass */
        exec_refs(p, 1, cb_finish());
        assert(TILE(p).skip_no_pipe == 1);
        lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); pfree(p);
        puts("PASS: tile dispatch fails closed: unbound texture, non-Rgba16f attachment, no pipeline set (all counted, pass untouched)");
    }
}
#endif

#ifdef LAGFX_HAVE_VULKAN
/* A tile kernel that uses a SIMD-group operation the translator cannot translate (air.simd_sum, hand-written fixture): the pipeline is NOT built (its result would be an OpUndef
 * stored into the imageblock), counted as pipe_unsupported_simd; the dispatch then finds no pipeline and the attachment keeps its clear. */
static void test_tile_unsupported_simd(struct lagfx_vk_state *vk) {
    size_t bn = 0, ln = 0;
    uint8_t *bc = slurp("simd_unsup_sum.air.bc", &bn);
    if (!bc) { puts("SKIP: fixture simd_unsup_sum.air.bc missing"); return; }
    uint8_t *lib = synth_mtlb(bc, bn, "simd_unsup_sum", 2, &ln);
    g_vert = false; g_S = vk->sg_size; g_W = 2 * g_S, g_H = 8, g_TW = 3 * g_S + 4, g_TH = g_H;
    unsigned nc, nk;
    lagfx_task_entry_t *t; lagfx_protocol_t *p = new_task(&t, lib, ln);
    tile_pass_clear(); cb_pipeline(REF_TILE); cb_dispatch_all(g_S, 4);
    exec_refs(p, 1, cb_finish());
    assert(TILE(p).pipe_unsupported_simd == 1 && TILE(p).pipes == 0 && TILE(p).pipe_failed >= 1 && TILE(p).dispatches == 0 && RUNC(p).submit_failed == 0);
    assert(compare(p, t, REF_DST, no_px, &nc, &nk) == 0);
    lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); pfree(p);
    free(lib); free(bc);
    puts("PASS: a tile kernel with an untranslatable SIMD-group operation (air.simd_sum) is not built (tile.pipe_unsupported_simd counted), the attachment is untouched");
}
#include "air/bitcode_reader.h"
#include "air2spv/translate.h"
/* Translator level (no device): the kernel translated for logical 32-lane groups (the Adreno case: a 64-wide subgroup) and for 64 / 128 wide ones is valid SPIR-V, differs from
 * the native translation, and the native translation is unchanged by a previous emulated one (the width is per call, reset afterwards). NOT executed at those widths. */
static void test_translate_widths(const uint8_t *bc, size_t bn) {
    lagfx_air_module_t *m = NULL;
    assert(lagfx_air_module_open(bc, bn, &m) == LAGFX_OK && m);
    uint8_t *nat = NULL, *nat2 = NULL, *e32 = NULL, *e4 = NULL; size_t nn = 0, nn2 = 0, n32 = 0, n4 = 0;
    assert(lagfx_air2spv_translate_module(m, &nat, &nn) == LAGFX_OK);
    lagfx_air2spv_set_simd_emulation(32); assert(lagfx_air2spv_translate_module(m, &e32, &n32) == LAGFX_OK);
    lagfx_air2spv_set_simd_emulation(4);  assert(lagfx_air2spv_translate_module(m, &e4, &n4) == LAGFX_OK);
    lagfx_air2spv_set_simd_emulation(0);  assert(lagfx_air2spv_translate_module(m, &nat2, &nn2) == LAGFX_OK);
    assert(nn == nn2 && !memcmp(nat, nat2, nn));
    assert(n32 > nn && (n32 != nn || memcmp(nat, e32, nn)) && n4 > nn);
    const char *names[3] = { "/tmp/lagfx-tile-e32.spv", "/tmp/lagfx-tile-e4.spv", "/tmp/lagfx-tile-nat.spv" };
    const uint8_t *bl[3] = { e32, e4, nat }; size_t ln[3] = { n32, n4, nn };
    if (system("spirv-val --version >/dev/null 2>&1") == 0) {
        for (int i = 0; i < 3; i++) {
            FILE *f = fopen(names[i], "wb"); assert(f); fwrite(bl[i], 1, ln[i], f); fclose(f);
            char cmd[256]; snprintf(cmd, sizeof cmd, "spirv-val --target-env vulkan1.3 %s", names[i]);
            assert(system(cmd) == 0);
            remove(names[i]);
        }
        puts("PASS: kernel translated for logical 32 / 4 lane simdgroups and natively is spirv-val clean; native output unchanged by the emulation setting");
    } else puts("PASS: emulated translations succeed and the native output is unchanged (spirv-val not installed: validity not checked)");
    free(nat); free(nat2); free(e32); free(e4);
    lagfx_air_module_free(m);
}

#ifdef LAGFX_HAVE_VULKAN
/* ---- BGRA8 imageblock: classification (pure) and translation (no device) ---- */
static uint64_t fnv64(const uint8_t *b, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull; return h; }
static void test_rt_classify(void) {
    const uint64_t RW = LAGFX_A64_TILE_F3_STORAGE_READ_NOFMT | LAGFX_A64_TILE_F3_STORAGE_WRITE_NOFMT;
    lagfx_a64_tile_rt_probe_t ok16 = { .present = true, .format = VK_FORMAT_R16G16B16A16_SFLOAT, .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .has_view = true };
    lagfx_a64_tile_rt_probe_t okb = { .present = true, .format = VK_FORMAT_B8G8R8A8_UNORM, .usage = VK_IMAGE_USAGE_STORAGE_BIT, .has_view = true, .nofmt_device = true, .f3_valid = true, .f3 = RW | 0x2000u };
    uint32_t why = 99;
    assert(LAGFX_A64_TILE_VKFMT_RGBA16F == (uint32_t)VK_FORMAT_R16G16B16A16_SFLOAT && LAGFX_A64_TILE_VKFMT_BGRA8 == (uint32_t)VK_FORMAT_B8G8R8A8_UNORM && LAGFX_A64_TILE_USAGE_STORAGE == (uint32_t)VK_IMAGE_USAGE_STORAGE_BIT);
    assert(LAGFX_A64_TILE_F3_STORAGE_READ_NOFMT == (uint64_t)VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT && LAGFX_A64_TILE_F3_STORAGE_WRITE_NOFMT == (uint64_t)VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT);
    assert(lagfx_a64_tile_rt_classify(&ok16, &why) == LAGFX_A64_TILE_RT_RGBA16F);                   /* the original path: no device feature needed */
    assert(lagfx_a64_tile_rt_classify(&okb, &why) == LAGFX_A64_TILE_RT_BGRA8);
    { lagfx_a64_tile_rt_probe_t sr = { .present = true, .format = VK_FORMAT_B8G8R8A8_UNORM, .usage = VK_IMAGE_USAGE_STORAGE_BIT, .has_view = true, .st_surr = true };   /* no without-format access, no Vulkan 1.3 */
      assert(lagfx_a64_tile_rt_classify(&sr, &why) == LAGFX_A64_TILE_RT_BGRA8_SURR);
      sr.usage = 0; assert(lagfx_a64_tile_rt_classify(&sr, &why) == 0 && why == 2);   /* the surrogate needs its storage usage too */ }
    lagfx_a64_tile_rt_probe_t r;
    r = ok16; r.present = false;                       assert(lagfx_a64_tile_rt_classify(&r, &why) == 0 && why == 0);
    r = ok16; r.format = VK_FORMAT_R8G8B8A8_UNORM;     assert(lagfx_a64_tile_rt_classify(&r, &why) == 0 && why == 1);   /* RGBA8 stays refused */
    r = okb;  r.format = VK_FORMAT_B8G8R8A8_SRGB;      assert(lagfx_a64_tile_rt_classify(&r, &why) == 0 && why == 1);
    r = ok16; r.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; assert(lagfx_a64_tile_rt_classify(&r, &why) == 0 && why == 2);
    r = okb;  r.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; assert(lagfx_a64_tile_rt_classify(&r, &why) == 0 && why == 2);
    r = ok16; r.has_view = false;                      assert(lagfx_a64_tile_rt_classify(&r, &why) == 0 && why == 3);
    r = okb;  r.linear = true;                         assert(lagfx_a64_tile_rt_classify(&r, &why) == 0 && why == 3);
    r = okb;  r.nofmt_device = false;                  assert(lagfx_a64_tile_rt_classify(&r, &why) == 0 && why == 4);
    r = okb;  r.f3_valid = false;                      assert(lagfx_a64_tile_rt_classify(&r, &why) == 0 && why == 5);   /* pre-1.3: cannot ask the format */
    r = okb;  r.f3 = LAGFX_A64_TILE_F3_STORAGE_READ_NOFMT;  assert(lagfx_a64_tile_rt_classify(&r, &why) == 0 && why == 5);   /* read only */
    r = okb;  r.f3 = LAGFX_A64_TILE_F3_STORAGE_WRITE_NOFMT; assert(lagfx_a64_tile_rt_classify(&r, &why) == 0 && why == 5);   /* write only */
    puts("PASS: imageblock attachment classes: RGBA16F accepted as before, BGRA8 accepted only with storage usage + view + device feature + format without-format bits, RGBA8 / sRGB refused, each cause distinct");
}

#include "air2spv/translate_function.h"
/* the OpTypeImage format operands (Dim 2D storage images) and OpCapability values of a SPIR-V module */
static void spv_scan(const uint8_t *spv, size_t n, uint32_t *fmt_rgba16f, uint32_t *fmt_unknown, uint32_t *cap55, uint32_t *cap56) {
    *fmt_rgba16f = *fmt_unknown = *cap55 = *cap56 = 0;
    for (size_t w = 5; (w + 1) * 4u <= n;) {
        uint32_t ins = lagfx_le32(spv + w * 4u), op = ins & 0xffffu, wc = ins >> 16;
        if (!wc || (w + wc) * 4u > n) break;
        if (op == 25 && wc >= 9u && lagfx_le32(spv + (w + 7) * 4u) == 2u) { uint32_t f = lagfx_le32(spv + (w + 8) * 4u); if (f == 2u) (*fmt_rgba16f)++; else if (f == 0u) (*fmt_unknown)++; }   /* OpTypeImage ... Sampled=2 Format */
        if (op == 17 && wc >= 2u) { uint32_t c = lagfx_le32(spv + (w + 1) * 4u); if (c == 55u) (*cap55)++; if (c == 56u) (*cap56)++; }
        w += wc;
    }
}
static void test_translate_bgra(const uint8_t *bc, size_t bn) {
    lagfx_air_module_t *m = NULL;
    assert(lagfx_air_module_open(bc, bn, &m) == LAGFX_OK && m);
    uint8_t *nat = NULL, *bg = NULL, *nat2 = NULL; size_t nn = 0, bgn = 0, nn2 = 0;
    assert(lagfx_air2spv_translate_module(m, &nat, &nn) == LAGFX_OK);
    /* the Rgba16f translation is byte-for-byte what the base commit 30ccf5e produced (FNV-1a of tile_simd_blur_15, measured there) */
    assert(nn == 4100u && fnv64(nat, nn) == 0x964e8c4e55e4c739ull);
    lagfx_air2spv_set_imageblock_unformatted(1u);
    assert(lagfx_air2spv_translate_module(m, &bg, &bgn) == LAGFX_OK);
    lagfx_air2spv_set_imageblock_unformatted(0u);
    assert(lagfx_air2spv_translate_module(m, &nat2, &nn2) == LAGFX_OK && nn2 == nn && !memcmp(nat, nat2, nn));   /* the setting is per call: back to the original output */
    uint32_t a, b, c55, c56;
    spv_scan(nat, nn, &a, &b, &c55, &c56);   assert(a == 1u && b == 0u && c55 == 0u && c56 == 0u);   /* original: one Rgba16f image, no without-format capabilities */
    spv_scan(bg, bgn, &a, &b, &c55, &c56);   assert(a == 0u && b == 1u && c55 == 1u && c56 == 1u);   /* variant: one Unknown-format image, Read + Write WithoutFormat */
    assert(bgn != nn || memcmp(bg, nat, nn));
    if (system("spirv-val --version >/dev/null 2>&1") == 0) {
        const char *nm = "/tmp/lagfx-tile-bgra.spv";
        FILE *f = fopen(nm, "wb"); assert(f); fwrite(bg, 1, bgn, f); fclose(f);
        char cmd[256]; snprintf(cmd, sizeof cmd, "spirv-val --target-env vulkan1.3 %s", nm);
        assert(system(cmd) == 0);
        remove(nm);
        puts("PASS: BGRA8 imageblock translation: Image Format Unknown + StorageImageRead/WriteWithoutFormat, spirv-val (vulkan1.3) clean; the Rgba16f translation is byte-identical to the base commit's (hash) and unaffected by the setting");
    } else puts("PASS: BGRA8 imageblock translation structure checked, Rgba16f output byte-identical to the base commit's (spirv-val not installed: validity not checked)");
    free(nat); free(nat2); free(bg);
    lagfx_air_module_free(m);
}
#endif

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
    test_translate_widths(bc, bn);
    test_rt_classify();
    test_translate_bgra(bc, bn);
    test_tile(lib, ln, 0);
    /* forced logical simdgroups narrower than the device subgroup: the same group-boundary logic as 32 lanes inside a 64-wide Adreno subgroup (a logical group is the lower /
     * upper part of the physical subgroup), run on lavapipe's real 32-wide one */
    for (uint32_t w = vk->sg_size >> 1; w >= 4u && !(w & (w - 1u)); w >>= 1) test_tile(lib, ln, w);
    test_tile_unsupported_simd(vk);
    free(lib); free(bc);
#else
    puts("SKIP: built without Vulkan");
#endif
    lagfx_device_free(g_dev);
    free(g_mem);
    return 0;
}
