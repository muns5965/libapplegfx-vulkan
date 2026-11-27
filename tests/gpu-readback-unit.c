/* F-2 independent GPU readback (src/vulkan/gpu_readback.c): pure compare / classify / gate / format tests (no GPU) and a lavapipe + validation integration
 * test that renders a known binary16 pattern into an RGBA16Float mapped surface, takes the independent vkCmdCopyImageToBuffer copy (A), and compares it with the
 * write-back path (staging B, fresh guest-page read C) for the same swap / mapping / generation. Negative controls: a corrupted staging byte, a corrupted GPU copy,
 * a corrupted guest read and a wrong stride must all be detected (FAIL) and never counted as a pass. Off by default: with no env var nothing is recorded.
 * Activation matrix (split flags): neither -> nothing; LAGFX_WB_VERIFY=1 only -> NO independent readback (no candidate, no GPU copy, no buffer, no wait accounting);
 * LAGFX_GPU_READBACK=1 only -> mode AB (A vs B, guest pages not re-read); both -> mode ABC. Also: identity checks (swap / mapping / generation / image / subresource /
 * fence), layout precondition, host-read barrier count, non-coherent invalidate, row pitch, a slot freed while pending, failure injection leaves the write-back unchanged.
 * lavapipe only; NOT verified on a hardware GPU.
 * SPDX-License-Identifier: MIT */
#include "dispatchers/channel_compute_dispatcher.h"
#include "dispatchers/ring_common.h"
#include "protocol/state.h"
#include "protocol/protocol.h"
#include "protocol/arm64_exec_decode.h"
#include "protocol/arm64_exec_run.h"
#include "protocol/arm64_resources.h"
#include "protocol/arm64_completion.h"
#include "protocol/arm64_objects.h"
#include "device.h"
#include "libapplegfx-vulkan.h"
#include "vulkan/instance.h"
#include "vulkan/command.h"
#include <vulkan/vulkan.h>
#include "arm64-exec-run-shaders.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/stat.h>
#include "protocol/arm64_dump.h"
#include "vulkan/gpu_readback.h"

extern uint64_t lagfx_vk_validation_msg_count;

#define MEM_SIZE   (2u << 20)
#define SHARED_PFN 1u
#define TABLE_PFN  2u
#define RING_PFN0  4u
#define STAMP_PFN  8u
#define ROOT_PFN   16u
#define TOP_PFN    17u
#define LEAF_PFN   18u
#define DATA_PFN0  32u
#define NVPN       24u

static uint8_t *g_mem;
static int g_irq;
static uint32_t g_stamps[256]; static int g_nstamps; static atomic_uint g_last_stamp;   /* every value written to the stamp cell of slot 1, in order */
static uint64_t g_rd_fail_lo, g_rd_fail_hi;   /* fault injection: reads overlapping [lo, hi) fail */
static uint64_t g_rd_calls;   /* every guest-memory read (the matrix test compares mode AB with mode ABC) */
static bool rd(void *o, uint64_t gpa, uint64_t len, void *dst) { (void)o; g_rd_calls++; if (gpa + len > MEM_SIZE) return false; if (g_rd_fail_hi && gpa < g_rd_fail_hi && gpa + len > g_rd_fail_lo) return false; memcpy(dst, g_mem + gpa, len); return true; }
static bool wr(void *o, uint64_t gpa, uint64_t len, const void *src) { (void)o; if (gpa + len > MEM_SIZE) return false;
    if (len == 4 && gpa == ((uint64_t)STAMP_PFN << 14) + 4 && g_nstamps < 256) { memcpy(&g_stamps[g_nstamps++], src, 4); atomic_store(&g_last_stamp, g_stamps[g_nstamps - 1]); } memcpy(g_mem + gpa, src, len); return true; }
static void irq(void *o, uint32_t v) { (void)o; (void)v; g_irq++; }
   /* only called under the execution lock / cq lock: one thread at a time */
static lagfx_task_t *cb_create_task(void *o, uint64_t s, void **p) { (void)o; (void)s; (void)p; return NULL; }
static void cb_destroy_task(void *o, lagfx_task_t *t) { (void)o; (void)t; }
static bool cb_map(void *a, lagfx_task_t *t, uint64_t o, const lagfx_physical_range_t *r, size_t c, bool ro) { (void)a; (void)t; (void)o; (void)r; (void)c; (void)ro; return true; }
static bool cb_unmap(void *a, lagfx_task_t *t, uint64_t o, uint64_t l) { (void)a; (void)t; (void)o; (void)l; return true; }
static void put32(uint64_t gpa, uint32_t v) { memcpy(g_mem + gpa, &v, 4); }
static uint32_t cell(uint32_t slot) { uint32_t v; memcpy(&v, g_mem + ((uint64_t)STAMP_PFN << 14) + 4u * slot, 4); return v; }
static uint32_t g_wp;
static lagfx_device_t *g_dev;

/* ---- the fake shell's surface table: mapping id -> descriptor bytes ---- */
#define PFN_LIST   60u    /* page list of the surfaces (u32 per 16 KiB page) */
#define PFN_SURF0  64u    /* surface pages */
#define PFN_LIST2  61u
#define SW 64u
#define SH 80u
#define SROW 320u         /* > SW * 4 = 256: 64 bytes of row padding */
#define SFIRST 0x100u     /* first-byte offset; 0x100 + 320 * 80 spans two 16 KiB pages */
#define SENT 0xabu
#define HROW 640u         /* RGhA: 64 * 8 = 512 bytes of pixels + 128 of padding; 0x100 + 640 * 80 spans four pages */
enum { MAP_OK = 7, MAP_OK2 = 17, MAP_RGHA = 8, MAP_HALF = 12, MAP_HOLE = 9, MAP_BADLIST = 10, MAP_BADGEOM = 11, MAP_NONE = 99 };
static int g_get_calls;
static void mkdesc(uint8_t *d, uint32_t list_gpa, uint32_t fourcc, uint64_t row, uint32_t first) {
    memset(d, 0, 0x70);
    uint32_t l = (list_gpa >> 12) | 1u; uint64_t alloc = row * SH, w = SW, h = SH;
    memcpy(d, &l, 4); memcpy(d + 4, &fourcc, 4); memcpy(d + 0x18, &alloc, 8); memcpy(d + 0x20, &w, 8); memcpy(d + 0x28, &h, 8); memcpy(d + 0x30, &row, 8);
    d[0x3e] = 2; memcpy(d + 0x60, &first, 4);
}
static int g_desc_fail_n;   /* the next n descriptor lookups fail (fault injection) */
static uint64_t g_remap_list;   /* nonzero: MAP_OK / MAP_HALF are served with this page list (the guest re-mapped the surface) */
static bool get_desc(void *o, uint32_t id, uint8_t *out, size_t cap, size_t *len) {
    (void)o; g_get_calls++;
    if (g_desc_fail_n > 0) { g_desc_fail_n--; return false; }
    uint8_t d[0x70];
    switch (id) {
    case MAP_OK: case MAP_OK2: mkdesc(d, PFN_LIST << 14, 0x42475241u, SROW, SFIRST); break;
    case MAP_RGHA: mkdesc(d, PFN_LIST << 14, 0x58585858u, SROW, SFIRST); break;                       /* an unknown fourcc: refused */
    case MAP_HALF: mkdesc(d, PFN_LIST << 14, 0x52476841u, HROW, SFIRST); break;                        /* 'RGhA': RGBA16Float, 8 bytes per pixel */
    case MAP_HOLE: mkdesc(d, PFN_LIST2 << 14, 0x42475241u, SROW, SFIRST); break;                       /* second page not present */
    case MAP_BADLIST: mkdesc(d, (uint32_t)(MEM_SIZE + 0x10000u), 0x42475241u, SROW, SFIRST); break;   /* page list outside guest memory */
    case MAP_BADGEOM: mkdesc(d, PFN_LIST << 14, 0x42475241u, 0x20000u, SFIRST); break;                 /* row pitch beyond Henge's bound */
    default: return false;
    }
    if (g_remap_list && (id == MAP_OK || id == MAP_HALF)) { uint32_t l = (uint32_t)(g_remap_list >> 12) | 1u; memcpy(d, &l, 4); }
    if (cap < sizeof d) return false;
    memcpy(out, d, sizeof d); *len = sizeof d;
    return true;
}
static uint8_t *surf_page(uint32_t i) { return g_mem + ((uint64_t)(PFN_SURF0 + i) << 14); }
static void surf_fill(uint8_t v) { memset(surf_page(0), v, 2u << 14); }
static void surf_lists(void) {
    uint32_t *l = (uint32_t *)(g_mem + ((uint64_t)PFN_LIST << 14)), *l2 = (uint32_t *)(g_mem + ((uint64_t)PFN_LIST2 << 14));
    l[0] = ((PFN_SURF0 + 0u) << 2) | 1u; l[1] = ((PFN_SURF0 + 1u) << 2) | 1u; l[2] = ((PFN_SURF0 + 2u) << 2) | 1u; l[3] = ((PFN_SURF0 + 3u) << 2) | 1u;   /* (pfn << 14) >> 12 = pfn << 2, bit 0 present */
    l2[0] = ((PFN_SURF0 + 0u) << 2) | 1u; l2[1] = ((PFN_SURF0 + 1u) << 2);     /* page 1: not present */
}
/* byte offset of pixel (x, y) in the surface's page run */
static uint8_t *half_px(uint32_t x, uint32_t y) { return surf_page(0) + SFIRST + (uint64_t)y * HROW + x * 8u; }
static uint32_t *surf_px(uint32_t x, uint32_t y) { return (uint32_t *)(surf_page(0) + SFIRST + (uint64_t)y * SROW + x * 4u); }

static lagfx_device_t *make_device(void) {
    lagfx_device_descriptor_t d;
    memset(&d, 0, sizeof d);
    d.shell.opaque = (void *)0xfeedu;
    d.shell.create_task = cb_create_task; d.shell.destroy_task = cb_destroy_task; d.shell.map_memory = cb_map; d.shell.unmap_memory = cb_unmap;
    d.shell.read_memory = rd; d.shell.write_memory = wr; d.shell.raise_interrupt = irq; d.shell.get_surface_desc = get_desc;
    d.thread_count = 2;
    char *err = NULL;
    return lagfx_device_new(&d, &err);
}

static void submit(lagfx_protocol_t *p, uint16_t op, uint32_t stamp, const uint32_t *words, uint32_t nwords) {
    uint32_t len = 12u + nwords * 4u;
    uint64_t ring = (uint64_t)RING_PFN0 << 14;
    uint8_t cmd[2048]; memset(cmd, 0, sizeof cmd);
    assert(len <= sizeof cmd);
    memcpy(cmd, &op, 2); memcpy(cmd + 4, &len, 4); memcpy(cmd + 8, &stamp, 4);
    if (nwords) memcpy(cmd + 12, words, nwords * 4u);
    assert(g_wp % 0x10000u + len <= 0x10000u);
    memcpy(g_mem + ring + (g_wp % 0x10000u), cmd, len);
    g_wp += len;
    put32(((uint64_t)SHARED_PFN << 14) + LAGFX_RING_FIFO_DESC_BASE_OFFSET + 0x00, g_wp);
    p->current_chan_id = 1;
    channel_compute_dispatcher_drain(p, 1);
}

static lagfx_protocol_t *mk(void) {
    memset(g_mem, 0, MEM_SIZE); g_wp = 0; g_irq = 0; g_nstamps = 0; atomic_store(&g_last_stamp, 0);
    lagfx_protocol_t *p = calloc(1, sizeof *p);
    p->magic = LAGFX_PROTOCOL_MAGIC; p->dev = g_dev;
    p->ring_page_shift = 14; p->protocol_mode = LAGFX_PROTO_ARM64_VMAPPLE;
    p->ring_shared_page_pfn = SHARED_PFN; p->ring_base_pfn = STAMP_PFN;
    uint64_t d = ((uint64_t)SHARED_PFN << 14) + LAGFX_RING_FIFO_DESC_BASE_OFFSET;
    put32(d + 0x0c, 1); put32(d + 0x10, TABLE_PFN);
    for (uint32_t i = 0; i < 4; i++) put32(((uint64_t)TABLE_PFN << 14) + 4u * i, RING_PFN0 + i);
    put32((uint64_t)ROOT_PFN << 14, TOP_PFN); put32(((uint64_t)ROOT_PFN << 14) + 4, 2);
    put32((uint64_t)TOP_PFN << 14, LEAF_PFN);
    for (uint32_t v = 0; v < NVPN; v++) put32(((uint64_t)LEAF_PFN << 14) + 4u * v, DATA_PFN0 + v);
    return p;
}
static uint8_t *va_ptr(uint64_t va) { assert((va >> 14) < NVPN); return g_mem + ((uint64_t)(DATA_PFN0 + (va >> 14)) << 14) + (va & 0x3fff); }
static void va_write(uint64_t va, const void *src, uint32_t n) {
    const uint8_t *s = src;
    while (n) { uint32_t in = (uint32_t)(va & 0x3fff), c = 0x4000 - in < n ? 0x4000 - in : n; memcpy(va_ptr(va), s, c); va += c; s += c; n -= c; }
}
static lagfx_task_entry_t *def_task(lagfx_protocol_t *p, uint32_t task) {
    lagfx_task_entry_t *t = lagfx_protocol_alloc_task_slot(p);
    assert(t);
    t->id = task; t->live = true; t->root_page_pfn = ROOT_PFN;
    t->lists_set = true; t->objects_addr = 0x100; t->objects_len = 0;
    return t;
}


/* ---- guest layout ---- */
#define VA_PAY    0x4000u
#define VPN_VB    10u
#define VPN_CMD   16u
#define REF_VB   2u
#define REF_PIPE 3u
#define REF_M1   10u    /* type 11 -> mapping MAP_OK */
#define REF_M2   11u    /* type 11 -> the same mapping: shares the image */
#define REF_M3   12u    /* MAP_RGHA */
#define REF_M4   13u    /* MAP_NONE */
#define REF_M5   14u    /* MAP_HOLE */
#define REF_M6   15u    /* MAP_BADLIST */
#define REF_M7   16u    /* MAP_BADGEOM */
#define REF_M9   18u    /* MAP_HALF ('RGhA') */
#define REF_M8   17u    /* MAP_OK2: another mapping id, same size */
#define RED   0xffff0000u
#define GREEN 0xff00ff00u
#define BLUE  0xff0000ffu

static void entry(uint32_t ref, uint32_t type, uint32_t size, uint64_t addr) {
    uint32_t e[3] = { type | size << 8, (uint32_t)addr, (uint32_t)(addr >> 32) };
    va_write(0x100u + ref * 12u, e, 12);
}
/* type 11 payload: {u64 mapping, u32 tag 0x39, u32 size 0x38, u32 ref, v2 description (0x38 bytes: type 2 at +0, format 80 at +1)} */
static void put_mapped(uint32_t ref, uint32_t pay_off, uint64_t mapping, uint32_t tag) {
    uint8_t h[20 + 0x38]; memset(h, 0, sizeof h);
    uint32_t sz = tag == 0x2f ? 48u : 0x38u; memcpy(h, &mapping, 8); memcpy(h + 8, &tag, 4); memcpy(h + 12, &sz, 4); memcpy(h + 16, &ref, 4);
    uint8_t *d = h + 20;
    if (tag == 0x39) { d[0] = 2; d[1] = 80; } else { d[0] = 2; d[2] = 80; }
    va_write(VA_PAY + pay_off, h, sizeof h);
    entry(ref, 11, sizeof h, VA_PAY + pay_off);
}
static void build_objects(void) {
    const float v[8] = { -0.9f, -0.9f, 0.9f, -0.9f, 0.0f, 0.9f, 0.0f, 0.0f };
    va_write((uint64_t)VPN_VB << 14, v, sizeof v);
    { uint8_t h[16] = {0}; uint64_t len = 0x4000; uint32_t vp = VPN_VB; memcpy(h, &len, 8); memcpy(h + 8, &vp, 4); va_write(VA_PAY + 0x200, h, 16); entry(REF_VB, 1, 16, VA_PAY + 0x200); }
    uint8_t pl[16] = {0}; uint32_t ptag = 0xe; memcpy(pl, &ptag, 4);
    va_write(VA_PAY + 0x300, pl, 16);
    entry(REF_PIPE, 7, 16, VA_PAY + 0x300);
    put_mapped(REF_M1, 0x400, MAP_OK, 0x39);
    put_mapped(REF_M2, 0x480, MAP_OK, 0x34);      /* a v1 description (tag != 0x39) over the same mapping */
    put_mapped(REF_M3, 0x500, MAP_RGHA, 0x39);
    put_mapped(REF_M4, 0x580, MAP_NONE, 0x39);
    put_mapped(REF_M5, 0x600, MAP_HOLE, 0x39);
    put_mapped(REF_M6, 0x680, MAP_BADLIST, 0x39);
    put_mapped(REF_M7, 0x700, MAP_BADGEOM, 0x39);
    put_mapped(REF_M8, 0x780, MAP_OK2, 0x39);
    put_mapped(REF_M9, 0x800, MAP_HALF, 0x2f);   /* the real guest's tag 0x2f, desc_size 48 (v1 layout) */
}

static uint8_t g_cb[4096];
static uint32_t g_cbn;
static void cb_reset(uint32_t type) { g_cbn = 8; memset(g_cb, 0, sizeof g_cb); memcpy(g_cb + 4, &type, 4); }
static void cb_cmd(uint32_t tag, const void *payload, uint32_t n) {
    uint32_t sz = 8 + n;
    memcpy(g_cb + g_cbn, &tag, 4); memcpy(g_cb + g_cbn + 4, &sz, 4);
    if (n) memcpy(g_cb + g_cbn + 8, payload, n);
    g_cbn += sz;
}
static uint32_t cb_finish(uint32_t slot) { memcpy(g_cb, &g_cbn, 4); va_write(((uint64_t)(VPN_CMD + slot) << 14), g_cb, g_cbn); return g_cbn; }
static void cb_pass(uint32_t ref, uint16_t load, const double clear[4]) {
    uint8_t r[584]; memset(r, 0, sizeof r);
    uint8_t *c = r + 0x4c;
    memcpy(c, &ref, 4); memcpy(c + 20, &load, 2);
    uint16_t store = 1; memcpy(c + 22, &store, 2);
    memcpy(c + 0x1c, clear, 32);
    cb_cmd(26, r, sizeof r);
}
static void cb_render_common(void) {
    double v[6] = { 0, 0, SW, SH, 0, 1 }; cb_cmd(130, v, sizeof v);
    uint32_t pr = REF_PIPE; cb_cmd(116, &pr, 4);
    uint8_t r[20]; uint32_t idx = 0, cnt = 1, vb = REF_VB; uint64_t off = 0;
    memcpy(r, &idx, 4); memcpy(r + 4, &cnt, 4); memcpy(r + 8, &vb, 4); memcpy(r + 12, &off, 8); cb_cmd(125, r, 20);
}
/* one Render segment: pass on `target` (load action, clear colour), optionally a triangle; res = {ref, flags} pairs besides VB / PIPE */
static void run_exec(lagfx_protocol_t *p, uint32_t stamp, uint32_t target, uint16_t load, const double clear[4], bool draw, const uint32_t (*res)[2], uint32_t nres, bool with_cb) {
    uint32_t len = 0;
    if (with_cb) {
        cb_reset(0); cb_pass(target, load, clear); cb_render_common();
        if (draw) { uint8_t r[8] = { 3, 0, 0, 0, 0, 0, 3, 0 }; cb_cmd(1, r, 8); }
        len = cb_finish(0);
    }
    uint32_t w[3 + 2 * 16 + 4], n = 0;
    w[n++] = 1; w[n++] = nres + 2; w[n++] = with_cb ? 1 : 0;
    w[n++] = REF_VB; w[n++] = 1; w[n++] = REF_PIPE; w[n++] = 0;
    for (uint32_t i = 0; i < nres; i++) { w[n++] = res[i][0]; w[n++] = res[i][1]; }
    if (with_cb) { uint64_t off = (uint64_t)VPN_CMD << 14, l = len; memcpy(w + n, &off, 8); n += 2; memcpy(w + n, &l, 8); n += 2; }
    submit(p, 0x43, stamp, w, n);
}

static VkShaderModule g_vs, g_fs;
static void rel(void *u) { (void)u; }
static bool resolver(void *c, lagfx_protocol_t *p, const lagfx_task_entry_t *t, uint32_t ref, const uint32_t e[3], lagfx_a64_pipeline_src_t *out) {
    (void)c; (void)t; (void)ref; (void)e;
    struct lagfx_vk_state *vk = ((lagfx_device_t *)p->dev)->vk;
    if (!g_vs) {
        VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof k_vs_spv, .pCode = k_vs_spv };
        assert(vkCreateShaderModule(vk->device, &ci, NULL, &g_vs) == VK_SUCCESS);
        ci.codeSize = sizeof k_fs_spv, ci.pCode = k_fs_spv;
        assert(vkCreateShaderModule(vk->device, &ci, NULL, &g_fs) == VK_SUCCESS);
    }
    memset(out, 0, sizeof *out);
    out->desc.vertex_shader = g_vs, out->desc.fragment_shader = g_fs, out->desc.layout = vk->empty_layout;
    out->desc.n_vtx_inputs = 1, out->desc.vtx_in_loc[0] = 0, out->desc.vtx_in_comp[0] = 2, out->desc.vtx_stride = 8;
    out->vertex_buffer_index = 0;
    out->release = rel;
    return true;
}


#define CNT(p) ((p)->diag2.exec.run)

/* independent FNV-1a over whole LE u64 pixels of a guest surface, rows at HROW from SFIRST (the write-back result as the guest sees it) */
static uint64_t guest_hash8(void) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t y = 0; y < SH; y++) for (uint32_t x = 0; x < SW; x++) { uint64_t v; memcpy(&v, half_px(x, y), 8); h = (h ^ v) * 1099511628211ull; }
    return h;
}
static uint64_t guest_hash4(void) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t y = 0; y < SH; y++) for (uint32_t x = 0; x < SW; x++) { uint32_t v = *surf_px(x, y); h = (h ^ v) * 1099511628211ull; }
    return h;
}

/* ================= pure tests (no GPU) ================= */
static void pure_tests(void) {
    enum { W = 7, H = 5 };
    uint16_t a[W * H * 4], b[W * H * 4], c[W * H * 4];
    for (unsigned i = 0; i < W * H * 4; i++) a[i] = (uint16_t)(i % 5 == 0 ? 0x0000 : i % 5 == 1 ? 0x3c00 : i % 5 == 2 ? 0x3800 : i % 5 == 3 ? 0x2000 : 0x3c00);   /* 0, 1, 0.5, 2^-7, 1 */
    memcpy(b, a, sizeof a); memcpy(c, a, sizeof c);
    lagfx_rb_img_t A = { (uint8_t *)a, W * 8, sizeof a }, B = { (uint8_t *)b, W * 8, sizeof b }, C = { (uint8_t *)c, W * 8, sizeof c };
    lagfx_rb_cmp_t r;
    assert(lagfx_rb_compare(&A, &B, &C, W, H, 8, &r) == LAGFX_RB_OK);
    assert(r.ab_px == 0 && r.bc_px == 0 && r.ac_px == 0 && r.ab_x == -1 && r.bc_y == -1 && r.bytes == W * H * 8 && r.hash_a == r.hash_b && r.hash_b == r.hash_c);
    assert(r.half_stats && r.nan_px == 0 && r.inf_px == 0 && r.neg_px == 0 && r.gt1_px == 0);
    { uint64_t h = 1469598103934665603ull; for (unsigned i = 0; i < W * H; i++) { uint64_t v; memcpy(&v, (uint8_t *)a + i * 8, 8); h = (h ^ v) * 1099511628211ull; } assert(h == r.hash_a); }   /* = wbverify's u64 hash */
    /* one bit of B (pixel 3,2) and one different bit of C (pixel 5,4) */
    ((uint8_t *)b)[(2 * W + 3) * 8 + 4] ^= 1; ((uint8_t *)c)[(4 * W + 5) * 8 + 7] ^= 0x80;
    assert(lagfx_rb_compare(&A, &B, &C, W, H, 8, &r) == LAGFX_RB_OK);
    assert(r.ab_px == 1 && r.ab_x == 3 && r.ab_y == 2 && r.bc_px == 2 && r.bc_x == 3 && r.bc_y == 2 && r.ac_px == 1 && r.hash_a != r.hash_b);
    /* classification on A: NaN, +Inf, -Inf, -0.5, -0 (not negative), 2.0, 1.0 (not above one), max finite half, a subnormal */
    memcpy(b, a, sizeof b); memcpy(c, a, sizeof c);
    a[0] = 0x7e00; a[4] = 0x7c00; a[8] = 0xfc00; a[12] = 0xb800; a[16] = 0x8000; a[20] = 0x4000; a[24] = 0x3c00; a[28] = 0x7bff; a[32] = 0x0001; a[33] = 0x7c01;   /* pixel 8 has two NaN-class channels */
    memcpy(b, a, sizeof b); memcpy(c, a, sizeof c);
    assert(lagfx_rb_compare(&A, &B, &C, W, H, 8, &r) == LAGFX_RB_OK);
    assert(r.nan_px == 2 && r.inf_px == 2 && r.neg_px == 1 && r.gt1_px == 2);   /* px0 NaN; px1 +Inf; px2 -Inf; px3 neg; px4 -0 none; px5 2.0 gt1; px6 1.0 none; px7 0x7bff gt1; px8 subnormal + NaN(0x7c01) */
    printf("PASS: compare (A/B/C pixel + first diff + u64 hash = wbverify's) and half classification (nan %llu inf %llu neg %llu gt1 %llu)\n", (unsigned long long)r.nan_px, (unsigned long long)r.inf_px, (unsigned long long)r.neg_px, (unsigned long long)r.gt1_px);
    /* BGRA (bpp 4) */
    uint32_t p4a[W * H], p4b[W * H]; for (unsigned i = 0; i < W * H; i++) p4a[i] = 0xff000000u | i * 977u; memcpy(p4b, p4a, sizeof p4a);
    lagfx_rb_img_t A4 = { (uint8_t *)p4a, W * 4, sizeof p4a }, B4 = { (uint8_t *)p4b, W * 4, sizeof p4b };
    assert(lagfx_rb_compare(&A4, &B4, NULL, W, H, 4, &r) == LAGFX_RB_OK && r.ab_px == 0 && !r.half_stats && r.bc_px == 0 && r.hash_c == 0);
    p4b[17] ^= 0x100; assert(lagfx_rb_compare(&A4, &B4, NULL, W, H, 4, &r) == LAGFX_RB_OK && r.ab_px == 1 && r.ab_x == 3 && r.ab_y == 2);
    /* wrong strides are errors, never reads past the buffer */
    lagfx_rb_img_t big = { (uint8_t *)b, W * 8 + 8, sizeof b };          /* a stride one pixel wider than the data: the last rows would run past `len` */
    assert(lagfx_rb_compare(&A, &big, NULL, W, H, 8, &r) == LAGFX_RB_ERR_STRIDE && r.bad_img == 2u);
    lagfx_rb_img_t narrow = { (uint8_t *)a, W * 8 - 8, sizeof a };      /* a stride smaller than a row of pixels */
    assert(lagfx_rb_compare(&narrow, &B, &C, W, H, 8, &r) == LAGFX_RB_ERR_STRIDE && r.bad_img == 1u);
    lagfx_rb_img_t shortc = { (uint8_t *)c, W * 8, W * 8 * (H - 1) };
    assert(lagfx_rb_compare(&A, &B, &shortc, W, H, 8, &r) == LAGFX_RB_ERR_STRIDE && r.bad_img == 4u);
    assert(lagfx_rb_compare(&A, &B, NULL, W, H, 6, &r) == LAGFX_RB_ERR_ARG && lagfx_rb_compare(NULL, &B, NULL, W, H, 8, &r) == LAGFX_RB_ERR_ARG && lagfx_rb_compare(&A, &B, NULL, 0, H, 8, &r) == LAGFX_RB_ERR_ARG);
    printf("PASS: wrong stride / short buffer / bad bpp are errors (bad_img names the image), never a read past the buffer\n");
    /* gate */
    { lagfx_rb_gate_t g; memset(&g, 0, sizeof g); unsigned took = 0; uint64_t t = 1000;
      for (unsigned i = 1; i <= 100; i++) { bool tk = lagfx_rb_gate_admit(&g, t, 16); took += tk; if (i <= 4) assert(tk); else if (i % 16 == 0) assert(tk); else assert(!tk); }
      assert(took == 4 + 100 / 16);   /* 1..4, then 16, 32, 48, 64, 80, 96 */
      assert(!lagfx_rb_gate_admit(&g, t + 29000000000ull, 16) && lagfx_rb_gate_admit(&g, t + 31000000000ull, 16));   /* the 30 s gap rule: 101 is not a multiple of 16 but 31 s after the last sample */
      lagfx_rb_gate_t g2; memset(&g2, 0, sizeof g2); for (unsigned i = 0; i < 4; i++) assert(lagfx_rb_gate_admit(&g2, 0, 0)); assert(!lagfx_rb_gate_admit(&g2, 1, 0)); }
    printf("PASS: sampling gate: first 4, every 16th, and a candidate 30 s after the last sample\n");
    assert(lagfx_rb_format_bpp(97) == 8 && lagfx_rb_format_bpp(44) == 4 && lagfx_rb_format_bpp(50) == 4 && lagfx_rb_format_bpp(37) == 4 && lagfx_rb_format_bpp(43) == 4 && lagfx_rb_format_bpp(0) == 0 && lagfx_rb_format_bpp(109) == 0);
    /* line format: carries every field the phone log needs */
    { lagfx_rb_verdict_t v; memset(&v, 0, sizeof v); lagfx_rb_totals_t t; memset(&t, 0, sizeof t);
      v.kind = LAGFX_RB_FAIL; v.skip = "A!=B"; v.swap = 151; v.swap_fin = 151; v.mapping = 2; v.gen = 9; v.w = 1920; v.h = 1080; v.fourcc = 0x52476841u; v.bpp = 8; v.pitch = 15360;
      v.cmp.bytes = 16588800ull; v.cmp.ab_px = 3; v.cmp.ab_x = 5; v.cmp.ab_y = 6; v.cmp.bc_x = v.cmp.bc_y = -1; v.cmp.half_stats = true; v.cmp.nan_px = 7; t.fail = 1; t.sampled = 1;
      char line[3072]; size_t n = lagfx_rb_format_line(line, sizeof line, &v, &t);
      assert(n > 100 && strstr(line, "g7-gpu-readback: verdict=FAIL reason=A!=B swap=151 swap_fin=151 ms=2 gen=9 1920x1080 fourcc=52476841 bpp=8 pitch=15360 bytes=16588800 A_vs_B_mismatch_px=3"));
      assert(strstr(line, "nan_px=7") && strstr(line, "first_AB=5,6") && strstr(line, "hash_A=") && strstr(line, "totals[candidates=0 sampled=1 pass=0 fail=1"));
      char tiny[40]; assert(lagfx_rb_format_line(tiny, sizeof tiny, &v, &t) == sizeof tiny - 1 && tiny[sizeof tiny - 1] == 0); }
    printf("PASS: g7-gpu-readback line format (swap, mapping, generation, size, fourcc, bytes, mismatches, hashes, nan/inf/neg/>1, totals)\n");
    /* activation: LAGFX_WB_VERIFY alone does NOT enable the independent readback any more; LAGFX_GPU_READBACK alone does; both = full mode */
    unsetenv("LAGFX_WB_VERIFY"); unsetenv("LAGFX_GPU_READBACK");
    assert(!lagfx_rb_enabled() && !lagfx_rb_full_mode());
    setenv("LAGFX_WB_VERIFY", "1", 1); assert(!lagfx_rb_enabled() && !lagfx_rb_full_mode());
    unsetenv("LAGFX_WB_VERIFY");
    setenv("LAGFX_GPU_READBACK", "1", 1); assert(lagfx_rb_enabled() && !lagfx_rb_full_mode());
    setenv("LAGFX_WB_VERIFY", "1", 1); assert(lagfx_rb_enabled() && lagfx_rb_full_mode());
    setenv("LAGFX_WB_VERIFY", "0", 1); assert(lagfx_rb_enabled() && !lagfx_rb_full_mode()); setenv("LAGFX_WB_VERIFY", "11", 1); assert(!lagfx_rb_full_mode());
    setenv("LAGFX_GPU_READBACK", "0", 1); assert(!lagfx_rb_enabled()); setenv("LAGFX_GPU_READBACK", "11", 1); assert(!lagfx_rb_enabled());
    unsetenv("LAGFX_WB_VERIFY"); unsetenv("LAGFX_GPU_READBACK");
    printf("PASS: flag matrix (pure): neither off; WB_VERIFY alone does NOT enable the readback; GPU_READBACK alone does (AB); both = full (ABC); '0' / '11' off\n");
    /* identity checks */
    { lagfx_rb_ident_t rec = { .swap = 10, .gen = 5, .ident = 0x1234, .img = 0xabc, .fence_seq = 3, .mapping = 2, .w = 1920, .h = 1080, .bpp = 8 }, now = rec;
      now.fence_seq = 4; now.swap = 11;
      assert(lagfx_rb_identity_check(&rec, &now) == NULL);
      now.swap = 10; assert(lagfx_rb_identity_check(&rec, &now) == NULL);   /* same swap */
      lagfx_rb_ident_t t;
      t = now; t.swap = 9; assert(!strcmp(lagfx_rb_identity_check(&rec, &t), "swap_went_back"));
      t = now; t.gen = 6; assert(!strcmp(lagfx_rb_identity_check(&rec, &t), "gen_mismatch"));
      t = now; t.mapping = 3; assert(!strcmp(lagfx_rb_identity_check(&rec, &t), "mapping_mismatch"));
      t = now; t.ident = 0x1235; assert(!strcmp(lagfx_rb_identity_check(&rec, &t), "mapping_generation_changed"));
      t = now; t.img = 0xabd; assert(!strcmp(lagfx_rb_identity_check(&rec, &t), "image_replaced"));
      t = now; t.w = 1280; assert(!strcmp(lagfx_rb_identity_check(&rec, &t), "desc_changed"));
      t = now; t.bpp = 4; assert(!strcmp(lagfx_rb_identity_check(&rec, &t), "desc_changed"));
      t = now; t.mip = 1; assert(!strcmp(lagfx_rb_identity_check(&rec, &t), "subresource"));
      t = now; t.layer = 1; assert(!strcmp(lagfx_rb_identity_check(&rec, &t), "subresource"));
      { lagfx_rb_ident_t r2 = rec; r2.mip = 1; assert(!strcmp(lagfx_rb_identity_check(&r2, &now), "subresource")); }
      t = now; t.fence_seq = 3; assert(!strcmp(lagfx_rb_identity_check(&rec, &t), "fence_not_signalled"));
      assert(!strcmp(lagfx_rb_identity_check(NULL, &now), "no_inputs")); }
    printf("PASS: identity check: same swap (never backwards) / mapping id / mapping generation / write-back generation / image handle / size / subresource mip0 layer0 / fence signalled\n");
    /* preconditions */
    assert(!lagfx_rb_precheck(VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 97, 8, 1920, 1080, 1));
    assert(!strcmp(lagfx_rb_precheck(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 97, 8, 64, 64, 1), "no_transfer_src"));
    assert(!strcmp(lagfx_rb_precheck(VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 109, 8, 64, 64, 1), "unknown_format"));
    assert(!strcmp(lagfx_rb_precheck(VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 44, 8, 64, 64, 1), "bpp_format_mismatch"));
    assert(!strcmp(lagfx_rb_precheck(VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 97, 8, 0, 64, 1), "bad_extent"));
    assert(!strcmp(lagfx_rb_precheck(VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 97, 8, 64, 64, VK_IMAGE_LAYOUT_UNDEFINED), "layout_not_general"));
    assert(!strcmp(lagfx_rb_precheck(VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 97, 8, 64, 64, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL), "layout_not_general"));
    printf("PASS: preconditions: TRANSFER_SRC, known format, bpp, extent, layout GENERAL (image layout is restored by construction: no transition recorded)\n");
    /* wait accounting: counted only while a recorded copy waits for its fence; off = no-op */
    lagfx_rb_test_reset(); unsetenv("LAGFX_GPU_READBACK");
    lagfx_rb_note_wait(5000000, true); assert(lagfx_rb_totals()->waits == 0 && lagfx_rb_totals()->wait_ns == 0 && lagfx_rb_totals()->fence_seq == 0);
    setenv("LAGFX_GPU_READBACK", "1", 1);
    lagfx_rb_note_wait(5000000, true); assert(lagfx_rb_totals()->waits == 0);   /* nothing recorded: not a readback wait */
    unsetenv("LAGFX_GPU_READBACK");
    printf("PASS: wait accounting is a no-op while the readback is off or nothing was recorded\n");
    /* line format carries the sync state and the wait total */
    { lagfx_rb_verdict_t v; memset(&v, 0, sizeof v); lagfx_rb_totals_t t; memset(&t, 0, sizeof t); char line[2048];
      v.kind = LAGFX_RB_PASS; v.w = 4; v.h = 4; v.bpp = 8; v.pitch = 32; v.cmp.ab_x = v.cmp.ab_y = v.cmp.bc_x = v.cmp.bc_y = -1; t.waits = 3; t.wait_ns = 12345000; t.host_barriers = 3;
      lagfx_rb_format_line(line, sizeof line, &v, &t);
      assert(strstr(line, "readback_sync=on wbv=off mode=AB subres=mip0/layer0 layout=GENERAL->GENERAL") && strstr(line, "waits=3 wait_ms_total=12.345") && strstr(line, "host_barriers=3") && strstr(line, "may hide frame-freshness races"));
      v.full = true; lagfx_rb_format_line(line, sizeof line, &v, &t); assert(strstr(line, "wbv=on mode=ABC")); }
    printf("PASS: line records readback_sync=on, wbv=on|off, mode=AB|ABC, subresource, layout, host barriers and total fence wait ms\n");
}

/* ================= integration (lavapipe + validation) ================= */
static const lagfx_rb_verdict_t *V(void) { return lagfx_rb_last_verdict(); }
static void show(const char *tag) {   /* the same line the library logs, so the test output documents what the phone log will look like */
    char line[3072]; lagfx_rb_format_line(line, sizeof line, V(), lagfx_rb_totals());
    printf("LINE[%s] %s\n", tag, line);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    pure_tests();
    g_mem = calloc(1, MEM_SIZE);
    setenv("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT", 0);
    g_dev = make_device();
    assert(g_dev);
    if (!g_dev->vk || !g_dev->vk->initialized) { puts("SKIP: no Vulkan device (pure tests passed)"); lagfx_device_free(g_dev); free(g_mem); return 77; }
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_protocol_t *p = mk();
    def_task(p, 1);
    build_objects(); surf_lists();
    lagfx_a64_exec_set_pipeline_resolver(p, resolver, NULL);
    static const double pat[4] = { 0.5, 0.0078125, 0, 1 };   /* 0.5, 2^-7, 0, 1: all exactly representable in binary16: 0x3800 0x2000 0x0000 0x3c00; the triangle adds (1,0,0,1) */
    static const uint8_t bg8[8] = { 0x00, 0x38, 0x00, 0x20, 0x00, 0x00, 0x00, 0x3c }, tri8[8] = { 0x00, 0x3c, 0, 0, 0, 0, 0x00, 0x3c };
    const uint32_t res[1][2] = { { REF_M9, 2 | 4 } };

    /* (0) OFF (no env var): the exec writes back as before and the readback records / allocates / logs nothing */
    unsetenv("LAGFX_WB_VERIFY"); unsetenv("LAGFX_GPU_READBACK"); lagfx_rb_test_reset();
    memset(surf_page(0), SENT, 4u << 14);
    run_exec(p, 1, REF_M9, 2, pat, true, res, 1, true);
    assert(cell(1) == 1 && CNT(p).mapped_written_back == 1 && !V()->valid && lagfx_rb_totals()->candidates == 0 && lagfx_rb_totals()->sampled == 0);
    assert(!memcmp(half_px(0, 0), bg8, 8) && !memcmp(half_px(63, 79), bg8, 8) && !memcmp(half_px(32, 40), tri8, 8));
    uint64_t off_hash = guest_hash8(), off_down = CNT(p).mapped_bytes_down;
    uint8_t off_pages[4u << 14]; memcpy(off_pages, surf_page(0), sizeof off_pages);
    assert(lagfx_a64_res_lookup(p, 1, REF_M9)->ms != NULL);
    assert(lagfx_a64_test_ms_rb_allocated(p, MAP_HALF) == 0 && lagfx_rb_totals()->waits == 0 && lagfx_rb_totals()->host_barriers == 0 && lagfx_rb_totals()->fence_seq == 0);
    printf("PASS: readback OFF by default: write-back identical, no candidate / sample / line / buffer / wait accounting (guest hash %016llx)\n", (unsigned long long)off_hash);

    /* (0b) flag matrix row "WB only": LAGFX_WB_VERIFY=1 runs the existing staging -> guest verify and must NOT run the independent readback */
    setenv("LAGFX_WB_VERIFY", "1", 1); unsetenv("LAGFX_GPU_READBACK"); lagfx_rb_test_reset();
    memset(surf_page(0), SENT, 4u << 14);
    { uint64_t wbx = CNT(p).mapped_written_back;
      run_exec(p, 2, REF_M9, 2, pat, true, res, 1, true);
      assert(CNT(p).mapped_written_back == wbx + 1 && !memcmp(off_pages, surf_page(0), sizeof off_pages));
      assert(!V()->valid && lagfx_rb_totals()->candidates == 0 && lagfx_rb_totals()->sampled == 0 && lagfx_rb_totals()->host_barriers == 0 && lagfx_rb_totals()->waits == 0 && lagfx_rb_totals()->wait_ns == 0);
      assert(lagfx_a64_test_ms_rb_allocated(p, MAP_HALF) == 0);   /* no GPU readback buffer allocated, no GPU copy recorded */
      /* even many more execs: still nothing */
      for (unsigned i = 0; i < 6; i++) run_exec(p, 3 + i, REF_M9, 2, pat, true, res, 1, true);
      assert(lagfx_rb_totals()->candidates == 0 && lagfx_a64_test_ms_rb_allocated(p, MAP_HALF) == 0 && !V()->valid); }
    printf("PASS: flag matrix row WB only: write-back identical, NO readback candidate / GPU copy / buffer / barrier / wait accounting\n");
    unsetenv("LAGFX_WB_VERIFY");

    /* (0c) row "READBACK only": mode AB (A vs B), guest pages are not re-read (no C), the write-back itself is bit-identical */
    setenv("LAGFX_GPU_READBACK", "1", 1); lagfx_rb_test_reset();
    memset(surf_page(0), SENT, 4u << 14);
    p->diag2.disp.op_swap = 150;
    { uint64_t r0 = g_rd_calls;
      run_exec(p, 20, REF_M9, 2, pat, true, res, 1, true);
      uint64_t rd_ab = g_rd_calls - r0;
      assert(!memcmp(off_pages, surf_page(0), sizeof off_pages));
      const lagfx_rb_verdict_t *v = V();
      assert(v->valid && v->kind == LAGFX_RB_PASS && !v->full && v->cmp.ab_px == 0 && v->cmp.bc_px == 0 && v->cmp.hash_c == 0 && v->pages_skipped == 0 && v->cmp.hash_a == v->cmp.hash_b && v->cmp.hash_a == off_hash);
      assert(lagfx_a64_test_ms_rb_allocated(p, MAP_HALF) == 1 && lagfx_rb_totals()->host_barriers == 1 && lagfx_rb_totals()->waits >= 1 && lagfx_rb_totals()->wait_ns > 0 && lagfx_rb_totals()->fence_seq >= 1);
      char line[3072]; lagfx_rb_format_line(line, sizeof line, v, lagfx_rb_totals());
      assert(strstr(line, "readback_sync=on wbv=off mode=AB") && strstr(line, "verdict=PASS"));
      show("AB");
      /* the same exec in mode ABC reads the guest pages once more (C): strictly more guest reads than AB */
      setenv("LAGFX_WB_VERIFY", "1", 1); lagfx_rb_test_reset(); r0 = g_rd_calls;
      run_exec(p, 21, REF_M9, 2, pat, true, res, 1, true);
      uint64_t rd_abc = g_rd_calls - r0;
      assert(V()->kind == LAGFX_RB_PASS && V()->full && V()->cmp.hash_c == off_hash && rd_abc > rd_ab);
      unsetenv("LAGFX_WB_VERIFY"); }
    printf("PASS: flag matrix row READBACK only: mode=AB (A vs B, no guest re-read, hash %016llx), both flags: mode=ABC with C\n", (unsigned long long)off_hash);

    /* (1) ON: the same exec -> a PASS verdict for the same swap / mapping; bit-identical write-back to the OFF run */
    setenv("LAGFX_GPU_READBACK", "1", 1); setenv("LAGFX_WB_VERIFY", "1", 1); lagfx_rb_test_reset();   /* both flags: the full A/B/C comparison */
    memset(surf_page(0), SENT, 4u << 14);
    p->diag2.disp.op_swap = 151;   /* the swap number the verdict must name (the display counter before the exec's own swap) */
    run_exec(p, 2, REF_M9, 2, pat, true, res, 1, true);
    assert(!memcmp(off_pages, surf_page(0), sizeof off_pages) && CNT(p).mapped_bytes_down >= 2 * off_down);
    assert(!memcmp(off_pages, surf_page(0), sizeof off_pages));   /* readback on: guest pages byte-identical to readback off, padding / sentinel included */
    assert(lagfx_rb_totals()->candidates == 1 && lagfx_rb_totals()->sampled == 1 && lagfx_rb_totals()->pass == 1 && lagfx_rb_totals()->fail == 0 && lagfx_rb_totals()->skip == 0);
    { const lagfx_rb_verdict_t *v = V(); const lagfx_rb_cmp_t *c = &v->cmp;
      assert(v->valid && v->kind == LAGFX_RB_PASS && !v->skip && v->mapping == MAP_HALF && v->w == SW && v->h == SH && v->bpp == 8 && v->fourcc == 0x52476841u && v->pitch == SW * 8u);
      assert(v->swap == 151 && v->swap == v->swap_fin && v->gen != 0 && v->pages_skipped == 0 && v->c_stable);
      assert(c->bytes == SW * SH * 8u && c->ab_px == 0 && c->bc_px == 0 && c->ac_px == 0 && c->ab_x == -1 && c->bc_x == -1);
      assert(v->full && c->hash_a == c->hash_b && c->hash_b == c->hash_c && c->hash_a == guest_hash8() && c->hash_a == off_hash);   /* A (GPU image copy) == what the guest pages hold == the OFF run */
      assert(c->half_stats && c->nan_px == 0 && c->inf_px == 0 && c->neg_px == 0 && c->gt1_px == 0); }
    printf("PASS: readback ON: independent copy A == staging B == fresh guest read C for the same swap/mapping/gen (hash %016llx, %u bytes), exact halves 0/1/0.5/2^-7, no NaN/Inf/neg/>1\n", (unsigned long long)V()->cmp.hash_a, SW * SH * 8u);
    show("on-pass");
    uint64_t gen1 = V()->gen;

    /* (2) a second exec -> a new generation, still PASS (the gate takes candidates 1..4) */
    { static const double pat2[4] = { 0.25, 0, 1, 1 };
      run_exec(p, 3, REF_M9, 2, pat2, true, res, 1, true);
      assert(lagfx_rb_totals()->sampled == 2 && V()->kind == LAGFX_RB_PASS && V()->gen > gen1 && V()->cmp.ab_px == 0 && V()->cmp.hash_a == guest_hash8());
      assert(V()->cmp.hash_a != off_hash); }
    printf("PASS: next exec = next generation, PASS again, hash follows the new content\n");

    /* (3) negative controls: every one must FAIL and be counted, none may pass */
    static const struct { int hook; const char *why; uint64_t ab, bc, ac; } nc[] = {
        { 1, "A!=B", 1, 1, 0 },   /* corrupt one bit of staging B: A == C but B differs from both */
        { 2, "A!=B", 1, 0, 1 },   /* corrupt one bit of the GPU copy A */
        { 4, "B!=C", 0, 1, 1 },   /* corrupt one bit of the guest read C */
        { 3, "stride", 0, 0, 0 }, /* B compared with a stride of width*bpp + 8 */
    };
    for (unsigned i = 0; i < 4; i++) {
        lagfx_rb_test_reset(); lagfx_rb_test_hook = nc[i].hook;
        run_exec(p, 10 + i, REF_M9, 2, pat, true, res, 1, true);
        const lagfx_rb_verdict_t *v = V();
        assert(v->valid && v->kind == LAGFX_RB_FAIL && v->skip && !strcmp(v->skip, nc[i].why));
        assert(lagfx_rb_totals()->fail == 1 && lagfx_rb_totals()->pass == 0 && lagfx_rb_totals()->sampled == 1);
        if (nc[i].hook != 3) assert(v->cmp.ab_px == nc[i].ab && v->cmp.bc_px == nc[i].bc && v->cmp.ac_px == nc[i].ac);
        else assert(v->cmp.bad_img == 2u);
        if (nc[i].hook == 4) assert(!v->c_stable);   /* the fresh re-read of the guest differs from the corrupted first read */
        assert(!memcmp(half_px(32, 40), tri8, 8) && !memcmp(half_px(0, 0), bg8, 8));   /* the control acted on copies: the guest pages are untouched */
        show("negative");
        printf("PASS: negative control hook %d detected: FAIL reason=%s ab=%llu bc=%llu ac=%llu\n", nc[i].hook, v->skip, (unsigned long long)v->cmp.ab_px, (unsigned long long)v->cmp.bc_px, (unsigned long long)v->cmp.ac_px);
    }
    /* the guest re-mapped the surface (page list changed): the existing code drops the write-back, so there is no finish -> no verdict, and the next candidate counts it as abandoned */
    lagfx_rb_test_reset();
    { uint32_t *l = (uint32_t *)(g_mem + ((uint64_t)PFN_LIST << 14)); uint32_t save = l[1]; l[1] &= ~1u;
      uint64_t remapped = CNT(p).mapped_remapped;
      run_exec(p, 19, REF_M9, 2, pat, true, res, 1, true);
      assert(CNT(p).mapped_remapped == remapped + 1 && lagfx_rb_totals()->candidates == 1 && lagfx_rb_totals()->sampled == 0 && !V()->valid);
      l[1] = save;
      run_exec(p, 20, REF_M9, 2, pat, true, res, 1, true);
      assert(lagfx_rb_totals()->candidates == 2 && lagfx_rb_totals()->abandoned == 1 && lagfx_rb_totals()->sampled == 1 && V()->kind == LAGFX_RB_PASS); }
    printf("PASS: a dropped write-back (guest re-mapped the surface) yields no verdict and is counted as abandoned by the next candidate\n");
    /* a guest page that cannot be read back: the transfer skips it, the verdict is SKIP (counted, never a pass) */
    lagfx_rb_test_reset();
    g_rd_fail_lo = (uint64_t)(PFN_SURF0 + 1u) << 14, g_rd_fail_hi = (uint64_t)(PFN_SURF0 + 2u) << 14;
    run_exec(p, 21, REF_M9, 2, pat, true, res, 1, true);
    g_rd_fail_lo = g_rd_fail_hi = 0;
    show("pages-missing");
    assert(V()->valid && V()->kind == LAGFX_RB_SKIP && V()->skip && !strcmp(V()->skip, "guest_pages_skipped") && V()->pages_skipped > 0 && lagfx_rb_totals()->pass == 0 && lagfx_rb_totals()->skip == 1);
    printf("PASS: unreadable guest page -> SKIP guest_pages_skipped (counted, not a pass)\n");

    /* (4) non-coherent path exercised (invalidate), BGRA class has its own gate and passes */
    lagfx_rb_test_reset(); lagfx_rb_test_hook = 5;
    run_exec(p, 30, REF_M9, 2, pat, true, res, 1, true);
    assert(V()->kind == LAGFX_RB_PASS && lagfx_rb_totals()->invalidates == 1);
    lagfx_rb_test_hook = 0;
    { static const double green[4] = { 0, 1, 0, 1 };
      const uint32_t r1[1][2] = { { REF_M1, 2 | 4 } };
      surf_fill(SENT);
      run_exec(p, 31, REF_M1, 2, green, true, r1, 1, true);
      const lagfx_rb_verdict_t *v = V();
      assert(v->kind == LAGFX_RB_PASS && v->bpp == 4 && v->mapping == MAP_OK && v->cmp.hash_a == guest_hash4() && !v->cmp.half_stats && v->cmp.bytes == SW * SH * 4u && lagfx_rb_totals()->sampled == 2 && lagfx_rb_totals()->pass == 2); }
    printf("PASS: invalidate path (non-coherent) and the BGRA 64x80 surface class\n");

    /* (5) skipped candidates are reported, never silently passed */
    lagfx_rb_test_reset();
    { lagfx_rb_slot_t *sl = NULL; VkCommandBuffer fake = (VkCommandBuffer)(uintptr_t)8;
      lagfx_rb_record(g_dev->vk, &sl, fake, (VkImage)(uintptr_t)8, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_LAYOUT_GENERAL, 64, 80, 8, 5, 0, 1, 1);
      assert(V()->kind == LAGFX_RB_SKIP && !strcmp(V()->skip, "no_transfer_src"));
      lagfx_rb_record(g_dev->vk, &sl, fake, (VkImage)(uintptr_t)8, (VkFormat)109, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_LAYOUT_GENERAL, 64, 80, 8, 5, 0, 1, 1);
      assert(V()->kind == LAGFX_RB_SKIP && !strcmp(V()->skip, "unknown_format"));
      lagfx_rb_record(g_dev->vk, &sl, fake, (VkImage)(uintptr_t)8, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_LAYOUT_GENERAL, 64, 80, 8, 5, 0, 1, 1);
      assert(V()->kind == LAGFX_RB_SKIP && !strcmp(V()->skip, "bpp_format_mismatch") && lagfx_rb_totals()->skip == 3 && lagfx_rb_totals()->pass == 0 && sl == NULL);
      lagfx_rb_record(g_dev->vk, &sl, fake, (VkImage)(uintptr_t)8, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_LAYOUT_UNDEFINED, 64, 80, 8, 5, 0, 1, 1);
      assert(V()->kind == LAGFX_RB_SKIP && !strcmp(V()->skip, "layout_not_general") && lagfx_rb_totals()->skip == 4 && sl == NULL && lagfx_rb_totals()->host_barriers == 0); }
    printf("PASS: unusable candidates (no TRANSFER_SRC / unknown format / bpp mismatch) are logged as SKIP and counted\n");

    /* (6) rate limit: many candidates, few samples */
    lagfx_rb_test_reset(); setenv("LAGFX_GPU_READBACK_EVERY", "8", 1);
    for (unsigned i = 0; i < 20; i++) run_exec(p, 40 + i, REF_M9, 2, pat, true, res, 1, true);
    assert(lagfx_rb_totals()->candidates == 20 && lagfx_rb_totals()->sampled == 4 + 2 && lagfx_rb_totals()->not_sampled == 14 && lagfx_rb_totals()->pass == 6 && lagfx_rb_totals()->fail == 0);   /* 1..4, 8->no (seen 8 is a multiple: taken), 16 */
    unsetenv("LAGFX_GPU_READBACK_EVERY");
    printf("PASS: rate limit: 20 candidates -> %llu samples (first 4, then every 8th), %llu not sampled\n", (unsigned long long)lagfx_rb_totals()->sampled, (unsigned long long)lagfx_rb_totals()->not_sampled);

    /* (7) failure injection leaves state unchanged: the write-back (guest bytes, counters, pending flag) is identical to a run without the readback */
    for (int hook = 6; hook <= 7; hook++) {
        unsetenv("LAGFX_GPU_READBACK"); unsetenv("LAGFX_WB_VERIFY");
        static const double pat3[4] = { 0.125, 0.25, 0.5, 1 };
        memset(surf_page(0), SENT, 4u << 14);
        uint64_t wb0 = CNT(p).mapped_written_back;
        run_exec(p, 70, REF_M9, 2, pat3, true, res, 1, true);
        uint8_t base[4u << 14]; memcpy(base, surf_page(0), sizeof base);
        uint64_t wb_base = CNT(p).mapped_written_back - wb0;
        setenv("LAGFX_GPU_READBACK", "1", 1); lagfx_rb_test_reset(); lagfx_rb_test_hook = hook;
        memset(surf_page(0), SENT, 4u << 14);
        wb0 = CNT(p).mapped_written_back;
        run_exec(p, 71, REF_M9, 2, pat3, true, res, 1, true);
        lagfx_rb_test_hook = 0;
        assert(V()->valid && V()->kind == LAGFX_RB_SKIP && V()->skip && !strcmp(V()->skip, hook == 6 ? "alloc_failed" : "invalidate_failed") && lagfx_rb_totals()->pass == 0 && lagfx_rb_totals()->skip == 1);
        assert(!memcmp(base, surf_page(0), sizeof base) && CNT(p).mapped_written_back - wb0 == wb_base && lagfx_a64_test_ms_down_pending(p, MAP_HALF) == 0);
        show(hook == 6 ? "alloc-failed" : "invalidate-failed");
        unsetenv("LAGFX_GPU_READBACK");
    }
    printf("PASS: failure (buffer allocation / invalidate) -> SKIP with reason, write-back bytes + counters identical to the readback-off run, nothing pending afterwards\n");

    /* (8) lifetime and fence, driven directly with a real image: a slot freed while its sample is pending; a finish before any signalled wait; a copy that never ran */
    {   struct lagfx_vk_state *vk = g_dev->vk;
        VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = { 8, 8, 1 }, .mipLevels = 1, .arrayLayers = 1,
                                  .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VkImage img = VK_NULL_HANDLE; VkDeviceMemory imem = VK_NULL_HANDLE;
        assert(vkCreateImage(vk->device, &ici, NULL, &img) == VK_SUCCESS);
        VkMemoryRequirements rq; vkGetImageMemoryRequirements(vk->device, img, &rq);
        VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(vk->phys_device, &mp);
        uint32_t mt = UINT32_MAX; for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((rq.memoryTypeBits & (1u << i))) { mt = i; break; }
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = mt };
        assert(mt != UINT32_MAX && vkAllocateMemory(vk->device, &mai, NULL, &imem) == VK_SUCCESS && vkBindImageMemory(vk->device, img, imem, 0) == VK_SUCCESS);
        setenv("LAGFX_GPU_READBACK", "1", 1); unsetenv("LAGFX_WB_VERIFY"); lagfx_rb_test_reset();
        VkCommandBuffer cbs[8]; unsigned ncb = 0;   /* a fresh, never submitted recording per sample: one buffer per cb (a slot freed while recorded invalidates only that cb) */
#define NEWCB() ({ VkCommandBuffer c_ = VK_NULL_HANDLE; assert(lagfx_vk_cmdbuf_alloc(vk, &c_) == LAGFX_OK); \
                   VkCommandBufferBeginInfo bi_ = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT }; \
                   assert(vkBeginCommandBuffer(c_, &bi_) == VK_SUCCESS); assert(ncb < 8); cbs[ncb++] = c_; c_; })
        lagfx_rb_slot_t *sl = NULL;
        /* (8a) surface destroyed while the sample is pending: slot freed, counted, finish can never run on it, no crash / validation message */
        lagfx_rb_record(vk, &sl, NEWCB(), img, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_LAYOUT_GENERAL, 8, 8, 8, 4, 77, 9, 100);
        assert(sl && lagfx_rb_slot_allocated(sl) && lagfx_rb_totals()->host_barriers == 1 && lagfx_rb_totals()->candidates == 1);
        lagfx_rb_slot_free(vk, &sl);
        assert(sl == NULL && lagfx_rb_totals()->destroyed_pending == 1);
        lagfx_rb_finish(vk, &sl, NULL, NULL, NULL, img, 4, 77, 9, 100);   /* a finish on a freed slot is a no-op */
        assert(lagfx_rb_totals()->sampled == 0);
        printf("PASS: surface destroyed while a sample is pending: slot freed (destroyed_pending=1), finish is a no-op, no verdict\n");
        /* (8b) a finish before any signalled fence wait is refused; the host never reads what no fence made visible */
        lagfx_rb_test_reset();
        lagfx_rb_record(vk, &sl, NEWCB(), img, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_LAYOUT_GENERAL, 8, 8, 8, 4, 77, 9, 100);
        uint8_t stage[8 * 8 * 8]; memset(stage, 0, sizeof stage);
        lagfx_guest_surface_t gs; memset(&gs, 0, sizeof gs); gs.width = 8, gs.height = 8, gs.bpp = 8, gs.fourcc = 0x52476841u;
        lagfx_shell_callbacks_t sh; memset(&sh, 0, sizeof sh);
        lagfx_rb_finish(vk, &sl, &sh, &gs, stage, img, 4, 77, 9, 100);
        assert(V()->valid && V()->kind == LAGFX_RB_SKIP && !strcmp(V()->skip, "fence_not_signalled") && lagfx_rb_totals()->pass == 0);
        /* (8c) identity changes between record and finish are SKIPs */
        lagfx_rb_record(vk, &sl, NEWCB(), img, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_LAYOUT_GENERAL, 8, 8, 8, 4, 77, 10, 100);
        lagfx_rb_note_wait(1000, true);
        lagfx_rb_finish(vk, &sl, &sh, &gs, stage, img, 4, 78, 10, 100);
        assert(V()->kind == LAGFX_RB_SKIP && !strcmp(V()->skip, "mapping_generation_changed"));
        lagfx_rb_record(vk, &sl, NEWCB(), img, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_LAYOUT_GENERAL, 8, 8, 8, 4, 77, 11, 100);
        lagfx_rb_note_wait(1000, true);
        lagfx_rb_finish(vk, &sl, &sh, &gs, stage, (VkImage)(uintptr_t)0x1234, 4, 77, 11, 100);
        assert(V()->kind == LAGFX_RB_SKIP && !strcmp(V()->skip, "image_replaced"));
        lagfx_rb_record(vk, &sl, NEWCB(), img, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_LAYOUT_GENERAL, 8, 8, 8, 4, 77, 12, 100);
        lagfx_rb_note_wait(1000, true);
        lagfx_rb_finish(vk, &sl, &sh, &gs, stage, img, 5, 77, 12, 100);
        assert(V()->kind == LAGFX_RB_SKIP && !strcmp(V()->skip, "mapping_mismatch") && lagfx_rb_totals()->pass == 0 && lagfx_rb_totals()->fail == 0);
        printf("PASS: finish refuses (SKIP, never pass): no signalled fence / mapping generation changed / image replaced / mapping id changed\n");
        lagfx_rb_test_reset();   /* fresh sampling gate */
        /* (8d) a copy that never executed (poison still in the buffer) can never match real staging bytes -> FAIL A!=B */
        lagfx_rb_record(vk, &sl, NEWCB(), img, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_LAYOUT_GENERAL, 8, 8, 8, 4, 77, 13, 100);
        lagfx_rb_note_wait(1000, true);
        lagfx_rb_finish(vk, &sl, &sh, &gs, stage, img, 4, 77, 13, 100);
        assert(V()->kind == LAGFX_RB_FAIL && !strcmp(V()->skip, "A!=B") && V()->cmp.ab_px == 64 && lagfx_rb_totals()->pass == 0);
        printf("PASS: a copy that never ran (poison) is a FAIL, not a pass\n");
        lagfx_rb_slot_free(vk, &sl);
        for (unsigned i = 0; i < ncb; i++) lagfx_vk_cmdbuf_free(vk, cbs[i]);   /* never submitted: only the recording is exercised here */
        vkDestroyImage(vk->device, img, NULL); vkFreeMemory(vk->device, imem, NULL);
        unsetenv("LAGFX_GPU_READBACK"); lagfx_rb_test_reset();
    }

    printf("INFO: validation messenger %s, messages %llu\n", g_dev->vk->debug_messenger ? "ACTIVE" : "inactive", (unsigned long long)lagfx_vk_validation_msg_count);
    assert(lagfx_vk_validation_msg_count == 0);
    unsetenv("LAGFX_GPU_READBACK");
    lagfx_arm64_cq_shutdown(p, 0);
    lagfx_a64_exec_free(p);
    free(p);
    if (g_dev->vk->device != VK_NULL_HANDLE) {
        if (g_vs) vkDestroyShaderModule(g_dev->vk->device, g_vs, NULL);
        if (g_fs) vkDestroyShaderModule(g_dev->vk->device, g_fs, NULL);
    }
    lagfx_device_free(g_dev);
    free(g_mem);
    puts("PASS: gpu readback (F-2)");
    return 0;
}
