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

/* ---- scenario state read by the resolver ---- */
enum { FS_F, FS_U, FS_FACE, FS_MRT, FS_HALF };
static struct { int fs; bool blend; } g_rc;
static VkShaderModule g_rc_fs[5]; static VkDescriptorSetLayout g_rc_dsl[2]; static VkPipelineLayout g_rc_pl[2];

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
    if (g_rc.fs == FS_HALF) {
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

typedef struct { uint8_t bgra[RT_W * RT_H * 4]; uint8_t half[RT_W * RT_H * 8]; uint32_t u32[RT_W * RT_H]; uint64_t split_draws, subdraws, capped, unsupported, class_mismatch, draws, skipped, no_target, target_na, blend_off, pipe_fail; uint64_t val_msgs; } rc_res_t;
typedef void (*rc_body_t)(void);
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
    uint64_t v0 = lagfx_vk_validation_msg_count;
    cb_reset(); cb_pass_n(refs, loads, clr); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, 0);
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
    out->val_msgs = lagfx_vk_validation_msg_count - v0;
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
    static rc_res_t r; const uint32_t refs_u[2] = { REF_TEX3, 0 };
    g_rc.fs = FS_U;
    const double c7[4] = { 7, 0, 0, 0 };
    rc_run(refs_u, c7, NULL, b_none, false, &r);
    uint32_t bad = 0; for (uint32_t i = 0; i < RT_W * RT_H; i++) bad += r.u32[i] != 7u;
    assert(bad == 0 && r.val_msgs == 0);
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
    puts("PASS: an integer colour attachment is cleared through the uint32 / int32 member by its numeric class (R32Uint 7 -> 7; saturating); a float target keeps float32");
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
    puts("PASS: a guest texture descriptor with samples > 1 is refused with a dedicated counter (the Vulkan MSAA machinery is not wired to the PVG field: see src/vulkan/msaa.h); 1 sample is accepted");
}

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
    test_injected_formats();
    test_msaa_texture_refused();
    lagfx_device_free(g_dev);
    free(g_mem);
    return 0;
}
#else
int main(void) { puts("SKIP: built without Vulkan"); return 77; }
#endif
