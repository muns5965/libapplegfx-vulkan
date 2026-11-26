/* render-replay: offline render replay on lavapipe. Real Metal shaders (metallibs + serialized pipeline descriptors of an arm64 GPU dump, see dump-bindings.c)
 * are drawn with SYNTHETIC buffers / textures / samplers / attachments through the library's ARM64 exec path (the default pipeline resolver, exactly as the
 * guest's command stream would reach it), read back, written as raw + PNG and compared pixel by pixel with a small CPU model.
 *
 *   render-replay synth <scene|all|list> [outdir]   synthetic scenes (need a dump for the real shaders: --dump DIR or LAGFX_TEST_DUMP or /tmp/zx/dump)
 *   render-replay dump  [--dump DIR] [task ref]     EXECUTABILITY ONLY: every render PSO of the dump builds a Vulkan pipeline and runs one draw with synthetic
 *                                                   resources. No pixel reference: PASS means "built, one draw recorded, submitted, no validation message".
 *   render-replay capture <dir>                     PENDING: replays draws captured on the device (format: src/protocol/arm64_capture.h of the capture work,
 *                                                   not merged yet). It will fill the same rr_scene_t that the synthetic scenes use (objects + draws + targets).
 *
 * Vulkan validation: LAGFX_VK_VALIDATION=1 (messages are counted; any message fails a scene / smoke run).
 *
 * ---- What a check means (read before trusting a PASS) ----
 *  CPU model:   a hand-written reference of what the translated shader does (vertex: the matrix / attribute maths of the real shader, read from its SPIR-V;
 *               fragment: pass-through / texel fetch / the fbfetch maths). It models the SHADER and the Vulkan rasterization rules (pixel centres, perspective
 *               correct interpolation, clip y up = row 0). It is NOT Apple GPU behaviour: where this file says "CPU model" nothing is claimed about a Metal GPU.
 *  Metal-faithful: only the facts of the Metal API itself (pixel formats, clear values, load actions, blend factors as documented, MTLVertexFormat sizes) are
 *               used as a reference. Nothing about Apple GPU rasterization, rounding of interpolants or fp16 intermediate precision is assumed: those compare
 *               with a tolerance (stated per scene) or on pixels away from primitive edges only.
 *  Every scene has a NEGATIVE test: the same check run against a deliberately wrong reference must fail.
 * SPDX-License-Identifier: MIT */
#include "dispatchers/channel_compute_dispatcher.h"
#include "dispatchers/ring_common.h"
#include "protocol/state.h"
#include "protocol/arm64_exec_decode.h"
#include "protocol/arm64_exec_run.h"
#include "protocol/arm64_objects.h"
#include "protocol/arm64_pipeline.h"
#include "device.h"
#include "libapplegfx-vulkan.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef LAGFX_HAVE_VULKAN
#include "vulkan/instance.h"
extern uint64_t lagfx_vk_validation_msg_count;

/* ---------------------------------------------------------------- guest memory + device ---- */
#define RR_MEM_SIZE (24u << 20)
#define RR_NVPN     1000u
#define SHARED_PFN 1u
#define TABLE_PFN  2u
#define RING_PFN0  4u
#define ROOT_PFN   16u
#define TOP_PFN    17u
#define LEAF_PFN   18u
#define DATA_PFN0  32u
#define VA_OBJ     0x100u          /* object table: ref * 12 (refs < 4096) */
#define VA_HEAP    0x10000u        /* bump allocator start */
#define RR_MAX_REF 4000u

static uint8_t *g_mem; static lagfx_device_t *g_dev; static uint32_t g_wp; static uint64_t g_heap;
static bool m_rd(void *o, uint64_t gpa, uint64_t len, void *dst) { (void)o; if (gpa + len > RR_MEM_SIZE) return false; memcpy(dst, g_mem + gpa, len); return true; }
static bool m_wr(void *o, uint64_t gpa, uint64_t len, const void *src) { (void)o; if (gpa + len > RR_MEM_SIZE) return false; memcpy(g_mem + gpa, src, len); return true; }
static void m_irq(void *o, uint32_t v) { (void)o; (void)v; }
static lagfx_task_t *m_ct(void *o, uint64_t s, void **p) { (void)o; (void)s; (void)p; return NULL; }
static void m_dt(void *o, lagfx_task_t *t) { (void)o; (void)t; }
static bool m_map(void *a, lagfx_task_t *t, uint64_t o, const lagfx_physical_range_t *r, size_t c, bool ro) { (void)a; (void)t; (void)o; (void)r; (void)c; (void)ro; return true; }
static bool m_unmap(void *a, lagfx_task_t *t, uint64_t o, uint64_t l) { (void)a; (void)t; (void)o; (void)l; return true; }
static void put32(uint64_t gpa, uint32_t v) { memcpy(g_mem + gpa, &v, 4); }

static bool rr_open(void) {
    if (g_dev) return true;
    g_mem = calloc(1, RR_MEM_SIZE);
    lagfx_device_descriptor_t d; memset(&d, 0, sizeof d);
    d.shell.opaque = (void *)0xfeedu;
    d.shell.create_task = m_ct; d.shell.destroy_task = m_dt; d.shell.map_memory = m_map; d.shell.unmap_memory = m_unmap;
    d.shell.read_memory = m_rd; d.shell.write_memory = m_wr; d.shell.raise_interrupt = m_irq;
    d.thread_count = 2;
    char *err = NULL;
    g_dev = lagfx_device_new(&d, &err);
    return g_dev && g_dev->vk && g_dev->vk->initialized;
}
static void rr_close(void) { if (g_dev) lagfx_device_free(g_dev); free(g_mem); g_dev = NULL; g_mem = NULL; }

static void va_write(uint64_t va, const void *src, uint64_t n) {
    const uint8_t *s = src;
    while (n) {
        uint32_t vpn = (uint32_t)(va >> 14), in = (uint32_t)(va & 0x3fff); uint64_t c = 0x4000 - in < n ? 0x4000 - in : n;
        if (vpn >= RR_NVPN) { fprintf(stderr, "render-replay: guest VA 0x%llx out of the synthetic address space\n", (unsigned long long)va); exit(2); }
        memcpy(g_mem + ((uint64_t)(DATA_PFN0 + vpn) << 14) + in, s, c);
        va += c; s += c; n -= c;
    }
}
static uint64_t va_alloc(uint64_t n, uint64_t align) { g_heap = (g_heap + align - 1) & ~(align - 1); uint64_t a = g_heap; g_heap += n; return a; }

/* ---------------------------------------------------------------- scene description (the API the capture loader will fill too) ---- */
enum { RR_BUF = 1, RR_TEX, RR_SAMP, RR_FN, RR_PSO };
typedef struct {
    uint32_t ref, kind;
    const void *data; uint32_t len;     /* BUF: contents (len bytes, zero padded to len_alloc); FN: metallib; PSO: the serialized descriptor, tag 0xe; TEX: w*h*bpp pixels or NULL */
    uint32_t alloc;                     /* BUF: allocation size (>= len) */
    uint32_t w, h, pixfmt;              /* TEX: MTLPixelFormat (70 RGBA8Unorm, 80 BGRA8Unorm, 115 RGBA16Float) */
    uint32_t samp_state;                /* SAMP: serialized state word (arm64_sampler.h) */
} rr_obj_t;
typedef struct { uint32_t index, ref; uint64_t off; } rr_slot_t;
#define RR_MAXSLOT 16
typedef struct {
    uint32_t pso;                       /* object ref */
    uint32_t start, count;              /* non-indexed (index_ref == 0) */
    uint32_t index_ref, index_u32, index_off;   /* indexed: tag 7 */
    rr_slot_t vb[RR_MAXSLOT], fb[RR_MAXSLOT], ft[RR_MAXSLOT], fs[RR_MAXSLOT]; uint32_t nvb, nfb, nft, nfs;
} rr_draw_t;
typedef struct {
    const char *name;
    uint32_t w, h, ntarget;
    uint32_t target_ref[2]; uint16_t load[2]; double clear[2][4];   /* load: 0 dont care, 1 load, 2 clear (MTLLoadAction) */
    rr_obj_t obj[48]; uint32_t nobj;
    rr_draw_t draw[8]; uint32_t ndraw;
    int vb_override;                    /* >= 0: patch the resolved pipeline's vertex stream to this Metal buffer slot (the descriptor decoder leaves it at 0 when the
                                         * attribute records carry only non-default tokens; see the report: tool-side workaround, the library is NOT changed by it) */
    int patch_attrs;                    /* 1: force the PSO vertex layout float4@0 float2@16 float2@24 half4@32 (pso 93 family, see patch_attrs below) */
} rr_scene_t;
typedef struct {
    bool ran; uint64_t draws, skipped, submit_failed, val_msgs, resolved, built;
    uint32_t w, h, bpp[2]; uint8_t *px[2];   /* readback per target (malloc) */
    char why[160];
} rr_result_t;

static uint32_t rr_bpp(uint32_t fmt) { return fmt == 115 ? 8u : 4u; }
static void rr_obj_free_result(rr_result_t *r) { free(r->px[0]); free(r->px[1]); r->px[0] = r->px[1] = NULL; }

/* ---- guest object builders ---- */
static void entry(uint32_t ref, uint32_t type, uint32_t size, uint64_t addr) {
    uint32_t e[3] = { type | size << 8, (uint32_t)addr, (uint32_t)(addr >> 32) };
    va_write(VA_OBJ + (uint64_t)ref * 12u, e, 12);
}
static void mk_buffer(const rr_obj_t *o) {
    uint32_t alloc = o->alloc > o->len ? o->alloc : o->len; if (alloc < 16u) alloc = 16u;
    alloc = (alloc + 0x3fffu) & ~0x3fffu;
    uint64_t data = va_alloc(alloc, 0x4000);
    if (o->data) va_write(data, o->data, o->len);
    uint64_t pay = va_alloc(16, 16);
    uint8_t h[16] = {0}; uint64_t len = alloc; uint32_t vp = (uint32_t)(data >> 14);
    memcpy(h, &len, 8); memcpy(h + 8, &vp, 4);
    va_write(pay, h, 16);
    entry(o->ref, LAGFX_A64_OBJ_BUFFER, 16, pay);
}
/* texture (mirror layout, as arm64-exec-run-unit.c build_objects): {u64 length, u32 vpage}, layout (mips = 1, mip table: off, row, w, h, d), {tag 0x14, size, ref}, v1 description */
static void mk_texture(const rr_obj_t *o) {
    uint32_t bpp = rr_bpp(o->pixfmt);
    uint64_t tlen = (uint64_t)o->w * o->h * bpp, alloc = (tlen + 0x3fffu) & ~0x3fffull;
    uint64_t data = va_alloc(alloc, 0x4000);
    if (o->data) va_write(data, o->data, tlen);
    uint8_t t[220]; memset(t, 0, sizeof t);
    uint32_t vp = (uint32_t)(data >> 14);
    memcpy(t, &tlen, 8); memcpy(t + 8, &vp, 4);
    t[12] = 1;
    { uint8_t *mp = t + 12 + 0x18; uint64_t row = (uint64_t)o->w * bpp; uint32_t mw = o->w, mh = o->h, md = 1; memcpy(mp + 0x10, &row, 8); memcpy(mp + 0x18, &mw, 4); memcpy(mp + 0x1c, &mh, 4); memcpy(mp + 0x20, &md, 4); }
    uint32_t rec = 12 + 0x18 + 36, tag = 0x14;
    memcpy(t + rec, &tag, 4);
    uint8_t *d = t + rec + 12;
    d[0] = 2; d[2] = (uint8_t)o->pixfmt;
    uint32_t w = o->w, h = o->h, one = 1;
    memcpy(d + 4, &w, 4); memcpy(d + 8, &h, 4); memcpy(d + 12, &one, 4);
    d[16] = 1; d[18] = 1;
    uint32_t tsize = rec + 12 + 28;
    uint64_t pay = va_alloc(tsize, 16);
    va_write(pay, t, tsize);
    entry(o->ref, LAGFX_A64_OBJ_TEXTURE, tsize, pay);
}
static void mk_sampler(const rr_obj_t *o) {
    uint8_t sm[36]; memset(sm, 0, sizeof sm);
    uint32_t tag = 3, size = 36; float lo = 0.f, hi = 3.4028235e38f;
    memcpy(sm, &tag, 4); memcpy(sm + 4, &size, 4); memcpy(sm + 8, &o->ref, 4); memcpy(sm + 12, &o->samp_state, 4);
    memcpy(sm + 20, &lo, 4); memcpy(sm + 24, &hi, 4);
    uint64_t pay = va_alloc(36, 16);
    va_write(pay, sm, 36);
    entry(o->ref, LAGFX_A64_OBJ_SERIALIZED, 36, pay);
}
static void mk_fn(const rr_obj_t *o) {   /* function object (type 6): {u64 library VA, u32 length, u32 tag, u32 size}; the library is a metallib */
    uint64_t lib = va_alloc(o->len, 16);
    va_write(lib, o->data, o->len);
    uint8_t h[20] = {0}; uint32_t len = o->len;
    memcpy(h, &lib, 8); memcpy(h + 8, &len, 4);
    uint64_t pay = va_alloc(20, 16);
    va_write(pay, h, 20);
    entry(o->ref, LAGFX_A64_OBJ_FUNCTION, 20, pay);
}
static void mk_pso(const rr_obj_t *o) {
    uint64_t pay = va_alloc(o->len, 16);
    va_write(pay, o->data, o->len);
    entry(o->ref, LAGFX_A64_OBJ_SERIALIZED, o->len, pay);
}

/* ---- protocol + ring (as arm64-exec-run-unit.c) ---- */
static void submit(lagfx_protocol_t *p, uint16_t op, const uint32_t *words, uint32_t nwords) {
    uint32_t len = 12u + nwords * 4u; uint16_t arg8 = 0;
    uint64_t ring = (uint64_t)RING_PFN0 << 14;
    uint8_t cmd[8192]; memset(cmd, 0, sizeof cmd);
    if (len > sizeof cmd || g_wp % 0x10000u + len > 0x10000u) { fprintf(stderr, "render-replay: exec command does not fit the ring\n"); exit(2); }
    memcpy(cmd, &op, 2); memcpy(cmd + 2, &arg8, 2); memcpy(cmd + 4, &len, 4);
    uint32_t stamp = g_wp + 1u; memcpy(cmd + 8, &stamp, 4);
    if (nwords) memcpy(cmd + 12, words, nwords * 4u);
    memcpy(g_mem + ring + (g_wp % 0x10000u), cmd, len);
    g_wp += len;
    put32(((uint64_t)SHARED_PFN << 14) + LAGFX_RING_FIFO_DESC_BASE_OFFSET + 0x00, g_wp);
    p->current_chan_id = 1;
    channel_compute_dispatcher_drain(p, 1);
}
static lagfx_protocol_t *mk_protocol(void) {
    memset(g_mem, 0, RR_MEM_SIZE); g_wp = 0; g_heap = VA_HEAP;
    lagfx_protocol_t *p = calloc(1, sizeof *p);
    p->magic = LAGFX_PROTOCOL_MAGIC; p->dev = g_dev;
    p->ring_page_shift = 14; p->protocol_mode = LAGFX_PROTO_ARM64_VMAPPLE;
    p->ring_shared_page_pfn = SHARED_PFN;
    uint64_t d = ((uint64_t)SHARED_PFN << 14) + LAGFX_RING_FIFO_DESC_BASE_OFFSET;
    put32(d + 0x0c, 1); put32(d + 0x10, TABLE_PFN);
    for (uint32_t i = 0; i < 4; i++) put32(((uint64_t)TABLE_PFN << 14) + 4u * i, RING_PFN0 + i);
    put32((uint64_t)ROOT_PFN << 14, TOP_PFN); put32(((uint64_t)ROOT_PFN << 14) + 4, 2);
    put32((uint64_t)TOP_PFN << 14, LEAF_PFN);
    for (uint32_t v = 0; v < RR_NVPN; v++) put32(((uint64_t)LEAF_PFN << 14) + 4u * v, DATA_PFN0 + v);
    lagfx_task_entry_t *t = lagfx_protocol_alloc_task_slot(p);
    t->id = 1; t->live = true; t->root_page_pfn = ROOT_PFN; t->lists_set = true; t->objects_addr = VA_OBJ; t->objects_len = 0;
    return p;
}

/* ---- command buffer ---- */
static uint8_t g_cb[1u << 16]; static uint32_t g_cbn;
static void cb_reset(void) { g_cbn = 8; memset(g_cb, 0, 8); }
static void cb_cmd(uint32_t tag, const void *pl, uint32_t n) {
    uint32_t sz = 8 + n;
    if (g_cbn + sz > sizeof g_cb) { fprintf(stderr, "render-replay: command buffer full\n"); exit(2); }
    memcpy(g_cb + g_cbn, &tag, 4); memcpy(g_cb + g_cbn + 4, &sz, 4);
    if (n) memcpy(g_cb + g_cbn + 8, pl, n); else memset(g_cb + g_cbn + 8, 0, 0);
    g_cbn += sz;
}
static void cb_pass_n(const uint32_t ref[2], const uint16_t load[2], const double clear[2][4]) {   /* as arm64-exec-run-unit.c cb_pass_n */
    uint8_t r[584]; memset(r, 0, sizeof r);
    for (int k = 0; k < 2; k++) {
        if (!ref[k]) continue;
        uint8_t *c = r + 0x4c + 0x3c * k;
        memcpy(c, &ref[k], 4); memcpy(c + 20, &load[k], 2);
        uint16_t store = 1; memcpy(c + 22, &store, 2);
        memcpy(c + 0x1c, clear[k], 32);
    }
    cb_cmd(26, r, sizeof r);
}
static void cb_slots(uint32_t tag, const rr_slot_t *s, uint32_t n, uint32_t stride_kind) {   /* 125 / 110: {index,count,{ref,off u64}}; 114 / 112: {index,count,ref...} */
    for (uint32_t i = 0; i < n; i++) {
        uint8_t r[20]; uint32_t idx = s[i].index, cnt = 1;
        memcpy(r, &idx, 4); memcpy(r + 4, &cnt, 4); memcpy(r + 8, &s[i].ref, 4);
        if (stride_kind == 12) { memcpy(r + 12, &s[i].off, 8); cb_cmd(tag, r, 20); } else cb_cmd(tag, r, 12);
    }
}
static void cb_finish(void) { uint32_t info = 0; memcpy(g_cb, &g_cbn, 4); memcpy(g_cb + 4, &info, 4); }

/* ---------------------------------------------------------------- fp16 helpers ---- */
static float h2f(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31u, m = h & 1023u, u;
    if (!e) { if (!m) u = s; else { int sh = 0; while (!(m & 1024u)) m <<= 1, sh++; m &= 1023u; u = s | (uint32_t)(113 - sh) << 23 | m << 13; } }
    else if (e == 31) u = s | 0x7f800000u | m << 13;
    else u = s | (e + 112u) << 23 | m << 13;
    float f; memcpy(&f, &u, 4); return f;
}
static uint16_t f2h(float f) {   /* round to nearest even */
    uint32_t u; memcpy(&u, &f, 4);
    uint32_t s = (u >> 16) & 0x8000u, a = u & 0x7fffffffu;
    if (a >= 0x7f800000u) return (uint16_t)(s | 0x7c00u | (a > 0x7f800000u ? 0x200u : 0u));
    if (a >= 0x477ff000u) return (uint16_t)(s | 0x7c00u);              /* >= 65520 rounds to inf */
    if (a < 0x38800000u) {                                            /* subnormal half or zero */
        if (a < 0x33000001u) return (uint16_t)s;
        uint32_t e = a >> 23, m = (a & 0x7fffffu) | 0x800000u, sh = 126u - e;
        uint32_t r = m >> sh, rem = m & ((1u << sh) - 1u), half = 1u << (sh - 1u);
        if (rem > half || (rem == half && (r & 1u))) r++;
        return (uint16_t)(s | r);
    }
    uint32_t r = ((a - 0x38000000u) >> 13), rem = a & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (r & 1u))) r++;
    return (uint16_t)(s | r);
}
static float rd_chan(const uint8_t *px, uint32_t fmt, uint32_t c) {
    if (fmt == 115) { uint16_t h; memcpy(&h, px + c * 2, 2); return h2f(h); }
    return px[c] / 255.0f;
}

/* ---------------------------------------------------------------- PNG (stored deflate) ---- */
static uint32_t crc32_(const uint8_t *d, size_t n, uint32_t c) {
    static uint32_t t[256]; if (!t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1; t[i] = v; }
    c = ~c; for (size_t i = 0; i < n; i++) c = t[(c ^ d[i]) & 255u] ^ (c >> 8); return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
static void png_chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n) {
    uint8_t h[8]; be32(h, n); memcpy(h + 4, type, 4); fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uint8_t *tmp = malloc(n + 4u); memcpy(tmp, type, 4); if (n) memcpy(tmp + 4, d, n);
    uint8_t cb[4]; be32(cb, crc32_(tmp, n + 4u, 0)); fwrite(cb, 1, 4, f); free(tmp);
}
/* linear RGBA (fp16 or 8 bit) -> 8-bit RGBA, clamp to [0,1], NO gamma (a preview of the stored values) */
static bool save_png(const char *path, const uint8_t *px, uint32_t w, uint32_t h, uint32_t fmt) {
    uint32_t bpp = rr_bpp(fmt); size_t raw = (size_t)h * (1u + (size_t)w * 4u);
    uint8_t *img = malloc(raw); if (!img) return false;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = img + (size_t)y * (1u + (size_t)w * 4u); row[0] = 0;
        for (uint32_t x = 0; x < w; x++) for (uint32_t c = 0; c < 4; c++) {
            float v = rd_chan(px + ((size_t)y * w + x) * bpp, fmt == 80 ? 70 : fmt, c);
            if (fmt == 80) v = rd_chan(px + ((size_t)y * w + x) * bpp, 70, c == 0 ? 2 : c == 2 ? 0 : c);   /* BGRA8 */
            if (!(v > 0.f)) v = 0.f;
            if (v > 1.f) v = 1.f;
            row[1 + x * 4 + c] = (uint8_t)(v * 255.0f + 0.5f);
        }
    }
    FILE *f = fopen(path, "wb"); if (!f) { free(img); return false; }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 }; fwrite(sig, 1, 8, f);
    uint8_t ih[13]; be32(ih, w); be32(ih + 4, h); ih[8] = 8, ih[9] = 6, ih[10] = ih[11] = ih[12] = 0; png_chunk(f, "IHDR", ih, 13);
    size_t nblk = (raw + 65534) / 65535, zl = 2 + raw + nblk * 5 + 4; uint8_t *z = malloc(zl); size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw; i++) { a = (a + img[i]) % 65521u; b = (b + a) % 65521u; }
    for (size_t off = 0; off < raw; off += 65535) {
        size_t n = raw - off < 65535 ? raw - off : 65535;
        z[o++] = off + n >= raw; z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8); z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, img + off, n); o += n;
    }
    be32(z + o, (b << 16) | a); o += 4;
    png_chunk(f, "IDAT", z, (uint32_t)o); png_chunk(f, "IEND", NULL, 0);
    fclose(f); free(z); free(img); return true;
}

/* ---------------------------------------------------------------- scene runner ---- */
static const char *g_vp_override_note;
static int g_vb_override = -1, g_patch_attrs;
static bool wrap_resolver(void *c, lagfx_protocol_t *p, const lagfx_task_entry_t *t, uint32_t ref, const uint32_t e[3], lagfx_a64_pipeline_src_t *out) {
    (void)c;
    if (!lagfx_a64_pipeline_resolve(NULL, p, t, ref, e, out)) return false;
    if (getenv("RR_VERBOSE")) {
        fprintf(stderr, "rr: resolved pso %u: vtx inputs %u:", ref, out->desc.n_vtx_inputs);
        for (uint32_t i = 0; i < out->desc.n_vtx_inputs; i++) fprintf(stderr, " loc%u/c%u", out->desc.vtx_in_loc[i], out->desc.vtx_in_comp[i]);
        fprintf(stderr, " pso_attrs %u stride %u vb_index %u n_bind %u\n", out->desc.n_pso_attrs, out->desc.vtx_stride, out->vertex_buffer_index, out->n_bind);
        for (uint32_t i = 0; i < out->n_bind; i++) fprintf(stderr, "rr:   bind %u kind %u stage %u metal %d\n", out->bind[i].binding, out->bind[i].kind, out->bind[i].stage, out->bind[i].metal_index);
    }
    if (g_vb_override >= 0) out->vertex_buffer_index = (uint32_t)g_vb_override;
    if (g_patch_attrs == 1 && out->desc.n_vtx_inputs == 4) {   /* MTLVertexFormat 31 Float4 @0, 29 Float2 @16, 29 Float2 @24, 27 Half4 @32 (the layout the descriptor of pso 93 names; the first attribute carries only default tokens and is not decoded) */
        out->desc.n_pso_attrs = 4;
        out->desc.pso_attr_fmt[0] = 31, out->desc.pso_attr_off[0] = 0; out->desc.pso_attr_fmt[1] = 29, out->desc.pso_attr_off[1] = 16;
        out->desc.pso_attr_fmt[2] = 29, out->desc.pso_attr_off[2] = 24; out->desc.pso_attr_fmt[3] = 27, out->desc.pso_attr_off[3] = 32;
        out->desc.vtx_stride = 48;
    }
    return true;
}

static void rr_run(const rr_scene_t *s, rr_result_t *res) {
    memset(res, 0, sizeof *res);
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    uint64_t v0 = lagfx_vk_validation_msg_count;
    lagfx_protocol_t *p = mk_protocol();
    lagfx_task_entry_t *t = lagfx_protocol_find_task(p, 1);
    uint32_t reslist[64][2], nres = 0;
    for (uint32_t i = 0; i < s->nobj; i++) {
        const rr_obj_t *o = &s->obj[i];
        if (o->ref >= RR_MAX_REF) { snprintf(res->why, sizeof res->why, "ref %u too large", o->ref); free(p); return; }
        switch (o->kind) {
        case RR_BUF: mk_buffer(o); break;
        case RR_TEX: mk_texture(o); break;
        case RR_SAMP: mk_sampler(o); break;
        case RR_FN: mk_fn(o); break;
        case RR_PSO: mk_pso(o); break;
        }
        if ((o->kind == RR_BUF || o->kind == RR_TEX) && nres < 64) {
            bool tgt = false; for (uint32_t k = 0; k < s->ntarget; k++) tgt |= s->target_ref[k] == o->ref;
            reslist[nres][0] = o->ref, reslist[nres][1] = tgt ? 2u : 1u; nres++;
        }
    }
    g_vb_override = s->vb_override, g_patch_attrs = s->patch_attrs;
    lagfx_a64_exec_set_pipeline_resolver(p, wrap_resolver, NULL);
    cb_reset();
    cb_pass_n(s->target_ref, s->load, s->clear);
    { double v[6] = { 0, 0, s->w, s->h, 0, 1 }; cb_cmd(130, v, sizeof v); }
    for (uint32_t d = 0; d < s->ndraw; d++) {
        const rr_draw_t *dr = &s->draw[d];
        cb_cmd(116, &dr->pso, 4);
        cb_slots(125, dr->vb, dr->nvb, 12); cb_slots(110, dr->fb, dr->nfb, 12);
        cb_slots(114, dr->ft, dr->nft, 4); cb_slots(112, dr->fs, dr->nfs, 4);
        if (dr->index_ref) { uint8_t r[12] = {0}; uint16_t prim = 3, it = (uint16_t)dr->index_u32, c = (uint16_t)dr->count, io = (uint16_t)dr->index_off; memcpy(r, &prim, 2); memcpy(r + 2, &it, 2); memcpy(r + 4, &dr->index_ref, 4); memcpy(r + 8, &c, 2); memcpy(r + 10, &io, 2); cb_cmd(7, r, 12); }
        else { uint8_t r[8]; uint32_t prim = 3; uint16_t st = (uint16_t)dr->start, c = (uint16_t)dr->count; memcpy(r, &prim, 4); memcpy(r + 4, &st, 2); memcpy(r + 6, &c, 2); cb_cmd(1, r, 8); }
    }
    cb_finish();
    uint64_t cmd_va = va_alloc(g_cbn, 16);
    va_write(cmd_va, g_cb, g_cbn);
    uint32_t w[3 + 2 * 64 + 4], n = 0;
    w[n++] = 1; w[n++] = nres; w[n++] = 1;
    for (uint32_t i = 0; i < nres; i++) w[n++] = reslist[i][0], w[n++] = reslist[i][1];
    uint64_t off = cmd_va, l = g_cbn; memcpy(w + n, &off, 8); n += 2; memcpy(w + n, &l, 8); n += 2;
    submit(p, 0x43, w, n);
    __typeof__(p->diag2.exec.run) *R = &p->diag2.exec.run;
    res->ran = true, res->draws = R->draws, res->skipped = R->draws_skipped, res->submit_failed = R->submit_failed;
    res->resolved = p->diag2.exec.pipe.resolved, res->built = p->diag2.exec.pipe.pipes_built;
    res->w = s->w, res->h = s->h;
    for (uint32_t k = 0; k < s->ntarget; k++) {
        uint32_t fmt = 0; for (uint32_t i = 0; i < s->nobj; i++) if (s->obj[i].ref == s->target_ref[k]) fmt = s->obj[i].pixfmt;
        res->bpp[k] = rr_bpp(fmt);
        size_t sz = (size_t)s->w * s->h * res->bpp[k]; res->px[k] = calloc(1, sz);
        uint32_t rw, rh;
        if (!lagfx_a64_exec_readback_texture(p, t, s->target_ref[k], res->px[k], sz, &rw, &rh)) snprintf(res->why, sizeof res->why, "readback of target %u failed", k);
    }
    {
        const __typeof__(p->diag2.exec.pipe) *P = &p->diag2.exec.pipe;
        struct { const char *n; uint64_t v; } rf[18] = { { "no_blob", P->rf_no_blob }, { "bad_tag", P->rf_bad_tag }, { "no_refs", P->rf_no_refs }, { "no_lib", P->rf_no_lib }, { "no_stage", P->rf_no_stage },
            { "module", P->rf_module }, { "translate", P->rf_translate }, { "layout", P->rf_layout }, { "multi_stream", P->rf_multi_stream }, { "vertex_res", P->rf_vertex_res },
            { "bindings", P->rf_bindings }, { "missing_cap", P->rf_missing_cap }, { "stub_fbfetch", P->stub_fbfetch }, { "pipe_build_failed", P->pipe_build_failed },
            { "desc_unbound", P->desc_unbound }, { "desc_failed", P->desc_failed }, { "no_pipeline", R->no_pipeline }, { "no_vertex_buf", R->no_vertex_buf } };
        size_t o2 = strlen(res->why);
        for (size_t i = 0; i < sizeof rf / sizeof rf[0]; i++) if (rf[i].v && o2 < sizeof res->why - 24) o2 += (size_t)snprintf(res->why + o2, sizeof res->why - o2, "%s=%llu ", rf[i].n, (unsigned long long)rf[i].v);
    }
    if (getenv("RR_VERBOSE")) {
        const __typeof__(p->diag2.exec.pipe) *P = &p->diag2.exec.pipe;
        fprintf(stderr, "rr: blend_decoded=%llu blend_off=%llu stub_blend=%llu pipes_built=%llu desc_sets=%llu desc_unbound=%llu no_target=%llu fbfetch_draws=%llu\n", (unsigned long long)P->blend_decoded, (unsigned long long)P->blend_off,
                (unsigned long long)P->stub_blend, (unsigned long long)P->pipes_built, (unsigned long long)P->desc_sets, (unsigned long long)P->desc_unbound, (unsigned long long)R->no_target, (unsigned long long)P->fbfetch_draws);
    }
    lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); free(p);
    res->val_msgs = lagfx_vk_validation_msg_count - v0;
}

/* ---------------------------------------------------------------- dump loader ---- */
typedef struct { uint32_t task, ref, type, tag; char file[96]; } dobj_t;
typedef struct { uint32_t task, ref; char file[96]; } dfn_t;
typedef struct { char dir[512]; dobj_t obj[1024]; uint32_t nobj; dfn_t fn[1024]; uint32_t nfn; uint32_t pso[512][2]; uint32_t npso; } dump_t;
static uint8_t *slurp(const char *path, uint32_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)l + 1u); if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
    fclose(f); *n = (uint32_t)l; return b;
}
static bool dump_load(dump_t *d, const char *dir) {
    memset(d, 0, sizeof *d); snprintf(d->dir, sizeof d->dir, "%s", dir);
    char path[640]; snprintf(path, sizeof path, "%s/index.txt", dir);
    FILE *f = fopen(path, "r"); if (!f) return false;
    static char line[16384];
    while (fgets(line, sizeof line, f)) {
        unsigned task = 0, ref = 0, type = 0; char file[96] = "", tagbuf[16] = "";
        const char *r = strstr(line, " ref="), *fl = strstr(line, " file=");
        if (!r || !fl || sscanf(strstr(line, "task="), "task=%u", &task) != 1) continue;
        ref = (unsigned)atoi(r + 5); sscanf(fl + 6, "%95s", file);
        if (!strncmp(line, "obj ", 4) && d->nobj < 1024) {
            const char *ty = strstr(line, " type="), *tg = strstr(line, " tag=");
            type = ty ? (unsigned)atoi(ty + 6) : 0; if (tg) sscanf(tg + 5, "%15s", tagbuf);
            dobj_t *o = &d->obj[d->nobj++]; o->task = task, o->ref = ref, o->type = type, o->tag = (uint32_t)strtoul(tagbuf, NULL, 0); snprintf(o->file, sizeof o->file, "%s", file);
            if (type == 7 && o->tag == 0xe && d->npso < 512) d->pso[d->npso][0] = task, d->pso[d->npso++][1] = ref;
        } else if (!strncmp(line, "fn ", 3) && d->nfn < 1024) {
            dfn_t *q = &d->fn[d->nfn++]; q->task = task, q->ref = ref; snprintf(q->file, sizeof q->file, "%s", file);
        }
    }
    fclose(f); return true;
}
static const dobj_t *dump_obj(const dump_t *d, uint32_t task, uint32_t ref) { for (uint32_t i = 0; i < d->nobj; i++) if (d->obj[i].task == task && d->obj[i].ref == ref) return &d->obj[i]; return NULL; }
static const dfn_t *dump_fn(const dump_t *d, uint32_t task, uint32_t ref) { for (uint32_t i = 0; i < d->nfn; i++) if (d->fn[i].task == task && d->fn[i].ref == ref) { const char *e = strstr(d->fn[i].file, ".bin"); (void)e; return &d->fn[i]; } return NULL; }
static bool scene_add(rr_scene_t *s, rr_obj_t o) { if (s->nobj >= 48u) return false; s->obj[s->nobj++] = o; return true; }
/* the PSO (task, ref) of the dump + the function objects its descriptor names; false if a file is missing. `swap_fs` (ref, library file) replaces one function's library
 * (the same Metal libraries of the dump, recombined: how a different fragment function is paired with a real vertex function / descriptor) */
static bool scene_add_pso(rr_scene_t *s, const dump_t *d, uint32_t task, uint32_t ref, uint32_t swap_ref, uint32_t swap_with) {
    const dobj_t *po = dump_obj(d, task, ref); if (!po) return false;
    char path[700]; uint32_t n = 0;
    snprintf(path, sizeof path, "%s/%s", d->dir, po->file);
    uint8_t *blob = slurp(path, &n); if (!blob) return false;
    lagfx_a64_pdesc_t pd;
    if (!lagfx_a64_pdesc_decode(blob, n, NULL, NULL, &pd)) { free(blob); return false; }
    scene_add(s, (rr_obj_t){ .ref = ref, .kind = RR_PSO, .data = blob, .len = n });
    for (uint32_t i = 0; i < pd.nrefs; i++) {
        uint32_t fr = pd.refs[i], src = fr == swap_ref ? swap_with : fr;
        const dfn_t *fn = dump_fn(d, task, src); if (!fn) continue;
        snprintf(path, sizeof path, "%s/%s", d->dir, fn->file);
        uint32_t ln = 0; uint8_t *lib = slurp(path, &ln); if (!lib) continue;
        bool dup = false; for (uint32_t k = 0; k < s->nobj; k++) dup |= s->obj[k].ref == fr;
        if (!dup) scene_add(s, (rr_obj_t){ .ref = fr, .kind = RR_FN, .data = lib, .len = ln });
    }
    return true;
}

/* ---------------------------------------------------------------- CPU model: rasterizer (Vulkan rules, perspective correct) ---- */
typedef struct { float clip[4]; float a[8]; } mv_t;   /* a model vertex: clip position + up to 8 interpolants */
/* Coverage / interpolation of one triangle at pixel (x, y) of a w x h target (clip y up = row 0, centre sampling). MT_IN: the pixel centre is more than `margin`
 * pixels inside every edge (a[] = perspective correct interpolants); MT_OUT: more than `margin` outside at least one edge; MT_EDGE: undecided (coverage depends on
 * the rasterizer's tie breaking and precision: such pixels are not compared). Vertices must have w > 0 (the model does not clip). */
enum { MT_OUT = 0, MT_IN, MT_EDGE };
static int model_tri(const mv_t v[3], uint32_t w, uint32_t h, uint32_t x, uint32_t y, float margin, uint32_t na, float *out) {
    float sx[3], sy[3], iw[3];
    for (int i = 0; i < 3; i++) {
        if (!(v[i].clip[3] > 0.f)) return MT_EDGE;
        iw[i] = 1.f / v[i].clip[3];
        sx[i] = (v[i].clip[0] * iw[i] * 0.5f + 0.5f) * (float)w; sy[i] = (0.5f - v[i].clip[1] * iw[i] * 0.5f) * (float)h;
    }
    float px = (float)x + 0.5f, py = (float)y + 0.5f;
    float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    if (fabsf(area) < 1e-6f) return MT_OUT;
    float l[3], dmin = 1e30f;
    for (int i = 0; i < 3; i++) {
        int j = (i + 1) % 3, k = (i + 2) % 3;
        float e = (sx[j] - px) * (sy[k] - py) - (sx[k] - px) * (sy[j] - py);   /* cross product = len(jk) * distance(p, line jk) */
        float len = hypotf(sx[k] - sx[j], sy[k] - sy[j]);
        l[i] = e / area;
        float dist = (area > 0 ? e : -e) / (len > 0 ? len : 1.f);
        if (dist < dmin) dmin = dist;
    }
    if (dmin < -margin) return MT_OUT;
    if (dmin < margin) return MT_EDGE;
    float den = l[0] * iw[0] + l[1] * iw[1] + l[2] * iw[2];
    for (uint32_t c = 0; c < na; c++) out[c] = (l[0] * iw[0] * v[0].a[c] + l[1] * iw[1] * v[1].a[c] + l[2] * iw[2] * v[2].a[c]) / den;
    return MT_IN;
}

/* comparison: returns the number of mismatching pixels among those the model decides (inside away from edges, or outside away from edges) */
typedef struct { uint32_t compared, bad, first_x, first_y; float worst; } rr_cmp_t;

/* ---------------------------------------------------------------- scenes ---- */
#define RR_W 32u
#define RR_H 32u
#define REF_RT0 3001u
#define REF_RT1 3002u
#define REF_MAT 3010u
#define REF_VTX 3011u
#define REF_IDX 3012u
#define REF_TEXS 3013u
#define REF_SMP 3014u
#define REF_UNI 3015u

typedef struct { float M[16]; } mat_t;
static void identity(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1.f; }
/* pso 12 / lib 4f70917c (vertex) model: clip = M0 * x + M1 * y + M3 (columns of the matrix buffer at Metal buffer 0), colour = attribute 0 (float4), position = attribute 1 (float2).
 * Source: the translated SPIR-V of that function (AccessChain 0, 1, 3 of the buffer; the attribute Locations 0 / 1). The fragment SimpleColorFragment outputs the interpolated colour. */
static void vs_color(const float *M, const float *vtx6, mv_t *o) {   /* vtx6 = color4, pos2 */
    for (int c = 0; c < 4; c++) o->clip[c] = M[c] * vtx6[4] + M[4 + c] * vtx6[5] + M[12 + c];
    for (int c = 0; c < 4; c++) o->a[c] = vtx6[c];
}
/* pso 72 / lib dc182c5a (vertex) model: same matrix maths, attributes: position float2, texcoord float2; out: texcoord */
static void vs_tex(const float *M, const float *vtx4, mv_t *o) {
    for (int c = 0; c < 4; c++) o->clip[c] = M[c] * vtx4[0] + M[4 + c] * vtx4[1] + M[12 + c];
    o->a[0] = vtx4[2], o->a[1] = vtx4[3];
}

static bool find_pso_sources(const dump_t *d, uint32_t task, uint32_t ref) { return dump_obj(d, task, ref) != NULL; }

/* common scene skeleton: 2 (or 1) RGBA16F targets, the matrix buffer at Metal slot 0 and the vertex stream at slot 1 */
static void scene_base(rr_scene_t *s, const char *name, uint32_t ntarget, const double clear0[4]) {
    memset(s, 0, sizeof *s);
    s->name = name; s->w = RR_W; s->h = RR_H; s->ntarget = ntarget; s->vb_override = -1;
    for (uint32_t k = 0; k < ntarget; k++) {
        s->target_ref[k] = REF_RT0 + k; s->load[k] = 2;
        for (int c = 0; c < 4; c++) s->clear[k][c] = k == 0 ? clear0[c] : (c == 1 ? 0.5 : c == 3 ? 1.0 : 0.25);   /* target 1: (0.25, 0.5, 0.25, 1) */
        scene_add(s, (rr_obj_t){ .ref = REF_RT0 + k, .kind = RR_TEX, .w = s->w, .h = s->h, .pixfmt = 115 });
    }
}
static void draw_std(rr_scene_t *s, uint32_t pso, uint32_t count, bool indexed) {
    rr_draw_t *d = &s->draw[s->ndraw++]; memset(d, 0, sizeof *d);
    d->pso = pso; d->count = count;
    d->vb[0] = (rr_slot_t){ 0, REF_VTX, 0 }; d->vb[1] = (rr_slot_t){ 1, REF_MAT, 0 }; d->nvb = 2;
    if (indexed) d->index_ref = REF_IDX;
}

/* ---- scene checks ---- */
typedef struct { const char *name; bool (*build)(rr_scene_t *, const dump_t *); void (*check)(const rr_scene_t *, const rr_result_t *, int wrong, rr_cmp_t *); const char *tolerance; } scene_def_t;

static const double CLR[4] = { 0.125, 0.25, 0.375, 1.0 };   /* exactly representable in fp16: the Metal clear value of a RGBA16Float target is bit exact */
static void cmp_px(rr_cmp_t *c, uint32_t x, uint32_t y, const float got[4], const float exp[4], float tol) {
    float worst = 0; for (int i = 0; i < 4; i++) { float e = fabsf(got[i] - h2f(f2h(exp[i]))); if (e > worst) worst = e; }   /* the target is fp16: the model value is rounded to fp16 (nearest even) */
    c->compared++;
    if (worst > tol || worst != worst) { if (!c->bad) c->first_x = x, c->first_y = y; c->bad++; }
    if (worst > c->worst) c->worst = worst;
}
static void get_px(const rr_result_t *r, uint32_t t, uint32_t x, uint32_t y, float o[4]) { for (int c = 0; c < 4; c++) o[c] = rd_chan(r->px[t] + ((size_t)y * r->w + x) * r->bpp[t], r->bpp[t] == 8 ? 115 : 70, (uint32_t)c); }

/* --- scene: solid triangle (SimpleColorFragment, constant colour); pixel-exact fp16 inside, clear colour outside, edge pixels not compared --- */
static const float SOLID_C[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
static float g_m_ident[16];
static bool build_solid(rr_scene_t *s, const dump_t *d) {
    scene_base(s, "solid", 1, CLR);
    if (!scene_add_pso(s, d, 2, 12, 0, 0)) return false;
    identity(g_m_ident);
    static float vtx[3][6];
    const float pos[3][2] = { { -0.75f, -0.75f }, { 0.75f, -0.5f }, { 0.0f, 0.8f } };
    for (int i = 0; i < 3; i++) { memcpy(vtx[i], SOLID_C, 16); vtx[i][4] = pos[i][0]; vtx[i][5] = pos[i][1]; }
    scene_add(s, (rr_obj_t){ .ref = REF_MAT, .kind = RR_BUF, .data = g_m_ident, .len = 64 });
    scene_add(s, (rr_obj_t){ .ref = REF_VTX, .kind = RR_BUF, .data = vtx, .len = sizeof vtx });
    draw_std(s, 12, 3, false);
    return true;
}
/* Colour triangles (vs_color model): per pixel, EDGE of any triangle -> skipped; else the last covering triangle's interpolated colour, else the clear colour.
 * wrong: 0 = the model, 1 = wrong colour (red + 0.5), 2 = wrong coverage (nothing drawn). */
static void model_color_tri(const rr_result_t *r, const float *M, const float *vtx, const uint32_t *idx, uint32_t ntri, int wrong, rr_cmp_t *c, float tol) {
    memset(c, 0, sizeof *c);
    for (uint32_t y = 0; y < r->h; y++) for (uint32_t x = 0; x < r->w; x++) {
        float got[4], exp[4] = { (float)CLR[0], (float)CLR[1], (float)CLR[2], (float)CLR[3] }; get_px(r, 0, x, y, got);
        bool skip = false, covered = false;
        for (uint32_t t = 0; t < ntri && !skip; t++) {
            mv_t v[3]; for (int i = 0; i < 3; i++) vs_color(M, vtx + 6 * idx[t * 3 + (uint32_t)i], &v[i]);
            float a[4]; int k = model_tri(v, r->w, r->h, x, y, 0.75f, 4, a);
            if (k == MT_EDGE) skip = true; else if (k == MT_IN) { covered = true; memcpy(exp, a, 16); }
        }
        if (skip) continue;
        if (wrong == 1 && covered) exp[0] += 0.5f;
        if (wrong == 2 && covered) { exp[0] = (float)CLR[0], exp[1] = (float)CLR[1], exp[2] = (float)CLR[2], exp[3] = (float)CLR[3]; }
        cmp_px(c, x, y, got, exp, tol);
    }
}
static void check_solid(const rr_scene_t *s, const rr_result_t *r, int wrong, rr_cmp_t *c) {
    static float vtx[18]; const float pos[3][2] = { { -0.75f, -0.75f }, { 0.75f, -0.5f }, { 0.0f, 0.8f } };
    for (int i = 0; i < 3; i++) { memcpy(vtx + 6 * i, SOLID_C, 16); vtx[6 * i + 4] = pos[i][0]; vtx[6 * i + 5] = pos[i][1]; }
    static const uint32_t idx[3] = { 0, 1, 2 };
    model_color_tri(r, g_m_ident, vtx, idx, 1, wrong, c, 0.0f);   /* tolerance 0: constant colour, clear colour; fp16 bit exact */
}

/* ---------------------------------------------------------------- driver ---- */
static const scene_def_t g_scenes[] = {
    { "solid", build_solid, check_solid, "0 (constant colour + clear colour: fp16 bit exact; pixels within 0.75 px of an edge are not compared)" },
};
static char g_dumpdir[512];
static const char *find_dump(void) {
    if (g_dumpdir[0]) return g_dumpdir;
    const char *e = getenv("LAGFX_TEST_DUMP"); if (!e) e = getenv("RR_DUMP");
    if (e) { snprintf(g_dumpdir, sizeof g_dumpdir, "%s", e); return g_dumpdir; }
    return "/tmp/zx/dump";
}
static void save_result(const rr_scene_t *s, const rr_result_t *r, const char *outdir, const char *tag) {
    if (!outdir) return;
    for (uint32_t k = 0; k < s->ntarget; k++) {
        char path[700]; uint32_t fmt = r->bpp[k] == 8 ? 115 : 80;
        snprintf(path, sizeof path, "%s/%s%s_rt%u.f16.raw", outdir, s->name, tag, k);
        FILE *f = fopen(path, "wb"); if (f) { fwrite(r->px[k], 1, (size_t)r->w * r->h * r->bpp[k], f); fclose(f); }
        snprintf(path, sizeof path, "%s/%s%s_rt%u.png", outdir, s->name, tag, k);
        save_png(path, r->px[k], r->w, r->h, fmt);
    }
}
/* returns 0 pass, 1 fail, 77 skip */
static int run_scene(const scene_def_t *sd, const dump_t *d, const char *outdir) {
    static rr_scene_t s; rr_result_t r;
    if (!sd->build(&s, d)) { printf("SKIP %-10s: the dump lacks what the scene needs\n", sd->name); return 77; }
    rr_run(&s, &r);
    int rc = 0;
    rr_cmp_t c, n1, n2;
    if (!r.ran || r.draws != s.ndraw || r.skipped || r.submit_failed || r.val_msgs || !r.px[0]) {
        printf("FAIL %-10s: draws %llu/%u skipped %llu submit_failed %llu validation %llu [%s]\n", sd->name, (unsigned long long)r.draws, s.ndraw, (unsigned long long)r.skipped, (unsigned long long)r.submit_failed, (unsigned long long)r.val_msgs, r.why);
        if (r.px[0]) save_result(&s, &r, outdir, "");
        rr_obj_free_result(&r); return 1;
    }
    sd->check(&s, &r, 0, &c); sd->check(&s, &r, 1, &n1); sd->check(&s, &r, 2, &n2);
    bool pos = c.compared > 0 && c.bad == 0, neg = n1.bad > 0 && n2.bad > 0;
    printf("%s %-10s: %u pixels compared, %u mismatches (worst %.4g; first at %u,%u); negative tests: wrong colour -> %u bad, wrong coverage -> %u bad%s; tolerance %s\n",
           pos && neg ? "PASS" : "FAIL", sd->name, c.compared, c.bad, c.worst, c.first_x, c.first_y, n1.bad, n2.bad, neg ? " (both detected)" : " (NOT DETECTED: the check is vacuous)", sd->tolerance);
    save_result(&s, &r, outdir, "");
    rc = pos && neg ? 0 : 1;
    rr_obj_free_result(&r);
    return rc;
}

/* ---- executability smoke over every render PSO of the dump (no pixel reference) ---- */
static int smoke_one(const dump_t *d, uint32_t task, uint32_t ref, bool verbose) {
    static rr_scene_t s; rr_result_t r;
    memset(&s, 0, sizeof s); s.name = "smoke"; s.w = s.h = 16; s.vb_override = -1; s.ntarget = 2;
    for (uint32_t k = 0; k < 2; k++) { s.target_ref[k] = REF_RT0 + k; s.load[k] = 2; s.clear[k][3] = 1.0; scene_add(&s, (rr_obj_t){ .ref = REF_RT0 + k, .kind = RR_TEX, .w = 16, .h = 16, .pixfmt = 115 }); }
    if (!scene_add_pso(&s, d, task, ref, 0, 0)) { printf("pso %u/%u: files missing\n", task, ref); return 1; }
    static float pat[4096]; for (uint32_t i = 0; i < 4096; i++) pat[i] = (float)(i % 7) * 0.125f - 0.25f;
    static uint8_t texpx[16 * 16 * 4]; for (uint32_t i = 0; i < sizeof texpx; i++) texpx[i] = (uint8_t)(i * 37u);
    scene_add(&s, (rr_obj_t){ .ref = REF_VTX, .kind = RR_BUF, .data = pat, .len = sizeof pat, .alloc = 1u << 16 });
    scene_add(&s, (rr_obj_t){ .ref = REF_TEXS, .kind = RR_TEX, .data = texpx, .w = 16, .h = 16, .pixfmt = 70 });
    scene_add(&s, (rr_obj_t){ .ref = REF_SMP, .kind = RR_SAMP, .samp_state = 0x86000000u });
    rr_draw_t *dr = &s.draw[s.ndraw++]; dr->pso = ref; dr->count = 3;
    for (uint32_t i = 0; i < 16; i++) { dr->vb[i] = (rr_slot_t){ i, REF_VTX, 0 }; dr->fb[i] = (rr_slot_t){ i, REF_VTX, 0 }; dr->ft[i] = (rr_slot_t){ i, REF_TEXS, 0 }; dr->fs[i] = (rr_slot_t){ i, REF_SMP, 0 }; }
    dr->nvb = dr->nfb = dr->nft = dr->nfs = 16;
    rr_run(&s, &r);
    bool ok = r.ran && r.draws == 1 && !r.skipped && !r.submit_failed && !r.val_msgs;
    if (verbose || !ok) printf("%s pso task=%u ref=%u: resolved=%llu built=%llu draws=%llu skipped=%llu submit_failed=%llu validation=%llu %s\n", ok ? "OK  " : "FAIL", task, ref, (unsigned long long)r.resolved, (unsigned long long)r.built,
        (unsigned long long)r.draws, (unsigned long long)r.skipped, (unsigned long long)r.submit_failed, (unsigned long long)r.val_msgs, r.why);
    rr_obj_free_result(&r);
    return ok ? 0 : 1;
}
static int run_dump(const dump_t *d, int argc, char **argv) {
    if (argc >= 2) return smoke_one(d, (uint32_t)atoi(argv[0]), (uint32_t)atoi(argv[1]), true);
    int ok = 0, bad = 0, tile = 0;
    for (uint32_t i = 0; i < d->npso; i++) {
        char path[700]; uint32_t n = 0; const dobj_t *o = dump_obj(d, d->pso[i][0], d->pso[i][1]);
        snprintf(path, sizeof path, "%s/%s", d->dir, o->file);
        uint8_t *b = slurp(path, &n);
        lagfx_a64_pdesc_t pd; bool isv = b && lagfx_a64_pdesc_decode(b, n, NULL, NULL, &pd);
        free(b);
        (void)isv;
        if (smoke_one(d, d->pso[i][0], d->pso[i][1], true) == 0) ok++; else bad++;
    }
    printf("executability only (no pixel reference): %d PSOs ran one draw, %d did not (%d tile/compute)\n", ok, bad, tile);
    return bad ? 1 : 0;
}

#ifndef RR_NO_MAIN
int main(int argc, char **argv) {
    const char *outdir = NULL; int na = 0; char *av[16];
    for (int i = 1; i < argc && na < 16; i++) {
        if (!strcmp(argv[i], "--dump") && i + 1 < argc) { snprintf(g_dumpdir, sizeof g_dumpdir, "%s", argv[++i]); continue; }
        av[na++] = argv[i];
    }
    if (na < 1) { fprintf(stderr, "usage: %s synth <scene|all|list> [outdir] | dump [task ref] | capture <dir>   [--dump DIR]\n", argv[0]); return 2; }
    if (!strcmp(av[0], "capture")) { puts("render-replay capture: PENDING. It will load the draws captured on the device (src/protocol/arm64_capture.h of the capture work) into rr_scene_t and run them through the same rr_run()."); return 3; }
    if (!strcmp(av[0], "synth") && na >= 2 && !strcmp(av[1], "list")) { for (size_t i = 0; i < sizeof g_scenes / sizeof g_scenes[0]; i++) puts(g_scenes[i].name); return 0; }
    if (!rr_open()) { puts("SKIP: no Vulkan device"); return 77; }
    static dump_t d;
    if (!dump_load(&d, find_dump())) { printf("SKIP: no dump at %s (--dump DIR / LAGFX_TEST_DUMP)\n", find_dump()); rr_close(); return 77; }
    int rc = 0;
    if (!strcmp(av[0], "dump")) rc = run_dump(&d, na - 1, av + 1);
    else if (!strcmp(av[0], "synth") && na >= 2) {
        outdir = na >= 3 ? av[2] : NULL; int ran = 0;
        for (size_t i = 0; i < sizeof g_scenes / sizeof g_scenes[0]; i++) if (!strcmp(av[1], "all") || !strcmp(av[1], g_scenes[i].name)) { int r = run_scene(&g_scenes[i], &d, outdir); ran++; if (r == 1) rc = 1; }
        if (!ran) { fprintf(stderr, "unknown scene %s\n", av[1]); rc = 2; }
    } else rc = 2;
    rr_close();
    return rc;
}
#endif
#else
#ifndef RR_NO_MAIN
int main(void) { puts("SKIP: built without Vulkan"); return 77; }
#endif
#endif
