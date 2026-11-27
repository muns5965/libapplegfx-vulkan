/* ARM64 exec: render compatibility (framebuffer fetch ordering / formats / usage / blend / MSAA refusal), lavapipe + injected less-capable devices.
 * tests/arm64-render-compat-unit.c
 *
 * Reuses the harness of arm64-exec-run-unit.c (guest memory, objects, command buffers, readback) by including it with its main() renamed.
 * Everything here runs on lavapipe: it checks the recorded Vulkan against analytic pixel values and against a reference render, and it checks counters. It says NOTHING about
 * what an Adreno driver does (no Adreno here; lavapipe's llvmpipe also renders fragments of one draw in order, so a missing rasterization-order guarantee cannot show up as a wrong
 * pixel here — the framebuffer-fetch ordering fix is therefore asserted through what is RECORDED: sub-draw counters, plus the pixels as a no-regression check).
 * SPDX-License-Identifier: MIT */
#define main exec_run_unit_main
#include "arm64-exec-run-unit.c"
#undef main

#ifdef LAGFX_HAVE_VULKAN
#include "arm64-render-compat-shaders.h"
#include "vulkan/format_caps.h"
#include "protocol/diag.h"

extern uint64_t lagfx_vk_validation_msg_count;

/* ---- validation-message baseline (tests/fixtures/val_baseline_audit.txt) ----
 * The Vulkan validation layer reports a handful of messages on this harness that are NOT about the code under test (the framebuffer-fetch / raster-order paths record a barrier inside a
 * dynamic rendering instance, an input attachment is read through a layout the older layer does not know, ...). The audit tests (S04 S05 S06 D1) run with the layer on, so instead of
 * asserting "0 messages" (false failures) or nothing (what a missing check looks like), each window val_begin(); ...; val_end(label) compares the messages it saw with a BASELINE LIST:
 *   - a message whose key (VUID + the first words of its text, handles / numbers scrubbed) is not listed for `label` fails the test,
 *   - a listed message that occurs MORE often than baseline-count x (number of rc_run execs in the window) also fails ("count increase"; the baseline count is per exec, so adding draws to
 *     a test does not need a new baseline, a new message or a repeated one per exec does),
 *   - fewer is fine (and printed as a note).
 * LAGFX_VAL_BASELINE_PRINT=1 prints "VALKEY\t<label>\t<count>\t<execs>\t<key>" lines instead of checking (how the file is generated; merge the runs of all env variants taking the max).
 * No messages at all (layer absent or silent) always passes. */
extern void (*lagfx_vk_validation_hook)(const char *msg);
#define VAL_MAXK 48
static struct { char key[200]; unsigned n; } g_val_cur[VAL_MAXK]; static int g_val_ncur; static unsigned g_val_overflow, g_val_runs;   /* g_val_runs: rc_run() calls (execs) inside the window */
static void val_key(const char *m, char *out, size_t cap) {   /* "VUID | text after the last '|' " with 0x... and digit runs scrubbed, 110 chars of text */
    const char *v = strstr(m, "VUID-"); char vu[96] = "no-VUID"; size_t k = 0;
    if (v) { while (v[k] && v[k] != ' ' && v[k] != ']' && k < sizeof vu - 1) vu[k] = v[k], k++; vu[k] = 0; }
    const char *t = m, *q; while ((q = strstr(t, "| ")) != NULL) t = q + 2;   /* the text after the last "| " (after "MessageID = 0x... |") */
    char txt[130]; size_t n = 0;
    for (const char *c = t; *c && n < 110; c++) {
        if (c[0] == '0' && c[1] == 'x') { txt[n++] = '#'; c += 2; while ((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f')) c++; c--; continue; }
        if (*c >= '0' && *c <= '9') { if (n && txt[n - 1] == '#') continue; txt[n++] = '#'; continue; }
        txt[n++] = *c == '\t' ? ' ' : *c;
    }
    txt[n] = 0; snprintf(out, cap, "%s | %s", vu, txt);
}
static void val_hook(const char *m) {
    char key[200]; val_key(m, key, sizeof key);
    for (int i = 0; i < g_val_ncur; i++) if (!strcmp(g_val_cur[i].key, key)) { g_val_cur[i].n++; return; }
    if (g_val_ncur < VAL_MAXK) { memcpy(g_val_cur[g_val_ncur].key, key, sizeof key); g_val_cur[g_val_ncur++].n = 1; } else g_val_overflow++;
}
static void val_begin(void) { g_val_ncur = 0; g_val_overflow = 0; g_val_runs = 0; lagfx_vk_validation_hook = val_hook; }
static void val_end(const char *label) {
    lagfx_vk_validation_hook = NULL;
    if (getenv("LAGFX_VAL_BASELINE_PRINT")) { for (int i = 0; i < g_val_ncur; i++) printf("VALKEY\t%s\t%u\t%u\t%s\n", label, g_val_cur[i].n, g_val_runs ? g_val_runs : 1u, g_val_cur[i].key); return; }
    if (!g_val_ncur && !g_val_overflow) return;
    char path[600]; snprintf(path, sizeof path, "%s/fixtures/val_baseline_audit.txt", SRCDIR);
    FILE *f = fopen(path, "r"); if (!f) { printf("FAIL val_end(%s): baseline file %s missing\n", label, path); abort(); }
    char ln[512]; int bad = 0;
    unsigned seen_base[VAL_MAXK] = {0}; bool found[VAL_MAXK] = {0};
    while (fgets(ln, sizeof ln, f)) {
        if (ln[0] == '#' || ln[0] == '\n') continue;
        char *lab = ln, *t1 = strchr(lab, '\t'); if (!t1) continue; *t1++ = 0;
        char *cnt = t1, *t2 = strchr(cnt, '\t'); if (!t2) continue; *t2++ = 0;
        char *key = t2; key[strcspn(key, "\r\n")] = 0;
        if (strcmp(lab, label) && strcmp(lab, "*")) continue;
        for (int i = 0; i < g_val_ncur; i++) if (!strcmp(key, g_val_cur[i].key)) { found[i] = true; if ((unsigned)atoi(cnt) > seen_base[i]) seen_base[i] = (unsigned)atoi(cnt); }
    }
    fclose(f);
    for (int i = 0; i < g_val_ncur; i++) {
        if (!found[i]) { printf("FAIL validation message not in the baseline [%s] x%u: %s\n", label, g_val_cur[i].n, g_val_cur[i].key); bad++; }
        else if (g_val_cur[i].n > seen_base[i] * (g_val_runs ? g_val_runs : 1u)) { printf("FAIL validation message COUNT INCREASE [%s]: %u > baseline %u per exec x %u execs: %s\n", label, g_val_cur[i].n, seen_base[i], g_val_runs ? g_val_runs : 1u, g_val_cur[i].key); bad++; }
        else if (g_val_cur[i].n < seen_base[i] * (g_val_runs ? g_val_runs : 1u)) printf("note: validation message fewer than the baseline [%s]: %u < %u x %u: %s\n", label, g_val_cur[i].n, seen_base[i], g_val_runs ? g_val_runs : 1u, g_val_cur[i].key);
    }
    if (g_val_overflow) { printf("FAIL validation messages [%s]: %u more than %d distinct keys\n", label, g_val_overflow, VAL_MAXK); bad++; }
    if (bad) abort();
}

/* ---- scenario state read by the resolver ---- */
enum { FS_F, FS_U, FS_FACE, FS_MRT, FS_HALF, FS_DISC };
static struct { int fs; bool blend; } g_rc;
static VkShaderModule g_rc_fs[6]; static uint8_t *g_disc_spv; static size_t g_disc_bytes;   /* FS_DISC: a fragment shader translated from a fixture by the test */ static VkDescriptorSetLayout g_rc_dsl[2]; static VkPipelineLayout g_rc_pl[2];

static VkShaderModule rc_module(struct lagfx_vk_state *vk, const uint32_t *w, size_t bytes) {
    VkShaderModule m; VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = w };
    assert(vkCreateShaderModule(vk->device, &ci, NULL, &m) == VK_SUCCESS);
    return m;
}
static void rc_layout(struct lagfx_vk_state *vk, int n) {   /* n input attachments at bindings 24, 25 */
    if (g_rc_pl[n - 1]) return;
    VkDescriptorSetLayoutBinding lb[2] = { { .binding = 24, .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
                                           { .binding = 25, .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT } };
    VkDescriptorSetLayoutCreateInfo dl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = (uint32_t)n, .pBindings = lb };
    assert(vkCreateDescriptorSetLayout(vk->device, &dl, NULL, &g_rc_dsl[n - 1]) == VK_SUCCESS);
    VkPipelineLayoutCreateInfo pl = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_rc_dsl[n - 1] };
    assert(vkCreatePipelineLayout(vk->device, &pl, NULL, &g_rc_pl[n - 1]) == VK_SUCCESS);
}
static bool rc_resolver(void *c, lagfx_protocol_t *p, const lagfx_task_entry_t *t, uint32_t ref, const uint32_t e[3], lagfx_a64_pipeline_src_t *out) {
    (void)c; (void)t; (void)ref; (void)e;
    struct lagfx_vk_state *vk = ((lagfx_device_t *)p->dev)->vk;
    if (!g_vs) g_vs = rc_module(vk, k_vs_spv, sizeof k_vs_spv);
    memset(out, 0, sizeof *out);
    out->desc.vertex_shader = g_vs, out->desc.layout = vk->empty_layout;
    out->desc.n_vtx_inputs = 1, out->desc.vtx_in_loc[0] = 0, out->desc.vtx_in_comp[0] = 2, out->desc.vtx_stride = 8;
    out->vertex_buffer_index = 0;
    out->desc.blend_set = 1, out->desc.color_write_mask = 0xf, out->desc.masks_valid = 1;
    for (int i = 0; i < 4; i++) out->desc.color_write_masks[i] = 0xf;
    out->desc.blend_src_rgb = out->desc.blend_src_a = VK_BLEND_FACTOR_ONE, out->desc.blend_dst_rgb = out->desc.blend_dst_a = VK_BLEND_FACTOR_ZERO;   /* blend off by default */
    if (g_rc.fs == FS_DISC) {
        out->desc.fragment_shader = rc_module(vk, (const uint32_t *)(const void *)g_disc_spv, g_disc_bytes);   /* a fresh module per run: the SPIR-V changes with the mode under test */
    } else if (g_rc.fs == FS_HALF) {
        if (!g_rc_fs[FS_HALF]) g_rc_fs[FS_HALF] = rc_module(vk, k_fs_half_spv, sizeof k_fs_half_spv);
        out->desc.fragment_shader = g_rc_fs[FS_HALF];
        if (g_rc.blend) out->desc.blend_enable = 1, out->desc.blend_dst_rgb = out->desc.blend_dst_a = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;   /* premultiplied source-over */
    } else {
        const uint32_t *w[4] = { k_fs_acc_f_spv, k_fs_acc_u_spv, k_fs_acc_face_spv, k_fs_acc_mrt_spv };
        const size_t sz[4] = { sizeof k_fs_acc_f_spv, sizeof k_fs_acc_u_spv, sizeof k_fs_acc_face_spv, sizeof k_fs_acc_mrt_spv };
        const int n = g_rc.fs == FS_MRT ? 2 : 1;
        if (!g_rc_fs[g_rc.fs]) g_rc_fs[g_rc.fs] = rc_module(vk, w[g_rc.fs], sz[g_rc.fs]);
        rc_layout(vk, n);
        out->desc.fragment_shader = g_rc_fs[g_rc.fs], out->desc.layout = g_rc_pl[n - 1], out->dsl = g_rc_dsl[n - 1];
        out->n_bind = (uint32_t)n;
        for (int i = 0; i < n; i++) out->bind[i].binding = (uint8_t)(24 + i), out->bind[i].kind = LAGFX_A64_BIND_INPUT, out->bind[i].stage = 1, out->bind[i].metal_index = (int16_t)i,
                                    out->bind[i].ia_class = g_rc.fs == FS_U ? 1u : 0u;
    }
    out->release = rel;
    return true;
}

/* ---- guest-side builders ---- */
static void put_tex(uint32_t ref, uint32_t vpn, uint32_t mtl_fmt, uint32_t bpp, uint32_t samples) {
    uint8_t t[220]; memset(t, 0, sizeof t);
    uint64_t tlen = RT_W * RT_H * (uint64_t)bpp; memcpy(t, &tlen, 8); memcpy(t + 8, &vpn, 4);
    t[12] = 1;
    { uint8_t *mp = t + 12 + 0x18; uint64_t row = RT_W * (uint64_t)bpp; uint32_t mw = RT_W, mh = RT_H, md = 1; memcpy(mp + 0x10, &row, 8); memcpy(mp + 0x18, &mw, 4); memcpy(mp + 0x1c, &mh, 4); memcpy(mp + 0x20, &md, 4); }
    uint32_t rec = 12 + 0x18 + 36, tag = 0x14; memcpy(t + rec, &tag, 4);
    uint8_t *d = t + rec + 12;
    d[0] = 2; d[2] = (uint8_t)mtl_fmt;
    uint32_t w = RT_W, h = RT_H, one = 1; memcpy(d + 4, &w, 4); memcpy(d + 8, &h, 4); memcpy(d + 12, &one, 4);
    d[16] = 1; d[18] = (uint8_t)samples;
    uint32_t tsize = rec + 12 + 28;
    uint32_t pay = 0x600u + (ref % 8u) * 0x100u;
    va_write(VA_PAY + pay, t, tsize);
    entry(ref, LAGFX_A64_OBJ_TEXTURE, tsize, VA_PAY + pay);
}
static void put_vertices(const float *xy, uint32_t nv) { va_write((uint64_t)VPN_VB << 14, xy, nv * 8u); }
static void put_indices(const uint16_t *ix, uint32_t n) { va_write((uint64_t)VPN_IB << 14, ix, n * 2u); }
static void cb_draw_p(uint32_t prim, uint32_t start, uint32_t count, uint32_t inst) {   /* tag 3: instanced draw {u16 start, u16 count, u16 instances, u16 prim} */
    uint8_t r[8]; uint16_t s = (uint16_t)start, c = (uint16_t)count, i = (uint16_t)inst, pr = (uint16_t)prim; memcpy(r, &s, 2); memcpy(r + 2, &c, 2); memcpy(r + 4, &i, 2); memcpy(r + 6, &pr, 2); cb_cmd(3, r, 8);
}
static void cb_draw_idx_p(uint32_t prim, uint32_t count, uint32_t inst) {   /* tag 9: indexed instanced {u16 prim, u16 index_type, u32 ref, u16 count, u16 index_offset, u16 instances} */
    uint8_t r[14] = {0}; uint16_t pr = (uint16_t)prim, c = (uint16_t)count, i = (uint16_t)inst; uint32_t ref = REF_IB; memcpy(r, &pr, 2); memcpy(r + 4, &ref, 4); memcpy(r + 8, &c, 2); memcpy(r + 12, &i, 2); cb_cmd(9, r, 14);
}
static uint16_t h16(const uint8_t *img8, uint32_t x, uint32_t y, uint32_t c) { return half_at(img8, x, y, c); }

typedef struct { uint8_t bgra[RT_W * RT_H * 4]; uint8_t half[RT_W * RT_H * 8]; uint32_t u32[RT_W * RT_H]; uint64_t split_draws, subdraws, capped, unsupported, class_mismatch, draws, skipped, no_target, target_na, blend_off, pipe_fail; uint64_t val_msgs, vp_degen, vp_empty, vp_inexp; } rc_res_t;
typedef void (*rc_body_t)(void);
static const double *g_vp_custom; static const uint64_t *g_sc_custom;   /* S05 tests: a recorded MTLViewport {x,y,w,h,zn,zf} / MTLScissorRect {x,y,w,h} instead of the full-target one */
/* One exec: pass over the listed attachments (slot 0 / 1 refs), the body records vertex-buffer binding + draws. Returns pixels and counters. */
static void rc_run(const uint32_t refs[2], const double clr0[4], const double clr1[4], rc_body_t body, bool raster_order, rc_res_t *out) {
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    lagfx_task_entry_t *t = def_task(p, 1);
    build_objects();
    put_tex(REF_TEX3, VPN_TEX3, 53, 4, 1);   /* R32Uint */
    g_res_tex2 = true; g_res_tex3 = true;
    lagfx_a64_exec_set_pipeline_resolver(p, rc_resolver, NULL);
    const bool saved = g_dev->vk->have_raster_order;
    g_dev->vk->have_raster_order = raster_order;
    const uint16_t loads[2] = { 2, 2 };
    const double clr[2][4] = { { clr0[0], clr0[1], clr0[2], clr0[3] }, { clr1 ? clr1[0] : 0, clr1 ? clr1[1] : 0, clr1 ? clr1[2] : 0, clr1 ? clr1[3] : 0 } };
    uint64_t v0 = lagfx_vk_validation_msg_count; g_val_runs++;
    cb_reset(); cb_pass_n(refs, loads, clr); if (g_vp_custom) cb_cmd(130, g_vp_custom, 48); else cb_viewport(RT_W, RT_H); if (g_sc_custom) cb_cmd(117, g_sc_custom, 32); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, 0);
    body();
    exec(p, 1, cb_finish(), true);
    g_dev->vk->have_raster_order = saved;
    memset(out, 0, sizeof *out);
    uint32_t w, h;
    if (lagfx_a64_exec_readback_texture(p, t, REF_TEX, out->bgra, sizeof out->bgra, &w, &h)) {} else memset(out->bgra, 0xee, sizeof out->bgra);
    if (!lagfx_a64_exec_readback_texture(p, t, REF_TEX2, out->half, sizeof out->half, &w, &h)) memset(out->half, 0xee, sizeof out->half);
    if (!lagfx_a64_exec_readback_texture(p, t, REF_TEX3, out->u32, sizeof out->u32, &w, &h)) memset(out->u32, 0xee, sizeof out->u32);
    const __typeof__(p->diag2.exec.fmt) *F = &p->diag2.exec.fmt;
    out->split_draws = F->fbf_split_draws, out->subdraws = F->fbf_split_subdraws, out->capped = F->fbf_split_capped, out->unsupported = F->fbf_split_unsupported, out->class_mismatch = F->fbf_class_mismatch;
    out->blend_off = F->blend_off_format, out->target_na = F->target_not_attachable;
    out->draws = p->diag2.exec.run.draws, out->skipped = p->diag2.exec.run.draws_skipped, out->no_target = p->diag2.exec.run.no_target, out->pipe_fail = p->diag2.exec.pipe.pipe_build_failed;
    out->val_msgs = lagfx_vk_validation_msg_count - v0, out->vp_degen = p->diag2.exec.run.viewport_degenerate, out->vp_empty = p->diag2.exec.run.viewport_empty_metal, out->vp_inexp = p->diag2.exec.run.viewport_inexpressible;
    lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); free(p);
    g_res_tex2 = g_res_tex3 = false;
}

/* ---- scenes: float2 vertices; the same big triangle / its repeats overlap on pixel (8,8) ---- */
static const float TRI[6] = { -0.9f, -0.9f, 0.9f, -0.9f, 0.0f, 0.9f };
static void v_tris(uint32_t n) { float v[2 * 3 * 16]; for (uint32_t i = 0; i < n * 3u; i++) v[2 * i] = TRI[2 * (i % 3)], v[2 * i + 1] = TRI[2 * (i % 3) + 1]; put_vertices(v, n * 3u); }
static void b_list4(void)  { v_tris(4); cb_draw_p(3, 0, 12, 1); }                         /* 4 identical triangles in ONE draw */
static void b_list1(void)  { v_tris(1); cb_draw_p(3, 0, 3, 1); }                          /* a single triangle: nothing to split */
static void b_idx4(void)   { v_tris(1); const uint16_t ix[12] = { 0,1,2, 0,1,2, 0,1,2, 0,1,2 }; put_indices(ix, 12); cb_draw_idx_p(3, 12, 1); }
static void b_inst4(void)  { v_tris(1); cb_draw_p(3, 0, 3, 4); }                          /* one triangle, 4 instances */
static void b_idxinst(void){ v_tris(1); const uint16_t ix[6] = { 0,1,2, 0,1,2 }; put_indices(ix, 6); cb_draw_idx_p(3, 6, 2); }   /* 2 triangles x 2 instances = 4 fragments on the pixel */
static void b_inst100(void){ v_tris(1); cb_draw_p(3, 0, 3, 100); }
static void b_inst17k(void){ v_tris(1); cb_draw_p(3, 0, 3, 17000); }                        /* over A64_FBF_SPLIT_MAX */
static void b_strip(void) {   /* 6 vertices cycling A B C: strip triangles (A,B,C) (B,A,C)-as-(v1,v3,v2) ... alternate front / back facing, all containing (8,8) */
    float v[12]; for (int i = 0; i < 6; i++) v[2 * i] = TRI[2 * (i % 3)], v[2 * i + 1] = TRI[2 * (i % 3) + 1]; put_vertices(v, 6); cb_draw_p(4, 0, 6, 1); }
static void b_strip_naive(void) {   /* the same four triangles, each as its OWN 3-vertex strip draw from v_i with no front-face flip (what a naive split would record) */
    float v[12]; for (int i = 0; i < 6; i++) v[2 * i] = TRI[2 * (i % 3)], v[2 * i + 1] = TRI[2 * (i % 3) + 1]; put_vertices(v, 6);
    for (uint32_t k = 0; k < 4; k++) cb_draw_p(4, k, 3, 1); }
static void b_lines4(void) { const float L[4] = { -0.9f, -0.0625f, 0.9f, -0.0625f }; float v[16]; for (int i = 0; i < 8; i++) v[i * 2] = L[(i % 2) * 2], v[i * 2 + 1] = L[(i % 2) * 2 + 1]; put_vertices(v, 8); cb_draw_p(1, 0, 8, 1); }   /* 4 line segments (list) */
static void b_lstrip(void) { const float L[4] = { -0.9f, -0.0625f, 0.9f, -0.0625f }; float v[10]; for (int i = 0; i < 5; i++) v[i * 2] = L[(i % 2) * 2], v[i * 2 + 1] = L[(i % 2) * 2 + 1]; put_vertices(v, 5); cb_draw_p(2, 0, 5, 1); }   /* 4 segments (strip) */
static void b_two_draws(void) { v_tris(1); cb_draw_p(3, 0, 3, 1); cb_draw_p(3, 0, 3, 1); }

#define IS_RED_ACC(v16, n) ((v16) == (uint16_t)((n) == 1 ? 0x3400 : (n) == 2 ? 0x3800 : (n) == 4 ? 0x3c00 : (n) == 3 ? 0x3a00 : 0xffff))   /* half(0.25 * n) */
static const double CBLUE[4] = { 0, 0, 1, 1 }, ZERO4[4] = { 0, 0, 0, 0 }, BLACK[4] = { 0, 0, 0, 1 };

/* UNORM8 model of n sequential "+0.25 red" steps with a read-back between them (what the fetch does on a BGRA8 target) */
static uint32_t acc8(uint32_t n) { uint32_t v = 0; for (uint32_t i = 0; i < n; i++) { double f = v / 255.0 + 0.25; if (f > 1) f = 1; v = (uint32_t)(f * 255.0 + 0.5); } return v; }

typedef struct { const char *name; rc_body_t body; uint32_t nprim; uint64_t expect_sub; uint32_t expect_n; } scene_t;

static void test_split_scenes(void) {
    const bool dev_ro = g_dev->vk->have_raster_order;
    const scene_t sc[] = {
        { "triangle list x4 (one draw)",          b_list4,   4, 4, 4 },
        { "indexed triangle list x4 (one draw)",  b_idx4,    4, 4, 4 },
        { "1 triangle x 4 instances",             b_inst4,   4, 4, 4 },
        { "2 indexed triangles x 2 instances",    b_idxinst, 4, 4, 4 },
        { "line list x4",                         b_lines4,  4, 4, 4 },
        { "line strip, 4 segments",               b_lstrip,  4, 4, 4 },
    };
    for (unsigned i = 0; i < sizeof sc / sizeof *sc; i++) {
        static rc_res_t ref, spl;
        const uint32_t refs16[2] = { REF_TEX2, 0 }, refs8[2] = { REF_TEX, 0 };
        g_rc.fs = FS_F;
        /* RGBA16F: N * 0.25 is exact in half precision */
        rc_run(refs16, ZERO4, NULL, sc[i].body, dev_ro, &ref);
        rc_run(refs16, ZERO4, NULL, sc[i].body, false, &spl);
        assert(ref.draws == (sc[i].body == b_two_draws ? 2u : 1u) && spl.draws == ref.draws && ref.pipe_fail == 0 && spl.pipe_fail == 0);
        assert(IS_RED_ACC(h16(ref.half, 8, 8, 0), sc[i].expect_n) && IS_RED_ACC(h16(spl.half, 8, 8, 0), sc[i].expect_n));
        assert(memcmp(ref.half, spl.half, sizeof ref.half) == 0);   /* the split render equals the single-draw render on every pixel */
        assert(spl.split_draws == 1 && spl.subdraws == sc[i].expect_sub);
        assert(dev_ro ? (ref.split_draws == 0 && ref.subdraws == 0) : (ref.split_draws == 1));          /* positive control: with rasterization-order access (or on a device without it, the reference run IS the split one) */
        const uint16_t half_red = h16(spl.half, 8, 8, 0);
        /* BGRA8: sequential UNORM8 read-modify-write: every overlapping fragment sees the previous one's stored value */
        rc_run(refs8, ZERO4, NULL, sc[i].body, false, &spl);
        uint32_t px8 = px(spl.bgra, 8, 8);
        assert(((px8 >> 16) & 255) == acc8(sc[i].expect_n));
        printf("  ok: %s: %llu sub-draws, pixel(8,8) half red 0x%04x, bgra8 red %u\n", sc[i].name, (unsigned long long)spl.subdraws, half_red, (px8 >> 16) & 255);
    }
    /* a single primitive / a single instance: nothing can overlap inside the draw, so nothing is split */
    { static rc_res_t r; const uint32_t refs16[2] = { REF_TEX2, 0 }; rc_run(refs16, ZERO4, NULL, b_list1, false, &r); assert(r.split_draws == 0 && r.subdraws == 0 && r.draws == 1 && IS_RED_ACC(h16(r.half, 8, 8, 0), 1)); }
    /* two separate draws: handled by the existing between-draw barrier, not split */
    { static rc_res_t r; const uint32_t refs16[2] = { REF_TEX2, 0 }; rc_run(refs16, ZERO4, NULL, b_two_draws, false, &r); assert(r.split_draws == 0 && r.draws == 2 && IS_RED_ACC(h16(r.half, 8, 8, 0), 2)); }
    puts("PASS: framebuffer fetch without rasterization-order access: one draw with overlapping primitives / instances is recorded primitive by primitive (counted), pixels equal the single-draw reference");
}

static void test_split_strip_and_cap(void) {
    const bool dev_ro = g_dev->vk->have_raster_order;
    static rc_res_t ref, spl, naive; const uint32_t refs16[2] = { REF_TEX2, 0 }, refs8[2] = { REF_TEX, 0 };
    g_rc.fs = FS_FACE;
    rc_run(refs16, ZERO4, NULL, b_strip, dev_ro, &ref);
    rc_run(refs16, ZERO4, NULL, b_strip, false, &spl);
    assert(ref.pipe_fail == 0 && spl.pipe_fail == 0 && ref.draws == 1 && spl.draws == 1);
    /* strip triangles alternate facing (cyclic vertices): 2 front (+0.25 red) and 2 back (+0.125 green; the even ones are back-facing here) fragments on the pixel */
    assert(h16(ref.half, 8, 8, 0) == 0x3800 && h16(ref.half, 8, 8, 1) == 0x3400 && h16(spl.half, 8, 8, 0) == 0x3800 && h16(spl.half, 8, 8, 1) == 0x3400);
    assert(memcmp(ref.half, spl.half, sizeof ref.half) == 0);
    assert(spl.split_draws == 1 && spl.subdraws == 4 && spl.unsupported == 0 && (dev_ro ? ref.split_draws == 0 : ref.split_draws == 1));
    /* NEGATIVE control: the same four triangles as separate 3-vertex strip draws WITHOUT flipping the front face for odd i: the odd ones flip facing, the image differs from the reference */
    rc_run(refs16, ZERO4, NULL, b_strip_naive, dev_ro, &naive);
    assert(h16(naive.half, 8, 8, 0) == 0 && h16(naive.half, 8, 8, 1) == 0x3800 && memcmp(naive.half, ref.half, sizeof ref.half) != 0);
    /* on BGRA8 too */
    rc_run(refs8, ZERO4, NULL, b_strip, false, &spl);
    puts("PASS: triangle strip split keeps the primitive order and the per-triangle facing (odd triangles flip the front face); a naive per-triangle strip draw does not (negative control)");

    /* cap: 100 instances are split, 17000 (> A64_FBF_SPLIT_MAX) are recorded whole and counted */
    g_rc.fs = FS_F;
    rc_run(refs8, ZERO4, NULL, b_inst100, false, &spl);
    assert(spl.split_draws == 1 && spl.subdraws == 100 && spl.capped == 0 && ((px(spl.bgra, 8, 8) >> 16) & 255) == 255);
    rc_run(refs8, ZERO4, NULL, b_inst17k, false, &spl);
    assert(spl.split_draws == 0 && spl.subdraws == 0 && spl.capped == 1 && spl.draws == 1 && ((px(spl.bgra, 8, 8) >> 16) & 255) == 255);
    puts("PASS: split cap: 100 sub-draws recorded, 17000 recorded whole and counted (fbf_split_capped)");
}

/* MRT: BGRA8 slot 0 and RGBA16F slot 1, FS reads both and adds 0.25 red / 0.125 green; 4 overlapping triangles in one draw */
static void test_split_mrt(void) {
    const bool dev_ro = g_dev->vk->have_raster_order;
    static rc_res_t ref, spl; const uint32_t refs[2] = { REF_TEX, REF_TEX2 };
    g_rc.fs = FS_MRT;
    rc_run(refs, ZERO4, ZERO4, b_list4, dev_ro, &ref);
    rc_run(refs, ZERO4, ZERO4, b_list4, false, &spl);
    assert(ref.pipe_fail == 0 && spl.pipe_fail == 0 && spl.draws == 1 && spl.split_draws == 1 && spl.subdraws == 4);
    assert(((px(spl.bgra, 8, 8) >> 16) & 255) == acc8(4) && h16(spl.half, 8, 8, 1) == 0x3800 && h16(spl.half, 8, 8, 0) == 0);   /* slot 0 red 4 x 0.25; slot 1 green 4 x 0.125 = 0.5 */
    assert(memcmp(ref.bgra, spl.bgra, sizeof ref.bgra) == 0 && memcmp(ref.half, spl.half, sizeof ref.half) == 0);
    puts("PASS: multi-render-target framebuffer fetch (BGRA8 + RGBA16Float), both attachments read and written through the split draw");
}

/* Numeric class of the attachment vs the shader's input-attachment type (VUID-vkCmdDraw-format-07753) */
static void b_list4_u(void) { b_list4(); }
static void test_fetch_class(void) {
    static rc_res_t r; const uint32_t refs_u[2] = { REF_TEX3, 0 }, refs_f[2] = { REF_TEX2, 0 };
    const bool dev_ro = g_dev->vk->have_raster_order;
    for (int ro = 0; ro < 2; ro++) {
        if (ro && !dev_ro) break;
        /* positive control: uint shader on an R32Uint attachment: 4 overlapping fragments add 4 */
        g_rc.fs = FS_U;
        rc_run(refs_u, ZERO4, NULL, b_list4_u, ro, &r);
        assert(r.class_mismatch == 0 && r.draws == 1 && r.pipe_fail == 0 && r.u32[8 * RT_W + 8] == 4u && r.u32[0] == 0u);
        /* negative: a FLOAT input attachment declared for an R32Uint attachment: the draw is refused and counted, the attachment keeps its clear */
        g_rc.fs = FS_F;
        rc_run(refs_u, ZERO4, NULL, b_list4_u, ro, &r);
        assert(r.class_mismatch == 1 && r.draws == 0 && r.skipped == 1 && r.u32[8 * RT_W + 8] == 0u);
        /* negative: a UINT input attachment declared for an RGBA16F attachment */
        g_rc.fs = FS_U;
        rc_run(refs_f, ZERO4, NULL, b_list4_u, ro, &r);
        assert(r.class_mismatch == 1 && r.draws == 0 && r.skipped == 1 && h16(r.half, 8, 8, 0) == 0);
        /* positive: float shader on RGBA16F */
        g_rc.fs = FS_F;
        rc_run(refs_f, ZERO4, NULL, b_list4_u, ro, &r);
        assert(r.class_mismatch == 0 && r.draws == 1 && IS_RED_ACC(h16(r.half, 8, 8, 0), 4));
    }
    puts("PASS: input-attachment numeric class vs attachment format: matching float / uint draws run; float-on-uint and uint-on-float are refused and counted");
}

/* S04 (docs/spec-audit.md): VkClearColorValue's union member must follow the attachment's numeric class (VkClearColorValue: float32 for float / normalized formats, int32 for
 * SINT, uint32 for UINT [V]). An R32Uint target cleared with the float bits of 7.0 reads back 0x40E00000; with the uint32 member it reads back 7. */
static void b_none(void) {}
static void test_clear_integer_class(void) {
    val_begin();
    static rc_res_t r; const uint32_t refs_u[2] = { REF_TEX3, 0 };
    g_rc.fs = FS_U;
    const double c7[4] = { 7, 0, 0, 0 };
    rc_run(refs_u, c7, NULL, b_none, false, &r);
    uint32_t bad = 0; for (uint32_t i = 0; i < RT_W * RT_H; i++) bad += r.u32[i] != 7u;
    assert(bad == 0);
    const double big[4] = { 5e9, 0, 0, 0 };   /* beyond uint32: saturates (Vulkan leaves a value the format cannot hold undefined) */
    rc_run(refs_u, big, NULL, b_none, false, &r);
    assert(r.u32[0] == 0xffffffffu && r.u32[RT_W * RT_H - 1] == 0xffffffffu);
    const double neg[4] = { -3, 0, 0, 0 };
    rc_run(refs_u, neg, NULL, b_none, false, &r);
    assert(r.u32[0] == 0u);
    /* a float target keeps float semantics */
    static rc_res_t rf; const uint32_t refs_f[2] = { REF_TEX2, 0 }; const double half[4] = { 0.5, 0, 0, 1 };
    g_rc.fs = FS_F;
    rc_run(refs_f, half, NULL, b_none, false, &rf);
    assert(h16(rf.half, 3, 3, 0) == 0x3800u);   /* binary16 0.5 */
    val_end("S04 clear");
    puts("PASS: an integer colour attachment is cleared through the uint32 / int32 member by its numeric class (R32Uint 7 -> 7; saturating); a float target keeps float32");
}

/* S05 (docs/spec-audit.md): the guest's MTLViewport is used as recorded. A viewport larger than the target / with a negative origin is legal in Vulkan (VkViewport [V]: bounded only by
 * viewportBoundsRange / maxViewportDimensions) and must map NDC through the FULL declared rect; the old path clamped the rect to the target, which rescaled the mapping. One
 * triangle (NDC (-0.9,-0.9) (0.9,-0.9) (0,0.9)) on the 16x16 R32Uint target; a covered pixel reads 1 (the fragment shader adds 1). */
/* S05 classification as a pure function: lagfx_vk_region_to_vk with explicit device limits (maxViewportDimensions 16384, viewportBoundsRange [-32768, 32767]) on a 16 x 16 target. */
#include "vulkan/draw_record.h"
static void test_viewport_classes(void) {
    static const struct { double vp[4]; lagfx_vp_status_t want; const char *why; } t[] = {
        { { 0, 0, 16, 16 }, LAGFX_VP_OK, "full target" },
        { { -8, -8, 32, 32 }, LAGFX_VP_OK, "wider than the target, negative origin" },
        { { 0.5, 0.25, 15.5, 15.75 }, LAGFX_VP_OK, "fractional" },
        { { -16384, 0, 16384, 16 }, LAGFX_VP_OK, "inside the bounds range, off target (Vulkan draws nothing itself)" },
        { { 0, 0, 16384, 16384 }, LAGFX_VP_OK, "exactly maxViewportDimensions" },
        { { 0, 0, 0, 16 }, LAGFX_VP_EMPTY_METAL, "zero width" },
        { { 0, 0, 16, -1 }, LAGFX_VP_EMPTY_METAL, "negative height" },
        { { 0, 0, 0.0 / 0.0, 16 }, LAGFX_VP_EMPTY_METAL, "NaN width" },
        { { 0, 0, 16384.5, 16 }, LAGFX_VP_INEXPRESSIBLE, "wider than maxViewportDimensions, meets the target" },
        { { -20000, 0, 40000, 16 }, LAGFX_VP_INEXPRESSIBLE, "x < bounds range and w > max: covers the target" },
        { { 0.0 / 0.0, 0, 16, 16 }, LAGFX_VP_INEXPRESSIBLE, "NaN origin" },
        { { 0, 0, 1.0 / 0.0, 16 }, LAGFX_VP_INEXPRESSIBLE, "infinite width" },
        { { -40000, 0, 8000, 16 }, LAGFX_VP_EMPTY_METAL, "x < bounds range, rectangle [-40000, -32000) misses the target: exact skip" },
        { { 40000, 0, 16, 16 }, LAGFX_VP_EMPTY_METAL, "x + w > bounds range, rectangle right of the target: exact skip" },
        { { 0, 40000, 16, 16 }, LAGFX_VP_EMPTY_METAL, "y beyond the bounds range, below the target: exact skip" },
        { { -32769, 0, 32800, 16 }, LAGFX_VP_INEXPRESSIBLE, "starts 1 below the bounds range, w beyond max, meets the target" },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        lagfx_draw_region_t g; memset(&g, 0, sizeof g);
        g.metal = true, g.m_vp_set = true, g.lim_dim = 16384.0f, g.lim_lo = -32768.0f, g.lim_hi = 32767.0f;
        for (int k = 0; k < 4; k++) g.m_vp[k] = t[i].vp[k];
        VkViewport vp; VkRect2D sc;
        const lagfx_vp_status_t got = lagfx_vk_region_to_vk(&g, 16, 16, true, &vp, &sc);
        if (got != t[i].want) { printf("FAIL viewport class [%s]: got %d want %d\n", t[i].why, (int)got, (int)t[i].want); abort(); }
        if (got != LAGFX_VP_OK && (sc.extent.width != 0 || sc.extent.height != 0)) { printf("FAIL viewport class [%s]: skipped viewport must have an empty scissor\n", t[i].why); abort(); }
        if (got == LAGFX_VP_OK && (vp.width != (float)t[i].vp[2] || vp.height != -(float)t[i].vp[3])) { printf("FAIL viewport class [%s]: the viewport must be kept as recorded (negative height = Y flip)\n", t[i].why); abort(); }
    }
    puts("PASS: viewport classes (pure): drawable / fractional / off-target-expressible kept as recorded; empty or off-target-inexpressible = viewport_empty_metal; drawable-but-inexpressible = viewport_inexpressible");
}

static void test_viewport_semantics(void) {
    val_begin();
    static rc_res_t r; const uint32_t refs_u[2] = { REF_TEX3, 0 };
    g_rc.fs = FS_U;
#define PX(x, y) r.u32[(y) * RT_W + (x)]
    const double vp_wide[6] = { 0, 0, 32, 16, 0, 1 };   /* twice the target width: pixel (15,14) maps to NDC x = -0.031, inside the triangle; clamped to 16 wide it maps to 0.94, outside */
    g_vp_custom = vp_wide; rc_run(refs_u, ZERO4, NULL, b_list1, false, &r);
    assert(r.draws == 1 && r.vp_degen == 0 && PX(15, 14) == 1u && PX(0, 14) == 0u);
    const double vp_neg[6] = { -8, 0, 32, 16, 0, 1 };   /* negative origin: pixel (0,14) maps to NDC x = -0.47 (inside); the old path clamped the origin to 0 */
    g_vp_custom = vp_neg; rc_run(refs_u, ZERO4, NULL, b_list1, false, &r);
    assert(r.vp_degen == 0 && PX(0, 14) == 1u && PX(15, 14) == 1u);
    const double vp_frac[6] = { 0.5, 0, 16, 16, 0, 1 };   /* fractional origin: shifts the mapping by half a pixel, no truncation to 0 */
    g_vp_custom = vp_frac; rc_run(refs_u, ZERO4, NULL, b_list1, false, &r);
    assert(r.vp_degen == 0 && PX(8, 14) == 1u);
    const double vp_zero[6] = { 0, 0, 0, 0, 0, 1 };   /* an explicit empty viewport covers nothing (it is NOT "unset" = full target): nothing is drawn in Metal either -> viewport_empty_metal */
    g_vp_custom = vp_zero; rc_run(refs_u, ZERO4, NULL, b_list1, false, &r);
    assert(r.vp_degen == 1 && r.vp_empty == 1 && r.vp_inexp == 0 && PX(8, 8) == 0u && PX(6, 6) == 0u);
    const double vp_negw[6] = { 0, 0, -16, 16, 0, 1 };   /* negative size: empty (Metal's rule for it is [N]) */
    g_vp_custom = vp_negw; rc_run(refs_u, ZERO4, NULL, b_list1, false, &r);
    assert(r.vp_empty == 1 && r.vp_inexp == 0 && PX(8, 8) == 0u);
    const double vp_nanw[6] = { 0, 0, 0.0 / 0.0, 16, 0, 1 };   /* NaN size: empty */
    g_vp_custom = vp_nanw; rc_run(refs_u, ZERO4, NULL, b_list1, false, &r);
    assert(r.vp_empty == 1 && r.vp_inexp == 0 && PX(8, 8) == 0u);
    const double vp_nanx[6] = { 0.0 / 0.0, 0, 16, 16, 0, 1 };   /* NaN origin: no defined meaning, Vulkan cannot express it -> viewport_inexpressible */
    g_vp_custom = vp_nanx; rc_run(refs_u, ZERO4, NULL, b_list1, false, &r);
    assert(r.vp_empty == 0 && r.vp_inexp == 1 && PX(8, 8) == 0u);
    const double vp_huge[6] = { 0, 0, 1e6, 16, 0, 1 };   /* beyond maxViewportDimensions AND meeting the target: Metal would draw, Vulkan cannot express it -> viewport_inexpressible, skipped */
    g_vp_custom = vp_huge; rc_run(refs_u, ZERO4, NULL, b_list1, false, &r);
    assert(r.vp_degen == 1 && r.vp_empty == 0 && r.vp_inexp == 1 && PX(8, 8) == 0u);
    const double vp_far[6] = { -1e6, 0, 16, 16, 0, 1 };   /* x far below viewportBoundsRange but the rectangle [-1e6, -1e6+16) misses the target: nothing drawn in Metal either -> viewport_empty_metal (exact skip) */
    g_vp_custom = vp_far; rc_run(refs_u, ZERO4, NULL, b_list1, false, &r);
    assert(r.vp_empty == 1 && r.vp_inexp == 0 && PX(8, 8) == 0u);
    const double vp_off[6] = { -1000, 0, 16, 16, 0, 1 };   /* the same kind of viewport, still expressible: Vulkan itself draws nothing (no counter) - what the exact skip above reproduces */
    g_vp_custom = vp_off; rc_run(refs_u, ZERO4, NULL, b_list1, false, &r);
    assert(r.vp_degen == 0 && PX(8, 8) == 0u && PX(0, 14) == 0u && PX(15, 14) == 0u);
    const double vp_cover[6] = { -20000, -20000, 40000, 40000, 0, 1 };   /* wider than maxViewportDimensions[0] (16384 here) and covering the whole target: inexpressible, skipped, counted */
    g_vp_custom = vp_cover; rc_run(refs_u, ZERO4, NULL, b_list1, false, &r);
    assert((g_dev->vk->phys_props.limits.maxViewportDimensions[0] >= 40000u) || (r.vp_inexp == 1 && r.vp_empty == 0 && PX(8, 8) == 0u));
    g_vp_custom = NULL;
    const uint64_t sc_in[4] = { 4, 4, 4, 4 };   /* scissor [4,8) x [4,8): (6,6) is inside the triangle and the scissor, (6,10) inside the triangle only */
    g_sc_custom = sc_in; rc_run(refs_u, ZERO4, NULL, b_list1, false, &r);
    assert(PX(6, 6) == 1u && PX(6, 10) == 0u && PX(2, 6) == 0u);
    const uint64_t sc_big[4] = { 0, 0, 100, 100 };   /* extends past the target: clamped to it, everything inside still drawn */
    g_sc_custom = sc_big; rc_run(refs_u, ZERO4, NULL, b_list1, false, &r);
    assert(PX(6, 6) == 1u && PX(6, 10) == 1u);
    const uint64_t sc_off[4] = { 20, 0, 4, 4 };   /* entirely outside the target: empty (the old path fell back to the full target) */
    g_sc_custom = sc_off; rc_run(refs_u, ZERO4, NULL, b_list1, false, &r);
    assert(PX(6, 6) == 0u && PX(6, 10) == 0u);
    g_sc_custom = NULL;
    rc_run(refs_u, ZERO4, NULL, b_list1, false, &r);   /* control: nothing recorded -> the default viewport / scissor is the whole target */
    assert(PX(6, 6) == 1u && PX(6, 10) == 1u && r.vp_degen == 0);
#undef PX
    val_end("S05 viewport");
    puts("PASS: Metal viewport used as recorded (wider than the target, negative / fractional origin; empty or inexpressible = nothing drawn, counted); scissor clamped to the target, not rescaled");
}

/* S06 (docs/spec-audit.md): air.discard_fragment must discard. Translate a hand-written fragment shader `if (pos.x < 8) discard_fragment(); return red;` (and an unconditional-discard one),
 * run it on the 16x16 RGBA16Float target through the real pipeline path, with OpKill (default) and OpDemoteToHelperInvocation (when the device enabled it). */
#include "air/bitcode_reader.h"
#include "air2spv/translate.h"
#include "vulkan/caps.h"
static bool load_disc(const char *fixture, bool demote) {
    char path[600]; snprintf(path, sizeof path, "%s/fixtures/%s", SRCDIR, fixture);
    FILE *f = fopen(path, "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *bc = malloc((size_t)n); if (!bc || fread(bc, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(bc); return false; }
    fclose(f);
    lagfx_air_module_t *m = NULL; lagfx_stage_keys_t vso; memset(&vso, 0, sizeof vso);
    bool ok = lagfx_air_module_open(bc, (size_t)n, &m) == LAGFX_OK && m;
    free(g_disc_spv); g_disc_spv = NULL;
    lagfx_air2spv_set_discard_demote(demote);
    if (ok) ok = lagfx_air2spv_translate_module_ex(m, &vso, &g_disc_spv, &g_disc_bytes) == LAGFX_OK && g_disc_spv;
    lagfx_air2spv_set_discard_demote(false);
    if (m) lagfx_air_module_free(m);
    free(bc);
    return ok;
}
static void test_discard_fragment(void) {
    val_begin();
    static rc_res_t r; const uint32_t refs_f[2] = { REF_TEX2, 0 };
    const bool have_demote = (g_dev->vk->caps_enabled & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_DEMOTE)) != 0;
    for (int mode = 0; mode < 2; mode++) {
        if (mode && !have_demote) { puts("note: device does not enable shaderDemoteToHelperInvocation: demote mode not run"); break; }
        g_rc.fs = FS_DISC;
        assert(load_disc("spec_discard_branch.air.bc", mode == 1));
        rc_run(refs_f, ZERO4, NULL, b_list1, false, &r);
        assert(r.draws == 1 && r.pipe_fail == 0);
        assert(h16(r.half, 12, 14, 0) == 0x3c00u);   /* x >= 8, inside the triangle: red */
        assert(h16(r.half, 12, 14, 3) == 0x3c00u);
        assert(h16(r.half, 4, 14, 0) == 0u && h16(r.half, 4, 14, 3) == 0u);   /* x < 8, inside the triangle: discarded, keeps the clear */
        assert(load_disc("spec_discard_linear.air.bc", mode == 1));
        rc_run(refs_f, ZERO4, NULL, b_list1, false, &r);
        assert(r.draws == 1 && r.pipe_fail == 0);
        assert(h16(r.half, 12, 14, 0) == 0u && h16(r.half, 4, 14, 0) == 0u);   /* everything discarded */
    }
    free(g_disc_spv); g_disc_spv = NULL;
    val_end("S06 discard");
    puts("PASS: air.discard_fragment discards (OpKill in a split block; OpDemoteToHelperInvocation when the device enables it): branch shader keeps x<8 at the clear, unconditional discard leaves the target untouched");
}

/* S06, follow-up: a discard inside the two recognised loop plans (plain rotated loop, guarded loop) must discard too. An earlier revision dropped it there (OpKill needs a block split and the
 * plans' OpPhi parents are fixed labels); the split now moves the entry label. Fixtures: gen-discard-fixtures.sh. Guarded: x > 8 enters the loop (0.25 after two halvings), x < 8 takes the
 * guard edge (0.75, the merge phi's entry value); plain: 0.125 after three halvings. A discard in the loop body of the guarded shape is conditional per pixel (only x > 8 gets there), the
 * others discard every pixel. `half` of the target: red = value, alpha = 1 when drawn, 0 (the clear) when discarded. Runs OpKill, and OpDemoteToHelperInvocation when the device has it. */
static void test_discard_loop_plans(void) {
    static rc_res_t r; const uint32_t refs_f[2] = { REF_TEX2, 0 };
    const bool have_demote = (g_dev->vk->caps_enabled & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_DEMOTE)) != 0;
    static const struct { const char *fx; uint16_t hi_r, lo_r; int hi_drawn, lo_drawn; } k[] = {   /* hi = pixel (12,14) (x > 8), lo = pixel (4,14) (x < 8) */
        { "spec_discard_g_none.air.bc",  0x3400u, 0x3a00u, 1, 1 },
        { "spec_discard_g_entry.air.bc", 0, 0, 0, 0 },
        { "spec_discard_g_loop.air.bc",  0, 0x3a00u, 0, 1 },
        { "spec_discard_g_merge.air.bc", 0, 0, 0, 0 },
        { "spec_discard_p_none.air.bc",  0x3000u, 0x3000u, 1, 1 },
        { "spec_discard_p_entry.air.bc", 0, 0, 0, 0 },
        { "spec_discard_p_loop.air.bc",  0, 0, 0, 0 },
        { "spec_discard_p_merge.air.bc", 0, 0, 0, 0 },
    };
    for (int mode = 0; mode < 2; mode++) {
        if (mode && !have_demote) break;
        for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
            g_rc.fs = FS_DISC;
            assert(load_disc(k[i].fx, mode == 1));
            val_begin();
            rc_run(refs_f, ZERO4, NULL, b_list1, false, &r);
            val_end(k[i].fx);
            if (r.pipe_fail || r.draws != 1) { printf("FAIL %s mode=%d: draws=%llu pipe_fail=%llu\n", k[i].fx, mode, (unsigned long long)r.draws, (unsigned long long)r.pipe_fail); abort(); }
            const uint16_t hr = h16(r.half, 12, 14, 0), ha = h16(r.half, 12, 14, 3), lr = h16(r.half, 4, 14, 0), la = h16(r.half, 4, 14, 3);
            const bool ok = hr == k[i].hi_r && ha == (k[i].hi_drawn ? 0x3c00u : 0u) && lr == k[i].lo_r && la == (k[i].lo_drawn ? 0x3c00u : 0u);
            if (!ok) { printf("FAIL %s mode=%s: x>8 (r=%04x a=%04x) want (%04x, drawn=%d); x<8 (r=%04x a=%04x) want (%04x, drawn=%d)\n", k[i].fx, mode ? "demote" : "kill", hr, ha, k[i].hi_r, k[i].hi_drawn, lr, la, k[i].lo_r, k[i].lo_drawn); abort(); }
        }
    }
    free(g_disc_spv); g_disc_spv = NULL;
    puts("PASS: air.discard_fragment inside the plain and the guarded loop plan discards (entry / loop body / merge block; conditional per pixel through the guard), OpKill and demote");
}

/* S06, follow-up: derivatives after a discard. A fragment shader discards odd-x pixels and reads dFdx(pos.x) / dFdy(pos.y) afterwards; the surviving even-x pixels have a discarded quad
 * neighbour, so the derivative needs that lane's value. With OpDemoteToHelperInvocation the lane stays a helper invocation and the derivative is 1.0 [V: SPV_EXT_demote_to_helper_invocation:
 * derivatives stay valid]. OpKill terminates the lane: SPIR-V gives such a derivative no defined input (OpKill is deprecated for OpTerminateInvocation / demote [V changelog]); what
 * lavapipe returns is reported, and only the discard itself is asserted for that mode. */
static void test_discard_derivatives(void) {
    static rc_res_t r; const uint32_t refs_f[2] = { REF_TEX2, 0 };
    const bool have_demote = (g_dev->vk->caps_enabled & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_DEMOTE)) != 0;
    for (int mode = 0; mode < 2; mode++) {
        if (mode && !have_demote) { puts("note: device does not enable shaderDemoteToHelperInvocation: demote derivative test not run"); break; }
        g_rc.fs = FS_DISC;
        assert(load_disc("spec_discard_deriv.air.bc", mode == 1));
        val_begin();
        rc_run(refs_f, ZERO4, NULL, b_list1, false, &r);
        val_end("spec_discard_deriv.air.bc");
        assert(r.draws == 1 && r.pipe_fail == 0);
        assert(h16(r.half, 13, 14, 3) == 0u && h16(r.half, 13, 14, 0) == 0u);   /* odd x: discarded, keeps the clear */
        assert(h16(r.half, 12, 14, 3) == 0x3c00u);                              /* even x: drawn */
        const uint16_t dx = h16(r.half, 12, 14, 0), dy = h16(r.half, 12, 14, 1);
        printf("note: %s: dFdx / dFdy of a surviving pixel next to a discarded lane = %04x / %04x (1.0 = 3c00)\n", mode ? "demote" : "kill  ", dx, dy);
        if (mode) assert(dx == 0x3c00u && dy == 0x3c00u);
    }
    free(g_disc_spv); g_disc_spv = NULL;
    puts("PASS: derivatives after a discard: demote keeps the discarded lane as a helper (dFdx = dFdy = 1.0 for the neighbour); OpKill result reported");
}

/* D1: MSL round() rounds halves away from zero. GLSL.std.450 Round leaves halves unspecified and lavapipe (like most drivers) rounds to even: 2.5 -> 2. The fixture computes
 * (round(2.5)/4, -round(-2.5)/4, round(0.5)/2, -round(-0.5)/2): (0.75, 0.75, 0.5, 0.5) for half-away, (0.5, 0.5, 0, 0) for ties-to-even. */
static void test_round_halves_away(void) {
    val_begin();
    static rc_res_t r; const uint32_t refs_f[2] = { REF_TEX2, 0 };
    g_rc.fs = FS_DISC;
    assert(load_disc("spec_round_fragment.air.bc", false));
    rc_run(refs_f, ZERO4, NULL, b_list1, false, &r);
    assert(r.draws == 1 && r.pipe_fail == 0);
    assert(h16(r.half, 12, 14, 0) == 0x3a00u && h16(r.half, 12, 14, 1) == 0x3a00u && h16(r.half, 12, 14, 2) == 0x3800u && h16(r.half, 12, 14, 3) == 0x3800u);
    free(g_disc_spv); g_disc_spv = NULL;
    val_end("D1 round");
    puts("PASS: air.round rounds halves away from zero (2.5 -> 3, -2.5 -> -3, 0.5 -> 1, -0.5 -> -1) on the device, not to even");
}

/* ---- injected less-capable device (wraps the real queries; overrides one format / usage) ---- */
typedef struct { lagfx_fmtcap_backend_t real; VkFormat fmt; VkFormatFeatureFlags clear; VkImageUsageFlags refuse_any; VkImageUsageFlags refuse_all; } inj_t;
static inj_t g_inj; static lagfx_fmtcap_backend_t g_inj_be;
static void inj_fp(void *c, VkFormat f, VkFormatProperties2 *o) { inj_t *i = c; i->real.format_props2(i->real.ctx, f, o); if (f == i->fmt) o->formatProperties.optimalTilingFeatures &= ~i->clear; }
static VkResult inj_ifp(void *c, const VkPhysicalDeviceImageFormatInfo2 *in, VkImageFormatProperties2 *o) {
    inj_t *i = c;
    if (in->format == i->fmt && ((i->refuse_any && (in->usage & i->refuse_any)) || (i->refuse_all && (in->usage & i->refuse_all) == i->refuse_all))) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    return i->real.image_format_props2(i->real.ctx, in, o);
}
static void inject(VkFormat f, VkFormatFeatureFlags clear, VkImageUsageFlags any, VkImageUsageFlags all) {
    static lagfx_fmtcap_backend_t st;
    const lagfx_fmtcap_backend_t *real = lagfx_fmtcap_device_backend(g_dev->vk->phys_device, &st);
    g_inj = (inj_t){ *real, f, clear, any, all };
    g_inj_be = (lagfx_fmtcap_backend_t){ &g_inj, inj_fp, inj_ifp };
    lagfx_fmtcap_test_backend = &g_inj_be;
}
static void uninject(void) { lagfx_fmtcap_test_backend = NULL; }

/* run the premultiplied source-over draw of fs_half over a blue clear on `ref`'s texture and return counters + pixel */
static void b_half(void) { v_tris(1); cb_draw_p(3, 0, 3, 1); }
static void test_injected_formats(void) {
    static rc_res_t r; const uint32_t refs8[2] = { REF_TEX, 0 }, refs16[2] = { REF_TEX2, 0 };
    g_rc.fs = FS_HALF;
    /* blend: positive control (device blends BGRA8): source (0.5,0,0,0.5) over blue (0,0,1,1) = (0.5,0,0.5,1) */
    g_rc.blend = true;
    rc_run(refs8, CBLUE, NULL, b_half, true, &r);
    uint32_t v = px(r.bgra, 8, 8);
    assert(r.draws == 1 && r.blend_off == 0 && (v >> 24) == 255 && abs((int)((v >> 16) & 255) - 128) <= 1 && (v & 255) >= 127 && (v & 255) <= 128 && ((v >> 8) & 255) == 0);
    /* negative: the injected device has no COLOR_ATTACHMENT_BLEND for BGRA8: blending is switched off and counted, the source replaces the target (0.5,0,0,0.5) */
    inject(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT, 0, 0);
    rc_run(refs8, CBLUE, NULL, b_half, true, &r);
    v = px(r.bgra, 8, 8);
    assert(r.draws == 1 && r.blend_off == 1 && abs((int)(v >> 24) - 128) <= 1 && abs((int)((v >> 16) & 255) - 128) <= 1 && (v & 255) == 0);
    /* a format that blends is unaffected by an injection on another format (RGBA16F target, BGRA8 injected) */
    rc_run(refs16, CBLUE, NULL, b_half, true, &r);
    assert(r.draws == 1 && r.blend_off == 0 && h16(r.half, 8, 8, 3) == 0x3c00 && h16(r.half, 8, 8, 2) == 0x3800);   /* blue 1 * (1-0.5) = 0.5, alpha 1.0 */
    uninject();

    /* the device cannot render to BGRA8: the texture is created without COLOR/INPUT usage (counted as trimmed), the pass names it: target_not_attachable + no_target, no draw */
    g_rc.blend = false;
    inject(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT, 0, 0);
    rc_run(refs8, CBLUE, NULL, b_half, true, &r);
    assert(r.target_na == 1 && r.no_target >= 1 && r.draws == 0);
    uninject();
    rc_run(refs8, CBLUE, NULL, b_half, true, &r);   /* positive control: the same scene on the real device renders */
    { uint32_t q = px(r.bgra, 8, 8); assert(r.target_na == 0 && r.no_target == 0 && r.draws == 1 && abs((int)(q >> 24) - 128) <= 1 && abs((int)((q >> 16) & 255) - 128) <= 1 && (q & 255) == 0); }

    /* a refused usage COMBINATION (colour attachment + input attachment together) on RGBA16F: the input attachment is dropped, rendering still works; the image is made, counted as trimmed */
    inject(VK_FORMAT_R16G16B16A16_SFLOAT, 0, 0, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
    rc_run(refs16, CBLUE, NULL, b_half, true, &r);
    assert(r.draws == 1 && r.pipe_fail == 0 && r.no_target == 0);
    uninject();
    puts("PASS: injected less-capable devices: blend feature missing -> blend off + counted; colour attachment missing -> counted refusal of the pass; refused usage combination -> trimmed image still renders");
}

/* A guest texture whose descriptor names 4 samples is refused (counted); a pass naming it renders nothing and nothing crashes. */
static void test_msaa_texture_refused(void) {
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    lagfx_task_entry_t *t = def_task(p, 1); (void)t;
    build_objects();
    put_tex(REF_TEX2, VPN_TEX2, 115, 8, 4);   /* RGBA16Float, 4 samples */
    g_res_tex2 = true;
    g_rc.fs = FS_HALF; g_rc.blend = false;
    lagfx_a64_exec_set_pipeline_resolver(p, rc_resolver, NULL);
    const uint32_t refs[2] = { REF_TEX2, 0 }; const uint16_t loads[2] = { 2, 2 }; const double clr[2][4] = { { 0, 0, 1, 1 }, { 0, 0, 0, 0 } };
    cb_reset(); cb_pass_n(refs, loads, clr); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, 0); b_half();
    exec(p, 1, cb_finish(), true);
    assert(p->diag2.exec.fmt.msaa_refused >= 1 && p->diag2.exec.run.no_target >= 1 && p->diag2.exec.run.draws == 0);
    lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); free(p);
    g_res_tex2 = false;
    /* control: the same texture with 1 sample is accepted */
    lagfx_protocol_t *q = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    def_task(q, 1); build_objects(); put_tex(REF_TEX2, VPN_TEX2, 115, 8, 1); g_res_tex2 = true;
    lagfx_a64_exec_set_pipeline_resolver(q, rc_resolver, NULL);
    cb_reset(); cb_pass_n(refs, loads, clr); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, 0); b_half();
    exec(q, 1, cb_finish(), true);
    assert(q->diag2.exec.fmt.msaa_refused == 0 && q->diag2.exec.run.draws == 1);
    lagfx_a64_exec_drop_task(q, 1); lagfx_a64_exec_free(q); free(q);
    g_res_tex2 = false;
    puts("PASS: a guest texture descriptor with samples > 1 is refused with a dedicated counter (a sample count on a non-multisample texture type is not a texture Metal can create; the real multisample path is tested in arm64-msaa-guest-unit.c); 1 sample is accepted");
}

#ifndef LAGFX_COMPAT_NO_MAIN   /* arm64-msaa-guest-unit.c includes this file as its harness and has its own main */
int main(void) {
    g_mem = calloc(1, MEM_SIZE);
    g_dev = make_device();
    assert(g_dev);
    if (!g_dev->vk || !g_dev->vk->initialized) { puts("SKIP: no Vulkan device"); lagfx_device_free(g_dev); free(g_mem); return 77; }
    printf("device: local_read=%d raster_order=%d\n", (int)g_dev->vk->have_local_read, (int)g_dev->vk->have_raster_order);
    if (!g_dev->vk->have_local_read) { puts("SKIP: no dynamicRenderingLocalRead on this device (framebuffer fetch tests need it)"); return 77; }
    test_split_scenes();
    test_split_strip_and_cap();
    test_split_mrt();
    test_fetch_class();
    test_clear_integer_class();
    test_viewport_classes();
    test_viewport_semantics();
    test_discard_fragment();
    test_discard_loop_plans();
    test_discard_derivatives();
    test_round_halves_away();
    test_injected_formats();
    test_msaa_texture_refused();
    lagfx_device_free(g_dev);
    free(g_mem);
    return 0;
}
#endif /* LAGFX_COMPAT_NO_MAIN */
#else
int main(void) { puts("SKIP: built without Vulkan"); return 77; }
#endif
