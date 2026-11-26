/* ARM64 pipeline resolver / descriptor binding / topology / cache invalidation / real-data capture, on lavapipe (NOT verified on a hardware GPU).
 * Shaders: tests/fixtures/triangle.metallib (a real metallib) and the real AIR fixtures guest_viewporttondc_vert / login_texturecopy_frag, wrapped in a synthetic
 * MTLB container. SKIPs the Vulkan part when no ICD is present, the fixtures when missing. NOT Apple shaders captured from a guest.
 * SPDX-License-Identifier: MIT */
#include "dispatchers/channel_compute_dispatcher.h"
#include "dispatchers/ring_common.h"
#include "protocol/state.h"
#include "protocol/arm64_exec_decode.h"
#include "protocol/arm64_exec_run.h"
#include "protocol/arm64_objects.h"
#include "protocol/arm64_pipeline.h"
#include "protocol/arm64_dump.h"
#include "protocol/arm64_completion.h"
#include "protocol/arm64_resources.h"
#include "vulkan/command.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
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
#include <time.h>

#define MEM_SIZE   (2u << 20)
#define SHARED_PFN 1u
#define TABLE_PFN  2u
#define RING_PFN0  4u
#define ROOT_PFN   16u
#define TOP_PFN    17u
#define LEAF_PFN   18u
#define DATA_PFN0  32u
#define NVPN       24u

/* test protocols are calloc'ed: the deferred-completion queue the drain creates must be shut down before the struct goes (leak check under ASAN) */
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

static lagfx_device_t *g_dev;   /* real device: Vulkan state when an ICD exists */

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
    assert(len <= sizeof cmd);
    memcpy(cmd, &op, 2); memcpy(cmd + 2, &arg8, 2); memcpy(cmd + 4, &len, 4);
    uint32_t stamp = g_wp + 1u; memcpy(cmd + 8, &stamp, 4);
    if (nwords) memcpy(cmd + 12, words, nwords * 4u);
    assert(g_wp % 0x10000u + len <= 0x10000u);
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

/* ---- guest layout ---- */
#define VA_OBJ    0x0100u
#define VA_PAY    0x4000u            /* payloads of the entries (page 1) */
#define VA_CMD    0x8000u            /* command buffer (page 2) */
#define VPN_LIB_TRI  12u             /* triangle.metallib */
#define VPN_LIB_VERT 13u             /* synthetic MTLB: ViewportToNDC vertex */
#define VPN_LIB_FRAG 14u             /* synthetic MTLB: TextureCopy fragment */
#define VPN_POS 16u
#define VPN_UV  17u
#define VPN_MAT 18u
#define REF_TEX  1u
#define REF_TEX2 2u
#define REF_PIPE_TRI 3u              /* resource-free pipeline: triangle.metallib vertex + fragment */
#define REF_FN_TRI_A 5u
#define REF_FN_TRI_B 6u
#define REF_PIPE_TEX 7u              /* ViewportToNDC + TextureCopy: 3 vertex buffers, a texture */
#define REF_FN_VERT  8u
#define REF_FN_FRAG  9u
#define REF_BUF_POS  10u
#define REF_BUF_UV   11u
#define REF_BUF_MAT  12u
#define REF_LBUF 20u                 /* buffer behind the linear textures */
#define REF_LTEX 21u                 /* tag 9 linear texture over REF_LBUF (sampled) */
#define REF_LRT  22u                 /* a second linear texture over the SAME bytes (render target) */
#define VPN_LBUF 19u
#define REF_PIPE_TRI2 13u            /* a second entry for the same payload (replacement tests) */
#define RT_W 16u
#define RT_H 16u

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
static void put_texture(uint32_t ref, uint32_t pay_off) {
    uint8_t t[200]; memset(t, 0, sizeof t);
    { uint64_t tlen = RT_W * RT_H * 4u; memcpy(t, &tlen, 8); }   /* guest-layout length (no guest pages: vpage 0), mip 0 table entry: the mirror (arm64_resources.c) parses these */
    t[12] = 1;
    { uint8_t *mp = t + 12 + 0x18; uint64_t row = RT_W * 4u; uint32_t mw = RT_W, mh = RT_H, md = 1; memcpy(mp + 0x10, &row, 8); memcpy(mp + 0x18, &mw, 4); memcpy(mp + 0x1c, &mh, 4); memcpy(mp + 0x20, &md, 4); }
    uint32_t rec = 12 + 0x18 + 36, tag = 0x14;
    memcpy(t + rec, &tag, 4);
    uint8_t *d = t + rec + 12;
    d[0] = 2; d[2] = 80;
    uint32_t w = RT_W, h = RT_H, one = 1;
    memcpy(d + 4, &w, 4); memcpy(d + 8, &h, 4); memcpy(d + 12, &one, 4);
    d[16] = 1; d[18] = 1;
    uint32_t tsize = rec + 12 + 28;   /* the mirror (arm64_resources.c) requires the 28-byte description */
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
/* A one-function MTLB around real AIR bitcode (same container layout as the extractor's real-corpus format: header u64s at 0x18/0x48, FET with NAME/TYPE/OFFT/MDSZ/ENDT). */
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
/* A function object: {u64 library VA, u32 length, u32 tag, u32 size} */
static void put_function(uint32_t ref, uint32_t pay_off, uint32_t vpn, const uint8_t *lib, size_t len) {
    va_write((uint64_t)vpn << 14, lib, (uint32_t)len);
    uint8_t h[20] = {0};
    uint64_t va = (uint64_t)vpn << 14; uint32_t l = (uint32_t)len, tag = 1, sz = 20;
    memcpy(h, &va, 8); memcpy(h + 8, &l, 4); memcpy(h + 12, &tag, 4); memcpy(h + 16, &sz, 4);
    va_write(VA_PAY + pay_off, h, 20);
    entry(ref, LAGFX_A64_OBJ_FUNCTION, 20, VA_PAY + pay_off);
}
/* A serialized pipeline: {0xe, size, ref, length, body}; body = `04 <u32 function ref>` tokens, then an optional vertex-descriptor run. */
static uint32_t put_pipeline(uint32_t ref, uint32_t pay_off, const uint32_t *fns, uint32_t nfn, uint32_t salt) {
    uint8_t b[256]; memset(b, 0, sizeof b);
    uint32_t o = 16;
    for (uint32_t i = 0; i < nfn; i++) { b[o++] = 0x04; memcpy(b + o, &fns[i], 4); o += 4; b[o++] = 0x00; }   /* a trailing key byte, as in a token stream */
    b[o++] = 0x05; memcpy(b + o, &salt, 4); o += 4;                                                          /* harmless: a value that names no function */
    uint32_t tag = 0xe, len = o - 16;
    memcpy(b, &tag, 4); memcpy(b + 4, &o, 4); memcpy(b + 8, &ref, 4); memcpy(b + 12, &len, 4);
    va_write(VA_PAY + pay_off, b, o);
    entry(ref, LAGFX_A64_OBJ_SERIALIZED, o, VA_PAY + pay_off);
    return o;
}

static uint8_t *g_tri; static size_t g_tri_n;
static uint8_t *g_vlib, *g_flib; static size_t g_vlib_n, g_flib_n;
static bool g_have_fix;

static bool load_fixtures(void) {
    size_t vn, fn;
    uint8_t *v = slurp("guest_viewporttondc_vert.air.bc", &vn), *f = slurp("login_texturecopy_frag.air.bc", &fn);
    g_tri = slurp("triangle.metallib", &g_tri_n);
    if (!g_tri || !v || !f) { free(v); free(f); return false; }
    g_vlib = synth_mtlb(v, vn, "viewport_to_ndc", 0, &g_vlib_n);   /* TYPE byte: 0 = vertex, 1 = fragment (extractor enum, see air2spirv.c) */
    g_flib = synth_mtlb(f, fn, "texture_copy", 1, &g_flib_n);
    free(v); free(f);
    return true;
}

static void build_objects(void) {
    put_texture(REF_TEX, 0x000);
    put_texture(REF_TEX2, 0x100);
    put_function(REF_FN_TRI_A, 0x400, VPN_LIB_TRI, g_tri, g_tri_n);
    put_function(REF_FN_TRI_B, 0x420, VPN_LIB_TRI, g_tri, g_tri_n);
    uint32_t tri[2] = { REF_FN_TRI_A, REF_FN_TRI_B };
    put_pipeline(REF_PIPE_TRI, 0x500, tri, 2, 1);
    put_function(REF_FN_VERT, 0x440, VPN_LIB_VERT, g_vlib, g_vlib_n);
    put_function(REF_FN_FRAG, 0x460, VPN_LIB_FRAG, g_flib, g_flib_n);
    uint32_t tx[2] = { REF_FN_VERT, REF_FN_FRAG };
    put_pipeline(REF_PIPE_TEX, 0x700, tx, 2, 2);
    const float pos[8] = { -1, -1, 1, -1, -1, 1, 1, 1 }, uv[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    const float mat[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 0, 0,  0, 0, 0, 1 };   /* position = x * row0 + y * row1 + row3 */
    va_write((uint64_t)VPN_POS << 14, pos, sizeof pos); va_write((uint64_t)VPN_UV << 14, uv, sizeof uv); va_write((uint64_t)VPN_MAT << 14, mat, sizeof mat);
    put_buffer_object(REF_BUF_POS, 0x200, 0x4000, VPN_POS);
    put_buffer_object(REF_BUF_UV, 0x220, 0x4000, VPN_UV);
    put_buffer_object(REF_BUF_MAT, 0x240, 0x4000, VPN_MAT);
}

/* ---- command buffer builder ---- */
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
static void cb_viewport(double w, double h) { double v[6] = { 0, 0, w, h, 0, 1 }; cb_cmd(130, v, sizeof v); }
static void cb_pipeline(uint32_t ref) { cb_cmd(116, &ref, 4); }
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
static void cb_draw(uint32_t prim, uint32_t start, uint32_t count) { uint8_t r[8]; uint16_t s = (uint16_t)start, c = (uint16_t)count; memcpy(r, &prim, 4); memcpy(r + 4, &s, 2); memcpy(r + 6, &c, 2); cb_cmd(1, r, 8); }
static uint32_t cb_finish(void) {
    uint32_t seglen = g_cbn, info = 0;
    memcpy(g_cb, &seglen, 4); memcpy(g_cb + 4, &info, 4);
    va_write(VA_CMD, g_cb, g_cbn);
    return g_cbn;
}
static bool g_res_tex, g_res_lin;
static uint32_t g_extra_refs[16], g_nextra;   /* more refs for the exec's resource list (the reason tests list objects that must or must not be created) */
static void exec(lagfx_protocol_t *p, uint32_t task, uint32_t len) {
    static const uint32_t refs[] = { REF_TEX, REF_TEX2, REF_PIPE_TRI, REF_BUF_POS, REF_BUF_UV, REF_BUF_MAT, REF_FN_TRI_A, REF_PIPE_TRI2, REF_PIPE_TEX };
    static const uint32_t lrefs[] = { REF_LBUF, REF_LTEX, REF_LRT };
    uint32_t nr = g_res_tex ? 9u : 8u;           /* the textured pipeline is in the resource list only when the test uses it (listing it creates its object) */
    uint32_t w[3 + 2 * 28 + 4], n = 0;
    w[n++] = task; w[n++] = nr + (g_res_lin ? 3u : 0u) + g_nextra; w[n++] = 1;
    for (uint32_t i = 0; i < nr; i++) { w[n++] = refs[i]; w[n++] = 0; }
    if (g_res_lin) for (uint32_t i = 0; i < 3; i++) { w[n++] = lrefs[i]; w[n++] = 0; }
    for (uint32_t i = 0; i < g_nextra; i++) { w[n++] = g_extra_refs[i]; w[n++] = 0; }
    uint64_t off = VA_CMD, l = len;
    memcpy(w + n, &off, 8); n += 2; memcpy(w + n, &l, 8); n += 2;
    submit(p, 0x43, 0, w, n);
}
static uint32_t px(const uint8_t *img, uint32_t x, uint32_t y) { uint32_t v; memcpy(&v, img + (y * RT_W + x) * 4, 4); return v; }
#define GREEN 0xff00ff00u
#define BLUE  0xff0000ffu

static void test_pdesc(void) {
    uint8_t b[160]; memset(b, 0, sizeof b);
    uint32_t o = 16;
    b[o++] = 4; uint32_t v = 7; memcpy(b + o, &v, 4); o += 4; b[o++] = 0;
    b[o++] = 4; v = 300; memcpy(b + o, &v, 4); o += 4;            /* a value that is not a function */
    b[o++] = 4; v = 9; memcpy(b + o, &v, 4); o += 4;
    b[o++] = 4; v = 7; memcpy(b + o, &v, 4); o += 4;              /* duplicate */
    uint8_t *a = b + o;                                           /* attr: fmt 29 @16 buf 1 ; fmt 31 @0 buf 1 */
    uint32_t at[2][3] = { { 29, 16, 1 }, { 31, 0, 1 } };
    for (int i = 0; i < 2; i++) { a[0] = 1; a[1] = 4; memcpy(a + 2, &at[i][0], 4); a[6] = 2; a[7] = 4; memcpy(a + 8, &at[i][1], 4); a[12] = 3; a[13] = 4; memcpy(a + 14, &at[i][2], 4); a += 18; }
    for (int i = 0; i < 3; i++) { a[0] = 4; uint32_t st = 32; memcpy(a + 1, &st, 4); a[5] = 2; a += 6; }
    uint32_t tag = 0xe, total = (uint32_t)(a - b), len = total - 16;
    memcpy(b, &tag, 4); memcpy(b + 4, &total, 4); memcpy(b + 12, &len, 4);
    lagfx_a64_pdesc_t d;
    assert(lagfx_a64_pdesc_decode(b, total, NULL, NULL, &d));
    assert(d.tag == 0xe && d.length == len);
    assert(d.nrefs >= 3 && d.refs[0] == 7 && d.refs[1] == 300 && d.refs[2] == 9);          /* no validator: every token; first-seen order; deduplicated */
    assert(d.nattrs == 2 && d.attrs[0].off == 0 && d.attrs[0].fmt == 31 && d.attrs[1].off == 16 && d.attrs[1].bufidx == 1);   /* sorted by offset */
    assert(d.stride == 32);
    assert(!lagfx_a64_pdesc_decode(b, 8, NULL, NULL, &d));                                  /* shorter than the header */
    puts("PASS: pipeline descriptor decode (refs, vertex attrs, stride)");
}

#ifdef LAGFX_HAVE_VULKAN
#define PIPE(p) ((p)->diag2.exec.pipe)
#define RUNC(p) ((p)->diag2.exec.run)
static const double G4[4] = { 0, 1, 0, 1 }, B4[4] = { 0, 0, 1, 1 };

static lagfx_protocol_t *new_task(lagfx_task_entry_t **t) {
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    *t = def_task(p, 1);
    build_objects();
    return p;
}
static void tri_exec(lagfx_protocol_t *p, uint32_t pipe_ref, uint32_t prim, uint32_t nverts, bool with_vbuf) {
    (void)with_vbuf;
    cb_reset(); cb_pass(REF_TEX, 2, G4); cb_viewport(RT_W, RT_H); cb_pipeline(pipe_ref); cb_draw(prim, 0, nverts);
    exec(p, 1, cb_finish());
}

static void test_resolver_triangle_and_topology(void) {
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_task_entry_t *t;
    lagfx_protocol_t *p = new_task(&t);
    uint8_t img[RT_W * RT_H * 4];
    uint32_t w, h;
    /* the DEFAULT resolver (no lagfx_a64_exec_set_pipeline_resolver call): triangle.metallib's vertex + fragment, through the guest descriptor */
    tri_exec(p, REF_PIPE_TRI, 3, 3, false);
    assert(PIPE(p).resolved == 1 && PIPE(p).resolve_failed == 0 && PIPE(p).xlate_air2spv == 2 && RUNC(p).pipeline_unresolved == 0);
    assert(RUNC(p).draws == 1 && RUNC(p).submit_failed == 0 && PIPE(p).pipes_built == 1 && PIPE(p).topo[3] == 1);
    assert(PIPE(p).stub_blend == 1 && PIPE(p).stub_depth_stencil == 0 && PIPE(p).stub_raster_state == 0 && PIPE(p).stub_color_format_from_pass == 1);
    assert(lagfx_a64_exec_readback_texture(p, t, REF_TEX, img, sizeof img, &w, &h));
    uint32_t centre = px(img, 8, 8), corner = px(img, 0, 0);
    printf("INFO: triangle list: centre=%08x corner=%08x\n", centre, corner);
    assert(corner == GREEN && centre != GREEN);                       /* something was drawn over the clear colour, the corner stays clear */
    /* the same pipeline object, other topologies: one VkPipeline each, cached */
    tri_exec(p, REF_PIPE_TRI, 4, 3, false);                          /* triangle strip of 3 vertices = the same triangle */
    assert(lagfx_a64_exec_readback_texture(p, t, REF_TEX, img, sizeof img, NULL, NULL) && px(img, 8, 8) == centre && PIPE(p).topo[4] == 1);
    tri_exec(p, REF_PIPE_TRI, 1, 3, false);                          /* line list of 3 vertices: one line, no fill at the centre column away from it */
    assert(PIPE(p).topo[1] == 1 && RUNC(p).draws == 3);
    tri_exec(p, REF_PIPE_TRI, 0, 3, false);                          /* points (this shader has no PointSize) */
    tri_exec(p, REF_PIPE_TRI, 2, 3, false);                          /* line strip */
    assert(PIPE(p).point_no_size == 1 && PIPE(p).topo[0] == 0 && PIPE(p).topo[2] == 1 && RUNC(p).draws == 4);   /* triangle_vertex writes no PointSize: a point draw would be invalid Vulkan, it is skipped and counted */
    tri_exec(p, REF_PIPE_TRI, 3, 3, false);                          /* back to the list: cached, not rebuilt */
    assert(PIPE(p).pipes_built == 4 && RUNC(p).draws == 5);
    tri_exec(p, REF_PIPE_TRI, 5, 3, false);                          /* triangle fan etc. = unsupported primitive: counted, skipped */
    assert(RUNC(p).prim_unsupported == 1 && RUNC(p).draws == 5 && RUNC(p).draws_skipped == 2);
    /* a pipeline whose function refs name nothing: unresolved, counted by cause, nothing aborts */
    uint32_t none[1] = { 99 };
    put_pipeline(REF_PIPE_TRI2, 0x800, none, 1, 3);
    tri_exec(p, REF_PIPE_TRI2, 3, 3, false);
    assert(PIPE(p).rf_no_refs == 1 && RUNC(p).pipeline_unresolved == 1 && RUNC(p).draws == 5);
    assert(RUNC(p).no_pipeline_why[1] == 1 && RUNC(p).no_pipeline_why[0] == 0 && RUNC(p).no_pipeline == 1);   /* the cause: tag 116 named a pipeline that did not resolve */
    lagfx_a64_exec_drop_task(p, 1);
    lagfx_a64_exec_free(p);
    pfree(p);
    puts("PASS: default resolver on lavapipe: triangle.metallib, line/strip/list/strip topologies cached per pipeline, point skipped (no PointSize), unsupported primitive, unresolvable pipeline");
}

static void test_textured_draw(void) {
    g_res_tex = true;
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_task_entry_t *t;
    lagfx_protocol_t *p = new_task(&t);
    uint8_t img[RT_W * RT_H * 4];
    const uint32_t vb[3] = { REF_BUF_POS, REF_BUF_UV, REF_BUF_MAT }, tex[1] = { REF_TEX };
    /* pass 1 clears REF_TEX green; pass 2 draws a full-screen quad into REF_TEX2 (cleared blue) that SAMPLES REF_TEX: 3 vertex storage buffers (tag 125),
     * a fragment texture (114), the shader's inline sampler (default sampler). Every pixel must come out green. */
    cb_reset();
    cb_pass(REF_TEX, 2, G4);
    cb_pass(REF_TEX2, 2, B4); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE_TEX); cb_bufs(125, 0, vb, 3); cb_refs(114, 0, tex, 1); cb_draw(4, 0, 4);
    exec(p, 1, cb_finish());
    printf("INFO: textured draw: resolved=%llu failed=%llu rf_bindings=%llu rf_vertex_res=%llu translate=%llu module=%llu layout=%llu sets=%llu unbound=%llu\n", (unsigned long long)PIPE(p).resolved,
           (unsigned long long)PIPE(p).resolve_failed, (unsigned long long)PIPE(p).rf_bindings, (unsigned long long)PIPE(p).rf_vertex_res, (unsigned long long)PIPE(p).rf_translate,
           (unsigned long long)PIPE(p).rf_module, (unsigned long long)PIPE(p).rf_layout, (unsigned long long)PIPE(p).desc_sets, (unsigned long long)PIPE(p).desc_unbound);
    assert(PIPE(p).resolved == 2 && PIPE(p).resolve_failed == 0 && RUNC(p).draws == 1 && PIPE(p).desc_sets == 1 && RUNC(p).submit_failed == 0);
    assert(lagfx_a64_exec_readback_texture(p, t, REF_TEX2, img, sizeof img, NULL, NULL));
    assert(px(img, 8, 8) == GREEN && px(img, 0, 0) == GREEN && px(img, 15, 15) == GREEN && px(img, 15, 0) == GREEN);   /* sampled through the descriptor set */
    assert(RUNC(p).tag_done[114] == 1 && RUNC(p).tag_done[125] == 1);

    /* the same draw while no texture is bound: descriptor unbound, draw skipped (Metal leaves it undefined), the clear stays */
    cb_reset(); cb_pass(REF_TEX2, 2, B4); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE_TEX); cb_bufs(125, 0, vb, 3); cb_draw(4, 0, 4);
    exec(p, 1, cb_finish());
    assert(PIPE(p).desc_unbound == 1 && RUNC(p).draws == 1 && RUNC(p).draws_skipped == 1);
    assert(lagfx_a64_exec_readback_texture(p, t, REF_TEX2, img, sizeof img, NULL, NULL) && px(img, 8, 8) == BLUE);
    /* sampling the attachment being rendered: skipped (a feedback loop) */
    const uint32_t self[1] = { REF_TEX2 };
    cb_reset(); cb_pass(REF_TEX2, 2, B4); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE_TEX); cb_bufs(125, 0, vb, 3); cb_refs(114, 0, self, 1); cb_draw(4, 0, 4);
    exec(p, 1, cb_finish());
    assert(PIPE(p).desc_feedback == 1 && RUNC(p).draws == 1);
    /* a storage-buffer offset the device cannot bind (4 is not a multiple of minStorageBufferOffsetAlignment): no longer skipped, an aligned copy of the range is bound (ext_gathered) */
    cb_reset(); cb_pass(REF_TEX2, 2, B4); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE_TEX); cb_bufs(125, 0, vb, 3); cb_refs(114, 0, tex, 1);
    { uint8_t r[12] = {0}; uint32_t idx = 2; uint64_t off = 4; memcpy(r, &idx, 4); memcpy(r + 4, &off, 8); cb_cmd(126, r, 12); }
    cb_draw(4, 0, 4);
    exec(p, 1, cb_finish());
    assert(PIPE(p).desc_unaligned == 0 && PIPE(p).desc_unbound == 1 && PIPE(p).desc_feedback == 1);
    assert(RUNC(p).ext_gathered == 1 && RUNC(p).ext_gather_failed == 0 && RUNC(p).draws == 2);
    /* gather failure classification: ten distinct unaligned offsets in ONE exec; the exec keeps 8 aligned copies, the rest fail with cause 2 (cache full), counted and skipped */
    uint64_t g0 = RUNC(p).ext_gathered, u0 = PIPE(p).desc_unaligned;
    cb_reset(); cb_pass(REF_TEX2, 2, B4); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE_TEX); cb_bufs(125, 0, vb, 3); cb_refs(114, 0, tex, 1);
    for (uint32_t k = 0; k < 10; k++) { uint8_t r[12] = {0}; uint32_t idx = 2; uint64_t off = 4 + 8ull * k; memcpy(r, &idx, 4); memcpy(r + 4, &off, 8); cb_cmd(126, r, 12); cb_draw(4, 0, 4); }
    exec(p, 1, cb_finish());
    assert(RUNC(p).ext_gathered - g0 == 8 && RUNC(p).ext_gather_failed == 2 && RUNC(p).ext_gather_why[2] == 2 && RUNC(p).ext_gather_why[0] + RUNC(p).ext_gather_why[1] + RUNC(p).ext_gather_why[3] == 0);
    assert(PIPE(p).desc_unaligned - u0 == 2);
    lagfx_a64_exec_drop_task(p, 1);
    lagfx_a64_exec_free(p);
    pfree(p);
    g_res_tex = false;
    puts("PASS: descriptor binding on lavapipe: 3 storage buffers + texture + inline sampler read back, unbound / feedback skipped, misaligned offset gathered");
}


/* Texture handles that do not resolve, by reason (desc_tex_why). One pass, one draw per handle: each draw samples a different ref in fragment slot 0 and is skipped
 * (desc_unbound, nothing drawn). Every reason a handle can fail for that the test setup can reach is produced once and must be counted under its own name, and the
 * first distinct texture of each reason is recorded with its object table entry type / Metal format / extent. */
static void put_texture_ex(uint32_t ref, uint32_t pay_off, uint32_t fmt, uint32_t mtype, uint32_t w, uint32_t h, uint32_t samples) {
    put_texture(ref, pay_off);
    uint8_t t[200]; memset(t, 0, sizeof t);
    uint32_t rec = 12 + 0x18 + 36, tsize = rec + 12 + 28;
    va_write(VA_PAY + pay_off, t, 0);   /* (no-op: keeps the layout of put_texture, patched below) */
    uint8_t *tp = g_mem + ((uint64_t)(DATA_PFN0 + 1) << 14) + (pay_off);   /* VA_PAY is page 1 */
    uint8_t *d = tp + rec + 12;
    d[0] = (uint8_t)mtype; d[2] = (uint8_t)fmt; d[3] = (uint8_t)(fmt >> 8);
    memcpy(d + 4, &w, 4); memcpy(d + 8, &h, 4);
    uint16_t sm = (uint16_t)samples; memcpy(d + 18, &sm, 2);
    (void)tsize;
}
void lagfx_diag_stats(lagfx_protocol_t *p, uint64_t t, bool final);
static char *capture_stats(lagfx_protocol_t *p);
static void test_tex_unbound_reasons(void) {
    g_res_tex = true;
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_task_entry_t *t;
    lagfx_protocol_t *p = new_task(&t);
    t->objects_len = 12u * 64u;
    const uint32_t vb[3] = { REF_BUF_POS, REF_BUF_UV, REF_BUF_MAT };
    const uint32_t R_BC1 = 30, R_MSAA = 31, R_OK_UNLISTED = 32, R_CUBE = 33, R_EMPTY = 34, R_MAPPED = 36, R_DIMS = 37, R_BEYOND = 100;
    put_texture_ex(R_BC1, 0x800, 130 /* BC1_RGBA */, 2, RT_W, RT_H, 1);
    put_texture_ex(R_MSAA, 0x900, 80, 2, RT_W, RT_H, 4);
    put_texture_ex(R_OK_UNLISTED, 0xa00, 80, 2, RT_W, RT_H, 1);
    put_texture_ex(R_CUBE, 0xb00, 80, 5 /* MTLTextureTypeCube */, RT_W, RT_H, 1);
    put_texture_ex(R_DIMS, 0xc00, 80, 2, 0, RT_H, 1);
    { uint8_t mp[56]; memset(mp, 0, sizeof mp); uint64_t mapping = 7; uint32_t tag = 0x2f; memcpy(mp, &mapping, 8); memcpy(mp + 8, &tag, 4); mp[22] = 80;
      va_write(VA_PAY + 0xd00, mp, sizeof mp); entry(R_MAPPED, 11, sizeof mp, VA_PAY + 0xd00); }   /* a mapped texture; the test shell has no surface descriptor for mapping 7 */
    g_nextra = 0;
    g_extra_refs[g_nextra++] = R_BC1; g_extra_refs[g_nextra++] = R_MSAA; g_extra_refs[g_nextra++] = R_CUBE; g_extra_refs[g_nextra++] = R_DIMS; g_extra_refs[g_nextra++] = R_MAPPED;
    const uint32_t seq[] = { 0 /* nil */, R_BEYOND, R_EMPTY, REF_BUF_POS, REF_FN_VERT, R_OK_UNLISTED, R_BC1, R_MSAA, R_CUBE, R_DIMS, R_MAPPED };
    cb_reset(); cb_pass(REF_TEX2, 2, B4); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE_TEX); cb_bufs(125, 0, vb, 3);
    cb_draw(4, 0, 4);                                                       /* no texture record at all: slot_unbound */
    for (unsigned i = 0; i < sizeof seq / sizeof *seq; i++) { cb_refs(114, 0, &seq[i], 1); cb_draw(4, 0, 4); }
    { uint8_t big[8 + 4 * 33]; memset(big, 0, sizeof big); uint32_t cnt = 33; memcpy(big + 4, &cnt, 4); cb_cmd(114, big, sizeof big); cb_draw(4, 0, 4); }   /* a record with more handles than the table holds: refused */
    exec(p, 1, cb_finish());
    g_nextra = 0;
    const unsigned NDRAW = 1 + (unsigned)(sizeof seq / sizeof *seq) + 1;
    assert(RUNC(p).draws == 0 && PIPE(p).desc_unbound == NDRAW && PIPE(p).desc_unbound_why[3] == NDRAW);
    uint64_t sum = 0;
    for (unsigned c = 0; c < TEXWHY_COUNT; c++) sum += PIPE(p).desc_tex_why[c];
    assert(sum == NDRAW);                                                   /* the split adds up to the old single counter */
    assert(PIPE(p).desc_tex_why[TEXWHY_SLOT_UNBOUND] == 1);
    assert(PIPE(p).desc_tex_why[TEXWHY_REF_ZERO] == 1);
    assert(PIPE(p).desc_tex_why[TEXWHY_NO_TABLE] == 1);                     /* ref 100 is beyond the object table length */
    assert(PIPE(p).desc_tex_why[TEXWHY_ENTRY_EMPTY] == 1);
    assert(PIPE(p).desc_tex_why[TEXWHY_NOT_TEXTURE] == 1);                  /* a buffer */
    assert(PIPE(p).desc_tex_why[TEXWHY_UNKNOWN_TYPE] == 1);                 /* a function */
    assert(PIPE(p).desc_tex_why[TEXWHY_NOT_MIRRORED] == 1);                 /* a well-formed texture that was not in the exec's resource list */
    assert(PIPE(p).desc_tex_why[TEXWHY_C_FORMAT] == 1);
    assert(PIPE(p).desc_tex_why[TEXWHY_C_MSAA] == 1);
    assert(PIPE(p).desc_tex_why[TEXWHY_C_TYPE] == 1);
    assert(PIPE(p).desc_tex_why[TEXWHY_C_DIMS] == 1);
    assert(PIPE(p).desc_tex_why[TEXWHY_C_MAPPED_NODESC] == 1);
    assert(PIPE(p).desc_tex_why[TEXWHY_RECORD_BAD] == 1);
    /* the logged distinct texture of a reason names the object: entry type, Metal format, extent */
    assert(PIPE(p).desc_tex_nseen[TEXWHY_C_FORMAT] == 1 && PIPE(p).desc_tex_seen[TEXWHY_C_FORMAT][0].ref == R_BC1 && PIPE(p).desc_tex_seen[TEXWHY_C_FORMAT][0].type == 2 &&
           PIPE(p).desc_tex_seen[TEXWHY_C_FORMAT][0].mtl_format == 130 && PIPE(p).desc_tex_seen[TEXWHY_C_FORMAT][0].w == RT_W && PIPE(p).desc_tex_seen[TEXWHY_C_FORMAT][0].h == RT_H);
    assert(PIPE(p).desc_tex_seen[TEXWHY_NOT_MIRRORED][0].ref == R_OK_UNLISTED && PIPE(p).desc_tex_seen[TEXWHY_NOT_MIRRORED][0].mtl_format == 80 && PIPE(p).desc_tex_seen[TEXWHY_NOT_MIRRORED][0].task == 1);
    assert(PIPE(p).desc_tex_seen[TEXWHY_C_MAPPED_NODESC][0].type == 11 && PIPE(p).desc_tex_seen[TEXWHY_C_MAPPED_NODESC][0].mapping == 7);
    assert(PIPE(p).desc_tex_seen[TEXWHY_UNKNOWN_TYPE][0].type == LAGFX_A64_OBJ_FUNCTION);
    /* the diagnosis is also callable on its own, and follows the guest: the entry gets filled in later (created later than the first draw) */
    lagfx_a64_texdiag_t dg;
    assert(lagfx_a64_res_diagnose(p, 1, R_EMPTY, &dg) && dg.why == TEXWHY_ENTRY_EMPTY);
    put_texture_ex(R_EMPTY, 0xe00, 80, 2, RT_W, RT_H, 1);
    assert(lagfx_a64_res_diagnose(p, 1, R_EMPTY, &dg) && dg.why == TEXWHY_NOT_MIRRORED && dg.mtl_format == 80);
    g_extra_refs[g_nextra++] = R_EMPTY;
    cb_reset(); cb_pass(REF_TEX2, 2, B4); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE_TEX); cb_bufs(125, 0, vb, 3); { uint32_t r1 = R_EMPTY; cb_refs(114, 0, &r1, 1); } cb_draw(4, 0, 4);
    exec(p, 1, cb_finish());
    g_nextra = 0;
    assert(RUNC(p).draws == 1 && PIPE(p).desc_unbound == NDRAW);            /* once the entry is filled in and listed, it resolves and draws */
    {   /* the log line carries the draw context: draw index, pipeline ref, shader names / hashes, binding, colour attachment 0 */
        char *st = capture_stats(p);
        assert(strstr(st, "obj_table[created=") && strstr(st, "texwhy[slot_unbound=1 record_bad=1 ref_zero=1 no_table=1"));
        free(st);
        const char *pn = "viewport_to_ndc";
        assert(strlen(pn) < sizeof ((lagfx_a64_pipeline_src_t *)0)->vs_name);
    }
    lagfx_a64_exec_drop_task(p, 1);
    lagfx_a64_exec_free(p);
    pfree(p);
    g_res_tex = false;
    puts("PASS: unresolved texture handles are counted by reason (nil, beyond the table, empty entry, buffer, function, not listed, bad format / MSAA / type / extent, mapped without a surface, refused record) and logged with their format and size");
}

/* The mirror's object table reuses the slot of a released object. Before, every release left a hole and every later creation appended: MAX_OBJS (4096) creations over
 * a run, however few objects were alive, refused every new texture (silently: no counter) = "texture handle did not resolve". */
static void test_object_slots_reused(void) {
    g_res_tex = true;
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_task_entry_t *t;
    lagfx_protocol_t *p = new_task(&t);
    tri_exec(p, REF_PIPE_TRI, 3, 3, false);
    uint64_t slots0 = lagfx_a64_res_stats(p)->obj_slots, created0 = lagfx_a64_res_stats(p)->created;
    assert(slots0 > 0);
    for (unsigned i = 0; i < 40; i++) {
        lagfx_a64_res_forget(p, 1, REF_TEX);
        lagfx_a64_res_forget(p, 1, REF_BUF_POS);
        tri_exec(p, REF_PIPE_TRI, 3, 3, false);
    }
    assert(lagfx_a64_res_stats(p)->created >= created0 + 80);               /* 80 recreations ... */
    assert(lagfx_a64_res_stats(p)->obj_slots == slots0 && lagfx_a64_res_stats(p)->obj_table_full == 0);   /* ... in the freed slots: the table did not grow */
    lagfx_a64_exec_drop_task(p, 1);
    lagfx_a64_exec_free(p);
    pfree(p);
    g_res_tex = false;
    puts("PASS: released object slots of the mirror table are reused (no growth over forget / recreate cycles)");
}

void lagfx_diag_stats(lagfx_protocol_t *p, uint64_t t, bool final);
/* the statistics block the diagnostics print for p, captured from stderr */
static char *capture_stats(lagfx_protocol_t *p) {
    fflush(stderr);
    char path[] = "/tmp/lagfx-stats-XXXXXX";
    int fd = mkstemp(path), save = dup(2);
    assert(fd >= 0 && save >= 0);
    dup2(fd, 2);
    lagfx_diag_stats(p, 1, true);
    fflush(stderr);
    dup2(save, 2); close(save);
    off_t n = lseek(fd, 0, SEEK_END);
    char *b = calloc(1, (size_t)n + 1);
    lseek(fd, 0, SEEK_SET);
    assert(read(fd, b, (size_t)n) == n);
    close(fd); unlink(path);
    return b;
}
static void res_cycle(lagfx_protocol_t *p, uint32_t task, const uint32_t *refs, uint32_t n) {   /* one mirror exec listing refs, ended and waited for (no GPU work recorded) */
    uint32_t res[64];
    assert(n <= 32);
    for (uint32_t i = 0; i < n; i++) res[2 * i] = refs[i], res[2 * i + 1] = 0;
    lagfx_a64_rexec_t *x = lagfx_a64_res_begin(p, task, (const uint8_t *)res, n);
    assert(x);
    (void)lagfx_a64_res_end(x);
    while (lagfx_a64_res_poll(p, 100) == LAGFX_A64_PENDING) {}
}

/* Slot reuse safety. Objects are found by (task, ref) stored IN the object, never by a slot index, and every object is its own heap block with a unique `gen`: a
 * slot that is reused cannot make an old ref hit the new object, the same ref of another task is a different object, an object released while an exec records or is in
 * flight is parked (m->dead) and freed only after that exec finished, and a new object is calloc'ed (no residual state). */
static void test_slot_reuse_safety(void) {
    g_res_tex = true;
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_task_entry_t *t1;
    lagfx_protocol_t *p = new_task(&t1);
    lagfx_task_entry_t *t2 = def_task(p, 2);
    (void)t2;
    /* (a) same ref number in two tasks stays two objects; a stale ref misses even when its slot holds another object now */
    const uint32_t both[2] = { REF_TEX, REF_BUF_POS };
    res_cycle(p, 1, both, 2); res_cycle(p, 2, both, 2);
    lagfx_a64_obj_t *a1 = lagfx_a64_res_lookup(p, 1, REF_TEX), *a2 = lagfx_a64_res_lookup(p, 2, REF_TEX);
    assert(a1 && a2 && a1 != a2 && a1->gen != a2->gen && a1->task == 1 && a2->task == 2);
    lagfx_a64_res_forget(p, 1, REF_TEX);
    assert(lagfx_a64_res_lookup(p, 1, REF_TEX) == NULL && lagfx_a64_res_lookup(p, 2, REF_TEX) == a2);
    const uint32_t other[1] = { REF_TEX2 };
    uint64_t reused0 = PIPE(p).obj_reused;
    res_cycle(p, 1, other, 1);                                             /* a new object takes the freed slot */
    assert(PIPE(p).obj_reused == reused0 + 1);
    assert(lagfx_a64_res_lookup(p, 1, REF_TEX) == NULL);                    /* the stale ref is a miss, not the new object */
    assert(lagfx_a64_res_lookup(p, 1, REF_TEX2) && lagfx_a64_res_lookup(p, 2, REF_TEX) == a2);
    uint64_t miss0 = PIPE(p).obj_lookup_miss;
    (void)lagfx_a64_res_lookup(p, 1, REF_TEX);
    assert(PIPE(p).obj_lookup_miss == miss0 + 1);
    /* (b)(c) release while an exec records: parked, still readable, freed only after the exec; the same ref recreated meanwhile is a distinct, fresh object */
    tri_exec(p, REF_PIPE_TRI, 3, 3, false);                                 /* task 1: REF_TEX is rendered into: it has a view and is host_valid */
    lagfx_a64_obj_t *old = lagfx_a64_res_lookup(p, 1, REF_TEX);
    assert(old && old->view != VK_NULL_HANDLE && old->host_valid);
    uint64_t gen0 = old->gen, dest0 = lagfx_a64_res_stats(p)->destroyed, def0 = lagfx_a64_res_stats(p)->deferred_frees;
    uint32_t res[2] = { REF_BUF_POS, 0 };
    lagfx_a64_rexec_t *x = lagfx_a64_res_begin(p, 1, (const uint8_t *)res, 1);
    assert(x);
    lagfx_a64_res_forget(p, 1, REF_TEX);
    assert(lagfx_a64_res_stats(p)->deferred_frees == def0 + 1 && lagfx_a64_res_stats(p)->destroyed == dest0);   /* logically released, physically alive */
    assert(old->gen == gen0 && old->img != VK_NULL_HANDLE && old->view != VK_NULL_HANDLE);
    lagfx_a64_obj_t *nw = lagfx_a64_res_object(x, REF_TEX);                 /* the same ref again inside this exec, in the freed slot */
    assert(nw && nw != old && nw->gen > gen0 && nw->img != old->img);
    assert(nw->view == VK_NULL_HANDLE && !nw->guest_stale && nw->wgen == 0 && !nw->leak && nw->ms == NULL && !nw->linear);   /* calloc'ed: nothing of the old record */
    assert(old->gen == gen0);
    (void)lagfx_a64_res_end(x);
    while (lagfx_a64_res_poll(p, 100) == LAGFX_A64_PENDING) {}
    assert(lagfx_a64_res_stats(p)->destroyed == dest0 + 1);                 /* freed only now: the exec that could use it has finished (its fence signalled) */
    lagfx_a64_exec_drop_task(p, 1);
    lagfx_a64_exec_drop_task(p, 2);
    lagfx_a64_exec_free(p);
    pfree(p);
    g_res_tex = false;
    puts("PASS: slot reuse safety: refs are per task and matched by (task, ref), a stale ref misses, a released object is parked until its exec finished, a recreated one is fresh");
}

/* More than MAX_OBJS (4096) create / release cycles with a few objects alive never fill the table; and when 4096 objects really are alive the next creation is
 * refused, counted (obj_table.full), named table_full for a texture, and logged in the statistics block; releasing one makes room again. */
static void test_object_table_stress_and_full(void) {
    g_res_tex = true;
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_task_entry_t *t;
    lagfx_protocol_t *p = new_task(&t);
    const uint32_t two[2] = { REF_BUF_POS, REF_BUF_UV };
    for (unsigned i = 0; i < 4200; i++) {
        res_cycle(p, 1, two, 2);
        lagfx_a64_res_forget(p, 1, REF_BUF_POS);
        lagfx_a64_res_forget(p, 1, REF_BUF_UV);
    }
    assert(PIPE(p).obj_created >= 8400 && PIPE(p).obj_full == 0 && PIPE(p).obj_high_water <= 4u && PIPE(p).obj_peak_live <= 4u);
    assert(PIPE(p).obj_reused >= 8390 && PIPE(p).obj_live == 0 && PIPE(p).obj_destroyed == PIPE(p).obj_created);
    lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); pfree(p);

    /* a real full table: task 3's object table (64 KiB at VA 0x20000) names 4096 buffers and one texture, all listed in one exec */
    p = new_task(&t);
    lagfx_task_entry_t *t3 = def_task(p, 3);
    t3->objects_addr = 0x20000;
    put_buffer_object(REF_BUF_POS, 0x200, 0x4000, 0);                       /* the payload shared by all the entries */
    uint32_t e[3] = { LAGFX_A64_OBJ_BUFFER | 16u << 8, (uint32_t)(VA_PAY + 0x200), 0 };
    for (uint32_t r = 1; r <= 4096; r++) va_write(0x20000u + r * 12u, e, 12);
    put_texture(2, 0x100);
    {   /* the texture entry of ref 4097 (types and payload of ref 2 of the first object table) */
        uint8_t eb[12]; memcpy(eb, g_mem + ((uint64_t)DATA_PFN0 << 14) + 0x100 + 2 * 12, 12);
        va_write(0x20000u + 4097u * 12u, eb, 12);
    }
    uint32_t *res = malloc(8u * 4097u);
    for (uint32_t i = 0; i < 4097; i++) res[2 * i] = i + 1u, res[2 * i + 1] = 0;
    lagfx_a64_rexec_t *x = lagfx_a64_res_begin(p, 3, (const uint8_t *)res, 4097);
    assert(x);
    (void)lagfx_a64_res_end(x);
    while (lagfx_a64_res_poll(p, 1000) == LAGFX_A64_PENDING) {}
    free(res);
    assert(PIPE(p).obj_live == 4096 && PIPE(p).obj_peak_live == 4096 && PIPE(p).obj_high_water == 4096);
    assert(PIPE(p).obj_full == 1 && lagfx_a64_res_stats(p)->obj_table_full == 1);
    assert(lagfx_a64_res_lookup(p, 3, 4097) == NULL && lagfx_a64_res_lookup(p, 3, 4096) != NULL);
    lagfx_a64_texdiag_t dg;
    assert(lagfx_a64_res_diagnose(p, 3, 4097, &dg) && dg.why == TEXWHY_TABLE_FULL && dg.type == 2);
    char *st = capture_stats(p);
    assert(strstr(st, "obj_table[created=4096 destroyed=0 reused=0 live=4096 peak_live=4096 high_water=4096 full=1 lookup_miss=1 "));
    free(st);
    lagfx_a64_res_forget(p, 3, 7);                                          /* room again: the texture is created in the freed slot */
    uint32_t again[1] = { 4097 };
    res_cycle(p, 3, again, 1);
    assert(lagfx_a64_res_lookup(p, 3, 4097) != NULL && PIPE(p).obj_full == 1 && PIPE(p).obj_reused == 1 && PIPE(p).obj_live == 4096);
    lagfx_a64_exec_drop_task(p, 3); lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); pfree(p);
    g_res_tex = false;
    puts("PASS: object table: 8400 create / release cycles never fill it; with 4096 live objects the next creation is refused, counted (full), named table_full and printed in obj_table[...]");
}

/* A linear (buffer backed) texture is sampled: Metal shows later changes of the buffer in the texture, so the mirror image is re-pulled when the buffer changed.
 * (1) a draw samples it, (2) the guest rewrites the buffer between two execs, (3) inside ONE exec an earlier pass renders into another linear texture over the
 * same bytes (pushed into the buffer) and a later pass samples the first one. */
static void put_linear(uint32_t ref, uint32_t pay_off, uint32_t bref) {
    uint8_t l[0x40]; memset(l, 0, sizeof l);
    uint32_t tag = 9, sz = 0x20 + 24; uint64_t off = 0, row = RT_W * 4u;
    memcpy(l, &tag, 4); memcpy(l + 4, &sz, 4); memcpy(l + 12, &bref, 4); memcpy(l + 16, &off, 8); memcpy(l + 24, &row, 8);
    uint8_t *d = l + 0x20; d[0] = 2; d[2] = 80; uint32_t w = RT_W, h = RT_H; memcpy(d + 4, &w, 4); memcpy(d + 8, &h, 4); d[16] = 1; d[18] = 1;
    va_write(VA_PAY + pay_off, l, sz); entry(ref, LAGFX_A64_OBJ_LINEAR_TEX, sz, VA_PAY + pay_off);
}
static void fill_lbuf(uint32_t v) { uint32_t a[RT_W * RT_H]; for (uint32_t i = 0; i < RT_W * RT_H; i++) a[i] = v; va_write((uint64_t)VPN_LBUF << 14, a, sizeof a); }
static void test_linear_sampled(void) {
    g_res_tex = g_res_lin = true;
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_task_entry_t *t;
    lagfx_protocol_t *p = new_task(&t);
    uint8_t img[RT_W * RT_H * 4];
    const uint32_t vb[3] = { REF_BUF_POS, REF_BUF_UV, REF_BUF_MAT }, lt[1] = { REF_LTEX };
    const uint32_t PA = 0xff112233u, PB = 0xff445566u;
    put_buffer_object(REF_LBUF, 0x260, 0x4000, VPN_LBUF);
    put_linear(REF_LTEX, 0x600, REF_LBUF);
    put_linear(REF_LRT, 0x640, REF_LBUF);
    fill_lbuf(PA);
    /* (1) */
    cb_reset(); cb_pass(REF_TEX2, 2, B4); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE_TEX); cb_bufs(125, 0, vb, 3); cb_refs(114, 0, lt, 1); cb_draw(4, 0, 4);
    exec(p, 1, cb_finish());
    assert(RUNC(p).draws == 1 && PIPE(p).desc_unbound == 0);
    assert(lagfx_a64_exec_readback_texture(p, t, REF_TEX2, img, sizeof img, NULL, NULL) && px(img, 8, 8) == PA && px(img, 0, 15) == PA);
    /* (2) guest rewrites the buffer between execs */
    fill_lbuf(PB);
    cb_reset(); cb_pass(REF_TEX2, 2, B4); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE_TEX); cb_bufs(125, 0, vb, 3); cb_refs(114, 0, lt, 1); cb_draw(4, 0, 4);
    exec(p, 1, cb_finish());
    assert(RUNC(p).draws == 2 && lagfx_a64_exec_readback_texture(p, t, REF_TEX2, img, sizeof img, NULL, NULL) && px(img, 8, 8) == PB);
    /* (3) one exec: pass 1 renders GREEN into REF_LRT (its result is pushed into the shared buffer), pass 2 samples REF_LTEX: it must show the new bytes */
    uint64_t rs0 = lagfx_a64_res_stats(p)->linear_resync;
    cb_reset(); cb_pass(REF_LRT, 2, G4); cb_pass(REF_TEX2, 2, B4); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE_TEX); cb_bufs(125, 0, vb, 3); cb_refs(114, 0, lt, 1); cb_draw(4, 0, 4);
    exec(p, 1, cb_finish());
    assert(RUNC(p).draws == 3 && PIPE(p).desc_linear_stale == 0);
    assert(lagfx_a64_exec_readback_texture(p, t, REF_TEX2, img, sizeof img, NULL, NULL) && px(img, 8, 8) == GREEN && px(img, 15, 15) == GREEN);
    assert(lagfx_a64_res_stats(p)->linear_resync == rs0 + 1);
    lagfx_a64_exec_drop_task(p, 1);
    lagfx_a64_exec_free(p);
    pfree(p);
    g_res_tex = g_res_lin = false;
    puts("PASS: a linear (buffer backed) texture is sampled: current at the first draw, follows a guest rewrite between execs and a render push inside one exec (re-pulled outside the pass)");
}

static void test_invalidation(void) {
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_task_entry_t *t;
    lagfx_protocol_t *p = new_task(&t);
    uint8_t img[RT_W * RT_H * 4];
    uint32_t live; uint64_t cr, de;
    tri_exec(p, REF_PIPE_TRI, 3, 3, false);
    assert(PIPE(p).resolved == 1 && PIPE(p).pipes_built == 1);
    uint32_t centre;
    assert(lagfx_a64_exec_readback_texture(p, t, REF_TEX, img, sizeof img, NULL, NULL));
    centre = px(img, 8, 8);
    /* (a) the guest rewrites the serialized payload IN PLACE (the 12-byte entry is unchanged): the pipeline is replaced at the next set-pipeline */
    uint32_t tri[2] = { REF_FN_TRI_A, REF_FN_TRI_B };
    put_pipeline(REF_PIPE_TRI, 0x500, tri, 2, 77);                 /* same size, other bytes */
    tri_exec(p, REF_PIPE_TRI, 3, 3, false);
    assert(PIPE(p).pipe_replaced == 1 && PIPE(p).resolved == 2 && PIPE(p).pipes_built == 2 && RUNC(p).draws == 2);
    assert(lagfx_a64_exec_readback_texture(p, t, REF_TEX, img, sizeof img, NULL, NULL) && px(img, 8, 8) == centre);
    lagfx_a64_exec_object_counts(p, &live, &cr, &de);
    assert(de >= 1);                                                /* the old pipeline object was released ... */
    assert(lagfx_a64_exec_deferred_count(p) == 0);                  /* ... and its VkPipeline went through the deferred list, which the finished exec has flushed */
    tri_exec(p, REF_PIPE_TRI, 3, 3, false);
    assert(PIPE(p).pipe_replaced == 1 && PIPE(p).resolved == 2 && PIPE(p).pipes_built == 2);   /* unchanged payload: nothing rebuilt */
    /* (b) the guest writes a NEW 12-byte entry (other address): the table replaces the object */
    uint32_t sz = put_pipeline(REF_PIPE_TRI2, 0x900, tri, 2, 5);
    entry(REF_PIPE_TRI, LAGFX_A64_OBJ_SERIALIZED, sz, VA_PAY + 0x900);
    tri_exec(p, REF_PIPE_TRI, 3, 3, false);
    assert(PIPE(p).resolved >= 3 && PIPE(p).pipes_built == 3 && RUNC(p).draws == 4);   /* (the second entry REF_PIPE_TRI2 is in the resource list and resolves too) */
    /* (c) a pending submission (the wait reports a timeout) keeps everything alive: dropping the task while it is "running" parks the objects */
    lagfx_vk_test_inject_fence_result(VK_TIMEOUT);
    tri_exec(p, REF_PIPE_TRI, 3, 3, false);
    lagfx_vk_test_inject_fence_result(0x7fffffff);
    assert(RUNC(p).submit_pending == 1);
    lagfx_a64_exec_drop_task(p, 1);
    assert(lagfx_a64_exec_deferred_count(p) >= 2);                  /* objects (and their VkPipelines) wait for the fence, not destroyed under it */
    struct timespec ts = { 0, 2 * 1000000 };
    for (int i = 0; i < 2000 && lagfx_a64_exec_poll(p); i++) nanosleep(&ts, NULL);
    assert(lagfx_a64_exec_poll(p) == 0 && lagfx_a64_exec_deferred_count(p) == 0);   /* released once the fence signalled */
    lagfx_a64_exec_object_counts(p, &live, &cr, &de);
    assert(live == 0 && cr == de);
    lagfx_a64_exec_free(p);
    pfree(p);
    puts("PASS: cache invalidation: payload rewritten in place, entry replaced, release deferred behind a pending fence");
}
#endif

/* ---- capture (LAGFX_ARM64_DUMP) ---- */
static char g_dir[256];
static void mkdtemp_dir(void) { snprintf(g_dir, sizeof g_dir, "/tmp/lagfx-dump-XXXXXX"); assert(mkdtemp(g_dir)); }
static size_t file_size(const char *dir, const char *name) {
    char p[600]; snprintf(p, sizeof p, "%s/%s", dir, name);
    struct stat st;
    return stat(p, &st) == 0 ? (size_t)st.st_size : (size_t)-1;
}
static char *read_all(const char *dir, const char *name) {
    char p[600]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    char *b = calloc(1, 1 << 16);
    size_t n = fread(b, 1, (1 << 16) - 1, f);
    b[n] = 0;
    fclose(f);
    return b;
}
static bool dir_has(const char *dir, const char *prefix, char *found, size_t cap) {
    DIR *d = opendir(dir);
    struct dirent *e;
    bool ok = false;
    while (d && (e = readdir(d))) if (!strncmp(e->d_name, prefix, strlen(prefix))) { snprintf(found, cap, "%s", e->d_name); ok = true; break; }
    if (d) closedir(d);
    return ok;
}

static void dump_exec(lagfx_protocol_t *p) {
    cb_reset(); cb_pass(REF_TEX, 2, (const double[4]){ 0, 1, 0, 1 });
    cb_pipeline(REF_PIPE_TEX);
    { const uint32_t vb[3] = { REF_BUF_POS, REF_BUF_UV, REF_BUF_MAT }, tx[1] = { REF_TEX }, sm[1] = { 0 }; cb_bufs(125, 0, vb, 3); cb_refs(114, 0, tx, 1); cb_refs(112, 0, sm, 1); }
    cb_draw(4, 0, 4);
    cb_pipeline(REF_PIPE_TRI); cb_draw(3, 0, 3);
    exec(p, 1, cb_finish());
}

static void test_dump(void) {
    g_res_tex = true;
    unsetenv("LAGFX_ARM64_EXEC");
    setenv("LAGFX_ARM64_DUMP", "1", 1);
    mkdtemp_dir();
    setenv("LAGFX_ARM64_DUMP_DIR", g_dir, 1);
    lagfx_a64_dump_reset();
    /* decode-only: no LAGFX_ARM64_EXEC, no Vulkan involvement */
    lagfx_task_entry_t *t;
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    t = def_task(p, 1); (void)t;
    build_objects();
    dump_exec(p);
    assert(p->a64_exec == NULL && p->diag2.exec.run.execs == 0);                          /* nothing executed */
    assert(PIPE(p).dump_objects >= 6 && PIPE(p).dump_files >= 4 && PIPE(p).dump_draws == 2 && PIPE(p).dump_bytes > g_tri_n);
    char f[300];
    assert(dir_has(g_dir, "pso_1_7_", f, sizeof f) && dir_has(g_dir, "pso_1_3_", f, sizeof f) && dir_has(g_dir, "lib_", f, sizeof f));
    char *idx = read_all(g_dir, "index.txt");
    assert(idx && strstr(idx, "obj task=1 ref=7") && strstr(idx, "truncated=0") && strstr(idx, "pso task=1 ref=7 function_refs=[8,9]") && strstr(idx, "fn task=1 ref=8")
           && strstr(idx, "draw n=0") && strstr(idx, "vbuf[0:10@0 1:11@0 2:12@0]") && strstr(idx, "ftex[0:1]") && strstr(idx, "fsamp[]") /* ref 0 = empty slot, not listed */);
    assert(strstr(idx, "functions=[viewport_to_ndc(stage=1,") && strstr(idx, "functions=[triangle_vertex(stage=1,"));
    /* the file holds exactly the guest bytes */
    char lib[300];
    assert(dir_has(g_dir, "lib_", lib, sizeof lib));
    assert(file_size(g_dir, lib) != (size_t)-1);
    free(idx);
    /* the same objects again: not dumped twice */
    uint64_t before = PIPE(p).dump_objects;
    dump_exec(p);
    assert(PIPE(p).dump_objects == before && PIPE(p).dump_draws == 2 + 2);
    pfree(p);

    /* caps: a per-file cap below the library size writes .TRUNCATED and says so; the total cap stops files but the index still lists every blob */
    mkdtemp_dir();
    setenv("LAGFX_ARM64_DUMP_DIR", g_dir, 1);
    setenv("LAGFX_ARM64_DUMP_MAX_FILE", "100", 1);
    setenv("LAGFX_ARM64_DUMP_MAX_TOTAL", "250", 1);
    lagfx_a64_dump_reset();
    p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    def_task(p, 1); build_objects();
    dump_exec(p);
    assert(dir_has(g_dir, "lib_", f, sizeof f) && strstr(f, ".TRUNCATED") && file_size(g_dir, f) == 100);
    printf("INFO: capped run: truncated=%llu dropped=%llu bytes=%llu files=%llu\n", (unsigned long long)PIPE(p).dump_truncated, (unsigned long long)PIPE(p).dump_skipped_cap, (unsigned long long)PIPE(p).dump_bytes, (unsigned long long)PIPE(p).dump_files); fflush(stdout);
    assert(PIPE(p).dump_truncated >= 1 && PIPE(p).dump_skipped_cap >= 1 && PIPE(p).dump_bytes <= 250);
    idx = read_all(g_dir, "index.txt");
    assert(idx && strstr(idx, "truncated=1") && strstr(idx, "saved_size=100") && strstr(idx, "saved_size=0") && strstr(idx, "full_size=6652"));
    free(idx); pfree(p);

    /* no overwrite, no symlink following: pre-create what would be written */
    mkdtemp_dir();
    char first[300], victim[300];
    {   /* learn a library file name from a normal run in another dir, then plant a symlink with that name */
        char d2[256]; snprintf(d2, sizeof d2, "/tmp/lagfx-dump-XXXXXX"); assert(mkdtemp(d2));
        setenv("LAGFX_ARM64_DUMP_DIR", d2, 1);
        unsetenv("LAGFX_ARM64_DUMP_MAX_FILE"); unsetenv("LAGFX_ARM64_DUMP_MAX_TOTAL");
        lagfx_a64_dump_reset();
        p = mk(LAGFX_PROTO_ARM64_VMAPPLE); def_task(p, 1); build_objects(); dump_exec(p); pfree(p);
        assert(dir_has(d2, "lib_", first, sizeof first));
    }
    snprintf(victim, sizeof victim, "%s/victim.txt", g_dir);
    { FILE *v = fopen(victim, "wb"); fputs("KEEP", v); fclose(v); }
    char lnk[600]; snprintf(lnk, sizeof lnk, "%s/%s", g_dir, first);
    assert(symlink(victim, lnk) == 0);
    setenv("LAGFX_ARM64_DUMP_DIR", g_dir, 1);
    lagfx_a64_dump_reset();
    p = mk(LAGFX_PROTO_ARM64_VMAPPLE); def_task(p, 1); build_objects(); dump_exec(p); pfree(p);
    char *vk = read_all(g_dir, "victim.txt");
    assert(vk && !strcmp(vk, "KEEP"));                                                       /* the symlink was not followed, the file not overwritten */
    free(vk);
    /* a directory that is a symlink, and one that does not exist: logged once, capture keeps logging, nothing written */
    char sl[300]; snprintf(sl, sizeof sl, "%s/linkdir", g_dir);
    assert(symlink(g_dir, sl) == 0);
    setenv("LAGFX_ARM64_DUMP_DIR", sl, 1);
    lagfx_a64_dump_reset();
    p = mk(LAGFX_PROTO_ARM64_VMAPPLE); def_task(p, 1); build_objects(); dump_exec(p);
    assert(PIPE(p).dump_objects >= 6 && PIPE(p).dump_files == 0);
    pfree(p);
    setenv("LAGFX_ARM64_DUMP_DIR", "/nonexistent-lagfx-dir", 1);
    lagfx_a64_dump_reset();
    p = mk(LAGFX_PROTO_ARM64_VMAPPLE); def_task(p, 1); build_objects(); dump_exec(p);
    assert(PIPE(p).dump_objects >= 6 && PIPE(p).dump_files == 0);
    pfree(p);
    unsetenv("LAGFX_ARM64_DUMP_DIR");
    lagfx_a64_dump_reset();

#ifdef LAGFX_HAVE_VULKAN
    if (g_dev->vk && g_dev->vk->initialized) {   /* both on: the capture and the execution both happen */
        setenv("LAGFX_ARM64_EXEC", "1", 1);
        mkdtemp_dir();
        setenv("LAGFX_ARM64_DUMP_DIR", g_dir, 1);
        lagfx_a64_dump_reset();
        p = mk(LAGFX_PROTO_ARM64_VMAPPLE); def_task(p, 1); build_objects();
        cb_reset(); cb_pass(REF_TEX, 2, G4); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE_TRI); cb_draw(3, 0, 3); exec(p, 1, cb_finish());
        if (0) printf("INFO: nt=%llu np=%llu ni=%llu bad=%llu nopipe=%llu skipped=%llu ou=%llu\n", (unsigned long long)RUNC(p).no_target, (unsigned long long)RUNC(p).no_pass, (unsigned long long)RUNC(p).pipeline_unresolved,(unsigned long long)RUNC(p).bad_rec,(unsigned long long)RUNC(p).no_pipeline,(unsigned long long)RUNC(p).draws_skipped,(unsigned long long)RUNC(p).obj_unsupported); printf("INFO: both on: objs=%llu files=%llu draws=%llu resolved=%llu failed=%llu\n", (unsigned long long)PIPE(p).dump_objects, (unsigned long long)PIPE(p).dump_files, (unsigned long long)RUNC(p).draws, (unsigned long long)PIPE(p).resolved, (unsigned long long)PIPE(p).resolve_failed);
        assert(PIPE(p).dump_objects >= 6 && PIPE(p).dump_files >= 4 && RUNC(p).draws == 1 && PIPE(p).resolved >= 2);
        lagfx_a64_exec_free(p); pfree(p);
        unsetenv("LAGFX_ARM64_EXEC");
    }
#endif
    unsetenv("LAGFX_ARM64_DUMP_DIR"); unsetenv("LAGFX_ARM64_DUMP");
    g_res_tex = false;
    lagfx_a64_dump_reset();
    puts("PASS: capture: decode-only (no exec, no Vulkan), full payload files + index, caps/.TRUNCATED, no overwrite/symlink, bad dir, with exec");
}

/* DECODE-ONLY (LAGFX_ARM64_EXEC unset): the resource list holds the render target only; the pipeline is named by tag 116 in the Render segment. The walk of the
 * segment must still capture the pipeline and the functions it names. */
static void test_dump_decode_only_tag116(void) {
    unsetenv("LAGFX_ARM64_EXEC");
    setenv("LAGFX_ARM64_DUMP", "1", 1);
    mkdtemp_dir();
    setenv("LAGFX_ARM64_DUMP_DIR", g_dir, 1);
    lagfx_a64_dump_reset();
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    def_task(p, 1); build_objects();
    cb_reset(); cb_pass(REF_TEX, 2, G4); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE_TRI); cb_draw(3, 0, 3);
    uint32_t len = cb_finish();
    uint32_t w[3 + 2 + 4], n = 0;
    w[n++] = 1; w[n++] = 1; w[n++] = 1; w[n++] = REF_TEX; w[n++] = 0;
    uint64_t off = VA_CMD, l = len;
    memcpy(w + n, &off, 8); n += 2; memcpy(w + n, &l, 8); n += 2;
    submit(p, 0x43, 0, w, n);
    assert(p->a64_exec == NULL && RUNC(p).execs == 0);                       /* nothing executed, no resolver ran */
    char f[300];
    assert(dir_has(g_dir, "pso_1_3_", f, sizeof f) && dir_has(g_dir, "lib_", f, sizeof f));
    char *idx = read_all(g_dir, "index.txt");
    assert(idx && strstr(idx, "obj task=1 ref=3 type=7") && strstr(idx, "pso task=1 ref=3 function_refs=[5,6]") && strstr(idx, "fn task=1 ref=5"));
    free(idx);
    pfree(p);
    unsetenv("LAGFX_ARM64_DUMP_DIR"); unsetenv("LAGFX_ARM64_DUMP");
    lagfx_a64_dump_reset();
    puts("PASS: decode-only capture of a pipeline + functions named only by tag 116 (EXEC unset)");
}

#ifdef LAGFX_HAVE_VULKAN
/* The pipeline (type 7) and its functions (type 6) are reached ONLY through tag 116 and the resolver: the resource list holds the render target and nothing else.
 * Capture must still produce pso + library files and the obj / pso / fn index lines. */
static void test_dump_via_resolver(void) {
    if (!g_dev->vk || !g_dev->vk->initialized) return;
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    setenv("LAGFX_ARM64_DUMP", "1", 1);
    mkdtemp_dir();
    setenv("LAGFX_ARM64_DUMP_DIR", g_dir, 1);
    lagfx_a64_dump_reset();
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    def_task(p, 1); build_objects();
    cb_reset(); cb_pass(REF_TEX, 2, G4); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE_TRI); cb_draw(3, 0, 3);
    uint32_t len = cb_finish();
    uint32_t w[3 + 2 + 4], n = 0;
    w[n++] = 1; w[n++] = 1; w[n++] = 1; w[n++] = REF_TEX; w[n++] = 0;
    uint64_t off = VA_CMD, l = len;
    memcpy(w + n, &off, 8); n += 2; memcpy(w + n, &l, 8); n += 2;
    submit(p, 0x43, 0, w, n);
    assert(RUNC(p).draws == 1 && PIPE(p).resolved == 1);                       /* the draw happened: the pipeline resolved although no resource list named it */
    assert(PIPE(p).dump_objects >= 2 && PIPE(p).dump_files >= 2);
    char f[300];
    assert(dir_has(g_dir, "pso_1_3_", f, sizeof f) && dir_has(g_dir, "lib_", f, sizeof f));
    char *idx = read_all(g_dir, "index.txt");
    assert(idx && strstr(idx, "obj task=1 ref=3 type=7") && strstr(idx, "pso task=1 ref=3 function_refs=[5,6]") && strstr(idx, "fn task=1 ref=5") && strstr(idx, "truncated=0"));
    free(idx);
    uint64_t before = PIPE(p).dump_objects;                                     /* the same pipeline again: the seen set keeps it to one capture */
    submit(p, 0x43, 0, w, n);
    assert(PIPE(p).dump_objects == before);
    lagfx_a64_exec_free(p); pfree(p);
    unsetenv("LAGFX_ARM64_DUMP_DIR"); unsetenv("LAGFX_ARM64_DUMP"); unsetenv("LAGFX_ARM64_EXEC");
    lagfx_a64_dump_reset();
    puts("PASS: capture of a pipeline + functions reached only through the resolver (tag 116), not the resource list");
}

/* Real descriptors from the device dump (pso_2_12: BGRA8Unorm, dst factors 5/5 without a key-2 token; pso_2_135: RGBA16Float, key 2 = 1 plus dst 5/5). The blend
 * keys follow MTLRenderPipelineColorAttachmentDescriptor's property order; see lagfx_a64_blend_t for what is and is not proven. */
static void test_pdesc_blend(void) {
    static const uint8_t b12[] = { 0x0e,0x00,0x00,0x00,0xc8,0x00,0x00,0x00,0x0c,0x00,0x00,0x00,0xb8,0x00,0x00,0x00,0x04,0x03,0x04,0x1c,0x00,0x00,0x00,0x08,0x04,0x80,0x00,0x00,0x00,0x01,0x04,0x0b,0x00,0x00,0x00,0x02,0x04,0x0a,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x04,0x10,0x00,0x00,0x00,0x01,0x04,0x3c,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x0c,0x00,0x00,0x00,0x19,0x00,0x00,0x00,0x02,0x00,0x04,0x00,0x00,0x00,0x00,0x01,0x04,0x1f,0x00,0x00,0x00,0x03,0x00,0x04,0x01,0x00,0x00,0x00,0x01,0x04,0x1d,0x00,0x00,0x00,0x02,0x04,0x10,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x0c,0x00,0x00,0x00,0x19,0x00,0x00,0x00,0x02,0x00,0x04,0x00,0x00,0x00,0x00,0x03,0x04,0x20,0x00,0x00,0x00,0x02,0x00,0x04,0x00,0x00,0x00,0x00,0x03,0x04,0x20,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x0c,0x00,0x00,0x00,0x25,0x00,0x00,0x00,0x04,0x00,0x04,0x00,0x00,0x00,0x00,0x01,0x04,0x50,0x00,0x00,0x00,0x04,0x04,0x05,0x00,0x00,0x00,0x07,0x04,0x05,0x00,0x00,0x00,0x03,0x00,0x04,0x01,0x00,0x00,0x00,0x01,0x04,0x73,0x00,0x00,0x00,0x09,0x04,0x00,0x00,0x00,0x00 };
    static const uint8_t b135[] = { 0x0e,0x00,0x00,0x00,0xd0,0x00,0x00,0x00,0x87,0x00,0x00,0x00,0xbe,0x00,0x00,0x00,0x04,0x03,0x04,0x1c,0x00,0x00,0x00,0x08,0x04,0x80,0x00,0x00,0x00,0x01,0x04,0x1b,0x00,0x00,0x00,0x02,0x04,0x86,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x04,0x10,0x00,0x00,0x00,0x01,0x04,0x3c,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x0c,0x00,0x00,0x00,0x19,0x00,0x00,0x00,0x02,0x00,0x04,0x00,0x00,0x00,0x00,0x01,0x04,0x1d,0x00,0x00,0x00,0x03,0x00,0x04,0x01,0x00,0x00,0x00,0x01,0x04,0x1d,0x00,0x00,0x00,0x02,0x04,0x08,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x0c,0x00,0x00,0x00,0x19,0x00,0x00,0x00,0x02,0x00,0x04,0x00,0x00,0x00,0x00,0x03,0x04,0x10,0x00,0x00,0x00,0x02,0x00,0x04,0x00,0x00,0x00,0x00,0x03,0x04,0x10,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x0c,0x00,0x00,0x00,0x2b,0x00,0x00,0x00,0x05,0x00,0x04,0x00,0x00,0x00,0x00,0x01,0x04,0x73,0x00,0x00,0x00,0x02,0x04,0x01,0x00,0x00,0x00,0x04,0x04,0x05,0x00,0x00,0x00,0x07,0x04,0x05,0x00,0x00,0x00,0x03,0x00,0x04,0x01,0x00,0x00,0x00,0x01,0x04,0x73,0x00,0x00,0x00,0x09,0x04,0x00,0x00,0x00,0x00,0x00,0x00 };
    lagfx_a64_pdesc_t d;
    assert(lagfx_a64_pdesc_decode(b12, sizeof b12, NULL, NULL, &d) && d.color0.found && d.color0.pixfmt == 80u);
    assert(!lagfx_a64_blend_on(&d.color0));   /* factors without key 2: blending off */
    assert(d.color0.any_blend_token && !d.color0.enabled_token && d.color0.src_rgb == 1u && d.color0.dst_rgb == 5u && d.color0.dst_a == 5u && d.color0.mask == 0xfu);
    assert(lagfx_a64_pdesc_decode(b135, sizeof b135, NULL, NULL, &d) && d.color0.found && d.color0.pixfmt == 115u && d.color0.enabled_token && lagfx_a64_blend_on(&d.color0) && d.color0.dst_rgb == 5u);
    assert(d.color0.found1 && d.color0.pixfmt1 == 115u);
    assert(d.color0.mask1 == 0u);   /* the dump's second attachment record carries writeMask 0 */
    assert(!d.color0.a1.enabled_token && !d.color0.a1.any_blend_token && d.color0.a1.src_rgb == 1u && d.color0.a1.dst_rgb == 0u);   /* real dump: attachment 1 has no blend tokens */
    {   /* attachment 1 with its own blend tokens: `02 04 1`, `03 04 4`, `04 04 5` placed before its writeMask (keys increasing) */
        uint8_t u[sizeof b135 + 18], *w = u; const uint8_t extra[18] = { 2,4,1,0,0,0, 3,4,4,0,0,0, 4,4,5,0,0,0 };
        size_t mk = 0; for (size_t i = sizeof b135 - 6; i > 0; i--) if (b135[i] == 0x09 && b135[i + 1] == 0x04) { mk = i; break; }
        assert(mk);
        memcpy(w, b135, mk); memcpy(w + mk, extra, 18); memcpy(w + mk + 18, b135 + mk, sizeof b135 - mk);
        uint32_t L = lagfx_le32(u + 12) + 18u; u[12] = (uint8_t)L; u[13] = (uint8_t)(L >> 8);
        lagfx_a64_pdesc_t e;
        assert(lagfx_a64_pdesc_decode(u, sizeof u, NULL, NULL, &e) && e.color0.found1);
        assert(e.color0.a1.enabled_token && e.color0.a1.enabled_value == 1u && e.color0.a1.src_rgb == 4u && e.color0.a1.dst_rgb == 5u && e.color0.mask1 == 0u);
    }
    {   /* structural checks: an inconsistent length, a truncated body and an explicit key-2 value 0 are not mis-decoded */
        uint8_t t[sizeof b135]; memcpy(t, b135, sizeof t);
        for (size_t k = sizeof t - 30; k > 16; k--) if (t[k] == 0x0c && !t[k + 1] && t[k + 9] == 0 && t[k + 10] == 4) { t[k + 4]++; break; }   /* the LAST chunk: len != 6*count+13 */
        assert(lagfx_a64_pdesc_decode(t, sizeof t, NULL, NULL, &d) && !d.color0.found);
        t[12] = (uint8_t)(b135[12] - 40);   /* header says the body is shorter than the record */
        assert(lagfx_a64_pdesc_decode(t, sizeof t, NULL, NULL, &d) && !d.color0.found);
        memcpy(t, b135, sizeof t);
        for (size_t k = sizeof t - 30; k > 16; k--) if (t[k] == 0x02 && t[k + 1] == 0x04 && t[k + 2] == 0x01 && t[k + 3] == 0) { t[k + 2] = 0; break; }
        assert(lagfx_a64_pdesc_decode(t, sizeof t, NULL, NULL, &d) && d.color0.found && d.color0.enabled_token && d.color0.enabled_value == 0u);
    }
    /* a descriptor with only the pixel format token: no blend token at all -> blending certainly off */
    static const uint8_t bplain[] = { 0x0e,0,0,0, 0x32,0,0,0, 1,0,0,0, 0x22,0,0,0,  0x0c,0,0,0, 0x19,0,0,0, 0x02, 0x00,0x04,0,0,0,0, 0x01,0x04,0x73,0,0,0, 0x03,0x00,0x04,0x01,0,0,0, 0x01,0x04,0x73,0,0,0 };
    assert(lagfx_a64_pdesc_decode(bplain, sizeof bplain, NULL, NULL, &d) && d.color0.found && d.color0.pixfmt == 115u && !d.color0.any_blend_token);
    /* garbage / vertex-only: nothing found, Metal defaults */
    static const uint8_t bnone[] = { 0x0e,0,0,0, 0x10,0,0,0, 1,0,0,0, 0,0,0,0 };
    assert(lagfx_a64_pdesc_decode(bnone, sizeof bnone, NULL, NULL, &d) && !d.color0.found && d.color0.src_rgb == 1u && d.color0.mask == 0xfu);
    puts("PASS: ARM64 descriptor colour attachment 0 (pixel format + blend tokens) decoded from real dump descriptors");
}

/* Device-capability gate (vulkan/caps.h): a translated module that declares a capability the device was not created with is not built; the draw is skipped. The
 * 'feature missing' seam lagfx_vk_caps_inject_missing marks capabilities as missing on the lavapipe device (which reports everything). */
static void test_missing_caps(void) {
    /* pure: the OpCapability / version scan */
    uint32_t m[] = { 0x07230203u, 0x00010400u, 0, 8, 0, 0x00020011u, 1u, 0x00020011u, 11u, 0x00020011u, 39u, 0x00020011u, 4448u, 0x00020011u, 4441u, 0x0004000fu, 0, 1, 0x6e69616du };
    uint32_t need = lagfx_spv_required_caps(m, sizeof m);
    assert(need == (LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT64) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT8) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SB8) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_VARPTR_SB) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SPIRV14)));
    {   /* capabilities inferred from USE, with none declared: a half Input variable and a StorageBuffer with a uchar member */
        /* encoding: OpTypeFloat (22) len 3, OpTypeInt (21) len 4, OpTypePointer (32) len 4, OpTypeStruct (30) len 3 */
        uint32_t a[] = { 0x07230203u, 0x00010400u, 0, 8, 0,
            0x00030016u, 1u, 16u,                     /* %1 half */
            0x00040020u, 2u, 1u, 1u };                /* %2 = Input ptr %1 */
        uint32_t need_io = lagfx_spv_required_caps(a, sizeof a);
        assert(need_io & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_FLOAT16));
        assert(need_io & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_IO16) && !(need_io & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SB16)));
        uint32_t b[] = { 0x07230203u, 0x00010400u, 0, 8, 0,
            0x00040015u, 1u, 8u, 0u,                  /* %1 uchar */
            0x0003001eu, 2u, 1u,                      /* %2 struct { %1 } */
            0x00040020u, 3u, 12u, 2u };               /* %3 = StorageBuffer ptr %2 */
        uint32_t need_sb = lagfx_spv_required_caps(b, sizeof b);
        assert(need_sb & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SB8));
        assert(!(need_sb & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SB16)) && !(need_sb & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_UB8)));
        {   /* declare what is used: the module gets OpCapability StorageBuffer8BitAccess (4448) + OpExtension SPV_KHR_8bit_storage below SPIR-V 1.5; a second call changes nothing */
            uint8_t *blob = malloc(sizeof b); memcpy(blob, b, sizeof b); size_t bl = sizeof b;
            assert(lagfx_spv_declare_used_caps(&blob, &bl) && bl == sizeof b + 8u + 28u);
            const uint32_t *nw = (const uint32_t *)blob;
            assert(nw[5] == ((2u << 16) | 17u) && nw[6] == 4448u && (nw[7] & 0xffffu) == 10u && !memcmp(&nw[8], "SPV_KHR_8bit_storage", 20));
            size_t bl2 = bl; assert(lagfx_spv_declare_used_caps(&blob, &bl2) && bl2 == bl);
            free(blob);
        }
    }
    {   /* fail closed: modules that cannot be analysed must not be read as "needs nothing" */
        uint32_t mk;
        uint32_t bad_id[] = { 0x07230203u, 0x00010400u, 0, 4, 0, 0x00030016u, 9u, 16u };   /* type id 9 >= bound 4 */
        assert(!lagfx_spv_required_caps_ex(bad_id, sizeof bad_id, &mk) && mk == LAGFX_VK_CAP_ALL);
        uint32_t huge[] = { 0x07230203u, 0x00010400u, 0, 0x7fffffffu, 0 };
        assert(!lagfx_spv_required_caps_ex(huge, sizeof huge, &mk) && mk == LAGFX_VK_CAP_ALL);
        uint32_t trunc[] = { 0x07230203u, 0x00010400u, 0, 8, 0, 0x00050016u, 1u };   /* length 5 words, 2 present */
        assert(!lagfx_spv_required_caps_ex(trunc, sizeof trunc, &mk) && mk == LAGFX_VK_CAP_ALL);
        assert(!lagfx_spv_required_caps_ex(m, 7, &mk) && mk == LAGFX_VK_CAP_ALL);
        /* half matrix behind a StorageBuffer pointer: reaches the 16-bit scalar through OpTypeMatrix */
        uint32_t mx[] = { 0x07230203u, 0x00010200u, 0, 8, 0,
            0x00030016u, 1u, 16u,        /* %1 half */
            0x00040017u, 2u, 1u, 4u,     /* %2 half4 */
            0x00040018u, 3u, 2u, 4u,     /* %3 matrix of %2 x4 */
            0x0003001eu, 4u, 3u,         /* %4 struct { %3 } */
            0x00040020u, 5u, 12u, 4u };  /* %5 = StorageBuffer ptr %4 */
        assert(lagfx_spv_required_caps_ex(mx, sizeof mx, &mk) && (mk & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SB16)));
        /* declaring on SPIR-V 1.2 adds the capability and SPV_KHR_16bit_storage */
        uint8_t *blob = malloc(sizeof mx); memcpy(blob, mx, sizeof mx); size_t bl = sizeof mx;
        assert(lagfx_spv_declare_used_caps(&blob, &bl) && bl == sizeof mx + 8u + 28u);
        const uint32_t *nw = (const uint32_t *)blob;
        assert(nw[6] == 4433u && !memcmp(&nw[8], "SPV_KHR_16bit_storage", 21));
        free(blob);
        uint8_t *bb = malloc(sizeof bad_id); memcpy(bb, bad_id, sizeof bad_id); size_t bbl = sizeof bad_id;
        assert(!lagfx_spv_declare_used_caps(&bb, &bbl));   /* unanalysable → translation fails instead of passing a blind module */
        free(bb);
    }
    assert(lagfx_spv_required_caps(m, 7) == 0 && lagfx_vk_caps_parse("int64,bogus,float16") == (LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT64) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_FLOAT16)));
    struct lagfx_vk_state *vk = g_dev->vk;
    char line[512];
    lagfx_vk_caps_line(vk, line, sizeof line);
    printf("INFO: %s\n", line);
    assert(strstr(line, "g7-gpu-caps:") && vk->caps_enabled == (vk->caps_enabled & LAGFX_VK_CAP_ALL) && (vk->caps_enabled | vk->caps_missing) == LAGFX_VK_CAP_ALL);
    uint32_t saved_en = vk->caps_enabled, saved_ms = vk->caps_missing;
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    /* baseline: nothing missing, the pipeline resolves and draws */
    lagfx_task_entry_t *t;
    lagfx_protocol_t *p = new_task(&t);
    tri_exec(p, REF_PIPE_TRI, 3, 3, false);
    assert(PIPE(p).resolved == 1 && RUNC(p).draws == 1 && PIPE(p).rf_missing_cap == 0);
    lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); pfree(p);
    /* every capability 'missing': the generated shaders declare at least Int64 / VariablePointers... or SPIR-V 1.4; those pipelines must not be built */
    lagfx_vk_caps_inject_missing(vk, LAGFX_VK_CAP_ALL);
    p = new_task(&t);
    tri_exec(p, REF_PIPE_TRI, 3, 3, false);
    uint64_t total = 0;
    for (unsigned c = 0; c < LAGFX_VK_CAP_COUNT; c++) total += PIPE(p).stub_missing_cap[c];
    printf("INFO: missing-cap: rf_missing_cap=%llu stubs=%llu draws=%llu skipped=%llu resolved=%llu built=%llu\n", (unsigned long long)PIPE(p).rf_missing_cap, (unsigned long long)total,
           (unsigned long long)RUNC(p).draws, (unsigned long long)RUNC(p).draws_skipped, (unsigned long long)PIPE(p).resolved, (unsigned long long)PIPE(p).pipes_built);
    assert(PIPE(p).rf_missing_cap == 1 && total >= 1 && PIPE(p).resolved == 0 && PIPE(p).pipes_built == 0);   /* no shader module, no pipeline: nothing invalid reached Vulkan */
    assert(RUNC(p).draws == 0 && RUNC(p).draws_skipped >= 1 && RUNC(p).submit_failed == 0);
    lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); pfree(p);
    /* only a capability the shaders do not use is missing: unaffected */
    vk->caps_enabled = saved_en, vk->caps_missing = saved_ms;
    uint32_t need_tri = 0;
    {   /* what the triangle shaders need = what the previous run counted */
        p = new_task(&t);
        lagfx_vk_caps_inject_missing(vk, LAGFX_VK_CAP_ALL);
        tri_exec(p, REF_PIPE_TRI, 3, 3, false);
        for (unsigned c = 0; c < LAGFX_VK_CAP_COUNT; c++) if (PIPE(p).stub_missing_cap[c]) need_tri |= LAGFX_VK_CAP_BIT(c);
        lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); pfree(p);
        vk->caps_enabled = saved_en, vk->caps_missing = saved_ms;
    }
    assert(need_tri != 0);
    lagfx_vk_caps_inject_missing(vk, LAGFX_VK_CAP_ALL & ~need_tri);
    p = new_task(&t);
    tri_exec(p, REF_PIPE_TRI, 3, 3, false);
    assert(PIPE(p).rf_missing_cap == 0 && PIPE(p).resolved == 1 && RUNC(p).draws == 1);
    lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); pfree(p);
    vk->caps_enabled = saved_en, vk->caps_missing = saved_ms;
    printf("PASS: device-capability gate: shader needing missing caps (mask 0x%x) -> no pipeline, draw skipped, stub_missing_cap counted; unrelated missing caps do not block\n", need_tri);
}
#endif

int main(void) {
    g_mem = calloc(1, MEM_SIZE);
    g_dev = make_device();
    assert(g_dev);
    test_pdesc(); test_pdesc_blend();
    g_have_fix = load_fixtures();
    if (!g_have_fix) { puts("SKIP: fixtures (triangle.metallib, guest_viewporttondc_vert, login_texturecopy_frag) missing"); lagfx_device_free(g_dev); free(g_mem); return 77; }
    test_dump();
    test_dump_decode_only_tag116();
#ifdef LAGFX_HAVE_VULKAN
    if (g_dev->vk && g_dev->vk->initialized) {
        test_dump_via_resolver();
        test_resolver_triangle_and_topology();
        test_textured_draw();
        test_linear_sampled();
        test_tex_unbound_reasons();
        test_object_slots_reused();
        test_slot_reuse_safety();
        test_object_table_stress_and_full();
        test_invalidation();
        test_missing_caps();
    } else {
        puts("SKIP: no Vulkan device; resolver/binding/invalidation tests not run (pdesc + decode-only capture ran)");
        lagfx_device_free(g_dev); free(g_mem);
        return 0;
    }
#else
    puts("SKIP: built without Vulkan (pdesc + decode-only capture ran)");
#endif
    lagfx_device_free(g_dev);
    free(g_mem);
    return 0;
}
