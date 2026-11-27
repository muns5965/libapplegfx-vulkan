/* ARM64 scratch-slot sharing between the tile path (skip_unaligned gathers in arm64_tile.c) and the graphics path (gather_copy in arm64_exec_run.c) on lavapipe.
 * Both take their private aligned copies of unaligned storage-buffer ranges from the same per-exec pool (MAX_TMP = 64 slots). ONE exec here holds graphics passes (textured
 * quads whose position buffer is bound at an offset the device cannot bind) and a tile pass (real tile_simd_blur_15, region dispatches, one tile each, its two buffers bound
 * at unaligned offsets): under the cap, exactly at the cap, over the cap (the failing draws / dispatches are skipped and counted, every earlier one still renders correct
 * data: each draw owns a cell of its target and each dispatch a tile, checked against an expected map from a sequential model of the slot pool), and slot reuse by the next execs.
 * NOT verified on a hardware GPU. SPDX-License-Identifier: MIT */
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
#define NVPN       48u

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
#define VPN_LIB 6u        /* the tile kernel library */
#define VPN_LIB_VERT 12u  /* synthetic MTLB: ViewportToNDC vertex */
#define VPN_LIB_FRAG 13u  /* synthetic MTLB: TextureCopy fragment */
#define VPN_POS 16u
#define VPN_UV  17u
#define VPN_MAT 18u
#define VPN_UBO 9u
#define VPN_WBUF 10u
#define VPN_SRC 30u       /* up to 12 pages: the tile source texture */
#define REF_SRC 1u
#define REF_DST 2u
#define REF_UBO 3u
#define REF_WBUF 4u
#define REF_FN 5u
#define REF_TILE 6u
#define REF_GTEX 20u      /* graphics: sampled texture (cleared green) */
#define REF_GRT_A 21u     /* graphics render targets, 64 x 64, one 4 x 4 cell per draw */
#define REF_GRT_B 22u
#define REF_PIPE_TEX 23u
#define REF_FN_VERT 24u
#define REF_FN_FRAG 25u
#define REF_UV  26u
#define REF_MAT 27u
#define REF_GMS 60u       /* 4x multisampled BGRA8 target, 64 x 64 (freshness tests) */
#define REF_POS0 28u      /* NPOS position buffer objects, all over the same guest page (distinct objects = distinct gather cache generations) */
#define NPOS 12u
#define GRT_DIM 64u
#define CELL 4u
#define CELLS_ROW (GRT_DIM / CELL)
#define TILES_X 8u        /* tile target: TILES_X x TILES_Y tiles of (S, 4) */
#define TILES_Y 8u
#define MAX_TMP_SLOTS 64u /* MAX_TMP of arm64_resources.c (private there) */

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

static uint8_t g_cb[32768];
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


/* A serialized graphics pipeline: {0xe, size, ref, length, body}; body = the root entry (tag 1 / tag 2 = function refs, as in arm64-pipe-unit.c). */
static void put_pipeline(uint32_t ref, uint32_t pay_off, const uint32_t *fns, uint32_t nfn) {
    uint8_t b[256]; memset(b, 0, sizeof b);
    uint32_t o = 16;
    b[o++] = (uint8_t)(nfn + 1u);   /* root entry: tag 1 = vertex function, tag 2 = fragment function, tag 8 = a value that names no function */
    for (uint32_t i = 0; i < nfn; i++) { b[o++] = (uint8_t)(1u + i); b[o++] = 0x04; memcpy(b + o, &fns[i], 4); o += 4; }
    b[o++] = 0x08; b[o++] = 0x04; { uint32_t salt = 2; memcpy(b + o, &salt, 4); o += 4; }
    uint32_t tag = 0xe, len = o - 16;
    memcpy(b, &tag, 4); memcpy(b + 4, &o, 4); memcpy(b + 8, &ref, 4); memcpy(b + 12, &len, 4);
    va_write(VA_PAY + pay_off, b, o);
    entry(ref, LAGFX_A64_OBJ_SERIALIZED, o, VA_PAY + pay_off);
}
static void cb_viewport(double x, double y, double w, double h) { double v[6] = { x, y, w, h, 0, 1 }; cb_cmd(130, v, sizeof v); }
static void cb_bufs(uint32_t tag, uint32_t index, const uint32_t *refs, uint32_t n) {
    uint8_t r[8 + 12 * 8] = {0};
    memcpy(r, &index, 4); memcpy(r + 4, &n, 4);
    for (uint32_t i = 0; i < n; i++) memcpy(r + 8 + 12 * i, &refs[i], 4);
    cb_cmd(tag, r, 8 + 12 * n);
}
static void cb_refs(uint32_t tag, uint32_t index, const uint32_t *refs, uint32_t n) {
    uint8_t r[8 + 4 * 8] = {0};
    memcpy(r, &index, 4); memcpy(r + 4, &n, 4);
    for (uint32_t i = 0; i < n; i++) memcpy(r + 8 + 4 * i, &refs[i], 4);
    cb_cmd(tag, r, 8 + 4 * n);
}
static void cb_buf_offset(uint32_t index, uint64_t off) { uint8_t r[12] = {0}; memcpy(r, &index, 4); memcpy(r + 4, &off, 8); cb_cmd(126, r, 12); }
static void cb_draw(uint32_t prim, uint32_t start, uint32_t count) { uint8_t r[8]; uint16_t s = (uint16_t)start, c = (uint16_t)count; memcpy(r, &prim, 4); memcpy(r + 4, &s, 2); memcpy(r + 6, &c, 2); cb_cmd(1, r, 8); }

static const double G4[4] = { 0, 1, 0, 1 }, B4[4] = { 0, 0, 1, 1 };
#define GREEN 0xff00ff00u
#define BLUE  0xff0000ffu

#ifdef LAGFX_HAVE_VULKAN
#define TILE(p) ((p)->diag2.exec.tile)
#define RUNC(p) ((p)->diag2.exec.run)
#define PIPE(p) ((p)->diag2.exec.pipe)
#define SENT 3.0f

static uint32_t g_S, g_W, g_H, g_TW, g_TH;
static float g_w[15];
static uint8_t *g_vlib, *g_flib; static size_t g_vlib_n, g_flib_n;
/* byte offsets inside the position page that hold a copy of the quad (4 floats pairs = 32 bytes): residues 4 / 8 / 12 modulo any alignment >= 16, pairwise distances 36 / 72 / 36
 * that are not multiples of it, so the three binds of one object never share a gather copy */
static const uint64_t POS_OFFS[3] = { 4, 40, 76 };
#define UBO_UNAL 20u      /* a second copy of the uniforms / weights at an offset the device cannot bind */
#define WBUF_UNAL 36u

static void build_objects(const uint8_t *lib, size_t libn) {
    g_W = TILES_X * g_S; g_H = TILES_Y * 4u; g_TW = g_W + g_S + 4u; g_TH = g_H;
    put_function(REF_FN, 0x400, VPN_LIB, lib, libn);
    put_tile_pipeline(REF_TILE, 0x500, REF_FN);
    put_texture(REF_SRC, 0x000, g_TW, g_TH, 115, 8, VPN_SRC);
    put_texture(REF_DST, 0x100, g_W, g_H, 115, 8, 0);
    put_buffer_object(REF_UBO, 0x300, 0x4000, VPN_UBO);
    put_buffer_object(REF_WBUF, 0x320, 0x4000, VPN_WBUF);
    uint16_t *tp = calloc((size_t)g_TW * g_TH * 4, 2);
    for (uint32_t y = 0; y < g_TH; y++) for (uint32_t x = 0; x < g_TW; x++) for (int ch = 0; ch < 4; ch++) tp[((size_t)y * g_TW + x) * 4 + ch] = f2h(texv(x, y, ch));
    va_write((uint64_t)VPN_SRC << 14, tp, g_TW * g_TH * 8);
    free(tp);
    uint16_t u[8] = { 0, 0, (uint16_t)g_S, 0, 1, f2h(0.5f), 0, 0 };   /* { src_offset, fill_offset, horizontal, edr_scale } */
    va_write((uint64_t)VPN_UBO << 14, u, sizeof u); va_write(((uint64_t)VPN_UBO << 14) + UBO_UNAL, u, sizeof u);
    uint16_t w16[16] = {0};
    for (int i = 0; i < 15; i++) { g_w[i] = ((uint32_t)i <= g_S) ? (float)(i % 4 + 1) / 8.0f : 0.0f; w16[i] = f2h(g_w[i]); }
    va_write((uint64_t)VPN_WBUF << 14, w16, sizeof w16); va_write(((uint64_t)VPN_WBUF << 14) + WBUF_UNAL, w16, sizeof w16);
    /* graphics */
    put_texture(REF_GTEX, 0x800, GRT_DIM, GRT_DIM, 80, 4, 0);
    put_texture(REF_GRT_A, 0x880, GRT_DIM, GRT_DIM, 80, 4, 0);
    put_texture(REF_GRT_B, 0x900, GRT_DIM, GRT_DIM, 80, 4, 0);
    put_function(REF_FN_VERT, 0x980, VPN_LIB_VERT, g_vlib, g_vlib_n);
    put_function(REF_FN_FRAG, 0xa00, VPN_LIB_FRAG, g_flib, g_flib_n);
    const uint32_t tx[2] = { REF_FN_VERT, REF_FN_FRAG };
    put_pipeline(REF_PIPE_TEX, 0xa80, tx, 2);
    const float pos[8] = { -1, -1, 1, -1, -1, 1, 1, 1 }, uv[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    const float mat[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 0, 0,  0, 0, 0, 1 };
    for (uint32_t k = 0; k < 3; k++) va_write(((uint64_t)VPN_POS << 14) + POS_OFFS[k], pos, sizeof pos);   /* the quad at each bind offset; offset 0 holds zeros */
    va_write((uint64_t)VPN_UV << 14, uv, sizeof uv); va_write((uint64_t)VPN_MAT << 14, mat, sizeof mat);
    put_buffer_object(REF_UV, 0xb00, 0x4000, VPN_UV);
    put_buffer_object(REF_MAT, 0xb20, 0x4000, VPN_MAT);
    for (uint32_t i = 0; i < NPOS; i++) put_buffer_object(REF_POS0 + i, 0xb40 + 0x20 * i, 0x4000, VPN_POS);
}
static float model(uint32_t x, uint32_t y, int ch) {
    float acc = 0;
    for (uint32_t i = 0; i <= g_S && i < 15; i++) acc += g_w[i] * texv(x + i, y, ch);
    return ch < 3 ? acc * 0.5f : acc;
}
static lagfx_protocol_t *new_task(lagfx_task_entry_t **t, const uint8_t *lib, size_t libn) {
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    *t = def_task(p, 1);
    build_objects(lib, libn);
    return p;
}
static void exec_all(lagfx_protocol_t *p, uint32_t task, uint32_t len) {
    uint32_t refs[10 + NPOS], flags[10 + NPOS], nr = 0;
    const uint32_t fixed[] = { REF_SRC, REF_DST, REF_UBO, REF_WBUF, REF_GTEX, REF_GRT_A, REF_GRT_B, REF_PIPE_TEX, REF_UV, REF_MAT };
    for (uint32_t i = 0; i < sizeof fixed / sizeof *fixed; i++) { refs[nr] = fixed[i]; flags[nr++] = (fixed[i] == REF_SRC || fixed[i] == REF_UBO || fixed[i] == REF_WBUF) ? 1u : 0u; }
    for (uint32_t i = 0; i < NPOS; i++) { refs[nr] = REF_POS0 + i; flags[nr++] = 0; }
    uint32_t w[3 + 2 * (10 + NPOS) + 4], n = 0;
    w[n++] = task; w[n++] = nr; w[n++] = 1;
    for (uint32_t i = 0; i < nr; i++) { w[n++] = refs[i]; w[n++] = flags[i]; }
    uint64_t off = VA_CMD, l = len;
    memcpy(w + n, &off, 8); n += 2; memcpy(w + n, &l, 8); n += 2;
    submit(p, 0x43, 0, w, n);
}

static bool g_tile_ok[TILES_X * TILES_Y];
static bool pix_computed(uint32_t x, uint32_t y) { uint32_t k = (y / 4u) * TILES_X + x / g_S; return k < TILES_X * TILES_Y && g_tile_ok[k]; }
static unsigned compare_tiles(lagfx_protocol_t *p, lagfx_task_entry_t *t) {
    uint16_t *img = malloc((size_t)g_W * g_H * 8);
    uint32_t w, h;
    assert(lagfx_a64_exec_readback_texture(p, t, REF_DST, img, (size_t)g_W * g_H * 8, &w, &h) && w == g_W && h == g_H);
    unsigned bad = 0;
    for (uint32_t y = 0; y < g_H; y++) for (uint32_t x = 0; x < g_W; x++) {
        bool c = pix_computed(x, y);
        for (int ch = 0; ch < 4; ch++) {
            float got = h2f(img[((size_t)y * g_W + x) * 4 + ch]), want = c ? model(x, y, ch) : SENT;
            if (got != want && bad++ < 6) printf("  tile mismatch (%u,%u).%d %s: got %g expected %g\n", x, y, ch, c ? "computed" : "kept", got, want);
        }
    }
    free(img);
    return bad;
}
/* every 4 x 4 cell of the target: green when its draw rendered, the clear (blue) otherwise or when no draw owns it */
static unsigned compare_cells(lagfx_protocol_t *p, lagfx_task_entry_t *t, uint32_t ref, const bool *ok, uint32_t n) {
    uint32_t img[GRT_DIM * GRT_DIM];
    assert(lagfx_a64_exec_readback_texture(p, t, ref, img, sizeof img, NULL, NULL));
    unsigned bad = 0;
    for (uint32_t y = 0; y < GRT_DIM; y++) for (uint32_t x = 0; x < GRT_DIM; x++) {
        uint32_t k = (y / CELL) * CELLS_ROW + x / CELL;
        uint32_t want = (k < n && ok[k]) ? GREEN : BLUE;
        if (img[y * GRT_DIM + x] != want && bad++ < 6) printf("  cell mismatch (%u,%u) cell %u: got %08x expected %08x\n", x, y, k, img[y * GRT_DIM + x], want);
    }
    return bad;
}

typedef struct { const char *name; uint32_t g1, t, g2, spt; } mix_t;   /* draws before the tile pass, tile dispatches, draws after it, slots per tile dispatch (1 or 2) */

/* One exec: [clear GTEX] [pass A: g1 draws] [tile pass: t dispatches] [pass B: g2 draws]. Draw j (over A then B) binds position object j / 3 at offset POS_OFFS[j % 3].
 * The expected outcome follows from a sequential model of the exec's pool: 64 slots shared; graphics copies also limited to 32 per exec (A64_GATHER_CACHE, cause 2, checked
 * before the pool, cause 3). A tile dispatch takes its slots one bind after the other: if the pool runs out in between, the first copy stays allocated and the dispatch is skipped. */
static void run_mix(lagfx_protocol_t *p, lagfx_task_entry_t *t, const mix_t *m, uint32_t *slots_used_out) {
    assert(m->g1 + m->g2 <= 3u * NPOS && m->t <= TILES_X * TILES_Y && (m->spt == 1u || m->spt == 2u) && m->g1 <= CELLS_ROW * CELLS_ROW && m->g2 <= CELLS_ROW * CELLS_ROW);
    static const double sent[4] = { SENT, SENT, SENT, SENT };
    const uint32_t tex[1] = { REF_GTEX };
    const uint32_t vb[3] = { REF_POS0, REF_UV, REF_MAT };
    uint32_t j = 0;
    cb_reset();
    cb_pass(REF_GTEX, 2, G4);
    for (int ph = 0; ph < 3; ph++) {
        if (ph == 1) {   /* the tile pass */
            cb_pass(REF_DST, 2, sent);
            cb_pipeline(REF_TILE); cb_tile_buf(0, REF_UBO, UBO_UNAL); cb_tile_buf(1, REF_WBUF, m->spt == 2u ? WBUF_UNAL : 0u); cb_tile_tex(0, REF_SRC);
            for (uint32_t k = 0; k < m->t; k++) cb_dispatch_region(g_S, 4, (uint64_t)(k % TILES_X) * g_S, (uint64_t)(k / TILES_X) * 4u, g_S, 4);
            continue;
        }
        uint32_t n = ph == 0 ? m->g1 : m->g2;
        cb_pass(ph == 0 ? REF_GRT_A : REF_GRT_B, 2, B4); cb_pipeline(REF_PIPE_TEX); cb_bufs(125, 0, vb, 3); cb_refs(114, 0, tex, 1);
        for (uint32_t c = 0; c < n; c++, j++) {
            uint32_t pr = REF_POS0 + j / 3u;
            cb_bufs(125, 0, &pr, 1); cb_buf_offset(0, POS_OFFS[j % 3u]);
            cb_viewport((c % CELLS_ROW) * CELL, (c / CELLS_ROW) * CELL, CELL, CELL);
            cb_draw(4, 0, 4);
        }
    }
    /* the model */
    bool gok[2][CELLS_ROW * CELLS_ROW] = {{0}};
    uint32_t slots = 0, ngc = 0, exp_gfx = 0, exp_gfx_f = 0, why2 = 0, why3 = 0, exp_tg = 0, exp_tf = 0, exp_tskip = 0, exp_td = 0;
    memset(g_tile_ok, 0, sizeof g_tile_ok);
    for (int ph = 0; ph < 3; ph++) {
        if (ph == 1) {
            for (uint32_t k = 0; k < m->t; k++) {
                bool ok = true;
                for (uint32_t b = 0; b < m->spt; b++) {
                    if (slots < MAX_TMP_SLOTS) { slots++; exp_tg++; } else { exp_tf++; ok = false; break; }
                }
                if (ok) exp_td++, g_tile_ok[k] = true; else exp_tskip++;
            }
            continue;
        }
        uint32_t n = ph == 0 ? m->g1 : m->g2;
        for (uint32_t c = 0; c < n; c++) {
            if (ngc >= 32u) { why2++; exp_gfx_f++; }
            else if (slots >= MAX_TMP_SLOTS) { why3++; exp_gfx_f++; }
            else { slots++; ngc++; exp_gfx++; gok[ph == 0 ? 0 : 1][c] = true; }
        }
    }
    uint64_t tg0 = TILE(p).gathered, tf0 = TILE(p).gather_failed, ts0 = TILE(p).skip_unaligned, td0 = TILE(p).dispatches, tw0 = TILE(p).unaligned_written;
    uint64_t rg0 = RUNC(p).ext_gathered, rf0 = RUNC(p).ext_gather_failed, rr0 = RUNC(p).ext_gather_reused, w2 = RUNC(p).ext_gather_why[2], w3 = RUNC(p).ext_gather_why[3];
    uint64_t wo0 = RUNC(p).ext_gather_why[0] + RUNC(p).ext_gather_why[1] + RUNC(p).ext_gather_why[4];
    uint64_t dr0 = RUNC(p).draws, du0 = PIPE(p).desc_unaligned, sf0 = RUNC(p).submit_failed;
    exec_all(p, 1, cb_finish());
    printf("INFO: %s: gfx %u ok / %u failed (count cap %u, pool %u), tile dispatches %u ok / %u skipped, tile copies %u made / %u refused, pool slots used %u\n", m->name,
           exp_gfx, exp_gfx_f, why2, why3, exp_td, exp_tskip, exp_tg, exp_tf, slots);
    assert(slots <= MAX_TMP_SLOTS);
    assert(RUNC(p).submit_failed == sf0);
    assert(RUNC(p).ext_gathered - rg0 == exp_gfx && RUNC(p).ext_gather_failed - rf0 == exp_gfx_f && RUNC(p).ext_gather_reused == rr0);
    assert(RUNC(p).ext_gather_why[2] - w2 == why2 && RUNC(p).ext_gather_why[3] - w3 == why3);
    assert(RUNC(p).ext_gather_why[0] + RUNC(p).ext_gather_why[1] + RUNC(p).ext_gather_why[4] == wo0);   /* no other cause */
    assert(RUNC(p).draws - dr0 == exp_gfx && PIPE(p).desc_unaligned - du0 == exp_gfx_f);
    assert(TILE(p).gathered - tg0 == exp_tg && TILE(p).gather_failed - tf0 == exp_tf && TILE(p).skip_unaligned - ts0 == exp_tskip && TILE(p).dispatches - td0 == exp_td && TILE(p).unaligned_written == tw0);
    /* the pool is shared: the copies of both paths together never exceed it */
    assert((RUNC(p).ext_gathered - rg0) + (TILE(p).gathered - tg0) == slots);
    assert(compare_cells(p, t, REF_GRT_A, gok[0], m->g1) == 0);
    assert(compare_cells(p, t, REF_GRT_B, gok[1], m->g2) == 0);
    assert(compare_tiles(p, t) == 0);
    if (slots_used_out) *slots_used_out = slots;
}

static void test_mix(const uint8_t *lib, size_t libn) {
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    struct lagfx_vk_state *vk = g_dev->vk;
    bool wide = vk->sg_size > 32u && !(vk->sg_size % 32u) && vk->sg_min == vk->sg_max;
    g_S = (lagfx_vk_can_pin_subgroup32(vk) || wide) ? 32u : vk->sg_size;
    if (g_S < 4 || g_S > 64) { printf("SKIP: subgroup size %u not handled by this test\n", g_S); return; }
    setenv("LAGFX_TILE_ANY_SUBGROUP", "1", 1);   /* a device that cannot be pinned runs the kernel with its own subgroup size, as arm64-tile-unit does */
    if (vk->phys_props.limits.minStorageBufferOffsetAlignment < 16u) { puts("SKIP: gather mix (the device binds storage buffers at any 4-aligned offset, or at 8: the test needs offsets that cannot share a copy)"); return; }
    unsetenv("LAGFX_A64_GATHER_BUDGET"); unsetenv("LAGFX_A64_GATHER_CACHE");
    lagfx_task_entry_t *t; lagfx_protocol_t *p = new_task(&t, lib, libn);
    uint32_t used;

    /* (a) mixed use, well under the cap: 6 draws, 4 dispatches (2 copies each), 5 draws = 19 slots; every cell and tile correct */
    run_mix(p, t, &(mix_t){ "(a) under the cap", 6, 4, 5, 2 }, &used);
    assert(used == 19u);
    puts("PASS: one exec with graphics gathers and tile gathers under the slot cap: all 11 draws and 4 tile dispatches rendered correct data");

    /* (b) exactly at the cap: 12 + 20 x 2 + 12 = 64 slots (and 32 slots with 1-copy dispatches: 16 + 32 + 16, which also fills the 32 graphics copies exactly) */
    run_mix(p, t, &(mix_t){ "(b1) exactly 64 slots", 12, 20, 12, 2 }, &used);
    assert(used == MAX_TMP_SLOTS && TILE(p).gather_failed == 0 && RUNC(p).ext_gather_failed == 0);
    run_mix(p, t, &(mix_t){ "(b2) exactly 64 slots, 32 graphics copies, 1-copy dispatches", 16, 32, 16, 1 }, &used);
    assert(used == MAX_TMP_SLOTS && TILE(p).gather_failed == 0 && RUNC(p).ext_gather_failed == 0);
    puts("PASS: exactly 64 shared slots used (two compositions): nothing fails, every draw and tile correct");

    /* (c) over the cap */
    uint64_t sk = TILE(p).skip_unaligned, gf = RUNC(p).ext_gather_failed;
    run_mix(p, t, &(mix_t){ "(c1) over the cap, graphics run out last", 12, 22, 12, 2 }, &used);
    assert(used == MAX_TMP_SLOTS && RUNC(p).ext_gather_failed - gf == 4 && TILE(p).skip_unaligned == sk);   /* 8 of the last 12 draws got a copy, 4 did not */
    sk = TILE(p).skip_unaligned; gf = RUNC(p).ext_gather_failed;
    run_mix(p, t, &(mix_t){ "(c2) over the cap, tile runs out mid-dispatch (63 used)", 15, 26, 10, 2 }, &used);
    assert(used == MAX_TMP_SLOTS && TILE(p).skip_unaligned - sk == 2 && RUNC(p).ext_gather_failed - gf == 10);   /* dispatch 25 got one copy and failed on its second, 26 none, every late draw failed */
    gf = RUNC(p).ext_gather_failed;
    run_mix(p, t, &(mix_t){ "(c3) graphics copy-count cap (32) with the pool not full", 18, 4, 18, 1 }, &used);
    assert(used == 36u && RUNC(p).ext_gather_failed - gf == 4);
    puts("PASS: over the cap: the draws / dispatches that find no slot are skipped and counted (graphics cause 3 or 2, tile skip_unaligned), no overrun, every earlier gather still correct");

    /* (d) the slots are per exec: after the over-cap execs a full-pool exec succeeds again, twice in a row, and so does a light one */
    gf = RUNC(p).ext_gather_failed; sk = TILE(p).skip_unaligned;
    run_mix(p, t, &(mix_t){ "(d1) full pool again after an over-cap exec", 12, 20, 12, 2 }, &used);
    run_mix(p, t, &(mix_t){ "(d2) and again", 12, 20, 12, 2 }, &used);
    run_mix(p, t, &(mix_t){ "(d3) light exec", 3, 2, 3, 2 }, &used);
    assert(used == 10u && RUNC(p).ext_gather_failed == gf && TILE(p).skip_unaligned == sk);
    puts("PASS: scratch slots are released per exec: consecutive full-pool and light execs each get the whole pool");

    lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); pfree(p);
}

/* ---- freshness of a gathered copy against GPU writes recorded earlier in the SAME exec ----
 * The gather snapshot is taken while commands are recorded; a blit fill / copy recorded earlier in the exec has not run yet, so a host (memcpy) copy would hold the OLD bytes.
 * One exec = [render seg: pass A, one draw whose position buffer is bound at an unaligned offset] [blit seg: a GPU write to that buffer] [render seg: pass B, the same draw].
 * The write zeroes the quad, so a fresh copy draws nothing in pass B (cell stays blue) and a stale one draws the quad (green). Pass A always draws it. */
/* every exec buffer is one segment: {off, len} each, at its own address */
static uint64_t g_segoff[8], g_seglen[8]; static uint32_t g_nseg;
static void seg_begin(void) { cb_reset(); }
static void seg_end(uint8_t type, uint8_t cont) {
    uint32_t seglen = g_cbn, info = (uint32_t)type | ((uint32_t)cont << 8);
    memcpy(g_cb, &seglen, 4); memcpy(g_cb + 4, &info, 4);
    assert(g_nseg < 8u);
    g_segoff[g_nseg] = VA_CMD + 0x2000ull * g_nseg, g_seglen[g_nseg] = g_cbn;
    va_write(g_segoff[g_nseg], g_cb, g_cbn); g_nseg++;
}
static void exec_segs(lagfx_protocol_t *p, uint32_t task) {
    uint32_t refs[11 + NPOS], flags[11 + NPOS], nr = 0;
    const uint32_t fixed[] = { REF_SRC, REF_DST, REF_UBO, REF_WBUF, REF_GTEX, REF_GRT_A, REF_GRT_B, REF_PIPE_TEX, REF_UV, REF_MAT, REF_GMS };
    for (uint32_t i = 0; i < sizeof fixed / sizeof *fixed; i++) { refs[nr] = fixed[i]; flags[nr++] = (fixed[i] == REF_SRC || fixed[i] == REF_UBO || fixed[i] == REF_WBUF) ? 1u : 0u; }
    for (uint32_t i = 0; i < NPOS; i++) { refs[nr] = REF_POS0 + i; flags[nr++] = 0; }
    uint32_t w[3 + 2 * (11 + NPOS) + 4 * 8], n = 0;
    w[n++] = task; w[n++] = nr; w[n++] = g_nseg;
    for (uint32_t i = 0; i < nr; i++) { w[n++] = refs[i]; w[n++] = flags[i]; }
    for (uint32_t i = 0; i < g_nseg; i++) { memcpy(w + n, &g_segoff[i], 8); n += 2; memcpy(w + n, &g_seglen[i], 8); n += 2; }
    submit(p, 0x43, 0, w, n);
}
static void blit_fill(uint32_t buf, uint64_t loc, uint64_t len, uint8_t v) {
    uint8_t r[0x18]; memset(r, 0, sizeof r);
    memcpy(r, &buf, 4); memcpy(r + 4, &loc, 8); memcpy(r + 12, &len, 8); r[20] = v;
    cb_cmd(306, r, sizeof r);
}
static void blit_copy(uint32_t a, uint32_t b, uint64_t so, uint64_t dof, uint64_t n) {
    uint8_t r[0x20]; memset(r, 0, sizeof r);
    memcpy(r, &a, 4); memcpy(r + 4, &b, 4); memcpy(r + 8, &so, 8); memcpy(r + 0x10, &dof, 8); memcpy(r + 0x18, &n, 8);
    cb_cmd(301, r, sizeof r);
}
static void fresh_draw_pass(uint32_t grt, uint32_t posref, uint64_t off) {
    const uint32_t tex[1] = { REF_GTEX }, vb[3] = { REF_POS0, REF_UV, REF_MAT };
    cb_pass(grt, 2, B4); cb_pipeline(REF_PIPE_TEX); cb_bufs(125, 0, vb, 3); cb_refs(114, 0, tex, 1);
    cb_bufs(125, 0, &posref, 1); cb_buf_offset(0, off);
    cb_viewport(0, 0, CELL, CELL); cb_draw(4, 0, 4);
}
/* mode 0: no write between the draws (the copy must be reused: ONE gather, quad drawn twice); 1: blit fill zeroes the quad; 2: blit copy of zeros over the quad */
static void run_fresh(lagfx_protocol_t *p, lagfx_task_entry_t *t, int mode, const char *name) {
    const uint64_t off = POS_OFFS[0];
    g_nseg = 0;
    seg_begin(); cb_pass(REF_GTEX, 2, G4); fresh_draw_pass(REF_GRT_A, REF_POS0, off); seg_end(0, 0);
    if (mode == 1) { seg_begin(); blit_fill(REF_POS0, 0, 0x100, 0); seg_end(2, 0); }
    if (mode == 2) { seg_begin(); blit_copy(REF_UV, REF_POS0, 0x1000, off, 32); seg_end(2, 0); }
    seg_begin(); fresh_draw_pass(REF_GRT_B, REF_POS0, off); seg_end(0, 0);
    uint64_t rg0 = RUNC(p).ext_gathered, rf0 = RUNC(p).ext_gather_failed, sf0 = RUNC(p).submit_failed;
    exec_segs(p, 1);
    assert(RUNC(p).submit_failed == sf0 && RUNC(p).ext_gather_failed == rf0);
    uint32_t a[GRT_DIM * GRT_DIM], b[GRT_DIM * GRT_DIM];
    assert(lagfx_a64_exec_readback_texture(p, t, REF_GRT_A, a, sizeof a, NULL, NULL));
    assert(lagfx_a64_exec_readback_texture(p, t, REF_GRT_B, b, sizeof b, NULL, NULL));
    uint32_t wantB = mode == 0 ? GREEN : BLUE;
    printf("INFO: %s: pass A cell0 %08x, pass B cell0 %08x (want %08x), gathers made %llu\n", name, a[0], b[0], wantB, (unsigned long long)(RUNC(p).ext_gathered - rg0));
    assert(a[0] == GREEN);
    if (b[0] != wantB) { printf("FAIL: %s: the second draw used a stale gather copy (pass B cell0 %08x, expected %08x)\n", name, b[0], wantB); exit(1); }
    if (mode == 0) assert(RUNC(p).ext_gathered - rg0 == 1u);   /* no write in between: the copy of the first draw serves the second */
}
/* The tile path: a blit fill zeroes the scale of the uniforms (the half at byte 10, 0.5 before) in the unaligned copy at UBO_UNAL, then one tile is dispatched whose uniform buffer is bound
 * there. A fresh gather gives colour channels 0 (acc * 0); a copy of the old host bytes gives the model value (acc * 0.5). */
static void run_fresh_tile(lagfx_protocol_t *p, lagfx_task_entry_t *t) {
    static const double sent[4] = { SENT, SENT, SENT, SENT };
    g_nseg = 0;
    seg_begin(); blit_fill(REF_UBO, UBO_UNAL + 10u, 2, 0); seg_end(2, 0);
    seg_begin(); cb_pass(REF_DST, 2, sent); cb_pipeline(REF_TILE); cb_tile_buf(0, REF_UBO, UBO_UNAL); cb_tile_buf(1, REF_WBUF, 0); cb_tile_tex(0, REF_SRC);
    cb_dispatch_region(g_S, 4, 0, 0, g_S, 4); seg_end(0, 0);
    uint64_t tg0 = TILE(p).gathered, tf0 = TILE(p).gather_failed, td0 = TILE(p).dispatches, gg0 = TILE(p).gather_gpu, sf0 = RUNC(p).submit_failed;
    exec_segs(p, 1);
    assert(RUNC(p).submit_failed == sf0 && TILE(p).dispatches - td0 == 1u && TILE(p).gather_failed == tf0);
    uint16_t *img = malloc((size_t)g_W * g_H * 8);
    uint32_t w, h;
    assert(lagfx_a64_exec_readback_texture(p, t, REF_DST, img, (size_t)g_W * g_H * 8, &w, &h) && w == g_W && h == g_H);
    float got = h2f(img[0]), stale = model(0, 0, 0);
    printf("INFO: (f3) tile gather after a blit fill: pixel (0,0).r = %g (fresh 0, stale %g), tile gathers %llu (by GPU %llu)\n", got, stale, (unsigned long long)(TILE(p).gathered - tg0), (unsigned long long)(TILE(p).gather_gpu - gg0));
    free(img);
    assert(stale != 0.0f);
    if (got != 0.0f) { puts("FAIL: (f3) the tile dispatch read a stale uniform buffer copy"); exit(1); }
    assert(TILE(p).gather_gpu - gg0 >= 1u);
}
/* One draw of cell c of the open pass: the position buffer object posref at off, the cell's viewport. */
static void fresh_draw_cell(uint32_t posref, uint64_t off, uint32_t c) {
    cb_bufs(125, 0, &posref, 1); cb_buf_offset(0, off);
    cb_viewport((c % CELLS_ROW) * CELL, (c / CELLS_ROW) * CELL, CELL, CELL); cb_draw(4, 0, 4);
}
static void fresh_pass_open(uint32_t grt) {
    const uint32_t tex[1] = { REF_GTEX }, vb[3] = { REF_POS0, REF_UV, REF_MAT };
    cb_pass(grt, 2, B4); cb_pipeline(REF_PIPE_TEX); cb_bufs(125, 0, vb, 3); cb_refs(114, 0, tex, 1);
}
static uint32_t read_cell(lagfx_protocol_t *p, lagfx_task_entry_t *t, uint32_t ref, uint32_t c) {
    uint32_t px[GRT_DIM * GRT_DIM];
    assert(lagfx_a64_exec_readback_texture(p, t, ref, px, sizeof px, NULL, NULL));
    return px[(c / CELLS_ROW) * CELL * GRT_DIM + (c % CELLS_ROW) * CELL];
}
/* (f5) a write is pending while the open pass is MULTISAMPLED (no GPU copy possible there): that draw gets the host bytes, and they must not be taken as fresh by a later 1x pass that
 * binds the same range. */
static void run_fresh_ms(lagfx_protocol_t *p, lagfx_task_entry_t *t) {
    g_nseg = 0;
    seg_begin(); blit_fill(REF_POS0, 0, 0x100, 0); seg_end(2, 0);
    seg_begin(); cb_pass(REF_GTEX, 2, G4); fresh_pass_open(REF_GMS); fresh_draw_cell(REF_POS0, POS_OFFS[0], 0); seg_end(0, 0);
    seg_begin(); fresh_pass_open(REF_GRT_B); fresh_draw_cell(REF_POS0, POS_OFFS[0], 0); seg_end(0, 0);
    uint64_t hs0 = RUNC(p).ext_gather_host_stale, gp0 = RUNC(p).ext_gather_gpu, sf0 = RUNC(p).submit_failed, gf0 = RUNC(p).ext_gather_failed;
    exec_segs(p, 1);
    assert(RUNC(p).submit_failed == sf0 && RUNC(p).ext_gather_failed == gf0);
    uint32_t c0 = read_cell(p, t, REF_GRT_B, 0);
    printf("INFO: (f5) multisampled pass, then a 1x pass: host-stale copies %llu, GPU copies %llu, 1x pass cell0 %08x (want %08x)\n", (unsigned long long)(RUNC(p).ext_gather_host_stale - hs0), (unsigned long long)(RUNC(p).ext_gather_gpu - gp0), c0, BLUE);
    assert(RUNC(p).ext_gather_host_stale - hs0 == 1u && RUNC(p).ext_gather_gpu - gp0 == 1u);
    if (c0 != BLUE) { puts("FAIL: (f5) the 1x pass reused the stale copy of the multisampled pass"); exit(1); }
}
/* (f8) a pass that is ended and begun again for a GPU copy keeps what it drew before and draws after. Cells 1 and 3 are drawn before / after the copy that cell 0 needs, cell 2 not at all. */
static void run_fresh_pass_keep(lagfx_protocol_t *p, lagfx_task_entry_t *t) {
    g_nseg = 0;
    seg_begin(); blit_fill(REF_POS0, 0, 0x100, 0); seg_end(2, 0);
    seg_begin(); cb_pass(REF_GTEX, 2, G4); fresh_pass_open(REF_GRT_A);
    fresh_draw_cell(REF_POS0 + 1, POS_OFFS[1], 1);
    fresh_draw_cell(REF_POS0, POS_OFFS[0], 0);
    fresh_draw_cell(REF_POS0 + 2, POS_OFFS[2], 3);
    seg_end(0, 0);
    uint64_t gp0 = RUNC(p).ext_gather_gpu, sf0 = RUNC(p).submit_failed, gf0 = RUNC(p).ext_gather_failed;
    exec_segs(p, 1);
    assert(RUNC(p).submit_failed == sf0 && RUNC(p).ext_gather_failed == gf0 && RUNC(p).ext_gather_gpu - gp0 == 1u);
    uint32_t c[4] = { read_cell(p, t, REF_GRT_A, 0), read_cell(p, t, REF_GRT_A, 1), read_cell(p, t, REF_GRT_A, 2), read_cell(p, t, REF_GRT_A, 3) };
    printf("INFO: (f8) pass ended and resumed for a GPU copy: cells %08x %08x %08x %08x (want blue, green, blue, green)\n", c[0], c[1], c[2], c[3]);
    if (c[0] != BLUE || c[1] != GREEN || c[2] != BLUE || c[3] != GREEN) { puts("FAIL: (f8) the pass lost what it drew before the copy or after it"); exit(1); }
}
/* (f7) a tile kernel that only READS a buffer does not mark it written: two dispatches over the same unaligned uniforms and no write in the exec need no GPU copy. */
static void run_fresh_tile_readonly(lagfx_protocol_t *p) {
    static const double sent[4] = { SENT, SENT, SENT, SENT };
    g_nseg = 0;
    seg_begin(); cb_pass(REF_DST, 2, sent); cb_pipeline(REF_TILE); cb_tile_buf(0, REF_UBO, UBO_UNAL); cb_tile_buf(1, REF_WBUF, 0); cb_tile_tex(0, REF_SRC);
    cb_dispatch_region(g_S, 4, 0, 0, g_S, 4); cb_dispatch_region(g_S, 4, g_S, 0, g_S, 4); seg_end(0, 0);
    uint64_t td0 = TILE(p).dispatches, gg0 = TILE(p).gather_gpu, tg0 = TILE(p).gathered, tf0 = TILE(p).gather_failed;
    exec_segs(p, 1);
    printf("INFO: (f7) two tile dispatches over a read-only unaligned buffer: gathers %llu (by GPU %llu)\n", (unsigned long long)(TILE(p).gathered - tg0), (unsigned long long)(TILE(p).gather_gpu - gg0));
    assert(TILE(p).dispatches - td0 == 2u && TILE(p).gather_failed == tf0 && TILE(p).gathered - tg0 == 2u);
    if (TILE(p).gather_gpu != gg0) { puts("FAIL: (f7) a read-only tile buffer was treated as written"); exit(1); }
}
static void test_fresh(const uint8_t *lib, size_t libn) {
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    struct lagfx_vk_state *vk = g_dev->vk;
    g_S = vk->sg_size >= 4 && vk->sg_size <= 64 ? (lagfx_vk_can_pin_subgroup32(vk) ? 32u : vk->sg_size) : 32u;
    if (vk->phys_props.limits.minStorageBufferOffsetAlignment < 16u) { puts("SKIP: gather freshness (the device binds storage buffers at any 4-aligned offset)"); return; }
    unsetenv("LAGFX_A64_GATHER_BUDGET"); unsetenv("LAGFX_A64_GATHER_CACHE");
    lagfx_task_entry_t *t; lagfx_protocol_t *p = new_task(&t, lib, libn);
    run_fresh(p, t, 0, "(f0) no write between the draws: the copy is reused");
    run_fresh(p, t, 1, "(f1) blit fill between the draws");
    run_fresh(p, t, 2, "(f2) blit buffer copy between the draws");
    run_fresh_tile(p, t);
    run_fresh_tile_readonly(p);
    {   /* the multisampled target: descriptor type 4, sample count 4 */
        put_texture(REF_GMS, 0x900, GRT_DIM, GRT_DIM, 80, 4, 0);
        uint8_t ty = 4, sc = 4; const uint32_t rec = 12 + 0x18 + 36;
        va_write(VA_PAY + 0x900 + rec + 12, &ty, 1); va_write(VA_PAY + 0x900 + rec + 12 + 18, &sc, 1);
    }
    run_fresh_ms(p, t);
    run_fresh_pass_keep(p, t);
    puts("PASS: a gather after a GPU write recorded earlier in the same exec sees the new bytes; without a write the copy is reused");
    lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); pfree(p);
}
#endif

int main(void) {
    g_mem = calloc(1, MEM_SIZE);
    g_dev = make_device();
    assert(g_dev);
#ifdef LAGFX_HAVE_VULKAN
    size_t bn = 0, ln = 0, vn = 0, fn = 0;
    uint8_t *bc = slurp("tile_simd_blur_15.air.bc", &bn), *v = slurp("guest_viewporttondc_vert.air.bc", &vn), *f = slurp("login_texturecopy_frag.air.bc", &fn);
    if (!bc || !v || !f) { puts("SKIP: fixtures (tile_simd_blur_15, guest_viewporttondc_vert, login_texturecopy_frag) missing"); lagfx_device_free(g_dev); free(g_mem); return 77; }
    if (!g_dev->vk || !g_dev->vk->initialized) { puts("SKIP: no Vulkan device"); lagfx_device_free(g_dev); free(g_mem); return 77; }
    struct lagfx_vk_state *vk = g_dev->vk;
    uint32_t need = LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SUBGROUP) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SUBGROUP_SHUFFLE);
    if ((vk->caps_enabled & need) != need) { puts("SKIP: device has no compute subgroup shuffle"); lagfx_device_free(g_dev); free(g_mem); return 77; }
    uint8_t *lib = synth_mtlb(bc, bn, "tile_simd_blur_15", 2, &ln);
    g_vlib = synth_mtlb(v, vn, "viewport_to_ndc", 0, &g_vlib_n);
    g_flib = synth_mtlb(f, fn, "texture_copy", 1, &g_flib_n);
    test_mix(lib, ln);
    test_fresh(lib, ln);
    free(lib); free(bc); free(v); free(f); free(g_vlib); free(g_flib);
#else
    puts("SKIP: built without Vulkan");
#endif
    lagfx_device_free(g_dev);
    free(g_mem);
    return 0;
}
