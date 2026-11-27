/* ARM64 render exec: sampled texture views over the descriptor's real mip chain and array layers. Lavapipe, with the Vulkan validation layer when LAGFX_VK_VALIDATION=1.
 * The mirror image is created with the guest descriptor's mip count / array length and every level / slice is uploaded; the sampled view covers all of them (a 2D view of layer 0
 * for a texture2d shader, a 2D_ARRAY view of every layer for a texture2d_array shader). Every (layer, level) holds its own solid colour, so what a draw reads is told from the
 * pixel: a fragment shader (hand-written GLSL, arm64-exec-texview-shaders.h) samples with explicit LOD = column and, for the array shader, layer = row of the 16 x 16 target,
 * and every pixel is compared with a CPU model of the texture (colour table) and of the sampler's LOD clamps. Negative controls: a sampler without a mip filter, a texture
 * with one level, an array shader on a plain 2D texture, a 2D shader on an array texture (layer 0, counted), LOD clamps of a tag-113 sampler bind.
 * Reuses the scaffolding of arm64-exec-run-unit.c. SPDX-License-Identifier: MIT */
#define main run_unit_main
#include "arm64-exec-run-unit.c"
#undef main

#ifdef LAGFX_HAVE_VULKAN
#include "arm64-exec-texview-shaders.h"
#include "arm64-exec-res-shaders.h"   /* k_fs_unorm_spv: texture(sampler2D(tex@24, smp@25), gl_FragCoord.xy), implicit LOD */

#define REF_SAMP 8u
extern uint64_t lagfx_vk_validation_msg_count;
static struct { int arr; VkShaderModule m[2], fsm[3]; VkDescriptorSetLayout dsl; VkPipelineLayout pl; } g_t;

static bool tv_resolver(void *c, lagfx_protocol_t *p, const lagfx_task_entry_t *t, uint32_t ref, const uint32_t e[3], lagfx_a64_pipeline_src_t *out) {
    (void)c; (void)t; (void)ref; (void)e;
    struct lagfx_vk_state *vk = ((lagfx_device_t *)p->dev)->vk;
    memset(out, 0, sizeof *out);
    if (!g_t.m[0]) {
        VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof k_vs_spv, .pCode = k_vs_spv };
        assert(vkCreateShaderModule(vk->device, &ci, NULL, &g_t.m[0]) == VK_SUCCESS);
    }
    VkShaderModule *fsm = g_t.fsm;   /* one per shader */
    if (!fsm[g_t.arr]) {
        const uint32_t *code = g_t.arr == 2 ? k_fs_unorm_spv : g_t.arr ? k_fs_arr_spv : k_fs_lod2d_spv;
        size_t bytes = g_t.arr == 2 ? sizeof k_fs_unorm_spv : g_t.arr ? sizeof k_fs_arr_spv : sizeof k_fs_lod2d_spv;
        VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code };
        assert(vkCreateShaderModule(vk->device, &ci, NULL, &fsm[g_t.arr]) == VK_SUCCESS);
    }
    if (!g_t.pl) {
        VkDescriptorSetLayoutBinding b[2] = { { .binding = 24, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
                                              { .binding = 25, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT } };
        VkDescriptorSetLayoutCreateInfo dl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b };
        assert(vkCreateDescriptorSetLayout(vk->device, &dl, NULL, &g_t.dsl) == VK_SUCCESS);
        VkPipelineLayoutCreateInfo pl = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_t.dsl };
        assert(vkCreatePipelineLayout(vk->device, &pl, NULL, &g_t.pl) == VK_SUCCESS);
    }
    out->desc.vertex_shader = g_t.m[0], out->desc.fragment_shader = fsm[g_t.arr];
    out->desc.layout = g_t.pl, out->dsl = g_t.dsl;
    out->desc.n_vtx_inputs = 1, out->desc.vtx_in_loc[0] = 0, out->desc.vtx_in_comp[0] = 2, out->desc.vtx_stride = 8;
    out->vertex_buffer_index = 0;
    out->n_bind = 2;
    out->bind[0].binding = 24, out->bind[0].kind = LAGFX_A64_BIND_IMAGE, out->bind[0].stage = 1, out->bind[0].metal_index = 0, out->bind[0].arr = (uint8_t)(g_t.arr == 1);
    if (g_t.arr == 2) out->desc.fragment_spv = k_fs_unorm_spv, out->desc.fragment_spv_bytes = sizeof k_fs_unorm_spv;   /* what arm64_pipeline.c hands over: the exec rewrites it per unnormalized use (spv_unnorm.c) */
    out->bind[1].binding = 25, out->bind[1].kind = LAGFX_A64_BIND_SAMPLER, out->bind[1].stage = 1, out->bind[1].metal_index = 0;
    return true;
}

static void p32(uint8_t *b, uint32_t o, uint32_t v) { memcpy(b + o, &v, 4); }

/* The CPU model of the guest texture: every (layer, level) is one solid colour; the u32 is what the render target reads back (bytes B G R A of a BGRA8 texel). */
static uint32_t col(uint32_t layer, uint32_t level) { return 255u << 24 | (40u + 60u * level) << 16 | (40u + 60u * layer) << 8 | 0x80u; }

#define TEX_W 8u
/* A BGRA8 texture of 8 x 8, `mips` levels and `layers` slices (mtype 2 = 2D, 3 = 2D array) in the mirror layout: level l at offset sum(prev) of each slice, slice stride = the whole chain. */
static void put_tex(uint32_t mtype, uint32_t mips, uint32_t layers) {
    uint32_t lvl_off[8], tot = 0;
    for (uint32_t l = 0; l < mips; l++) { lvl_off[l] = tot; tot += (TEX_W >> l) * (TEX_W >> l) * 4u; }
    static uint32_t data[4096];
    memset(data, 0, sizeof data);
    for (uint32_t s = 0; s < layers; s++) for (uint32_t l = 0; l < mips; l++)
        for (uint32_t i = 0; i < (TEX_W >> l) * (TEX_W >> l); i++) data[(s * tot + lvl_off[l]) / 4u + i] = col(s, l);
    va_write((uint64_t)VPN_TEX3 << 14, data, layers * tot);
    uint8_t t[512]; memset(t, 0, sizeof t);
    uint64_t tlen = (uint64_t)layers * tot, stride = tot; uint32_t tvp = VPN_TEX3;
    memcpy(t, &tlen, 8); memcpy(t + 8, &tvp, 4);
    uint16_t m16 = (uint16_t)mips, s16 = (uint16_t)layers; memcpy(t + 12, &m16, 2); memcpy(t + 14, &s16, 2); memcpy(t + 12 + 12, &stride, 8);
    for (uint32_t l = 0; l < mips; l++) {
        uint8_t *mp = t + 12 + 0x18 + 36u * l; uint64_t off = lvl_off[l], row = (TEX_W >> l) * 4u; uint32_t mw = TEX_W >> l, mh = TEX_W >> l, md = 1;
        memcpy(mp, &off, 8); memcpy(mp + 0x10, &row, 8); memcpy(mp + 0x18, &mw, 4); memcpy(mp + 0x1c, &mh, 4); memcpy(mp + 0x20, &md, 4);
    }
    uint32_t rec = 12 + 0x18 + 36u * mips, tag = 0x14;
    memcpy(t + rec, &tag, 4);
    uint8_t *d = t + rec + 12;
    d[0] = (uint8_t)mtype; d[2] = 80;
    uint32_t w = TEX_W, h = TEX_W, dep = 1; memcpy(d + 4, &w, 4); memcpy(d + 8, &h, 4); memcpy(d + 12, &dep, 4);
    uint16_t m2 = (uint16_t)mips, sm = 1, ar = (uint16_t)layers; memcpy(d + 16, &m2, 2); memcpy(d + 18, &sm, 2); memcpy(d + 20, &ar, 2);
    uint32_t tsize = rec + 12 + 28;
    va_write(VA_PAY + 0x600, t, tsize);
    entry(REF_TEX3, LAGFX_A64_OBJ_TEXTURE, tsize, VA_PAY + 0x600);
}

static lagfx_protocol_t *tv_run(int arr, uint32_t samp_state, uint32_t mtype, uint32_t mips, uint32_t layers) {
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    g_t.arr = arr;
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    def_task(p, 1);
    build_objects();
    const float v[6] = { -1.f, -1.f, 3.f, -1.f, -1.f, 3.f };   /* one triangle that covers the whole target: every cell is drawn */
    va_write((uint64_t)VPN_VB << 14, v, sizeof v);
    uint8_t sm[36]; memset(sm, 0, sizeof sm);
    p32(sm, 0, 3), p32(sm, 4, 36), p32(sm, 8, REF_SAMP), p32(sm, 12, samp_state);
    float lo = 0.f, hi = 3.4028235e38f; memcpy(sm + 20, &lo, 4), memcpy(sm + 24, &hi, 4);
    va_write(VA_PAY + 0x500, sm, sizeof sm);
    entry(REF_SAMP, LAGFX_A64_OBJ_SERIALIZED, 36, VA_PAY + 0x500);
    put_tex(mtype, mips, layers);
    g_res_tex3 = true;
    lagfx_a64_exec_set_pipeline_resolver(p, tv_resolver, NULL);
    return p;
}
static float g_clamp_lo = -1.f, g_clamp_hi = -1.f;   /* >= 0: bind the fragment sampler with tag 113 (LOD clamps) instead of tag 112 */
static void tv_cmds(void) {
    uint8_t r[12]; p32(r, 0, 0), p32(r, 4, 1), p32(r, 8, REF_TEX3); cb_cmd(114, r, 12);
    if (g_clamp_lo >= 0.f) { uint8_t q[20]; p32(q, 0, 0), p32(q, 4, 1), p32(q, 8, REF_SAMP); memcpy(q + 12, &g_clamp_lo, 4), memcpy(q + 16, &g_clamp_hi, 4); cb_cmd(113, q, 20); }
    else { p32(r, 8, REF_SAMP); cb_cmd(112, r, 12); }
    cb_draw(0, 3);
}
static void tv_exec(lagfx_protocol_t *p, uint8_t *img) {
    static const double blue[4] = { 0, 0, 1, 1 };   /* a skipped draw leaves the clear colour */
    cb_reset(); cb_pass(REF_TEX, 2, blue); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, 0);
    tv_cmds();
    exec(p, 1, cb_finish(), true);
    assert(p->diag2.exec.run.submit_failed == 0);
    lagfx_task_entry_t *te = lagfx_protocol_find_task(p, 1);
    assert(te && lagfx_a64_exec_readback_texture(p, te, REF_TEX, img, RT_W * RT_H * 4, NULL, NULL));
}
static void done(lagfx_protocol_t *p) { lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); free(p); }

/* every pixel of cell (cx, cy) = row / column of 4 x 4 pixels must read want(cx, cy) */
typedef uint32_t (*want_fn)(uint32_t cx, uint32_t cy);
static void check(const char *what, const uint8_t *img, want_fn w) {
    for (uint32_t y = 0; y < RT_H; y++) for (uint32_t x = 0; x < RT_W; x++) {
        uint32_t got = px(img, x, y), want = w(x / 4u, y / 4u);
        if (got != want) { fprintf(stderr, "%s (%u,%u): got %08x want %08x\n", what, x, y, got, want); assert(0); }
    }
}
static uint32_t g_lo, g_hi;   /* the LOD clamps the model applies: level = clamp(column, lo, hi) (lo..hi clipped to the chain) */
static uint32_t lvl(uint32_t cx) { uint32_t l = cx < g_lo ? g_lo : cx; return l > g_hi ? g_hi : l; }
static uint32_t w_lod(uint32_t cx, uint32_t cy) { (void)cy; return col(0, lvl(cx)); }
static uint32_t w_arr4(uint32_t cx, uint32_t cy) { return col(cy, lvl(cx)); }
static uint32_t w_arr1(uint32_t cx, uint32_t cy) { (void)cy; return col(0, lvl(cx)); }

static void test_mip_chain(void) {
    uint8_t img[RT_W * RT_H * 4];
    __typeof__(((lagfx_protocol_t *)0)->diag2.exec.run) *R;
    for (int k = 0; k < 2; k++) {   /* mip filter linear (2) and nearest (1): solid levels at integer LODs read the same */
        lagfx_protocol_t *p = tv_run(0, k ? 0x85000000u : 0x86000000u, 2, 4, 1);
        g_lo = 0, g_hi = 3; g_clamp_lo = -1.f;
        tv_exec(p, img);
        R = &p->diag2.exec.run;
        assert(R->draws == 1 && R->draws_skipped == 0 && p->diag2.exec.pipe.desc_unbound == 0);
        check("mip chain", img, w_lod);
        assert(R->ext_tex_mip_views == 1 && R->ext_tex_mips_base_only == 0 && R->ext_tex_layers_base_only == 0 && R->ext_tex_array_views == 0);
        done(p);
    }
    /* negative control 1: a sampler without a mip filter (word mip field 0) reads level 0 everywhere although the view has four levels */
    lagfx_protocol_t *p = tv_run(0, 0x84000000u, 2, 4, 1);
    g_lo = g_hi = 0;
    tv_exec(p, img);
    check("no mip filter", img, w_lod);
    assert(p->diag2.exec.run.draws == 1 && p->diag2.exec.run.ext_tex_mip_views == 1);
    done(p);
    /* negative control 2: a texture with ONE level: the LOD is clamped to the view's levels -> level 0 for every column */
    p = tv_run(0, 0x86000000u, 2, 1, 1);
    g_lo = g_hi = 0;
    tv_exec(p, img);
    check("one level", img, w_lod);
    assert(p->diag2.exec.run.ext_tex_mip_views == 0 && p->diag2.exec.run.ext_tex_mips_base_only == 0);
    done(p);
    /* the guest's LOD clamps (tag 113 {index, count}, {ref, lodMin, lodMax}) apply on top of the view's range */
    static const float cl[3][2] = { { 0.f, 1.f }, { 2.f, 3.f }, { 1.f, 2.f } };
    for (int k = 0; k < 3; k++) {
        p = tv_run(0, 0x86000000u, 2, 4, 1);
        g_clamp_lo = cl[k][0], g_clamp_hi = cl[k][1], g_lo = (uint32_t)cl[k][0], g_hi = (uint32_t)cl[k][1];
        tv_exec(p, img);
        check("lod clamps", img, w_lod);
        assert(p->diag2.exec.run.tag_done[113] == 1 && p->diag2.exec.run.ext_samp_lod_binds >= 1);
        done(p);
    }
    g_clamp_lo = -1.f;
    puts("PASS: 2D view over a 4-level chain: explicit LOD (linear / nearest mip filter) reads the colour of that level; no mip filter / one level / LOD clamps behave as the CPU model");
}

static void test_array(void) {
    uint8_t img[RT_W * RT_H * 4];
    /* texture2d_array shader on a 4-layer x 4-level texture: row = layer, column = level, all sixteen colours */
    lagfx_protocol_t *p = tv_run(1, 0x86000000u, 3, 4, 4);
    g_lo = 0, g_hi = 3; g_clamp_lo = -1.f;
    tv_exec(p, img);
    __typeof__(p->diag2.exec.run) *R = &p->diag2.exec.run;
    assert(R->draws == 1 && R->draws_skipped == 0 && p->diag2.exec.pipe.desc_unbound == 0);
    check("array x chain", img, w_arr4);
    assert(R->ext_tex_array_views == 1 && R->ext_tex_mip_views == 1 && R->ext_tex_mips_base_only == 0 && R->ext_tex_layers_base_only == 0);
    done(p);
    /* array shader, LOD clamped to levels 1..2 */
    p = tv_run(1, 0x86000000u, 3, 4, 4);
    g_clamp_lo = 1.f, g_clamp_hi = 2.f, g_lo = 1, g_hi = 2;
    tv_exec(p, img);
    check("array lod clamps", img, w_arr4);
    done(p);
    g_clamp_lo = -1.f;
    /* negative control: the array shader on a plain 2D texture (one layer) sees a one-layer array; layers 1..3 clamp to layer 0 (Vulkan: layer = clamp(round(coord), 0, layers - 1)) */
    p = tv_run(1, 0x86000000u, 2, 4, 1);
    g_lo = 0, g_hi = 3;
    tv_exec(p, img);
    check("array shader on 2D", img, w_arr1);
    assert(p->diag2.exec.run.draws == 1 && p->diag2.exec.run.ext_tex_array_views == 1 && p->diag2.exec.run.ext_tex_layers_base_only == 0);
    done(p);
    /* negative control: a plain texture2d shader on a 4-layer texture reads layer 0 only; the omitted layers are counted */
    p = tv_run(0, 0x86000000u, 3, 4, 4);
    g_lo = 0, g_hi = 3;
    tv_exec(p, img);
    check("2D shader on array", img, w_lod);
    R = &p->diag2.exec.run;
    assert(R->draws == 1 && R->ext_tex_layers_base_only == 1 && R->ext_tex_array_views == 0 && R->ext_tex_mips_base_only == 0 && R->ext_tex_mip_views == 1);
    done(p);
    puts("PASS: 2D_ARRAY view of all layers x all levels: sixteen (layer, level) colours read back exactly; one-layer array, 2D-shader-on-array (layer 0, counted) controls");
}

/* The integration case: unnormalized-coordinates samplers (the exec's explicit-LOD-0 shader variant, spv_unnorm.c) on a multi-mip texture. Vulkan only lets such a sampler read a view of one
 * level and one layer, so the draw gets the level 0 view (counted as dropped mips) and reads the texels of level 0 whatever the texture holds; the same shader with a NORMALIZED sampler
 * (coordinates in texels = far outside [0, 1], so the implicit LOD is huge) reads the last level through the whole-chain view. */
static uint32_t w_zero(uint32_t cx, uint32_t cy) { (void)cx; (void)cy; return col(0, 0); }
static uint32_t w_last(uint32_t cx, uint32_t cy) { (void)cx; (void)cy; return col(0, 3); }
static void test_unnorm_mips(void) {
    uint8_t img[RT_W * RT_H * 4];
    g_clamp_lo = -1.f;
    lagfx_protocol_t *p = tv_run(2, 0x06000000u, 2, 4, 1);   /* SYNTHETIC word: mip linear + unnormalized (bit 31 clear), as arm64-exec-res-unit.c */
    tv_exec(p, img);
    __typeof__(p->diag2.exec.run) *R = &p->diag2.exec.run;
    assert(R->draws == 1 && R->draws_skipped == 0 && R->ext_unnorm_lod0 == 1 && R->ext_unnorm_skipped == 0 && R->ext_samp_unnorm == 1);
    check("unnormalized sampler on a 4-level texture", img, w_zero);
    assert(R->ext_tex_mips_base_only == 1 && R->ext_tex_mip_views == 0);   /* the Vulkan restriction is a counted drop */
    done(p);
    p = tv_run(2, 0x86000000u, 2, 4, 1);   /* normalized, mip linear: implicit LOD of texel-unit coordinates selects the last level */
    tv_exec(p, img);
    R = &p->diag2.exec.run;
    assert(R->draws == 1 && R->ext_unnorm_lod0 == 0 && R->ext_tex_mips_base_only == 0 && R->ext_tex_mip_views == 1);
    check("normalized sampler, implicit LOD, 4-level texture", img, w_last);
    done(p);
    p = tv_run(2, 0x06000000u, 3, 4, 4);   /* unnormalized sampler, plain 2D shader, 4-layer 4-level texture: level 0 / layer 0 view, both counted */
    tv_exec(p, img);
    R = &p->diag2.exec.run;
    assert(R->draws == 1 && R->ext_unnorm_lod0 == 1 && R->ext_tex_mips_base_only == 1 && R->ext_tex_layers_base_only == 1);
    check("unnormalized sampler on a layered 4-level texture", img, w_zero);
    done(p);
    puts("PASS: unnormalized sampler variant + multi-mip texture: level 0 view (Vulkan: one level / one layer), counted; the normalized control reads the last level of the same chain");
}

int main(void) {
    g_mem = calloc(1, MEM_SIZE);
    g_dev = make_device();
    assert(g_dev);
    if (!(g_dev->vk && g_dev->vk->initialized)) { puts("SKIP: no Vulkan device"); lagfx_device_free(g_dev); free(g_mem); return 77; }
    test_mip_chain();
    test_array();
    test_unnorm_mips();
    assert(lagfx_vk_validation_msg_count == 0);   /* checked before the test's own shader modules / layouts are destroyed */
    struct lagfx_vk_state *vk = g_dev->vk;
    for (int i = 0; i < 3; i++) { if (i < 2 && g_t.m[i]) vkDestroyShaderModule(vk->device, g_t.m[i], NULL); if (g_t.fsm[i]) vkDestroyShaderModule(vk->device, g_t.fsm[i], NULL); }
    if (g_t.pl) vkDestroyPipelineLayout(vk->device, g_t.pl, NULL);
    if (g_t.dsl) vkDestroyDescriptorSetLayout(vk->device, g_t.dsl, NULL);
    lagfx_device_free(g_dev);
    free(g_mem);
    return 0;
}
#else
int main(void) { puts("SKIP: built without Vulkan"); return 77; }
#endif
