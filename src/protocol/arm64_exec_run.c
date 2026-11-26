/*
 * libapplegfx-vulkan — ARM64 GPU exec (0x43 / 0x45): execution of Render segments on Vulkan. See arm64_exec_run.h.
 */
#include "arm64_exec_run.h"
#include "arm64_objects.h"
#include "arm64_pipeline.h"
#include "arm64_depth_stencil.h"
#include "arm64_exec_decode.h"
#include "arm64_draw_decode.h"
#include "arm64_sampler.h"
#include "arm64_resources.h"
#include "arm64_completion.h"
#include "arm64_dump.h"
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
#include "../translate/render_encoder.h"
#include "../handlers/compute/render_inner_ops.h"
#include "render_pass.h"
#include <vulkan/vulkan.h>

#define A64_MAX_VB    31u
#define A64_MAX_FB    32u
#define A64_MAX_FSAMP 16u
#define A64_PIPE_CACHE 8u
#define A64_GATHER_MAX (8ull << 20)
#define A64_GATHER_CACHE 8u

enum { K_PIPE = 3, K_SAMP = 4, K_DSS = 5 };

/* gen: unique per creation (a replaced (task, ref) is a new object with a new gen); every cache key below includes it. Mirror buffers/textures carry their own gen. */
typedef struct { uint32_t kind; uint64_t gen; } objhdr_t;
#define A64_SAMP_VARIANTS 4u
/* A sampler plus the variants made for setFragmentSamplerState:lodMinClamp:lodMaxClamp: (tag 0x71): same state, other LOD clamps; owned by this object. */
typedef struct {
    objhdr_t h; VkSampler s;
    VkSamplerCreateInfo ci; bool mip;                                        /* what s was made from; mip = the descriptor is mip mapped (clamps apply) */
    struct { float lo, hi; VkSampler s; } var[A64_SAMP_VARIANTS]; uint32_t nvar;
} a64_samp_t;
typedef struct { objhdr_t h; lagfx_a64_dss_t d; } a64_dss_t;   /* a decoded depth/stencil state object (tag 4) */
typedef struct {
    objhdr_t h;
    lagfx_a64_pipeline_src_t src;
    struct { VkPipeline pl; VkFormat fmt[4]; VkFormat dfmt[2]; uint8_t topo; } cache[A64_PIPE_CACHE];   /* topo: topology | 0x10 for the alt (explicit LOD) fragment shader */   /* one VkPipeline per (colour formats of slots 0..3, depth / stencil attachment formats, topology), owned by THIS object only */
    uint32_t ncache;
    uint32_t ref;
} a64_pipe_t;

typedef struct { int kind; void *obj; VkPipeline pipe; VkDescriptorSet ds; } deferred_t;   /* kind 0 = whole object, 1 = a VkPipeline, 2 = a descriptor set */

/* Buffers, textures and the exec's command buffer / fence / write-backs belong to the resource mirror (arm64_resources.c): ONE exec context and ONE
 * command buffer per exec. This module keeps only what is Render specific: the (task, ref) table of pipeline objects and the open pass. */
typedef struct {
    lagfx_a64_objects_t *objs;
    lagfx_protocol_t *p;
    struct lagfx_vk_state *vk;
    lagfx_a64_pipeline_resolver_t resolver; void *resolver_ctx;
    lagfx_a64_rexec_t *x;            /* the exec being recorded (mirror context), NULL outside an exec */
    bool pass_open;                  /* a tag 26 was seen and the segment chain has not ended it */
    bool last_fetch;                 /* the previous draw of this pass read input attachments (framebuffer fetch) */
    bool rendering;                  /* the render encoder has a vkCmdBeginRendering open (pass_open without it = a pass with no usable target) */
    lagfx_a64_obj_t *rt; uint32_t rt_w, rt_h;   /* the first mirror texture being drawn into (rts[first used slot]) */
    lagfx_a64_obj_t *rts[4]; uint32_t nslots;   /* colour attachments by their ORIGINAL slot (fragment output location); a null entry = unused slot; nslots = highest used slot + 1 */
    a64_pipe_t *pipe; bool pipe_set;
    const lagfx_task_entry_t *task;  /* the exec's task */
    bool dispatching, consumed;      /* inside lagfx_render_inner_dispatch for a tag of this module */
    lagfx_translate_render_state_t rs;   /* the existing render encoder: begin / bind / draw / end of the open pass */
    struct { lagfx_a64_obj_t *b; uint64_t off; } vb[A64_MAX_VB];
    struct { lagfx_a64_obj_t *b; uint64_t off; } fb[A64_MAX_FB];   /* fragment buffers (tags 110/111) */
    lagfx_a64_obj_t *ftex[A64_MAX_FB];                             /* fragment textures (114): mirror textures, GENERAL layout */
    a64_samp_t *fsamp[A64_MAX_FSAMP];                              /* fragment samplers (112) */
    lagfx_a64_obj_t *vtex[A64_MAX_FB];                             /* vertex textures (129) */
    a64_samp_t *vsamp[A64_MAX_FSAMP]; VkSampler vvar[A64_MAX_FSAMP];   /* vertex samplers (127) and the LOD-clamp variants of tag 128 */
    VkPipeline bound_vk;                                           /* the VkPipeline bound in the open pass */
    VkSampler defsamp; VkDescriptorPool dpool; VkDescriptorSet last_ds; uint64_t last_ds_key;   /* descriptor sets: one per draw, reused while the binding key is unchanged */
    uint64_t gen_ctr; VkDeviceSize min_sb_align;
    VkSampler fvar[A64_MAX_FSAMP];                                 /* fragment sampler slot overridden by a LOD-clamp bind (0x71): a variant owned by fsamp[i], else NULL */
    VkDeviceSize max_sb_range;
    struct { uint64_t gen, off, range; VkBuffer buf; } gc[8]; uint32_t ngc;   /* aligned copies made in this exec (gather_copy) */
    lagfx_draw_region_t region;
    /* depth / stencil attachment of the open pass (NULL / UNDEFINED = none) and the encoder state Metal keeps outside the pipeline; all of it is reset by a new pass
     * (a new Metal encoder starts from the defaults) and applied as Vulkan dynamic state before each draw */
    lagfx_a64_obj_t *dsobj; VkFormat ds_dfmt, ds_sfmt;
    struct { lagfx_a64_dss_t dss; uint32_t sref[2]; uint32_t cull, winding; float bias[3]; bool dirty; } es;
    bool wrote_storage;              /* a draw of the open pass may write storage images: pass_end adds a shader-write barrier */
    deferred_t *def; uint32_t ndef, capdef;
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
        if (pp->src.release) pp->src.release(pp->src.user);
        free(pp);
    } else if (h->kind == K_SAMP) {
        a64_samp_t *sm = obj;
        if (r->vk && sm->s) vkDestroySampler(r->vk->device, sm->s, NULL);
        for (uint32_t i = 0; r->vk && i < sm->nvar; i++) if (sm->var[i].s) vkDestroySampler(r->vk->device, sm->var[i].s, NULL);
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
    uint32_t sf = lagfx_a64_sampler_decode(d, size < sizeof d ? size : sizeof d, r->vk->have_sampler_anisotropy, r->vk->phys_props.limits.maxSamplerAnisotropy, &sm->ci);
    sm->mip = (sf & LAGFX_A64_SAMP_MIPMAPPED) != 0;
    if (sf & LAGFX_A64_SAMP_COMPARE_IGNORED) CNT(p).ext_samp_compare++;
    if (sf & LAGFX_A64_SAMP_UNNORM_APPLIED) CNT(p).ext_samp_unnorm++;
    if (sf & LAGFX_A64_SAMP_MIRROR_CLAMP_APPROX) CNT(p).ext_samp_mirror_clamp++;
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
    if (!r->resolver(r->resolver_ctx, p, t, ref, e, &pp->src)) { CNT(p).pipeline_unresolved++; free(pp); return NULL; }
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
    r->x = NULL;
    r->pass_open = r->rendering = r->wrote_storage = false, r->rt = NULL, r->pipe = NULL, r->pipe_set = false;
    memset(r->rts, 0, sizeof r->rts), r->nslots = 0;
    if (!r->vk) { CNT(p).no_vulkan++; return false; }
    flush_deferred(r);   /* an earlier submission may have finished */
    r->task = task;
    memset(r->vb, 0, sizeof r->vb);
    memset(&r->region, 0, sizeof r->region);
    reset_frag_tables(r);
    r->last_ds = VK_NULL_HANDLE, r->bound_vk = VK_NULL_HANDLE;
    r->ngc = 0;
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

/* pl == VK_NULL_HANDLE records a failed build (looked up like a hit, returns NULL). */
static void cache_put(a64_pipe_t *pp, const VkFormat fmts[4], const VkFormat dfm[2], uint32_t topo, VkPipeline pl, a64_run_t *r) {
    if (pp->ncache == A64_PIPE_CACHE) { defer(r, 1, NULL, pp->cache[0].pl); memmove(&pp->cache[0], &pp->cache[1], (A64_PIPE_CACHE - 1u) * sizeof pp->cache[0]); pp->ncache--; }
    pp->cache[pp->ncache].pl = pl, memcpy(pp->cache[pp->ncache].fmt, fmts, 4 * sizeof(VkFormat)), memcpy(pp->cache[pp->ncache].dfmt, dfm, 2 * sizeof(VkFormat)), pp->cache[pp->ncache].topo = (uint8_t)topo;
    pp->ncache++;
}

/* The VkPipeline for (pass colour format, topology): built lazily, cached on the pipeline object (which is per generation). */
static VkPipeline pipeline_for(a64_run_t *r, lagfx_protocol_t *p, a64_pipe_t *pp, const VkFormat fmts[4], uint32_t nslots, uint32_t topo) {
    const VkFormat dfm[2] = { r->ds_dfmt, r->ds_sfmt };
    const uint32_t key = topo; topo &= 15u;   /* bit 4 of the key: build with the explicit-LOD fragment shader */
    for (uint32_t i = 0; i < pp->ncache; i++)
        if (!memcmp(pp->cache[i].fmt, fmts, 4 * sizeof(VkFormat)) && !memcmp(pp->cache[i].dfmt, dfm, sizeof dfm) && pp->cache[i].topo == key) return pp->cache[i].pl;
    lagfx_pipeline_desc_t d = pp->src.desc;
    if (key & 16u) d.fragment_shader = pp->src.alt_fragment_shader;
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
    if (r->vk->have_raster_order) for (uint32_t i = 0; i < pp->src.n_bind; i++) if (pp->src.bind[i].kind == LAGFX_A64_BIND_INPUT) d.raster_order_access = 1;
    d.topology_plus1 = (uint8_t)(topo + 1u);
    VkPipeline pl = VK_NULL_HANDLE;
    if (lagfx_pipeline_build(r->vk->device, &d, &pl) != LAGFX_OK) {
        p->diag2.exec.pipe.pipe_build_failed++;
        lagfx_a64_dump_spirv(p, d.vertex_spv, d.vertex_spv_bytes, d.fragment_spv, d.fragment_spv_bytes);   /* LAGFX_ARM64_DUMP=1 + dir: the failing modules, for offline spirv-val */
        cache_put(pp, fmts, dfm, key, VK_NULL_HANDLE, r);   /* failure cache: the same (pipeline, formats, topology) is not rebuilt (and re-logged) on every draw */
        return VK_NULL_HANDLE;
    }
    p->diag2.exec.pipe.pipes_built++;
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

/* A storage-buffer range that starts at an offset the device cannot bind (not a multiple of minStorageBufferOffsetAlignment): the bytes [off, off + range) of the
 * mirror buffer are copied (host side, at record time) into an exec-owned scratch buffer and that is bound at offset 0. Reference: the independent pvg-vulkan
 * runtime does the same ("gather") for guest binds that are not zero-copyable. Limits: the copy is a snapshot of the mirror at this draw (GPU writes of the same
 * exec to that buffer, e.g. by an earlier blit, or writes by the shader itself, are not seen / not written back), and ranges above A64_GATHER_MAX are refused.
 * One copy per (buffer generation, offset) and exec. NULL = not possible (the draw is skipped, counted as desc_unaligned). */
static VkBuffer gather_copy(a64_run_t *r, lagfx_protocol_t *p, lagfx_a64_obj_t *b, uint64_t off, uint64_t range) {
    if (!b->map || range > A64_GATHER_MAX) { CNT(p).ext_gather_failed++; return VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < r->ngc; i++) if (r->gc[i].gen == b->gen && r->gc[i].off == off && r->gc[i].range == range) return r->gc[i].buf;
    VkBuffer sb; uint8_t *mp;
    if (r->ngc == A64_GATHER_CACHE || !lagfx_a64_res_scratch(r->x, range, &sb, &mp)) { CNT(p).ext_gather_failed++; return VK_NULL_HANDLE; }
    memcpy(mp, b->map + off, (size_t)range);
    r->gc[r->ngc].gen = b->gen, r->gc[r->ngc].off = off, r->gc[r->ngc].range = range, r->gc[r->ngc].buf = sb, r->ngc++;
    CNT(p).ext_gathered++;
    return sb;
}

/* Fills and binds the set-0 descriptor set of the pipeline from the Metal tables recorded by tags 125/126/110/111/114/112. Anything unbound / misaligned /
 * a feedback loop skips the draw (counted): Metal leaves those undefined. The key (pipeline gen, then per binding the object gen and offset) lets consecutive
 * draws share one set; a replaced object has a new gen, so a stale set is never reused. Mirror textures stay in GENERAL layout (valid for sampling), so no
 * layout change is recorded inside the pass. A set is parked in the deferred list and freed only once no recorded / running work can use it. */
static bool bind_descriptors(a64_run_t *r, lagfx_protocol_t *p, a64_pipe_t *pp) {
    const lagfx_a64_pipeline_src_t *s = &pp->src;
    if (!s->n_bind) return true;
    if (!descriptor_pool(r)) { p->diag2.exec.pipe.desc_failed++; return false; }
    VkDescriptorBufferInfo bi[LAGFX_A64_MAX_BINDINGS];
    VkDescriptorImageInfo ii[LAGFX_A64_MAX_BINDINGS];
    VkWriteDescriptorSet w[LAGFX_A64_MAX_BINDINGS];
    uint64_t key = 1469598103934665603ull ^ pp->h.gen;
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
            if (!b || off >= b->length) { p->diag2.exec.pipe.desc_unbound++; return false; }
            VkBuffer dbuf = b->buf; VkDeviceSize doff = off, range = b->length - off;
            if (r->max_sb_range && range > r->max_sb_range) range = r->max_sb_range, CNT(p).ext_range_clamped++;   /* the descriptor range may not exceed maxStorageBufferRange */
            if (off % r->min_sb_align && s->bind[i].writable) { CNT(p).ext_wsb_skipped++; p->diag2.exec.pipe.desc_unaligned++; return false; }   /* a gathered copy would swallow the shader's writes (and the copy is a snapshot): skip, counted, rather than diverge */
            if (off % r->min_sb_align) {   /* Metal allows any 4-aligned offset, Vulkan only multiples of minStorageBufferOffsetAlignment: bind an aligned copy of the range */
                dbuf = gather_copy(r, p, b, off, range);
                if (!dbuf) { p->diag2.exec.pipe.desc_unaligned++; return false; }
                doff = 0;
            }
            bi[i] = (VkDescriptorBufferInfo){ dbuf, doff, range };
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w[i].pBufferInfo = &bi[i];
            MIX(b->gen), MIX(off), MIX((uint64_t)(uintptr_t)dbuf);
        } else if (s->bind[i].kind == LAGFX_A64_BIND_INPUT) {   /* framebuffer fetch: colour attachment mi of the open pass */
            lagfx_a64_obj_t *t = mi < 4u ? r->rts[mi] : NULL;
            if (!t || !t->view || !r->rendering) { p->diag2.exec.pipe.desc_unbound++; return false; }
            ii[i] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, t->view, (VkImageLayout)1000232000 /* RENDERING_LOCAL_READ */ };
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, w[i].pImageInfo = &ii[i];
            MIX(t->gen), MIX(0x1f0u);
        } else if (s->bind[i].kind == LAGFX_A64_BIND_STORAGE_IMAGE) {   /* texture2d<T, access::write>: the mirror texture itself, GENERAL layout */
            lagfx_a64_obj_t *t = mi_s >= 0 && mi < A64_MAX_FB ? r->ftex[mi] : NULL;
            if (!t) { p->diag2.exec.pipe.desc_unbound++; return false; }
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
            if (!t) { p->diag2.exec.pipe.desc_unbound++; return false; }
            bool fb = false;
            for (uint32_t k = 0; k < r->nslots && k < 4u; k++) fb |= t == r->rts[k];
            if (fb) { p->diag2.exec.pipe.desc_feedback++; return false; }   /* sampling an attachment being rendered */
            VkImageView v = t->linear ? VK_NULL_HANDLE : lagfx_a64_res_sample_view(r->x, t);   /* a linear texture needs a buffer pull recorded outside a pass: not sampled */
            if (!v) { p->diag2.exec.pipe.desc_unbound++; return false; }
            ii[i] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL };
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, w[i].pImageInfo = &ii[i];
            MIX(t->gen);
        } else {
            a64_samp_t *sm = mi_s >= 0 && mi < A64_MAX_FSAMP ? (st ? r->fsamp[mi] : r->vsamp[mi]) : NULL;
            VkSampler *vvt = st ? r->fvar : r->vvar;
            VkSampler vs = sm ? (vvt[mi] ? vvt[mi] : sm->s) : (mi_s < 0 ? default_sampler(r) : VK_NULL_HANDLE);   /* -1: an inline sampler of the shader: the default one; fvar: LOD clamps of tag 0x71 */
            if (!vs) { p->diag2.exec.pipe.desc_unbound++; return false; }
            ii[i] = (VkDescriptorImageInfo){ vs, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, w[i].pImageInfo = &ii[i];
            MIX(sm ? sm->h.gen : 0xd5u), MIX((uint64_t)(uintptr_t)vs);
        }
    }
#undef MIX
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
    return true;
}

/* Everything a draw needs besides the vertex buffer: the VkPipeline of this primitive type (Metal 0 point, 1 line, 2 line strip, 3 triangle, 4 triangle strip =
 * the VkPrimitiveTopology values) bound in the open pass, and the descriptor set. False = draw skipped (counted). */
static bool prepare_pipeline(a64_run_t *r, lagfx_protocol_t *p, uint32_t prim) {
    if (prim > 4u) { CNT(p).prim_unsupported++; CNT(p).draws_skipped++; return false; }
    if (prim == 0u && r->pipe->src.no_point_size) { p->diag2.exec.pipe.point_no_size++; CNT(p).draws_skipped++; return false; }   /* Metal needs [[point_size]] too */
    if (!r->vk->have_local_read)   /* a pipeline that reads the attachments cannot run without the device feature */
        for (uint32_t i = 0; i < r->pipe->src.n_bind; i++)
            if (r->pipe->src.bind[i].kind == LAGFX_A64_BIND_INPUT) { p->diag2.exec.pipe.stub_fbfetch++; CNT(p).draws_skipped++; return false; }
    VkFormat fmts[4] = { VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED };
    for (uint32_t i = 0; i < r->nslots && i < 4u; i++) if (r->rts[i]) fmts[i] = r->rts[i]->format;
    uint32_t alt = 0;   /* an unnormalized-coordinates sampler in a fragment sampler slot of this pipeline: only explicit-LOD sampling is valid, so the LOD 0 variant of the fragment shader */
    for (uint32_t i = 0; i < r->pipe->src.n_bind; i++) {
        const int16_t mi = r->pipe->src.bind[i].metal_index;
        if (r->pipe->src.bind[i].stage != 1 || r->pipe->src.bind[i].kind != LAGFX_A64_BIND_SAMPLER || mi < 0 || (uint32_t)mi >= A64_MAX_FSAMP) continue;
        if (r->fsamp[mi] && r->fsamp[mi]->ci.unnormalizedCoordinates) alt = 16u;
    }
    if (alt && !r->pipe->src.alt_fragment_shader) { CNT(p).ext_unnorm_skipped++; CNT(p).draws_skipped++; return false; }
    if (alt) CNT(p).ext_unnorm_lod0++;
    VkPipeline vp = pipeline_for(r, p, r->pipe, fmts, r->nslots, prim | alt);
    if (!vp) { CNT(p).no_pipeline++; CNT(p).draws_skipped++; return false; }
    if (r->bound_vk != vp) {
        if (lagfx_translate_render_bind_vk_pipeline(&r->rs, vp, r->pipe->src.desc.layout) != LAGFX_OK) { CNT(p).no_pipeline++; CNT(p).draws_skipped++; return false; }
        r->bound_vk = vp;
    }
    if (!bind_descriptors(r, p, r->pipe)) { CNT(p).draws_skipped++; return false; }
    bool fetch = false;
    for (uint32_t i = 0; i < r->pipe->src.n_bind; i++) fetch |= r->pipe->src.bind[i].kind == LAGFX_A64_BIND_INPUT;
    if (fetch || r->last_fetch) {   /* same-pixel dependency inside the pass: earlier colour writes -> this draw's input-attachment read, and (after a reading draw) its read -> later writes */
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT };
        vkCmdPipelineBarrier(lagfx_a64_res_cmdbuf(r->x), VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_DEPENDENCY_BY_REGION_BIT, 1, &mb, 0, NULL, 0, NULL);
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
        if (r->dsobj) lagfx_a64_res_ds_end(r->x, r->dsobj);
    }
    r->dsobj = NULL, r->ds_dfmt = r->ds_sfmt = VK_FORMAT_UNDEFINED;
    r->pass_open = r->rendering = false, r->last_fetch = false;
    memset(r->rts, 0, sizeof r->rts), r->nslots = 0;
    r->rt = NULL, r->pipe = NULL, r->pipe_set = false, r->bound_vk = VK_NULL_HANDLE;
}

#define R32(o) lagfx_le32(rec + (o))
#define R16(o) ((uint32_t)rec[(o)] | (uint32_t)rec[(o) + 1] << 8)
static double rd_f64(const uint8_t *b) { uint64_t u = lagfx_le64(b); double d; memcpy(&d, &u, 8); return d; }


static bool dup_of_color(const a64_run_t *r, const lagfx_a64_obj_t *o) {
    for (uint32_t q = 0; q < 4u; q++) if (r->rts[q] == o) return true;
    return false;
}

/* tag 26 (inner opcode 0x1a): the 584-byte pass descriptor, parsed by the existing lagfx_parse_render_pass_descriptor (render_pass.c). Up to four colour
 * attachments (by original slot) and one depth / stencil texture are rendered to; begin/end of the dynamic rendering go through the existing render encoder
 * (translate/render_encoder.c). */
static void begin_pass(a64_run_t *r, lagfx_protocol_t *p, const uint8_t *rec, uint32_t n) {
    pass_end(r);
    CNT(p).passes++;
    r->pass_open = true;                        /* even without a usable target: the following tags belong to this pass */
    memset(r->vb, 0, sizeof r->vb);
    memset(&r->region, 0, sizeof r->region);
    reset_frag_tables(r);
    lagfx_render_pass_full_desc_t d;
    if (n < LAGFX_RENDER_PASS_WIRE_SIZE || lagfx_parse_render_pass_descriptor(rec, n, &d) != 0) { CNT(p).bad_rec++; tag_unsup(p, 26); return; }
    {   /* a new encoder: Metal's defaults for everything the depth / stencil / raster records set */
        memset(&r->es, 0, sizeof r->es);
        lagfx_a64_dss_default(&r->es.dss);
        r->es.dirty = true;
    }
    /* colour attachments by their ORIGINAL slot (the fragment output location); up to 4. The SAME mirror object a blit / sync / write-back of this exec
     * resolves for the ref. A slot whose target cannot be built is left unused (and counted), the others still render. */
    VkRenderingAttachmentInfo ca[4];
    memset(ca, 0, sizeof ca);
    for (int k = 0; k < 4; k++) ca[k] = (VkRenderingAttachmentInfo){ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = VK_NULL_HANDLE,
        .imageLayout = r->vk->have_local_read ? (VkImageLayout)1000232000 : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE };
    lagfx_a64_obj_t *first = NULL;
    uint32_t fw = 0, fh = 0, nslots = 0;
    for (uint32_t k = 0; k < d.color_attachment_count && k < LAGFX_RENDER_PASS_MAX_COLOR_ATTACHMENTS; k++) {
        uint32_t slot = d.colors[k].slot;
        if (slot >= 4u) { CNT(p).extra_attach_ignored++; continue; }
        uint64_t lin0 = lagfx_a64_res_stats(p)->linear_unsupported;
        lagfx_a64_obj_t *t = lagfx_a64_res_object(r->x, d.colors[k].texture_ref);
        if (lagfx_a64_res_stats(p)->linear_unsupported != lin0) CNT(p).obj_unsupported_linear++;      /* a type 8/9 render target the mirror could not build */
        if (!t || t->kind != LAGFX_A64_TEXTURE) { CNT(p).no_target++; continue; }
        uint32_t load = d.colors[k].load_action;   /* MTLLoadAction: 0 DontCare, 1 Load, 2 Clear */
        VkAttachmentLoadOp lop = load == 2u ? VK_ATTACHMENT_LOAD_OP_CLEAR : load == 1u ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        VkClearValue cv;
        memset(&cv, 0, sizeof cv);
        if (lop == VK_ATTACHMENT_LOAD_OP_CLEAR) for (int c = 0; c < 4; c++) cv.color.float32[c] = (float)d.colors[k].clear_color[c];
        VkImageView view; uint32_t w, h;
        if (!lagfx_a64_res_target_begin(r->x, t, &view, &w, &h)) { CNT(p).no_target++; continue; }   /* layout GENERAL -> COLOR_ATTACHMENT_OPTIMAL */
        if (first && (w != fw || h != fh)) { lagfx_a64_res_target_end(r->x, t); CNT(p).extra_attach_ignored++; continue; }   /* one render area for all attachments */
        bool dup = false;
        for (uint32_t q = 0; q < 4u; q++) dup |= r->rts[q] == t;
        if (dup) { lagfx_a64_res_target_end(r->x, t); CNT(p).extra_attach_ignored++; continue; }   /* the same image twice is not a valid attachment set */
        if (!first) first = t, fw = w, fh = h;
        r->rts[slot] = t;
        ca[slot].imageView = view, ca[slot].loadOp = lop, ca[slot].storeOp = VK_ATTACHMENT_STORE_OP_STORE, ca[slot].clearValue = cv;
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
        if (!vk_core13(r->vk)) { CNT(p).ds_no_device_support++; p->diag2.exec.pipe.stub_depth_stencil++; }
        else {
            o = lagfx_a64_res_object(r->x, dref ? dref : sref);
            if (!o || o->kind != LAGFX_A64_TEXTURE || !o->ds_aspects || (dref && !(o->ds_aspects & 1u)) || (!dref && !(o->ds_aspects & 2u))) { CNT(p).ds_attach_unsupported++; o = NULL; }
            else if (dref && sref && sref != dref) { CNT(p).ds_split_ignored++; }   /* the stencil lives in another texture: only the depth one is attached */
            else if (dref && d.depth_level) { CNT(p).ds_attach_unsupported++; o = NULL; }
        }
        if (o && lagfx_a64_res_ds_begin(r->x, o, &dview, &dw, &dh)) {
            if ((first && (dw < fw || dh < fh)) || dup_of_color(r, o)) { CNT(p).ds_attach_unsupported++; CNT(p).extra_attach_ignored++; }   /* attachments must cover the render area */
            else dso = o;
        } else if (o) CNT(p).ds_attach_unsupported++;
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
    if (lagfx_translate_render_begin_ds(r->vk, &r->rs, lagfx_a64_res_cmdbuf(r->x), first ? first->img : dso->img, fw, fh, NULL, ca, nslots,
                                        dso && (dso->ds_aspects & 1u) ? &dai : NULL, dso && (dso->ds_aspects & 2u) ? &sai : NULL) != LAGFX_OK) {
        for (uint32_t q = 0; q < 4u; q++) if (r->rts[q]) lagfx_a64_res_target_end(r->x, r->rts[q]);
        if (dso) lagfx_a64_res_ds_end(r->x, dso);
        memset(r->rts, 0, sizeof r->rts), r->nslots = 0;
        CNT(p).no_target++; tag_partial(p, 26); return;
    }
    if (dso) {
        r->dsobj = dso;
        r->ds_dfmt = (dso->ds_aspects & 1u) ? dso->format : VK_FORMAT_UNDEFINED, r->ds_sfmt = (dso->ds_aspects & 2u) ? dso->format : VK_FORMAT_UNDEFINED;
        CNT(p).ds_passes++;
    }
    r->rendering = true, r->rt = first, r->rt_w = fw, r->rt_h = fh;
    tag_done(p, 26);
}

static void set_pipeline(a64_run_t *r, lagfx_protocol_t *p, const uint8_t *rec, uint32_t n) {
    if (n < 4u) { CNT(p).bad_rec++; tag_unsup(p, 116); return; }
    a64_pipe_t *pp = pipeline_current(r, p, R32(0));
    r->pipe_set = false, r->pipe = NULL;     /* Apple: Metal crashes on a missing pipeline, and on drawing without one */
    if (!pp) { tag_partial(p, 116); return; }
    r->pipe = pp, r->pipe_set = true;        /* the VkPipeline is built and bound at the draw (it depends on the primitive type) */
    tag_done(p, 116);
}

static bool draw_ready(a64_run_t *r, lagfx_protocol_t *p) {
    if (!r->pipe_set || !r->pipe) { CNT(p).no_pipeline++; CNT(p).draws_skipped++; return false; }   /* Apple: no pipeline, no draw */
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
    if (r->dsobj && (lagfx_a64_dss_depth_test(&r->es.dss) || lagfx_a64_dss_stencil_test(&r->es.dss))) CNT(p).ds_draws++;
    lagfx_vk_render_target_t ext = { .width = r->rt_w, .height = r->rt_h };   /* set_region only needs the extent */
    lagfx_vk_cmd_set_region(lagfx_a64_res_cmdbuf(r->x), &ext, &r->region);   /* existing viewport/scissor helper of draw_record.c (Y-flip, clamping) */
    if (r->pipe->src.desc.n_vtx_inputs) {
        uint32_t s = r->pipe->src.vertex_buffer_index;
        if (s >= A64_MAX_VB || !r->vb[s].b || r->vb[s].off >= r->vb[s].b->length) { CNT(p).no_vertex_buf++; CNT(p).draws_skipped++; return false; }
        lagfx_translate_render_bind_vertex_buffer(&r->rs, r->vb[s].b->buf, r->vb[s].off);
    }
    return true;
}

/* Every draw record (tags 0..11 direct, 0x10 / 0x11 indirect; layouts in arm64_draw_decode.h). Indirect draws read their argument block from the mirror of
 * the argument buffer at record time (a snapshot, as the independent reference does: it does not see GPU writes of the same exec). A draw with zero vertices /
 * indices or zero instances draws nothing and is done, not skipped. */
static void draw_cmd(a64_run_t *r, lagfx_protocol_t *p, uint32_t tag, const uint8_t *rec, uint32_t n) {
    lagfx_a64_draw_t d;
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
    if (!bind_for_draw(r, p, d.prim)) { tag_partial(p, tag); return; }
    lagfx_status_t st;
    if (d.indexed) {
        lagfx_translate_render_bind_index_buffer(&r->rs, ib->buf, d.index_offset, d.index_type == 1u ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        st = lagfx_translate_render_draw_indexed(&r->rs, (uint32_t)d.count, (uint32_t)d.instances, index_start, (int32_t)d.base_vertex, (uint32_t)d.base_instance);
    } else {
        st = lagfx_translate_render_draw(&r->rs, (uint32_t)d.count, (uint32_t)d.instances, (uint32_t)d.first_vertex, (uint32_t)d.base_instance);
    }
    if (st != LAGFX_OK) { CNT(p).draws_skipped++; tag_partial(p, tag); return; }
    CNT(p).draws++;
    tag_done(p, tag);
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
            lagfx_a64_obj_t *b = lagfx_a64_res_object(r->x, R32(8 + i * 12));
            if (b && b->kind != LAGFX_A64_BUFFER) b = NULL;
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
        if (count > lim || (uint64_t)n < 8u + 4ull * count) { CNT(p).bad_rec++; tag_unsup(p, tag); break; }
        for (uint32_t i = 0; i < count && index + i < lim; i++) {
            if (tex) {   /* only textures the mirror already holds (the exec's resource list): creating + uploading one now would record copies inside the pass */
                lagfx_a64_obj_t *t = lagfx_a64_res_lookup(p, r->task->id, R32(8 + i * 4));
                tt[index + i] = t && t->kind == LAGFX_A64_TEXTURE ? t : NULL;
            } else ss[index + i] = get_kind(r, p, r->task, R32(8 + i * 4), K_SAMP), vv[index + i] = VK_NULL_HANDLE;
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
        r->region.sc_x = (uint32_t)lagfx_le64(rec), r->region.sc_y = (uint32_t)lagfx_le64(rec + 8);
        r->region.sc_w = (uint32_t)lagfx_le64(rec + 16), r->region.sc_h = (uint32_t)lagfx_le64(rec + 24);
        tag_done(p, tag);
        break;
    case 130: {                              /* MTLViewport: originX, originY, width, height, znear, zfar as doubles */
        if (n < 48u) { CNT(p).bad_rec++; tag_unsup(p, tag); break; }
        double x = rd_f64(rec), y = rd_f64(rec + 8), w = rd_f64(rec + 16), h = rd_f64(rec + 24);
        r->region.vp_x = x > 0 ? (uint32_t)x : 0u, r->region.vp_y = y > 0 ? (uint32_t)y : 0u;
        r->region.vp_w = w > 0 ? (uint32_t)w : 0u, r->region.vp_h = h > 0 ? (uint32_t)h : 0u;
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
                ss[index + i] = sm, vv[index + i] = VK_NULL_HANDLE;
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

static bool supported_tag(uint32_t tag) {
    switch (tag) { case 26: case 116: case 125: case 110: case 126: case 111: case 114: case 112: case 117: case 130: case 7: case 1: case 104: case 107: case 108: case 115: case 119: return true; default: return false; }
}
/* Tags this module executes itself (no handler of the x86-shaped table in render_inner_ops.c exists for them): draw variants, LOD sampler binds, hints, fences, barriers. */
static bool hint_tag(uint32_t tag) { return tag == 137u || tag == 27u || tag == 134u || tag == 135u || tag == 24u || tag == 25u; }
static bool direct_tag(uint32_t tag) {
    switch (tag) {
    case 0: case 2: case 3: case 4: case 5: case 6: case 8: case 9: case 10: case 11: case 16: case 17: case 113: case 128: case 127: case 129:
    case 137: case 27: case 134: case 135: case 24: case 25: case 22: case 23: case 133: return true;
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
        if (tag >= 30u && tag <= 36u) {                         /* these describe the pass that follows */
            if (tag == 32u || tag == 33u) tag_unsup(p, tag); else tag_partial(p, tag);
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
    r->x = NULL;
    lagfx_a64_status_t st = lagfx_a64_res_end(x);   /* submits once, bounded wait (lagfx_vk_wait_bounded), then the flag-0x04 write-backs if DONE */
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
