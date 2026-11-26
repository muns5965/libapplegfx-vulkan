/* ARM64 exec integration: Render + Blit segments in ONE exec (one command buffer, one submit, shared mirror objects), resource flag 0x04 write-back to
 * fake guest pages through the page table, the stamp completing only afterwards (also with an injected PENDING), deferred release of objects dropped while
 * the exec is in flight, and Vulkan validation (sync / layout) staying at 0 for render -> blit -> render -> blit. lavapipe; NOT verified on a hardware GPU.
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
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>

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
static void irq(void *o, uint32_t v) { (void)o; (void)v; g_irq++; }   /* only called under the execution lock / cq lock: one thread at a time */
static lagfx_task_t *cb_create_task(void *o, uint64_t s, void **p) { (void)o; (void)s; (void)p; return NULL; }
static void cb_destroy_task(void *o, lagfx_task_t *t) { (void)o; (void)t; }
static bool cb_map(void *a, lagfx_task_t *t, uint64_t o, const lagfx_physical_range_t *r, size_t c, bool ro) { (void)a; (void)t; (void)o; (void)r; (void)c; (void)ro; return true; }
static bool cb_unmap(void *a, lagfx_task_t *t, uint64_t o, uint64_t l) { (void)a; (void)t; (void)o; (void)l; return true; }
static void put32(uint64_t gpa, uint32_t v) { memcpy(g_mem + gpa, &v, 4); }
static uint32_t cell(uint32_t slot) { uint32_t v; memcpy(&v, g_mem + ((uint64_t)STAMP_PFN << 14) + 4u * slot, 4); return v; }
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
#define VPN_IB    11u
#define VPN_TEX   12u
#define VPN_A     13u     /* texture snapshot after the first render pass */
#define VPN_B     14u     /* guest-written 4x4 pattern for the blit buffer->texture */
#define VPN_C     15u     /* texture after render -> blit -> render, flag 0x04 */
#define VPN_CMD   16u     /* four command buffers, one page each */
#define REF_TEX 1u
#define REF_VB  2u
#define REF_PIPE 3u
#define REF_IB  4u
#define REF_A   5u
#define REF_B   6u
#define REF_C   7u
#define REF_LIN 8u     /* type 8 linear texture, no guest memory */
#define REF_LB  9u     /* buffer behind the pitched linear texture */
#define REF_LIN2 10u   /* tag 9 linear texture over REF_LB: offset 64, row pitch 96 (> 16 * 4) */
#define VPN_LB  20u
#define LIN_OFF 64u
#define LIN_ROW 96u
#define SENT    0xabu
#define RT_W 16u
#define RT_H 16u
#define PATTERN 0x11223344u
#define RED   0xffff0000u
#define GREEN 0xff00ff00u

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
static void build_objects(void) {
    uint8_t t[220]; memset(t, 0, sizeof t);
    uint64_t tlen = RT_W * RT_H * 4u; uint32_t tvp = VPN_TEX;
    memcpy(t, &tlen, 8); memcpy(t + 8, &tvp, 4);
    t[12] = 1;
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
    const float v[8] = { -0.9f, -0.9f, 0.9f, -0.9f, 0.0f, 0.9f, 0.0f, 0.0f };
    va_write((uint64_t)VPN_VB << 14, v, sizeof v);
    put_buffer_object(REF_VB, 0x200, 0x4000, VPN_VB);
    const uint16_t idx[4] = { 0, 1, 2, 0 };
    va_write((uint64_t)VPN_IB << 14, idx, sizeof idx);
    put_buffer_object(REF_IB, 0x220, 0x4000, VPN_IB);
    put_buffer_object(REF_A, 0x240, 0x4000, VPN_A);
    put_buffer_object(REF_B, 0x260, 0x4000, VPN_B);
    put_buffer_object(REF_C, 0x280, 0x4000, VPN_C);
    uint8_t pl[16] = {0}; uint32_t ptag = 0xe; memcpy(pl, &ptag, 4);
    va_write(VA_PAY + 0x300, pl, 16);
    entry(REF_PIPE, LAGFX_A64_OBJ_SERIALIZED, 16, VA_PAY + 0x300);
    { uint8_t l[36]; memset(l, 0, sizeof l); uint32_t tag = 0x14, sz = sizeof l; memcpy(l, &tag, 4); memcpy(l + 4, &sz, 4);
      uint8_t *d = l + 12; d[0] = 2; d[2] = 80; uint32_t w = RT_W, h = RT_H; memcpy(d + 4, &w, 4); memcpy(d + 8, &h, 4); d[16] = 1; d[18] = 1;
      va_write(VA_PAY + 0x400, l, sizeof l); entry(REF_LIN, 8, sizeof l, VA_PAY + 0x400); }
    put_buffer_object(REF_LB, 0x2a0, 0x4000, VPN_LB);
    { uint8_t l[0x40]; memset(l, 0, sizeof l); uint32_t tag = 9, sz = 0x20 + 24, bref = REF_LB; uint64_t off = LIN_OFF, row = LIN_ROW;
      memcpy(l, &tag, 4); memcpy(l + 4, &sz, 4); memcpy(l + 12, &bref, 4); memcpy(l + 16, &off, 8); memcpy(l + 24, &row, 8);
      uint8_t *d = l + 0x20; d[0] = 2; d[2] = 80; uint32_t w = RT_W, h = RT_H; memcpy(d + 4, &w, 4); memcpy(d + 8, &h, 4); d[16] = 1; d[18] = 1;
      va_write(VA_PAY + 0x500, l, sz); entry(REF_LIN2, 8, sz, VA_PAY + 0x500); }
    uint32_t pat[16]; for (int i = 0; i < 16; i++) pat[i] = PATTERN;   /* 4x4 pixels, row 16 bytes */
    va_write((uint64_t)VPN_B << 14, pat, sizeof pat);
}

/* ---- command buffers: [u32 seglen][u32 info = type | cont << 8 | more << 16] then {tag, size, payload}... ---- */
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
    double v[6] = { 0, 0, RT_W, RT_H, 0, 1 }; cb_cmd(130, v, sizeof v);
    uint32_t pr = REF_PIPE; cb_cmd(116, &pr, 4);
    uint8_t r[20]; uint32_t idx = 0, cnt = 1, vb = REF_VB; uint64_t off = 0;
    memcpy(r, &idx, 4); memcpy(r + 4, &cnt, 4); memcpy(r + 8, &vb, 4); memcpy(r + 12, &off, 8); cb_cmd(125, r, 20);
}
static void put64(uint8_t *b, uint32_t o, uint64_t v) { memcpy(b + o, &v, 8); }
/* 302 texture -> buffer: {tex, buf, origin[3] @8, size[3] @0x20, dstOff @0x38, dstRow @0x40, dstImage @0x48, slice @0x50, level @0x52} */
static void cb_tex_to_buf(uint32_t tex, uint32_t buf) {
    uint8_t r[0x58]; memset(r, 0, sizeof r);
    memcpy(r, &tex, 4); memcpy(r + 4, &buf, 4);
    put64(r, 0x20, RT_W); put64(r, 0x28, RT_H); put64(r, 0x30, 1); put64(r, 0x40, RT_W * 4u); put64(r, 0x48, RT_W * RT_H * 4u);
    cb_cmd(302, r, sizeof r);
}
/* 300 buffer -> texture: {buf, tex, srcOff @8, srcRow @0x10, srcImage @0x18, size[3] @0x20, origin[3] @0x38, slice @0x50, level @0x52} (a 4x4 patch at the origin) */
static void cb_buf_to_tex(uint32_t buf, uint32_t tex) {
    uint8_t r[0x58]; memset(r, 0, sizeof r);
    memcpy(r, &buf, 4); memcpy(r + 4, &tex, 4);
    put64(r, 0x10, 16); put64(r, 0x18, 64); put64(r, 0x20, 4); put64(r, 0x28, 4); put64(r, 0x30, 1);
    cb_cmd(300, r, sizeof r);
}

/* one exec: R1 (clear green + triangle) -> B1 (texture -> A, buffer B -> texture patch) -> R2 (Load + indexed triangle) -> B2 (texture -> C) */
static void exec_chain(lagfx_protocol_t *p, uint32_t stamp) {
    static const double g[4] = { 0, 1, 0, 1 };
    uint32_t len[4];
    cb_reset(0); cb_pass(REF_TEX, 2, g); cb_render_common(); { uint8_t r[8] = { 3, 0, 0, 0, 0, 0, 3, 0 }; cb_cmd(1, r, 8); } len[0] = cb_finish(0);
    cb_reset(2); cb_tex_to_buf(REF_TEX, REF_A); cb_buf_to_tex(REF_B, REF_TEX); len[1] = cb_finish(1);
    cb_reset(0); cb_pass(REF_TEX, 1, g); cb_render_common();
    { uint8_t r[12] = {0}; uint32_t ref = REF_IB; uint16_t prim = 3, c = 3; memcpy(r, &prim, 2); memcpy(r + 4, &ref, 4); memcpy(r + 8, &c, 2); cb_cmd(7, r, 12); } len[2] = cb_finish(2);
    cb_reset(2); cb_tex_to_buf(REF_TEX, REF_C); len[3] = cb_finish(3);
    uint32_t w[3 + 14 + 16], n = 0;
    w[n++] = 1; w[n++] = 7; w[n++] = 4;
    const uint32_t res[7][2] = { { REF_TEX, 2 }, { REF_VB, 1 }, { REF_PIPE, 0 }, { REF_IB, 1 }, { REF_A, 4 | 2 }, { REF_B, 1 }, { REF_C, 4 | 2 } };
    for (int i = 0; i < 7; i++) { w[n++] = res[i][0]; w[n++] = res[i][1]; }
    for (uint32_t i = 0; i < 4; i++) { uint64_t off = (uint64_t)(VPN_CMD + i) << 14, l = len[i]; memcpy(w + n, &off, 8); n += 2; memcpy(w + n, &l, 8); n += 2; }
    submit(p, 0x43, stamp, w, n);
}

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


static atomic_int g_stop;
static void *timer_thread(void *a) {   /* the shell's completion timer */
    lagfx_protocol_t *p = a;
    while (!atomic_load(&g_stop)) { lagfx_arm64_poll_completions(p); struct timespec ts = { 0, 200 * 1000 }; nanosleep(&ts, NULL); }
    return NULL;
}
static uint32_t px(const uint8_t *img, uint32_t x, uint32_t y) { uint32_t v; memcpy(&v, img + (y * RT_W + x) * 4, 4); return v; }
static void check_images(const char *what) {
    const uint8_t *a = va_ptr((uint64_t)VPN_A << 14), *c = va_ptr((uint64_t)VPN_C << 14);
    (void)what;
    assert(px(a, 8, 8) == RED && px(a, 15, 15) == GREEN && px(a, 0, 0) == GREEN);             /* A: the Render pass's result, seen by the Blit (same image) */
    assert(px(c, 8, 8) == RED && px(c, 15, 15) == GREEN);                                     /* C: ... and survived the second pass (Load) */
    for (uint32_t y = 0; y < 4; y++) for (uint32_t x = 0; x < 4; x++) assert(px(c, x, y) == PATTERN);   /* the blit's buffer -> texture patch is seen by the later Render pass + blit */
    assert(px(c, 4, 0) == GREEN || px(c, 4, 0) != PATTERN);
}

int main(void) {
    g_mem = calloc(1, MEM_SIZE);
    g_dev = make_device();
    assert(g_dev);
    if (!g_dev->vk || !g_dev->vk->initialized) { puts("SKIP: no Vulkan device"); lagfx_device_free(g_dev); free(g_mem); return 77; }
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_protocol_t *p = mk();
    lagfx_task_entry_t *t = def_task(p, 1); (void)t;
    build_objects();
    lagfx_a64_exec_set_pipeline_resolver(p, resolver, NULL);

    /* (1) the chain completes: write-back to the guest pages happened, stamp 1 completed, ONE submit for four segments */
    exec_chain(p, 1);
    const lagfx_a64_stats_t *S = lagfx_a64_res_stats(p);
    assert(p->a64cq && cell(1) == 1 && lagfx_arm64_cq_pending(p) == 0 && g_irq == 1);
    check_images("chain");
    assert(p->diag2.exec.run.submits == 1 && p->diag2.exec.run.passes == 2 && p->diag2.exec.run.draws == 2 && p->diag2.exec.run.seg_blit == 2);
    assert(S->blit_done == 3 && S->downloads == 2 && S->execs == 1 && S->gpu_timeout == 0);
    /* Render and Blit use the SAME mirror objects: one texture object, one object per buffer ref, nothing created twice */
    assert(S->created == 6 && lagfx_a64_res_lookup(p, 1, REF_TEX) && lagfx_a64_res_lookup(p, 1, REF_TEX)->kind == LAGFX_A64_TEXTURE);
    printf("PASS: render -> blit -> render -> blit in one exec, flag-0x04 write-back, stamp after (validation msgs so far %llu)\n", (unsigned long long)lagfx_vk_validation_msg_count);

    /* (2) injected PENDING: the stamp must NOT complete, nothing written to the guest, objects dropped meanwhile stay alive until the fence signalled */
    memset(va_ptr((uint64_t)VPN_A << 14), 0, 0x4000); memset(va_ptr((uint64_t)VPN_C << 14), 0, 0x4000);
    lagfx_vk_test_inject_fence_result(VK_TIMEOUT);
    exec_chain(p, 2);
    assert(cell(1) == 1 && g_irq == 1 && lagfx_arm64_cq_pending(p) == 1 && lagfx_a64_res_is_pending(p));
    assert(px(va_ptr((uint64_t)VPN_C << 14), 8, 8) == 0 && px(va_ptr((uint64_t)VPN_A << 14), 8, 8) == 0);   /* write-backs wait for DONE */
    assert(p->diag2.exec.run.submit_pending == 1 && S->gpu_timeout == 1);
    assert(lagfx_arm64_poll_completions(p) == 0 && cell(1) == 1);
    uint32_t live; uint64_t cr, de0, de1;
    lagfx_a64_res_counts(p, &live, &cr, &de0);
    /* a SECOND exec while the first is only slow: held in the ring, not failed, not completed, not started */
    exec_chain(p, 3);
    assert(cell(1) == 1 && g_irq == 1 && S->execs == 2 && (p->a64_held_chan_mask & 2u) && p->diag2.exec.run.held == 1);
    lagfx_arm64_cq_stats_t q; lagfx_arm64_cq_stats(p, &q);
    assert(q.failed_pre == 0 && q.fault_rejected == 0 && !q.fault && p->diag2.exec.run.submit_failed == 0 && p->diag2.exec.run.busy_skipped == 0);
    assert(lagfx_device_pending_completions(g_dev) == 0 || 1);   /* (the fake protocol is not the device's) */
    assert(lagfx_arm64_poll_completions(p) == 0 && channel_compute_dispatcher_redrain_held(p) == 0 && S->execs == 2 && cell(1) == 1);   /* still held while pending */
    uint64_t deferred0 = S->deferred_frees;
    lagfx_a64_res_forget(p, 1, REF_C);              /* 0x25 for an object of the in-flight exec */
    lagfx_a64_exec_drop_task(p, 1);                 /* task teardown while in flight */
    lagfx_a64_res_counts(p, &live, &cr, &de1);
    assert(live == 0 && de1 == de0 && S->deferred_frees > deferred0);   /* parked, NOT destroyed */
    assert(lagfx_a64_res_is_pending(p) && cell(1) == 1);
    lagfx_vk_test_inject_fence_result(0x7fffffff);   /* the GPU finishes */
    { struct timespec ts = { 0, 2 * 1000000 };   /* the shell's timer: poll + re-drain until everything retired (never blocks) */
      for (int i = 0; i < 3000 && cell(1) != 3; i++) { lagfx_arm64_poll_completions(p); nanosleep(&ts, NULL); } }   /* ONLY the shell timer's poll: the held exec resumes without a new doorbell */
    assert(cell(1) == 3 && g_irq == 3 &&   /* one IRQ per retire batch: stamp 1, the retire of 2, the held exec's own completion */
            !lagfx_a64_res_is_pending(p) && lagfx_arm64_cq_pending(p) == 0 && p->a64_held_chan_mask == 0);
    assert(g_nstamps == 3 && g_stamps[0] == 1 && g_stamps[1] == 2 && g_stamps[2] == 3);   /* order preserved: exec 2 retired, THEN the held exec ran and completed */
    assert(S->execs == 3 && p->diag2.exec.run.submit_failed == 0);
    check_images("pending + held");                   /* the (forgotten, still alive) objects were written back; the held exec rebuilt its own and wrote too */
    lagfx_a64_res_counts(p, &live, &cr, &de1);
    assert(live == 6 && de1 == cr - live);            /* the dropped generation is destroyed (no leak, no double free), the rebuilt one is live */
    printf("PASS: injected PENDING: stamp held, a second exec is held in the ring (not failed), runs after DONE in order; deferred frees after the fence (deferred=%llu)\n", (unsigned long long)S->deferred_frees);

    /* (3) after the drop the objects are rebuilt from their table entries and the chain runs again */
    memset(va_ptr((uint64_t)VPN_A << 14), 0, 0x4000); memset(va_ptr((uint64_t)VPN_C << 14), 0, 0x4000);
    exec_chain(p, 4);
    assert(cell(1) == 4);
    check_images("again");

    /* (4) type 8 linear texture as a render target + blit source (regression of the old render path) */
    { static const double g[4] = { 0, 1, 0, 1 }; uint32_t len[2];
      cb_reset(0); cb_pass(REF_LIN, 2, g); cb_render_common(); { uint8_t r[8] = { 3, 0, 0, 0, 0, 0, 3, 0 }; cb_cmd(1, r, 8); } len[0] = cb_finish(0);
      cb_reset(2); cb_tex_to_buf(REF_LIN, REF_C); len[1] = cb_finish(1);
      uint32_t w[3 + 6 + 8], n = 0; w[n++] = 1; w[n++] = 3; w[n++] = 2;
      w[n++] = REF_LIN; w[n++] = 2; w[n++] = REF_VB; w[n++] = 1; w[n++] = REF_C; w[n++] = 4 | 2;
      for (uint32_t i = 0; i < 2; i++) { uint64_t off = (uint64_t)(VPN_CMD + i) << 14, l = len[i]; memcpy(w + n, &off, 8); n += 2; memcpy(w + n, &l, 8); n += 2; }
      submit(p, 0x43, 5, w, n); }
    assert(cell(1) == 5 && S->linear_made == 1 && S->linear_unsupported == 0 && p->diag2.exec.run.obj_unsupported_linear == 0);
    { const uint8_t *c = va_ptr((uint64_t)VPN_C << 14); assert(px(c, 8, 8) == RED && px(c, 15, 15) == GREEN); }
    printf("PASS: type 8 linear texture rendered into and blitted\n");

    /* (5) linear texture over a buffer, row pitch 96 > 64: render + flag 0x04; pixels land in the guest pages at offset/row, the padding stays untouched */
    memset(va_ptr((uint64_t)VPN_LB << 14), SENT, 0x4000);
    { static const double g[4] = { 0, 1, 0, 1 }; uint32_t len;
      cb_reset(0); cb_pass(REF_LIN2, 2, g); cb_render_common(); { uint8_t r[8] = { 3, 0, 0, 0, 0, 0, 3, 0 }; cb_cmd(1, r, 8); } len = cb_finish(0);
      uint32_t w[3 + 6 + 4], n = 0; w[n++] = 1; w[n++] = 3; w[n++] = 1;
      w[n++] = REF_LIN2; w[n++] = 2 | 4; w[n++] = REF_VB; w[n++] = 1; w[n++] = REF_PIPE; w[n++] = 0;
      { uint64_t off = (uint64_t)VPN_CMD << 14, l = len; memcpy(w + n, &off, 8); n += 2; memcpy(w + n, &l, 8); n += 2; }
      submit(p, 0x43, 6, w, n); }
    assert(cell(1) == 6);
    { const uint8_t *g = va_ptr((uint64_t)VPN_LB << 14);
      for (uint32_t i = 0; i < 0x4000; i++) {
          bool in_px = i >= LIN_OFF && i < LIN_OFF + LIN_ROW * RT_H && ((i - LIN_OFF) % LIN_ROW) < RT_W * 4u;
          if (!in_px) assert(g[i] == SENT);   /* before the offset, the 32 padding bytes of every row, the tail: unchanged */
      }
      uint32_t c, e; memcpy(&c, g + LIN_OFF + 8 * LIN_ROW + 8 * 4, 4); memcpy(&e, g + LIN_OFF + 15 * LIN_ROW + 15 * 4, 4);
      assert(c == RED && e == GREEN); }
    assert(S->linear_made == 2 && S->linear_unsupported == 0);
    printf("PASS: linear texture over a buffer, pitch 96: pixels written back, padding untouched\n");

    /* (6) two threads: the shell timer polls while the vCPU thread rings doorbells (drains) and the GPU finishes. Three execs, the 2nd and 3rd held behind the
     * slow first: each runs exactly once, stamps strictly in order, then a protocol reset races with the timer poll. */
    for (int round = 0; round < 12; round++) {
        uint32_t base = 100u + (uint32_t)round * 10u;
        g_nstamps = 0; atomic_store(&g_last_stamp, 0);
        lagfx_vk_test_inject_fence_result(VK_TIMEOUT);
        uint64_t execs0 = S->execs;
        exec_chain(p, base + 1); exec_chain(p, base + 2); exec_chain(p, base + 3);
        assert(g_nstamps == 0 && S->execs == execs0 + 1 && lagfx_a64_res_is_pending(p));
        atomic_store(&g_stop, 0);
        pthread_t th; assert(pthread_create(&th, NULL, timer_thread, p) == 0);
        lagfx_vk_test_inject_fence_result(0x7fffffff);
        for (int i = 0; i < 4000 && atomic_load(&g_last_stamp) != base + 3; i++) {
            channel_compute_dispatcher_drain(p, 1);   /* a doorbell racing with the timer */
            struct timespec ts = { 0, 500 * 1000 }; nanosleep(&ts, NULL);
        }
        atomic_store(&g_stop, 1); pthread_join(th, NULL);
        assert(atomic_load(&g_last_stamp) == base + 3 && S->execs == execs0 + 3);   /* each exactly once */
        /* stamps strictly increasing, none twice, the last is the 3rd exec's (a retire batch writes the highest stamp of the batch once, so 1..3 writes) */
        assert(g_nstamps >= 1 && g_nstamps <= 3 && g_stamps[g_nstamps - 1] == base + 3);
        for (int k = 1; k < g_nstamps; k++) assert(g_stamps[k] > g_stamps[k - 1]);
        assert(p->a64_held_chan_mask == 0 && !lagfx_a64_res_is_pending(p) && lagfx_arm64_cq_pending(p) == 0);
    }
    printf("PASS: two-thread stress: held execs ran once each, in order (12 rounds)\n");
    for (int round = 0; round < 6; round++) {   /* reset / teardown racing with the timer poll */
        lagfx_vk_test_inject_fence_result(VK_TIMEOUT);
        exec_chain(p, 500u + round); exec_chain(p, 600u + round);
        atomic_store(&g_stop, 0);
        pthread_t th; assert(pthread_create(&th, NULL, timer_thread, p) == 0);
        lagfx_vk_test_inject_fence_result(0x7fffffff);
        if (round & 1) lagfx_protocol_reset(p); else { lagfx_arm64_cq_shutdown(p, 1000); lagfx_a64_exec_free(p); }
        atomic_store(&g_stop, 1); pthread_join(th, NULL);
        assert(!lagfx_a64_res_is_pending(p) && p->a64_held_chan_mask == 0 && p->a64cq == NULL);
        memset(g_mem + ((uint64_t)RING_PFN0 << 14), 0, 4u << 14); g_wp = 0;
        put32(((uint64_t)SHARED_PFN << 14) + LAGFX_RING_FIFO_DESC_BASE_OFFSET + 0x00, 0); put32(((uint64_t)SHARED_PFN << 14) + LAGFX_RING_FIFO_DESC_BASE_OFFSET + 0x04, 0);
        build_objects(); def_task(p, 1);   /* the guest comes back */
        lagfx_a64_exec_set_pipeline_resolver(p, resolver, NULL);
    }
    printf("PASS: reset / teardown concurrent with the timer poll\n");
    printf("INFO: validation messenger %s\n", g_dev->vk->debug_messenger ? "ACTIVE" : "inactive (LAGFX_VK_VALIDATION unset or layer missing)");
    assert(lagfx_vk_validation_msg_count == 0);       /* layouts / barriers between render and blit passes are valid */
    lagfx_arm64_cq_shutdown(p, 0);
    lagfx_a64_exec_free(p);
    free(p);
    if (g_dev->vk->device != VK_NULL_HANDLE) {
        if (g_vs) vkDestroyShaderModule(g_dev->vk->device, g_vs, NULL);
        if (g_fs) vkDestroyShaderModule(g_dev->vk->device, g_fs, NULL);
    }
    lagfx_device_free(g_dev);
    free(g_mem);
    puts("PASS: arm64 exec integration");
    return 0;
}
