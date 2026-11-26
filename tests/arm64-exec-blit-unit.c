/* ARM64 exec Blit execution + resource flag semantics on lavapipe (validation layer on), driven through a virtual ring and fake guest memory.
 * SPDX-License-Identifier: MIT */
#include "dispatchers/channel_compute_dispatcher.h"
#include "dispatchers/ring_common.h"
#include "protocol/state.h"
#include "protocol/arm64_exec_decode.h"
#include "protocol/arm64_resources.h"
#include "vulkan/instance.h"
#include "device.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint64_t lagfx_vk_validation_msg_count;

#define MEM_SIZE   (4u << 20)
#define SHARED_PFN 1u
#define TABLE_PFN  2u
#define RING_PFN0  4u
#define ROOT_PFN   16u
#define TOP_PFN    17u
#define LEAF_PFN   18u
#define DATA_PFN0  32u
#define NVPN       64u
#define PG         0x4000u

/* task VA layout (16 KiB pages): 0 object table, 1 descriptors, 8-9 bufA, 10-11 bufB, 12-13 bufC (13 -> far physical page), 16 texT, 17 texU, 20-24 tall tex,
 * 30-31 command buffer, 3 and 40+ unmapped except 40-43 = bufUnmapped region (vpn 41 unmapped) */
#define VA_OBJ (2u * PG)
#define VA_DESC (1u * PG)
#define VPN_A 8u
#define VPN_B 10u
#define VPN_C 12u
#define VPN_T 16u
#define VPN_U 17u
#define VPN_TALL 20u
#define VPN_CMD 30u
#define VPN_UNM 40u   /* 40 mapped, 41 unmapped */

static uint8_t *g_mem;
static bool rd(void *o, uint64_t gpa, uint64_t len, void *dst) { (void)o; if (gpa + len > MEM_SIZE) return false; memcpy(dst, g_mem + gpa, len); return true; }
static bool wr(void *o, uint64_t gpa, uint64_t len, const void *src) { (void)o; if (gpa + len > MEM_SIZE) return false; memcpy(g_mem + gpa, src, len); return true; }
static void irq(void *o, uint32_t v) { (void)o; (void)v; }
static void put32(uint64_t gpa, uint32_t v) { memcpy(g_mem + gpa, &v, 4); }
static uint32_t g_wp;
static lagfx_device_t *g_dev;
static lagfx_protocol_t *g_p;
static int g_fail;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, m); g_fail++; } } while (0)

static uint32_t pfn_of(uint32_t v) { return v == 13 ? 200u : DATA_PFN0 + v; }

static void submit(uint16_t op, uint16_t arg8, const uint32_t *words, uint32_t nwords) {
    uint32_t len = 12u + nwords * 4u;
    uint64_t ring = (uint64_t)RING_PFN0 << 14;
    uint8_t cmd[4096]; memset(cmd, 0, sizeof cmd);
    assert(len <= sizeof cmd);
    memcpy(cmd, &op, 2); memcpy(cmd + 2, &arg8, 2); memcpy(cmd + 4, &len, 4);
    uint32_t stamp = g_wp + 1u; memcpy(cmd + 8, &stamp, 4);
    if (nwords) memcpy(cmd + 12, words, nwords * 4u);
    if (g_wp % 0x10000u + len > 0x10000u) { fprintf(stderr, "ring wrap\n"); abort(); }
    memcpy(g_mem + ring + (g_wp % 0x10000u), cmd, len);
    g_wp += len;
    put32(((uint64_t)SHARED_PFN << 14) + LAGFX_RING_FIFO_DESC_BASE_OFFSET + 0x00, g_wp);
    g_p->current_chan_id = 1;
    channel_compute_dispatcher_drain(g_p, 1);
}

static void va_write(uint64_t va, const void *src, uint32_t n) {
    const uint8_t *s = src;
    while (n) { uint32_t vpn = (uint32_t)(va >> 14), in = (uint32_t)(va & 0x3fff), c = PG - in < n ? PG - in : n;
        assert(vpn < NVPN && vpn != 3 && vpn != 41);
        memcpy(g_mem + ((uint64_t)pfn_of(vpn) << 14) + in, s, c); va += c; s += c; n -= c; }
}
static void va_read(uint64_t va, void *dst, uint32_t n) {
    uint8_t *d = dst;
    while (n) { uint32_t vpn = (uint32_t)(va >> 14), in = (uint32_t)(va & 0x3fff), c = PG - in < n ? PG - in : n;
        assert(vpn < NVPN && vpn != 3 && vpn != 41);
        memcpy(d, g_mem + ((uint64_t)pfn_of(vpn) << 14) + in, c); va += c; d += c; n -= c; }
}
static void va_fill(uint64_t va, uint8_t v, uint32_t n) { uint8_t *b = malloc(n); memset(b, v, n); va_write(va, b, n); free(b); }

static void setup_guest(void) {
    memset(g_mem, 0, MEM_SIZE); g_wp = 0;
    g_p->ring_page_shift = 14; g_p->protocol_mode = LAGFX_PROTO_ARM64_VMAPPLE; g_p->ring_shared_page_pfn = SHARED_PFN;
    uint64_t d = ((uint64_t)SHARED_PFN << 14) + LAGFX_RING_FIFO_DESC_BASE_OFFSET;
    put32(d + 0x0c, 1); put32(d + 0x10, TABLE_PFN);
    for (uint32_t i = 0; i < 4; i++) put32(((uint64_t)TABLE_PFN << 14) + 4u * i, RING_PFN0 + i);
    put32((uint64_t)ROOT_PFN << 14, TOP_PFN); put32(((uint64_t)ROOT_PFN << 14) + 4, 2);
    put32((uint64_t)TOP_PFN << 14, LEAF_PFN);
    for (uint32_t v = 0; v < NVPN; v++) { if (v == 3 || v == 41) continue; put32(((uint64_t)LEAF_PFN << 14) + 4u * v, pfn_of(v) | (v == 5 ? 0x80000000u : 0)); }
}

static void put_obj(uint32_t ref, uint32_t type, uint32_t size, uint64_t at) { uint32_t e[3] = { type | size << 8, (uint32_t)at, (uint32_t)(at >> 32) }; va_write(VA_OBJ + ref * 12u, e, 12); }

static uint32_t g_desc_off;   /* next free byte in the descriptor page */
static void mk_buffer(uint32_t ref, uint32_t type, uint64_t len, uint32_t vpage) {
    uint8_t h[16]; memcpy(h, &len, 8); memcpy(h + 8, &vpage, 4); memset(h + 12, 0, 4);
    uint64_t at = VA_DESC + g_desc_off; va_write(at, h, 16); g_desc_off += 16; put_obj(ref, type, 16, at);
}
/* texture: uncompressed v1 description; one mip table (mips) given as {off,row,w,h,d} */
static void mk_texture(uint32_t ref, uint64_t len, uint32_t vpage, uint32_t mtype, uint32_t fmt, uint32_t w, uint32_t h, uint32_t depth, uint32_t mips,
                       uint32_t slices, uint64_t base, uint64_t stride, const uint64_t (*mip)[5]) {
    uint8_t d[512]; memset(d, 0, sizeof d);
    memcpy(d, &len, 8); memcpy(d + 8, &vpage, 4);
    uint8_t *lay = d + 12;
    uint16_t m16 = (uint16_t)mips, s16 = (uint16_t)slices; memcpy(lay, &m16, 2); memcpy(lay + 2, &s16, 2); memcpy(lay + 4, &base, 8); memcpy(lay + 12, &stride, 8);
    for (uint32_t i = 0; i < mips; i++) {
        uint8_t *mp = lay + 0x18 + 36u * i; uint64_t off = mip[i][0], row = mip[i][1]; uint32_t mw = (uint32_t)mip[i][2], mh = (uint32_t)mip[i][3], md = (uint32_t)mip[i][4];
        memcpy(mp, &off, 8); memcpy(mp + 0x10, &row, 8); memcpy(mp + 0x18, &mw, 4); memcpy(mp + 0x1c, &mh, 4); memcpy(mp + 0x20, &md, 4);
    }
    uint8_t *rec = lay + 0x18 + 36u * mips; uint32_t tag = 0x14; memcpy(rec, &tag, 4);
    uint8_t *ds = rec + 12; ds[0] = (uint8_t)mtype; ds[1] = 0x17; ds[2] = (uint8_t)fmt; ds[3] = (uint8_t)(fmt >> 8);
    memcpy(ds + 4, &w, 4); memcpy(ds + 8, &h, 4); memcpy(ds + 12, &depth, 4);
    uint16_t m2 = (uint16_t)mips, sm = 1, ar = (uint16_t)slices; memcpy(ds + 16, &m2, 2); memcpy(ds + 18, &sm, 2); memcpy(ds + 20, &ar, 2);
    uint32_t size = (uint32_t)(12 + 0x18 + 36u * mips + 12 + 28);
    uint64_t at = VA_DESC + g_desc_off; va_write(at, d, size); g_desc_off += (size + 15u) & ~15u; put_obj(ref, 2, size, at);
}

/* refs */
enum { R_A = 1, R_B, R_C, R_T, R_U, R_TALL, R_UNM, R_FUNC, R_TEX3D, R_EMPTY };

static void build_objects(void) {
    g_desc_off = 0;
    mk_buffer(R_A, 1, 2 * PG, VPN_A); mk_buffer(R_B, 1, 2 * PG, VPN_B); mk_buffer(R_C, 19, 2 * PG, VPN_C);
    const uint64_t m64[1][5] = {{0, 256, 64, 64, 1}};
    mk_texture(R_T, PG, VPN_T, 2, 70, 64, 64, 1, 1, 1, 0, 0, m64);
    mk_texture(R_U, PG, VPN_U, 2, 70, 64, 64, 1, 1, 1, 0, 0, m64);
    const uint64_t mt[1][5] = {{0, 128, 32, 600, 1}};   /* 32x600 RGBA8: 76800 bytes over 5 pages, rows straddle pages */
    mk_texture(R_TALL, 5 * PG, VPN_TALL, 2, 70, 32, 600, 1, 1, 1, 0, 0, mt);
    mk_buffer(R_UNM, 1, 2 * PG, VPN_UNM);
    put_obj(R_FUNC, 6, 16, VA_DESC);   /* a function object: unsupported by the mirror */
    const uint64_t m3[1][5] = {{0, 64, 16, 16, 4}};   /* 3D 16x16x4 R8? use RGBA8 (row 64), image 1024 */
    mk_texture(R_TEX3D, PG, VPN_UNM + 8 /*unmapped vpage content: reads as zero*/, 7, 70, 16, 16, 4, 1, 1, 0, 0, m3);
}

static void def_task(uint32_t task) {
    uint32_t w[4] = { task * 2u, 0, 0, ROOT_PFN }; submit(0x38, 0, w, 4);
    lagfx_task_entry_t *t = lagfx_protocol_find_task(g_p, task);
    assert(t && t->root_page_pfn == ROOT_PFN);
    uint32_t l[7] = { task, VA_OBJ, 0, 0x30000, 0, 0x3000, 0x1000 }; submit(0x44, 0, l, 7);
    assert(t->lists_set && t->objects_addr == VA_OBJ);
}

/* ---- blit command builders (payload layouts from pvg_metal.m blit()) ---- */
typedef struct { uint8_t b[8192]; uint32_t n; } cmds_t;
static void w32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void w64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static uint8_t *add(cmds_t *c, uint32_t tag, uint32_t payload) { uint8_t *p = c->b + c->n; memset(p, 0, 8 + payload); w32(p, tag); w32(p + 4, 8 + payload); c->n += 8 + payload; return p + 8; }
static void c300(cmds_t *c, uint32_t buf, uint32_t tex, uint64_t off, uint64_t row, uint64_t img, uint64_t w, uint64_t h, uint64_t d, uint64_t ox, uint64_t oy, uint64_t oz, uint16_t slice, uint16_t level) {
    uint8_t *p = add(c, 300, 0x58); w32(p, buf); w32(p + 4, tex); w64(p + 8, off); w64(p + 0x10, row); w64(p + 0x18, img); w64(p + 0x20, w); w64(p + 0x28, h); w64(p + 0x30, d);
    w64(p + 0x38, ox); w64(p + 0x40, oy); w64(p + 0x48, oz); p[0x50] = (uint8_t)slice; p[0x51] = (uint8_t)(slice >> 8); p[0x52] = (uint8_t)level; p[0x53] = (uint8_t)(level >> 8);
}
static void c301(cmds_t *c, uint32_t a, uint32_t b, uint64_t so, uint64_t dof, uint64_t n) { uint8_t *p = add(c, 301, 0x20); w32(p, a); w32(p + 4, b); w64(p + 8, so); w64(p + 0x10, dof); w64(p + 0x18, n); }
static void c302(cmds_t *c, uint32_t tex, uint32_t buf, uint64_t ox, uint64_t oy, uint64_t oz, uint64_t w, uint64_t h, uint64_t d, uint16_t slice, uint16_t level, uint64_t off, uint64_t row, uint64_t img) {
    uint8_t *p = add(c, 302, 0x58); w32(p, tex); w32(p + 4, buf); w64(p + 8, ox); w64(p + 0x10, oy); w64(p + 0x18, oz); w64(p + 0x20, w); w64(p + 0x28, h); w64(p + 0x30, d);
    w64(p + 0x38, off); w64(p + 0x40, row); w64(p + 0x48, img); p[0x50] = (uint8_t)slice; p[0x52] = (uint8_t)level;
}
static void c303(cmds_t *c, uint32_t a, uint32_t b, uint64_t sx, uint64_t sy, uint64_t sz, uint64_t w, uint64_t h, uint64_t d, uint64_t dx, uint64_t dy, uint64_t dz,
                 uint16_t ss, uint16_t sl, uint16_t ds, uint16_t dl) {
    uint8_t *p = add(c, 303, 0x58); w32(p, a); w32(p + 4, b); w64(p + 8, sx); w64(p + 0x10, sy); w64(p + 0x18, sz); w64(p + 0x20, w); w64(p + 0x28, h); w64(p + 0x30, d);
    w64(p + 0x38, dx); w64(p + 0x40, dy); w64(p + 0x48, dz); p[0x50] = (uint8_t)ss; p[0x52] = (uint8_t)sl; p[0x54] = (uint8_t)ds; p[0x56] = (uint8_t)dl;
}
static void c306(cmds_t *c, uint32_t buf, uint64_t loc, uint64_t len, uint8_t v) { uint8_t *p = add(c, 306, 0x18); w32(p, buf); w64(p + 4, loc); w64(p + 12, len); p[20] = v; }

/* run one exec: resources {ref, flags}, one blit segment */
static void run(const uint32_t *rf, uint32_t nres, cmds_t *c) {
    uint8_t seg[8300]; uint32_t total = 8 + c->n;
    w32(seg, total); w32(seg + 4, 2 /* type blit */); memcpy(seg + 8, c->b, c->n);   /* real guests: seglen = whole buffer */
    va_write((uint64_t)VPN_CMD << 14, seg, total);
    uint32_t w[64], n = 0; w[n++] = 1; w[n++] = nres; w[n++] = 1;
    for (uint32_t i = 0; i < nres * 2; i++) w[n++] = rf[i];
    uint64_t off = (uint64_t)VPN_CMD << 14; w[n++] = (uint32_t)off; w[n++] = (uint32_t)(off >> 32); w[n++] = total; w[n++] = 0;
    submit(0x43, 0, w, n);
}

static void pat(uint8_t *b, uint32_t n, uint32_t seed) { for (uint32_t i = 0; i < n; i++) b[i] = (uint8_t)((i * 131u + seed * 17u + (i >> 8) * 29u) ^ 0x5a); }

static const lagfx_a64_stats_t *S(void) { return lagfx_a64_res_stats(g_p); }

static void test_buffer_copy_and_fill(void) {
    build_objects();
    uint8_t a[2 * PG], got[2 * PG], exp[2 * PG];
    pat(a, sizeof a, 1); va_write((uint64_t)VPN_A << 14, a, sizeof a);
    va_fill((uint64_t)VPN_B << 14, 0x11, 2 * PG);
    /* copy across the page boundary of A into B across the boundary at a different offset; download B (flag 4) */
    cmds_t c = {0}; c301(&c, R_A, R_B, PG - 0x40, PG - 0x18, 0x100);
    uint32_t rf[4] = { R_A, 1, R_B, 1 | 4 };
    uint64_t done0 = S()->blit_done;
    run(rf, 2, &c);
    memset(exp, 0x11, sizeof exp); memcpy(exp + PG - 0x18, a + PG - 0x40, 0x100);
    va_read((uint64_t)VPN_B << 14, got, sizeof got);
    CHECK(!memcmp(got, exp, sizeof got), "301 buffer copy across page boundaries reaches the guest pages");
    CHECK(S()->blit_done == done0 + 1 && S()->blit_tag[1] >= 1, "301 counted");
    /* fills: aligned, unaligned head/tail, crossing a page, tiny inside one word, zero length */
    va_write((uint64_t)VPN_B << 14, a, sizeof a);
    c.n = 0; c306(&c, R_B, 0, 0x20, 0xAB); c306(&c, R_B, 0x103, 10, 0xCD); c306(&c, R_B, PG - 3, 7, 0xEF); c306(&c, R_B, 0x2001, 2, 0x42); c306(&c, R_B, 0x3000, 0, 0x99);
    rf[2] = R_B, rf[3] = 1 | 4; rf[0] = 0;
    run(rf + 2, 1, &c);
    memcpy(exp, a, sizeof exp); memset(exp, 0xAB, 0x20); memset(exp + 0x103, 0xCD, 10); memset(exp + PG - 3, 0xEF, 7); memset(exp + 0x2001, 0x42, 2);
    va_read((uint64_t)VPN_B << 14, got, sizeof got);
    CHECK(!memcmp(got, exp, sizeof got), "306 fill (aligned/unaligned/straddling/tiny/zero) exact, neighbours untouched");
}

static void test_texture_roundtrip(void) {
    build_objects();
    uint8_t a[2 * PG], got[2 * PG];
    for (uint32_t i = 0; i < sizeof a; i++) a[i] = (uint8_t)(i * 7 + (i >> 9));
    va_write((uint64_t)VPN_A << 14, a, sizeof a);
    va_fill((uint64_t)VPN_B << 14, 0, 2 * PG);
    /* A[0x100 ...] rows of 16 px (64 B), stride 80 B -> T at (8,4) 16x8; then T -> B rows (stride 128) at 0x40; real command size 0x60 */
    cmds_t c = {0};
    c300(&c, R_A, R_T, 0x100, 80, 80 * 8, 16, 8, 1, 8, 4, 0, 0, 0);
    CHECK(c.b[4] == 0x60 && c.b[0] == 0x2c && c.b[1] == 0x01, "tag 300 has the real size 0x60");
    c302(&c, R_T, R_B, 8, 4, 0, 16, 8, 1, 0, 0, 0x40, 128, 128 * 8);
    uint32_t rf[6] = { R_A, 1, R_T, 1, R_B, 1 | 4 };
    run(rf, 3, &c);
    va_read((uint64_t)VPN_B << 14, got, sizeof got);
    bool ok = true;
    for (uint32_t y = 0; y < 8; y++) for (uint32_t x = 0; x < 64; x++) if (got[0x40 + y * 128 + x] != a[0x100 + y * 80 + x]) ok = false;
    for (uint32_t i = 0; i < 0x40; i++) if (got[i]) ok = false;
    for (uint32_t y = 0; y < 8; y++) for (uint32_t x = 64; x < 128; x++) if (got[0x40 + y * 128 + x]) ok = false;
    CHECK(ok, "300 then 302 round trip: exact rows, rest zero");
    /* 303: copy a sub-rect T -> U, read back from U */
    c.n = 0; c303(&c, R_T, R_U, 10, 5, 0, 8, 4, 1, 40, 50, 0, 0, 0, 0, 0); c302(&c, R_U, R_B, 40, 50, 0, 8, 4, 1, 0, 0, 0, 32, 128);
    uint32_t rf2[4] = { R_T, 0, R_B, 1 | 4 };
    uint8_t before[PG]; va_read((uint64_t)VPN_B << 14, before, 0);   (void)before;
    run(rf2, 2, &c);
    va_read((uint64_t)VPN_B << 14, got, 4 * 32);
    ok = true;
    for (uint32_t y = 0; y < 4; y++) for (uint32_t x = 0; x < 32; x++) if (got[y * 32 + x] != a[0x100 + (5 + y - 4) * 80 + (10 - 8) * 4 + x]) ok = false;
    CHECK(ok, "303 texture to texture sub-rect (then read back through U)");
}

static void test_tall_texture_straddle(void) {
    build_objects();
    /* the guest's 32x600 texture: 128-byte rows, pages break mid-row-group; guest data uploaded on first use, GPU copy out, GPU edit written back */
    uint8_t *tex = malloc(5 * PG), *got = malloc(2 * PG);
    pat(tex, 600 * 128, 9); memset(tex + 600 * 128, 0, 5 * PG - 600 * 128);
    va_write((uint64_t)VPN_TALL << 14, tex, 5 * PG);
    cmds_t c = {0};
    c302(&c, R_TALL, R_B, 0, 120, 0, 32, 40, 1, 0, 0, 0, 128, 128 * 40);   /* rows 120..159 cross the first page boundary (row 128 = byte 16384) */
    uint32_t rf[4] = { R_TALL, 1, R_B, 1 | 4 };
    run(rf, 2, &c);
    va_read((uint64_t)VPN_B << 14, got, 128 * 40);
    CHECK(!memcmp(got, tex + 120 * 128, 128 * 40), "texture whose rows straddle guest pages uploads exactly");
    /* GPU writes rows 125..130 of the texture from buffer A (fill A first), flag 4 on the texture -> guest pages */
    va_fill((uint64_t)VPN_A << 14, 0x6C, 2 * PG);
    c.n = 0; c300(&c, R_A, R_TALL, 0, 128, 128 * 6, 32, 6, 1, 0, 125, 0, 0, 0);
    uint32_t rf2[4] = { R_A, 1, R_TALL, 4 };
    run(rf2, 2, &c);
    uint8_t *after = malloc(5 * PG); va_read((uint64_t)VPN_TALL << 14, after, 5 * PG);
    memcpy(tex + 125 * 128, (uint8_t[1]){0}, 0); memset(tex + 125 * 128, 0x6C, 6 * 128);
    CHECK(!memcmp(after, tex, 5 * PG), "flag 4 writes the edited texture back through the page table (straddling pages)");
    free(tex); free(got); free(after);
}

static void test_flag_semantics(void) {
    build_objects();
    uint8_t a[2 * PG], got[2 * PG];
    pat(a, sizeof a, 3); va_write((uint64_t)VPN_A << 14, a, sizeof a);
    va_fill((uint64_t)VPN_C << 14, 0, 2 * PG);
    /* exec 1: GPU fills B with 0x77, flags 1|2 (guest wrote it, GPU changes it); no flag 4 -> guest copy of B keeps the old bytes and is stale */
    va_fill((uint64_t)VPN_B << 14, 0x21, 2 * PG);
    cmds_t c = {0}; c306(&c, R_B, 0, 2 * PG, 0x77);
    uint32_t rf1[2] = { R_B, 1 | 2 };
    run(rf1, 1, &c);
    va_read((uint64_t)VPN_B << 14, got, 64);
    CHECK(got[0] == 0x21, "flag 2 without 4: the guest copy is not updated");
    /* exec 2: B flags 0: copy B -> C; C flag 4. The stale guest bytes (0x21) must NOT replace the GPU result (0x77) */
    c.n = 0; c301(&c, R_B, R_C, 0, 0, 2 * PG);
    uint32_t rf2[4] = { R_B, 0, R_C, 1 | 4 };
    uint64_t stale0 = S()->stale_skips;
    run(rf2, 2, &c);
    va_read((uint64_t)VPN_C << 14, got, sizeof got);
    bool all77 = true; for (uint32_t i = 0; i < sizeof got; i++) if (got[i] != 0x77) all77 = false;
    CHECK(all77, "stale guest data does not overwrite the GPU's result (no 'black rectangle')");
    CHECK(S()->stale_skips > stale0, "stale skip counted");
    /* exec 3: the guest rewrites B and says so (flag 1): now the guest data wins */
    va_fill((uint64_t)VPN_B << 14, 0x33, 2 * PG);
    uint32_t rf3[4] = { R_B, 1, R_C, 1 | 4 };
    run(rf3, 2, &c);
    va_read((uint64_t)VPN_C << 14, got, sizeof got);
    bool all33 = true; for (uint32_t i = 0; i < sizeof got; i++) if (got[i] != 0x33) all33 = false;
    CHECK(all33, "flag 1 makes the guest's bytes authoritative again");
    /* exec 4: GPU result with flags 2|4: written back, stale cleared, so a later exec w/o flag 1 sees the guest copy (== GPU result) */
    c.n = 0; c306(&c, R_B, 0, 2 * PG, 0x55);
    uint32_t rf4[2] = { R_B, 1 | 2 | 4 };
    run(rf4, 1, &c);
    va_read((uint64_t)VPN_B << 14, got, 64);
    CHECK(got[0] == 0x55 && got[63] == 0x55, "flag 4 syncs the GPU result to the guest");
    /* texture variant: GPU writes T (flag 2), a later exec without flag 1 must read the GPU content, not the guest's zeros */
    va_fill((uint64_t)VPN_A << 14, 0xE0, 2 * PG);
    c.n = 0; c300(&c, R_A, R_T, 0, 256, 256 * 64, 64, 64, 1, 0, 0, 0, 0, 0);
    uint32_t rf5[4] = { R_A, 1, R_T, 1 | 2 };
    run(rf5, 2, &c);
    c.n = 0; c302(&c, R_T, R_C, 0, 0, 0, 64, 64, 1, 0, 0, 0, 256, 256 * 64);
    uint32_t rf6[4] = { R_T, 0, R_C, 1 | 4 };
    run(rf6, 2, &c);
    va_read((uint64_t)VPN_C << 14, got, 256 * 4);
    CHECK(got[0] == 0xE0 && got[255] == 0xE0, "texture: GPU content survives an exec without flag 1 (guest zeros not uploaded over it)");
}

static void test_bad_inputs(void) {
    build_objects();
    uint8_t a[2 * PG]; pat(a, sizeof a, 5); va_write((uint64_t)VPN_A << 14, a, sizeof a); va_fill((uint64_t)VPN_B << 14, 0x44, 2 * PG);
    const uint64_t BIG = ~0ull;
    cmds_t c = {0};
    c301(&c, R_A, R_B, BIG, 0, 16); c301(&c, R_A, R_B, 0, BIG - 3, 16); c301(&c, R_A, R_B, 0, 0, BIG); c301(&c, R_A, R_B, 100, 110, 50);   /* overlapping within a buffer is separate; this one is A->B so fine */
    c301(&c, R_A, R_A, 0, 8, 16);                              /* overlap in one buffer: rejected */
    c301(&c, 0, R_B, 0, 0, 4); c301(&c, 77, R_B, 0, 0, 4); c301(&c, R_FUNC, R_B, 0, 0, 4); c301(&c, R_T, R_B, 0, 0, 4);   /* null / unknown / function / texture-as-buffer */
    c306(&c, R_B, BIG, 4, 1); c306(&c, R_B, 4, BIG, 1); c306(&c, R_B, 2 * PG, 1, 1); c306(&c, R_T, 0, 4, 1); c306(&c, 0, 0, 4, 1);
    c300(&c, R_A, R_T, 0, 256, 0, 65, 1, 1, 0, 0, 0, 0, 0);    /* wider than the texture */
    c300(&c, R_A, R_T, 0, 256, 0, 1, 1, 1, 64, 0, 0, 0, 0);    /* origin outside */
    c300(&c, R_A, R_T, 0, 256, 0, 1, 1, 1, BIG, BIG, BIG, 0, 0);
    c300(&c, R_A, R_T, 0, 256, 0, BIG, BIG, BIG, 0, 0, 0, 0, 0);
    c300(&c, R_A, R_T, 0, 0, 0, 4, 4, 1, 0, 0, 0, 0, 0);       /* row 0 */
    c300(&c, R_A, R_T, 0, 2, 0, 4, 4, 1, 0, 0, 0, 0, 0);       /* row < w * bpp */
    c300(&c, R_A, R_T, 0, 255, 0, 4, 4, 1, 0, 0, 0, 0, 0);     /* row not a multiple of the pixel size */
    c300(&c, R_A, R_T, 2, 256, 0, 4, 4, 1, 0, 0, 0, 0, 0);     /* unaligned offset */
    c300(&c, R_A, R_T, 2 * PG - 8, 256, 0, 4, 4, 1, 0, 0, 0, 0, 0);   /* source runs past the buffer */
    c300(&c, R_A, R_T, BIG - 3, BIG - 3, BIG - 3, 4, 4, 1, 0, 0, 0, 0, 0);
    c300(&c, R_A, R_T, 0, 256, 0, 4, 4, 1, 0, 0, 0, 3, 0);     /* slice 3 of a 1-slice texture */
    c300(&c, R_A, R_T, 0, 256, 0, 4, 4, 1, 0, 0, 0, 0, 5);     /* level 5 of a 1-level texture */
    c300(&c, R_A, R_T, 0, 256, 0, 4, 4, 2, 0, 0, 0, 0, 0);     /* depth 2 into a 2D texture */
    c300(&c, R_T, R_A, 0, 256, 0, 4, 4, 1, 0, 0, 0, 0, 0);     /* operands swapped */
    c300(&c, R_A, R_FUNC, 0, 256, 0, 4, 4, 1, 0, 0, 0, 0, 0);
    c302(&c, R_T, R_B, 0, 0, 0, 4, 4, 1, 0, 0, 2 * PG - 8, 256, 0);   /* destination runs past the buffer */
    c302(&c, R_T, R_B, 60, 60, 0, 8, 8, 1, 0, 0, 0, 256, 0);          /* source box outside */
    c303(&c, R_T, R_U, 0, 0, 0, 65, 1, 1, 0, 0, 0, 0, 0, 0, 0);
    c303(&c, R_T, R_U, 0, 0, 0, 8, 8, 1, 60, 60, 0, 0, 0, 0, 0);       /* destination box outside */
    c303(&c, R_T, R_T, 0, 0, 0, 8, 8, 1, 4, 4, 0, 0, 0, 0, 0);         /* overlapping in one image */
    c303(&c, R_T, R_TEX3D, 0, 0, 0, 4, 4, 1, 0, 0, 0, 0, 0, 0, 0);    /* 2D -> 3D */
    c303(&c, R_A, R_U, 0, 0, 0, 4, 4, 1, 0, 0, 0, 0, 0, 0, 0);        /* a buffer as texture */
    { uint8_t *p = add(&c, 300, 0x10); (void)p; }                    /* too short for its tag */
    { uint8_t *p = add(&c, 306, 4); (void)p; }
    { uint8_t *p = add(&c, 9999, 0x20); (void)p; add(&c, 307, 4); }  /* unknown tag, generate-mipmaps: counted unsupported */
    lagfx_a64_stats_t s0 = *S();
    uint32_t rf[4] = { R_A, 1, R_B, 1 | 4 };
    /* one exec per command, so a command that is wrongly accepted is identified */
    uint32_t q = 0, idx = 0, wrongly_done = 0, silent = 0;
    while (q < c.n) {
        uint32_t tag = (uint32_t)c.b[q] | (uint32_t)c.b[q + 1] << 8, sz = c.b[q + 4] | c.b[q + 5] << 8;
        cmds_t one = {0}; memcpy(one.b, c.b + q, sz); one.n = sz;
        lagfx_a64_stats_t b0 = *S();
        run(rf, 2, &one);
        bool valid = (idx == 3);   /* the A->B 100..150 copy */
        uint64_t dn = S()->blit_done - b0.blit_done, rj = S()->blit_rejected - b0.blit_rejected, un = S()->blit_unsupported - b0.blit_unsupported;
        if (valid ? dn != 1 : dn != 0) wrongly_done++, fprintf(stderr, "dbg: command #%u tag %u done=%llu\n", idx, tag, (unsigned long long)dn);
        if (!valid && tag != 307 && tag != 9999 && rj != 1) silent++, fprintf(stderr, "dbg: command #%u tag %u not rejected (rej=%llu un=%llu)\n", idx, tag, (unsigned long long)rj, (unsigned long long)un);
        q += sz, idx++;
    }
    CHECK(wrongly_done == 0, "only the single valid command ran; every bad one was refused");
    CHECK(silent == 0, "every bad command counted as rejected");
    CHECK(S()->blit_unsupported >= s0.blit_unsupported + 2, "307 / unknown tags counted unsupported");
    uint8_t got[2 * PG]; va_read((uint64_t)VPN_B << 14, got, sizeof got);
    uint8_t exp[2 * PG]; memset(exp, 0x44, sizeof exp); memcpy(exp + 110, a + 100, 50);
    CHECK(!memcmp(got, exp, sizeof got), "rejected commands changed nothing");
    /* a buffer over unmapped guest pages: reads as zero, write-back skips the hole, nothing crashes */
    va_fill((uint64_t)VPN_UNM << 14, 0x99, PG);
    c.n = 0; c301(&c, R_UNM, R_B, 0, 0, 2 * PG);
    uint32_t rf2[4] = { R_UNM, 1 | 4, R_B, 1 | 4 };
    uint64_t um0 = S()->unmapped_pages;
    run(rf2, 2, &c);
    va_read((uint64_t)VPN_B << 14, got, sizeof got);
    CHECK(got[0] == 0x99 && got[PG] == 0 && S()->unmapped_pages > um0, "unmapped guest page: zero fill, write-back skipped, counted");
    /* an exec referencing refs that do not resolve, an empty slot and a bad task: no crash */
    uint32_t rf3[8] = { 0, 1, 9999, 1, R_EMPTY, 1, R_FUNC, 7 };
    c.n = 0; run(rf3, 4, &c);
    uint32_t w[8] = { 55, 0, 1, (uint32_t)((uint64_t)VPN_CMD << 14), 0, 16, 0, 0 }; submit(0x43, 0, w, 8);   /* unknown task */
}

static void rescmd(uint16_t op, const uint32_t *w, uint32_t n) { submit(op, 0, w, n); }

static void test_sync_invalidate(void) {
    build_objects();
    uint8_t got[2 * PG];
    va_fill((uint64_t)VPN_A << 14, 0x12, 2 * PG); va_fill((uint64_t)VPN_B << 14, 0x00, 2 * PG);
    /* GPU fill of B with flags 1|2 (no flag 4): guest copy stays 0, mirror marks it stale */
    cmds_t c = {0}; c306(&c, R_B, 0, 2 * PG, 0xA5);
    uint32_t rf[2] = { R_B, 1 | 2 };
    run(rf, 1, &c);
    va_read((uint64_t)VPN_B << 14, got, 16); CHECK(got[0] == 0, "before synchronize the guest still has its own bytes");
    /* an unknown ref is skipped, counted, no crash */
    uint32_t sy[7] = { 1, 3, 999, R_B, R_TALL };
    uint64_t sk0 = S()->sync_skipped;
    rescmd(0x35, sy, 5);
    va_read((uint64_t)VPN_B << 14, got, sizeof got);
    bool all = true; for (uint32_t i = 0; i < sizeof got; i++) if (got[i] != 0xA5) all = false;
    CHECK(all, "0x35 synchronize writes the GPU's bytes to the guest pages");
    CHECK(S()->sync_cmds >= 1 && S()->sync_skipped >= sk0 + 1, "0x35 skipped objects counted");
    /* 0x3e is the same operation: GPU writes again (flag 2 only), then 0x3e */
    c.n = 0; c306(&c, R_B, 0, 0x40, 0x3C); uint32_t rf2[2] = { R_B, 2 };
    run(rf2, 1, &c);
    uint32_t sy2[3] = { 1, 1, R_B }; rescmd(0x3e, sy2, 3);
    va_read((uint64_t)VPN_B << 14, got, 0x41);
    CHECK(got[0] == 0x3C && got[0x3f] == 0x3C && got[0x40] == 0xA5, "0x3e synchronize");
    /* 0x34 invalidate: guest-invalid bit (e[6]) marks stale: an exec without flag 1 keeps the GPU bytes; guest-valid (e[7]) lets the guest's win again */
    va_fill((uint64_t)VPN_B << 14, 0x77, 2 * PG);
    uint8_t inv[8] = { R_B, 0, 0, 0, 0, 0, 1, 0 };   /* guest invalid */
    uint32_t w[4]; w[0] = 1; w[1] = 1; memcpy(w + 2, inv, 8);
    rescmd(0x34, w, 4);
    c.n = 0; c301(&c, R_B, R_C, 0, 0, 0x10); uint32_t rf3[4] = { R_B, 0, R_C, 1 | 4 };
    run(rf3, 2, &c); va_read((uint64_t)VPN_C << 14, got, 0x10);
    CHECK(got[0] == 0x3C, "0x34 guest-invalid: guest bytes (0x77) not uploaded over the GPU result");
    uint8_t inv2[8] = { R_B, 0, 0, 0, 0, 0, 0, 1 };  /* guest valid */
    memcpy(w + 2, inv2, 8); rescmd(0x34, w, 4);
    run(rf3, 2, &c); va_read((uint64_t)VPN_C << 14, got, 0x10);
    CHECK(got[0] == 0x77, "0x34 guest-valid: the guest bytes are used again");
    uint8_t inv3[8] = { 77, 0, 0, 0, 1, 0, 0, 0 };   /* unknown ref: ignored */
    memcpy(w + 2, inv3, 8); rescmd(0x34, w, 4);
    CHECK(S()->inv_cmds == 3 && S()->inv_objs == 2 && S()->inv_missing == 1, "0x34 counters");
    /* 0x3f hint, truncated / absurd counts */
    uint32_t h[2] = { 1, 0 }; rescmd(0x3f, h, 2);
    uint32_t bad[2] = { 1, 0x7fffffffu }; rescmd(0x35, bad, 2); rescmd(0x34, bad, 2); rescmd(0x35, bad, 1);
    CHECK(S()->hint_cmds == 1 && S()->res_cmd_bad == 3, "0x3f counted; bad sizes refused without crash");
    /* disabled: dispatcher falls back to the old log+ack path, nothing changes */
    setenv("LAGFX_ARM64_EXEC", "0", 1);
    uint64_t sc = S()->sync_cmds; rescmd(0x35, sy2, 3); rescmd(0x3e, sy2, 3);
    CHECK(S()->sync_cmds == sc, "OFF: 0x35/0x3e not acted on");
    setenv("LAGFX_ARM64_EXEC", "1", 1);
}

static void test_gating(void) {
    build_objects();
    uint8_t a[PG]; pat(a, sizeof a, 8); va_write((uint64_t)VPN_A << 14, a, sizeof a); va_fill((uint64_t)VPN_B << 14, 0x11, 2 * PG);
    cmds_t c = {0}; c301(&c, R_A, R_B, 0, 0, 256);
    uint32_t rf[4] = { R_A, 1, R_B, 1 | 4 };
    uint64_t e0 = S()->execs;
    unsetenv("LAGFX_ARM64_EXEC");
    run(rf, 2, &c);
    uint8_t got[256]; va_read((uint64_t)VPN_B << 14, got, 256);
    CHECK(got[0] == 0x11 && S()->execs == e0, "default OFF: nothing executed, guest memory untouched");
    setenv("LAGFX_ARM64_EXEC", "0", 1);
    run(rf, 2, &c); va_read((uint64_t)VPN_B << 14, got, 256);
    CHECK(got[0] == 0x11 && S()->execs == e0, "LAGFX_ARM64_EXEC=0: still off");
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    /* x86 mode: never executes even when the env is set */
    g_p->protocol_mode = 0;
    CHECK(lagfx_a64_res_begin(g_p, 1, (const uint8_t *)rf, 2) == NULL, "x86 mode: begin refuses");
    g_p->protocol_mode = LAGFX_PROTO_ARM64_VMAPPLE;
    run(rf, 2, &c); va_read((uint64_t)VPN_B << 14, got, 256);
    CHECK(!memcmp(got, a, 256) && S()->execs == e0 + 1, "ON: the copy happens");
}

int main(void) {
    setenv("LAGFX_VK_VALIDATION", "1", 1);
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    g_mem = calloc(1, MEM_SIZE);
    lagfx_device_descriptor_t dd; memset(&dd, 0, sizeof dd);
    dd.shell.read_memory = rd; dd.shell.write_memory = wr; dd.shell.raise_interrupt = irq; dd.thread_count = 2;
    dd.protocol_mode = LAGFX_PROTO_ARM64_VMAPPLE; dd.ring_page_shift = 14;
    char *err = NULL;
    g_dev = lagfx_device_new(&dd, &err);
    if (!g_dev || !g_dev->vk || !g_dev->vk->initialized) { printf("SKIP: no Vulkan device (%s)\n", err ? err : "?"); return 0; }
    g_p = g_dev->protocol_state;
    setup_guest(); def_task(1);
    test_buffer_copy_and_fill();
    setup_guest(); def_task(1); test_texture_roundtrip();
    setup_guest(); def_task(1); test_tall_texture_straddle();
    setup_guest(); def_task(1); test_flag_semantics();
    setup_guest(); def_task(1); test_bad_inputs();
    setup_guest(); def_task(1); test_sync_invalidate();
    setup_guest(); def_task(1); test_gating();
    CHECK(lagfx_vk_validation_msg_count == 0, "no validation-layer messages");
    CHECK(S()->gpu_fail == 0 && S()->gpu_timeout == 0, "no GPU failures/timeouts");
    printf("bad_object=%llu res=%llu\n", (unsigned long long)S()->bad_object, (unsigned long long)S()->res); printf("execs=%llu blit_done=%llu rejected=%llu noop=%llu unsupported=%llu uploads=%llu downloads=%llu stale_skips=%llu validation=%llu\n",
           (unsigned long long)S()->execs, (unsigned long long)S()->blit_done, (unsigned long long)S()->blit_rejected, (unsigned long long)S()->blit_noop,
           (unsigned long long)S()->blit_unsupported, (unsigned long long)S()->uploads, (unsigned long long)S()->downloads, (unsigned long long)S()->stale_skips,
           (unsigned long long)lagfx_vk_validation_msg_count);
    lagfx_device_free(g_dev);
    free(g_mem);
    printf(g_fail ? "FAILED (%d)\n" : "ALL PASS\n", g_fail);
    return g_fail != 0;
}
