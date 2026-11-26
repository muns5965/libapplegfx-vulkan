/* ARM64 exec: type 11 (OBJ_MAPPED_TEXTURE) render targets over guest display surfaces. A fake shell supplies surface descriptors (get_surface_desc) and guest
 * memory; a type-11 ref is cleared + drawn by an exec and, with resource flag 0x04, the pixels land in the fake guest pages at the descriptor's row pitch and
 * first-byte offset. Also: stale guest never overwrites GPU data, flag 0x01 uploads, one image per mapping id across refs, unsupported fourcc / missing callback /
 * missing descriptor / absent pages / unreadable page list are counted and never crash. lavapipe only; NOT verified on a hardware GPU.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/stat.h>
#include "protocol/arm64_dump.h"

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
static bool rd(void *o, uint64_t gpa, uint64_t len, void *dst) { (void)o; if (gpa + len > MEM_SIZE) return false; memcpy(dst, g_mem + gpa, len); return true; }
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
static bool get_desc(void *o, uint32_t id, uint8_t *out, size_t cap, size_t *len) {
    (void)o; g_get_calls++;
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

/* 0x34 invalidate {task, count, {ref, host invalid, host valid, guest invalid, guest valid}} */
static void invalidate(lagfx_protocol_t *p, uint32_t stamp, uint32_t ref, uint8_t host_inv, uint8_t host_val, uint8_t guest_inv, uint8_t guest_val) {
    uint8_t b[20] = {0}; uint32_t task = 1, cnt = 1;
    memcpy(b, &task, 4); memcpy(b + 4, &cnt, 4); memcpy(b + 8, &ref, 4);
    b[12] = host_inv; b[13] = host_val; b[14] = guest_inv; b[15] = guest_val;
    submit(p, 0x34, stamp, (const uint32_t *)b, 5);
}
static void synchronize(lagfx_protocol_t *p, uint32_t stamp, uint32_t ref) {
    uint32_t w[3] = { 1, 1, ref };
    submit(p, 0x35, stamp, w, 3);
}

#define CNT(p) ((p)->diag2.exec.run)

int main(void) {
    g_mem = calloc(1, MEM_SIZE);
    g_dev = make_device();
    assert(g_dev);
    if (!g_dev->vk || !g_dev->vk->initialized) { puts("SKIP: no Vulkan device"); lagfx_device_free(g_dev); free(g_mem); return 77; }
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_protocol_t *p = mk();
    def_task(p, 1);
    build_objects(); surf_lists();
    lagfx_a64_exec_set_pipeline_resolver(p, resolver, NULL);
    static const double green[4] = { 0, 1, 0, 1 }, blue[4] = { 0, 0, 1, 1 };

    /* (1) clear + triangle into a type-11 target with flag 0x04: the pixels land in the guest surface pages at the row pitch / first offset, padding untouched */
    surf_fill(SENT);
    { const uint32_t res[1][2] = { { REF_M1, 2 | 4 } };
      run_exec(p, 1, REF_M1, 2, green, true, res, 1, true); }
    assert(cell(1) == 1 && lagfx_arm64_cq_pending(p) == 0);
    assert(CNT(p).no_target == 0 && CNT(p).passes == 1 && CNT(p).draws == 1 && CNT(p).draws_skipped == 0);
    assert(CNT(p).mapped_created == 1 && CNT(p).mapped_uploaded == 1 && CNT(p).mapped_written_back == 1 && CNT(p).mapped_unsupported == 0 && CNT(p).mapped_no_desc == 0);
    assert(CNT(p).mapped_bytes_up == SW * SH * 4u && CNT(p).mapped_bytes_down == SW * SH * 4u);
    assert(*surf_px(32, 40) == RED && *surf_px(0, 0) == GREEN && *surf_px(63, 79) == GREEN);
    { const uint8_t *s0 = surf_page(0);
      for (uint32_t i = 0; i < 2u << 14; i++) {   /* before `first`, the 64 padding bytes of every row, the tail: still the sentinel */
          bool in_px = i >= SFIRST && i < SFIRST + SROW * SH && ((i - SFIRST) % SROW) < SW * 4u;
          if (!in_px) assert(s0[i] == SENT);
      } }
    assert(*surf_px(10, 79) == GREEN || *surf_px(10, 79) == RED);   /* the last row lives on the second page (offset 0x100 + 79 * 320 > 16 KiB): written there too */
    assert((uint64_t)SFIRST + (uint64_t)SROW * 79 > 0x4000u);
    printf("PASS: type-11 target cleared + drawn, flag 0x04 wrote %llu bytes at pitch %u / first 0x%x, padding untouched\n", (unsigned long long)CNT(p).mapped_bytes_down, SROW, SFIRST);

    /* (2) a second ref / another v1 description over the SAME mapping id shares the one image */
    { lagfx_a64_obj_t *a = lagfx_a64_res_lookup(p, 1, REF_M1);
      const uint32_t res[1][2] = { { REF_M2, 0 } };
      run_exec(p, 2, REF_M2, 1, green, false, res, 1, true);   /* Load pass on the other ref: must see the red triangle */
      lagfx_a64_obj_t *b = lagfx_a64_res_lookup(p, 1, REF_M2);
      assert(a && b && a != b && a->img == b->img && a->ms == b->ms && CNT(p).mapped_created == 1 && CNT(p).mapped_shared == 1);
      uint8_t *img = malloc(SW * SH * 4u); uint32_t w, h;
      assert(lagfx_a64_res_readback(p, 1, REF_M2, img, SW * SH * 4u, &w, &h) && w == SW && h == SH);
      uint32_t c; memcpy(&c, img + (40u * SW + 32u) * 4u, 4); assert(c == RED);
      free(img); }
    printf("PASS: two refs naming one mapping id share one image\n");

    /* (3) stale guest never overwrites GPU output: GPU clears to blue (flag 2, no write-back); the guest pages keep the old pixels; host_valid is dropped and the
     * guest is marked stale (0x34), then an exec with flag 4 only must write BLUE back (the stale guest copy was NOT uploaded) */
    { const uint32_t res[1][2] = { { REF_M1, 2 } };
      run_exec(p, 3, REF_M1, 2, blue, false, res, 1, true); }
    assert(*surf_px(32, 40) == RED);                       /* flag 2 without 4: the guest is not told */
    invalidate(p, 4, REF_M1, 1, 0, 1, 0);                  /* host invalid, guest invalid */
    memset(surf_page(0), 0x5a, 2u << 14);                  /* guest garbage that must not reach the image */
    uint64_t up0 = CNT(p).mapped_uploaded;
    { const uint32_t res[1][2] = { { REF_M1, 4 } };
      run_exec(p, 5, REF_M1, 1, blue, false, res, 1, true); }
    assert(cell(1) == 5 && CNT(p).mapped_uploaded == up0);   /* no upload over the stale mark */
    assert(*surf_px(32, 40) == BLUE && *surf_px(0, 0) == BLUE && *surf_px(63, 79) == BLUE);
    assert(lagfx_a64_res_stats(p)->stale_skips >= 1);
    printf("PASS: a stale guest copy does not overwrite GPU data (blue written back, no upload)\n");

    /* (4) flag 0x01 (guest wrote it) uploads: the guest's pixels become the image's; a following 0x35 synchronize writes them back unchanged */
    for (uint32_t y = 0; y < SH; y++) for (uint32_t x = 0; x < SW; x++) *surf_px(x, y) = 0xff000000u | (y << 8) | x;
    { const uint32_t res[1][2] = { { REF_M1, 1 } };
      run_exec(p, 6, 0, 0, green, false, res, 1, false); }   /* no command buffer at all: just the resource flags */
    assert(cell(1) == 6 && CNT(p).mapped_uploaded == up0 + 1);
    { uint8_t *img = malloc(SW * SH * 4u); uint32_t w, h;
      assert(lagfx_a64_res_readback(p, 1, REF_M1, img, SW * SH * 4u, &w, &h));
      for (uint32_t y = 0; y < SH; y++) for (uint32_t x = 0; x < SW; x++) { uint32_t c; memcpy(&c, img + ((size_t)y * SW + x) * 4u, 4); assert(c == (0xff000000u | (y << 8) | x)); }
      free(img); }
    surf_fill(SENT);
    uint64_t wb0 = CNT(p).mapped_written_back;
    synchronize(p, 7, REF_M1);
    assert(cell(1) == 7 && CNT(p).mapped_written_back == wb0 + 1 && *surf_px(5, 7) == (0xff000000u | (7u << 8) | 5u) && *surf_px(63, 79) == (0xff000000u | (79u << 8) | 63u));
    printf("PASS: flag 0x01 uploads guest pixels (row pitch honoured); 0x35 synchronize writes the image back\n");

    /* (5) failure cases: counted, never fatal, the stamp always completes */
    { uint64_t unsup = CNT(p).mapped_unsupported, nodesc = CNT(p).mapped_no_desc, nt = CNT(p).no_target;
      { const uint32_t res[1][2] = { { REF_M3, 4 } }; run_exec(p, 8, REF_M3, 2, green, true, res, 1, true); }   /* half-float surface */
      assert(cell(1) == 8 && CNT(p).mapped_unsupported > unsup && CNT(p).no_target > nt && !lagfx_a64_res_lookup(p, 1, REF_M3));
      { const uint32_t res[1][2] = { { REF_M4, 4 } }; run_exec(p, 9, REF_M4, 2, green, true, res, 1, true); }   /* no descriptor for the id */
      assert(cell(1) == 9 && CNT(p).mapped_no_desc > nodesc && !lagfx_a64_res_lookup(p, 1, REF_M4));
      unsup = CNT(p).mapped_unsupported;
      { const uint32_t res[1][2] = { { REF_M7, 4 } }; run_exec(p, 10, REF_M7, 2, green, true, res, 1, true); }  /* descriptor outside the bounds */
      assert(cell(1) == 10 && CNT(p).mapped_unsupported > unsup && !lagfx_a64_res_lookup(p, 1, REF_M7)); }
    printf("PASS: unsupported fourcc / unknown mapping / out-of-range geometry counted, exec completes\n");

    /* callback NULL: counted as no_desc, nothing crashes */
    { uint64_t nodesc = CNT(p).mapped_no_desc;
      g_dev->desc.shell.get_surface_desc = NULL;
      { const uint32_t res[1][2] = { { REF_M8, 4 } }; run_exec(p, 11, REF_M8, 2, green, true, res, 1, true); }
      g_dev->desc.shell.get_surface_desc = get_desc;
      assert(cell(1) == 11 && CNT(p).mapped_no_desc > nodesc && !lagfx_a64_res_lookup(p, 1, REF_M8)); }
    printf("PASS: missing shell callback counted, exec completes\n");

    /* a page of the surface that is not present: its bytes are skipped (guest keeps them) and counted, the present page is written */
    { surf_fill(SENT);
      uint64_t sk = CNT(p).mapped_pages_skipped;
      const uint32_t res[1][2] = { { REF_M5, 2 | 4 } };
      run_exec(p, 12, REF_M5, 2, green, true, res, 1, true);
      assert(cell(1) == 12 && CNT(p).mapped_pages_skipped > sk && CNT(p).mapped_written_back >= 2);
      const uint8_t *s1 = surf_page(1);
      for (uint32_t i = 0; i < 0x4000; i++) assert(s1[i] == SENT);                       /* the absent page was never touched */
      assert(*surf_px(0, 0) == GREEN || surf_page(0)[SFIRST] != SENT); }
    printf("PASS: a non-present surface page is skipped and counted (pages_skipped=%llu)\n", (unsigned long long)CNT(p).mapped_pages_skipped);

    /* page list outside guest memory: the pull fails (counted), the pass still renders, nothing is written back, nothing crashes */
    { uint64_t nodesc = CNT(p).mapped_no_desc, wb = CNT(p).mapped_written_back;
      const uint32_t res[1][2] = { { REF_M6, 2 | 4 } };
      run_exec(p, 13, REF_M6, 2, green, true, res, 1, true);
      assert(cell(1) == 13 && CNT(p).mapped_no_desc > nodesc && CNT(p).mapped_written_back == wb); }
    printf("PASS: unreadable page list counted, no write-back, no crash\n");

    /* capture (LAGFX_ARM64_DUMP): a type-11 object of the resource list gets a `mapped` index line with the surface geometry from the shell */
    { char dir[64]; snprintf(dir, sizeof dir, "/tmp/lagfx-mapped-XXXXXX"); assert(mkdtemp(dir));
      setenv("LAGFX_ARM64_DUMP", "1", 1); setenv("LAGFX_ARM64_DUMP_DIR", dir, 1); lagfx_a64_dump_reset();
      const uint32_t res[2][2] = { { REF_M1, 0 }, { REF_M4, 0 } };
      run_exec(p, 14, REF_M1, 1, green, false, res, 2, true);
      lagfx_a64_dump_reset();
      char path[128], buf[8192]; snprintf(path, sizeof path, "%s/index.txt", dir);
      FILE *f = fopen(path, "rb"); assert(f); size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
      assert(strstr(buf, "mapped task=1 ref=10 mapping=7 tag=0x39 desc_size=56 surface=64x80 fourcc=42475241 row=320 first=256 first128="));
      assert(strstr(buf, "mapped task=1 ref=13 mapping=99 tag=0x39 desc_size=56 surface=unknown"));   /* the shell does not know this id */
      unsetenv("LAGFX_ARM64_DUMP"); unsetenv("LAGFX_ARM64_DUMP_DIR"); }
    printf("PASS: capture writes a `mapped` index line per type-11 resource (mapping, tag, desc_size, surface geometry)\n");

    /* (RGhA) the guest's main display surface: RGBA16Float, 8 bytes per pixel, row pitch 640 (> 512), tag 0x2f payload */
    { static const double col[4] = { 0.5, 0.25, 1, 1 };
      memset(surf_page(0), SENT, 4u << 14);
      uint64_t cr = CNT(p).mapped_created, un = CNT(p).mapped_unsupported, nt = CNT(p).no_target;
      const uint32_t res[1][2] = { { REF_M9, 2 | 4 } };
      run_exec(p, 15, REF_M9, 2, col, true, res, 1, true);
      assert(cell(1) == 15 && CNT(p).mapped_created == cr + 1 && CNT(p).mapped_unsupported == un && CNT(p).no_target == nt);
      assert(lagfx_a64_res_lookup(p, 1, REF_M9)->format == VK_FORMAT_R16G16B16A16_SFLOAT && lagfx_a64_res_lookup(p, 1, REF_M9)->bpp == 8);
      static const uint8_t clear8[8] = { 0x00, 0x38, 0x00, 0x34, 0x00, 0x3c, 0x00, 0x3c }, red8[8] = { 0x00, 0x3c, 0, 0, 0, 0, 0x00, 0x3c };   /* 0.5 .25 1 1 / 1 0 0 1 */
      assert(!memcmp(half_px(0, 0), clear8, 8) && !memcmp(half_px(63, 79), clear8, 8) && !memcmp(half_px(32, 40), red8, 8));
      const uint8_t *s0 = surf_page(0);
      for (uint32_t i = 0; i < 4u << 14; i++) {
          bool in_px = i >= SFIRST && i < SFIRST + HROW * SH && ((i - SFIRST) % HROW) < SW * 8u;
          if (!in_px) assert(s0[i] == SENT);
      }
      assert(CNT(p).mapped_bytes_down >= SW * SH * 8u);
      /* guest upload round trip: guest pixels (flag 0x01) -> image -> readback -> synchronize -> guest */
      for (uint32_t y = 0; y < SH; y++) for (uint32_t x = 0; x < SW; x++) { uint16_t v[4] = { (uint16_t)x, (uint16_t)y, (uint16_t)(x ^ y), 0x3c00 }; memcpy(half_px(x, y), v, 8); }
      { const uint32_t r1[1][2] = { { REF_M9, 1 } }; run_exec(p, 16, 0, 0, col, false, r1, 1, false); }
      uint8_t *img = malloc(SW * SH * 8u); uint32_t w, h;
      assert(lagfx_a64_res_readback(p, 1, REF_M9, img, SW * SH * 8u, &w, &h) && w == SW && h == SH);
      for (uint32_t y = 0; y < SH; y++) for (uint32_t x = 0; x < SW; x++) { uint16_t v[4] = { (uint16_t)x, (uint16_t)y, (uint16_t)(x ^ y), 0x3c00 }; assert(!memcmp(img + ((size_t)y * SW + x) * 8u, v, 8)); }
      free(img);
      memset(surf_page(0), SENT, 4u << 14);
      synchronize(p, 17, REF_M9);
      for (uint32_t y = 0; y < SH; y++) for (uint32_t x = 0; x < SW; x++) { uint16_t v[4] = { (uint16_t)x, (uint16_t)y, (uint16_t)(x ^ y), 0x3c00 }; assert(!memcmp(half_px(x, y), v, 8)); }
      assert(s0[SFIRST + 600] == SENT); }
    printf("PASS: RGhA (RGBA16Float) surface: clear + draw written back as raw half floats at pitch %u, guest upload round trip exact\n", HROW);

    /* (3b) EXPERIMENT switch LAGFX_ARM64_MAPPED_WRITEBACK=1: a flag-2 exec (GPU rendered into the surface) also writes the surface to the guest. OFF: unchanged (the
     * guest keeps its pixels although the GPU drew GREEN); ON: the guest sees GREEN after that exec, once. */
    { const uint32_t res[1][2] = { { REF_M1, 2 } };
      uint64_t wbx = CNT(p).mapped_written_back;
      run_exec(p, 30, REF_M1, 2, green, false, res, 1, true);
      surf_fill(SENT);
      assert(*surf_px(32, 40) == 0xabababab && CNT(p).mapped_written_back == wbx);   /* switch off */
      setenv("LAGFX_ARM64_MAPPED_WRITEBACK", "1", 1);
      run_exec(p, 31, REF_M1, 2, green, false, res, 1, true);
      unsetenv("LAGFX_ARM64_MAPPED_WRITEBACK");
      assert(*surf_px(32, 40) == GREEN && *surf_px(0, 0) == GREEN && *surf_px(63, 79) == GREEN && CNT(p).mapped_written_back == wbx + 1); }
    printf("PASS: MAPPED_WRITEBACK experiment: off = guest untouched by flag 2, on = rendered surface written back once per exec\n");

    printf("INFO: validation messenger %s, messages %llu\n", g_dev->vk->debug_messenger ? "ACTIVE" : "inactive", (unsigned long long)lagfx_vk_validation_msg_count);
    assert(lagfx_vk_validation_msg_count == 0);
    lagfx_arm64_cq_shutdown(p, 0);
    lagfx_a64_exec_free(p);
    free(p);
    if (g_dev->vk->device != VK_NULL_HANDLE) {
        if (g_vs) vkDestroyShaderModule(g_dev->vk->device, g_vs, NULL);
        if (g_fs) vkDestroyShaderModule(g_dev->vk->device, g_fs, NULL);
    }
    lagfx_device_free(g_dev);
    free(g_mem);
    puts("PASS: arm64 mapped textures");
    return 0;
}
