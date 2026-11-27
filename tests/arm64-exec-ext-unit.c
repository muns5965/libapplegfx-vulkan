/* ARM64 render exec extensions on lavapipe (LAGFX_ARM64_EXEC=1): the draw variants (wide / instanced / base vertex / base instance / indexed / indirect), zero-count
 * draws, residency / fence / barrier tags, LOD-clamp sampler binds. Reuses the scaffolding of arm64-exec-run-unit.c (guest memory, ring, object table, trivial
 * shaders: a big triangle from vertices 0..2; vertices 1..3 make a small one that does not cover pixel (5, 8)).
 * SPDX-License-Identifier: MIT */
#define main run_unit_main
#include "arm64-exec-run-unit.c"
#undef main

#ifdef LAGFX_HAVE_VULKAN
#define REF_IND  6u
#define REF_SAMP 7u
#define VPN_IND  14u
static void p16(uint8_t *b, uint32_t o, uint16_t v) { memcpy(b + o, &v, 2); }
static void p32(uint8_t *b, uint32_t o, uint32_t v) { memcpy(b + o, &v, 4); }
static void p64(uint8_t *b, uint32_t o, uint64_t v) { memcpy(b + o, &v, 8); }

static uint32_t g_args[5];
static uint32_t g_samp_state = 0x86000000u;   /* SYNTHETIC state word (mip linear, normalized; built from the INFER layout of arm64_sampler.h, not captured) */
/* one exec: green clear, pipeline, vertex buffer 0, then cmds(); returns the protocol with the pixels readable through img */
static lagfx_protocol_t *ext_run(void (*cmds)(void), uint8_t *img) {
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    lagfx_task_entry_t *t = def_task(p, 1);
    build_objects();
    va_write((uint64_t)VPN_IND << 14, g_args, sizeof g_args);
    put_buffer_object(REF_IND, 0x240, 0x4000, VPN_IND);
    {   /* a serialized sampler (tag 3, 36 bytes) */
        uint8_t sm[36]; memset(sm, 0, sizeof sm);
        p32(sm, 0, 3), p32(sm, 4, 36), p32(sm, 8, REF_SAMP), p32(sm, 12, g_samp_state);
        float lo = 0.f, hi = 3.4028235e38f; memcpy(sm + 20, &lo, 4), memcpy(sm + 24, &hi, 4);
        va_write(VA_PAY + 0x500, sm, sizeof sm);
        entry(REF_SAMP, LAGFX_A64_OBJ_SERIALIZED, 36, VA_PAY + 0x500);
    }
    lagfx_a64_exec_set_pipeline_resolver(p, resolver, NULL);
    static const double green[4] = { 0, 1, 0, 1 };
    cb_reset(); cb_pass(REF_TEX, 2, green); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, 0);
    cmds();
    exec(p, 1, cb_finish(), true);
    assert(p->diag2.exec.run.submit_failed == 0);
    assert(lagfx_a64_exec_readback_texture(p, t, REF_TEX, img, RT_W * RT_H * 4, NULL, NULL));
    return p;
}
static void done(lagfx_protocol_t *p) { lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); free(p); }

/* tag layout helpers (payload sizes = the written part of each record) */
static void c_instanced(uint16_t start, uint16_t count, uint16_t inst) { uint8_t r[8]; p16(r, 0, start), p16(r, 2, count), p16(r, 4, inst), p16(r, 6, 3); cb_cmd(3, r, 8); }
static void c_base(uint16_t inst, uint16_t base_inst) { uint8_t r[12] = {0}; p16(r, 0, 0), p16(r, 2, 3), p16(r, 4, inst), p16(r, 6, base_inst), p16(r, 8, 3); cb_cmd(5, r, 12); }
static void c_wide(void) { uint8_t r[20]; p32(r, 0, 3), p64(r, 4, 0), p64(r, 12, 3); cb_cmd(0, r, 20); }
static void c_idx_full(uint16_t off, uint16_t count, uint16_t inst, int16_t bv, uint16_t bi) {
    uint8_t r[20] = {0}; p16(r, 0, 3), p16(r, 2, 0), p32(r, 4, REF_IB), p16(r, 8, off), p16(r, 10, count), p16(r, 12, inst), p16(r, 14, (uint16_t)bv), p16(r, 16, bi); cb_cmd(11, r, 20);
}
static void c_idx_inst(uint16_t inst) { uint8_t r[16] = {0}; p16(r, 0, 3), p16(r, 2, 0), p32(r, 4, REF_IB), p16(r, 8, 3), p16(r, 10, 0), p16(r, 12, inst); cb_cmd(9, r, 16); }
static void c_idx_wide(void) { uint8_t r[24]; p16(r, 0, 3), p16(r, 2, 0), p32(r, 4, REF_IB), p64(r, 8, 3), p64(r, 16, 0); cb_cmd(6, r, 24); }
static void c_ind(uint64_t off) { uint8_t r[16] = {0}; p64(r, 0, off), p32(r, 8, REF_IND), p16(r, 12, 3); cb_cmd(16, r, 16); }
static void c_ind_idx(void) { uint8_t r[28]; p16(r, 0, 3), p16(r, 2, 0), p32(r, 4, REF_IB), p32(r, 8, REF_IND), p64(r, 12, 0), p64(r, 20, 0); cb_cmd(17, r, 28); }

static void k_inst2(void) { c_instanced(0, 3, 2); }
static void k_inst0(void) { c_instanced(0, 3, 0); }
static void k_first1(void) { c_instanced(1, 3, 1); }
static void k_wide(void) { c_wide(); }
static void k_base_inst(void) { c_base(1, 5); }
static void k_full0(void) { c_idx_full(0, 3, 1, 0, 0); }
static void k_full_bv1(void) { c_idx_full(0, 3, 1, 1, 0); }
static void k_full_off2(void) { c_idx_full(2, 3, 1, 0, 0); }         /* indices {1, 2, 0}: the same triangle; a swapped offset / count would not draw it */
static void k_full_off2_bv1(void) { c_idx_full(2, 3, 1, 1, 0); }     /* vertices 2, 3, 1: the small one */
static void k_idx_inst2(void) { c_idx_inst(2); }
static void k_idx_wide(void) { c_idx_wide(); }
static void k_ind(void) { c_ind(0); }
static void k_ind_oob(void) { c_ind(0x4000 - 8); }
static void k_ind_idx(void) { c_ind_idx(); }
static void k_noops(void) {
    uint8_t r[16]; memset(r, 0, sizeof r);
    p32(r, 0, 1), p16(r, 4, 1), p16(r, 6, 2), p32(r, 8, REF_VB);   /* useResource: count 1, usage, stages, ref */
    cb_cmd(137, r, 12);
    p32(r, 0, 1), p16(r, 4, 2), p32(r, 6, REF_VB);                 /* useHeap: count, stages u16, refs from +6 */
    cb_cmd(27, r, 10);
    p32(r, 0, 1), p32(r, 4, REF_VB); cb_cmd(134, r, 8);             /* useHeaps */
    p32(r, 0, 1), p32(r, 4, 1), p32(r, 8, REF_VB); cb_cmd(135, r, 12);   /* useResources */
    p32(r, 0, 77), p32(r, 4, 2); cb_cmd(24, r, 8); cb_cmd(25, r, 8);     /* updateFence / waitForFence */
    cb_cmd(22, r, 8); cb_cmd(23, r, 4); cb_cmd(133, r, 0);              /* memory barriers, texture barrier */
    c_instanced(0, 3, 1);
}
static void k_lod(void) {
    uint8_t r[20]; p32(r, 0, 0), p32(r, 4, 1), p32(r, 8, REF_SAMP);
    float lo = 1.f, hi = 4.f; memcpy(r + 12, &lo, 4), memcpy(r + 16, &hi, 4);
    cb_cmd(113, r, 20);                                              /* a variant */
    cb_cmd(113, r, 20);                                              /* same clamps again: the cached variant */
    lo = 2.f; memcpy(r + 12, &lo, 4); cb_cmd(113, r, 20);
    uint8_t q[12]; p32(q, 0, 0), p32(q, 4, 1), p32(q, 8, REF_SAMP); cb_cmd(112, q, 12);   /* the plain bind resets the override */
    cb_cmd(113, r, 8);                                               /* short record */
    c_instanced(0, 3, 1);
}

static bool red_left(const uint8_t *img) { return px(img, 5, 8) == RED; }
static bool green_left(const uint8_t *img) { return px(img, 5, 8) == GREEN; }

static void test_ext_draws(void) {
    uint8_t img[RT_W * RT_H * 4];
    lagfx_protocol_t *p; __typeof__(p->diag2.exec.run) *R;
    memset(g_args, 0, sizeof g_args);

    p = ext_run(k_inst2, img); R = &p->diag2.exec.run;
    assert(red_left(img) && R->draws == 1 && R->tag_done[3] == 1 && R->ext_draw_instanced == 1 && R->draws_skipped == 0); done(p);
    p = ext_run(k_inst0, img); R = &p->diag2.exec.run;
    assert(green_left(img) && R->draws == 0 && R->ext_draw_zero == 1 && R->draws_skipped == 0 && R->tag_done[3] == 1); done(p);   /* zero instances draw nothing, and are not an error */
    p = ext_run(k_first1, img); R = &p->diag2.exec.run;
    assert(green_left(img) && R->draws == 1); done(p);                                                                           /* first vertex 1: the small triangle */
    p = ext_run(k_wide, img); R = &p->diag2.exec.run;
    assert(red_left(img) && R->draws == 1 && R->ext_draw_wide == 1 && R->tag_done[0] == 1); done(p);
    p = ext_run(k_base_inst, img); R = &p->diag2.exec.run;
    assert(red_left(img) && R->draws == 1 && R->ext_draw_base == 1 && R->tag_done[5] == 1); done(p);
    p = ext_run(k_full0, img); R = &p->diag2.exec.run;
    assert(red_left(img) && R->draws == 1 && R->tag_done[11] == 1); done(p);
    p = ext_run(k_full_bv1, img);
    assert(green_left(img) && p->diag2.exec.run.draws == 1 && p->diag2.exec.run.ext_draw_base == 1); done(p);                    /* base vertex 1: vertices 1, 2, 3 */
    p = ext_run(k_full_off2, img);
    assert(red_left(img) && p->diag2.exec.run.draws == 1); done(p);                                                              /* offset BEFORE count in this record */
    p = ext_run(k_full_off2_bv1, img);
    assert(green_left(img) && p->diag2.exec.run.draws == 1); done(p);
    p = ext_run(k_idx_inst2, img);
    assert(red_left(img) && p->diag2.exec.run.draws == 1 && p->diag2.exec.run.ext_draw_instanced == 1 && p->diag2.exec.run.tag_done[9] == 1); done(p);
    p = ext_run(k_idx_wide, img);
    assert(red_left(img) && p->diag2.exec.run.draws == 1 && p->diag2.exec.run.tag_done[6] == 1); done(p);

    /* indirect: the argument block {vertexCount, instanceCount, vertexStart, baseInstance} of a mirror buffer */
    g_args[0] = 3, g_args[1] = 1, g_args[2] = 0, g_args[3] = 0;
    p = ext_run(k_ind, img); R = &p->diag2.exec.run;
    assert(red_left(img) && R->draws == 1 && R->ext_draw_indirect == 1 && R->tag_done[16] == 1); done(p);
    g_args[2] = 1;
    p = ext_run(k_ind, img);
    assert(green_left(img) && p->diag2.exec.run.draws == 1); done(p);
    g_args[2] = 0, g_args[1] = 0;
    p = ext_run(k_ind, img);
    assert(green_left(img) && p->diag2.exec.run.draws == 0 && p->diag2.exec.run.ext_draw_zero == 1); done(p);
    g_args[1] = 1;
    p = ext_run(k_ind_oob, img); R = &p->diag2.exec.run;
    assert(green_left(img) && R->draws == 0 && R->ext_draw_indirect_bad == 1 && R->draws_skipped == 1 && R->tag_partial[16] == 1); done(p);   /* block runs past the buffer: refused */
    /* indexed indirect {indexCount, instanceCount, indexStart, baseVertex, baseInstance} */
    g_args[0] = 3, g_args[1] = 1, g_args[2] = 0, g_args[3] = 0, g_args[4] = 0;
    p = ext_run(k_ind_idx, img);
    assert(red_left(img) && p->diag2.exec.run.draws == 1 && p->diag2.exec.run.tag_done[17] == 1); done(p);
    g_args[3] = 1;
    p = ext_run(k_ind_idx, img);
    assert(green_left(img) && p->diag2.exec.run.draws == 1); done(p);
    puts("PASS: draw variants on lavapipe (wide, instanced, zero count, first vertex, base vertex / instance, indexed offset order, indirect)");
}

static void test_ext_tags(void) {
    uint8_t img[RT_W * RT_H * 4];
    memset(g_args, 0, sizeof g_args);
    lagfx_protocol_t *p = ext_run(k_noops, img);
    __typeof__(p->diag2.exec.run) *R = &p->diag2.exec.run;
    assert(red_left(img) && R->draws == 1 && R->tag_unsup[137] == 0 && R->tag_done[137] == 1 && R->tag_done[27] == 1 && R->tag_done[134] == 1 && R->tag_done[135] == 1);
    assert(R->tag_done[24] == 1 && R->tag_done[25] == 1 && R->ext_noop_hint == 4 && R->ext_noop_fence == 2);
    assert(R->tag_partial[22] == 1 && R->tag_partial[23] == 1 && R->tag_partial[133] == 1 && R->ext_barrier == 3);    /* barriers are recorded as seen, not applied */
    done(p);

    g_samp_state = 0x860000a0u;   /* SYNTHETIC: mip linear, compare greater (ignored, counted) */
    p = ext_run(k_lod, img); R = &p->diag2.exec.run;
    assert(red_left(img) && R->tag_done[113] == 3 && R->ext_samp_lod_binds == 3 && R->tag_done[112] == 1 && R->bad_rec == 1 && R->ext_samp_compare == 1 && p->diag2.exec.pipe.samplers == 1);
    done(p);
    g_samp_state = 0x86000000u;
    puts("PASS: residency / fence no-ops, barriers counted, LOD-clamp sampler binds (variants cached), unmapped sampler state counted");
}

int main(void) {
    g_mem = calloc(1, MEM_SIZE);
    g_dev = make_device();
    assert(g_dev);
    if (!(g_dev->vk && g_dev->vk->initialized)) { puts("SKIP: no Vulkan device"); lagfx_device_free(g_dev); free(g_mem); return 77; }
    test_ext_draws();
    test_ext_tags();
    if (g_vs) vkDestroyShaderModule(g_dev->vk->device, g_vs, NULL);
    if (g_fs) vkDestroyShaderModule(g_dev->vk->device, g_fs, NULL);
    lagfx_device_free(g_dev);
    free(g_mem);
    return 0;
}
#else
int main(void) { puts("SKIP: built without Vulkan"); return 77; }
#endif
