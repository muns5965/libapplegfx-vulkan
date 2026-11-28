/*
 * libapplegfx-vulkan — ARM64 GPU exec (0x43 / 0x45): execution of Render segments on Vulkan. See arm64_exec_run.h.
 */
#include "arm64_exec_run.h"
#include "../air2spv/spv_reflect.h"
#include "arm64_objects.h"
#include "arm64_pipeline.h"
#include "arm64_depth_stencil.h"
#include "arm64_exec_decode.h"
#include "arm64_draw_decode.h"
#include "arm64_sampler.h"
#include "arm64_resources.h"
#include "arm64_tile.h"
#include "arm64_completion.h"
#include "arm64_hold.h"
#include "arm64_dump.h"
#include "draw_diag.h"
#include "gap_diag.h"
#include "../device.h"
#include "../common/le.h"
#include "../common/log.h"

#include <stdlib.h>
#include <string.h>

#ifndef LAGFX_HAVE_VULKAN

bool lagfx_a64_exec_enabled(const lagfx_protocol_t *p) { (void)p; return false; }
void lagfx_a64_exec_set_pipeline_resolver(lagfx_protocol_t *p, lagfx_a64_pipeline_resolver_t fn, void *ctx) { (void)p; (void)fn; (void)ctx; }
bool lagfx_a64_exec_begin(lagfx_protocol_t *p, lagfx_task_entry_t *task, const uint8_t *res, uint32_t nres) { (void)p; (void)task; (void)res; (void)nres; return false; }
void lagfx_a64_exec_segment(lagfx_protocol_t *p, lagfx_task_entry_t *task, const uint8_t *buf, uint32_t avail, uint64_t len, const lagfx_exec_seg_t *seg) {
    (void)p; (void)task; (void)buf; (void)avail; (void)len; (void)seg;
}
lagfx_a64_exec_status_t lagfx_a64_exec_end(lagfx_protocol_t *p) { (void)p; return LAGFX_A64_EXEC_NONE; }
uint32_t lagfx_a64_exec_poll(lagfx_protocol_t *p) { (void)p; return 0; }
bool lagfx_a64_exec_render_op(lagfx_protocol_t *p, uint32_t tag, const uint8_t *payload, size_t len) { (void)p; (void)tag; (void)payload; (void)len; return false; }
void lagfx_a64_exec_drop_task(lagfx_protocol_t *p, uint32_t task_id) { (void)p; (void)task_id; }
void lagfx_a64_exec_free(lagfx_protocol_t *p) { (void)p; }
uint32_t lagfx_a64_exec_deferred_count(const lagfx_protocol_t *p) { (void)p; return 0; }
void lagfx_a64_exec_object_counts(const lagfx_protocol_t *p, uint32_t *live, uint64_t *created, uint64_t *destroyed) {
    (void)p; if (live) *live = 0; if (created) *created = 0; if (destroyed) *destroyed = 0;
}
bool lagfx_a64_exec_readback_texture(lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint32_t ref, void *dst, size_t dst_size, uint32_t *w, uint32_t *h) {
    (void)p; (void)task; (void)ref; (void)dst; (void)dst_size; (void)w; (void)h; return false;
}

#else  /* LAGFX_HAVE_VULKAN */

#include "../vulkan/instance.h"
#include "../vulkan/command.h"
#include "../vulkan/draw_record.h"
#include "../vulkan/format_caps.h"
#include "../vulkan/filter_cubic.h"
#include "../translate/render_encoder.h"
#include "../handlers/compute/render_inner_ops.h"
#include "render_pass.h"
#include "vulkan/render_pass.h"
#include "arm64_capture_rec.h"
#include "arm64_msaa.h"
#include "../vulkan/msaa_host.h"
#include <vulkan/vulkan.h>

#define A64_MAX_VB    31u
#define A64_MAX_FB    32u
#define A64_MAX_FSAMP 16u
#define A64_PIPE_CACHE 8u
#define A64_FS_VARIANTS 8u
#define A64_GATHER_MAX (8ull << 20)
/* Gather limits per exec. A64_GATHER_CACHE bounds the number of private copies (each is one of the exec's MAX_TMP=64 scratch slots, which blit tail staging also
 * uses: 32 leaves 32 for those). A64_GATHER_BUDGET bounds their total bytes: 64 MiB = the worst case the old limits allowed (8 copies x A64_GATHER_MAX 8 MiB), so
 * host memory per exec does not grow; the copies are host-visible mirror-sized scratch, freed with the exec. Each copy is a range "to the end of the buffer"
 * (the shader's real extent is unknown), so copies are usually far smaller than A64_GATHER_MAX. */
#define A64_GATHER_CACHE 32u
#define A64_GATHER_BUDGET (64ull << 20)

enum { K_PIPE = 3, K_SAMP = 4, K_DSS = 5 };

/* gen: unique per creation (a replaced (task, ref) is a new object with a new gen); every cache key below includes it. Mirror buffers/textures carry their own gen. */
typedef struct { uint32_t kind; uint64_t gen; } objhdr_t;
#define A64_SAMP_VARIANTS 4u
/* A sampler plus the variants made for setFragmentSamplerState:lodMinClamp:lodMaxClamp: (tag 0x71): same state, other LOD clamps; owned by this object. */
typedef struct {
    objhdr_t h; VkSampler s;
    VkSamplerCreateInfo ci; bool mip;                                        /* what s was made from; mip = the descriptor is mip mapped (clamps apply) */
    struct { float lo, hi; VkSampler s; } var[A64_SAMP_VARIANTS]; uint32_t nvar;
    uint32_t cflags;                                                         /* LAGFX_A64_SAMP_CUBIC_MIN / _MAG: the guest's filter field was 2 (opt-in cubic path, filter_cubic.h) */
    struct { VkSampler base, s; } cub[A64_SAMP_VARIANTS + 1u]; uint32_t ncub;   /* cubic twins of s / var[i].s, made on first use (only with LAGFX_CUBIC=1 and the extension enabled); owned by this object */
} a64_samp_t;
typedef struct { objhdr_t h; lagfx_a64_dss_t d; } a64_dss_t;   /* a decoded depth/stencil state object (tag 4) */
typedef struct {
    objhdr_t h;
    lagfx_a64_pipeline_src_t src;
    struct { VkPipeline pl; VkFormat fmt[4]; VkFormat dfmt[2]; uint8_t topo, samples, fmask; uint64_t lkey; } cache[A64_PIPE_CACHE];   /* topo: topology | (1 + variant index) << 4 for an unnormalized-sampler shader variant */   /* one VkPipeline per (colour formats of slots 0..3, depth / stencil attachment formats, topology, descriptor binding table key), owned by THIS object only */
    uint32_t ncache;
    /* Shader variants for draws that bind an unnormalized-coordinates sampler (spv_unnorm.c): keyed by the set of unnormalized sampler slots of the vertex / fragment table. rc 0 = the shader never
     * samples through them (original shader), 1 = rewritten module(s), -1 = an unsupported use (draw skipped). Built lazily at the first such draw, owned by this object. */
    struct { uint32_t vmask, fmask; uint8_t ms; int rc; VkShaderModule vm, fm; } fv[A64_FS_VARIANTS];   /* ms: the fragment module is the multisampled-fetch rewrite (spv_msfetch.c) */ uint32_t nfv;
    uint32_t ref;
} a64_pipe_t;

typedef struct { int kind; void *obj; VkPipeline pipe; VkDescriptorSet ds; } deferred_t;   /* kind 0 = whole object, 1 = a VkPipeline, 2 = a descriptor set */

/* Buffers, textures and the exec's command buffer / fence / write-backs belong to the resource mirror (arm64_resources.c): ONE exec context and ONE
 * command buffer per exec. This module keeps only what is Render specific: the (task, ref) table of pipeline objects and the open pass. */
typedef struct { uint32_t ref; lagfx_a64_texdiag_t dg; } a64_texmiss_t;   /* an unresolved texture handle: ref + lagfx_a64_res_diagnose() result at the time the record named it */

typedef struct {
    lagfx_a64_objects_t *objs;
    lagfx_protocol_t *p;
    struct lagfx_vk_state *vk;
    lagfx_a64_pipeline_resolver_t resolver; void *resolver_ctx;
    lagfx_a64_rexec_t *x;            /* the exec being recorded (mirror context), NULL outside an exec */
    lagfx_a64_cap_t *cap; bool cap_pass;   /* render replay capture (LAGFX_ARM64_DUMP_DATA=1): the exec's recorder (owned by the mirror's finish hook once the exec ended) and an open captured pass */
    uint32_t fsref[A64_MAX_FSAMP], vsref[A64_MAX_FSAMP];   /* object refs of the bound fragment / vertex samplers (capture only) */
    struct { uint8_t bkind, stage; int16_t slot; uint32_t vkb, ref; } ddm;   /* the binding that stopped the current draw (draw_diag.h); reset per draw, written on failure paths only */
    uint32_t exec_draw;              /* draw records seen in this exec so far (log context of an unresolved texture) */
    bool pass_open;                  /* a tag 26 was seen and the segment chain has not ended it */
    bool last_fetch;                 /* the previous draw of this pass read input attachments (framebuffer fetch) */
    bool rendering;                  /* the render encoder has a vkCmdBeginRendering open (pass_open without it = a pass with no usable target) */
    lagfx_a64_obj_t *rt; uint32_t rt_w, rt_h;   /* the first mirror texture being drawn into (rts[first used slot]) */
    lagfx_a64_obj_t *rts[4]; uint32_t nslots;   /* colour attachments by their ORIGINAL slot (fragment output location); a null entry = unused slot; nslots = highest used slot + 1 */
    a64_pipe_t *pipe; bool pipe_set, pipe_failed;   /* pipe_failed: tag 116 named a pipeline the resolver could not make */
    const lagfx_task_entry_t *task;  /* the exec's task */
    bool dispatching, consumed;      /* inside lagfx_render_inner_dispatch for a tag of this module */
    lagfx_translate_render_state_t rs;   /* the existing render encoder: begin / bind / draw / end of the open pass */
    struct { lagfx_a64_obj_t *b; uint64_t off; } vb[A64_MAX_VB];
    struct { lagfx_a64_obj_t *b; uint64_t off; } fb[A64_MAX_FB];   /* fragment buffers (tags 110/111) */
    lagfx_a64_obj_t *ftex[A64_MAX_FB];                             /* fragment textures (114): mirror textures, GENERAL layout */
    a64_samp_t *fsamp[A64_MAX_FSAMP];                              /* fragment samplers (112) */
    lagfx_a64_obj_t *vtex[A64_MAX_FB];                             /* vertex textures (129) */
    struct { uint32_t ref, why, type; } vbmiss[A64_MAX_VB], fbmiss[A64_MAX_FB];   /* why vb / fb slot i holds no buffer (why 0 = never named since the encoder began, 1 ref did not resolve, 2 not a buffer; type = guest entry type) */
    a64_texmiss_t fmiss[A64_MAX_FB], vmiss[A64_MAX_FB];            /* why ftex / vtex slot i is NULL (0 = never named since the encoder began): split of desc_unbound_why[3] */
    a64_samp_t *vsamp[A64_MAX_FSAMP]; VkSampler vvar[A64_MAX_FSAMP];   /* vertex samplers (127) and the LOD-clamp variants of tag 128 */
    VkPipeline bound_vk;                                           /* the VkPipeline bound in the open pass */
    VkSampler defsamp; VkDescriptorPool dpool; VkDescriptorSet last_ds; uint64_t last_ds_key;   /* descriptor sets: one per draw, reused while the binding key is unchanged */
    uint64_t gen_ctr; VkDeviceSize min_sb_align;
    VkSampler fvar[A64_MAX_FSAMP];                                 /* fragment sampler slot overridden by a LOD-clamp bind (0x71): a variant owned by fsamp[i], else NULL */
    VkDeviceSize max_sb_range;
    struct { uint64_t gen, wgen, off, range; VkBuffer buf; } gc[A64_GATHER_CACHE]; uint32_t ngc; uint64_t gc_bytes, gc_budget; uint32_t gc_cap;   /* aligned copies made in this exec (gather_copy) */
    lagfx_draw_region_t region;
    /* depth / stencil attachment of the open pass (NULL / UNDEFINED = none) and the encoder state Metal keeps outside the pipeline; all of it is reset by a new pass
     * (a new Metal encoder starts from the defaults) and applied as Vulkan dynamic state before each draw */
    lagfx_a64_obj_t *dsobj; VkFormat ds_dfmt, ds_sfmt;
    struct { lagfx_a64_dss_t dss; uint32_t sref[2]; uint32_t cull, winding; float bias[3]; float blend_color[4]; bool dirty; } es;
    bool wrote_storage;              /* a draw of the open pass may write storage images: pass_end adds a shader-write barrier */
    deferred_t *def; uint32_t ndef, capdef;
    lagfx_a64_tile_t *tile;          /* tile shader stage (arm64_tile.c), created by the first tile record */
    lagfx_a64_msaa_pass_t ms;        /* multisample state of the open pass (arm64_msaa.c): sample count, deferred store tags, resolve targets */
    struct { VkRenderingAttachmentInfo ca[4], dai, sai; bool has_d, has_s; VkImage img; } rsnap;   /* how the open pass began: a tile dispatch ends and resumes it (LOAD ops) */
} a64_run_t;

#define RUN(p) ((a64_run_t *)(p)->a64_exec)
#define CNT(p) ((p)->diag2.exec.run)

#define RUN(p) ((a64_run_t *)(p)->a64_exec)
#define CNT(p) ((p)->diag2.exec.run)

bool lagfx_a64_exec_enabled(const lagfx_protocol_t *p) {
    if (!lagfx_proto_arm64(p)) return false;
    const char *e = getenv("LAGFX_ARM64_EXEC");
    return e && e[0] == '1' && e[1] == 0;
}

static struct lagfx_vk_state *get_vk(lagfx_protocol_t *p) {
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    struct lagfx_vk_state *vk = dev ? dev->vk : NULL;
    return (vk && vk->initialized && vk->device != VK_NULL_HANDLE) ? vk : NULL;
}

/* The depth / stencil and raster state is applied with the Vulkan 1.3 core dynamic-state commands (cull mode, front face, depth test / write / compare, stencil
 * test / op). A device below 1.3 keeps the previous behaviour (state ignored, counted). */
static bool vk_core13(const struct lagfx_vk_state *vk) {
    uint32_t v = vk->phys_props.apiVersion;
    return VK_VERSION_MAJOR(v) > 1u || (VK_VERSION_MAJOR(v) == 1u && VK_VERSION_MINOR(v) >= 3u);
}

/* ---- destruction (parked while the GPU may still use the object: a command buffer being recorded or a submission in flight) ---- */

static void destroy_pipeline_vk(struct lagfx_vk_state *vk, VkPipeline pl) { if (vk && pl) vkDestroyPipeline(vk->device, pl, NULL); }
static void destroy_obj_now(a64_run_t *r, void *obj) {
    objhdr_t *h = obj;
    if (h->kind == K_PIPE) {
        a64_pipe_t *pp = obj;
        for (uint32_t i = 0; i < pp->ncache; i++) destroy_pipeline_vk(r->vk, pp->cache[i].pl);
        for (uint32_t i = 0; r->vk && i < pp->nfv; i++) { if (pp->fv[i].vm) vkDestroyShaderModule(r->vk->device, pp->fv[i].vm, NULL); if (pp->fv[i].fm) vkDestroyShaderModule(r->vk->device, pp->fv[i].fm, NULL); }
        if (pp->src.release) pp->src.release(pp->src.user);
        free(pp);
    } else if (h->kind == K_SAMP) {
        a64_samp_t *sm = obj;
        if (r->vk && sm->s) vkDestroySampler(r->vk->device, sm->s, NULL);
        for (uint32_t i = 0; r->vk && i < sm->nvar; i++) if (sm->var[i].s) vkDestroySampler(r->vk->device, sm->var[i].s, NULL);
        for (uint32_t i = 0; r->vk && i < sm->ncub; i++) if (sm->cub[i].s) vkDestroySampler(r->vk->device, sm->cub[i].s, NULL);
        free(sm);
    } else free(obj);
}
static void defer(a64_run_t *r, int kind, void *obj, VkPipeline pl) {
    if (r->ndef == r->capdef) {
        uint32_t nc = r->capdef ? r->capdef * 2u : 64u;
        deferred_t *n = realloc(r->def, nc * sizeof *n);
        if (!n) return;   /* cannot park it: leaking one object is the safe choice (freeing under a recording command buffer is not) */
        r->def = n, r->capdef = nc;
    }
    r->def[r->ndef++] = (deferred_t){ kind, obj, pl, VK_NULL_HANDLE };
}
static void obj_destroy_cb(void *ctx, void *obj) {
    a64_run_t *r = ctx;
    if (lagfx_a64_res_busy(r->p)) defer(r, 0, obj, VK_NULL_HANDLE); else destroy_obj_now(r, obj);   /* in use by recorded or still-running work */
}
/* Only once no exec records and none is in flight (its fence signalled). */
static void flush_deferred(a64_run_t *r) {
    if (lagfx_a64_res_busy(r->p)) return;
    lagfx_a64_tile_flush(r->tile);   /* its descriptor sets are no longer referenced */
    for (uint32_t i = 0; i < r->ndef; i++) {
        if (r->def[i].kind == 2) { if (r->vk && r->dpool) vkFreeDescriptorSets(r->vk->device, r->dpool, 1, &r->def[i].ds); }
        else if (r->def[i].kind) destroy_pipeline_vk(r->vk, r->def[i].pipe);
        else destroy_obj_now(r, r->def[i].obj);
    }
    r->ndef = 0;
    r->last_ds = VK_NULL_HANDLE, r->last_ds_key = 0;
}

/* ---- object creation: only pipelines live in this table; buffers and textures are the mirror's (arm64_resources.c) ---- */

/* Serialized sampler (tag 3): decoded by arm64_sampler.c (layout and what is / is not mapped are documented there). */
static void *make_sampler(a64_run_t *r, lagfx_protocol_t *p, const lagfx_task_entry_t *t, const uint32_t e[3]) {
    uint8_t d[32];
    uint32_t size = e[0] >> 8;
    if (!r->vk || size < 20u || !lagfx_arm64_task_read(p, t, (uint64_t)e[2] << 32 | e[1], d, size < sizeof d ? size : sizeof d)) { CNT(p).obj_unsupported++; return NULL; }
    a64_samp_t *sm = calloc(1, sizeof *sm);
    if (!sm) return NULL;
    sm->h.kind = K_SAMP;
    uint32_t sf = lagfx_a64_sampler_decode_ex(d, size < sizeof d ? size : sizeof d, r->vk->have_sampler_anisotropy, r->vk->phys_props.limits.maxSamplerAnisotropy, r->vk->have_sampler_mirror_clamp, &sm->ci);
    sm->mip = (sf & LAGFX_A64_SAMP_MIPMAPPED) != 0;
    sm->cflags = sf & (LAGFX_A64_SAMP_CUBIC_MIN | LAGFX_A64_SAMP_CUBIC_MAG);
    if (sf & LAGFX_A64_SAMP_COMPARE_IGNORED) CNT(p).ext_samp_compare++;
    if (sf & LAGFX_A64_SAMP_UNNORM_APPLIED) CNT(p).ext_samp_unnorm++;
    if (sf & LAGFX_A64_SAMP_MIRROR_CLAMP_APPROX) CNT(p).ext_samp_mirror_clamp++;
    if (sf & LAGFX_A64_SAMP_FILTER_UNKNOWN) CNT(p).ext_samp_filter_unknown++;
    if (sf & (LAGFX_A64_SAMP_ADDR_UNKNOWN | LAGFX_A64_SAMP_MIP_UNKNOWN | LAGFX_A64_SAMP_FILTER_UNKNOWN)) {   /* enum values outside the public Metal range: counted + logged once per state word (sampler_cache.c) */
        if (sf & LAGFX_A64_SAMP_ADDR_UNKNOWN) CNT(p).ext_samp_addr_unknown++;
        if (sf & LAGFX_A64_SAMP_MIP_UNKNOWN) CNT(p).ext_samp_mip_unknown++;
        lagfx_sampler_diag_rt_count((sf & LAGFX_A64_SAMP_ADDR_UNKNOWN) != 0, (sf & LAGFX_A64_SAMP_MIP_UNKNOWN) != 0, (sf & LAGFX_A64_SAMP_FILTER_UNKNOWN) != 0, lagfx_le32(d + 12));
    }
    if (vkCreateSampler(r->vk->device, &sm->ci, NULL, &sm->s) != VK_SUCCESS) { free(sm); p->diag2.exec.pipe.sampler_failed++; return NULL; }
    p->diag2.exec.pipe.samplers++;
    return sm;
}

/* The sampler with other LOD clamps (setFragmentSamplerState:lodMinClamp:lodMaxClamp:). A non-mip-mapped descriptor keeps its single level whatever the clamps
 * say. Variants are cached on the sampler object (at most A64_SAMP_VARIANTS; further ones fall back to the plain sampler, counted). */
static VkSampler sampler_variant(a64_run_t *r, a64_samp_t *sm, float lo, float hi) {
    if (!sm->mip) return sm->s;
    if (!(lo >= 0.f)) lo = 0.f;
    if (!(hi >= lo)) return sm->s;
    if (hi > VK_LOD_CLAMP_NONE) hi = VK_LOD_CLAMP_NONE;
    for (uint32_t i = 0; i < sm->nvar; i++) if (sm->var[i].lo == lo && sm->var[i].hi == hi) return sm->var[i].s;
    VkSamplerCreateInfo ci = sm->ci;
    ci.minLod = lo, ci.maxLod = hi;
    if (sm->nvar == A64_SAMP_VARIANTS) { CNT(r->p).ext_samp_lod_dropped++; return sm->s; }   /* a recorded descriptor set may still use an old one: never replaced, the plain sampler is used */
    VkSampler v = VK_NULL_HANDLE;
    if (vkCreateSampler(r->vk->device, &ci, NULL, &v) != VK_SUCCESS) return sm->s;
    sm->var[sm->nvar].lo = lo, sm->var[sm->nvar].hi = hi, sm->var[sm->nvar].s = v, sm->nvar++;
    return v;
}

static void *make_dss(a64_run_t *r, lagfx_protocol_t *p, const lagfx_task_entry_t *t, const uint32_t e[3]) {
    uint8_t d[LAGFX_A64_DSS_SIZE];
    a64_dss_t *o;
    if ((e[0] >> 8) != LAGFX_A64_DSS_SIZE || !lagfx_arm64_task_read(p, t, (uint64_t)e[2] << 32 | e[1], d, sizeof d)) { CNT(p).dss_bad++; return NULL; }
    o = calloc(1, sizeof *o);
    if (!o) return NULL;
    if (!lagfx_a64_dss_decode(d, sizeof d, &o->d)) { CNT(p).dss_bad++; free(o); return NULL; }
    (void)r;
    o->h.kind = K_DSS;
    CNT(p).dss_decoded++;
    return o;
}

static void *make_pipeline(a64_run_t *r, lagfx_protocol_t *p, const lagfx_task_entry_t *t, uint32_t ref, const uint32_t e[3]) {
    if (!r->resolver) { CNT(p).pipeline_unresolved++; return NULL; }
    a64_pipe_t *pp = calloc(1, sizeof *pp);
    if (!pp) return NULL;
    pp->h.kind = K_PIPE, pp->ref = ref;
    const uint64_t hold_t0 = lagfx_hold_now_ns();
    const bool resolved = r->resolver(r->resolver_ctx, p, t, ref, e, &pp->src);
    lagfx_hold_note(LAGFX_HOLD_RESOLVE, hold_t0, 0);
    if (!resolved) { CNT(p).pipeline_unresolved++; free(pp); return NULL; }
    return pp;
}

static void *obj_create_inner(a64_run_t *r, lagfx_protocol_t *p, const lagfx_task_entry_t *t, uint32_t ref, const uint32_t e[3]) {
    lagfx_a64_dump_touch_object(p, t, ref, e);   /* LAGFX_ARM64_DUMP=1: a pipeline / sampler first read here is captured even if no resource list named it */
    if ((e[0] & 0xffu) == LAGFX_A64_OBJ_SERIALIZED) {   /* by the payload's own tag: 3 = sampler, 4 = depth/stencil state; anything else goes to the pipeline resolver, which rejects what it cannot execute */
        uint8_t tg[4];
        bool have = (e[0] >> 8) >= 4u && lagfx_arm64_task_read(p, t, (uint64_t)e[2] << 32 | e[1], tg, 4);
        if (have && lagfx_le32(tg) == 3u) return make_sampler(r, p, t, e);
        if (have && lagfx_le32(tg) == LAGFX_A64_DSS_TAG) return make_dss(r, p, t, e);
        return make_pipeline(r, p, t, ref, e);
    }
    CNT(p).obj_unsupported++;
    return NULL;
}
static void *obj_create_cb(void *ctx, lagfx_protocol_t *p, const lagfx_task_entry_t *t, uint32_t ref, const uint32_t e[3]) {
    a64_run_t *r = ctx;
    void *o = obj_create_inner(r, p, t, ref, e);
    if (o) ((objhdr_t *)o)->gen = ++r->gen_ctr;
    return o;
}

static a64_run_t *run_get(lagfx_protocol_t *p, bool create) {
    if (p->a64_exec || !create) return RUN(p);
    a64_run_t *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->p = p;
    lagfx_a64_obj_ops_t ops = { r, obj_create_cb, obj_destroy_cb };
    r->objs = lagfx_a64_objects_new(&ops);
    if (!r->objs) { free(r); return NULL; }
    r->resolver = lagfx_a64_pipeline_resolve;   /* the default resolver of ARM64 mode (arm64_pipeline.c); lagfx_a64_exec_set_pipeline_resolver overrides it */
    p->a64_exec = r;
    return r;
}

static void *get_kind(a64_run_t *r, lagfx_protocol_t *p, const lagfx_task_entry_t *t, uint32_t ref, uint32_t kind) {
    void *o = lagfx_a64_objects_get(r->objs, p, t, ref, NULL);
    return (o && ((objhdr_t *)o)->kind == kind) ? o : NULL;
}
static a64_pipe_t *get_pipe(a64_run_t *r, lagfx_protocol_t *p, const lagfx_task_entry_t *t, uint32_t ref) { return get_kind(r, p, t, ref, K_PIPE); }

static void pass_end(a64_run_t *r);
static void reset_frag_tables(a64_run_t *r);

/* ---- API: configuration and lifetime ---- */

void lagfx_a64_exec_set_pipeline_resolver(lagfx_protocol_t *p, lagfx_a64_pipeline_resolver_t fn, void *ctx) {
    if (!p) return;
    a64_run_t *r = run_get(p, true);
    if (r) r->resolver = fn, r->resolver_ctx = ctx;
}

void lagfx_a64_exec_drop_task(lagfx_protocol_t *p, uint32_t task_id) {
    a64_run_t *r = p ? RUN(p) : NULL;
    if (r) {
        r->vk = get_vk(p);
        lagfx_a64_objects_drop_task(r->objs, task_id);
    }
    lagfx_a64_res_drop_task(p, task_id);   /* mirror objects: freed after the fence of an exec that may use them */
}

void lagfx_a64_exec_free(lagfx_protocol_t *p) {
    if (!p) return;
    lagfx_a64_exec_lock(p);   /* no drain / poll while the exec state is torn down */
    a64_run_t *r = RUN(p);
    if (r) {
        r->vk = get_vk(p);
        pass_end(r);                          /* an exec abandoned half-way (nothing submitted) */
    }
    bool gpu_busy = lagfx_a64_res_is_pending(p) && lagfx_a64_res_poll(p, 2000) == LAGFX_A64_PENDING;   /* one bounded wait; what is not done is leaked, never freed under the GPU */
    lagfx_a64_res_destroy(p);                 /* mirror: abandons an open exec, frees every object (needs the Vulkan device alive) */
    if (!r) { lagfx_a64_exec_unlock(p); return; }
    if (gpu_busy) r->vk = NULL;               /* the Vulkan pipelines may still be in use: leak them */
    p->a64_exec = NULL;                       /* res busy state is gone: the table frees its objects at once */
    lagfx_a64_objects_free(r->objs);
    for (uint32_t i = 0; i < r->ndef; i++) {
        if (r->def[i].kind == 2) { if (r->vk && r->dpool) vkFreeDescriptorSets(r->vk->device, r->dpool, 1, &r->def[i].ds); }
        else if (r->def[i].kind) destroy_pipeline_vk(r->vk, r->def[i].pipe);
        else destroy_obj_now(r, r->def[i].obj);
    }
    if (r->vk) {   /* gpu_busy leaves r->vk NULL: with work in flight these are leaked, never destroyed under the GPU */
        if (r->dpool) vkDestroyDescriptorPool(r->vk->device, r->dpool, NULL);
        if (r->defsamp) vkDestroySampler(r->vk->device, r->defsamp, NULL);
        lagfx_a64_tile_free(r->tile);
    }
    free(r->def);
    free(r);
    lagfx_a64_exec_unlock(p);
}

void lagfx_a64_exec_object_counts(const lagfx_protocol_t *p, uint32_t *live, uint64_t *created, uint64_t *destroyed) {
    lagfx_a64_obj_stats_t st;
    a64_run_t *r = p ? RUN(p) : NULL;
    lagfx_a64_objects_stats(r ? r->objs : NULL, &st);
    uint32_t ml; uint64_t mc, md;
    lagfx_a64_res_counts((lagfx_protocol_t *)p, &ml, &mc, &md);
    if (live) *live = st.live + ml;
    if (created) *created = st.created + mc;
    if (destroyed) *destroyed = st.destroyed + md;
}

uint32_t lagfx_a64_exec_deferred_count(const lagfx_protocol_t *p) { a64_run_t *r = p ? RUN(p) : NULL; return r ? r->ndef : 0; }

bool lagfx_a64_exec_readback_texture(lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint32_t ref, void *dst, size_t dst_size, uint32_t *width, uint32_t *height) {
    return p && task && lagfx_a64_res_readback(p, task->id, ref, dst, dst_size, width, height);
}


/* ---- the exec ---- */

static void tag_done(lagfx_protocol_t *p, uint32_t tag) { if (tag < 512u) CNT(p).tag_done[tag]++; else CNT(p).tag_over++; }
static void tag_partial(lagfx_protocol_t *p, uint32_t tag) { if (tag < 512u) CNT(p).tag_partial[tag]++; else CNT(p).tag_over++; }
static void tag_unsup(lagfx_protocol_t *p, uint32_t tag) { if (tag < 512u) CNT(p).tag_unsup[tag]++; else CNT(p).tag_over++; }

bool lagfx_a64_exec_begin(lagfx_protocol_t *p, lagfx_task_entry_t *task, const uint8_t *res, uint32_t nres) {
    if (!p || !lagfx_a64_exec_enabled(p)) return false;
    a64_run_t *r = run_get(p, true);
    if (!r) return false;
    CNT(p).execs++;
    r->vk = get_vk(p);
    r->x = NULL, r->cap = NULL, r->cap_pass = false;
    r->pass_open = r->rendering = r->wrote_storage = false, r->rt = NULL, r->pipe = NULL, r->pipe_set = false, r->pipe_failed = false;
    memset(r->rts, 0, sizeof r->rts), r->nslots = 0;
    if (!r->vk) { CNT(p).no_vulkan++; return false; }
    flush_deferred(r);   /* an earlier submission may have finished */
    r->task = task;
    memset(r->vb, 0, sizeof r->vb);
    memset(&r->region, 0, sizeof r->region);
    reset_frag_tables(r);
    r->last_ds = VK_NULL_HANDLE, r->bound_vk = VK_NULL_HANDLE;
    r->ngc = 0, r->gc_bytes = 0, r->gc_budget = A64_GATHER_BUDGET, r->gc_cap = A64_GATHER_CACHE;
    { const char *e = getenv("LAGFX_A64_GATHER_BUDGET"); if (e && *e) { char *end; unsigned long long v = strtoull(e, &end, 10); if (!*end && v <= A64_GATHER_BUDGET) r->gc_budget = v; } }   /* test / tuning hook: may only LOWER the budget */
    { const char *e = getenv("LAGFX_A64_GATHER_CACHE"); if (e && *e) { char *end; unsigned long v = strtoul(e, &end, 10); if (!*end && v <= A64_GATHER_CACHE) r->gc_cap = (uint32_t)v; } }   /* same, for the copy count */
    r->exec_draw = 0;
    if (!task || (nres && !res)) return false;
    /* ONE exec context + ONE command buffer for Render and Blit segments; the resource-flag rules (0x01 / 0x02 uploads, stale guard) are applied inside */
    r->x = lagfx_a64_res_begin(p, task->id, res, nres);
    if (!r->x) {
        if (lagfx_a64_res_is_pending(p)) {   /* NOT reachable through the compute drain (it holds exec commands in the ring while one is in flight, see
                                              * channel_compute_dispatcher.c); only a direct caller or a FAULTed GPU gets here: this exec is not executed, so it must not claim success */
            CNT(p).busy_skipped++;
            lagfx_arm64_submit_mark_failed(lagfx_arm64_submit_current(p));
        } else CNT(p).no_vulkan++;
        return false;
    }
    r->cap = lagfx_a64_cap_begin(p, r->x, task->id, 0);   /* NULL unless LAGFX_ARM64_DUMP_DATA=1 (+ dump dir) */
    /* the resource list's pipeline / sampler objects (serialized, type 7) are resolved here, as before: the mirror only owns buffers and textures */
    for (uint32_t i = 0; i < nres; i++) {
        uint32_t ref = lagfx_le32(res + 8u * i), e[3];
        if (lagfx_a64_objects_read_entry(p, task, ref, e) && (e[0] & 0xffu) == LAGFX_A64_OBJ_SERIALIZED) (void)lagfx_a64_objects_get(r->objs, p, task, ref, NULL);
    }
    return true;
}

/* ---- pipeline / descriptor support (draw time) ---- */

static void reset_frag_tables(a64_run_t *r) {
    memset(r->fb, 0, sizeof r->fb), memset(r->ftex, 0, sizeof r->ftex), memset(r->fsamp, 0, sizeof r->fsamp), memset(r->fvar, 0, sizeof r->fvar);
    memset(r->vtex, 0, sizeof r->vtex), memset(r->vsamp, 0, sizeof r->vsamp), memset(r->vvar, 0, sizeof r->vvar);
    memset(r->fmiss, 0, sizeof r->fmiss), memset(r->vmiss, 0, sizeof r->vmiss), memset(r->fbmiss, 0, sizeof r->fbmiss), memset(r->vbmiss, 0, sizeof r->vbmiss);   /* TEXWHY_SLOT_UNBOUND = 0 */
    lagfx_a64_tile_reset(r->tile);
}

/* The guest rewrote the serialized payload under an unchanged object entry: the pipeline built from the old bytes is stale. The (task, ref) object is replaced
 * (a new generation); the old one and its VkPipelines are released through the deferred list (never under a recording or still-running command buffer). */
static a64_pipe_t *pipeline_current(a64_run_t *r, lagfx_protocol_t *p, uint32_t ref) {
    a64_pipe_t *pp = get_pipe(r, p, r->task, ref);
    uint32_t e[3];
    uint64_t h;
    if (pp && pp->src.blob_hash && lagfx_a64_objects_read_entry(p, r->task, ref, e) && lagfx_a64_payload_hash(p, r->task, e, &h) && h != pp->src.blob_hash) {
        lagfx_a64_objects_invalidate(r->objs, r->task->id, ref);
        p->diag2.exec.pipe.pipe_replaced++;
        pp = get_pipe(r, p, r->task, ref);
    }
    return pp;
}

/* Legacy backend: the colour slots that are framebuffer-fetch slots in the CURRENT pass (1x target carrying INPUT_ATTACHMENT usage). Part of the pipeline cache key, because a legacy
 * pipeline is built against a render pass with exactly this input-attachment set. 0 on the dynamic backend (no render pass object). */
static uint32_t run_fetch_mask(const a64_run_t *r) {
    uint32_t m = 0;
    if (lagfx_vk_render_backend(r->vk) != LAGFX_RENDER_LEGACY) return 0;
    for (uint32_t i = 0; i < 4u; i++) if (r->rts[i] && lagfx_a64_res_target_fetch(r->x, r->rts[i])) m |= 1u << i;
    return m;
}

/* pl == VK_NULL_HANDLE records a failed build (looked up like a hit, returns NULL). */
static void cache_put(a64_pipe_t *pp, const VkFormat fmts[4], const VkFormat dfm[2], uint32_t topo, VkPipeline pl, a64_run_t *r) {
    if (pp->ncache == A64_PIPE_CACHE) { defer(r, 1, NULL, pp->cache[0].pl); memmove(&pp->cache[0], &pp->cache[1], (A64_PIPE_CACHE - 1u) * sizeof pp->cache[0]); pp->ncache--; }
    pp->cache[pp->ncache].pl = pl, memcpy(pp->cache[pp->ncache].fmt, fmts, 4 * sizeof(VkFormat)), memcpy(pp->cache[pp->ncache].dfmt, dfm, 2 * sizeof(VkFormat)), pp->cache[pp->ncache].topo = (uint8_t)topo, pp->cache[pp->ncache].samples = (uint8_t)(r->ms.samples > 1u ? r->ms.samples : 1u), pp->cache[pp->ncache].fmask = (uint8_t)run_fetch_mask(r), pp->cache[pp->ncache].lkey = pp->src.layout_key;
    pp->ncache++;
}

/* Blending needs the attachment format's COLOR_ATTACHMENT_BLEND feature (VUID-vkCmdDraw-blendEnable-04727: blendEnable must be VK_FALSE otherwise). The decoded / default
 * blend state is switched OFF for a slot whose format cannot blend and the pipeline is built with that: counted (fmt.blend_off_format, one per slot per built pipeline), never
 * built as an invalid draw. Without independentBlend every attachment must share one state (VUID 00605), so ONE unblendable attachment switches blending off for all. */
static void blend_for_formats(a64_run_t *r, lagfx_protocol_t *p, lagfx_pipeline_desc_t *d, const VkFormat fmts[4], uint32_t nslots) {
    lagfx_fmtcap_backend_t be_store;
    const lagfx_fmtcap_backend_t *be = lagfx_fmtcap_active(r->vk->phys_device, &be_store);
    struct { bool en; uint32_t sr, dr, orr, sa, da, oa; } eff[4];
    const uint32_t n = nslots ? (nslots > 4u ? 4u : nslots) : 1u;
    const bool s0_default = !d->blend_set;   /* blend_set == 0: the builder's fixed premultiplied source-over, enabled */
    for (uint32_t i = 0; i < n; i++) {
        bool own = i >= 1u && d->slot_blend[i].set && d->independent_blend;
        if (own) eff[i].en = d->slot_blend[i].enable, eff[i].sr = d->slot_blend[i].src_rgb, eff[i].dr = d->slot_blend[i].dst_rgb, eff[i].orr = d->slot_blend[i].op_rgb,
                 eff[i].sa = d->slot_blend[i].src_a, eff[i].da = d->slot_blend[i].dst_a, eff[i].oa = d->slot_blend[i].op_a;
        else if (s0_default) eff[i].en = true, eff[i].sr = VK_BLEND_FACTOR_ONE, eff[i].dr = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, eff[i].orr = VK_BLEND_OP_ADD, eff[i].sa = VK_BLEND_FACTOR_ONE,
                 eff[i].da = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, eff[i].oa = VK_BLEND_OP_ADD;
        else eff[i].en = d->blend_enable != 0, eff[i].sr = d->blend_src_rgb, eff[i].dr = d->blend_dst_rgb, eff[i].orr = d->blend_op_rgb, eff[i].sa = d->blend_src_a, eff[i].da = d->blend_dst_a, eff[i].oa = d->blend_op_a;
    }
    bool ok[4], bad = false, any_bad_blend = false;
    for (uint32_t i = 0; i < n; i++) {
        VkFormat f = (nslots <= 1u) ? (d->no_color ? VK_FORMAT_UNDEFINED : d->color_format) : fmts[i];
        ok[i] = f == VK_FORMAT_UNDEFINED || lagfx_fmtcap_blend_ok(be, f);
        if (!ok[i]) { bad = true; if (eff[i].en) any_bad_blend = true; }
    }
    if (!bad || !any_bad_blend) return;
    if (n <= 1u || !d->independent_blend) {   /* one shared state (or one attachment): off */
        for (uint32_t i = 0; i < n; i++) if (!ok[i] && eff[i].en) p->diag2.exec.fmt.blend_off_format++;
        d->blend_set = 1, d->blend_enable = 0, d->blend_src_rgb = d->blend_src_a = VK_BLEND_FACTOR_ONE, d->blend_dst_rgb = d->blend_dst_a = VK_BLEND_FACTOR_ZERO, d->blend_op_rgb = d->blend_op_a = VK_BLEND_OP_ADD;
        if (s0_default) d->color_write_mask = 0xfu;   /* the fixed state wrote every channel; a decoded mask (0 = write nothing) stays as it is */
        for (uint32_t i = 1; i < 4u; i++) d->slot_blend[i].set = 0;
        return;
    }
    /* independent blend: slot 0 through the descriptor's fixed fields, the others through slot_blend[] (materialised for every attachment so a slot that copied attachment 0 keeps its own state) */
    for (uint32_t i = 0; i < n; i++) {
        const bool en = eff[i].en && ok[i];
        if (eff[i].en && !ok[i]) p->diag2.exec.fmt.blend_off_format++;
        if (i == 0u) {
            if (s0_default) d->color_write_mask = 0xfu;
            d->blend_set = 1, d->blend_enable = en, d->blend_src_rgb = eff[0].sr, d->blend_dst_rgb = eff[0].dr, d->blend_op_rgb = eff[0].orr, d->blend_src_a = eff[0].sa, d->blend_dst_a = eff[0].da, d->blend_op_a = eff[0].oa;
        } else {
            d->slot_blend[i].set = 1, d->slot_blend[i].enable = en, d->slot_blend[i].src_rgb = eff[i].sr, d->slot_blend[i].dst_rgb = eff[i].dr, d->slot_blend[i].op_rgb = eff[i].orr,
            d->slot_blend[i].src_a = eff[i].sa, d->slot_blend[i].dst_a = eff[i].da, d->slot_blend[i].op_a = eff[i].oa;
        }
    }
}

/* The VkPipeline for (pass colour format, topology): built lazily, cached on the pipeline object (which is per generation). */
static VkPipeline pipeline_for(a64_run_t *r, lagfx_protocol_t *p, a64_pipe_t *pp, const VkFormat fmts[4], uint32_t nslots, uint32_t topo) {
    const VkFormat dfm[2] = { r->ds_dfmt, r->ds_sfmt };
    const uint32_t key = topo; topo &= 15u;   /* bits 4..6 of the key: 1 + index of the unnormalized-sampler shader variant of the pipeline (0 = the shaders as resolved) */
    for (uint32_t i = 0; i < pp->ncache; i++)
        if (!memcmp(pp->cache[i].fmt, fmts, 4 * sizeof(VkFormat)) && !memcmp(pp->cache[i].dfmt, dfm, sizeof dfm) && pp->cache[i].topo == key && pp->cache[i].samples == (r->ms.samples > 1u ? r->ms.samples : 1u) && pp->cache[i].fmask == run_fetch_mask(r) && pp->cache[i].lkey == pp->src.layout_key) return pp->cache[i].pl;
    lagfx_pipeline_desc_t d = pp->src.desc;
    if (key >> 4) {
        const uint32_t vi = (key >> 4) - 1u;
        if (vi < pp->nfv) {
            if (pp->fv[vi].vm) d.vertex_shader = pp->fv[vi].vm;
            if (pp->fv[vi].fm) d.fragment_shader = pp->fv[vi].fm;
            if (pp->fv[vi].ms) d.sample_shading = 1u, d.min_sample_shading_permille = 1000u;   /* the fetch reads SampleId: one invocation per sample */
        }
    }
    d.color_format = fmts[0] != VK_FORMAT_UNDEFINED ? fmts[0] : VK_FORMAT_B8G8R8A8_UNORM;
    if (nslots == 1u && fmts[0] == VK_FORMAT_UNDEFINED) d.no_color = 1;   /* a depth/stencil-only pass: the one colour slot is unused */
    if (vk_core13(r->vk)) {   /* cull / front face / depth bias and the depth / stencil state are encoder state, set dynamically before each draw (see apply_encoder_state) */
        d.dyn_state = 1, d.ds_depth_format = dfm[0], d.ds_stencil_format = dfm[1];
    }
    if (nslots > 1u) {   /* several colour attachments (or a gap before the first): one format per slot, write mask per slot from the descriptor */
        d.n_color = (uint8_t)nslots;
        d.independent_blend = r->vk->have_independent_blend;
        for (uint32_t i = 0; i < 4u; i++) d.color_formats[i] = (uint32_t)fmts[i];
        if (!r->vk->have_independent_blend) {   /* the device cannot give attachments different blend/mask: say so instead of silently forcing attachment 0's state */
            bool diff = false;
            for (uint32_t i = 1; i < nslots && i < 4u; i++) {
                if (fmts[i] == VK_FORMAT_UNDEFINED) continue;
                if (d.masks_valid && d.color_write_masks[i] != d.color_write_masks[0]) diff = true;
                if (d.slot_blend[i].set && (d.slot_blend[i].enable != d.blend_enable || (d.slot_blend[i].enable && (d.slot_blend[i].src_rgb != d.blend_src_rgb || d.slot_blend[i].dst_rgb != d.blend_dst_rgb || d.slot_blend[i].op_rgb != d.blend_op_rgb || d.slot_blend[i].src_a != d.blend_src_a || d.slot_blend[i].dst_a != d.blend_dst_a || d.slot_blend[i].op_a != d.blend_op_a)))) diff = true;
            }
            if (diff) p->diag2.exec.run.blend_indep_mismatch++;
        }
    }
    blend_for_formats(r, p, &d, fmts, nslots);
    if (lagfx_vk_raster_order(r->vk)) for (uint32_t i = 0; i < pp->src.n_bind; i++) if (pp->src.bind[i].kind == LAGFX_A64_BIND_INPUT) d.raster_order_access = 1;
    d.topology_plus1 = (uint8_t)(topo + 1u);
    d.raster_samples = (uint8_t)(r->ms.samples > 1u ? r->ms.samples : 1u);   /* must equal the attachments' count; the pipeline descriptor's own count field is not decoded [N]: taken from the pass (Metal requires them equal) */
    if (lagfx_msaa_inject) {   /* UNWIRED test / injection hook (msaa_host.h): the pipeline descriptor's alpha-to-coverage and sample-shading bits are not decoded [N] */
        d.alpha_to_coverage = lagfx_msaa_inject->alpha_to_coverage ? 1u : 0u;
        if (lagfx_msaa_inject->sample_shading) {
            if (!r->vk->have_sample_rate_shading) {   /* exact: refused and cached as a failed build, never built without per-sample shading */
                p->diag2.exec.msaa.sample_shading_refused++;
                cache_put(pp, fmts, dfm, key, VK_NULL_HANDLE, r);
                return VK_NULL_HANDLE;
            }
            d.sample_shading = 1u, d.min_sample_shading_permille = lagfx_msaa_inject->min_sample_shading_permille;
        }
    }
    if (lagfx_vk_render_backend(r->vk) == LAGFX_RENDER_LEGACY) {   /* a VkRenderPass device: the pipeline is built against the pass render_begin will start (render_encoder.c begin_legacy), or not at all */
        lagfx_render_desc_t rd; VkRenderPass lrp = VK_NULL_HANDLE; lagfx_legacy_refuse_t why = LAGFX_LR_NONE;
        if (lagfx_pipeline_render_desc_why(&d, &rd, &why)) { rd.fetch_mask = lagfx_legacy_fetch_mask(&rd) & run_fetch_mask(r);
            { uint32_t need = 0; for (uint32_t bi = 0; bi < pp->src.n_bind; bi++) if (pp->src.bind[bi].kind == LAGFX_A64_BIND_INPUT && pp->src.bind[bi].metal_index >= 0 && pp->src.bind[bi].metal_index < 4) need |= 1u << pp->src.bind[bi].metal_index;
              if (need & ~rd.fetch_mask) {   /* the shader reads an input attachment of a slot that is not a fetch slot of this pass (no INPUT_ATTACHMENT usage / MSAA): the pipeline would be invalid against that render pass (VUID-VkGraphicsPipelineCreateInfo-renderPass-06038) */
                  p->diag2.exec.pipe.desc_unbound++, p->diag2.exec.pipe.desc_unbound_why[1]++; cache_put(pp, fmts, dfm, key, VK_NULL_HANDLE, r); return VK_NULL_HANDLE; } } lrp = lagfx_vk_render_pass_compat(r->vk, &rd); if (lrp == VK_NULL_HANDLE) why = LAGFX_LR_PASS; }
        if (lrp == VK_NULL_HANDLE) { p->diag2.exec.pipe.legacy_shape_refused++, p->diag2.exec.pipe.legacy_pipe_why[why]++; cache_put(pp, fmts, dfm, key, VK_NULL_HANDLE, r); return VK_NULL_HANDLE; }
        d.render_pass = (uint64_t)(uintptr_t)lrp;
    }
    VkPipeline pl = VK_NULL_HANDLE;
    const uint64_t hold_t0 = lagfx_hold_now_ns();
    const bool built = lagfx_pipeline_build(r->vk->device, &d, &pl) == LAGFX_OK;
    lagfx_hold_note(built ? LAGFX_HOLD_PIPE_BUILD : LAGFX_HOLD_PIPE_FAIL, hold_t0, 0);
    if (!built) {
        p->diag2.exec.pipe.pipe_build_failed++;
        lagfx_a64_dump_spirv(p, d.vertex_spv, d.vertex_spv_bytes, d.fragment_spv, d.fragment_spv_bytes);   /* LAGFX_ARM64_DUMP=1 + dir: the failing modules, for offline spirv-val */
        cache_put(pp, fmts, dfm, key, VK_NULL_HANDLE, r);   /* failure cache: the same (pipeline, formats, topology) is not rebuilt (and re-logged) on every draw */
        return VK_NULL_HANDLE;
    }
    p->diag2.exec.pipe.pipes_built++;
    if (r->ms.samples > 1u) p->diag2.exec.msaa.pipes_ms++;
    if (d.alpha_to_coverage) p->diag2.exec.msaa.a2c_pipes++;
    if (d.sample_shading) p->diag2.exec.msaa.sample_shading_pipes++;
    if (topo < 8u) p->diag2.exec.pipe.topo[topo]++;
    cache_put(pp, fmts, dfm, key, pl, r);
    return pl;
}

static VkSampler default_sampler(a64_run_t *r) {
    if (!r->defsamp) {
        VkSamplerCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
        if (vkCreateSampler(r->vk->device, &ci, NULL, &r->defsamp) != VK_SUCCESS) r->defsamp = VK_NULL_HANDLE;
    }
    return r->defsamp;
}

/* The VkSampler of a constexpr sampler (its raw AIR word, named in the shader). Always a normalized sampler: the translator divides the coordinates of a pixel
 * sampler by the image size in the shader. VK_NULL_HANDLE (counted by the caller as an unbound sampler) when the cache is full or creation failed. */
static lagfx_cubic_why_t cubic_stage_why(a64_run_t *r, const a64_pipe_t *pp, uint32_t st, bool combo_ok);
static void cubic_count(a64_run_t *r, lagfx_cubic_why_t w);
static VkSampler cxs_sampler(a64_run_t *r, const a64_pipe_t *pp, uint32_t stage, uint64_t raw) {
    lagfx_constexpr_sampler_t cs;
    if (!lagfx_constexpr_sampler_decode(raw, &cs)) return VK_NULL_HANDLE;   /* the translator only names decodable words: a corrupt module */
    if (r->vk->sampler_cache.device == VK_NULL_HANDLE) lagfx_sampler_cache_init(&r->vk->sampler_cache, r->vk->device, r->vk->have_sampler_anisotropy);
    lagfx_sampler_state_t st = lagfx_sampler_state_from_constexpr_norm(&cs);
    if (pp && lagfx_cubic_optin() && (cs.mag_filter == 2u || cs.min_filter == 2u)) {   /* the would-be bicubic code: the opt-in path (the default stays LINEAR, counted by the cache) */
        lagfx_cubic_why_t w = cubic_stage_why(r, pp, stage, st.max_anisotropy <= 1.0f && !st.unnormalized);
        cubic_count(r, w);
        if (w == LAGFX_CUBIC_USE) {
            if (cs.mag_filter == 2u) st.mag_filter = LAGFX_FILTER_CUBIC;
            if (cs.min_filter == 2u) st.min_filter = LAGFX_FILTER_CUBIC;
        }
    }
    return lagfx_sampler_cache_get(&r->vk->sampler_cache, &st);
}

/* ---- opt-in cubic filtering (LAGFX_CUBIC=1, filter_cubic.h) ---- */

/* The decision for one stage: every sampled texture of the stage must be cubic-capable (the pairing of image and sampler is the shader's business). `combo_ok` = the sampler is
 * normalized and has no anisotropy. The first failing reason is returned; LAGFX_CUBIC_USE when the stage has no sampled texture at all. */
static lagfx_cubic_why_t cubic_stage_why(a64_run_t *r, const a64_pipe_t *pp, uint32_t st, bool combo_ok) {
    lagfx_cubic_ctx_t cx = { .optin = lagfx_cubic_optin(), .ext_enabled = r->vk->have_filter_cubic };
    lagfx_fmtcap_backend_t store;
    const lagfx_fmtcap_backend_t *be = lagfx_fmtcap_active(r->vk->phys_device, &store);
    lagfx_cubic_why_t w = lagfx_cubic_decide(&cx, LAGFX_FMT_FEATURE_FILTER_CUBIC, true, combo_ok);   /* the stage-independent part first */
    if (w != LAGFX_CUBIC_USE) return w;
    for (uint32_t i = 0; i < pp->src.n_bind; i++) {
        const lagfx_a64_bind_t *b = &pp->src.bind[i];
        if (b->stage != st || b->kind != LAGFX_A64_BIND_IMAGE || b->metal_index < 0 || (uint32_t)b->metal_index >= A64_MAX_FB) continue;
        const lagfx_a64_obj_t *t = st ? r->ftex[b->metal_index] : r->vtex[b->metal_index];
        if (!t) continue;   /* an unbound texture fails the draw on its own */
        w = lagfx_cubic_decide(&cx, lagfx_fmtcap_features(be, t->format), !b->dim3, combo_ok);
        if (w != LAGFX_CUBIC_USE) return w;
    }
    return LAGFX_CUBIC_USE;
}

/* The cubic twin of `vs` (sm->s or one of its LOD variants): same state, VK_FILTER_CUBIC_EXT for the filter(s) whose guest field was 2. NULL = not possible (variant table full / unknown base). */
static VkSampler cubic_twin(a64_run_t *r, a64_samp_t *sm, VkSampler vs) {
    for (uint32_t i = 0; i < sm->ncub; i++) if (sm->cub[i].base == vs) return sm->cub[i].s;
    if (sm->ncub >= A64_SAMP_VARIANTS + 1u) return VK_NULL_HANDLE;
    VkSamplerCreateInfo ci = sm->ci;
    if (vs != sm->s) {
        uint32_t k = 0;
        while (k < sm->nvar && sm->var[k].s != vs) k++;
        if (k == sm->nvar) return VK_NULL_HANDLE;
        ci.minLod = sm->var[k].lo, ci.maxLod = sm->var[k].hi;
    }
    lagfx_cubic_apply(&ci, (sm->cflags & LAGFX_A64_SAMP_CUBIC_MIN) != 0, (sm->cflags & LAGFX_A64_SAMP_CUBIC_MAG) != 0);
    VkSampler c = VK_NULL_HANDLE;
    if (vkCreateSampler(r->vk->device, &ci, NULL, &c) != VK_SUCCESS) return VK_NULL_HANDLE;
    sm->cub[sm->ncub].base = vs, sm->cub[sm->ncub].s = c, sm->ncub++;
    return c;
}

static void cubic_count(a64_run_t *r, lagfx_cubic_why_t w) {
    CNT(r->p).ext_cubic_requested++;
    if (w == LAGFX_CUBIC_USE) CNT(r->p).ext_cubic_used++; else CNT(r->p).ext_cubic_fallback[w]++;
}

static bool descriptor_pool(a64_run_t *r) {
    if (r->dpool) return true;
    VkDescriptorPoolSize sz[5] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096 }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2048 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 2048 }, { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1024 },
                                   { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024 } };
    VkDescriptorPoolCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, .maxSets = 512, .poolSizeCount = 5, .pPoolSizes = sz };
    if (vkCreateDescriptorPool(r->vk->device, &ci, NULL, &r->dpool) != VK_SUCCESS) { r->dpool = VK_NULL_HANDLE; return false; }
    VkPhysicalDeviceProperties pr;
    vkGetPhysicalDeviceProperties(r->vk->phys_device, &pr);
    r->min_sb_align = pr.limits.minStorageBufferOffsetAlignment ? pr.limits.minStorageBufferOffsetAlignment : 1;
    r->max_sb_range = pr.limits.maxStorageBufferRange;
    return true;
}

static void tile_suspend(void *c);
static bool tile_resume(void *c);
/* A storage-buffer range that starts at an offset the device cannot bind (not a multiple of minStorageBufferOffsetAlignment): the bytes [off, off + range) of the
 * mirror buffer are copied (host side, at record time) into an exec-owned scratch buffer and that is bound at offset 0. Reference: the independent pvg-vulkan
 * runtime does the same ("gather") for guest binds that are not zero-copyable. Limits: the copy is a snapshot taken at this draw: GPU writes recorded earlier in the
 * same exec (blit, texture copy, shader / tile writes) are included (the copy is then made by the GPU), writes by the draw's own shaders are not written back, and ranges above A64_GATHER_MAX are refused.
 * One copy per (buffer generation, write generation wgen, offset) and exec: a write the mirror recorded since the copy (wgen moved) makes the copy stale, so it is not reused; a later bind whose range lies inside an earlier copy at an alignment-multiple distance reuses it (*doff). NULL = not possible (the draw is skipped, counted as desc_unaligned). */
/* Records the copy of b[off, off + len) into dst on the GPU, after every GPU write recorded so far in this exec. A copy cannot be recorded inside a dynamic-rendering instance:
 * the open pass is ended and begun again (LOAD ops, the way a tile dispatch does it). 1 = done, 0 = not possible here (multisampled pass; nothing recorded), -1 = recorded but the pass
 * could not be resumed (the draw must be skipped). */
static int gather_gpu(a64_run_t *r, lagfx_protocol_t *p, lagfx_a64_obj_t *b, uint64_t off, uint64_t len, VkBuffer dst) {
    const bool was = r->rendering;
    if (was && r->ms.samples > 1u) return 0;
    if (was) tile_suspend(r);
    bool ok = lagfx_a64_res_gather_record(r->x, b, off, len, dst);
    if (was && !tile_resume(r)) { CNT(p).ext_gather_gpu_lost++; return -1; }
    return ok ? 1 : 0;
}

static VkBuffer gather_copy(a64_run_t *r, lagfx_protocol_t *p, lagfx_a64_obj_t *b, uint64_t off, uint64_t range, VkDeviceSize *doff) {
    *doff = 0;
    if (!b->map) { CNT(p).ext_gather_failed++, CNT(p).ext_gather_why[0]++; return VK_NULL_HANDLE; }
    if (range > A64_GATHER_MAX) { CNT(p).ext_gather_failed++, CNT(p).ext_gather_why[1]++; return VK_NULL_HANDLE; }   /* range = the whole rest of the buffer, not what the shader reads */
    /* A GPU write to b recorded earlier in this exec has not run yet: the host mapping does not hold it, so the bytes are taken by a GPU copy ordered after it. */
    const bool pend = lagfx_a64_res_gpu_pending(r->x, b);
    /* An existing copy of the same buffer generation that starts at c->off <= off and covers [off, off + range) serves this bind too when the distance is a multiple of
     * the device alignment: bind it at that offset (the same bytes, the same snapshot, the same descriptor range). Equal (off, range) is the distance 0 case. A copy older than the
     * buffer's last write (wgen) is refreshed in place by the GPU while that write is still pending (the earlier draws that read it run before the refresh), otherwise not reused. */
    for (uint32_t i = 0; i < r->ngc; i++) {
        __typeof__(r->gc[0]) *c = &r->gc[i];
        if (c->gen != b->gen || c->off > off || off + range > c->off + c->range || (off - c->off) % r->min_sb_align) continue;
        if (c->wgen != b->wgen) {
            if (!pend) continue;
            int g = gather_gpu(r, p, b, c->off, c->range, c->buf);
            if (g < 0) { CNT(p).ext_gather_failed++; return VK_NULL_HANDLE; }
            if (!g) continue;
            c->wgen = b->wgen;
            CNT(p).ext_gather_gpu_refreshed++;
        }
        *doff = off - c->off;
        if (*doff) CNT(p).ext_gather_reused++;
        return c->buf;
    }
    VkBuffer sb; uint8_t *mp;
    if (r->ngc >= r->gc_cap) { CNT(p).ext_gather_failed++, CNT(p).ext_gather_why[2]++; return VK_NULL_HANDLE; }   /* A64_GATHER_CACHE distinct copies per exec */
    if (range > r->gc_budget - r->gc_bytes) { CNT(p).ext_gather_failed++, CNT(p).ext_gather_why[4]++; return VK_NULL_HANDLE; }   /* total bytes of this exec's copies would exceed A64_GATHER_BUDGET */
    if (!lagfx_a64_res_scratch_xfer(r->x, range, &sb, &mp)) { CNT(p).ext_gather_failed++, CNT(p).ext_gather_why[3]++; return VK_NULL_HANDLE; }   /* scratch slots (64 per exec) used up or the allocation failed */
    int g = pend ? gather_gpu(r, p, b, off, range, sb) : 0;
    if (g < 0) { CNT(p).ext_gather_failed++; return VK_NULL_HANDLE; }   /* the slot stays allocated (freed with the exec) */
    r->gc_bytes += range;
    if (g) CNT(p).ext_gather_gpu++;
    else {
        memcpy(mp, b->map + off, (size_t)range);
        if (pend) {   /* a write is pending but the GPU copy was not possible (multisampled pass): these host bytes may be older than the buffer. They serve THIS draw only; registering them
                       * under the current wgen would let a later draw (e.g. in a 1x pass, where the GPU copy works) take them as fresh. */
            CNT(p).ext_gather_host_stale++, CNT(p).ext_gathered++, CNT(p).ext_gather_bytes += range;
            return sb;
        }
    }
    r->gc[r->ngc].gen = b->gen, r->gc[r->ngc].wgen = b->wgen, r->gc[r->ngc].off = off, r->gc[r->ngc].range = range, r->gc[r->ngc].buf = sb, r->ngc++;
    CNT(p).ext_gathered++, CNT(p).ext_gather_bytes += range;
    return sb;
}

/* A sampled texture slot is empty at a draw: count it by reason (desc_tex_why) and log the first 8 distinct textures of each reason, so a device log names them.
 * Distinct = (task, ref, entry type, format, extent). The reason was fixed when the texture record named the slot (a64_texmiss_t), or is slot_unbound / slot_range. */
static void tex_miss(a64_run_t *r, lagfx_protocol_t *p, const a64_pipe_t *pp, uint32_t bind_i, uint32_t stage, int16_t mi_s) {
    a64_texmiss_t none; memset(&none, 0, sizeof none);
    const a64_texmiss_t *mm = &none;
    if (mi_s < 0 || (uint32_t)mi_s >= A64_MAX_FB) none.dg.why = TEXWHY_SLOT_RANGE;
    else mm = &(stage ? r->fmiss : r->vmiss)[mi_s];
    unsigned why = (unsigned)mm->dg.why < LAGFX_A64_TEXWHY_COUNT ? (unsigned)mm->dg.why : 0u;
    r->ddm.bkind = 1, r->ddm.stage = (uint8_t)stage, r->ddm.slot = mi_s, r->ddm.vkb = pp->src.bind[bind_i].binding, r->ddm.ref = mm->ref;
    __typeof__(p->diag2.exec.pipe) *Q = &p->diag2.exec.pipe;
    Q->desc_tex_why[why]++;
    for (unsigned k = 0; k < Q->desc_tex_nseen[why]; k++) {
        __typeof__(Q->desc_tex_seen[0][0]) *e = &Q->desc_tex_seen[why][k];
        if (e->task == r->task->id && e->ref == mm->ref && e->type == mm->dg.type && e->mtl_format == mm->dg.mtl_format && e->w == mm->dg.width && e->h == mm->dg.height) return;
    }
    if (Q->desc_tex_nseen[why] >= 8u) return;
    __typeof__(Q->desc_tex_seen[0][0]) *e = &Q->desc_tex_seen[why][Q->desc_tex_nseen[why]++];
    e->task = r->task->id, e->ref = mm->ref, e->type = mm->dg.type, e->mtl_format = mm->dg.mtl_format, e->w = mm->dg.width, e->h = mm->dg.height, e->mapping = mm->dg.mapping;
    static const char *const kind[] = { "?", "buffer", "texture", "?", "?", "?", "function", "serialized", "linear_texture", "linear_texture", "?", "mapped_texture" };
    /* draw context (facts at the failure, no interpretation): the exec's draw index, pass number, the pipeline object and its shader pair, which Vulkan binding / Metal slot,
     * and colour attachment 0. Whether the shader really samples the slot is not known here: the binding exists because the translated SPIR-V declares the texture. */
    char rt[96] = "none";
    if (r->rts[0]) snprintf(rt, sizeof rt, "ref=%u mapping=%u mtl_format=%u size=%ux%u", (unsigned)r->rts[0]->ref, (unsigned)(r->rts[0]->ms ? r->rts[0]->mapping : 0u), (unsigned)r->rts[0]->mtl_format,
                            (unsigned)r->rts[0]->width, (unsigned)r->rts[0]->height);
    char l[640];
    snprintf(l, sizeof l, "g7-gpu-tex-unbound: why=%s task=%u ref=%u stage=%s slot=%d entry_type=%u kind=%s mtl_format=%u size=%ux%u mapping=%u (distinct %u/8) | draw=%u pass=%llu pipeline_ref=%u vs=%s/%016llx fs=%s/%016llx vk_binding=%u declared_in_shader=yes sampled=unknown rt0[%s]",
             lagfx_a64_texwhy_name(why), (unsigned)r->task->id,
             (unsigned)mm->ref, stage ? "fragment" : "vertex", (int)mi_s, (unsigned)mm->dg.type, mm->dg.type < sizeof kind / sizeof *kind ? kind[mm->dg.type] : "?", (unsigned)mm->dg.mtl_format,
             (unsigned)mm->dg.width, (unsigned)mm->dg.height, (unsigned)mm->dg.mapping, (unsigned)Q->desc_tex_nseen[why], (unsigned)r->exec_draw, (unsigned long long)CNT(p).passes, (unsigned)pp->ref,
             pp->src.vs_name[0] ? pp->src.vs_name : "?", (unsigned long long)pp->src.vs_hash, pp->src.fs_name[0] ? pp->src.fs_name : "?", (unsigned long long)pp->src.fs_hash, (unsigned)pp->src.bind[bind_i].binding, rt);
    fprintf(stderr, "%s\n", l);
    LAGFX_LOG("%s", l);
}

/* A buffer slot of a draw holds no usable buffer: count it by reason (desc_buf_why) and log the first 8 distinct (stage, slot, ref, reason). Facts at the failure only. */
static void buf_miss(a64_run_t *r, lagfx_protocol_t *p, const a64_pipe_t *pp, uint32_t bind_i, const lagfx_a64_obj_t *b, uint64_t off) {
    const lagfx_a64_pipeline_src_t *s = &pp->src;
    int16_t mi_s = s->bind[bind_i].metal_index; uint32_t st = s->bind[bind_i].stage, mi = (uint32_t)mi_s;
    uint32_t why, ref = 0, type = 0;
    if (mi_s < 0) why = 0;                                                   /* the shader's binding has no Metal slot */
    else if (mi >= (st ? A64_MAX_FB : A64_MAX_VB)) why = 1;                  /* slot beyond the table */
    else {
        const __typeof__(r->vbmiss[0]) *m = st ? &r->fbmiss[mi] : &r->vbmiss[mi];
        ref = m->ref, type = m->type;
        why = b ? 6u : m->why == 0 ? 2u : m->why == 1 ? 3u : 4u;             /* 6: bound, but the offset is past the buffer's end */
        if (why == 6u) type = LAGFX_A64_OBJ_BUFFER;
    }
    r->ddm.bkind = 2, r->ddm.stage = (uint8_t)st, r->ddm.slot = mi_s, r->ddm.vkb = s->bind[bind_i].binding, r->ddm.ref = ref;
    __typeof__(p->diag2.exec.pipe) *Q = &p->diag2.exec.pipe;
    Q->desc_buf_why[why]++;
    for (unsigned k = 0; k < Q->desc_buf_nseen[why]; k++) if (Q->desc_buf_seen[why][k].ref == ref && Q->desc_buf_seen[why][k].slot == mi && Q->desc_buf_seen[why][k].stage == st) return;
    if (Q->desc_buf_nseen[why] >= 8u) return;
    __typeof__(Q->desc_buf_seen[0][0]) *e = &Q->desc_buf_seen[why][Q->desc_buf_nseen[why]++];
    e->ref = ref, e->slot = mi, e->stage = st;
    static const char *const nm[] = { "no_metal_slot", "slot_range", "slot_never_bound", "ref_unresolved", "not_a_buffer", "?", "offset_past_end" };
    char l[420];
    snprintf(l, sizeof l, "g7-gpu-buf-unbound: why=%s task=%u stage=%s slot=%d ref=%u entry_type=%u offset=%llu length=%llu | draw=%u pass=%llu pipeline_ref=%u vs=%s fs=%s vk_binding=%u writable=%d",
             nm[why], (unsigned)r->task->id, st ? "fragment" : "vertex", (int)mi_s, (unsigned)ref, (unsigned)type, (unsigned long long)off, (unsigned long long)(b ? b->length : 0),
             (unsigned)r->exec_draw, (unsigned long long)CNT(p).passes, (unsigned)pp->ref, s->vs_name[0] ? s->vs_name : "?", s->fs_name[0] ? s->fs_name : "?", (unsigned)s->bind[bind_i].binding, (int)s->bind[bind_i].writable);
    fprintf(stderr, "%s\n", l);
    LAGFX_LOG("%s", l);
}

/* Fills and binds the set-0 descriptor set of the pipeline from the Metal tables recorded by tags 125/126/110/111/114/112. Anything unbound / misaligned /
 * a feedback loop skips the draw (counted): Metal leaves those undefined. The key (pipeline gen, then per binding the object gen and offset) lets consecutive
 * draws share one set; a replaced object has a new gen, so a stale set is never reused. Mirror textures stay in GENERAL layout (valid for sampling), so no
 * layout change is recorded inside the pass. A set is parked in the deferred list and freed only once no recorded / running work can use it. */
static uint32_t unnorm_mask(const a64_run_t *r, const a64_pipe_t *pp, uint32_t stage);
static bool bind_descriptors(a64_run_t *r, lagfx_protocol_t *p, a64_pipe_t *pp) {
    const lagfx_a64_pipeline_src_t *s = &pp->src;
    if (!s->n_bind) return true;
    if (!descriptor_pool(r)) { p->diag2.exec.pipe.desc_failed++; return false; }
    VkDescriptorBufferInfo bi[LAGFX_A64_MAX_BINDINGS];
    VkDescriptorImageInfo ii[LAGFX_A64_MAX_BINDINGS];
    VkWriteDescriptorSet w[LAGFX_A64_MAX_BINDINGS];
    lagfx_a64_obj_t *wl[LAGFX_A64_MAX_BINDINGS]; uint32_t nwl = 0;   /* shader-writable buffers of this draw: marked written only when the draw is going to be recorded */
    uint64_t key = 1469598103934665603ull ^ pp->h.gen;
    /* A draw with an unnormalized-coordinates sampler in a stage: Vulkan only lets such a sampler read a view of ONE level and ONE layer, and which image a sampler is paired with is the
     * shader's business (known only to the SPIR-V rewrite), so every plain 2D sampled texture of that stage gets its level 0 / layer 0 view (a texture with more is counted as dropped). */
    const bool unn[2] = { unnorm_mask(r, pp, 0) != 0, unnorm_mask(r, pp, 1) != 0 };
#define DDM(k) (r->ddm.bkind = (k), r->ddm.stage = (uint8_t)st, r->ddm.slot = mi_s, r->ddm.vkb = s->bind[i].binding, r->ddm.ref = 0)
#define MIX(v) key = (key ^ (uint64_t)(v)) * 1099511628211ull
    for (uint32_t i = 0; i < s->n_bind; i++) {
        int16_t mi_s = s->bind[i].metal_index;
        uint32_t mi = (uint32_t)mi_s, st = s->bind[i].stage;
        memset(&w[i], 0, sizeof w[i]);
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, w[i].dstBinding = s->bind[i].binding, w[i].descriptorCount = 1;
        MIX(s->bind[i].binding);
        if (s->bind[i].kind == LAGFX_A64_BIND_BUFFER) {
            lagfx_a64_obj_t *b = mi_s < 0 ? NULL : st ? (mi < A64_MAX_FB ? r->fb[mi].b : NULL) : (mi < A64_MAX_VB ? r->vb[mi].b : NULL);
            uint64_t off = st ? (mi < A64_MAX_FB ? r->fb[mi].off : 0) : (mi < A64_MAX_VB ? r->vb[mi].off : 0);
            if (!b || off >= b->length) { p->diag2.exec.pipe.desc_unbound++, p->diag2.exec.pipe.desc_unbound_why[0]++; buf_miss(r, p, pp, i, b, off); return false; }
            VkBuffer dbuf = b->buf; VkDeviceSize doff = off, range = b->length - off;
            if (s->bind[i].writable) wl[nwl++] = b;   /* the shader may write it: a linear texture over it is stale from here on, and a gather of it needs the GPU (marked below) */
            if (r->max_sb_range && range > r->max_sb_range) range = r->max_sb_range, CNT(p).ext_range_clamped++;   /* the descriptor range may not exceed maxStorageBufferRange */
            if (off % r->min_sb_align && s->bind[i].writable) { CNT(p).ext_wsb_skipped++; p->diag2.exec.pipe.desc_unaligned++; return false; }   /* a gathered copy would swallow the shader's writes (and the copy is a snapshot): skip, counted, rather than diverge */
            if (off % r->min_sb_align) {   /* Metal allows any 4-aligned offset, Vulkan only multiples of minStorageBufferOffsetAlignment: bind an aligned copy of the range */
                dbuf = gather_copy(r, p, b, off, range, &doff);
                if (!dbuf) { p->diag2.exec.pipe.desc_unaligned++; return false; }
            }
            bi[i] = (VkDescriptorBufferInfo){ dbuf, doff, range };
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w[i].pBufferInfo = &bi[i];
            MIX(b->gen), MIX(off), MIX((uint64_t)(uintptr_t)dbuf), MIX(doff);
        } else if (s->bind[i].kind == LAGFX_A64_BIND_INPUT) {   /* framebuffer fetch: colour attachment mi of the open pass */
            lagfx_a64_obj_t *t = mi < 4u ? r->rts[mi] : NULL;
            if (!t || !t->view || !r->rendering) { p->diag2.exec.pipe.desc_unbound++, p->diag2.exec.pipe.desc_unbound_why[1]++; DDM(3); return false; }
            if (!(t->usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) { if (t->samples_req > 1u) p->diag2.exec.msaa.fbfetch_ms_refused++; p->diag2.exec.pipe.desc_unbound++, p->diag2.exec.pipe.desc_unbound_why[1]++; DDM(3); return false; }   /* the view's usage must include it (a descriptor of an image without it is invalid) */
            ii[i] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, t->view, lagfx_vk_fetch_layout(r->vk) /* RENDERING_LOCAL_READ (dynamic) / GENERAL (legacy) */ };
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, w[i].pImageInfo = &ii[i];
            MIX(t->gen), MIX(0x1f0u);
        } else if (s->bind[i].kind == LAGFX_A64_BIND_STORAGE_IMAGE) {   /* texture2d<T, access::write>: the mirror texture itself, GENERAL layout */
            lagfx_a64_obj_t *t = mi_s >= 0 && mi < A64_MAX_FB ? r->ftex[mi] : NULL;
            if (!t) { p->diag2.exec.pipe.desc_unbound++, p->diag2.exec.pipe.desc_unbound_why[2]++; DDM(3); return false; }
            bool fb = false;
            for (uint32_t k = 0; k < r->nslots && k < 4u; k++) fb |= t == r->rts[k];
            if (fb) { p->diag2.exec.pipe.desc_feedback++; return false; }   /* writing an attachment being rendered */
            VkImageView v = t->linear ? VK_NULL_HANDLE : lagfx_a64_res_storage_view(r->x, t);   /* linear (buffer backed) and non-storage formats cannot be written */
            if (!v) { CNT(p).ext_storage_img_unsupported++; return false; }
            ii[i] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL };
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, w[i].pImageInfo = &ii[i];
            MIX(t->gen), MIX(0x57u);
            r->wrote_storage = true;
        } else if (s->bind[i].kind == LAGFX_A64_BIND_IMAGE) {
            lagfx_a64_obj_t *t = mi_s >= 0 && mi < A64_MAX_FB ? (st ? r->ftex[mi] : r->vtex[mi]) : NULL;
            if (!t) { p->diag2.exec.pipe.desc_unbound++, p->diag2.exec.pipe.desc_unbound_why[3]++; tex_miss(r, p, pp, i, st, mi_s); return false; }
            bool fb = false;
            for (uint32_t k = 0; k < r->nslots && k < 4u; k++) fb |= t == r->rts[k];
            if (fb) { p->diag2.exec.pipe.desc_feedback++; return false; }   /* sampling an attachment being rendered */
            if (t->samples_req > 1u) { p->diag2.exec.msaa.sampled_ms_refused++; p->diag2.exec.pipe.desc_unbound++, p->diag2.exec.pipe.desc_unbound_why[4]++; DDM(1); return false; }   /* a multisampled texture read needs texture2d_ms in the translator (spelling unverified): the draw is skipped, counted */
            /* A linear texture (a buffer's bytes seen as an image): the mirror image is a copy that begin_pass (outside the pass) re-pulls whenever the backing buffer changed.
             * A change after that, inside the open pass (a shader-writable bind, a tile kernel, ...) leaves the image stale: skip rather than draw with old bytes. */
            if (t->linear && !lagfx_a64_res_linear_fresh(r->x, t)) { p->diag2.exec.pipe.desc_unbound++, p->diag2.exec.pipe.desc_unbound_why[4]++, p->diag2.exec.pipe.desc_linear_stale++; DDM(1); return false; }
            /* The shader's image dimensionality must be the texture's: a Dim3D descriptor needs a 3D view of a 3D image, a Dim2D one a 2D view (anything else is invalid Vulkan). */
            uint32_t svf = 0;
            VkImageView v = lagfx_a64_res_sample_view_for(r->x, t, s->bind[i].dim3 != 0, s->bind[i].arr != 0, !s->bind[i].dim3 && !s->bind[i].arr && unn[st], &svf);
            if (!v && lagfx_a64_res_is_3d(t) != (s->bind[i].dim3 != 0)) { CNT(p).ext_tex_dim_mismatch++; p->diag2.exec.pipe.desc_unbound++, p->diag2.exec.pipe.desc_unbound_why[4]++; DDM(1); return false; }
            if (v && s->bind[i].dim3) CNT(p).ext_tex3d_binds++;
            /* The view covers the texture's whole mip chain (and, for a texture2d_array shader, every layer). What it still leaves out is counted per bound descriptor per draw: levels
             * the whole-chain view could not be made for (fallback to level 0), and array layers a non-array (texture2d) shader cannot address (layer 0). */
            if (v && (svf & LAGFX_A64_SV_MIPS_DROPPED)) CNT(p).ext_tex_mips_base_only++;
            if (v && (svf & LAGFX_A64_SV_LAYERS_DROPPED)) CNT(p).ext_tex_layers_base_only++;
            if (v && (svf & LAGFX_A64_SV_MIP_VIEW)) CNT(p).ext_tex_mip_views++;
            if (v && (svf & LAGFX_A64_SV_ARRAY_VIEW)) CNT(p).ext_tex_array_views++;
            if (!v) { p->diag2.exec.pipe.desc_unbound++, p->diag2.exec.pipe.desc_unbound_why[4]++; DDM(1); return false; }
            ii[i] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL };
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, w[i].pImageInfo = &ii[i];
            MIX(t->gen);
        } else {
            a64_samp_t *sm = mi_s >= 0 && mi < A64_MAX_FSAMP ? (st ? r->fsamp[mi] : r->vsamp[mi]) : NULL;
            VkSampler *vvt = st ? r->fvar : r->vvar;
            VkSampler vs = sm ? (vvt[mi] ? vvt[mi] : sm->s) : (mi_s < 0 ? default_sampler(r) : VK_NULL_HANDLE);   /* -1: an inline sampler of the shader: the default one; fvar: LOD clamps of tag 0x71 */
            if (sm && !s->bind[i].cxs && sm->cflags && lagfx_cubic_optin()) {   /* a runtime sampler whose filter field was 2: cubic only when every condition holds, else LINEAR (as without the opt-in) + a counted reason */
                lagfx_cubic_why_t cw = cubic_stage_why(r, pp, st, !sm->ci.unnormalizedCoordinates && !sm->ci.anisotropyEnable);
                VkSampler cv = cw == LAGFX_CUBIC_USE ? cubic_twin(r, sm, vs) : VK_NULL_HANDLE;
                if (cw == LAGFX_CUBIC_USE && !cv) cw = LAGFX_CUBIC_NO_COMBO;
                cubic_count(r, cw);
                if (cv) vs = cv;
            }
            if (s->bind[i].cxs) { vs = cxs_sampler(r, pp, st, s->bind[i].cxs_raw); if (vs) CNT(p).ext_cxs_binds++; }   /* a constexpr sampler: its own state, not the default */
            if (!vs) { p->diag2.exec.pipe.desc_unbound++, p->diag2.exec.pipe.desc_unbound_why[5]++; DDM(3); return false; }
            ii[i] = (VkDescriptorImageInfo){ vs, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, w[i].pImageInfo = &ii[i];
            MIX(sm ? sm->h.gen : 0xd5u), MIX((uint64_t)(uintptr_t)vs);
        }
    }
#undef MIX
#undef DDM
    VkDescriptorSet ds = r->last_ds_key == key ? r->last_ds : VK_NULL_HANDLE;
    if (!ds) {
        VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = r->dpool, .descriptorSetCount = 1, .pSetLayouts = &s->dsl };
        if (vkAllocateDescriptorSets(r->vk->device, &ai, &ds) != VK_SUCCESS) { p->diag2.exec.pipe.desc_failed++; return false; }
        for (uint32_t i = 0; i < s->n_bind; i++) w[i].dstSet = ds;
        vkUpdateDescriptorSets(r->vk->device, s->n_bind, w, 0, NULL);
        if (r->ndef == r->capdef) {
            uint32_t nc = r->capdef ? r->capdef * 2u : 64u;
            deferred_t *d2 = realloc(r->def, nc * sizeof *d2);
            if (!d2) { vkFreeDescriptorSets(r->vk->device, r->dpool, 1, &ds); p->diag2.exec.pipe.desc_failed++; return false; }
            r->def = d2, r->capdef = nc;
        }
        r->def[r->ndef++] = (deferred_t){ 2, NULL, VK_NULL_HANDLE, ds };   /* freed once nothing recorded or running can use it */
        r->last_ds = ds, r->last_ds_key = key;
        p->diag2.exec.pipe.desc_sets++;
    }
    vkCmdBindDescriptorSets(lagfx_a64_res_cmdbuf(r->x), VK_PIPELINE_BIND_POINT_GRAPHICS, s->desc.layout, 0, 1, &ds, 0, NULL);
    for (uint32_t k = 0; k < nwl; k++) lagfx_a64_res_buf_written(r->x, wl[k]);   /* every descriptor is valid and bound: the draw follows */
    return true;
}

/* Same-pixel dependency inside a dynamic render pass (VK_KHR_dynamic_rendering_local_read): framebuffer-space stages only, VK_DEPENDENCY_BY_REGION_BIT, a global memory
 * barrier (no image / buffer barriers: those would need the RENDERING_LOCAL_READ layout rules of VUID-vkCmdPipelineBarrier-image-09555). Colour writes -> input attachment
 * read / colour read / colour write. */
static void fetch_barrier(a64_run_t *r) {
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT };
    vkCmdPipelineBarrier(lagfx_a64_res_cmdbuf(r->x), VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_DEPENDENCY_BY_REGION_BIT, 1, &mb, 0, NULL, 0, NULL);
}

/* The slots of one stage's sampler table (0 vertex, 1 fragment) that this draw feeds with an unnormalized-coordinates sampler, restricted to the sampler bindings the pipeline declares. */
static uint32_t unnorm_mask(const a64_run_t *r, const a64_pipe_t *pp, uint32_t stage) {
    uint32_t m = 0;
    for (uint32_t i = 0; i < pp->src.n_bind; i++) {
        const lagfx_a64_bind_t *b = &pp->src.bind[i];
        if (b->stage != stage || b->kind != LAGFX_A64_BIND_SAMPLER || b->cxs || b->metal_index < 0 || (uint32_t)b->metal_index >= A64_MAX_FSAMP) continue;
        const a64_samp_t *sm = stage ? r->fsamp[b->metal_index] : r->vsamp[b->metal_index];
        if (sm && sm->ci.unnormalizedCoordinates) m |= 1u << b->metal_index;
    }
    return m;
}

/* The shader variant for a draw whose samplers at vmask / fmask are unnormalized. Returns 0 = the shaders as resolved (they never sample through those samplers: NOT a reason to skip),
 * 1 + index = a variant of the pipeline (rewritten per use by lagfx_spv_unnorm_rewrite), -1 = skip the draw (an unsupported use, no SPIR-V to analyse, or the module could not be made; counted). */
static int unnorm_variant(a64_run_t *r, lagfx_protocol_t *p, a64_pipe_t *pp, uint32_t vmask, uint32_t fmask, bool msfetch) {
    for (uint32_t i = 0; i < pp->nfv; i++)
        if (pp->fv[i].vmask == vmask && pp->fv[i].fmask == fmask && pp->fv[i].ms == (uint8_t)msfetch) return pp->fv[i].rc < 0 ? -1 : pp->fv[i].rc == 0 ? 0 : (int)i + 1;
    if (pp->nfv == A64_FS_VARIANTS) { CNT(p).ext_unnorm_unsup[4]++; return -1; }   /* more distinct sampler situations than the table holds (not seen in any capture) */
    const uint32_t slot = pp->nfv++;
    pp->fv[slot].vmask = vmask, pp->fv[slot].fmask = fmask, pp->fv[slot].ms = (uint8_t)msfetch, pp->fv[slot].rc = 0, pp->fv[slot].vm = pp->fv[slot].fm = VK_NULL_HANDLE;
    int rc_all = 0;
    for (uint32_t st = 0; st < 2u; st++) {
        const uint32_t mask = st ? fmask : vmask;
        if (!mask && !(st && msfetch)) continue;
        uint32_t bn[A64_MAX_FSAMP], nb = 0;
        for (uint32_t i = 0; i < pp->src.n_bind && nb < A64_MAX_FSAMP; i++) {
            const lagfx_a64_bind_t *b = &pp->src.bind[i];
            if (b->stage == st && b->kind == LAGFX_A64_BIND_SAMPLER && !b->cxs && b->metal_index >= 0 && (uint32_t)b->metal_index < A64_MAX_FSAMP && (mask >> b->metal_index & 1u)) bn[nb++] = b->binding;
        }
        const uint8_t *spv = (const uint8_t *)(st ? pp->src.desc.fragment_spv : pp->src.desc.vertex_spv);
        const size_t sz = st ? pp->src.desc.fragment_spv_bytes : pp->src.desc.vertex_spv_bytes;
        uint8_t *o = NULL; size_t osz = 0; lagfx_spv_unnorm_report_t rep;
        memset(&rep, 0, sizeof rep);
        int rc = !mask ? 0 : spv ? lagfx_spv_unnorm_rewrite(spv, sz, bn, nb, 1, &o, &osz, &rep) : -2;
        if (rc == -2) { CNT(p).ext_unnorm_unsup[4]++; rc_all = -1; continue; }   /* the resolver gave no SPIR-V: the uses cannot be judged */
        CNT(p).ext_unnorm_use_implicit0 += rep.implicit_to_lod0, CNT(p).ext_unnorm_use_bias_dropped += rep.bias_dropped, CNT(p).ext_unnorm_use_offset_folded += rep.offset_folded;
        CNT(p).ext_unnorm_use_explicit_kept += rep.explicit_kept, CNT(p).ext_unnorm_use_grad_kept += rep.grad_kept, CNT(p).ext_unnorm_use_gather_kept += rep.gather_kept, CNT(p).ext_unnorm_use_unresolved += rep.uses_unresolved;
        if (rc >= 0 && st && msfetch) {   /* framebuffer fetch in a multisampled pass: the (possibly already unnorm-rewritten) fragment module reads the invocation's sample */
            uint8_t *o2 = NULL; size_t o2sz = 0; uint32_t nr = 0;
            const int r2 = lagfx_spv_msfetch_rewrite(rc == 1 ? o : spv, rc == 1 ? osz : sz, &o2, &o2sz, &nr);
            if (r2 < 0) { p->diag2.exec.msaa.fbfetch_ms_refused++; rc = -3; }
            else if (r2 == 1) { free(o); o = o2, osz = o2sz, rc = 1; p->diag2.exec.msaa.fbfetch_ms_variants++, p->diag2.exec.msaa.fbfetch_ms_reads += nr; }
        }
        if (rc == -3) rc_all = -1;
        else if (rc < 0) {
            CNT(p).ext_unnorm_unsup[0] += rep.unsup_dref, CNT(p).ext_unnorm_unsup[1] += rep.unsup_proj, CNT(p).ext_unnorm_unsup[2] += rep.unsup_sparse, CNT(p).ext_unnorm_unsup[3] += rep.unsup_image_dim, CNT(p).ext_unnorm_unsup[4] += rep.unsup_other;
            LAGFX_WARN("arm64 pipeline %u: %s shader samples through an unnormalized-coordinates sampler in a way Vulkan forbids (dref %u proj %u sparse %u image_dim %u other %u): draws with this sampler are skipped (counted)",
                       pp->ref, st ? "fragment" : "vertex", rep.unsup_dref, rep.unsup_proj, rep.unsup_sparse, rep.unsup_image_dim, rep.unsup_other);
            rc_all = -1;
        } else if (rc == 1) {
            VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = osz, .pCode = (const uint32_t *)(const void *)o };
            VkShaderModule m = VK_NULL_HANDLE;
            if (vkCreateShaderModule(r->vk->device, &ci, NULL, &m) != VK_SUCCESS) { CNT(p).ext_unnorm_variant_fail++; rc_all = -1; }
            else { if (st) pp->fv[slot].fm = m; else pp->fv[slot].vm = m; CNT(p).ext_unnorm_variants++; if (rc_all == 0) rc_all = 1; }
        }
        free(o);
    }
    if (rc_all < 0) { if (pp->fv[slot].vm) vkDestroyShaderModule(r->vk->device, pp->fv[slot].vm, NULL); if (pp->fv[slot].fm) vkDestroyShaderModule(r->vk->device, pp->fv[slot].fm, NULL); pp->fv[slot].vm = pp->fv[slot].fm = VK_NULL_HANDLE; }
    pp->fv[slot].rc = rc_all;
    return rc_all < 0 ? -1 : rc_all == 0 ? 0 : (int)slot + 1;
}

/* Everything a draw needs besides the vertex buffer: the VkPipeline of this primitive type (Metal 0 point, 1 line, 2 line strip, 3 triangle, 4 triangle strip =
 * the VkPrimitiveTopology values) bound in the open pass, and the descriptor set. False = draw skipped (counted). */
static bool prepare_pipeline(a64_run_t *r, lagfx_protocol_t *p, uint32_t prim) {
    if (prim > 4u) { CNT(p).prim_unsupported++; CNT(p).draws_skipped++; return false; }
    if (prim == 0u && r->pipe->src.no_point_size) { p->diag2.exec.pipe.point_no_size++; CNT(p).draws_skipped++; return false; }   /* Metal needs [[point_size]] too */
    bool msfetch = false;   /* framebuffer fetch of multisampled attachments: the shader needs a multisampled SubpassData type and a per-sample read (spv_msfetch.c) */
    if (r->ms.samples > 1u)
        for (uint32_t i = 0; i < r->pipe->src.n_bind; i++)
            if (r->pipe->src.bind[i].kind == LAGFX_A64_BIND_INPUT) msfetch = true;
    if (msfetch && !r->vk->have_sample_rate_shading) { p->diag2.exec.msaa.fbfetch_ms_refused++; p->diag2.exec.msaa.sample_shading_refused++; CNT(p).draws_skipped++; return false; }   /* no per-sample shading on this device: refused, counted */
    if (msfetch && !lagfx_vk_fetch_ms_capable(r->vk)) { p->diag2.exec.msaa.fbfetch_ms_refused++; p->diag2.exec.msaa.fbfetch_ms_no_local_read_ms++; CNT(p).draws_skipped++; return false; }   /* the device does not offer local reads of multisampled attachments (Vulkan 1.4 property): refused, counted */
    if (!lagfx_vk_fetch_capable(r->vk))   /* a pipeline that reads the attachments cannot run without the device feature */
        for (uint32_t i = 0; i < r->pipe->src.n_bind; i++)
            if (r->pipe->src.bind[i].kind == LAGFX_A64_BIND_INPUT) { p->diag2.exec.pipe.stub_fbfetch++, p->diag2.exec.pipe.stub_fbfetch_why[LAGFX_A64_FBF_NO_LOCAL_READ]++; CNT(p).draws_skipped++; return false; }
    VkFormat fmts[4] = { VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED };
    for (uint32_t i = 0; i < r->nslots && i < 4u; i++) if (r->rts[i]) fmts[i] = r->rts[i]->format;
    for (uint32_t i = 0; i < r->pipe->src.n_bind; i++) {   /* VUID-vkCmdDraw-format-07753: the numeric type of the attachment's format and the Sampled Type of the shader's input attachment must match */
        const lagfx_a64_bind_t *b = &r->pipe->src.bind[i];
        if (b->kind != LAGFX_A64_BIND_INPUT || b->metal_index < 0 || b->metal_index > 3 || !r->rts[b->metal_index]) continue;
        if ((int)b->ia_class != lagfx_fmtcap_class(r->rts[b->metal_index]->format)) { p->diag2.exec.fmt.fbf_class_mismatch++, CNT(p).draws_skipped++; return false; }
    }
    uint32_t alt = 0;   /* unnormalized-coordinates samplers in this draw: the shader USES that sample through them are rewritten (per OpSampledImage, spv_unnorm.c); a shader that never samples through them runs unchanged */
    {
        const uint32_t vmask = unnorm_mask(r, r->pipe, 0), fmask = unnorm_mask(r, r->pipe, 1);
        if (vmask | fmask | (uint32_t)msfetch) {
            int v = unnorm_variant(r, p, r->pipe, vmask, fmask, msfetch);
            if (v < 0) { if (!msfetch) CNT(p).ext_unnorm_skipped++; CNT(p).draws_skipped++; return false; }
            if (v == 0) { if (vmask | fmask) CNT(p).ext_unnorm_unused++; }
            else alt = (uint32_t)v << 4, (vmask | fmask) ? (void)CNT(p).ext_unnorm_lod0++ : (void)0;
        }
    }
    VkPipeline vp = pipeline_for(r, p, r->pipe, fmts, r->nslots, prim | alt);
    if (!vp) { CNT(p).no_pipeline++, CNT(p).no_pipeline_why[2]++; CNT(p).draws_skipped++; if (lagfx_vk_render_backend(r->vk) == LAGFX_RENDER_LEGACY) p->diag2.exec.pipe.legacy_draws_dropped_pipe++; return false; }   /* 2: vkCreateGraphicsPipelines failed (now or earlier: failures are cached) */
    if (r->bound_vk != vp) {
        if (lagfx_translate_render_bind_vk_pipeline(&r->rs, vp, r->pipe->src.desc.layout) != LAGFX_OK) { CNT(p).no_pipeline++, CNT(p).no_pipeline_why[3]++; CNT(p).draws_skipped++; return false; }   /* 3: recording the bind failed */
        r->bound_vk = vp;
    }
    if (!bind_descriptors(r, p, r->pipe)) { CNT(p).draws_skipped++; return false; }
    bool fetch = false;
    for (uint32_t i = 0; i < r->pipe->src.n_bind; i++) fetch |= r->pipe->src.bind[i].kind == LAGFX_A64_BIND_INPUT;
    if (fetch || r->last_fetch) {   /* same-pixel dependency inside the pass: earlier colour writes -> this draw's input-attachment read, and (after a reading draw) its read -> later writes */
        fetch_barrier(r);
        if (fetch) p->diag2.exec.pipe.fbfetch_draws++;
    }
    r->last_fetch = fetch;
    return true;
}

static void pass_end(a64_run_t *r) {
    if (r->rendering) {
        lagfx_translate_render_end(&r->rs);
        if (r->wrote_storage) {   /* fragment storage-image writes are not covered by the colour attachment barrier of target_end */
            VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
            vkCmdPipelineBarrier(lagfx_a64_res_cmdbuf(r->x), VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        }
        r->wrote_storage = false;
        for (uint32_t i = 0; i < r->nslots && i < 4u; i++) if (r->rts[i]) lagfx_a64_res_target_end(r->x, r->rts[i]);   /* COLOR_ATTACHMENT_OPTIMAL -> GENERAL + barrier: a following blit / pass sees the result */
        lagfx_a64_msaa_pass_end(r->p, r->x, &r->ms);   /* resolve targets back to GENERAL; the copy of a 1x-fallback attachment */
        if (r->dsobj) lagfx_a64_res_ds_end(r->x, r->dsobj);
        if (r->cap_pass) lagfx_a64_cap_pass_close(r->cap);   /* capture: post copies + bound resources, recorded after the attachments are back in GENERAL */
    }
    r->cap_pass = false;
    r->dsobj = NULL, r->ds_dfmt = r->ds_sfmt = VK_FORMAT_UNDEFINED;
    r->pass_open = r->rendering = false, r->last_fetch = false;
    memset(r->rts, 0, sizeof r->rts), r->nslots = 0;
    r->rt = NULL, r->pipe = NULL, r->pipe_set = r->pipe_failed = false, r->bound_vk = VK_NULL_HANDLE;
}

#define R32(o) lagfx_le32(rec + (o))
#define R16(o) ((uint32_t)rec[(o)] | (uint32_t)rec[(o) + 1] << 8)
static double rd_f64(const uint8_t *b) { uint64_t u = lagfx_le64(b); double d; memcpy(&d, &u, 8); return d; }


static bool dup_of_color(const a64_run_t *r, const lagfx_a64_obj_t *o) {
    for (uint32_t q = 0; q < 4u; q++) if (r->rts[q] == o) return true;
    return false;
}

/* EXACT-mode whole-pass rejection (arm64_msaa.h): undoes whatever begin_pass has begun so far (attachment layouts back to GENERAL, resolve targets, depth / stencil), records NO
 * rendering for the pass (no load / clear op is ever executed), counts ms_pass_refused with the reason. The following records of the pass find no target and draw nothing. */
static void reject_pass(a64_run_t *r, lagfx_protocol_t *p, lagfx_a64_obj_t *ds_begun, const char *what, int idx, const char *why, uint32_t ref) {
    lagfx_a64_msaa_veto_t v; memset(&v, 0, sizeof v);
    if (idx >= 0) snprintf(v.what, sizeof v.what, "%s%d", what, idx); else snprintf(v.what, sizeof v.what, "%s", what);
    v.why = why, v.ref = ref;
    for (uint32_t q = 0; q < 4u; q++) if (r->rts[q]) lagfx_a64_res_target_end(r->x, r->rts[q]);
    lagfx_a64_msaa_pass_abort(r->x, &r->ms);
    if (ds_begun) lagfx_a64_res_ds_end(r->x, ds_begun);
    memset(r->rts, 0, sizeof r->rts), r->nslots = 0, r->ms.samples = 0;
    lagfx_a64_msaa_reject(p, &v);
    CNT(p).no_target++; tag_partial(p, 26);
}

/* tag 26 (inner opcode 0x1a): the 584-byte pass descriptor, parsed by the existing lagfx_parse_render_pass_descriptor (render_pass.c). Up to four colour
 * attachments (by original slot) and one depth / stencil texture are rendered to; begin/end of the dynamic rendering go through the existing render encoder
 * (translate/render_encoder.c). */
static void begin_pass(a64_run_t *r, lagfx_protocol_t *p, const uint8_t *rec, uint32_t n) {
    pass_end(r);
    lagfx_a64_res_sync_linear(r->x);   /* linear textures whose buffer changed since the last pull: copied now, outside any pass */
    CNT(p).passes++;
    r->pass_open = true;                        /* even without a usable target: the following tags belong to this pass */
    memset(r->vb, 0, sizeof r->vb);
    memset(&r->region, 0, sizeof r->region);
    reset_frag_tables(r);
    r->ms.samples = 0;
    lagfx_render_pass_full_desc_t d;
    if (n < LAGFX_RENDER_PASS_WIRE_SIZE || lagfx_parse_render_pass_descriptor(rec, n, &d) != 0) { CNT(p).bad_rec++; tag_unsup(p, 26); return; }
    {   /* a new encoder: Metal's defaults for everything the depth / stencil / raster records set */
        memset(&r->es, 0, sizeof r->es);
        lagfx_a64_dss_default(&r->es.dss);
        r->es.dirty = true;
    }
    bool strict = false;   /* EXACT mode and a multisampled attachment named: all-or-nothing pass (arm64_msaa.h) */
    lagfx_a64_msaa_look_t look;   /* the objects the descriptor names, looked up once */
    {
        lagfx_a64_msaa_veto_t veto;
        if (!lagfx_a64_msaa_pass_gate(p, r->x, &r->ms, &d, vk_core13(r->vk), &look, &strict, &veto)) { lagfx_a64_msaa_reject(p, &veto); CNT(p).no_target++; tag_partial(p, 26); return; }
    }
    /* colour attachments by their ORIGINAL slot (the fragment output location); up to 4. The SAME mirror object a blit / sync / write-back of this exec
     * resolves for the ref. A slot whose target cannot be built is left unused (and counted), the others still render. */
    VkRenderingAttachmentInfo ca[4];
    memset(ca, 0, sizeof ca);
    for (int k = 0; k < 4; k++) ca[k] = (VkRenderingAttachmentInfo){ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = VK_NULL_HANDLE,
        .imageLayout = lagfx_vk_fetch_layout(r->vk), .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE };
    lagfx_a64_obj_t *first = NULL;
    uint32_t fw = 0, fh = 0, nslots = 0;
    uint32_t cap_load[4] = {0}; double cap_clear[4][4]; memset(cap_clear, 0, sizeof cap_clear);
    const bool capw = r->cap && lagfx_a64_cap_pass_wanted(r->cap);
    if (capw) {   /* capture: contents BEFORE the pass, copied while the attachments are still in GENERAL (nothing is recorded otherwise) */
        for (uint32_t k = 0; k < d.color_attachment_count && k < LAGFX_RENDER_PASS_MAX_COLOR_ATTACHMENTS; k++)
            if (d.colors[k].slot < 4u) lagfx_a64_cap_pre(r->cap, look.c[k]);
    }
    for (uint32_t k = 0; k < d.color_attachment_count && k < LAGFX_RENDER_PASS_MAX_COLOR_ATTACHMENTS; k++) {
        uint32_t slot = d.colors[k].slot;
        if (slot >= 4u) { if (strict) { reject_pass(r, p, NULL, "colour", (int)slot, "slot_out_of_range", d.colors[k].texture_ref); return; } CNT(p).extra_attach_ignored++; continue; }
        lagfx_a64_obj_t *t = look.c[k];
        if (look.lin[k]) CNT(p).obj_unsupported_linear++;      /* a type 8/9 render target the mirror could not build */
        if (strict && (!t || t->kind != LAGFX_A64_TEXTURE)) { reject_pass(r, p, NULL, "colour", (int)slot, "texture_missing", d.colors[k].texture_ref); return; }
        if (!t || t->kind != LAGFX_A64_TEXTURE) { CNT(p).no_target++; if (!t) lagfx_a64_msaa_note_missing(p, r->x, d.colors[k].texture_ref); continue; }
        if (strict && !lagfx_a64_msaa_attach_ok(p, t)) { reject_pass(r, p, NULL, "colour", (int)slot, "host_sample_count_differs", d.colors[k].texture_ref); return; }
        if (!lagfx_a64_msaa_attach_ok(p, t)) { CNT(p).no_target++; continue; }   /* exact mode: never render into an image whose host sample count is not the guest's */
        if (strict && first && lagfx_a64_msaa_obj_samples(t) != r->ms.samples) { reject_pass(r, p, NULL, "colour", (int)slot, "mixed_sample_counts", d.colors[k].texture_ref); return; }
        if (first && lagfx_a64_msaa_obj_samples(t) != r->ms.samples) { CNT(p).extra_attach_ignored++; p->diag2.exec.msaa.attach_mixed_ignored++; continue; }   /* one sample count per pass (Vulkan: every attachment equals rasterizationSamples) */
        uint32_t load = d.colors[k].load_action;   /* MTLLoadAction: 0 DontCare, 1 Load, 2 Clear */
        VkAttachmentLoadOp lop = load == 2u ? VK_ATTACHMENT_LOAD_OP_CLEAR : load == 1u ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        VkClearValue cv;
        memset(&cv, 0, sizeof cv);
        if (lop == VK_ATTACHMENT_LOAD_OP_CLEAR) lagfx_fmtcap_clear_color(t->format, d.colors[k].clear_color, &cv.color);   /* the union member follows the format's numeric class (S04) */
        VkImageView view; uint32_t w, h;
        if (!(t->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) p->diag2.exec.fmt.target_not_attachable++;   /* the device cannot render to this format (counted: not a silent no_target) */
        if (!lagfx_a64_res_target_begin(r->x, t, &view, &w, &h)) { if (strict) { reject_pass(r, p, NULL, "colour", (int)slot, "target_begin_failed", d.colors[k].texture_ref); return; } CNT(p).no_target++; continue; }   /* layout GENERAL -> COLOR_ATTACHMENT_OPTIMAL */
        if (first && (w != fw || h != fh)) { lagfx_a64_res_target_end(r->x, t); if (strict) { reject_pass(r, p, NULL, "colour", (int)slot, "extent_mismatch", d.colors[k].texture_ref); return; } CNT(p).extra_attach_ignored++; continue; }   /* one render area for all attachments */
        bool dup = false;
        for (uint32_t q = 0; q < 4u; q++) dup |= r->rts[q] == t;
        if (dup) { lagfx_a64_res_target_end(r->x, t); if (strict) { reject_pass(r, p, NULL, "colour", (int)slot, "duplicate_attachment", d.colors[k].texture_ref); return; } CNT(p).extra_attach_ignored++; continue; }   /* the same image twice is not a valid attachment set */
        if (!first) first = t, fw = w, fh = h, r->ms.samples = lagfx_a64_msaa_obj_samples(t);
        r->rts[slot] = t;
        cap_load[slot] = load; for (int c = 0; c < 4; c++) cap_clear[slot][c] = d.colors[k].clear_color[c];
        ca[slot].imageView = view, ca[slot].imageLayout = lagfx_a64_res_target_layout(r->x, t), ca[slot].loadOp = lop, ca[slot].storeOp = VK_ATTACHMENT_STORE_OP_STORE, ca[slot].clearValue = cv;
        {   /* multisampled texture: store action / resolve texture (arm64_msaa.c); a plain texture keeps STORE (a resolve action on it is counted) */
            const lagfx_a64_obj_t *att[4] = { r->rts[0], r->rts[1], r->rts[2], r->rts[3] };
            lagfx_a64_msaa_color(p, r->x, &r->ms, slot, slot, t, d.colors[k].store_action, d.colors[k].resolve_texture_ref, d.colors[k].resolve_level || d.colors[k].resolve_slice, att, &ca[slot]);
            if (strict && r->ms.fail) { reject_pass(r, p, NULL, "resolve", (int)slot, r->ms.fail, d.colors[k].resolve_texture_ref); return; }   /* a resolve the store action requires could not be set up */
        }
        if (slot + 1u > nslots) nslots = slot + 1u;
        if (slot > 0u) CNT(p).multi_attach++;
    }
    /* depth / stencil attachment: one mirror texture of a depth, stencil or combined format. A combined texture is attached with both aspects whichever the pass
     * names (an aspect the pass does not name is loaded and stored, i.e. left alone). Store actions are not honoured (set later by tags 105 / 120): the attachment
     * is always stored, which only ever keeps more than Metal would. Anything else is counted and the pass runs without depth / stencil. */
    VkRenderingAttachmentInfo dai = {0}, sai = {0};
    lagfx_a64_obj_t *dso = NULL;
    VkImageView dview = VK_NULL_HANDLE; uint32_t dw = 0, dh = 0;
    if (d.has_depth || d.has_stencil) {
        uint32_t dref = d.has_depth ? d.depth_texture_ref : 0u, sref = d.has_stencil ? d.stencil_texture_ref : 0u;
        lagfx_a64_obj_t *o = NULL;
        if (strict && !vk_core13(r->vk)) { reject_pass(r, p, NULL, "depth", -1, "no_depth_stencil_support", dref ? dref : sref); return; }
        if (!vk_core13(r->vk)) { CNT(p).ds_no_device_support++; p->diag2.exec.pipe.stub_depth_stencil++; }
        else {
            o = look.ds;
            if (!o) lagfx_a64_msaa_note_missing(p, r->x, dref ? dref : sref);
            else if (!lagfx_a64_msaa_attach_ok(p, o)) o = NULL;   /* counted below as ds_attach_unsupported */
            if (strict && (!o || o->kind != LAGFX_A64_TEXTURE || !o->ds_aspects || (dref && !(o->ds_aspects & 1u)) || (!dref && !(o->ds_aspects & 2u)) || (dref && sref && sref != dref) || (dref && d.depth_level))) { reject_pass(r, p, NULL, dref ? "depth" : "stencil", -1, "depth_stencil_texture_unusable", dref ? dref : sref); return; }
            if (!o || o->kind != LAGFX_A64_TEXTURE || !o->ds_aspects || (dref && !(o->ds_aspects & 1u)) || (!dref && !(o->ds_aspects & 2u))) { CNT(p).ds_attach_unsupported++; o = NULL; }
            else if (dref && sref && sref != dref) { CNT(p).ds_split_ignored++; }   /* the stencil lives in another texture: only the depth one is attached */
            else if (dref && d.depth_level) { CNT(p).ds_attach_unsupported++; o = NULL; }
        }
        if (o && lagfx_a64_res_ds_begin(r->x, o, &dview, &dw, &dh)) {
            if (strict && ((first && (dw < fw || dh < fh)) || dup_of_color(r, o))) { reject_pass(r, p, o, dref ? "depth" : "stencil", -1, "extent_or_duplicate", dref ? dref : sref); return; }
            if (strict && first && lagfx_a64_msaa_obj_samples(o) != r->ms.samples) { reject_pass(r, p, o, dref ? "depth" : "stencil", -1, "mixed_sample_counts", dref ? dref : sref); return; }
            if ((first && (dw < fw || dh < fh)) || dup_of_color(r, o)) { CNT(p).ds_attach_unsupported++; CNT(p).extra_attach_ignored++; }   /* attachments must cover the render area */
            else if (first && lagfx_a64_msaa_obj_samples(o) != r->ms.samples) { CNT(p).ds_attach_unsupported++; p->diag2.exec.msaa.ds_mismatch_ignored++; }   /* depth / stencil and colour of one pass share one sample count */
            else {
                dso = o;
                if (!first) r->ms.samples = lagfx_a64_msaa_obj_samples(o);   /* a depth-only multisampled pass */
                if (r->ms.samples > 1u && (d.depth_resolve_texture_ref || d.stencil_resolve_texture_ref || d.depth_store_action >= 2u || d.stencil_store_action >= 2u)) p->diag2.exec.msaa.ds_resolve_unsupported++;   /* depth / stencil resolve: stored only */
            }
        } else if (o) { if (strict) { reject_pass(r, p, NULL, dref ? "depth" : "stencil", -1, "ds_begin_failed", dref ? dref : sref); return; } CNT(p).ds_attach_unsupported++; }
    }
    if (!first && !dso) { CNT(p).no_target++; tag_partial(p, 26); return; }
    if (dso) {
        const bool want_d = d.has_depth && d.depth_texture_ref && (dso->ds_aspects & 1u), want_s = d.has_stencil && d.stencil_texture_ref == (want_d ? d.depth_texture_ref : d.stencil_texture_ref) && (dso->ds_aspects & 2u);
        dai = (VkRenderingAttachmentInfo){ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dview, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
        sai = dai;
        if (want_d) {
            double cd = d.depth_clear_value;
            dai.loadOp = d.depth_load_action == 2u ? VK_ATTACHMENT_LOAD_OP_CLEAR : d.depth_load_action == 1u ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            dai.clearValue.depthStencil.depth = cd >= 1.0 ? 1.0f : cd > 0.0 ? (float)cd : 0.0f;   /* Metal clamps the clear depth to [0, 1] (also maps NaN to 0) */
        }
        if (want_s) {
            sai.loadOp = d.stencil_load_action == 2u ? VK_ATTACHMENT_LOAD_OP_CLEAR : d.stencil_load_action == 1u ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            sai.clearValue.depthStencil.stencil = d.stencil_clear_value;
        }
        if (!first) { fw = dw, fh = dh; }
    }
    if (!first) {   /* depth / stencil only: one unused colour slot keeps the pipeline and rendering-info counts consistent */
        nslots = 1u;
        CNT(p).ds_depth_only_passes++;
    }
    r->nslots = nslots;
    VkFormat cfmt[4] = { VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED };   /* the legacy backend (VkRenderPass) needs the formats the infos do not carry */
    for (uint32_t q = 0; q < 4u; q++) if (r->rts[q]) cfmt[q] = r->rts[q]->format;
    r->rs.legacy_refuse = 0;
    if (lagfx_translate_render_begin_fmt(r->vk, &r->rs, lagfx_a64_res_cmdbuf(r->x), first ? first->img : dso->img, fw, fh, ca, nslots,
                                         dso && (dso->ds_aspects & 1u) ? &dai : NULL, dso && (dso->ds_aspects & 2u) ? &sai : NULL,
                                         cfmt, dso ? dso->format : VK_FORMAT_UNDEFINED, r->ms.samples > 1u ? r->ms.samples : 1u) != LAGFX_OK) {
        for (uint32_t q = 0; q < 4u; q++) if (r->rts[q]) lagfx_a64_res_target_end(r->x, r->rts[q]);
        lagfx_a64_msaa_pass_abort(r->x, &r->ms);
        if (dso) lagfx_a64_res_ds_end(r->x, dso);
        memset(r->rts, 0, sizeof r->rts), r->nslots = 0;
        if (r->rs.legacy_refuse && r->rs.legacy_refuse < LAGFX_LR__COUNT) p->diag2.exec.pipe.legacy_begin_why[r->rs.legacy_refuse]++;   /* per cause: which shape the VkRenderPass backend could not begin */
        if (strict) { lagfx_a64_msaa_veto_t v; memset(&v, 0, sizeof v); snprintf(v.what, sizeof v.what, "pass"), v.why = "render_begin_failed"; lagfx_a64_msaa_reject(p, &v); }
        CNT(p).no_target++; tag_partial(p, 26); return;
    }
    if (dso) {
        r->dsobj = dso;
        r->ds_dfmt = (dso->ds_aspects & 1u) ? dso->format : VK_FORMAT_UNDEFINED, r->ds_sfmt = (dso->ds_aspects & 2u) ? dso->format : VK_FORMAT_UNDEFINED;
        CNT(p).ds_passes++;
    }
    r->rendering = true, r->rt = first, r->rt_w = fw, r->rt_h = fh;
    if (first && lagfx_vk_render_backend(r->vk) == LAGFX_RENDER_LEGACY)
        for (uint32_t q = 0; q < nslots && q < 4u; q++) if (!r->rts[q]) { p->diag2.exec.pipe.legacy_gap_passes++; break; }   /* an unused colour slot, kept in place */
    for (uint32_t k = 0; k < d.color_attachment_count && k < LAGFX_RENDER_PASS_MAX_COLOR_ATTACHMENTS && k < 2u; k++)   /* idle-gap diagnostics: logged only in the window after a gap, one relaxed load otherwise */
        lagfx_gap_note_pass(d.colors[k].texture_ref, d.colors[k].slot, d.colors[k].load_action, d.colors[k].store_action, d.colors[k].clear_color, fw, fh, d.color_attachment_count);
    if (r->ms.samples > 1u) p->diag2.exec.msaa.ms_passes++;
    if (r->cap) {
        lagfx_a64_cap_att_in_t ai[4]; uint32_t na = 0;
        for (uint32_t k = 0; k < 4u; k++) if (r->rts[k]) { memset(&ai[na], 0, sizeof ai[na]); ai[na].o = r->rts[k], ai[na].slot = k, ai[na].load = cap_load[k]; for (int c = 0; c < 4; c++) ai[na].clear[c] = (float)cap_clear[k][c]; na++; }
        lagfx_a64_cap_pass_in_t pi = { .w = fw, .h = fh, .depth = dso && (dso->ds_aspects & 1u), .stencil = dso && (dso->ds_aspects & 2u), .depth_ref = d.depth_texture_ref, .depth_vkfmt = dso ? (uint32_t)dso->format : 0u,
            .depth_load = d.depth_load_action, .stencil_load = d.stencil_load_action, .stencil_clear = d.stencil_clear_value, .depth_clear = (float)d.depth_clear_value };
        if (capw) lagfx_a64_cap_pass_open(r->cap, &pi, ai, na), r->cap_pass = true;
    }
    memcpy(r->rsnap.ca, ca, sizeof ca), r->rsnap.dai = dai, r->rsnap.sai = sai, r->rsnap.img = first ? first->img : dso->img;
    r->rsnap.has_d = dso && (dso->ds_aspects & 1u), r->rsnap.has_s = dso && (dso->ds_aspects & 2u);
    tag_done(p, 26);
}

static void set_pipeline(a64_run_t *r, lagfx_protocol_t *p, const uint8_t *rec, uint32_t n) {
    if (n < 4u) { CNT(p).bad_rec++; tag_unsup(p, 116); return; }
    a64_pipe_t *pp = pipeline_current(r, p, R32(0));
    r->pipe_set = r->pipe_failed = false, r->pipe = NULL;     /* Apple: Metal crashes on a missing pipeline, and on drawing without one */
    if (!pp) { r->pipe_failed = true; tag_partial(p, 116); return; }
    r->pipe = pp, r->pipe_set = true;        /* the VkPipeline is built and bound at the draw (it depends on the primitive type) */
    tag_done(p, 116);
}

static bool draw_ready(a64_run_t *r, lagfx_protocol_t *p) {
    if (!r->pipe_set || !r->pipe) { CNT(p).no_pipeline++, CNT(p).no_pipeline_why[r->pipe_failed ? 1 : 0]++; CNT(p).draws_skipped++; return false; }   /* Apple: no pipeline, no draw (why: 0 none set in this pass, 1 tag 116's pipeline did not resolve) */
    if (r->pipe->src.tile) { p->diag2.exec.tile.draws_ignored++; CNT(p).draws_skipped++; return false; }   /* a tile pipeline draws nothing */
    if (!r->rendering) { CNT(p).no_target++; CNT(p).draws_skipped++; return false; }
    return true;
}

/* Metal MTLCompareFunction / MTLStencilOperation and the Vulkan enums share one numbering (0..7), but the mapping is spelled out so a change on either side
 * cannot slip through unnoticed. */
static VkCompareOp vk_cmp(uint32_t m) {
    switch (m) {
    case LAGFX_MTL_CMP_NEVER: return VK_COMPARE_OP_NEVER;
    case LAGFX_MTL_CMP_LESS: return VK_COMPARE_OP_LESS;
    case LAGFX_MTL_CMP_EQUAL: return VK_COMPARE_OP_EQUAL;
    case LAGFX_MTL_CMP_LESS_EQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case LAGFX_MTL_CMP_GREATER: return VK_COMPARE_OP_GREATER;
    case LAGFX_MTL_CMP_NOT_EQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case LAGFX_MTL_CMP_GREATER_EQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}
static VkStencilOp vk_sop(uint32_t m) {
    switch (m) {
    case LAGFX_MTL_STENCIL_ZERO: return VK_STENCIL_OP_ZERO;
    case LAGFX_MTL_STENCIL_REPLACE: return VK_STENCIL_OP_REPLACE;
    case LAGFX_MTL_STENCIL_INC_CLAMP: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case LAGFX_MTL_STENCIL_DEC_CLAMP: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case LAGFX_MTL_STENCIL_INVERT: return VK_STENCIL_OP_INVERT;
    case LAGFX_MTL_STENCIL_INC_WRAP: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case LAGFX_MTL_STENCIL_DEC_WRAP: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

/* Records the encoder state a draw depends on that is not in the VkPipeline (all of it dynamic): cull mode, winding, depth bias, and, with a depth / stencil
 * attachment in the pass, the depth test / write / compare and the stencil test, operations, masks and reference of both faces. Only when something changed
 * since the last draw of the pass. Winding: the viewport is flipped (negative height) to give Metal's y-up clip space, and Vulkan decides the facing from the
 * final window coordinates, so on-screen clockwise (Metal's default, 0) is VK_FRONT_FACE_CLOCKWISE. */
static void apply_encoder_state(a64_run_t *r, lagfx_protocol_t *p) {
    if (!r->es.dirty || !vk_core13(r->vk)) return;
    VkCommandBuffer cb = lagfx_a64_res_cmdbuf(r->x);
    vkCmdSetCullMode(cb, r->es.cull == 1u ? VK_CULL_MODE_FRONT_BIT : r->es.cull == 2u ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cb, r->es.winding == 1u ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    float clamp = r->es.bias[2];
    if (clamp != 0.0f && !r->vk->have_depth_bias_clamp) { clamp = 0.0f; CNT(p).bias_clamp_dropped++; }
    vkCmdSetDepthBias(cb, r->es.bias[0], clamp, r->es.bias[1]);
    if (r->dsobj) {
        const lagfx_a64_dss_t *d = &r->es.dss;
        vkCmdSetDepthTestEnable(cb, lagfx_a64_dss_depth_test(d) ? VK_TRUE : VK_FALSE);
        vkCmdSetDepthWriteEnable(cb, d->depth_write ? VK_TRUE : VK_FALSE);
        vkCmdSetDepthCompareOp(cb, vk_cmp(d->depth_compare));
        vkCmdSetStencilTestEnable(cb, lagfx_a64_dss_stencil_test(d) ? VK_TRUE : VK_FALSE);
        const VkStencilFaceFlags fc[2] = { VK_STENCIL_FACE_FRONT_BIT, VK_STENCIL_FACE_BACK_BIT };
        const lagfx_a64_ds_face_t *f[2] = { &d->front, &d->back };
        for (int i = 0; i < 2; i++) {
            vkCmdSetStencilOp(cb, fc[i], vk_sop(f[i]->fail_op), vk_sop(f[i]->pass_op), vk_sop(f[i]->depth_fail_op), vk_cmp(f[i]->compare));
            vkCmdSetStencilCompareMask(cb, fc[i], f[i]->read_mask);
            vkCmdSetStencilWriteMask(cb, fc[i], f[i]->write_mask);
            vkCmdSetStencilReference(cb, fc[i], r->es.sref[i]);
        }
    }
    r->es.dirty = false;
}

static bool bind_for_draw(a64_run_t *r, lagfx_protocol_t *p, uint32_t prim) {
    if (!prepare_pipeline(r, p, prim)) return false;
    apply_encoder_state(r, p);
    if (r->pipe->src.desc.blend_const_dyn) {   /* set before EVERY draw of such a pipeline: binding another pipeline may leave the dynamic value undefined */
        vkCmdSetBlendConstants(lagfx_a64_res_cmdbuf(r->x), r->es.blend_color);
        CNT(p).blend_const_draws++;
    }
    if (r->dsobj && (lagfx_a64_dss_depth_test(&r->es.dss) || lagfx_a64_dss_stencil_test(&r->es.dss))) CNT(p).ds_draws++;
    lagfx_gap_note_draw(prim, r->region.vp_x, r->region.vp_y, r->region.vp_w, r->region.vp_h, r->region.vp_z_set ? r->region.vp_znear : 0.0f, r->region.vp_z_set ? r->region.vp_zfar : 1.0f, r->region.sc_x, r->region.sc_y, r->region.sc_w, r->region.sc_h, r->rt_w, r->rt_h);   /* idle-gap diagnostics: window only */
    lagfx_vk_render_target_t ext = { .width = r->rt_w, .height = r->rt_h };   /* set_region only needs the extent */
    r->region.metal = true, r->region.lim_dim = (float)(r->vk->phys_props.limits.maxViewportDimensions[0] < r->vk->phys_props.limits.maxViewportDimensions[1] ? r->vk->phys_props.limits.maxViewportDimensions[0] : r->vk->phys_props.limits.maxViewportDimensions[1]),
        r->region.lim_lo = r->vk->phys_props.limits.viewportBoundsRange[0], r->region.lim_hi = r->vk->phys_props.limits.viewportBoundsRange[1];
    {   /* Metal viewport / scissor as recorded (fractional, partly outside the target allowed); a viewport that draws nothing in Metal either (viewport_empty_metal) or one Vulkan cannot express (viewport_inexpressible) draws nothing here (S05) */
        const lagfx_vp_status_t vps = lagfx_vk_cmd_set_region(lagfx_a64_res_cmdbuf(r->x), &ext, &r->region);
        if (vps != LAGFX_VP_OK) {
            CNT(p).viewport_degenerate++;
            const uint64_t n = vps == LAGFX_VP_EMPTY_METAL ? ++CNT(p).viewport_empty_metal : ++CNT(p).viewport_inexpressible;
            if ((n & (n - 1u)) == 0u)   /* 1, 2, 4, 8 ...: the split is not part of the periodic g7-gpu-ds line (diag.c prints the sum as viewport_degenerate) */
                LAGFX_LOG("g7-gpu-vp: %s draws skipped=%llu (viewport x=%g y=%g w=%g h=%g, target %ux%u; limits dim=%g bounds=[%g,%g])", vps == LAGFX_VP_EMPTY_METAL ? "viewport_empty_metal" : "viewport_inexpressible", (unsigned long long)n,
                          r->region.m_vp[0], r->region.m_vp[1], r->region.m_vp[2], r->region.m_vp[3], r->rt_w, r->rt_h, (double)r->region.lim_dim, (double)r->region.lim_lo, (double)r->region.lim_hi);
        }
    }
    if (r->pipe->src.desc.n_vtx_inputs) {
        uint32_t s = r->pipe->src.vertex_buffer_index;
        if (s >= A64_MAX_VB || !r->vb[s].b || r->vb[s].off >= r->vb[s].b->length) { CNT(p).no_vertex_buf++; CNT(p).draws_skipped++; return false; }
        lagfx_translate_render_bind_vertex_buffer(&r->rs, r->vb[s].b->buf, r->vb[s].off);
    }
    return true;
}

/* Framebuffer fetch WITHOUT rasterization-order attachment access (the device lacks VK_EXT/ARM_rasterization_order_attachment_access, or LAGFX_VK_NO_RASTER_ORDER=1).
 * Metal's fetch is ordered: a fragment reads what the previous fragment of the same pixel wrote, also inside ONE draw (primitives overlap, instances overlap). Vulkan only gives
 * that with the extension ("from one fragment to the next, in rasterization order, without explicit synchronization"); without it a read of a pixel another primitive of the same
 * draw writes is a data race, and a barrier is only possible BETWEEN draws. So the draw is recorded one primitive at a time (instance-major, then primitive order = the Vulkan
 * primitive order of the Primitive Order section) with the by-region barrier between consecutive sub-draws. Capped (A64_FBF_SPLIT_MAX sub-draws); over the cap, or with a topology /
 * primitives, the draw is REFUSED (counted fbf_split_capped, draws_skipped): recording it whole would make overlapping fragments race, i.e. silently wrong. On a device below Vulkan 1.3 a strip is split
 * WITHOUT the front-face flip: that path never culls (static cull NONE) and has no stencil / FrontFacing use, so facing cannot change coverage (fbf_split_unsupported is no longer counted). Triangle strips: triangle i is {v_i, v_i+1+(i%2),
 * v_i+2-(i%2)}; a 3-vertex strip draw from v_i is that triangle with the opposite winding for odd i, so the front face is flipped for those sub-draws (provoking vertex v_i is the
 * same). CONSTRAINT: every sub-draw is its own draw, so gl_PrimitiveID restarts at 0 in each (and gl_VertexIndex / InstanceIndex are re-based by the offsets below). The translator emits no
 * PrimitiveID today (grep src); if it ever does, this split must carry the original id (e.g. through a push constant) or refuse such pipelines. Applies to every device that lacks rasterization order.
 * Instance built-ins: the translator emits no InstanceId (grep), InstanceIndex = firstInstance + n is kept by firstInstance = base_instance + n. */
#define A64_FBF_SPLIT_MAX 16384ull
#define A64_PRE13_APPLIES_RASTER_STATE 0   /* set to 1 when apply_encoder_state starts applying cull / front face / stencil without core 1.3 */
/* 0 = no split needed, 1 = split, -1 = needs a split that is over the cap: refuse the draw. */
static int fbf_split_plan(a64_run_t *r, lagfx_protocol_t *p, const lagfx_a64_draw_t *d, uint64_t *nprim, uint32_t *nv, uint32_t *step) {
    if (lagfx_vk_raster_order(r->vk)) return 0;   /* the legacy backend never has it: overlapping primitives of a fetch draw are split */
    bool fetch = false;
    for (uint32_t i = 0; i < r->pipe->src.n_bind; i++) fetch |= r->pipe->src.bind[i].kind == LAGFX_A64_BIND_INPUT;
    if (!fetch) return 0;
    uint64_t n = d->count;
    switch (d->prim) {
    case 0: *nprim = n, *nv = 1, *step = 1; break;
    case 1: *nprim = n / 2u, *nv = 2, *step = 2; break;
    case 2: *nprim = n >= 2u ? n - 1u : 0u, *nv = 2, *step = 1; break;
    case 3: *nprim = n / 3u, *nv = 3, *step = 3; break;
    default: *nprim = n >= 3u ? n - 2u : 0u, *nv = 3, *step = 1; break;
    }
    if (*nprim * d->instances <= 1u) return 0;   /* one primitive: nothing can overlap inside the draw */
    /* Below Vulkan 1.3 the front-face flip of odd strip triangles is not available, and not needed ONLY while that path applies no culling and no stencil (apply_encoder_state returns early:
     * pipelines keep the static cull NONE, depth / stencil attachments are refused). Checked, not assumed: a pass that does carry a depth / stencil attachment, or a future change that applies
     * cull / stencil state without core 1.3, must not silently take this branch - the strip is refused (fbf_split_unsupported) until a front-face pipeline variant exists. */
    if (d->prim == 4u && *nprim > 1u && !vk_core13(r->vk) && (r->dsobj || A64_PRE13_APPLIES_RASTER_STATE)) { p->diag2.exec.fmt.fbf_split_unsupported++; return -1; }
    if (*nprim * d->instances > A64_FBF_SPLIT_MAX) { p->diag2.exec.fmt.fbf_split_capped++; return -1; }
    return 1;
}

static lagfx_status_t fbf_draw_split(a64_run_t *r, lagfx_protocol_t *p, const lagfx_a64_draw_t *d, uint32_t index_start, uint64_t nprim, uint32_t nv, uint32_t step) {
    VkCommandBuffer cb = lagfx_a64_res_cmdbuf(r->x);
    const VkFrontFace base = r->es.winding == 1u ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    bool flipped = false, first = true;
    lagfx_status_t st = LAGFX_OK;
    p->diag2.exec.fmt.fbf_split_draws++;
    for (uint64_t inst = 0; inst < d->instances && st == LAGFX_OK; inst++)
        for (uint64_t k = 0; k < nprim && st == LAGFX_OK; k++) {
            if (!first) fetch_barrier(r);
            first = false;
            const bool odd = d->prim == 4u && (k & 1u);
            if (odd != flipped && vk_core13(r->vk)) vkCmdSetFrontFace(cb, odd ? (base == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE) : base), flipped = odd;
            const uint32_t off = (uint32_t)(k * step), fi = (uint32_t)d->base_instance + (uint32_t)inst;
            if (p->diag2.exec.fmt.fbf_split_subdraws++ == 0u || k + inst == 0u) {   /* the first sub-draw of the draw goes through the encoder (dummy vertex buffer / log); the rest record directly: no per-sub-draw log line */
                st = d->indexed ? lagfx_translate_render_draw_indexed(&r->rs, nv, 1u, index_start + off, (int32_t)d->base_vertex, fi)
                                : lagfx_translate_render_draw(&r->rs, nv, 1u, (uint32_t)d->first_vertex + off, fi);
            } else if (d->indexed) vkCmdDrawIndexed(cb, nv, 1u, index_start + off, (int32_t)d->base_vertex, fi);
            else vkCmdDraw(cb, nv, 1u, (uint32_t)d->first_vertex + off, fi);
        }
    if (flipped) vkCmdSetFrontFace(cb, base);
    return st;
}

/* Every draw record (tags 0..11 direct, 0x10 / 0x11 indirect; layouts in arm64_draw_decode.h). Indirect draws read their argument block from the mirror of
 * the argument buffer at record time (a snapshot, as the independent reference does: it does not see GPU writes of the same exec). A draw with zero vertices /
 * indices or zero instances draws nothing and is done, not skipped. */
typedef struct { bool have; lagfx_a64_draw_t d; uint32_t index_start; lagfx_a64_obj_t *ib; } cap_dinfo_t;   /* what draw_cmd_inner resolved (indirect arguments included) */

static void draw_cmd_inner(a64_run_t *r, lagfx_protocol_t *p, uint32_t tag, const uint8_t *rec, uint32_t n, cap_dinfo_t *di) {
    lagfx_a64_draw_t d;
    r->exec_draw++;   /* 1-based index of this draw record in the exec (diagnostics) */
    if (lagfx_a64_draw_decode(tag, rec, n, &d) != LAGFX_A64_DRAW_OK) { CNT(p).bad_rec++; tag_unsup(p, tag); return; }
    if (!draw_ready(r, p)) { tag_partial(p, tag); return; }
    uint32_t index_start = 0;
    if (d.indirect) {
        lagfx_a64_obj_t *ab = lagfx_a64_res_object(r->x, d.indirect_ref);
        uint32_t alen = d.indexed ? LAGFX_A64_DRAW_INDEXED_ARGS_LEN : LAGFX_A64_DRAW_UNINDEXED_ARGS_LEN;
        if (!ab || ab->kind != LAGFX_A64_BUFFER || !ab->map || d.indirect_offset > ab->length || ab->length - d.indirect_offset < alen ||
            !lagfx_a64_draw_apply_indirect(&d, ab->map + d.indirect_offset, alen, &index_start)) {
            CNT(p).ext_draw_indirect_bad++, CNT(p).draws_skipped++; tag_partial(p, tag); return;
        }
        CNT(p).ext_draw_indirect++;
    }
    if (d.prim > 4u) { CNT(p).prim_unsupported++; CNT(p).draws_skipped++; tag_partial(p, tag); return; }
    if (!d.count || !d.instances) { CNT(p).ext_draw_zero++; tag_done(p, tag); return; }   /* Metal draws nothing */
    if (d.count > 0xffffffffull || d.instances > 0xffffffffull || d.first_vertex > 0xffffffffull || d.base_instance > 0xffffffffull ||
        d.base_vertex > 0x7fffffffll || d.base_vertex < -0x7fffffffll - 1) { CNT(p).prim_unsupported++; CNT(p).draws_skipped++; tag_partial(p, tag); return; }
    if (d.instances > 1u) CNT(p).ext_draw_instanced++;
    if (d.base_instance || d.base_vertex) CNT(p).ext_draw_base++;
    if (tag < 12u && !(tag & 1u)) CNT(p).ext_draw_wide++;
    lagfx_a64_obj_t *ib = NULL;
    uint32_t isz = d.index_type == 1u ? 4u : 2u;
    if (d.indexed) {
        ib = lagfx_a64_res_object(r->x, d.index_ref);
        uint64_t base = d.index_offset;   /* bytes; the indirect form adds its index start (in elements) on top, as the first index of the draw */
        if (d.index_type > 1u || !ib || ib->kind != LAGFX_A64_BUFFER || base % isz || base > ib->length || ((uint64_t)index_start + d.count) * isz > ib->length - base) {
            CNT(p).no_index_buf++, CNT(p).draws_skipped++; tag_partial(p, tag); return;
        }
    }
    if (di) di->have = true, di->d = d, di->index_start = index_start, di->ib = ib;
    if (!bind_for_draw(r, p, d.prim)) { tag_partial(p, tag); return; }
    lagfx_status_t st;
    uint64_t sp_n = 0; uint32_t sp_nv = 0, sp_step = 0;
    const int plan = fbf_split_plan(r, p, &d, &sp_n, &sp_nv, &sp_step);
    if (plan < 0) { CNT(p).draws_skipped++; tag_partial(p, tag); return; }   /* over the split cap: refused and counted, never run unordered */
    const bool split = plan > 0;
    if (d.indexed) lagfx_translate_render_bind_index_buffer(&r->rs, ib->buf, d.index_offset, d.index_type == 1u ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    if (split) st = fbf_draw_split(r, p, &d, index_start, sp_n, sp_nv, sp_step);
    else if (d.indexed) st = lagfx_translate_render_draw_indexed(&r->rs, (uint32_t)d.count, (uint32_t)d.instances, index_start, (int32_t)d.base_vertex, (uint32_t)d.base_instance);
    else st = lagfx_translate_render_draw(&r->rs, (uint32_t)d.count, (uint32_t)d.instances, (uint32_t)d.first_vertex, (uint32_t)d.base_instance);
    if (st != LAGFX_OK) { CNT(p).draws_skipped++; tag_partial(p, tag); return; }
    CNT(p).draws++;
    if (r->ms.samples > 1u) p->diag2.exec.msaa.draws_ms++;
    tag_done(p, tag);
}


/* ---- render replay capture of one draw (arm64_capture.c; nothing below runs unless r->cap) ---- */

static const char *cap_skip_reason(const a64_run_t *r, lagfx_protocol_t *p, const uint64_t *before) {
    static const char *const nm[] = { "no_pipeline", "no_target", "no_vertex_buf", "no_index_buf", "prim_unsupported", "indirect_bad", "desc_unbound", "desc_unaligned", "desc_feedback", "desc_failed", "point_no_size", "stub_fbfetch", "unnorm_skipped" };
    uint64_t now[13] = { CNT(p).no_pipeline, CNT(p).no_target, CNT(p).no_vertex_buf, CNT(p).no_index_buf, CNT(p).prim_unsupported, CNT(p).ext_draw_indirect_bad, p->diag2.exec.pipe.desc_unbound,
                         p->diag2.exec.pipe.desc_unaligned, p->diag2.exec.pipe.desc_feedback, p->diag2.exec.pipe.desc_failed, p->diag2.exec.pipe.point_no_size, p->diag2.exec.pipe.stub_fbfetch, CNT(p).ext_unnorm_skipped };
    (void)r;
    for (unsigned i = 0; i < 13; i++) if (now[i] != before[i]) return nm[i];
    return "other";
}

static void cap_counters(lagfx_protocol_t *p, uint64_t *v) {
    v[0] = CNT(p).no_pipeline, v[1] = CNT(p).no_target, v[2] = CNT(p).no_vertex_buf, v[3] = CNT(p).no_index_buf, v[4] = CNT(p).prim_unsupported, v[5] = CNT(p).ext_draw_indirect_bad, v[6] = p->diag2.exec.pipe.desc_unbound,
    v[7] = p->diag2.exec.pipe.desc_unaligned, v[8] = p->diag2.exec.pipe.desc_feedback, v[9] = p->diag2.exec.pipe.desc_failed, v[10] = p->diag2.exec.pipe.point_no_size, v[11] = p->diag2.exec.pipe.stub_fbfetch, v[12] = CNT(p).ext_unnorm_skipped;
}

static void cap_fill(a64_run_t *r, const cap_dinfo_t *di, lagfx_a64_cap_draw_in_t *o) {
    memset(o, 0, sizeof *o);
    const lagfx_a64_draw_t *d = &di->d;
    o->tag = 0, o->prim = d->prim, o->indexed = d->indexed, o->count = d->count, o->instances = d->instances, o->first_vertex = d->first_vertex, o->base_instance = d->base_instance, o->base_vertex = d->base_vertex;
    o->index_start = di->index_start, o->index_type = d->index_type, o->index_off = d->index_offset, o->ib = di->ib;
    for (int i = 0; i < 4; i++) o->vp[i] = r->region.m_vp_set ? (float)r->region.m_vp[i] : 0.0f, o->sc[i] = r->region.m_sc_set ? (uint32_t)(r->region.m_sc[i] > 0xffffffffull ? 0xffffffffull : r->region.m_sc[i]) : 0u;
    o->vp[4] = r->region.vp_znear, o->vp[5] = r->region.vp_zfar;
    o->cull = r->es.cull, o->winding = r->es.winding, memcpy(o->bias, r->es.bias, sizeof o->bias);
    o->depth_test = lagfx_a64_dss_depth_test(&r->es.dss), o->depth_write = r->es.dss.depth_write, o->depth_cmp = r->es.dss.depth_compare, o->stencil_test = lagfx_a64_dss_stencil_test(&r->es.dss);
    o->sref[0] = r->es.sref[0], o->sref[1] = r->es.sref[1];
    for (uint32_t i = 0; i < 4u && i < r->nslots; i++) if (r->rts[i]) o->color_fmts[i] = (uint32_t)r->rts[i]->format;
    o->ncolor_fmts = r->nslots;
    a64_pipe_t *pp = r->pipe;
    if (!pp) return;
    const lagfx_pipeline_desc_t *pd = &pp->src.desc;
    o->pipe_ref = pp->ref, o->pipe_gen = pp->h.gen, o->pipe_hash = pp->src.blob_hash;
    o->vs = pd->vertex_spv, o->vs_bytes = pd->vertex_spv_bytes, o->fs = pd->fragment_spv, o->fs_bytes = pd->fragment_spv_bytes;
    o->vtx_slot = pp->src.vertex_buffer_index, o->vtx_stride = pd->vtx_stride;
    if (pd->n_vtx_inputs && o->vtx_slot < A64_MAX_VB) o->vb = r->vb[o->vtx_slot].b, o->vb_off = r->vb[o->vtx_slot].off;
    o->n_vtx_in = pd->n_vtx_inputs < 8u ? pd->n_vtx_inputs : 8u;
    for (uint32_t i = 0; i < o->n_vtx_in; i++) o->vtx_in_loc[i] = pd->vtx_in_loc[i], o->vtx_in_comp[i] = pd->vtx_in_comp[i];
    o->n_attr = pd->n_pso_attrs < 8u ? pd->n_pso_attrs : 8u;
    for (uint32_t i = 0; i < o->n_attr; i++) o->attr_fmt[i] = pd->pso_attr_fmt[i], o->attr_off[i] = pd->pso_attr_off[i];
    o->blend_set = pd->blend_set, o->blend = pd->blend_enable, o->blend_rgb[0] = pd->blend_src_rgb, o->blend_rgb[1] = pd->blend_dst_rgb, o->blend_rgb[2] = pd->blend_op_rgb;
    o->blend_a[0] = pd->blend_src_a, o->blend_a[1] = pd->blend_dst_a, o->blend_a[2] = pd->blend_op_a, o->write_mask = pd->color_write_mask;
    for (uint32_t i = 0; i < pp->src.n_bind && i < LAGFX_A64_MAX_BINDINGS; i++) {
        lagfx_a64_cap_bind_in_t *b = &o->binds[o->nbind++];
        int16_t mis = pp->src.bind[i].metal_index;
        uint32_t mi = (uint32_t)mis, st = pp->src.bind[i].stage;
        b->binding = pp->src.bind[i].binding, b->kind = pp->src.bind[i].kind, b->stage = st, b->idx = mi, b->writable = pp->src.bind[i].writable;
        if (mis < 0) continue;
        if (b->kind == LAGFX_A64_BIND_BUFFER) {
            if (st ? mi < A64_MAX_FB : mi < A64_MAX_VB) {
                b->obj = st ? r->fb[mi].b : r->vb[mi].b, b->off = st ? r->fb[mi].off : r->vb[mi].off;
                if (b->obj) b->range = b->off < b->obj->length ? b->obj->length - b->off : 0;
            }
        } else if (b->kind == LAGFX_A64_BIND_IMAGE) {
            if (mi < A64_MAX_FB) b->obj = st ? r->ftex[mi] : r->vtex[mi];
        } else if (b->kind == LAGFX_A64_BIND_SAMPLER) {
            if (mi >= A64_MAX_FSAMP) continue;
            a64_samp_t *sm = st ? r->fsamp[mi] : r->vsamp[mi];
            VkSampler ov = (st ? r->fvar : r->vvar)[mi];
            if (!sm) continue;
            b->has_samp = true, b->samp.ref = (st ? r->fsref : r->vsref)[mi], b->samp.gen = sm->h.gen, b->samp.ci = sm->ci, b->samp.mip = sm->mip, b->samp.lod_lo = sm->ci.minLod, b->samp.lod_hi = sm->ci.maxLod;
            for (uint32_t k = 0; ov && k < sm->nvar; k++) if (sm->var[k].s == ov) { b->samp.lod_lo = sm->var[k].lo, b->samp.lod_hi = sm->var[k].hi; break; }
        }
    }
}

/* Per-draw diagnostics (draw_diag.h): counters before / after one draw classify its outcome; the event goes into the bounded table, nothing is formatted here. */
enum { DD_C_DRAWS, DD_C_SKIPPED, DD_C_NOPIPE, DD_C_UB_BUF, DD_C_UB_IN, DD_C_UB_SI, DD_C_UB_TEX, DD_C_UB_VIEW, DD_C_UB_SAMP, DD_C_NOTGT, DD_C_NOVB, DD_C_NOIB, DD_C_PRIM, DD_C_INDBAD, DD_C_UNAL, DD_C_FB, DD_C_DFAIL,
       DD_C_NOSIZE, DD_C_FBF, DD_C_UNNORM, DD_C_ZERO, DD_C_BADREC, DD_C_TILE, DD_C_N };
_Static_assert(LAGFX_LR__COUNT <= 12, "state.h legacy_pipe_why[] / legacy_begin_why[] hold LAGFX_LR__COUNT entries");
_Static_assert(DD_C_N == LAGFX_DD_NCTR, "draw_diag.c ctr_name[] must match the DD_C_* enum");
static void dd_counters(lagfx_protocol_t *p, uint64_t *v) {
    const __typeof__(p->diag2.exec.pipe) *Q = &p->diag2.exec.pipe;
    v[DD_C_DRAWS] = CNT(p).draws, v[DD_C_SKIPPED] = CNT(p).draws_skipped, v[DD_C_NOPIPE] = CNT(p).no_pipeline, v[DD_C_UB_BUF] = Q->desc_unbound_why[0], v[DD_C_UB_IN] = Q->desc_unbound_why[1],
    v[DD_C_UB_SI] = Q->desc_unbound_why[2], v[DD_C_UB_TEX] = Q->desc_unbound_why[3], v[DD_C_UB_VIEW] = Q->desc_unbound_why[4], v[DD_C_UB_SAMP] = Q->desc_unbound_why[5], v[DD_C_NOTGT] = CNT(p).no_target,
    v[DD_C_NOVB] = CNT(p).no_vertex_buf, v[DD_C_NOIB] = CNT(p).no_index_buf, v[DD_C_PRIM] = CNT(p).prim_unsupported, v[DD_C_INDBAD] = CNT(p).ext_draw_indirect_bad, v[DD_C_UNAL] = Q->desc_unaligned,
    v[DD_C_FB] = Q->desc_feedback, v[DD_C_DFAIL] = Q->desc_failed, v[DD_C_NOSIZE] = Q->point_no_size, v[DD_C_FBF] = Q->stub_fbfetch, v[DD_C_UNNORM] = CNT(p).ext_unnorm_skipped, v[DD_C_ZERO] = CNT(p).ext_draw_zero,
    v[DD_C_BADREC] = CNT(p).bad_rec, v[DD_C_TILE] = p->diag2.exec.tile.draws_ignored;
}
static void dd_report(a64_run_t *r, lagfx_protocol_t *p, struct lagfx_draw_diag *dd, const uint64_t *b) {
    uint64_t a[DD_C_N]; dd_counters(p, a);
    lagfx_draw_ev_t ev; memset(&ev, 0, sizeof ev);
    ev.task = r->task ? r->task->id : 0, ev.draw = r->exec_draw, ev.pass = CNT(p).passes, ev.slot = -1;
    if (r->pipe) ev.pso = r->pipe->ref, ev.vs = (uint32_t)(r->pipe->src.vs_hash >> 32), ev.fs = (uint32_t)(r->pipe->src.fs_hash >> 32);
    if (r->rts[0]) ev.rt_w = r->rts[0]->width, ev.rt_h = r->rts[0]->height, ev.rt_fmt = (uint32_t)r->rts[0]->format;
#define CH(i) (a[i] != b[i])
    if (CH(DD_C_DRAWS)) ev.reason = LAGFX_DD_OK;
    else if (CH(DD_C_NOPIPE)) ev.reason = LAGFX_DD_NO_PIPELINE, ev.why = !r->pipe_set ? "none_set" : r->pipe_failed ? "unresolved" : "vk_build_or_bind";
    else if (CH(DD_C_UB_BUF) || CH(DD_C_UB_TEX) || CH(DD_C_UB_VIEW) || CH(DD_C_UB_IN) || CH(DD_C_UB_SI) || CH(DD_C_UB_SAMP)) {
        ev.reason = CH(DD_C_UB_BUF) ? LAGFX_DD_UNBOUND_BUF : (CH(DD_C_UB_SAMP) && !CH(DD_C_UB_TEX) && !CH(DD_C_UB_VIEW)) ? LAGFX_DD_OTHER : LAGFX_DD_UNBOUND_TEX;
        if (ev.reason == LAGFX_DD_OTHER) ev.why = "unbound_sampler";
        ev.bkind = r->ddm.bkind, ev.stage = r->ddm.stage, ev.slot = r->ddm.slot, ev.vkb = r->ddm.vkb, ev.bref = r->ddm.ref;
    } else {
        static const struct { int c; const char *n; } nm[] = { { DD_C_NOTGT, "no_target" }, { DD_C_NOVB, "no_vertex_buf" }, { DD_C_NOIB, "no_index_buf" }, { DD_C_PRIM, "prim_unsupported" }, { DD_C_INDBAD, "indirect_bad" },
            { DD_C_UNAL, "desc_unaligned" }, { DD_C_FB, "desc_feedback" }, { DD_C_DFAIL, "desc_failed" }, { DD_C_NOSIZE, "point_no_size" }, { DD_C_FBF, "stub_fbfetch" }, { DD_C_UNNORM, "unnorm_skipped" },
            { DD_C_ZERO, "zero_draw" }, { DD_C_BADREC, "bad_rec" }, { DD_C_TILE, "tile_pipeline" } };
        ev.reason = LAGFX_DD_OTHER, ev.why = "unclassified";
        for (unsigned i = 0; i < sizeof nm / sizeof *nm; i++) if (CH(nm[i].c)) { ev.why = nm[i].n; break; }
    }
    for (unsigned i = 2; i < DD_C_N; i++) if (CH(i)) ev.chg |= 1u << i;   /* every skip counter that moved (draws / skipped excluded) */
#undef CH
    lagfx_draw_diag_record(dd, &ev);
}

static void draw_cmd_dd(a64_run_t *r, lagfx_protocol_t *p, uint32_t tag, const uint8_t *rec, uint32_t n);
static void draw_cmd_plain(a64_run_t *r, lagfx_protocol_t *p, uint32_t tag, const uint8_t *rec, uint32_t n);
static void draw_cmd(a64_run_t *r, lagfx_protocol_t *p, uint32_t tag, const uint8_t *rec, uint32_t n) {
    if (lagfx_draw_diag_get(p)) { draw_cmd_dd(r, p, tag, rec, n); return; }   /* disabled (G7_DRAW_DIAG=0): this one load and branch is all that runs */
    draw_cmd_plain(r, p, tag, rec, n);
}
static void draw_cmd_plain(a64_run_t *r, lagfx_protocol_t *p, uint32_t tag, const uint8_t *rec, uint32_t n) {
    if (!r->cap) { draw_cmd_inner(r, p, tag, rec, n, NULL); return; }
    bool sel = false;
    uint32_t dn = lagfx_a64_cap_draw_enter(r->cap, r->pipe ? r->pipe->ref : 0u, &sel);
    if (!sel) { draw_cmd_inner(r, p, tag, rec, n, NULL); return; }
    uint64_t c0[13], d0 = CNT(p).draws, s0 = CNT(p).draws_skipped;
    cap_counters(p, c0);
    cap_dinfo_t di;
    memset(&di, 0, sizeof di);
    draw_cmd_inner(r, p, tag, rec, n, &di);
    if (!di.have) return;   /* not decodable / zero-sized: no record */
    lagfx_a64_cap_draw_in_t *in = malloc(sizeof *in);
    if (!in) return;
    cap_fill(r, &di, in);
    in->tag = tag;
    if (CNT(p).draws != d0) lagfx_a64_cap_draw(r->cap, dn, NULL, in);
    else if (CNT(p).draws_skipped != s0) lagfx_a64_cap_draw(r->cap, dn, cap_skip_reason(r, p, c0), in);
    free(in);
}

static void draw_cmd_dd(a64_run_t *r, lagfx_protocol_t *p, uint32_t tag, const uint8_t *rec, uint32_t n) {
    uint64_t b[DD_C_N]; dd_counters(p, b);
    memset(&r->ddm, 0, sizeof r->ddm), r->ddm.slot = -1;
    draw_cmd_plain(r, p, tag, rec, n);
    dd_report(r, p, lagfx_draw_diag_get(p), b);
}

/* Tile records (arm64_tile.c). A dispatch cannot run inside a dynamic-rendering instance: tile_suspend ends it (attachments -> GENERAL), the compute dispatch is
 * recorded by arm64_tile.c, tile_resume begins the same instance again with the original attachments but LOAD ops, so their contents are preserved. */
static void tile_suspend(void *c) {
    a64_run_t *r = c;
    lagfx_translate_render_end(&r->rs);
    for (uint32_t i = 0; i < r->nslots && i < 4u; i++) if (r->rts[i]) lagfx_a64_res_target_end(r->x, r->rts[i]);
    if (r->dsobj) lagfx_a64_res_ds_end(r->x, r->dsobj);
    r->rendering = false;
}
static bool tile_resume(void *c) {
    a64_run_t *r = c;
    VkRenderingAttachmentInfo ca[4], dai = r->rsnap.dai, sai = r->rsnap.sai;
    memcpy(ca, r->rsnap.ca, sizeof ca);
    uint32_t begun = 0;
    bool ok = true;
    for (uint32_t i = 0; ok && i < 4u; i++) {
        if (!ca[i].imageView || !r->rts[i]) continue;
        VkImageView v; uint32_t w, h;
        if (!lagfx_a64_res_target_begin(r->x, r->rts[i], &v, &w, &h)) { ok = false; break; }
        ca[i].imageView = v, ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, begun |= 1u << i;
    }
    bool dbegun = false;
    if (ok && r->dsobj) {
        VkImageView v; uint32_t w, h;
        if (lagfx_a64_res_ds_begin(r->x, r->dsobj, &v, &w, &h)) { dbegun = true, dai.imageView = sai.imageView = v, dai.loadOp = sai.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; }
        else ok = false;
    }
    VkFormat cfmt[4] = { VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED };
    for (uint32_t q = 0; q < 4u; q++) if (r->rts[q]) cfmt[q] = r->rts[q]->format;
    r->rs.legacy_refuse = 0;
    if (ok && lagfx_translate_render_begin_fmt(r->vk, &r->rs, lagfx_a64_res_cmdbuf(r->x), r->rsnap.img, r->rt_w, r->rt_h, ca, r->nslots,
                                               r->rsnap.has_d ? &dai : NULL, r->rsnap.has_s ? &sai : NULL,
                                               cfmt, r->dsobj ? r->dsobj->format : VK_FORMAT_UNDEFINED, r->ms.samples > 1u ? r->ms.samples : 1u) == LAGFX_OK) {
        r->rendering = true, r->bound_vk = VK_NULL_HANDLE, r->last_fetch = false, r->es.dirty = true;
        return true;
    }
    if (r->rs.legacy_refuse && r->rs.legacy_refuse < LAGFX_LR__COUNT) r->p->diag2.exec.pipe.legacy_begin_why[r->rs.legacy_refuse]++;
    for (uint32_t i = 0; i < 4u; i++) if (begun & (1u << i)) lagfx_a64_res_target_end(r->x, r->rts[i]);
    if (dbegun) lagfx_a64_res_ds_end(r->x, r->dsobj);
    return false;   /* the pass goes on without rendering: its draws are counted as no_target */
}
static void tile_tag(a64_run_t *r, lagfx_protocol_t *p, uint32_t tag, const uint8_t *rec, uint32_t n) {
    if (r->ms.samples > 1u) { p->diag2.exec.msaa.tile_ms_refused++; tag_partial(p, tag); return; }   /* a tile dispatch suspends and resumes the pass: not modelled with a resolve / multisampled attachments */
    if (!r->tile) r->tile = lagfx_a64_tile_new(r->vk);
    if (!r->tile) { tag_unsup(p, tag); return; }
    if (tag == LAGFX_A64_TAG_TILE_DISPATCH || tag == LAGFX_A64_TAG_TILE_REGION || tag == LAGFX_A64_TAG_TILE_REGION_RT) {
        lagfx_a64_tile_disp_t d;
        if (!lagfx_a64_tile_decode_dispatch(tag, rec, n, &d)) { CNT(p).bad_rec++; p->diag2.exec.tile.skip_bad_rec++; tag_unsup(p, tag); return; }
        if (!r->rendering) { p->diag2.exec.tile.skip_no_pass++; tag_partial(p, tag); return; }
        if (!r->pipe_set || !r->pipe || !r->pipe->src.tile) { p->diag2.exec.tile.skip_no_pipe++; tag_partial(p, tag); return; }
        lagfx_a64_tile_env_t e = { .p = p, .x = r->x, .nslots = r->nslots, .tgt_w = r->rt_w, .tgt_h = r->rt_h, .ctx = r, .suspend = tile_suspend, .resume = tile_resume };
        memcpy(e.rts, r->rts, sizeof e.rts);
        if (lagfx_a64_tile_dispatch(r->tile, r->pipe->src.tile, &e, &d)) tag_done(p, tag); else tag_partial(p, tag);
        return;
    }
    int rc = lagfx_a64_tile_bind_tag(r->tile, p, r->x, r->task->id, tag, rec, n);
    if (rc == 0) tag_done(p, tag); else if (rc == 1) tag_partial(p, tag); else { CNT(p).bad_rec++; tag_unsup(p, tag); }
}

/* One supported tag, already inside an open pass. Reached from the existing render_inner_ops.c handlers through lagfx_a64_exec_render_op. */
static void render_tag(a64_run_t *r, lagfx_protocol_t *p, uint32_t tag, const uint8_t *rec, uint32_t n) {
    switch (tag) {
    case 26: begin_pass(r, p, rec, n); break;
    case 116: set_pipeline(r, p, rec, n); break;
    case 125: case 110: {                    /* {index, count}, {ref, offset u64}... */
        if (n < 8u) { CNT(p).bad_rec++; tag_unsup(p, tag); break; }
        uint32_t index = R32(0), count = R32(4);
        if (count > A64_MAX_VB || (uint64_t)n < 8u + 12ull * count) { CNT(p).bad_rec++; tag_unsup(p, tag); break; }
        for (uint32_t i = 0; i < count && index + i < A64_MAX_VB; i++) {
            uint32_t bref = R32(8 + i * 12);
            lagfx_a64_obj_t *b = lagfx_a64_res_object(r->x, bref);
            uint32_t bwhy = b ? (b->kind != LAGFX_A64_BUFFER ? 2u : 3u) : 1u, e3[3] = { 0, 0, 0 };
            if (b && b->kind != LAGFX_A64_BUFFER) b = NULL;
            if (!b) (void)lagfx_a64_objects_read_entry(p, r->task, bref, e3);
            if (tag == 125 && index + i < A64_MAX_VB) r->vbmiss[index + i].ref = bref, r->vbmiss[index + i].why = b ? 3u : bwhy, r->vbmiss[index + i].type = e3[0] & 0xffu;
            else if (tag == 110 && index + i < A64_MAX_FB) r->fbmiss[index + i].ref = bref, r->fbmiss[index + i].why = b ? 3u : bwhy, r->fbmiss[index + i].type = e3[0] & 0xffu;
            uint64_t off = lagfx_le64(rec + 12 + i * 12);
            if (tag == 125) r->vb[index + i].b = b, r->vb[index + i].off = off;
            else if (index + i < A64_MAX_FB) r->fb[index + i].b = b, r->fb[index + i].off = off;
        }
        tag_done(p, tag);
        break;
    }
    case 126: case 111:                      /* {index, offset u64} */
        if (n < 12u) { CNT(p).bad_rec++; tag_unsup(p, tag); break; }
        {
            uint32_t idx = R32(0);
            if (tag == 126 && idx < A64_MAX_VB) r->vb[idx].off = lagfx_le64(rec + 4);
            if (tag == 111 && idx < A64_MAX_FB) r->fb[idx].off = lagfx_le64(rec + 4);
            tag_done(p, tag);
        }
        break;
    case 114: case 112: case 129: case 127: {   /* {index, count}, refs: fragment (114 / 112) and vertex (129 / 127) textures / samplers */
        if (n < 8u) { CNT(p).bad_rec++; tag_unsup(p, tag); break; }
        const bool vs = tag == 129u || tag == 127u, tex = tag == 114u || tag == 129u;
        lagfx_a64_obj_t **tt = vs ? r->vtex : r->ftex; a64_samp_t **ss = vs ? r->vsamp : r->fsamp; VkSampler *vv = vs ? r->vvar : r->fvar;
        uint32_t index = R32(0), count = R32(4), lim = tex ? A64_MAX_FB : A64_MAX_FSAMP;
        if (count > lim || (uint64_t)n < 8u + 4ull * count) {
            if (tex) for (uint32_t i = 0; i < count && index + i < lim; i++) { a64_texmiss_t *mm = &(vs ? r->vmiss : r->fmiss)[index + i]; memset(mm, 0, sizeof *mm), mm->dg.why = TEXWHY_RECORD_BAD; }
            CNT(p).bad_rec++; tag_unsup(p, tag); break;
        }
        for (uint32_t i = 0; i < count && index + i < lim; i++) {
            if (tex) {   /* only textures the mirror already holds (the exec's resource list): creating + uploading one now would record copies inside the pass */
                uint32_t tref = R32(8 + i * 4);
                lagfx_a64_obj_t *t = lagfx_a64_res_lookup(p, r->task->id, tref);
                tt[index + i] = t && t->kind == LAGFX_A64_TEXTURE ? t : NULL;
                a64_texmiss_t *mm = &(vs ? r->vmiss : r->fmiss)[index + i];
                memset(mm, 0, sizeof *mm);
                if (!tt[index + i]) mm->ref = tref, (void)lagfx_a64_res_diagnose(p, r->task->id, tref, &mm->dg);   /* why not: counted per reason when a draw needs it */
            } else ss[index + i] = get_kind(r, p, r->task, R32(8 + i * 4), K_SAMP), vv[index + i] = VK_NULL_HANDLE, (vs ? r->vsref : r->fsref)[index + i] = R32(8 + i * 4);
        }
        if (vs) CNT(p).ext_vs_tex_binds++;
        tag_done(p, tag);
        break;
    }
    case 104: {                              /* setDepthStencilState {ref u32}: ref 0 = the default state (no depth test, no write, stencil off) */
        if (n < 4u) { CNT(p).bad_rec++; tag_unsup(p, tag); break; }
        uint32_t ref = R32(0);
        a64_dss_t *o = ref ? (a64_dss_t *)get_kind(r, p, r->task, ref, K_DSS) : NULL;
        lagfx_a64_dss_default(&r->es.dss);
        r->es.dirty = true;
        if (o) { r->es.dss = o->d; CNT(p).dss_bound++; tag_done(p, tag); }
        else if (!ref) tag_done(p, tag);
        else { CNT(p).dss_unresolved++; tag_partial(p, tag); }   /* not a decodable depth/stencil object: the default (no depth test) stays, as when no state is bound */
        break;
    }
    case 119:                                /* setStencilReferenceValue / setStencilFront:back: {front u32, back u32} (the one-value selector writes it twice) */
        if (n < 8u) { CNT(p).bad_rec++; tag_unsup(p, tag); break; }
        r->es.sref[0] = R32(0), r->es.sref[1] = R32(4), r->es.dirty = true;
        CNT(p).stencil_ref_set++;
        tag_done(p, tag);
        break;
    case 101: {                              /* setBlendColor {red f32, green f32, blue f32, alpha f32} (render_inner_ops.c 0x65 SetBlendColor, 16 bytes): the constant of the blend factors 11..14; read by the next draw of a pipeline with dynamic blend constants */
        if (n < 16u) { CNT(p).bad_rec++; tag_unsup(p, tag); break; }
        lagfx_a64_blend_color_decode(rec, r->es.blend_color);
        CNT(p).blend_color_set++;
        tag_done(p, tag);
        break;
    }
    case 107: case 115: {                    /* setCullMode (0 none, 1 front, 2 back) / setFrontFacingWinding (0 clockwise, 1 counter-clockwise): {mode u64} */
        if (n < 8u) { CNT(p).bad_rec++; tag_unsup(p, tag); break; }
        uint64_t m = lagfx_le64(rec);
        if (m > (tag == 107 ? 2u : 1u)) { tag_unsup(p, tag); break; }
        if (tag == 107) r->es.cull = (uint32_t)m, CNT(p).cull_set++; else r->es.winding = (uint32_t)m, CNT(p).winding_set++;
        r->es.dirty = true;
        tag_done(p, tag);
        break;
    }
    case 108: {                              /* setDepthBias:slopeScale:clamp: {bias f32, slope f32, clamp f32} */
        if (n < 12u) { CNT(p).bad_rec++; tag_unsup(p, tag); break; }
        for (int i = 0; i < 3; i++) { uint32_t u = R32(4 * i); float f; memcpy(&f, &u, 4); r->es.bias[i] = f == f ? f : 0.0f; }
        r->es.dirty = true;
        CNT(p).bias_set++;
        tag_done(p, tag);
        break;
    }
    case 117:                                /* MTLScissorRect: x, y, width, height as u64 */
        if (n < 32u) { CNT(p).bad_rec++; tag_unsup(p, tag); break; }
        for (int i = 0; i < 4; i++) r->region.m_sc[i] = lagfx_le64(rec + 8 * i);   /* kept whole: the clamp to the target happens at record time (lagfx_vk_region_to_vk) */
        r->region.m_sc_set = true;
        tag_done(p, tag);
        break;
    case 130: {                              /* MTLViewport: originX, originY, width, height, znear, zfar as doubles */
        if (n < 48u) { CNT(p).bad_rec++; tag_unsup(p, tag); break; }
        double x = rd_f64(rec), y = rd_f64(rec + 8), w = rd_f64(rec + 16), h = rd_f64(rec + 24);
        r->region.m_vp[0] = x, r->region.m_vp[1] = y, r->region.m_vp[2] = w, r->region.m_vp[3] = h, r->region.m_vp_set = true;   /* as recorded: no truncation, no clamp, no rescale (S05) */
        {   /* znear / zfar: the depth range the clip-space z is mapped to (Vulkan only takes [0, 1]; a value outside it is clamped, a NaN reads as the default) */
            double zn = rd_f64(rec + 32), zf = rd_f64(rec + 40);
            r->region.vp_znear = zn >= 1.0 ? 1.0f : zn > 0.0 ? (float)zn : 0.0f, r->region.vp_zfar = zf >= 1.0 ? 1.0f : zf > 0.0 ? (float)zf : (zf == zf ? 0.0f : 1.0f);
            r->region.vp_z_set = true;
            CNT(p).viewport_depth_set++;
        }
        tag_done(p, tag);
        break;
    }
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9: case 10: case 11: case 16: case 17:
        draw_cmd(r, p, tag, rec, n);
        break;
    case 113: case 128:                      /* {index, count}, {ref, lod min f32, lod max f32}...: fragment / vertex sampler with LOD clamps */
        if (n < 8u) { CNT(p).bad_rec++; tag_unsup(p, tag); break; }
        {
            uint32_t index = R32(0), count = R32(4);
            if (count > A64_MAX_FSAMP || (uint64_t)n < 8u + 12ull * count) { CNT(p).bad_rec++; tag_unsup(p, tag); break; }
            a64_samp_t **ss = tag == 128 ? r->vsamp : r->fsamp; VkSampler *vv = tag == 128 ? r->vvar : r->fvar;
            for (uint32_t i = 0; i < count && index + i < A64_MAX_FSAMP; i++) {
                a64_samp_t *sm = get_kind(r, p, r->task, R32(8 + i * 12), K_SAMP);
                ss[index + i] = sm, vv[index + i] = VK_NULL_HANDLE, (tag == 128 ? r->vsref : r->fsref)[index + i] = R32(8 + i * 12);
                if (!sm) continue;
                uint32_t lo = R32(12 + i * 12), hi = R32(16 + i * 12);
                float flo, fhi;
                memcpy(&flo, &lo, 4), memcpy(&fhi, &hi, 4);
                VkSampler v = sampler_variant(r, sm, flo, fhi);
                if (v != sm->s) vv[index + i] = v;
            }
            CNT(p).ext_samp_lod_binds++;
            tag_done(p, tag);
        }
        break;
    case 155: case 156: case 157: case 158: case 159: case 160: case 161: case 162: case 163: case 164:   /* tile shader binds / dispatches (arm64_tile.c) */
        tile_tag(r, p, tag, rec, n);
        break;
    case 137: case 27: case 134: case 135:   /* useResource / useHeap / useHeaps / useResources: residency hints, nothing to do (every mirror object is resident) */
        CNT(p).ext_noop_hint++;
        tag_done(p, tag);
        break;
    case 24: case 25:                        /* updateFence / waitForFence: the whole exec is one in-order command buffer; ordering between encoders is already given */
        CNT(p).ext_noop_fence++;
        tag_done(p, tag);
        break;
    case 22: case 23: case 133:              /* memoryBarrier(resources / scope), textureBarrier: not recorded (partial) */
        CNT(p).ext_barrier++;
        tag_partial(p, tag);
        break;
    default: tag_unsup(p, tag); break;
    }
}

/* Hook for the existing render_inner_ops.c handlers (same tag numbers as the x86 inner opcodes): returns true when the ARM64 executor is
 * recording an exec and took the op. In every other situation (x86, exec off, no exec running) it returns false at once and the handler
 * behaves exactly as before. */
bool lagfx_a64_exec_render_op(lagfx_protocol_t *p, uint32_t tag, const uint8_t *payload, size_t len) {
    a64_run_t *r = p ? RUN(p) : NULL;
    if (!r || !r->dispatching || !payload) return false;
    r->consumed = true;
    render_tag(r, p, tag, payload, (uint32_t)len);
    return true;
}

/* Textures a pass binds for sampling (tags 114 / 129 up to the next pass descriptor) that the exec's resource list did not name are created and made current NOW,
 * before the pass opens: creating one inside the pass would record copies into it. Whether the real guest lists the linear textures it samples is not known from the
 * device log (117 draws had type 8 textures with no mirror object and no creation failure, i.e. creation was never attempted); this makes the mirror independent of it.
 * Only records of the segment in hand are seen: a pass that continues into a later segment binds its textures there, after the pass opened (those stay as before). */
static void prescan_pass_textures(a64_run_t *r, lagfx_protocol_t *p, const uint8_t *recs, uint64_t lim) {
    uint64_t q = 0; uint32_t guard = 0;
    while (q + 8u <= lim && guard++ < LAGFX_EXEC_MAX_INNER) {
        uint32_t tag = lagfx_le32(recs + q), sz = lagfx_le32(recs + q + 4);
        if (sz < 8u || q + sz > lim) break;
        if (tag == 26u) break;
        if (tag == 114u || tag == 129u) {
            const uint8_t *rec = recs + q + 8u; uint32_t n = sz - 8u;
            uint32_t count = n >= 8u ? lagfx_le32(rec + 4) : 0u;
            if (count > A64_MAX_FB || (uint64_t)n < 8ull + 4ull * count) { q += sz; continue; }
            for (uint32_t i = 0; i < count; i++) {
                uint32_t ref = lagfx_le32(rec + 8 + 4 * i);
                if (ref && !lagfx_a64_res_lookup(p, r->task->id, ref)) CNT(p).ext_tex_prepared++;
                if (ref) (void)lagfx_a64_res_prepare_sampled(r->x, ref);
            }
        }
        q += sz;
    }
}

static bool supported_tag(uint32_t tag) {
    switch (tag) { case 26: case 116: case 125: case 110: case 126: case 111: case 114: case 112: case 117: case 130: case 7: case 1: case 104: case 107: case 108: case 115: case 119: return true; default: return false; }
}
/* Tags this module executes itself (no handler of the x86-shaped table in render_inner_ops.c exists for them): draw variants, LOD sampler binds, hints, fences, barriers. */
static bool hint_tag(uint32_t tag) { return tag == 137u || tag == 27u || tag == 134u || tag == 135u || tag == 24u || tag == 25u; }
static bool direct_tag(uint32_t tag) {
    switch (tag) {
    case 0: case 2: case 3: case 4: case 5: case 6: case 8: case 9: case 10: case 11: case 16: case 17: case 113: case 128: case 127: case 129: case 101:
    case 137: case 27: case 134: case 135: case 24: case 25: case 22: case 23: case 133: return true;
    case 155: case 156: case 157: case 158: case 159: case 160: case 161: case 162: case 163: case 164: return true;
    default: return false;
    }
}

void lagfx_a64_exec_segment(lagfx_protocol_t *p, lagfx_task_entry_t *task, const uint8_t *buf, uint32_t avail, uint64_t len, const lagfx_exec_seg_t *s) {
    a64_run_t *r = p ? RUN(p) : NULL;
    if (!r || !r->x || !buf || !s) return;
    (void)len;
    r->task = task;
    if (!s->cont || s->type != 0u) pass_end(r);                 /* Apple: a segment that does not continue (or a blit encoder) closes the render encoder */
    if (s->type == 2u) {                                        /* Blit: recorded into the SAME command buffer, after the render pass above was ended and its target
                                                                 * transitioned back to GENERAL; objects are the same mirror objects the Render tags use */
        uint32_t hl = s->prot_skipped ? 24u : 8u, have_b = avail > hl ? avail - hl : 0;
        CNT(p).seg_blit++;
        lagfx_a64_res_blit_segment(r->x, buf + hl, s->body_len < have_b ? s->body_len : have_b);
        return;
    }
    if (s->type != 0u) { CNT(p).seg_unsupported++; return; }    /* compute (1), information (4) and the rest: counted, not executed */
    CNT(p).seg_render++;
    uint64_t pos = s->prot_skipped ? 24u : 8u;
    uint64_t have = avail > pos ? avail - pos : 0, lim = s->body_len < have ? s->body_len : have, q = 0;
    uint32_t n_inner = 0;
    while (q + 8u <= lim && n_inner++ < LAGFX_EXEC_MAX_INNER) {
        uint32_t tag = lagfx_le32(buf + pos + q), sz = lagfx_le32(buf + pos + q + 4);
        if (sz < 8u || q + sz > lim) break;
        const uint8_t *rec = buf + pos + q + 8u;
        uint32_t n = sz - 8u;
        if (tag == 26u) { pass_end(r); prescan_pass_textures(r, p, buf + pos + q + sz, lim - q - sz); lagfx_a64_msaa_prescan(p, &r->ms, buf + pos + q + sz, lim - q - sz); }   /* outside any pass: the textures this pass samples; deferred store actions / resolve textures (tags 0x66 / 0x96) */
        if (tag >= 30u && tag <= 36u) {                         /* these describe the pass that follows */
            if (tag == 32u || tag == 33u) tag_unsup(p, tag); else tag_partial(p, tag);
        } else if ((tag == 102u || tag == 150u) && r->pass_open && r->ms.samples > 1u) {   /* read ahead by lagfx_a64_msaa_prescan at the pass's start */
            tag_done(p, tag);
        } else if (!supported_tag(tag) && !direct_tag(tag)) {
            tag_unsup(p, tag);
        } else if (tag != 26u && !r->pass_open && !hint_tag(tag)) {
            CNT(p).no_pass++;
            tag_unsup(p, tag);
        } else if (direct_tag(tag)) {
            r->consumed = true;
            render_tag(r, p, tag, rec, n);
        } else {
            /* through the existing table (render_inner_ops.c), whose handlers call lagfx_a64_exec_render_op */
            r->dispatching = true, r->consumed = false;
            lagfx_render_inner_dispatch(p, tag, rec, n);
            r->dispatching = false;
            if (!r->consumed) tag_unsup(p, tag);
        }
        q += sz;
    }
    if (!s->more) pass_end(r);
}


/* ---- submission: ONE submit per exec, bounded wait; "not finished yet" is PENDING, not failed ---- */

static void flush_if_idle(lagfx_protocol_t *p) {
    a64_run_t *r = p ? RUN(p) : NULL;
    if (r) { r->vk = get_vk(p); flush_deferred(r); }
}

uint32_t lagfx_a64_exec_poll(lagfx_protocol_t *p) {
    if (!p || !p->a64_res) return 0;
    if (lagfx_a64_res_is_pending(p)) lagfx_a64_res_poll(p, 0);
    flush_if_idle(p);
    return lagfx_a64_res_is_pending(p) ? 1u : 0u;
}

/* The completion queue's view of an exec that was still running when the bounded wait ended: finished (write-backs done) or not. */
static bool pending_wait(void *opaque, uint64_t timeout_ns, bool *failed) {
    lagfx_protocol_t *p = opaque;
    if (!lagfx_a64_res_is_pending(p)) return true;
    lagfx_a64_status_t st = lagfx_a64_res_poll(p, (uint32_t)(timeout_ns / 1000000ull));
    if (st == LAGFX_A64_PENDING) return false;
    if (st == LAGFX_A64_FAILED && failed) *failed = true;
    flush_if_idle(p);
    return true;
}
static void pending_release(void *opaque) { (void)opaque; }   /* nothing to free: teardown (lagfx_a64_exec_free) owns the exec */

lagfx_a64_exec_status_t lagfx_a64_exec_end(lagfx_protocol_t *p) {
    a64_run_t *r = p ? RUN(p) : NULL;
    if (!r || !r->x) return LAGFX_A64_EXEC_NONE;
    pass_end(r);
    lagfx_a64_rexec_t *x = r->x;
    r->x = NULL, r->cap = NULL;   /* the capture now belongs to the mirror's finish hook (written once the fence signalled) */
    const uint64_t hold_t0 = lagfx_hold_now_ns(), hold_down0 = CNT(p).mapped_bytes_down;
    lagfx_a64_status_t st = lagfx_a64_res_end(x);   /* submits once, bounded wait (lagfx_vk_wait_bounded), then the flag-0x04 write-backs if DONE */
    lagfx_hold_note(LAGFX_HOLD_EXEC_END, hold_t0, CNT(p).mapped_bytes_down - hold_down0);
    lagfx_arm64_submit_ctx_t *ctx = lagfx_arm64_submit_current(p);
    switch (st) {
    case LAGFX_A64_DONE:
        CNT(p).submits++;
        flush_deferred(r);
        return LAGFX_A64_EXEC_DONE;
    case LAGFX_A64_PENDING: {
        CNT(p).submits++;
        CNT(p).submit_pending++;
        lagfx_arm64_gpu_work_t w = { p, pending_wait, pending_release };
        /* the stamp of this command must not complete until lagfx_a64_res_poll reports DONE (write-backs included): the queue polls pending_wait */
        if (!lagfx_arm64_submit_add_work(ctx, &w)) CNT(p).pending_unqueued++;   /* no queue (decode used without the drain): the caller owns the stamp */
        LAGFX_LOG("arm64 exec: not finished after the bounded wait: kept pending, stamp deferred");
        return LAGFX_A64_EXEC_PENDING;
    }
    default:
        CNT(p).submit_failed++;
        lagfx_arm64_submit_mark_failed(ctx);   /* failed before / during submit: the stamp completes as failed, in order */
        flush_deferred(r);
        return LAGFX_A64_EXEC_FAILED;
    }
}

#endif /* LAGFX_HAVE_VULKAN */
