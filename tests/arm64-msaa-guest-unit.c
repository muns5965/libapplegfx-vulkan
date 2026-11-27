/* ARM64 exec: guest-visible multisample path (descriptor sample count -> image + pipeline samples, store actions, resolve texture, counted refusals), lavapipe + validation layer.
 * tests/arm64-msaa-guest-unit.c
 *
 * Reuses the harness of arm64-exec-run-unit.c / arm64-render-compat-unit.c (guest memory, objects, command buffers, readback, resolver). EVERY guest field the scenarios build
 * (texture type 4 + sample count at descriptor +18, store action at pass colour +0x16, resolve texture ref at +0x04, deferred tags 102 / 150) is the field this library READS; no
 * guest capture of a multisampled pass exists, so these tests prove the library does what its decoder says, NOT that the real guest writes those bytes (see docs section 5).
 * Exact references: CPU coverage of the standard Vulkan sample locations (lavapipe: 4x). Tolerances: resolved BGRA8 value within 2 UNORM steps of 127.5 * covered / 4 (the fragment
 * writes 0.5, rounded per sample, averaged, rounded again); RGBA16F within 2e-3 of 0.5 * covered / 4. Negative controls: a resolve target left at a 0x55 sentinel must stay untouched
 * when nothing is resolved; the reference of the other y orientation must NOT match.
 * Policy: the default mode is EXACT (guest count or refusal); the clamp / 1x-fallback scenarios run with LAGFX_MSAA_COMPAT=1 and have an exact-mode twin that must refuse. Only 1x and 4x
 * exist on lavapipe: 2x is unverified here (exact 2x only through the injected planner checks of vulkan-msaa-coverage-unit; on this device a 2x request is refused in exact mode and
 * falls back to 1x in compat mode). Validation: val_begin / val_end with labels that are NOT in the baseline file = no message allowed.
 * SPDX-License-Identifier: MIT */
#define LAGFX_COMPAT_NO_MAIN 1
#include "arm64-render-compat-unit.c"

#ifdef LAGFX_HAVE_VULKAN
#include "protocol/arm64_msaa.h"
#include "vulkan/msaa_stats.h"
#include <math.h>
static float half_to_float(uint16_t h) { const uint32_t sg = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u; float v = e == 0 ? ldexpf((float)m, -24) : ldexpf((float)(m | 1024u), (int)e - 25); return sg ? -v : v; }

#define REF_MS8  30u   /* BGRA8 multisample texture */
#define REF_MS16 31u   /* RGBA16F multisample texture */
#define REF_MSX  32u   /* shape / count experiments */
#define VPN_MS8  17u
#define VPN_MS16 18u
#define VPN_MSX  19u

static void put_ms_tex(uint32_t ref, uint32_t vpn, uint32_t mtl_fmt, uint32_t bpp, uint32_t type, uint32_t samples) {
    put_tex(ref, vpn, mtl_fmt, bpp, samples);
    /* the descriptor's type byte (+0 of the v1 description) of the payload put_tex wrote: patch the guest bytes */
    const uint32_t pay = 0x600u + (ref % 8u) * 0x100u, rec = 12 + 0x18 + 36;
    const uint8_t ty = (uint8_t)type;
    va_write(VA_PAY + pay + rec + 12, &ty, 1);
}
/* a pass: slot 0 (and optionally slot 1): texture ref, load clear 0, store action, resolve texture */
static void cb_pass_ms2(uint32_t ref0, uint16_t store0, uint32_t res0, uint32_t ref1, uint16_t store1, uint32_t res1) {
    uint8_t r[584]; memset(r, 0, sizeof r);
    const uint32_t refs[2] = { ref0, ref1 }, ress[2] = { res0, res1 }; const uint16_t sts[2] = { store0, store1 };
    for (int k = 0; k < 2; k++) {
        if (!refs[k]) continue;
        uint8_t *c = r + 0x4c + 0x3c * k; const uint16_t load = 2;
        memcpy(c, &refs[k], 4); memcpy(c + 4, &ress[k], 4); memcpy(c + 20, &load, 2); memcpy(c + 22, &sts[k], 2);   /* clear colour stays 0,0,0,0 */
    }
    cb_cmd(26, r, sizeof r);
}

/* ---- sample-count restriction of the (otherwise real) device, for the 1x-fallback scenario ---- */
typedef struct { lagfx_fmtcap_backend_t real; VkSampleCountFlags mask; } scm_t;
static scm_t g_scm; static lagfx_fmtcap_backend_t g_scm_be;
static void scm_fp(void *c, VkFormat f, VkFormatProperties2 *o) { scm_t *s = c; s->real.format_props2(s->real.ctx, f, o); }
static VkResult scm_ifp(void *c, const VkPhysicalDeviceImageFormatInfo2 *in, VkImageFormatProperties2 *o) {
    scm_t *s = c; VkResult r = s->real.image_format_props2(s->real.ctx, in, o);
    if (r == VK_SUCCESS) o->imageFormatProperties.sampleCounts &= s->mask;
    return r;
}
static void scm_set(VkSampleCountFlags mask) {
    static lagfx_fmtcap_backend_t st; const lagfx_fmtcap_backend_t *real = lagfx_fmtcap_device_backend(g_dev->vk->phys_device, &st);
    g_scm.real = *real; g_scm.mask = mask;
    g_scm_be = (lagfx_fmtcap_backend_t){ &g_scm, scm_fp, scm_ifp };
    lagfx_fmtcap_test_backend = &g_scm_be;
}

typedef struct {
    uint8_t bgra[RT_W * RT_H * 4], half[RT_W * RT_H * 8];
    lagfx_msaa_stats_t ms;
    uint64_t draws, skipped, no_target, msaa_refused, val;
    char line[1100];
} gr_t;

enum { P_PLAIN, P_DEFERRED, P_DEFERRED_NO_TAGS };
typedef struct { uint32_t ms_ref, ms_fmt, ms_bpp, ms_type, ms_samples; uint16_t store; uint32_t res_ref; int mode; int fs; uint32_t slot1_ref; bool sentinel; bool ms_is_target_1x; } sc_t;

static void sentinel_fill(void) { static uint8_t f[RT_W * RT_H * 8]; memset(f, 0x55, sizeof f); va_write((uint64_t)VPN_TEX << 14, f, RT_W * RT_H * 4u); va_write((uint64_t)VPN_TEX2 << 14, f, RT_W * RT_H * 8u); }

static void gr_run(const sc_t *s, gr_t *out) {
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    lagfx_task_entry_t *t = def_task(p, 1);
    build_objects();
    if (s->sentinel) sentinel_fill();
    if (s->ms_ref != REF_TEX && s->ms_ref != REF_TEX2) put_ms_tex(s->ms_ref, s->ms_ref == REF_MS16 ? VPN_MS16 : s->ms_ref == REF_MSX ? VPN_MSX : VPN_MS8, s->ms_fmt, s->ms_bpp, s->ms_type, s->ms_samples);
    g_res_tex2 = true;
    g_rc.fs = s->fs; g_rc.blend = false;
    lagfx_a64_exec_set_pipeline_resolver(p, rc_resolver, NULL);
    uint64_t v0 = lagfx_vk_validation_msg_count;
    cb_reset();
    cb_pass_ms2(s->ms_ref, s->store, s->res_ref, s->slot1_ref, 1, 0);
    if (s->mode == P_DEFERRED) {   /* tags 102 SetColorStoreAction {u32 action, u32 index} and 150 SetColorResolveTexture {u32 ref, u32 slot}: x86-era layout, UNVERIFIED on ARM64 */
        uint32_t a[2] = { 2, 0 }, b[2] = { s->res_ref, 0 }; cb_cmd(102, a, 8); cb_cmd(150, b, 8);
    }
    cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, 0);
    v_tris(1); cb_draw_p(3, 0, 3, 1);
    exec(p, 1, cb_finish(), true);
    memset(out, 0, sizeof *out);
    uint32_t w, h;
    if (!lagfx_a64_exec_readback_texture(p, t, REF_TEX, out->bgra, sizeof out->bgra, &w, &h)) memset(out->bgra, 0xee, sizeof out->bgra);
    if (!lagfx_a64_exec_readback_texture(p, t, REF_TEX2, out->half, sizeof out->half, &w, &h)) memset(out->half, 0xee, sizeof out->half);
    out->ms = p->diag2.exec.msaa;
    out->draws = p->diag2.exec.run.draws, out->skipped = p->diag2.exec.run.draws_skipped, out->no_target = p->diag2.exec.run.no_target, out->msaa_refused = p->diag2.exec.fmt.msaa_refused;
    out->val = lagfx_vk_validation_msg_count - v0;
    lagfx_msaa_stats_line(&p->diag2.exec.msaa, out->line, sizeof out->line);
    lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); free(p);
    g_res_tex2 = false;
}

/* compat texture creation, then the mode is switched to exact before the pass that uses it: the mirror object (made in compat) is looked up by the pass of a SECOND exec */
static void gr_run_switch(const sc_t *s, gr_t *out) {
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    lagfx_task_entry_t *t = def_task(p, 1);
    build_objects(); sentinel_fill();
    put_ms_tex(s->ms_ref, VPN_MS8, s->ms_fmt, s->ms_bpp, s->ms_type, s->ms_samples);
    g_res_tex2 = true; g_rc.fs = s->fs; g_rc.blend = false;
    lagfx_a64_exec_set_pipeline_resolver(p, rc_resolver, NULL);
    cb_reset(); cb_pass_ms2(s->ms_ref, 1, 0, 0, 1, 0); exec(p, 1, cb_finish(), true);   /* exec 1 (compat): creates the 8x -> 4x image, stores it */
    setenv("LAGFX_MSAA_COMPAT", "0", 1);   /* exact from here on */
    uint64_t v0 = lagfx_vk_validation_msg_count; const uint64_t d0 = p->diag2.exec.run.draws;
    cb_reset(); cb_pass_ms2(s->ms_ref, s->store, s->res_ref, 0, 1, 0); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, 0); v_tris(1); cb_draw_p(3, 0, 3, 1);
    exec(p, 1, cb_finish(), true);
    memset(out, 0, sizeof *out); uint32_t w, h;
    if (!lagfx_a64_exec_readback_texture(p, t, REF_TEX, out->bgra, sizeof out->bgra, &w, &h)) memset(out->bgra, 0xee, sizeof out->bgra);
    out->ms = p->diag2.exec.msaa, out->draws = p->diag2.exec.run.draws - d0, out->val = lagfx_vk_validation_msg_count - v0;
    lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); free(p);
    g_res_tex2 = false; setenv("LAGFX_MSAA_COMPAT", "1", 1);
}

/* ---- CPU coverage reference (Vulkan standard sample locations, 4x) of the harness's triangle TRI ---- */
static double efn(double ax, double ay, double bx, double by, double px, double py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); }
static const double L4[4][2] = { { 0.375, 0.125 }, { 0.875, 0.375 }, { 0.125, 0.625 }, { 0.625, 0.875 } };
static int cov4(bool flip, uint32_t x, uint32_t y, int *ambiguous) {
    double X[3], Y[3];
    for (int i = 0; i < 3; i++) X[i] = (TRI[2 * i] + 1.0) * 0.5 * RT_W, Y[i] = flip ? (1.0 - TRI[2 * i + 1]) * 0.5 * RT_H : (TRI[2 * i + 1] + 1.0) * 0.5 * RT_H;
    int c = 0;
    for (int s = 0; s < 4; s++) {
        const double sx = x + L4[s][0], sy = y + L4[s][1];
        const double e0 = efn(X[0], Y[0], X[1], Y[1], sx, sy), e1 = efn(X[1], Y[1], X[2], Y[2], sx, sy), e2 = efn(X[2], Y[2], X[0], Y[0], sx, sy);
        if (fabs(e0) < 1e-6 || fabs(e1) < 1e-6 || fabs(e2) < 1e-6) *ambiguous = 1;
        c += (e0 > 0 && e1 > 0 && e2 > 0) || (e0 < 0 && e1 < 0 && e2 < 0);
    }
    return c;
}
/* returns 1 when the BGRA8 (f16 = false) or RGBA16F image equals the reference with the given flip; *partial = pixels with 0 < k < 4 */
static bool resolved_matches(const gr_t *g, bool f16, bool flip, unsigned *partial) {
    unsigned part = 0;
    for (uint32_t y = 0; y < RT_H; y++) for (uint32_t x = 0; x < RT_W; x++) {
        int amb = 0; const int k = cov4(flip, x, y, &amb);
        assert(!amb);
        if (k > 0 && k < 4) part++;
        if (f16) {
            const uint16_t *hp = (const uint16_t *)(g->half + (y * RT_W + x) * 8);
            const double want = 0.5 * k / 4.0, r = half_to_float(hp[0]), a = half_to_float(hp[3]);
            if (fabs(r - want) > 2e-3 || fabs(a - want) > 2e-3 || half_to_float(hp[1]) != 0.0f || half_to_float(hp[2]) != 0.0f) { if (partial) *partial = part; return false; }
        } else {
            const uint32_t v = px(g->bgra, x, y); const double want = 127.5 * k / 4.0;
            if (fabs((double)((v >> 16) & 255) - want) > 2.0 || fabs((double)(v >> 24) - want) > 2.0 || (v & 255) != 0 || ((v >> 8) & 255) != 0) { if (partial) *partial = part; return false; }
        }
    }
    if (partial) *partial = part;
    return true;
}
static bool is_sentinel(const uint8_t *img, size_t n) { for (size_t i = 0; i < n; i++) if (img[i] != 0x55) return false; return true; }

static sc_t sc(uint32_t ref, uint32_t fmt, uint32_t bpp, uint32_t type, uint32_t samples, uint16_t store, uint32_t res) {
    sc_t s; memset(&s, 0, sizeof s); s.ms_ref = ref, s.ms_fmt = fmt, s.ms_bpp = bpp, s.ms_type = type, s.ms_samples = samples, s.store = store, s.res_ref = res, s.fs = FS_HALF;
    return s;
}

/* ---- multi-pass scenarios: one exec, several passes; readback of REF_TEX (BGRA8) and REF_MSX (BGRA8 plain texture used as a second resolve target) ---- */
#define REF_MSD 33u   /* multisampled depth texture */
static void cb_pass_gen(uint32_t ref, uint16_t load, uint16_t store, uint32_t res) {   /* one colour attachment: load action, store action, resolve texture (clear colour 0,0,0,0) */
    uint8_t r[584]; memset(r, 0, sizeof r);
    uint8_t *c = r + 0x4c; memcpy(c, &ref, 4); memcpy(c + 4, &res, 4); memcpy(c + 20, &load, 2); memcpy(c + 22, &store, 2);
    cb_cmd(26, r, sizeof r);
}
static void cb_pass_ms_depth(uint32_t cref, uint32_t dref, uint16_t dstore) {   /* colour 4x (clear, store) + depth attachment with depth store action `dstore` (depth load clear 1.0) */
    uint8_t r[584]; memset(r, 0, sizeof r);
    uint16_t load = 2, store = 1, dload = 2; const double one = 1.0;
    uint8_t *c = r + 0x4c; memcpy(c, &cref, 4); memcpy(c + 20, &load, 2); memcpy(c + 22, &store, 2);
    memcpy(r, &dref, 4); memcpy(r + 0x14, &dload, 2); memcpy(r + 0x16, &dstore, 2); memcpy(r + 0x1c, &one, 8);
    cb_cmd(26, r, sizeof r);
}
typedef struct { uint8_t tex[RT_W * RT_H * 4], msx[RT_W * RT_H * 4]; lagfx_msaa_stats_t ms; uint64_t draws, skipped, no_target, pipes_built, ds_unsup; char line[1100]; } mp_t;
static void mp_run(void (*build)(void), mp_t *o, bool with_depth) {
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    lagfx_task_entry_t *t = def_task(p, 1);
    build_objects(); sentinel_fill();
    put_ms_tex(REF_MS8, VPN_MS8, 80, 4, 4, 4);
    put_ms_tex(REF_MSX, VPN_MSX, 80, 4, 2, 1);
    if (with_depth) {   /* Depth32Float, type 4, 4 samples (descriptor +0 type, +18 samples) */
        put_ds_tex(REF_MSD, 0xe00, 252);
        const uint32_t rec = 12 + 0x18 + 36; const uint8_t ty = 4, ns = 4;
        va_write(VA_PAY + 0xe00 + rec + 12, &ty, 1); va_write(VA_PAY + 0xe00 + rec + 12 + 18, &ns, 1);
    }
    g_res_tex2 = true; g_rc.fs = FS_HALF; g_rc.blend = false;
    lagfx_a64_exec_set_pipeline_resolver(p, rc_resolver, NULL);
    val_begin();
    cb_reset(); build(); exec(p, 1, cb_finish(), true);
    val_end("msaa guest multipass");
    memset(o, 0, sizeof *o); uint32_t w, h;
    if (!lagfx_a64_exec_readback_texture(p, t, REF_TEX, o->tex, sizeof o->tex, &w, &h)) memset(o->tex, 0xee, sizeof o->tex);
    if (!lagfx_a64_exec_readback_texture(p, t, REF_MSX, o->msx, sizeof o->msx, &w, &h)) memset(o->msx, 0xee, sizeof o->msx);
    o->ms = p->diag2.exec.msaa, o->draws = p->diag2.exec.run.draws, o->skipped = p->diag2.exec.run.draws_skipped, o->no_target = p->diag2.exec.run.no_target, o->pipes_built = p->diag2.exec.pipe.pipes_built, o->ds_unsup = p->diag2.exec.run.ds_attach_unsupported;
    lagfx_msaa_stats_line(&p->diag2.exec.msaa, o->line, sizeof o->line);
    lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); free(p);
    g_res_tex2 = false;
}
static void draw_tri(void) { cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, 0); v_tris(1); cb_draw_p(3, 0, 3, 1); }
/* the two store-action scenarios: pass A draws the triangle into the 4x texture and stores with `act` (resolve -> REF_TEX); pass B LOADs the 4x texture back, draws nothing and resolves (action 2) -> REF_MSX */
static uint16_t g_act;
static void b_store_keep(void) { cb_pass_gen(REF_MS8, 2, g_act, REF_TEX); draw_tri(); cb_pass_gen(REF_MS8, 1, 2, REF_MSX); }
static void b_store_clear_ctl(void) { cb_pass_gen(REF_MS8, 2, g_act, REF_TEX); draw_tri(); cb_pass_gen(REF_MS8, 2, 2, REF_MSX); }   /* negative control: pass B clears instead of loading */
/* pipeline cache: the same pipeline object and colour format at 1x (REF_TEX), 4x (REF_MS8 -> REF_MSX), 1x again */
static void b_pipe_key(void) {
    cb_pass_gen(REF_TEX, 2, 1, 0); draw_tri();
    cb_pass_gen(REF_MS8, 2, 3, REF_MSX); draw_tri();
    cb_pass_gen(REF_TEX, 2, 1, 0); draw_tri();
}
static void b_pipe_key_1x_only(void) { cb_pass_gen(REF_TEX, 2, 1, 0); draw_tri(); cb_pass_gen(REF_TEX, 2, 1, 0); draw_tri(); }
static void b_ds_resolve(void) { cb_pass_ms_depth(REF_MS8, REF_MSD, 3); draw_tri(); }
static bool msx_matches(const mp_t *m, bool flip) { gr_t g; memset(&g, 0, sizeof g); memcpy(g.bgra, m->msx, sizeof g.bgra); return resolved_matches(&g, false, flip, NULL); }
static bool tex_matches(const mp_t *m, bool flip) { gr_t g; memset(&g, 0, sizeof g); memcpy(g.bgra, m->tex, sizeof g.bgra); return resolved_matches(&g, false, flip, NULL); }

static void test_resolve(bool f16, uint16_t store, const char *name) {
    static gr_t g;
    sc_t s = f16 ? sc(REF_MS16, 115, 8, 4, 4, store, REF_TEX2) : sc(REF_MS8, 80, 4, 4, 4, store, REF_TEX);
    s.sentinel = true;
    gr_run(&s, &g);
    unsigned part = 0;
    const bool a = resolved_matches(&g, f16, false, &part), b = resolved_matches(&g, f16, true, NULL);
    if (!a && !b) { for (uint32_t y = 0; y < RT_H; y++) { for (uint32_t x = 0; x < RT_W; x++) printf("%02x ", f16 ? (unsigned)(((const uint16_t *)(g.half + (y * RT_W + x) * 8))[0] >> 6) & 255u : (px(g.bgra, x, y) >> 16) & 255u); printf("\n"); } printf("draws=%llu ms_att=%llu res=%llu\n", (unsigned long long)g.draws, (unsigned long long)g.ms.ms_attach, (unsigned long long)g.ms.resolves); }
    for (int fl = 0; fl < 2; fl++) { /* count partial pixels of the matching orientation */ if ((fl ? b : a)) { part = 0; for (uint32_t y = 0; y < RT_H; y++) for (uint32_t x = 0; x < RT_W; x++) { int am = 0; int k = cov4(fl, x, y, &am); part += k > 0 && k < 4; } } }
    assert(part >= 6);                                                     /* the triangle covers pixels partly: the average is not 0 / 0.5 only */
    assert(a != b);                                                         /* exactly one y orientation matches: the reference can fail */
    assert(g.ms.ms_images_created == 1 && g.ms.ms_compat_clamped == 0 && g.ms.ms_compat_1x == 0 && g.ms.ms_exact_refused == 0 && g.ms.ms_attach == 1 && g.ms.resolves == 1 && g.ms.resolves_by_copy == 0);
    assert(g.ms.ms_passes == 1 && g.ms.pipes_ms == 1 && g.ms.draws_ms == 1 && g.draws == 1 && g.no_target == 0 && g.msaa_refused == 0);
    assert(g.ms.resolve_no_target == 0 && g.ms.store_unknown == 0 && g.ms.sampled_ms_refused == 0 && g.val == 0);
    printf("PASS %s 4x %s: resolved image == CPU coverage reference (orientation %s, %u partial pixels), images=1 attach=1 resolves=1 pipes_ms=1 draws_ms=1, validation 0\n", f16 ? "RGBA16F" : "BGRA8", name, a ? "y-down" : "y-flipped", part);
}

int main(void) {
    g_mem = calloc(1, MEM_SIZE);
    g_dev = make_device();
    assert(g_dev);
    if (!g_dev->vk || !g_dev->vk->initialized) { puts("SKIP: no Vulkan device"); lagfx_device_free(g_dev); free(g_mem); return 77; }
    VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(g_dev->vk->phys_device, &pr);
    if (!(pr.limits.framebufferColorSampleCounts & VK_SAMPLE_COUNT_4_BIT) || !pr.limits.standardSampleLocations) { puts("SKIP: needs 4x and standard sample locations"); return 77; }
    static gr_t g;

    /* 1/2: resolve by store action (2 = MultisampleResolve, 3 = StoreAndMultisampleResolve), both formats */
    test_resolve(false, 2, "store action 2");
    test_resolve(false, 3, "store action 3");
    test_resolve(true, 2, "store action 2");
    test_resolve(true, 3, "store action 3");

    /* 3: Store (1) with a resolve texture named: nothing is resolved, the sentinel survives (negative control: the result is not "always written") */
    { sc_t s = sc(REF_MS8, 80, 4, 4, 4, 1, REF_TEX); s.sentinel = true; gr_run(&s, &g);
      assert(is_sentinel(g.bgra, sizeof g.bgra) && g.ms.resolves == 0 && g.ms.ms_attach == 1 && g.draws == 1 && g.val == 0);
      puts("PASS store action 1 with a resolve texture: not resolved, the resolve target is untouched"); }

    /* 4: resolve action without a resolve texture: stored, counted */
    { sc_t s = sc(REF_MS8, 80, 4, 4, 4, 2, 0); s.sentinel = true; gr_run(&s, &g);
      assert(is_sentinel(g.bgra, sizeof g.bgra) && g.ms.resolve_no_target == 1 && g.ms.resolves == 0 && g.draws == 1 && g.val == 0);
      puts("PASS resolve action without a resolve texture: stored, resolve_no_target counted, target untouched"); }

    /* 5: resolve texture of the wrong shape (RGBA16F target for a BGRA8 attachment): refused, counted */
    { sc_t s = sc(REF_MS8, 80, 4, 4, 4, 2, REF_TEX2); s.sentinel = true; gr_run(&s, &g);
      assert(is_sentinel(g.half, sizeof g.half) && g.ms.resolve_no_target == 1 && g.ms.resolves == 0 && g.val == 0);
      puts("PASS resolve texture of another format: refused, resolve_no_target counted"); }

    /* 6: deferred store action / resolve texture (tags 102 / 150 after the pass; descriptor action 4 = Unknown) */
    { sc_t s = sc(REF_MS8, 80, 4, 4, 4, 4, REF_TEX); s.sentinel = true; s.mode = P_DEFERRED; gr_run(&s, &g);
      const bool m0 = resolved_matches(&g, false, false, NULL), m1 = resolved_matches(&g, false, true, NULL); assert(m0 != m1); printf("  deferred: tags=%llu resolves=%llu store_unknown=%llu\n", (unsigned long long)g.ms.deferred_store_tags, (unsigned long long)g.ms.resolves, (unsigned long long)g.ms.store_unknown);
      assert(g.ms.deferred_store_tags == 2 && g.ms.resolves == 1 && g.ms.store_unknown == 0 && g.val == 0);
      sc_t n = sc(REF_MS8, 80, 4, 4, 4, 4, REF_TEX); n.sentinel = true; n.mode = P_DEFERRED_NO_TAGS; gr_run(&n, &g);   /* negative control: no tags -> Unknown stays unresolved */
      assert(is_sentinel(g.bgra, sizeof g.bgra) && g.ms.store_unknown == 1 && g.ms.resolves == 0 && g.ms.deferred_store_tags == 0);
      puts("PASS deferred store action / resolve texture tags (layout UNVERIFIED on ARM64) resolve; without them action 4 is counted store_unknown and nothing is resolved"); }

    /* 7: resolve action on a single-sample attachment (invalid Metal): stored, counted, the pass still renders */
    { sc_t s = sc(REF_TEX, 80, 4, 2, 1, 2, REF_TEX2); s.sentinel = false; gr_run(&s, &g);
      assert(g.ms.resolve_on_1x == 1 && g.ms.ms_attach == 0 && g.ms.ms_passes == 0 && g.draws == 1 && g.val == 0);
      puts("PASS resolve action on a 1x texture: treated as Store, resolve_on_1x counted, no multisample state"); }

    /* 8e: EXACT (default): 8 samples asked on a device with 1|4: refused, no 4x approximation; the pass finds no target, nothing is drawn or resolved */
    { sc_t s = sc(REF_MS8, 80, 4, 4, 8, 2, REF_TEX); s.sentinel = true; gr_run(&s, &g);
      assert(g.ms.ms_exact_refused == 1 && g.ms.ms_tex_refused == 1 && g.ms.ms_images_created == 0 && g.ms.ms_compat_clamped == 0 && g.ms.ms_compat_1x == 0 && g.ms.ms_pass_refused == 1);
      assert(g.draws == 0 && g.no_target >= 1 && g.ms.resolves == 0 && g.ms.draws_ms == 0 && g.ms.pipes_ms == 0 && is_sentinel(g.bgra, sizeof g.bgra) && g.val == 0 && strstr(g.line, "mode=exact"));
      puts("PASS exact: 8 samples asked, device offers 1|4: texture refused (ms_exact_refused), pass attachment skipped (ms_pass_refused), no draw, resolve target untouched"); }
    /* 9e: EXACT: 2 samples on 1|4 (lavapipe has no 2x): refused */
    { sc_t s = sc(REF_MS8, 80, 4, 4, 2, 2, REF_TEX); s.sentinel = true; gr_run(&s, &g);
      assert(g.ms.ms_exact_refused == 1 && g.ms.ms_images_created == 0 && g.ms.ms_compat_1x == 0 && g.ms.resolves_by_copy == 0 && g.draws == 0 && g.ms.ms_pass_refused == 1 && is_sentinel(g.bgra, sizeof g.bgra) && g.val == 0);
      puts("PASS exact: 2 samples asked, device has no 2x: refused (no 1x fallback, no resolve-by-copy), no draw"); }
    /* 9f: EXACT: 3 samples (not a Metal / Vulkan count): refused */
    { sc_t s = sc(REF_MS8, 80, 4, 4, 3, 2, REF_TEX); s.sentinel = true; gr_run(&s, &g);
      assert(g.ms.ms_exact_refused == 1 && g.ms.ms_images_created == 0 && g.draws == 0 && g.val == 0);
      puts("PASS exact: 3 samples asked: refused (compat would take 2)"); }
    /* 10e: EXACT, injected device with 1 sample only: 4x refused */
    scm_set(VK_SAMPLE_COUNT_1_BIT);
    { sc_t s = sc(REF_MS8, 80, 4, 4, 4, 2, REF_TEX); s.sentinel = true; gr_run(&s, &g);
      assert(g.ms.ms_exact_refused == 1 && g.ms.ms_images_created == 0 && g.ms.ms_compat_1x == 0 && g.draws == 0 && g.val == 0 && is_sentinel(g.bgra, sizeof g.bgra));
      puts("PASS exact: injected 1-sample-only device, 4x asked: refused"); }
    lagfx_fmtcap_test_backend = NULL;
    /* exact 4x still works on this device (control for the refusals above): test_resolve ran first */

    /* COMPAT (LAGFX_MSAA_COMPAT=1, default OFF): the downgrades are permitted and counted separately; they are NOT exact MSAA */
    setenv("LAGFX_MSAA_COMPAT", "1", 1);
    /* 8c: 8 samples asked on a device with 1|4: clamped to 4, renders and resolves like 4x */
    { sc_t s = sc(REF_MS8, 80, 4, 4, 8, 2, REF_TEX); gr_run(&s, &g);
      assert(g.ms.ms_images_created == 1 && g.ms.ms_compat_clamped == 1 && g.ms.ms_compat_1x == 0 && g.ms.ms_exact_refused == 0 && g.ms.resolves == 1 && g.draws == 1 && g.val == 0 && strstr(g.line, "mode=compat") && strstr(g.line, "compat_clamped=1"));
      unsigned part; assert(resolved_matches(&g, false, false, &part) != resolved_matches(&g, false, true, NULL));
      puts("PASS compat: 8 samples asked, device offers 1|4: clamped to 4 (ms_compat_clamped), result equals the 4x reference"); }

    /* 9c: 2 samples asked on a device with 1|4 (no 2): explicit 1x fallback, the "resolve" is a copy; only 0 / 1x-coverage values */
    { sc_t s = sc(REF_MS8, 80, 4, 4, 2, 2, REF_TEX); s.sentinel = true; gr_run(&s, &g);
      assert(g.ms.ms_images_created == 0 && g.ms.ms_compat_1x == 1 && g.ms.ms_exact_refused == 0 && g.ms.resolves == 1 && g.ms.resolves_by_copy == 1 && g.draws == 1 && g.val == 0 && strstr(g.line, "compat_1x=1"));
      unsigned full = 0, partial = 0;
      for (uint32_t i = 0; i < RT_W * RT_H; i++) { uint32_t v; memcpy(&v, g.bgra + i * 4, 4); const uint32_t r = (v >> 16) & 255; if (r == 0) continue; if (r >= 127 && r <= 128) full++; else partial++; }
      assert(full > 40 && partial == 0);
      puts("PASS compat: 2 samples asked, device has no 2x: counted fallback to 1x (ms_compat_1x), resolved by a copy, only 0 / full-coverage values (no partial pixel)"); }

    /* 10c: injected: the device reports only 1 sample for any format: 4x asked -> 1x fallback, copy resolve */
    scm_set(VK_SAMPLE_COUNT_1_BIT);
    { sc_t s = sc(REF_MS8, 80, 4, 4, 4, 2, REF_TEX); s.sentinel = true; gr_run(&s, &g);
      assert(g.ms.ms_images_created == 0 && g.ms.ms_compat_1x == 1 && g.ms.resolves_by_copy == 1 && g.draws == 1 && g.val == 0);
      puts("PASS compat: injected 1-sample-only device: 4x asked -> compat_1x counted, copy resolve, renders"); }
    lagfx_fmtcap_test_backend = NULL;

    /* mode switched after a compat image exists: an exact-mode pass must not render into the approximation */
    { sc_t s = sc(REF_MS8, 80, 4, 4, 8, 2, REF_TEX); s.sentinel = true; gr_run_switch(&s, &g);
      assert(g.ms.ms_compat_clamped == 1 && g.ms.ms_pass_refused == 1 && g.draws == 0 && is_sentinel(g.bgra, sizeof g.bgra) && g.val == 0);
      puts("PASS exact-mode pass over a texture made in compat mode: attachment refused (ms_pass_refused), nothing drawn"); }
    unsetenv("LAGFX_MSAA_COMPAT");

    /* 11: bad descriptor shapes: refused, counted, the pass names a texture that does not exist (no draw) */
    { sc_t a = sc(REF_MSX, 80, 4, 4, 1, 2, REF_TEX); gr_run(&a, &g); assert(g.ms.ms_tex_bad_shape >= 1 && g.msaa_refused >= 1 && g.draws == 0);   /* type 4 with 1 sample */
      sc_t b = sc(REF_MSX, 80, 4, 8, 4, 2, REF_TEX); gr_run(&b, &g); assert(g.ms.ms_tex_bad_shape >= 1 && g.draws == 0);                                  /* 2D multisample array */
      sc_t c = sc(REF_MSX, 80, 4, 2, 4, 2, REF_TEX); gr_run(&c, &g); assert(g.ms.ms_tex_bad_shape >= 1 && g.draws == 0);                                  /* 4 samples on a plain 2D type */
      puts("PASS bad shapes (type 4 with 1 sample, type 8, 4 samples on type 2): refused with ms_tex_bad_shape, no draw"); }

    /* 12: framebuffer fetch (input attachment) in a multisampled pass: refused, counted; MS + fb-fetch is NOT supported (Vulkan input attachments of a multisampled image need a multisampled SubpassData) */
    { sc_t s = sc(REF_MS8, 80, 4, 4, 4, 2, REF_TEX); s.fs = FS_F; gr_run(&s, &g);
      assert(g.ms.fbfetch_ms_refused >= 1 && g.draws == 0 && g.skipped >= 1);
      puts("PASS framebuffer fetch in a multisampled pass: refused and counted (fbfetch_ms_refused), draw skipped; the split-draw path is not entered"); }

    /* 13: mixed sample counts in one pass: the 1x second attachment is ignored, counted */
    { sc_t s = sc(REF_MS8, 80, 4, 4, 4, 2, REF_TEX); s.slot1_ref = REF_TEX2; gr_run(&s, &g);
      assert(g.ms.attach_mixed_ignored == 1 && g.ms.resolves == 1 && g.draws == 1 && g.val == 0);
      puts("PASS mixed sample counts in one pass: the other attachment is ignored (attach_mixed_ignored)"); }

    /* store actions: 3 = StoreAndMultisampleResolve keeps the multisampled contents (pass B LOADs them and resolves again: same image), 2 = MultisampleResolve resolves too; the store op chosen for 2 is DONT_CARE */
    { static mp_t m;
      VkAttachmentStoreOp so; bool rs;
      assert(lagfx_msaa_store_map(2, true, &so, &rs) && so == VK_ATTACHMENT_STORE_OP_DONT_CARE && rs);   /* chosen: action 2 does not keep the multisampled contents */
      assert(lagfx_msaa_store_map(3, true, &so, &rs) && so == VK_ATTACHMENT_STORE_OP_STORE && rs);
      g_act = 3; mp_run(b_store_keep, &m, false);
      { const bool a = tex_matches(&m, false), b = tex_matches(&m, true); assert(a != b); const bool flip = b; assert(msx_matches(&m, flip));   /* both resolve targets equal the coverage reference of the same orientation */
        assert(m.ms.resolves == 2 && m.ms.ms_passes == 2 && m.draws == 1 && m.ms.resolve_no_target == 0 && m.ms.store_unknown == 0); }
      g_act = 3; mp_run(b_store_clear_ctl, &m, false);
      { assert(tex_matches(&m, false) != tex_matches(&m, true)); assert(!msx_matches(&m, false) && !msx_matches(&m, true)); }   /* control: when pass B clears instead of loading, its resolve is NOT the triangle: the comparison above does tell them apart */
      g_act = 2; mp_run(b_store_keep, &m, false);
      { const bool a = tex_matches(&m, false), b = tex_matches(&m, true); assert(a != b); assert(m.ms.resolves == 2 && m.draws == 1); }   /* action 2: the resolve target of pass A is correct; what pass B LOADs afterwards is undefined in Metal and is NOT asserted */
      puts("PASS store actions: 3 keeps the multisampled contents (a later Load pass resolves to the same image), 2 resolves correctly (DONT_CARE store op chosen, MS contents after it are not relied on); negative control (Clear instead of Load) differs"); }

    /* pipeline cache key includes the sample count: the same pipeline object / colour format at 1x and 4x builds two VkPipelines, a return to 1x hits the cache */
    { static mp_t m, one;
      mp_run(b_pipe_key_1x_only, &one, false);
      assert(one.pipes_built == 1 && one.ms.pipes_ms == 0 && one.draws == 2);
      mp_run(b_pipe_key, &m, false);
      assert(m.pipes_built == 2 && m.ms.pipes_ms == 1 && m.draws == 3 && m.ms.draws_ms == 1 && m.skipped == 0);   /* 1x, 4x = two distinct pipelines; the third pass (1x again) is a cache hit */
      assert(msx_matches(&m, false) != msx_matches(&m, true));   /* the 4x pass (through its own pipeline) resolved to the coverage reference */
      { unsigned full = 0, partial = 0; for (uint32_t i = 0; i < RT_W * RT_H; i++) { uint32_t v; memcpy(&v, m.tex + i * 4, 4); const uint32_t r = (v >> 16) & 255; if (r == 0) continue; if (r >= 127 && r <= 128) full++; else partial++; }
        assert(full > 40 && partial == 0); }   /* the 1x passes (through the 1x pipeline) have no partial-coverage pixel */
      puts("PASS pipeline cache: same shader / format at 1x and 4x -> 2 pipelines (pipes_built=2, pipes_ms=1), 1x again is a hit; validation: no message in the window"); }

    /* depth / stencil resolve in a 4x pass: stored only, counted and printed under unsupported[ds_resolve=..] */
    { static mp_t m; mp_run(b_ds_resolve, &m, true);
      assert(m.ms.ms_passes == 1 && m.ms.ds_resolve_unsupported == 1 && m.draws == 1 && m.ms.ms_pass_refused == 0);
      assert(strstr(m.line, "unsupported[ds_resolve=1 "));
      puts("PASS depth resolve (store action 3 on a 4x depth attachment): not implemented, counted ds_resolve_unsupported and printed under unsupported[]"); }

    /* the unsupported[] group names every unimplemented multisample feature */
    { lagfx_msaa_stats_t z; memset(&z, 0, sizeof z); z.ds_resolve_unsupported = 1, z.sampled_ms_refused = 2, z.air_ms_ops = 3, z.air_sample_builtins = 4, z.fbfetch_ms_refused = 5, z.tile_ms_refused = 6, z.blit_ms_refused = 7, z.capture_ms_skipped = 8;
      char l[1100]; lagfx_msaa_stats_line(&z, l, sizeof l);
      assert(strstr(l, "unsupported[ds_resolve=1 sampled_ms=2 fbfetch_ms=5 tile_ms=6 blit_ms=7 capture_ms=8 air_ms_ops=3 air_sample_id_mask=4]") && strstr(l, "mode=exact") && strlen(l) < 1000);
      setenv("LAGFX_MSAA_COMPAT", "1", 1); lagfx_msaa_stats_line(&z, l, sizeof l); assert(strstr(l, "mode=compat")); setenv("LAGFX_MSAA_COMPAT", "0", 1); lagfx_msaa_stats_line(&z, l, sizeof l); assert(strstr(l, "mode=exact")); unsetenv("LAGFX_MSAA_COMPAT");
      puts("PASS g7-gpu-msaa line: mode=exact|compat (only the string \"1\" selects compat), unsupported[ds_resolve sampled_ms fbfetch_ms tile_ms blit_ms capture_ms air_ms_ops air_sample_id_mask]"); }

    /* diagnostics line */
    { sc_t s = sc(REF_MS16, 115, 8, 4, 4, 3, REF_TEX2); gr_run(&s, &g);
      printf("%s\n", g.line); assert(strstr(g.line, "g7-gpu-msaa:") && strstr(g.line, "mode=exact images[ms_created=1 exact_refused=0 compat_clamped=0 compat_1x=0") && strstr(g.line, "resolves") != NULL);
      printf("%s\n", g.line);
      puts("PASS g7-gpu-msaa line carries the counts"); }

    lagfx_device_free(g_dev);
    free(g_mem);
    return 0;
}
#else
int main(void) { puts("SKIP: built without Vulkan"); return 77; }
#endif
