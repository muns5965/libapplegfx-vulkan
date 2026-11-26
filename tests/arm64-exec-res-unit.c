/* ARM64 render exec: shader-written textures (storage images), vertex-stage textures / samplers (tags 127 / 129), unnormalized-coordinates samplers (explicit
 * LOD 0 fragment variant) and writable storage buffers at unaligned offsets. Lavapipe, with the Vulkan validation layer when LAGFX_VK_VALIDATION=1. Reuses the
 * scaffolding of arm64-exec-run-unit.c; the pipelines come from this file's own resolver (hand-written GLSL, see arm64-exec-res-shaders.h).
 * SPDX-License-Identifier: MIT */
#define main run_unit_main
#include "arm64-exec-run-unit.c"
#undef main

#ifdef LAGFX_HAVE_VULKAN
#include "arm64-exec-res-shaders.h"
#include "air2spv/spv_reflect.h"

#define REF_SAMP 8u
enum { M_STORE = 1, M_VTEX, M_UNORM, M_SBUF };
static struct { int mode, no_alt, writable; VkShaderModule m[8]; } g_r;
static VkShaderModule g_mods[8];
static VkDescriptorSetLayout g_dsl[8]; static VkPipelineLayout g_pl[8];
static uint8_t *g_alt_spv; static size_t g_alt_bytes;

static VkShaderModule mod(struct lagfx_vk_state *vk, int slot, const uint32_t *code, size_t bytes) {
    if (!g_mods[slot]) {
        VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code };
        assert(vkCreateShaderModule(vk->device, &ci, NULL, &g_mods[slot]) == VK_SUCCESS);
    }
    return g_mods[slot];
}
static VkPipelineLayout layout(struct lagfx_vk_state *vk, int slot, const VkDescriptorSetLayoutBinding *b, uint32_t n, VkDescriptorSetLayout *dsl) {
    if (!g_pl[slot]) {
        VkDescriptorSetLayoutCreateInfo dl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = n, .pBindings = b };
        assert(vkCreateDescriptorSetLayout(vk->device, &dl, NULL, &g_dsl[slot]) == VK_SUCCESS);
        VkPipelineLayoutCreateInfo pl = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_dsl[slot] };
        assert(vkCreatePipelineLayout(vk->device, &pl, NULL, &g_pl[slot]) == VK_SUCCESS);
    }
    *dsl = g_dsl[slot];
    return g_pl[slot];
}
static void setb(lagfx_a64_pipeline_src_t *o, uint32_t i, uint32_t binding, uint32_t kind, uint32_t stage, int mi) {
    o->bind[i].binding = (uint8_t)binding, o->bind[i].kind = (uint8_t)kind, o->bind[i].stage = (uint8_t)stage, o->bind[i].metal_index = (int16_t)mi;
}
static bool res_resolver(void *c, lagfx_protocol_t *p, const lagfx_task_entry_t *t, uint32_t ref, const uint32_t e[3], lagfx_a64_pipeline_src_t *out) {
    (void)c; (void)t; (void)ref; (void)e;
    struct lagfx_vk_state *vk = ((lagfx_device_t *)p->dev)->vk;
    memset(out, 0, sizeof *out);
    out->desc.vertex_shader = mod(vk, 0, k_vs_spv, sizeof k_vs_spv), out->desc.fragment_shader = mod(vk, 1, k_fs_spv, sizeof k_fs_spv);
    out->desc.layout = vk->empty_layout;
    out->desc.n_vtx_inputs = 1, out->desc.vtx_in_loc[0] = 0, out->desc.vtx_in_comp[0] = 2, out->desc.vtx_stride = 8;
    out->vertex_buffer_index = 0;
    const VkShaderStageFlags F = VK_SHADER_STAGE_FRAGMENT_BIT, V = VK_SHADER_STAGE_VERTEX_BIT;
    switch (g_r.mode) {
    case M_STORE: {
        VkDescriptorSetLayoutBinding b[1] = { { .binding = 24, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = F } };
        out->desc.fragment_shader = mod(vk, 2, k_fs_store_spv, sizeof k_fs_store_spv);
        out->desc.layout = layout(vk, 2, b, 1, &out->dsl);
        out->n_bind = 1, setb(out, 0, 24, LAGFX_A64_BIND_STORAGE_IMAGE, 1, 0);
        break;
    }
    case M_VTEX: {
        VkDescriptorSetLayoutBinding b[2] = { { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 1, .stageFlags = V },
                                              { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 1, .stageFlags = V } };
        out->desc.vertex_shader = mod(vk, 3, k_vs_tex_spv, sizeof k_vs_tex_spv);
        out->desc.layout = layout(vk, 3, b, 2, &out->dsl);
        out->n_bind = 2, setb(out, 0, 0, LAGFX_A64_BIND_IMAGE, 0, 0), setb(out, 1, 1, LAGFX_A64_BIND_SAMPLER, 0, 0);
        break;
    }
    case M_UNORM: {
        VkDescriptorSetLayoutBinding b[2] = { { .binding = 24, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 1, .stageFlags = F },
                                              { .binding = 25, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 1, .stageFlags = F } };
        out->desc.fragment_shader = mod(vk, 4, k_fs_unorm_spv, sizeof k_fs_unorm_spv);
        out->desc.layout = layout(vk, 4, b, 2, &out->dsl);
        out->n_bind = 2, setb(out, 0, 24, LAGFX_A64_BIND_IMAGE, 1, 0), setb(out, 1, 25, LAGFX_A64_BIND_SAMPLER, 1, 0);
        if (!g_r.no_alt) {   /* exactly what arm64_pipeline.c does: the same SPIR-V rewritten to explicit LOD 0 */
            if (!g_alt_spv) assert(lagfx_spv_explicit_lod0((const uint8_t *)k_fs_unorm_spv, sizeof k_fs_unorm_spv, &g_alt_spv, &g_alt_bytes) == 1);
            out->alt_fragment_shader = mod(vk, 5, (const uint32_t *)g_alt_spv, g_alt_bytes);
        }
        break;
    }
    case M_SBUF: {
        VkDescriptorSetLayoutBinding b[1] = { { .binding = 24, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = F } };
        out->desc.fragment_shader = mod(vk, 6, k_fs_sbuf_spv, sizeof k_fs_sbuf_spv);
        out->desc.layout = layout(vk, 6, b, 1, &out->dsl);
        out->n_bind = 1, setb(out, 0, 24, LAGFX_A64_BIND_BUFFER, 1, 0);
        out->bind[0].writable = (uint8_t)g_r.writable;
        break;
    }
    default: break;
    }
    return true;
}

static void p32(uint8_t *b, uint32_t o, uint32_t v) { memcpy(b + o, &v, 4); }
/* the texture of REF_TEX2 (RGBA16Float, 16 x 16): pixel (x, y) = left for x < split, else right (4 half floats each) */
static void fill_tex2(const uint16_t left[4], const uint16_t right[4], uint32_t split) {
    static uint16_t px[RT_W * RT_H * 4];
    for (uint32_t y = 0; y < RT_H; y++) for (uint32_t x = 0; x < RT_W; x++) memcpy(px + (y * RT_W + x) * 4, x < split ? left : right, 8);
    va_write((uint64_t)VPN_TEX2 << 14, px, sizeof px);
}
static const uint16_t H_RED[4] = { 0x3c00, 0, 0, 0x3c00 }, H_GREEN[4] = { 0, 0x3c00, 0, 0x3c00 }, H_ZERO[4] = { 0, 0, 0, 0 };

static void k_cmds_store(void) { uint8_t r[12]; p32(r, 0, 0), p32(r, 4, 1), p32(r, 8, REF_TEX2); cb_cmd(114, r, 12); cb_draw(0, 3); }
static void k_cmds_vtex(void) {
    uint8_t r[12]; p32(r, 0, 0), p32(r, 4, 1), p32(r, 8, REF_TEX2); cb_cmd(129, r, 12);
    p32(r, 8, REF_SAMP); cb_cmd(127, r, 12);
    cb_draw(0, 3);
}
static void k_cmds_vtex_lod(void) {   /* a LOD-clamp bind of the vertex sampler (tag 128) instead of the plain one */
    uint8_t r[12]; p32(r, 0, 0), p32(r, 4, 1), p32(r, 8, REF_TEX2); cb_cmd(129, r, 12);
    uint8_t q[20]; p32(q, 0, 0), p32(q, 4, 1), p32(q, 8, REF_SAMP); float lo = 0.f, hi = 2.f; memcpy(q + 12, &lo, 4), memcpy(q + 16, &hi, 4); cb_cmd(128, q, 20);
    cb_draw(0, 3);
}
static void k_cmds_unorm(void) {
    uint8_t r[12]; p32(r, 0, 0), p32(r, 4, 1), p32(r, 8, REF_TEX2); cb_cmd(114, r, 12);
    p32(r, 8, REF_SAMP); cb_cmd(112, r, 12);
    cb_draw(0, 3);
}
static uint64_t g_sb_off;
static void k_cmds_sbuf(void) {
    uint8_t r[20]; p32(r, 0, 0), p32(r, 4, 1), p32(r, 8, REF_VB); memcpy(r + 12, &g_sb_off, 8); cb_cmd(110, r, 20);
    cb_draw(0, 3);
}

static lagfx_protocol_t *res_run(int mode, uint32_t samp_state) {
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    g_r.mode = mode;
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    def_task(p, 1);
    build_objects();
    uint8_t sm[36]; memset(sm, 0, sizeof sm);
    p32(sm, 0, 3), p32(sm, 4, 36), p32(sm, 8, REF_SAMP), p32(sm, 12, samp_state);
    float lo = 0.f, hi = 3.4028235e38f; memcpy(sm + 20, &lo, 4), memcpy(sm + 24, &hi, 4);
    va_write(VA_PAY + 0x500, sm, sizeof sm);
    entry(REF_SAMP, LAGFX_A64_OBJ_SERIALIZED, 36, VA_PAY + 0x500);
    lagfx_a64_exec_set_pipeline_resolver(p, res_resolver, NULL);
    return p;   /* the caller fills the texture, then calls res_exec */
}
static void res_exec(lagfx_protocol_t *p, void (*cmds)(void), uint8_t *img, uint8_t *img2) {
    static const double green[4] = { 0, 0, 1, 1 };   /* clear: blue, so a skipped draw is told from a red / green one */
    cb_reset(); cb_pass(REF_TEX, 2, green); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, 0);
    cmds();
    g_res_tex2 = true;
    exec(p, 1, cb_finish(), true);
    assert(p->diag2.exec.run.submit_failed == 0);
    lagfx_task_entry_t *te = lagfx_protocol_find_task(p, 1);
    assert(te && lagfx_a64_exec_readback_texture(p, te, REF_TEX, img, RT_W * RT_H * 4, NULL, NULL));
    if (img2) assert(lagfx_a64_exec_readback_texture(p, te, REF_TEX2, img2, RT_W * RT_H * 8, NULL, NULL));
}
#define BLUE 0xff0000ffu
static uint64_t px8(const uint8_t *img, uint32_t x, uint32_t y) { uint64_t v; memcpy(&v, img + (y * RT_W + x) * 8, 8); return v; }
static void done(lagfx_protocol_t *p) { lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); free(p); }

static void test_store(void) {
    uint8_t img[RT_W * RT_H * 4], img2[RT_W * RT_H * 8];
    lagfx_protocol_t *p = res_run(M_STORE, 0x86000000u);
    fill_tex2(H_ZERO, H_ZERO, 0);
    res_exec(p, k_cmds_store, img, img2);
    __typeof__(p->diag2.exec.run) *R = &p->diag2.exec.run;
    assert(R->draws == 1 && R->draws_skipped == 0 && px(img, 5, 8) == GREEN && p->diag2.exec.pipe.desc_sets >= 1);
    /* the fragment at (5, 8) wrote (1, .5, .25, 1) as half floats into the second texture; pixels outside the triangle stay as uploaded (0) */
    assert(px8(img2, 5, 8) == ((uint64_t)0x3c00 | (uint64_t)0x3800 << 16 | (uint64_t)0x3400 << 32 | (uint64_t)0x3c00 << 48));
    assert(px8(img2, 0, 0) == 0 && px8(img2, 15, 15) == 0);
    done(p);
    puts("PASS: air.write_texture_2d shape: storage image descriptor written by a fragment shader (lavapipe pixels)");
}

static void test_vertex_textures(void) {
    uint8_t img[RT_W * RT_H * 4];
    lagfx_protocol_t *p = res_run(M_VTEX, 0x86000000u);
    fill_tex2(H_RED, H_RED, 16);   /* r = 1 at the sample point: the triangle stays on screen */
    res_exec(p, k_cmds_vtex, img, NULL);
    __typeof__(p->diag2.exec.run) *R = &p->diag2.exec.run;
    assert(R->draws == 1 && R->draws_skipped == 0 && R->tag_done[129] == 1 && R->tag_done[127] == 1 && R->ext_vs_tex_binds == 2 && px(img, 5, 8) == RED);
    done(p);
    p = res_run(M_VTEX, 0x86000000u);
    fill_tex2(H_ZERO, H_ZERO, 16);   /* r = 0: the vertex shader moves the triangle off screen */
    res_exec(p, k_cmds_vtex, img, NULL);
    assert(p->diag2.exec.run.draws == 1 && px(img, 5, 8) == BLUE);
    done(p);
    p = res_run(M_VTEX, 0x86000000u);   /* tag 128 binds the sampler (with LOD clamps) for the vertex stage */
    fill_tex2(H_RED, H_RED, 16);
    res_exec(p, k_cmds_vtex_lod, img, NULL);
    assert(p->diag2.exec.run.draws == 1 && p->diag2.exec.run.tag_done[128] == 1 && p->diag2.exec.run.tag_unsup[128] == 0 && px(img, 5, 8) == RED);
    done(p);
    /* nothing bound for the vertex stage: the draw is skipped and counted, not recorded with a stale descriptor */
    p = res_run(M_VTEX, 0x86000000u);
    fill_tex2(H_RED, H_RED, 16);
    res_exec(p, k_cmds_store, img, NULL);
    assert(p->diag2.exec.run.draws == 0 && p->diag2.exec.run.draws_skipped == 1 && p->diag2.exec.pipe.desc_unbound == 1);
    done(p);
    puts("PASS: vertex-stage texture + sampler tables (tags 129 / 127 / 128) feed a vertex shader that samples with explicit LOD");
}

static void test_unnormalized(void) {
    uint8_t img[RT_W * RT_H * 4];
    g_r.no_alt = 0;
    lagfx_protocol_t *p = res_run(M_UNORM, 0x06000000u);   /* bit 31 clear = unnormalized coordinates */
    fill_tex2(H_RED, H_GREEN, 8);
    res_exec(p, k_cmds_unorm, img, NULL);
    __typeof__(p->diag2.exec.run) *R = &p->diag2.exec.run;
    assert(R->draws == 1 && R->ext_unnorm_lod0 == 1 && R->ext_samp_unnorm == 1);
    assert(px(img, 5, 8) == RED && px(img, 10, 8) == GREEN);   /* coordinates are texels: left half red, right half green (clamped normalized ones would give one colour) */
    done(p);
    p = res_run(M_UNORM, 0x86000000u);   /* a normalized sampler keeps the implicit-LOD shader */
    fill_tex2(H_RED, H_GREEN, 8);
    res_exec(p, k_cmds_unorm, img, NULL);
    assert(p->diag2.exec.run.draws == 1 && p->diag2.exec.run.ext_unnorm_lod0 == 0 && px(img, 5, 8) == px(img, 10, 8));
    done(p);
    g_r.no_alt = 1; memset(&g_mods[5], 0, sizeof g_mods[5]);
    p = res_run(M_UNORM, 0x06000000u);   /* no explicit-LOD variant available: skipped and counted (never an invalid draw) */
    fill_tex2(H_RED, H_GREEN, 8);
    res_exec(p, k_cmds_unorm, img, NULL);
    assert(p->diag2.exec.run.draws == 0 && p->diag2.exec.run.ext_unnorm_skipped == 1 && px(img, 5, 8) == BLUE);
    done(p);
    g_r.no_alt = 0;
    /* the rewrite itself */
    uint8_t *o = NULL; size_t n = 0;
    assert(lagfx_spv_explicit_lod0((const uint8_t *)k_fs_spv, sizeof k_fs_spv, &o, &n) == 0 && !o);   /* a shader without samples is left alone */
    assert(lagfx_spv_explicit_lod0((const uint8_t *)k_fs_unorm_spv, sizeof k_fs_unorm_spv, &o, &n) == 1 && o && n == sizeof k_fs_unorm_spv + 8 + 16);   /* + Lod operands, + one 0.0 constant */
    {
        uint32_t impl = 0, expl = 0; const uint32_t *w = (const uint32_t *)o;
        for (size_t i = 5; i < n / 4;) { uint32_t op = w[i] & 0xffff, wc = w[i] >> 16; impl += op == 87, expl += op == 88; i += wc; }
        assert(impl == 0 && expl == 1);
    }
    free(o);
    puts("PASS: unnormalized sampler: explicit-LOD fragment variant selected per draw (texel coordinates), normalized samplers unchanged, missing variant skipped + counted");
}

static void test_writable_buffer(void) {
    uint8_t img[RT_W * RT_H * 4];
    VkPhysicalDeviceProperties pr;
    vkGetPhysicalDeviceProperties(g_dev->vk->phys_device, &pr);
    if (pr.limits.minStorageBufferOffsetAlignment <= 4u) { puts("SKIP: writable buffer (the device accepts any 4-aligned storage buffer offset)"); return; }
    for (int w = 0; w < 2; w++) {
        g_r.writable = w;
        g_sb_off = 4;
        lagfx_protocol_t *p = res_run(M_SBUF, 0x86000000u);
        fill_tex2(H_ZERO, H_ZERO, 0);
        res_exec(p, k_cmds_sbuf, img, NULL);
        __typeof__(p->diag2.exec.run) *R = &p->diag2.exec.run;
        if (w) assert(R->draws == 0 && R->ext_wsb_skipped == 1 && R->ext_gathered == 0 && p->diag2.exec.pipe.desc_unaligned == 1 && px(img, 5, 8) == BLUE);   /* may be written: no private copy */
        else assert(R->draws == 1 && R->ext_wsb_skipped == 0 && R->ext_gathered == 1 && px(img, 5, 8) == RED);                                              /* read only: the aligned copy */
        done(p);
        g_sb_off = 0;
        p = res_run(M_SBUF, 0x86000000u);   /* an aligned offset binds the mirror itself, writable or not */
        fill_tex2(H_ZERO, H_ZERO, 0);
        res_exec(p, k_cmds_sbuf, img, NULL);
        assert(p->diag2.exec.run.draws == 1 && p->diag2.exec.run.ext_wsb_skipped == 0 && px(img, 5, 8) == RED);
        done(p);
    }
    g_r.writable = 0;
    puts("PASS: a storage buffer the shader may write is never gather-copied: skipped + counted at an unaligned offset");
}

int main(void) {
    g_mem = calloc(1, MEM_SIZE);
    g_dev = make_device();
    assert(g_dev);
    if (!(g_dev->vk && g_dev->vk->initialized)) { puts("SKIP: no Vulkan device"); lagfx_device_free(g_dev); free(g_mem); return 77; }
    if (!g_dev->vk->have_storage_img_write) { puts("SKIP: no storage image write without format"); lagfx_device_free(g_dev); free(g_mem); return 77; }
    test_store();
    test_vertex_textures();
    test_unnormalized();
    test_writable_buffer();
    lagfx_device_free(g_dev);
    free(g_mem);
    return 0;
}
#else
int main(void) { puts("SKIP: built without Vulkan"); return 77; }
#endif
