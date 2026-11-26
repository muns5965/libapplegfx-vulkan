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
#define VPN_IB    11u                /* index data page */
#define REF_TEX 1u
#define REF_VB  2u
#define REF_PIPE 3u
#define REF_IB  4u
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
    /* texture: {length, page}, layout (mips = 1), {tag, size, ref}, v1 description (2D, BGRA8 = 80, w x h, 1 sample) */
    uint8_t t[200]; memset(t, 0, sizeof t);
    t[12] = 1;                                   /* mips */
    uint32_t rec = 12 + 0x18 + 36, tag = 0x14;
    memcpy(t + rec, &tag, 4);
    uint8_t *d = t + rec + 12;
    d[0] = 2; d[2] = 80;
    uint32_t w = RT_W, h = RT_H, one = 1;
    memcpy(d + 4, &w, 4); memcpy(d + 8, &h, 4); memcpy(d + 12, &one, 4);
    d[16] = 1; d[18] = 1;
    uint32_t tsize = rec + 12 + 24;
    va_write(VA_PAY, t, tsize);
    entry(REF_TEX, LAGFX_A64_OBJ_TEXTURE, tsize, VA_PAY);
    /* vertex buffer: 4 vertices (float2). 0..2 a big triangle, 3 spare. */
    const float v[8] = { -0.9f, -0.9f, 0.9f, -0.9f, 0.0f, 0.9f, 0.0f, 0.0f };
    va_write((uint64_t)VPN_VB << 14, v, sizeof v);
    put_buffer_object(REF_VB, 0x200, 0x4000, VPN_VB);
    const uint16_t idx[4] = { 0, 1, 2, 0 };
    va_write((uint64_t)VPN_IB << 14, idx, sizeof idx);
    put_buffer_object(REF_IB, 0x220, 0x4000, VPN_IB);
    uint8_t pl[16] = {0}; uint32_t ptag = 0xe; memcpy(pl, &ptag, 4);
    va_write(VA_PAY + 0x300, pl, 16);
    entry(REF_PIPE, LAGFX_A64_OBJ_SERIALIZED, 16, VA_PAY + 0x300);
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
static void exec(lagfx_protocol_t *p, uint32_t task, uint32_t len, bool with_res) {
    uint32_t w[3 + 8 + 4], n = 0;
    w[n++] = task; w[n++] = with_res ? 4 : 0; w[n++] = 1;
    if (with_res) { w[n++] = REF_TEX; w[n++] = 2; w[n++] = REF_VB; w[n++] = 1; w[n++] = REF_PIPE; w[n++] = 0; w[n++] = REF_IB; w[n++] = 1; }
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
static VkShaderModule g_vs, g_fs;
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
    assert(R->tag_partial[114] == 1 && R->tag_unsup[99] == 1);
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

int main(void) {
    g_mem = calloc(1, MEM_SIZE);
    g_dev = make_device();
    assert(g_dev);
    test_objects();
#ifdef LAGFX_HAVE_VULKAN
    if (g_dev->vk && g_dev->vk->initialized) {
        test_exec_disabled();
        test_exec_render();
        test_exec_no_resolver_and_pending();
        if (g_vs) vkDestroyShaderModule(g_dev->vk->device, g_vs, NULL);
        if (g_fs) vkDestroyShaderModule(g_dev->vk->device, g_fs, NULL);
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
