/* Pipeline-build failure diagnostics: a graphics pipeline that the driver refuses (here lavapipe, by naming a non-existent entry point) logs ONE
 * `pipeline-build-fail:` line per distinct failure key (result name, module sizes + hashes, per-module vkCreateShaderModule verdict, vertex layout, colour format,
 * topology, dynamic rendering, caps mask), logs nothing for a success, and with LAGFX_ARM64_DUMP=1 + LAGFX_ARM64_DUMP_DIR writes the failing SPIR-V as
 * spv_<hash>_vs.spv / _fs.spv. lavapipe only; the Adreno VK_ERROR_UNKNOWN itself is NOT reproduced here.
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
#include "protocol/arm64_dump.h"
#include "vulkan/pipeline_build.h"
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
#include <dirent.h>

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


#define VA_PAY 0x4000u
#define VPN_VB 10u
#define VPN_CMD 16u
#define REF_TEX 1u
#define REF_VB 2u
#define REF_PIPE 3u
#define RT 16u
static void entry(uint32_t ref, uint32_t type, uint32_t size, uint64_t addr) {
    uint32_t e[3] = { type | size << 8, (uint32_t)addr, (uint32_t)(addr >> 32) };
    va_write(0x100u + ref * 12u, e, 12);
}
static void build_objects(void) {
    uint8_t t[220]; memset(t, 0, sizeof t);
    uint64_t tlen = RT * RT * 4u; uint32_t tvp = 12;
    memcpy(t, &tlen, 8); memcpy(t + 8, &tvp, 4); t[12] = 1;
    { uint8_t *mp = t + 12 + 0x18; uint64_t row = RT * 4u; uint32_t mw = RT, mh = RT, md = 1; memcpy(mp + 0x10, &row, 8); memcpy(mp + 0x18, &mw, 4); memcpy(mp + 0x1c, &mh, 4); memcpy(mp + 0x20, &md, 4); }
    uint32_t rec = 12 + 0x18 + 36, tag = 0x14; memcpy(t + rec, &tag, 4);
    uint8_t *d = t + rec + 12; d[0] = 2; d[2] = 80; uint32_t w = RT, h = RT, one = 1; memcpy(d + 4, &w, 4); memcpy(d + 8, &h, 4); memcpy(d + 12, &one, 4); d[16] = 1; d[18] = 1;
    uint32_t tsize = rec + 12 + 28;
    va_write(VA_PAY, t, tsize); entry(REF_TEX, 2, tsize, VA_PAY);
    const float v[8] = { -0.9f, -0.9f, 0.9f, -0.9f, 0.0f, 0.9f, 0.0f, 0.0f };
    va_write((uint64_t)VPN_VB << 14, v, sizeof v);
    { uint8_t h2[16] = {0}; uint64_t len = 0x4000; uint32_t vp = VPN_VB; memcpy(h2, &len, 8); memcpy(h2 + 8, &vp, 4); va_write(VA_PAY + 0x200, h2, 16); entry(REF_VB, 1, 16, VA_PAY + 0x200); }
    uint8_t pl[16] = {0}; uint32_t ptag = 0xe; memcpy(pl, &ptag, 4); va_write(VA_PAY + 0x300, pl, 16); entry(REF_PIPE, 7, 16, VA_PAY + 0x300);
}
static uint8_t g_cb[4096]; static uint32_t g_cbn;
static void cb_cmd(uint32_t tag, const void *payload, uint32_t n) {
    uint32_t sz = 8 + n; memcpy(g_cb + g_cbn, &tag, 4); memcpy(g_cb + g_cbn + 4, &sz, 4); if (n) memcpy(g_cb + g_cbn + 8, payload, n); g_cbn += sz;
}
static void run_draw(lagfx_protocol_t *p, uint32_t stamp) {
    g_cbn = 8; memset(g_cb, 0, sizeof g_cb);
    { uint8_t r[584]; memset(r, 0, sizeof r); uint32_t ref = REF_TEX; uint16_t load = 2, store = 1; double clr[4] = { 0, 1, 0, 1 };
      memcpy(r + 0x4c, &ref, 4); memcpy(r + 0x4c + 20, &load, 2); memcpy(r + 0x4c + 22, &store, 2); memcpy(r + 0x4c + 0x1c, clr, 32); cb_cmd(26, r, sizeof r); }
    { double v[6] = { 0, 0, RT, RT, 0, 1 }; cb_cmd(130, v, sizeof v); }
    { uint32_t pr = REF_PIPE; cb_cmd(116, &pr, 4); }
    { uint8_t r[20]; uint32_t idx = 0, cnt = 1, vb = REF_VB; uint64_t off = 0; memcpy(r, &idx, 4); memcpy(r + 4, &cnt, 4); memcpy(r + 8, &vb, 4); memcpy(r + 12, &off, 8); cb_cmd(125, r, 20); }
    { uint8_t r[8] = { 3, 0, 0, 0, 0, 0, 3, 0 }; cb_cmd(1, r, 8); }
    uint32_t len = g_cbn; memcpy(g_cb, &len, 4); va_write((uint64_t)VPN_CMD << 14, g_cb, len);
    uint32_t w[3 + 6 + 4], n = 0; w[n++] = 1; w[n++] = 3; w[n++] = 1; w[n++] = REF_TEX; w[n++] = 2; w[n++] = REF_VB; w[n++] = 1; w[n++] = REF_PIPE; w[n++] = 0;
    { uint64_t off = (uint64_t)VPN_CMD << 14, l = len; memcpy(w + n, &off, 8); n += 2; memcpy(w + n, &l, 8); n += 2; }
    submit(p, 0x43, stamp, w, n);
}

static VkShaderModule g_vs, g_fs;
static const char *g_entry = "nonexistent";    /* the failing entry-point name; "main"-like names of the shaders are found by lavapipe */
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
    out->desc.vertex_entry_point = g_entry, out->desc.fragment_entry_point = g_entry;
    out->desc.vertex_spv = k_vs_spv, out->desc.vertex_spv_bytes = sizeof k_vs_spv, out->desc.fragment_spv = k_fs_spv, out->desc.fragment_spv_bytes = sizeof k_fs_spv;
    out->desc.caps_enabled = vk->caps_enabled;
    out->desc.n_vtx_inputs = 1, out->desc.vtx_in_loc[0] = 0, out->desc.vtx_in_comp[0] = 2, out->desc.vtx_stride = 8;
    out->vertex_buffer_index = 0;
    out->release = rel;
    return true;
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    char *b = calloc(1, 1 << 17); size_t n = fread(b, 1, (1 << 17) - 1, f); b[n] = 0; fclose(f); return b;
}
static int count(const char *hay, const char *needle) { int c = 0; for (const char *q = hay; (q = strstr(q, needle)); q += strlen(needle)) c++; return c; }

int main(void) {
    char logf[64]; snprintf(logf, sizeof logf, "/tmp/lagfx-pipefail-XXXXXX");
    int lfd = mkstemp(logf); assert(lfd >= 0); close(lfd);
    setenv("LAGFX_LOG_FILE", logf, 1);
    g_mem = calloc(1, MEM_SIZE);
    g_dev = make_device();
    assert(g_dev);
    if (!g_dev->vk || !g_dev->vk->initialized) { puts("SKIP: no Vulkan device"); lagfx_device_free(g_dev); free(g_mem); return 77; }
    struct lagfx_vk_state *vk = g_dev->vk;

    /* (1) the builder directly: success logs nothing, a refused pipeline logs once per distinct key */
    VkShaderModule vs, fs;
    { VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof k_vs_spv, .pCode = k_vs_spv };
      assert(vkCreateShaderModule(vk->device, &ci, NULL, &vs) == VK_SUCCESS);
      ci.codeSize = sizeof k_fs_spv, ci.pCode = k_fs_spv; assert(vkCreateShaderModule(vk->device, &ci, NULL, &fs) == VK_SUCCESS); }
    lagfx_pipeline_desc_t d; memset(&d, 0, sizeof d);
    d.vertex_shader = vs, d.fragment_shader = fs, d.layout = vk->empty_layout, d.color_format = VK_FORMAT_B8G8R8A8_UNORM;
    d.vertex_spv = k_vs_spv, d.vertex_spv_bytes = sizeof k_vs_spv, d.fragment_spv = k_fs_spv, d.fragment_spv_bytes = sizeof k_fs_spv, d.caps_enabled = vk->caps_enabled;
    d.n_vtx_inputs = 1, d.vtx_in_loc[0] = 0, d.vtx_in_comp[0] = 2, d.vtx_stride = 8;
    VkPipeline pl = VK_NULL_HANDLE;
    d.vertex_entry_point = d.fragment_entry_point = "main";
    assert(lagfx_pipeline_build(vk->device, &d, &pl) == LAGFX_OK && pl);
    vkDestroyPipeline(vk->device, pl, NULL);
    char *log = slurp(logf);
    assert(log && count(log, "pipeline-build-fail:") == 0);          /* a success logs nothing */
    free(log);
    d.vertex_entry_point = d.fragment_entry_point = "nonexistent";
    for (int i = 0; i < 3; i++) { pl = VK_NULL_HANDLE; lagfx_status_t st = lagfx_pipeline_build(vk->device, &d, &pl); assert(st == LAGFX_ERR_BACKEND && !pl); }
    log = slurp(logf);
    assert(count(log, "pipeline-build-fail:") == 1);                  /* three identical failures: one line */
    assert(strstr(log, "result=VK_ERROR_") && strstr(log, "vs[bytes=") && strstr(log, "module=accepted") && strstr(log, "vertex_bindings=1 vertex_attrs=1[loc0:fmt103@0]")
           && strstr(log, "color_format=44") && strstr(log, "dynamic_rendering=1") && strstr(log, "caps_mask=0x"));
    free(log);
    d.topology_plus1 = VK_PRIMITIVE_TOPOLOGY_LINE_LIST + 1u;          /* another key */
    assert(lagfx_pipeline_build(vk->device, &d, &pl) == LAGFX_ERR_BACKEND);
    log = slurp(logf);
    assert(count(log, "pipeline-build-fail:") == 2 && strstr(log, "topology=1"));
    free(log);
    for (int i = 0; i < 12; i++) { d.color_format = (VkFormat)(VK_FORMAT_R8G8B8A8_UNORM + i * 3); (void)lagfx_pipeline_build(vk->device, &d, &pl); }   /* many keys: bounded */
    log = slurp(logf);
    assert(count(log, "pipeline-build-fail:") == 9 && count(log, "further failures are not logged") == 1);   /* 8 distinct keys + the single 'suppressed' line */
    free(log);
    vkDestroyShaderModule(vk->device, vs, NULL); vkDestroyShaderModule(vk->device, fs, NULL);
    printf("PASS: builder: success silent, one line per distinct failure key, bounded at 8 keys\n");

    /* (2) through an arm64 exec with LAGFX_ARM64_DUMP: the failing SPIR-V is written, the draw is skipped and counted, the stamp completes */
    char dir[64]; snprintf(dir, sizeof dir, "/tmp/lagfx-spv-XXXXXX"); assert(mkdtemp(dir));
    setenv("LAGFX_ARM64_EXEC", "1", 1); setenv("LAGFX_ARM64_DUMP", "1", 1); setenv("LAGFX_ARM64_DUMP_DIR", dir, 1);
    lagfx_a64_dump_reset();
    lagfx_protocol_t *p = mk();
    def_task(p, 1); build_objects();
    lagfx_a64_exec_set_pipeline_resolver(p, resolver, NULL);
    run_draw(p, 1);
    assert(cell(1) == 1 && p->diag2.exec.pipe.pipe_build_failed == 1 && p->diag2.exec.run.draws == 0 && p->diag2.exec.run.draws_skipped >= 1);
    run_draw(p, 2);                                                   /* the same failure again: no second capture */
    assert(cell(1) == 2);
    lagfx_a64_dump_reset();
    DIR *dd = opendir(dir); struct dirent *de; int nvs = 0, nfs = 0; char vsn[300] = "", fsn[300] = "";
    while (dd && (de = readdir(dd))) {
        size_t l = strlen(de->d_name);
        if (!strncmp(de->d_name, "spv_", 4) && l > 7 && !strcmp(de->d_name + l - 7, "_vs.spv")) nvs++, snprintf(vsn, sizeof vsn, "%s/%s", dir, de->d_name);
        if (!strncmp(de->d_name, "spv_", 4) && l > 7 && !strcmp(de->d_name + l - 7, "_fs.spv")) nfs++, snprintf(fsn, sizeof fsn, "%s/%s", dir, de->d_name);
    }
    if (dd) closedir(dd);
    assert(nvs == 1 && nfs == 1);
    { FILE *f = fopen(vsn, "rb"); uint8_t b[sizeof k_vs_spv + 8]; size_t n = fread(b, 1, sizeof b, f); fclose(f); assert(n == sizeof k_vs_spv && !memcmp(b, k_vs_spv, n)); }
    { FILE *f = fopen(fsn, "rb"); uint8_t b[sizeof k_fs_spv + 8]; size_t n = fread(b, 1, sizeof b, f); fclose(f); assert(n == sizeof k_fs_spv && !memcmp(b, k_fs_spv, n)); }
    char ip[128]; snprintf(ip, sizeof ip, "%s/index.txt", dir);
    char *idx = slurp(ip); assert(idx && count(idx, "spv vs=spv_") == 1); free(idx);
    printf("PASS: failing SPIR-V captured as spv_<hash>_vs.spv / _fs.spv (byte-exact, once)\n");
    log = slurp(logf); assert(count(log, "pipeline-build-fail:") >= 9); free(log);   /* (the exec's failure key was new or already counted: never crashes) */

    lagfx_arm64_cq_shutdown(p, 0);
    lagfx_a64_exec_free(p);
    free(p);
    if (g_vs) vkDestroyShaderModule(vk->device, g_vs, NULL);
    if (g_fs) vkDestroyShaderModule(vk->device, g_fs, NULL);
    lagfx_device_free(g_dev);
    free(g_mem);
    puts("PASS: pipeline build failure diagnostics");
    return 0;
}
