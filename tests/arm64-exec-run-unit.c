/* ARM64 0x43/0x45 execution of Render segments (LAGFX_ARM64_EXEC=1): (task, ref) object table lifetime, and real Vulkan recording
 * (lavapipe in CI / this container; NOT verified on a hardware GPU) read back pixel by pixel. SKIPs the Vulkan part when no ICD is present.
 * SPDX-License-Identifier: MIT */
#include "dispatchers/channel_compute_dispatcher.h"
#include "dispatchers/ring_common.h"
#include "protocol/state.h"
#include "protocol/arm64_exec_decode.h"
#include "protocol/arm64_exec_run.h"
#include "protocol/arm64_objects.h"
#include "device.h"
#include "libapplegfx-vulkan.h"
#ifdef LAGFX_HAVE_VULKAN
#include "vulkan/instance.h"
#include <vulkan/vulkan.h>
#include "arm64-exec-run-shaders.h"
#include "arm64-exec-blend-shader.h"
#include "arm64-exec-fetch-shader.h"
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

/* ---- guest layout ---- */
#define VA_OBJ    0x0100u            /* object list: ref * 12 (objects_addr must be non-zero) */
#define VA_PAY    0x4000u            /* payloads of the entries */
#define VA_CMD    0x8000u            /* command buffer */
#define VPN_VB    10u                /* vertex data page */
#define VPN_IB    11u
#define VPN_TEX   12u                /* guest pages behind the render-target texture */                /* index data page */
#define REF_TEX 1u
#define REF_VB  2u
#define REF_PIPE 3u
#define REF_IB  4u
#define REF_TEX2 5u
#define VPN_TEX2 13u
#define RT_W 16u
#define RT_H 16u

static void entry(uint32_t ref, uint32_t type, uint32_t size, uint64_t addr) {
    uint32_t e[3] = { type | size << 8, (uint32_t)addr, (uint32_t)(addr >> 32) };
    va_write(VA_OBJ + ref * 12u, e, 12);
}
static void put_buffer_object(uint32_t ref, uint32_t pay_off, uint64_t len, uint32_t vpage) {
    uint8_t h[16] = {0};
    memcpy(h, &len, 8); memcpy(h + 8, &vpage, 4);
    va_write(VA_PAY + pay_off, h, 16);
    entry(ref, LAGFX_A64_OBJ_BUFFER, 16, VA_PAY + pay_off);
}
static void build_objects(void) {
    /* texture (mirror layout): {u64 length, u32 vpage}, layout (mips = 1, mip table: off, row, w, h, d), {tag, size, ref}, v1 description (2D, BGRA8 = 80, w x h, 1 sample) */
    uint8_t t[220]; memset(t, 0, sizeof t);
    uint64_t tlen = RT_W * RT_H * 4u; uint32_t tvp = VPN_TEX;
    memcpy(t, &tlen, 8); memcpy(t + 8, &tvp, 4);
    t[12] = 1;                                   /* mips */
    { uint8_t *mp = t + 12 + 0x18; uint64_t row = RT_W * 4u; uint32_t mw = RT_W, mh = RT_H, md = 1; memcpy(mp + 0x10, &row, 8); memcpy(mp + 0x18, &mw, 4); memcpy(mp + 0x1c, &mh, 4); memcpy(mp + 0x20, &md, 4); }
    uint32_t rec = 12 + 0x18 + 36, tag = 0x14;
    memcpy(t + rec, &tag, 4);
    uint8_t *d = t + rec + 12;
    d[0] = 2; d[2] = 80;
    uint32_t w = RT_W, h = RT_H, one = 1;
    memcpy(d + 4, &w, 4); memcpy(d + 8, &h, 4); memcpy(d + 12, &one, 4);
    d[16] = 1; d[18] = 1;
    uint32_t tsize = rec + 12 + 28;
    va_write(VA_PAY, t, tsize);
    entry(REF_TEX, LAGFX_A64_OBJ_TEXTURE, tsize, VA_PAY);
    /* vertex buffer: 4 vertices (float2). 0..2 a big triangle, 3 spare. */
    const float v[8] = { -0.9f, -0.9f, 0.9f, -0.9f, 0.0f, 0.9f, 0.0f, 0.0f };
    va_write((uint64_t)VPN_VB << 14, v, sizeof v);
    put_buffer_object(REF_VB, 0x200, 0x4000, VPN_VB);
    const uint16_t idx[6] = { 0, 1, 2, 0, 1, 2 };
    va_write((uint64_t)VPN_IB << 14, idx, sizeof idx);
    put_buffer_object(REF_IB, 0x220, 0x4000, VPN_IB);
    uint8_t pl[16] = {0}; uint32_t ptag = 0xe; memcpy(pl, &ptag, 4);
    va_write(VA_PAY + 0x300, pl, 16);
    entry(REF_PIPE, LAGFX_A64_OBJ_SERIALIZED, 16, VA_PAY + 0x300);
    {   /* a second render target: RGBA16Float (pixel format 115), 8 bytes per pixel */
        uint8_t t2[220]; memset(t2, 0, sizeof t2);
        uint64_t len2 = RT_W * RT_H * 8u; uint32_t vp2 = VPN_TEX2;
        memcpy(t2, &len2, 8); memcpy(t2 + 8, &vp2, 4);
        t2[12] = 1;
        { uint8_t *mp = t2 + 12 + 0x18; uint64_t row = RT_W * 8u; uint32_t mw = RT_W, mh = RT_H, md = 1; memcpy(mp + 0x10, &row, 8); memcpy(mp + 0x18, &mw, 4); memcpy(mp + 0x1c, &mh, 4); memcpy(mp + 0x20, &md, 4); }
        uint32_t rec2 = 12 + 0x18 + 36, tag2 = 0x14;
        memcpy(t2 + rec2, &tag2, 4);
        uint8_t *d2 = t2 + rec2 + 12;
        d2[0] = 2; d2[2] = 115;
        uint32_t w2 = RT_W, h2 = RT_H, one2 = 1;
        memcpy(d2 + 4, &w2, 4); memcpy(d2 + 8, &h2, 4); memcpy(d2 + 12, &one2, 4);
        d2[16] = 1; d2[18] = 1;
        uint32_t ts2 = rec2 + 12 + 28;
        va_write(VA_PAY + 0x400, t2, ts2);
        entry(REF_TEX2, LAGFX_A64_OBJ_TEXTURE, ts2, VA_PAY + 0x400);
    }
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
    memcpy(c, &ref, 4);
    memcpy(c + 20, &load, 2);
    uint16_t store = 1; memcpy(c + 22, &store, 2);
    memcpy(c + 0x1c, clear, 32);
    cb_cmd(26, r, sizeof r);
}
/* a pass whose colour attachments are given per slot (0 = unused): texture ref, load action (2 clear), clear colour */
static void cb_pass_n(const uint32_t ref[2], const uint16_t load[2], const double clear[2][4]) {
    uint8_t r[584]; memset(r, 0, sizeof r);
    for (int k = 0; k < 2; k++) {
        if (!ref[k]) continue;
        uint8_t *c = r + 0x4c + 0x3c * k;
        memcpy(c, &ref[k], 4);
        memcpy(c + 20, &load[k], 2);
        uint16_t store = 1; memcpy(c + 22, &store, 2);
        memcpy(c + 0x1c, clear[k], 32);
    }
    cb_cmd(26, r, sizeof r);
}
static void cb_viewport(double w, double h) { double v[6] = { 0, 0, w, h, 0, 1 }; cb_cmd(130, v, sizeof v); }
static void cb_pipeline(uint32_t ref) { cb_cmd(116, &ref, 4); }
static void cb_vbuf(uint32_t ref, uint64_t off) { uint8_t r[20]; uint32_t idx = 0, cnt = 1; memcpy(r, &idx, 4); memcpy(r + 4, &cnt, 4); memcpy(r + 8, &ref, 4); memcpy(r + 12, &off, 8); cb_cmd(125, r, 20); }
static void cb_draw(uint32_t start, uint32_t count) { uint8_t r[8]; uint32_t prim = 3; uint16_t s = (uint16_t)start, c = (uint16_t)count; memcpy(r, &prim, 4); memcpy(r + 4, &s, 2); memcpy(r + 6, &c, 2); cb_cmd(1, r, 8); }
static void cb_draw_indexed(uint32_t ref, uint32_t count) { uint8_t r[12] = {0}; uint16_t prim = 3, it = 0, c = (uint16_t)count; memcpy(r, &prim, 2); memcpy(r + 2, &it, 2); memcpy(r + 4, &ref, 4); memcpy(r + 8, &c, 2); cb_cmd(7, r, 12); }
static uint32_t cb_finish(void) {
    uint32_t seglen = g_cbn, info = 0;      /* seglen = the whole buffer incl. the 8-byte header, as the real guest writes it; type 0 render */
    memcpy(g_cb, &seglen, 4); memcpy(g_cb + 4, &info, 4);
    va_write(VA_CMD, g_cb, g_cbn);
    return g_cbn;
}
static bool g_res_tex2;   /* also list the second render target in the exec's resource list */
static void exec(lagfx_protocol_t *p, uint32_t task, uint32_t len, bool with_res) {
    uint32_t w[3 + 10 + 4], n = 0;
    w[n++] = task; w[n++] = with_res ? (g_res_tex2 ? 5 : 4) : 0; w[n++] = 1;
    if (with_res) { w[n++] = REF_TEX; w[n++] = 2; w[n++] = REF_VB; w[n++] = 1; w[n++] = REF_PIPE; w[n++] = 0; w[n++] = REF_IB; w[n++] = 1; if (g_res_tex2) { w[n++] = REF_TEX2; w[n++] = 2; } }
    uint64_t off = VA_CMD, l = len;
    memcpy(w + n, &off, 8); n += 2; memcpy(w + n, &l, 8); n += 2;
    submit(p, 0x43, 0, w, n);
}

/* ---- (A) object table lifetime, no Vulkan ---- */
static int g_created, g_destroyed;
static void *fk_create(void *c, lagfx_protocol_t *p, const lagfx_task_entry_t *t, uint32_t ref, const uint32_t e[3]) {
    (void)c; (void)p; (void)t; (void)e;
    g_created++;
    if (ref == 99) return NULL;                      /* a failing create */
    int *o = malloc(sizeof *o); *o = (int)ref; return o;
}
static void fk_destroy(void *c, void *o) { (void)c; g_destroyed++; free(o); }

static void test_objects(void) {
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    lagfx_task_entry_t *t1 = def_task(p, 1), *t2 = def_task(p, 2);
    entry(1, 1, 16, 0x5000); entry(2, 1, 16, 0x5100); entry(3, 0, 0, 0); entry(99, 7, 16, 0x5200);
    lagfx_a64_obj_ops_t ops = { NULL, fk_create, fk_destroy };
    lagfx_a64_objects_t *t = lagfx_a64_objects_new(&ops);
    assert(t);
    g_created = g_destroyed = 0;
    uint32_t e[3];
    void *a = lagfx_a64_objects_get(t, p, t1, 1, e);
    assert(a && *(int *)a == 1 && (e[0] & 0xff) == 1 && g_created == 1);
    assert(lagfx_a64_objects_get(t, p, t1, 1, NULL) == a && g_created == 1);            /* hit: not created twice */
    assert(lagfx_a64_objects_get(t, p, t2, 1, NULL) != a && g_created == 2);            /* same ref, other task: its own object */
    assert(!lagfx_a64_objects_get(t, p, t1, 0, NULL));                                  /* ref 0 */
    assert(!lagfx_a64_objects_get(t, p, t1, 3, NULL) && g_created == 2);                /* empty slot: nothing built */
    assert(!lagfx_a64_objects_get(t, p, t1, 99, NULL) && g_created == 3);
    assert(!lagfx_a64_objects_get(t, p, t1, 99, NULL) && g_created == 3);               /* failure is remembered, not retried */
    entry(1, 1, 16, 0x6000);                                                            /* the guest rewrites the slot */
    void *a2 = lagfx_a64_objects_get(t, p, t1, 1, NULL);
    assert(a2 && g_created == 4 && g_destroyed == 1);                                   /* old destroyed, new created */
    entry(3, 1, 16, 0x5300);                                                            /* an empty slot gets filled later */
    assert(lagfx_a64_objects_get(t, p, t1, 3, NULL) && g_created == 5);
    lagfx_a64_obj_stats_t st;
    lagfx_a64_objects_stats(t, &st);
    assert(st.live == 4 && st.created == 4 && st.destroyed == 1 && st.failed == 1);      /* the failed create is not counted as created */
    assert(lagfx_a64_objects_drop_task(t, 1) == 3);                                     /* refs 1, 3 and the remembered failure 99 */
    lagfx_a64_objects_stats(t, &st);
    assert(st.live == 1);
    assert(lagfx_a64_objects_drop_task(t, 1) == 0);
    lagfx_a64_objects_free(t);
    assert(g_created - 1 == g_destroyed);                                               /* every created object (the NULL one aside) destroyed: no leaks */
    free(p);
    puts("PASS: object table lifetime");
}

#ifdef LAGFX_HAVE_VULKAN
/* ---- pipeline resolver: the two trivial SPIR-V shaders + the empty layout ---- */
static VkShaderModule g_vs, g_fs, g_fs_half, g_fs_mrt;
static struct { int set, enable, mrt; uint32_t sf, df, op, saf, daf, aop, mask, mask1; int s1add, nomasks, fetch, fail; } g_blend;
static VkShaderModule g_fs_fetch2, g_fetch2_unused; static VkDescriptorSetLayout g_fetch2_dsl; static VkPipelineLayout g_fetch2_pl;
static VkShaderModule g_fs_fetch; static VkDescriptorSetLayout g_fetch_dsl; static VkPipelineLayout g_fetch_pl;   /* desc.blend_* the resolver hands out (set == 0: the fixed state) */
static int g_released;
static void rel(void *u) { (void)u; g_released++; }
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
    if (g_blend.set) {
        if (!g_fs_half) { VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof k_fs_half_spv, .pCode = k_fs_half_spv }; assert(vkCreateShaderModule(vk->device, &ci, NULL, &g_fs_half) == VK_SUCCESS); }
        out->desc.fragment_shader = g_fs_half;   /* premultiplied (0.5, 0, 0, 0.5) */
        out->desc.blend_set = 1, out->desc.blend_enable = (uint8_t)g_blend.enable;
        out->desc.blend_src_rgb = g_blend.sf, out->desc.blend_dst_rgb = g_blend.df, out->desc.blend_op_rgb = g_blend.op;
        out->desc.blend_src_a = g_blend.saf, out->desc.blend_dst_a = g_blend.daf, out->desc.blend_op_a = g_blend.aop;
        out->desc.color_write_mask = g_blend.mask;
        out->desc.color_write_masks[0] = g_blend.mask, out->desc.color_write_masks[1] = g_blend.mask1, out->desc.color_write_masks[2] = out->desc.color_write_masks[3] = 0xfu;
        out->desc.masks_valid = 1;
        if (g_blend.s1add) { out->desc.slot_blend[1].set = 1, out->desc.slot_blend[1].enable = 1; out->desc.slot_blend[1].src_rgb = out->desc.slot_blend[1].dst_rgb = out->desc.slot_blend[1].src_a = out->desc.slot_blend[1].dst_a = VK_BLEND_FACTOR_ONE; }
        if (g_blend.nomasks) { out->desc.masks_valid = 0; for (int i = 0; i < 4; i++) out->desc.color_write_masks[i] = 0; }
        if (g_blend.mrt) {
            if (!g_fs_mrt) { VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof k_fs_mrt_spv, .pCode = k_fs_mrt_spv }; assert(vkCreateShaderModule(vk->device, &ci, NULL, &g_fs_mrt) == VK_SUCCESS); }
            out->desc.fragment_shader = g_fs_mrt;
        }
    }
    if (g_blend.fetch == 1) {   /* framebuffer fetch: FS reads input attachment 0 (binding 24) = colour attachment 0 */
        if (!g_fs_fetch) {
            VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof k_fs_fetch_spv, .pCode = k_fs_fetch_spv };
            assert(vkCreateShaderModule(vk->device, &ci, NULL, &g_fs_fetch) == VK_SUCCESS);
            VkDescriptorSetLayoutBinding lb = { .binding = 24, .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
            VkDescriptorSetLayoutCreateInfo dl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &lb };
            assert(vkCreateDescriptorSetLayout(vk->device, &dl, NULL, &g_fetch_dsl) == VK_SUCCESS);
            VkPipelineLayoutCreateInfo pl = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_fetch_dsl };
            assert(vkCreatePipelineLayout(vk->device, &pl, NULL, &g_fetch_pl) == VK_SUCCESS);
        }
        out->desc.fragment_shader = g_fs_fetch, out->desc.layout = g_fetch_pl, out->dsl = g_fetch_dsl;
        out->n_bind = 1, out->bind[0].binding = 24, out->bind[0].kind = LAGFX_A64_BIND_INPUT, out->bind[0].stage = 1, out->bind[0].metal_index = 0;
    }
    if (g_blend.fetch == 2) {   /* two input attachments (bindings 24, 25) and two outputs */
        if (!g_fs_fetch2) {
            VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof k_fs_fetch2_spv, .pCode = k_fs_fetch2_spv };
            assert(vkCreateShaderModule(vk->device, &ci, NULL, &g_fs_fetch2) == VK_SUCCESS);
            VkDescriptorSetLayoutBinding lb[2] = { { .binding = 24, .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
                                                   { .binding = 25, .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT } };
            VkDescriptorSetLayoutCreateInfo dl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = lb };
            assert(vkCreateDescriptorSetLayout(vk->device, &dl, NULL, &g_fetch2_dsl) == VK_SUCCESS);
            VkPipelineLayoutCreateInfo pl = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_fetch2_dsl };
            assert(vkCreatePipelineLayout(vk->device, &pl, NULL, &g_fetch2_pl) == VK_SUCCESS);
        }
        out->desc.fragment_shader = g_fs_fetch2, out->desc.layout = g_fetch2_pl, out->dsl = g_fetch2_dsl;
        out->n_bind = 2;
        for (int i = 0; i < 2; i++) out->bind[i].binding = (uint8_t)(24 + i), out->bind[i].kind = LAGFX_A64_BIND_INPUT, out->bind[i].stage = 1, out->bind[i].metal_index = (int16_t)i;
        out->desc.blend_set = 1, out->desc.blend_enable = 0, out->desc.blend_src_rgb = out->desc.blend_src_a = VK_BLEND_FACTOR_ONE, out->desc.blend_dst_rgb = out->desc.blend_dst_a = VK_BLEND_FACTOR_ZERO;
        out->desc.color_write_mask = 0xf, out->desc.masks_valid = 1;
        for (int i = 0; i < 4; i++) out->desc.color_write_masks[i] = 0xf;
    }
    if (g_blend.fail) out->desc.fragment_shader = VK_NULL_HANDLE;   /* pipeline build refuses a missing stage */
    out->release = rel;
    return true;
}

static uint32_t px(const uint8_t *img, uint32_t x, uint32_t y) { uint32_t v; memcpy(&v, img + (y * RT_W + x) * 4, 4); return v; }
#define RED   0xffff0000u   /* B8G8R8A8 little-endian word: A R G B */
#define GREEN 0xff00ff00u

static void test_exec_disabled(void) {
    unsetenv("LAGFX_ARM64_EXEC");
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    lagfx_task_entry_t *t = def_task(p, 1); (void)t;
    build_objects();
    static const double g[4] = { 0, 1, 0, 1 };
    cb_reset(); cb_pass(REF_TEX, 2, g); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, 0); cb_draw(0, 3);
    exec(p, 1, cb_finish(), true);
    assert(p->a64_exec == NULL && p->diag2.exec.run.execs == 0 && p->diag2.exec.run.passes == 0);
    assert(p->diag2.exec.received == 1 && p->diag2.exec.decoded == 1);              /* decode unchanged */
    /* x86 mode with the env on: still nothing */
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_protocol_t *q = mk(0);
    def_task(q, 1); build_objects();
    cb_reset(); cb_pass(REF_TEX, 2, g); cb_finish();
    exec(q, 1, g_cbn, true);
    assert(q->a64_exec == NULL && q->diag2.exec.run.execs == 0);
    unsetenv("LAGFX_ARM64_EXEC");
    free(p); free(q);
    puts("PASS: execution off by default / in x86 mode");
}

static void test_exec_render(void) {
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    lagfx_task_entry_t *t = def_task(p, 1);
    build_objects();
    lagfx_a64_exec_set_pipeline_resolver(p, resolver, NULL);
    static const double g[4] = { 0, 1, 0, 1 };
    uint8_t img[RT_W * RT_H * 4];
    uint32_t w, h;

    /* exec 1: clear green, pipeline, vertex buffer, viewport, one draw; plus a tag nobody implements and fragment textures (recorded, not bound) */
    cb_reset(); cb_pass(REF_TEX, 2, g); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, 0);
    { uint8_t r[16] = {0}; cb_cmd(114, r, 8); cb_cmd(99, r, 8); }
    cb_draw(0, 3);
    exec(p, 1, cb_finish(), true);
    __typeof__(p->diag2.exec.run) *R = &p->diag2.exec.run;
    assert(R->execs == 1 && R->passes == 1 && R->draws == 1 && R->submits == 1 && R->submit_failed == 0 && R->submit_pending == 0);
    assert(R->tag_done[26] == 1 && R->tag_done[116] == 1 && R->tag_done[125] == 1 && R->tag_done[130] == 1 && R->tag_done[1] == 1);
    assert(R->tag_done[114] == 1 && R->tag_unsup[99] == 1);                   /* fragment textures: bound now (count 0 here) */
    assert(p->diag2.exec.decoded == 1 && p->diag2.exec.failed == 0);                /* decode bookkeeping unchanged by execution */
    assert(lagfx_a64_exec_readback_texture(p, t, REF_TEX, img, sizeof img, &w, &h) && w == RT_W && h == RT_H);
    assert(px(img, 8, 8) == RED);                                                   /* inside the triangle */
    int green = (px(img, 0, 0) == GREEN) + (px(img, 15, 0) == GREEN) + (px(img, 0, 15) == GREEN) + (px(img, 15, 15) == GREEN);
    assert(green >= 2);                                                             /* the clear colour survives outside it */

    /* exec 2: Load + indexed draw (index buffer in a second guest page) over what exec 1 drew; clear to green first in a fresh pass to check Load keeps pixels */
    cb_reset(); cb_pass(REF_TEX, 1, g); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, 0); cb_draw_indexed(REF_IB, 3);
    exec(p, 1, cb_finish(), true);
    assert(R->execs == 2 && R->passes == 2 && R->draws == 2 && R->tag_done[7] == 1 && R->submit_failed == 0);
    assert(lagfx_a64_exec_readback_texture(p, t, REF_TEX, img, sizeof img, &w, &h));
    assert(px(img, 8, 8) == RED && green == (px(img, 0, 0) == GREEN) + (px(img, 15, 0) == GREEN) + (px(img, 0, 15) == GREEN) + (px(img, 15, 15) == GREEN));

    /* exec 3: malformed records and a draw with no pipeline: counted, nothing aborts, the pass still ends and the exec completes */
    cb_reset(); cb_pass(REF_TEX, 2, g);
    { uint8_t r[4] = {0}; cb_cmd(125, r, 4); cb_cmd(130, r, 4); cb_cmd(116, r, 0); }
    cb_cmd(1, (uint8_t[8]){3, 0, 0, 0, 0, 0, 3, 0}, 8);                             /* draw without pipeline_set */
    { uint8_t sh[4] = {0}; cb_cmd(26, sh, 4); }                                                       /* short begin-pass record */
    exec(p, 1, cb_finish(), true);
    assert(R->execs == 3 && R->bad_rec >= 4 && R->no_pipeline >= 1 && p->diag2.exec.received == 3);

    /* object lifetime: everything created, nothing leaked after teardown */
    uint32_t live; uint64_t cr, de;
    lagfx_a64_exec_object_counts(p, &live, &cr, &de);
    assert(live >= 3 && cr >= 3 && cr - de <= live);
    lagfx_a64_exec_drop_task(p, 1);
    lagfx_a64_exec_object_counts(p, &live, &cr, &de);
    assert(live == 0 && cr == de && g_released == 1);
    lagfx_a64_exec_free(p);
    assert(p->a64_exec == NULL);
    free(p);
    puts("PASS: render exec on Vulkan (pixels, tags, malformed records, teardown)");
}

static void test_exec_no_resolver_and_pending(void) {
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    lagfx_task_entry_t *t = def_task(p, 1);
    build_objects();
    static const double g[4] = { 0, 1, 0, 1 };
    cb_reset(); cb_pass(REF_TEX, 2, g); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, 0); cb_draw(0, 3);
    exec(p, 1, cb_finish(), true);
    __typeof__(p->diag2.exec.run) *R = &p->diag2.exec.run;
    uint8_t img[RT_W * RT_H * 4];
    assert(R->pipeline_unresolved >= 1 && R->draws == 0 && R->draws_skipped == 1 && R->passes == 1 && R->submit_failed == 0);
    assert(lagfx_a64_exec_readback_texture(p, t, REF_TEX, img, sizeof img, NULL, NULL));
    assert(px(img, 8, 8) == GREEN);                                                 /* the clear still happened */
    lagfx_a64_exec_free(p); free(p);

    /* a 1 ms wait may end before lavapipe is done (first pipeline build): that is PENDING, not failed, and the result must still be right once polled */
    setenv("LAGFX_GPU_WAIT_MS", "1", 1);
    p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    t = def_task(p, 1); build_objects();
    lagfx_a64_exec_set_pipeline_resolver(p, resolver, NULL);
    cb_reset(); cb_pass(REF_TEX, 2, g); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, 0); cb_draw(0, 3);
    exec(p, 1, cb_finish(), true);
    R = &p->diag2.exec.run;
    assert(R->submit_failed == 0 && R->submits == 1);
    struct timespec ts = { 0, 5 * 1000000 };
    for (int i = 0; i < 2000 && lagfx_a64_exec_poll(p); i++) nanosleep(&ts, NULL);
    assert(lagfx_a64_exec_poll(p) == 0);
    assert(lagfx_a64_exec_readback_texture(p, t, REF_TEX, img, sizeof img, NULL, NULL) && px(img, 8, 8) == RED);
    printf("INFO: 1 ms wait ended %s\n", R->submit_pending ? "PENDING (then reaped)" : "DONE (GPU was faster than 1 ms)");
    unsetenv("LAGFX_GPU_WAIT_MS");
    lagfx_a64_exec_free(p); free(p);
    puts("PASS: no resolver (draw skipped, clear kept) / bounded wait");
}
#endif


/* Blend state reaches the pixels: BGRA8 render target cleared to opaque blue (0,0,1,1), one triangle of the premultiplied colour (0.5,0,0,0.5) drawn over it
 * with the Vulkan state the resolver hands out. px() words are A R G B. */
static void blend_case(const char *name, int enable, uint32_t sf, uint32_t df, uint32_t op, uint32_t mask, uint32_t a, uint32_t r, uint32_t g, uint32_t b) {
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    lagfx_task_entry_t *t = def_task(p, 1);
    build_objects();
    lagfx_a64_exec_set_pipeline_resolver(p, resolver, NULL);
    g_blend.set = 1, g_blend.enable = enable, g_blend.sf = sf, g_blend.df = df, g_blend.op = op, g_blend.saf = sf, g_blend.daf = df, g_blend.aop = op, g_blend.mask = mask;
    static const double blue[4] = { 0, 0, 1, 1 };
    uint8_t img[RT_W * RT_H * 4]; uint32_t w, h;
    cb_reset(); cb_pass(REF_TEX, 2, blue); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, 0); cb_draw(0, 3);
    exec(p, 1, cb_finish(), true);
    assert(lagfx_a64_exec_readback_texture(p, t, REF_TEX, img, sizeof img, &w, &h));
    uint32_t v = px(img, 8, 8), ga = v >> 24, gr = (v >> 16) & 255, gg = (v >> 8) & 255, gb = v & 255;
    int ok = (uint32_t)abs((int)ga - (int)a) <= 1 && (uint32_t)abs((int)gr - (int)r) <= 1 && (uint32_t)abs((int)gg - (int)g) <= 1 && (uint32_t)abs((int)gb - (int)b) <= 1;
    if (!ok) fprintf(stderr, "blend case %s: got A%u R%u G%u B%u, want A%u R%u G%u B%u\n", name, ga, gr, gg, gb, a, r, g, b);
    assert(ok);
    assert(px(img, 0, 0) == 0xff0000ffu || mask != 0xfu || !enable);   /* outside the triangle the clear colour stays */
    lagfx_a64_exec_drop_task(p, 1);
    lagfx_a64_exec_free(p);
    free(p);
    g_blend.set = 0;
}

static void test_exec_blend(void) {
    enum { ZERO = VK_BLEND_FACTOR_ZERO, ONE = VK_BLEND_FACTOR_ONE, OMSA = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA };
    const uint32_t ALL = 0xfu;   /* VK_COLOR_COMPONENT_R|G|B|A */
    blend_case("off (overwrite)",             0, ONE, ZERO, VK_BLEND_OP_ADD, ALL, 128, 128, 0, 0);
    blend_case("source-over premultiplied",   1, ONE, OMSA, VK_BLEND_OP_ADD, ALL, 255, 128, 0, 128);
    blend_case("additive",                    1, ONE, ONE, VK_BLEND_OP_ADD, ALL, 255, 128, 0, 255);
    blend_case("reverse subtract",            1, ONE, ONE, VK_BLEND_OP_REVERSE_SUBTRACT, ALL, 128, 0, 0, 255);
    blend_case("write mask none",             0, ONE, ZERO, VK_BLEND_OP_ADD, 0u, 255, 0, 0, 255);
    blend_case("write mask RGB (alpha kept)", 0, ONE, ZERO, VK_BLEND_OP_ADD, 0x7u, 255, 128, 0, 0);
    blend_case("write mask A only",           0, ONE, ZERO, VK_BLEND_OP_ADD, 0x8u, 128, 0, 0, 255);
    puts("PASS: blend state reaches the pixels (off, source-over, additive, reverse subtract, write masks) on lavapipe");
}

/* Two colour attachments (slot 0 BGRA8, slot 1 RGBA16Float) rendered by ONE draw whose fragment shader writes o0 = (0.5,0,0,0.5), o1 = (0,1,0,1). */
static uint16_t half_at(const uint8_t *img, uint32_t x, uint32_t y, uint32_t c) { uint16_t v; memcpy(&v, img + ((y * RT_W + x) * 4u + c) * 2u, 2); return v; }
static void mrt_case(const char *name, bool slot0, bool slot1, uint32_t mask0, uint32_t mask1) {
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    lagfx_task_entry_t *t = def_task(p, 1);
    build_objects();
    lagfx_a64_exec_set_pipeline_resolver(p, resolver, NULL);
    g_blend.set = 1, g_blend.mrt = 1, g_blend.enable = 0, g_blend.sf = VK_BLEND_FACTOR_ONE, g_blend.df = VK_BLEND_FACTOR_ZERO, g_blend.op = VK_BLEND_OP_ADD;
    g_blend.saf = g_blend.sf, g_blend.daf = g_blend.df, g_blend.aop = g_blend.op, g_blend.mask = mask0, g_blend.mask1 = mask1;
    g_res_tex2 = slot1;
    const uint32_t refs[2] = { slot0 ? REF_TEX : 0u, slot1 ? REF_TEX2 : 0u };
    const uint16_t loads[2] = { 2, 2 };
    const double clr[2][4] = { { 0, 0, 1, 1 }, { g_blend.s1add ? 0.5 : 0, 0, 0, 1 } };
    cb_reset(); cb_pass_n(refs, loads, clr); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, 0); cb_draw(0, 3);
    exec(p, 1, cb_finish(), true);
    uint8_t img[RT_W * RT_H * 8]; uint32_t w, h;
    __typeof__(p->diag2.exec.run) *R = &p->diag2.exec.run;
    if (slot0) {
        assert(lagfx_a64_exec_readback_texture(p, t, REF_TEX, img, RT_W * RT_H * 4, &w, &h));
        uint32_t v = px(img, 8, 8), want = (mask0 & 1u) ? 0x80000080u : 0xff0000ffu;   /* R mask bit 0: A R G B = 128 128 0 0 vs the blue clear */
        (void)want;
        if (mask0 == 0xfu) assert((v >> 24) >= 127 && (v >> 24) <= 129 && ((v >> 16) & 255) >= 127 && ((v >> 16) & 255) <= 129 && (v & 255) == 0 && ((v >> 8) & 255) == 0);
    }
    if (slot1) {
        assert(lagfx_a64_exec_readback_texture(p, t, REF_TEX2, img, RT_W * RT_H * 8, &w, &h));
        if (mask1 == 0xfu && !g_blend.s1add) assert(half_at(img, 8, 8, 0) == 0 && half_at(img, 8, 8, 1) == 0x3c00 && half_at(img, 8, 8, 2) == 0 && half_at(img, 8, 8, 3) == 0x3c00);
        else if (mask1 == 0u && !g_blend.nomasks) assert(half_at(img, 8, 8, 1) == 0 && half_at(img, 8, 8, 3) == 0x3c00);   /* clear colour (0,0,0,1) stays */
    }
    if (slot1 && g_blend.s1add && g_dev->vk->have_independent_blend) assert(half_at(img, 8, 8, 0) == 0x3800 && half_at(img, 8, 8, 1) == 0x3c00);
    if (slot1 && g_blend.s1add && !g_dev->vk->have_independent_blend) assert(half_at(img, 8, 8, 0) == 0 && R->blend_indep_mismatch >= 1);   /* attachment 0's (off) state applies, and the difference is counted */   /* additive on slot 1 only: 0.5 + 0 red kept, green 1 */
    if (slot1 && g_blend.nomasks) assert(half_at(img, 8, 8, 1) == 0x3c00);   /* masks unknown: every slot writes */
    if (slot0 && slot1) assert(R->multi_attach >= 1);
    if (!slot0) assert(R->multi_attach >= 1 && R->submit_failed == 0);
    assert(R->submit_failed == 0 && R->draws == 1);
    lagfx_a64_exec_drop_task(p, 1);
    lagfx_a64_exec_free(p);
    free(p);
    g_blend.set = 0, g_blend.mrt = 0, g_res_tex2 = false;
    (void)name;
}

static void test_exec_fail_cache(void) {
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    def_task(p, 1); build_objects();
    lagfx_a64_exec_set_pipeline_resolver(p, resolver, NULL);
    g_blend.fail = 1;
    static const double c[4] = { 0, 0, 1, 1 };
    cb_reset(); cb_pass(REF_TEX, 2, c); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, 0); cb_draw(0, 3); cb_draw(0, 3); cb_draw(0, 3);
    exec(p, 1, cb_finish(), true);
    assert(p->diag2.exec.pipe.pipe_build_failed == 1);        /* built (and failed) once, the other two draws hit the failure cache */
    assert(p->diag2.exec.run.draws_skipped == 3 && p->diag2.exec.run.submit_failed == 0);
    lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); free(p);
    g_blend.fail = 0;
    puts("PASS: failed pipeline build is cached (one attempt for three draws)");
}

static void test_exec_fetch(void) {
    if (!g_dev->vk->have_local_read) {   /* LAGFX_VK_NO_LOCAL_READ=1 run: the draw is refused and counted, the clear survives */
        setenv("LAGFX_ARM64_EXEC", "1", 1);
        lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
        lagfx_task_entry_t *t = def_task(p, 1);
        build_objects();
        lagfx_a64_exec_set_pipeline_resolver(p, resolver, NULL);
        g_blend.fetch = 1;
        static const double blue[4] = { 0, 0, 1, 1 };
        cb_reset(); cb_pass(REF_TEX, 2, blue); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, 0); cb_draw(0, 3);
        exec(p, 1, cb_finish(), true);
        uint8_t img[RT_W * RT_H * 4]; uint32_t w, h;
        assert(p->diag2.exec.pipe.stub_fbfetch == 1 && p->diag2.exec.run.draws_skipped == 1 && p->diag2.exec.run.submit_failed == 0);
        assert(lagfx_a64_exec_readback_texture(p, t, REF_TEX, img, sizeof img, &w, &h) && px(img, 8, 8) == 0xff0000ffu);
        lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); free(p);
        g_blend.fetch = 0;
        puts("PASS: framebuffer fetch without local read: draw refused and counted, clear kept");
        return;
    }
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    lagfx_task_entry_t *t = def_task(p, 1);
    build_objects();
    lagfx_a64_exec_set_pipeline_resolver(p, resolver, NULL);
    g_blend.fetch = 1;
    static const double blue[4] = { 0, 0, 1, 1 };
    cb_reset(); cb_pass(REF_TEX, 2, blue); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, 0); cb_draw(0, 3); cb_draw(0, 3);
    exec(p, 1, cb_finish(), true);
    uint8_t img[RT_W * RT_H * 4]; uint32_t w, h;
    __typeof__(p->diag2.exec.run) *R = &p->diag2.exec.run;
    assert(R->submit_failed == 0 && R->draws == 2);
    assert(lagfx_a64_exec_readback_texture(p, t, REF_TEX, img, sizeof img, &w, &h));
    uint32_t v = px(img, 8, 8);   /* clear (0,0,1,1) read back by the FS, +0.5 red per draw: two draws -> red saturates at 1.0 (second draw reads the first draw's result) */
    assert(((v >> 16) & 255) == 255 && ((v >> 8) & 255) == 0 && (v & 255) == 255);
    assert(px(img, 0, 15) == 0xff0000ffu);   /* outside the triangle: the clear colour */
    assert(p->diag2.exec.pipe.fbfetch_draws == 2);
    lagfx_a64_exec_drop_task(p, 1);
    lagfx_a64_exec_free(p);
    free(p);
    g_blend.fetch = 0;
    {   /* ONE indexed draw of two overlapping identical triangles: the second fragment must read what the first wrote (0.5 -> 1.0 red) */
        lagfx_protocol_t *q = mk(LAGFX_PROTO_ARM64_VMAPPLE);
        lagfx_task_entry_t *tq = def_task(q, 1);
        build_objects();
        lagfx_a64_exec_set_pipeline_resolver(q, resolver, NULL);
        g_blend.fetch = 1;
        cb_reset(); cb_pass(REF_TEX, 2, blue); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, 0); cb_draw_indexed(REF_IB, 6);
        exec(q, 1, cb_finish(), true);
        assert(q->diag2.exec.run.submit_failed == 0 && q->diag2.exec.run.draws == 1);
        assert(lagfx_a64_exec_readback_texture(q, tq, REF_TEX, img, sizeof img, &w, &h));
        uint32_t v2 = px(img, 8, 8);
        assert(((v2 >> 16) & 255) == 255 && (v2 & 255) == 255);
        lagfx_a64_exec_drop_task(q, 1); lagfx_a64_exec_free(q); free(q);
        g_blend.fetch = 0;
    }
    /* two attachments: BGRA8 slot 0 + RGBA16F slot 1, FS reads BOTH (dest_0 / dest_1 told apart) and writes both */
    {
        lagfx_protocol_t *q = mk(LAGFX_PROTO_ARM64_VMAPPLE);
        lagfx_task_entry_t *tq = def_task(q, 1);
        build_objects();
        lagfx_a64_exec_set_pipeline_resolver(q, resolver, NULL);
        g_blend.fetch = 2; g_blend.set = 0; g_res_tex2 = true;
        const uint32_t refs[2] = { REF_TEX, REF_TEX2 };
        const uint16_t loads[2] = { 2, 2 };
        const double clr[2][4] = { { 0, 0, 1, 1 }, { 0, 1, 0, 1 } };
        cb_reset(); cb_pass_n(refs, loads, clr); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, 0); cb_draw(0, 3);
        exec(q, 1, cb_finish(), true);
        __typeof__(q->diag2.exec.run) *R2 = &q->diag2.exec.run;
        assert(R2->submit_failed == 0 && R2->draws == 1);
        assert(lagfx_a64_exec_readback_texture(q, tq, REF_TEX, img, sizeof img, &w, &h));
        assert(px(img, 8, 8) == GREEN);                         /* slot 0 now holds what slot 1 had (0,1,0,1) */
        uint8_t img8[RT_W * RT_H * 8];
        assert(lagfx_a64_exec_readback_texture(q, tq, REF_TEX2, img8, sizeof img8, &w, &h));
        assert(half_at(img8, 8, 8, 0) == 0x3800 && half_at(img8, 8, 8, 1) == 0 && half_at(img8, 8, 8, 2) == 0x3c00 && half_at(img8, 8, 8, 3) == 0x3c00);   /* slot 0's blue + 0.5 red, FP16 preserved */
        lagfx_a64_exec_drop_task(q, 1); lagfx_a64_exec_free(q); free(q);
        g_blend.fetch = 0; g_res_tex2 = false;
    }
    puts("PASS: framebuffer fetch (input attachment 0 = colour attachment 0, two draws read each other's output) on lavapipe");
}

static void test_exec_mrt(void) {
    mrt_case("both attachments written", true, true, 0xfu, 0xfu);
    mrt_case("attachment 1 write mask 0", true, true, 0xfu, 0u);
    mrt_case("only slot 1 attached (slot 0 unused)", false, true, 0xfu, 0xfu);
    g_blend.s1add = 1; mrt_case("slot 1 own blend (additive) while slot 0 is off", true, true, 0xfu, 0xfu); g_blend.s1add = 0;
    g_blend.nomasks = 1; mrt_case("masks not decoded: all slots write", true, true, 0u, 0u); g_blend.nomasks = 0;
    {   /* device without independentBlend: attachment 1 differs -> counted, not silently hidden */
        const uint8_t saved = g_dev->vk->have_independent_blend;
        g_dev->vk->have_independent_blend = 0;
        g_blend.s1add = 1;
        setenv("LAGFX_ARM64_EXEC", "1", 1);
        mrt_case("independentBlend off, differing slot 1", true, true, 0xfu, 0xfu);
        g_blend.s1add = 0; g_dev->vk->have_independent_blend = saved;
    }
    puts("PASS: two colour attachments (BGRA8 + RGBA16Float, original slots, per-slot write masks, unused slot 0) on lavapipe");
}

int main(void) {
    g_mem = calloc(1, MEM_SIZE);
    g_dev = make_device();
    assert(g_dev);
    test_objects();
#ifdef LAGFX_HAVE_VULKAN
    if (g_dev->vk && g_dev->vk->initialized) {
        test_exec_disabled();
        test_exec_render();
        test_exec_blend();
        test_exec_mrt();
        test_exec_fail_cache();
        test_exec_fetch();
        test_exec_no_resolver_and_pending();
        if (g_vs) vkDestroyShaderModule(g_dev->vk->device, g_vs, NULL);
        if (g_fs) vkDestroyShaderModule(g_dev->vk->device, g_fs, NULL);
        if (g_fs_half) vkDestroyShaderModule(g_dev->vk->device, g_fs_half, NULL);
        if (g_fs_mrt) vkDestroyShaderModule(g_dev->vk->device, g_fs_mrt, NULL);
    } else {
        puts("SKIP: no Vulkan device; execution tests not run");
        lagfx_device_free(g_dev); free(g_mem);
        return 77;
    }
#else
    puts("SKIP: built without Vulkan");
    return 77;
#endif
    lagfx_device_free(g_dev);
    free(g_mem);
    return 0;
}
