/*
 * libapplegfx-vulkan — ARM64 GPU exec (0x43 / 0x45): execution of Render segments on Vulkan. See arm64_exec_run.h.
 */
#include "arm64_exec_run.h"
#include "arm64_objects.h"
#include "arm64_pipeline.h"
#include "arm64_exec_decode.h"
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

enum { K_PIPE = 3, K_SAMP = 4 };

/* gen: unique per creation (a replaced (task, ref) is a new object with a new gen); every cache key below includes it. Mirror buffers/textures carry their own gen. */
typedef struct { uint32_t kind; uint64_t gen; } objhdr_t;
typedef struct { objhdr_t h; VkSampler s; } a64_samp_t;
typedef struct {
    objhdr_t h;
    lagfx_a64_pipeline_src_t src;
    struct { VkPipeline pl; VkFormat fmt[4]; uint8_t topo; } cache[A64_PIPE_CACHE];   /* one VkPipeline per (colour formats of slots 0..3, topology), owned by THIS object only */
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
    VkPipeline bound_vk;                                           /* the VkPipeline bound in the open pass */
    VkSampler defsamp; VkDescriptorPool dpool; VkDescriptorSet last_ds; uint64_t last_ds_key;   /* descriptor sets: one per draw, reused while the binding key is unchanged */
    uint64_t gen_ctr; VkDeviceSize min_sb_align;
    lagfx_draw_region_t region;
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

/* Serialized sampler (tag 3): {tag, size, ref, bits u64, lod min f32, lod max f32} -> VkSampler (pvg_metal.m makeSerialized). Compare function, unnormalized
 * coordinates, anisotropy and "mirror clamp to edge" are not mapped (the nearest plain mode is used). */
static VkSamplerAddressMode vk_addr(uint32_t m) {
    switch (m) {
    case 2: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1: case 3: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}
static void *make_sampler(a64_run_t *r, lagfx_protocol_t *p, const lagfx_task_entry_t *t, const uint32_t e[3]) {
    uint8_t d[32];
    uint32_t size = e[0] >> 8;
    if (!r->vk || size < 20u || !lagfx_arm64_task_read(p, t, (uint64_t)e[2] << 32 | e[1], d, size < sizeof d ? size : sizeof d)) { CNT(p).obj_unsupported++; return NULL; }
    uint64_t bits = lagfx_le64(d + 12);
    uint32_t lo = size >= 24u ? lagfx_le32(d + 20) : 0, hi = size >= 28u ? lagfx_le32(d + 24) : 0;
    float lod_min, lod_max;
    memcpy(&lod_min, &lo, 4), memcpy(&lod_max, &hi, 4);
    if (!(lod_max > lod_min) || lod_max > 32.f) lod_min = 0.f, lod_max = 32.f;
    uint32_t bc = (uint32_t)(bits >> 20) & 15u, mip = (uint32_t)(bits >> 24) & 3u;
    VkSamplerCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .minFilter = (bits & 3u) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, .magFilter = ((bits >> 2) & 3u) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = mip == 2u ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = vk_addr((uint32_t)(bits >> 8) & 15u), .addressModeV = vk_addr((uint32_t)(bits >> 12) & 15u), .addressModeW = vk_addr((uint32_t)(bits >> 16) & 15u),
        .minLod = mip ? lod_min : 0.f, .maxLod = mip ? lod_max : 0.25f,
        .borderColor = bc == 2u ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : bc == 1u ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK };
    a64_samp_t *sm = calloc(1, sizeof *sm);
    if (!sm) return NULL;
    sm->h.kind = K_SAMP;
    if (vkCreateSampler(r->vk->device, &ci, NULL, &sm->s) != VK_SUCCESS) { free(sm); p->diag2.exec.pipe.sampler_failed++; return NULL; }
    p->diag2.exec.pipe.samplers++;
    return sm;
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
    if ((e[0] & 0xffu) == LAGFX_A64_OBJ_SERIALIZED) {   /* by the payload's own tag: 3 = sampler; anything else goes to the pipeline resolver, which rejects what it cannot execute */
        uint8_t tg[4];
        if ((e[0] >> 8) >= 4u && lagfx_arm64_task_read(p, t, (uint64_t)e[2] << 32 | e[1], tg, 4) && lagfx_le32(tg) == 3u) return make_sampler(r, p, t, e);
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
    r->pass_open = r->rendering = false, r->rt = NULL, r->pipe = NULL, r->pipe_set = false;
    memset(r->rts, 0, sizeof r->rts), r->nslots = 0;
    if (!r->vk) { CNT(p).no_vulkan++; return false; }
    flush_deferred(r);   /* an earlier submission may have finished */
    r->task = task;
    memset(r->vb, 0, sizeof r->vb);
    memset(&r->region, 0, sizeof r->region);
    reset_frag_tables(r);
    r->last_ds = VK_NULL_HANDLE, r->bound_vk = VK_NULL_HANDLE;
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
    memset(r->fb, 0, sizeof r->fb), memset(r->ftex, 0, sizeof r->ftex), memset(r->fsamp, 0, sizeof r->fsamp);
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

/* The VkPipeline for (pass colour format, topology): built lazily, cached on the pipeline object (which is per generation). */
static VkPipeline pipeline_for(a64_run_t *r, lagfx_protocol_t *p, a64_pipe_t *pp, const VkFormat fmts[4], uint32_t nslots, uint32_t topo) {
    for (uint32_t i = 0; i < pp->ncache; i++) if (!memcmp(pp->cache[i].fmt, fmts, 4 * sizeof(VkFormat)) && pp->cache[i].topo == topo) return pp->cache[i].pl;
    lagfx_pipeline_desc_t d = pp->src.desc;
    d.color_format = fmts[0] != VK_FORMAT_UNDEFINED ? fmts[0] : VK_FORMAT_B8G8R8A8_UNORM;
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
    d.topology_plus1 = (uint8_t)(topo + 1u);
    VkPipeline pl = VK_NULL_HANDLE;
    if (lagfx_pipeline_build(r->vk->device, &d, &pl) != LAGFX_OK) {
        p->diag2.exec.pipe.pipe_build_failed++;
        lagfx_a64_dump_spirv(p, d.vertex_spv, d.vertex_spv_bytes, d.fragment_spv, d.fragment_spv_bytes);   /* LAGFX_ARM64_DUMP=1 + dir: the failing modules, for offline spirv-val */
        return VK_NULL_HANDLE;
    }
    p->diag2.exec.pipe.pipes_built++;
    if (topo < 8u) p->diag2.exec.pipe.topo[topo]++;
    if (pp->ncache == A64_PIPE_CACHE) { defer(r, 1, NULL, pp->cache[0].pl); memmove(&pp->cache[0], &pp->cache[1], (A64_PIPE_CACHE - 1u) * sizeof pp->cache[0]); pp->ncache--; }
    pp->cache[pp->ncache].pl = pl, memcpy(pp->cache[pp->ncache].fmt, fmts, 4 * sizeof(VkFormat)), pp->cache[pp->ncache].topo = (uint8_t)topo;
    pp->ncache++;
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
    VkDescriptorPoolSize sz[3] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096 }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2048 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 2048 } };
    VkDescriptorPoolCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, .maxSets = 512, .poolSizeCount = 3, .pPoolSizes = sz };
    if (vkCreateDescriptorPool(r->vk->device, &ci, NULL, &r->dpool) != VK_SUCCESS) { r->dpool = VK_NULL_HANDLE; return false; }
    VkPhysicalDeviceProperties pr;
    vkGetPhysicalDeviceProperties(r->vk->phys_device, &pr);
    r->min_sb_align = pr.limits.minStorageBufferOffsetAlignment ? pr.limits.minStorageBufferOffsetAlignment : 1;
    return true;
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
            if (off % r->min_sb_align) { p->diag2.exec.pipe.desc_unaligned++; return false; }
            bi[i] = (VkDescriptorBufferInfo){ b->buf, off, b->length - off };
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w[i].pBufferInfo = &bi[i];
            MIX(b->gen), MIX(off);
        } else if (s->bind[i].kind == LAGFX_A64_BIND_IMAGE) {
            lagfx_a64_obj_t *t = mi_s >= 0 && mi < A64_MAX_FB ? r->ftex[mi] : NULL;
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
            a64_samp_t *sm = mi_s >= 0 && mi < A64_MAX_FSAMP ? r->fsamp[mi] : NULL;
            VkSampler vs = sm ? sm->s : (mi_s < 0 ? default_sampler(r) : VK_NULL_HANDLE);   /* -1: an inline sampler of the shader: the default one */
            if (!vs) { p->diag2.exec.pipe.desc_unbound++; return false; }
            ii[i] = (VkDescriptorImageInfo){ vs, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, w[i].pImageInfo = &ii[i];
            MIX(sm ? sm->h.gen : 0xd5u);
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
    VkFormat fmts[4] = { VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED };
    for (uint32_t i = 0; i < r->nslots && i < 4u; i++) if (r->rts[i]) fmts[i] = r->rts[i]->format;
    VkPipeline vp = pipeline_for(r, p, r->pipe, fmts, r->nslots, prim);
    if (!vp) { CNT(p).no_pipeline++; CNT(p).draws_skipped++; return false; }
    if (r->bound_vk != vp) {
        if (lagfx_translate_render_bind_vk_pipeline(&r->rs, vp, r->pipe->src.desc.layout) != LAGFX_OK) { CNT(p).no_pipeline++; CNT(p).draws_skipped++; return false; }
        r->bound_vk = vp;
    }
    if (!bind_descriptors(r, p, r->pipe)) { CNT(p).draws_skipped++; return false; }
    return true;
}

static void pass_end(a64_run_t *r) {
    if (r->rendering) {
        lagfx_translate_render_end(&r->rs);
        for (uint32_t i = 0; i < r->nslots && i < 4u; i++) if (r->rts[i]) lagfx_a64_res_target_end(r->x, r->rts[i]);   /* COLOR_ATTACHMENT_OPTIMAL -> GENERAL + barrier: a following blit / pass sees the result */
    }
    r->pass_open = r->rendering = false;
    memset(r->rts, 0, sizeof r->rts), r->nslots = 0;
    r->rt = NULL, r->pipe = NULL, r->pipe_set = false, r->bound_vk = VK_NULL_HANDLE;
}

#define R32(o) lagfx_le32(rec + (o))
#define R16(o) ((uint32_t)rec[(o)] | (uint32_t)rec[(o) + 1] << 8)
static double rd_f64(const uint8_t *b) { uint64_t u = lagfx_le64(b); double d; memcpy(&d, &u, 8); return d; }


/* tag 26 (inner opcode 0x1a): the 584-byte pass descriptor, parsed by the existing lagfx_parse_render_pass_descriptor (render_pass.c). Only colour
 * attachment 0 is rendered to; begin/end of the dynamic rendering go through the existing render encoder (translate/render_encoder.c). */
static void begin_pass(a64_run_t *r, lagfx_protocol_t *p, const uint8_t *rec, uint32_t n) {
    pass_end(r);
    CNT(p).passes++;
    r->pass_open = true;                        /* even without a usable target: the following tags belong to this pass */
    memset(r->vb, 0, sizeof r->vb);
    memset(&r->region, 0, sizeof r->region);
    reset_frag_tables(r);
    lagfx_render_pass_full_desc_t d;
    if (n < LAGFX_RENDER_PASS_WIRE_SIZE || lagfx_parse_render_pass_descriptor(rec, n, &d) != 0) { CNT(p).bad_rec++; tag_unsup(p, 26); return; }
    if (d.has_depth || d.has_stencil) CNT(p).extra_attach_ignored++;
    /* colour attachments by their ORIGINAL slot (the fragment output location); up to 4. The SAME mirror object a blit / sync / write-back of this exec
     * resolves for the ref. A slot whose target cannot be built is left unused (and counted), the others still render. */
    VkRenderingAttachmentInfo ca[4];
    memset(ca, 0, sizeof ca);
    for (int k = 0; k < 4; k++) ca[k] = (VkRenderingAttachmentInfo){ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = VK_NULL_HANDLE,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE };
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
    if (!first) { CNT(p).no_target++; tag_partial(p, 26); return; }
    r->nslots = nslots;
    if (lagfx_translate_render_begin(r->vk, &r->rs, lagfx_a64_res_cmdbuf(r->x), first->img, fw, fh, NULL, ca, nslots) != LAGFX_OK) {
        for (uint32_t q = 0; q < 4u; q++) if (r->rts[q]) lagfx_a64_res_target_end(r->x, r->rts[q]);
        memset(r->rts, 0, sizeof r->rts), r->nslots = 0;
        CNT(p).no_target++; tag_partial(p, 26); return;
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

static bool bind_for_draw(a64_run_t *r, lagfx_protocol_t *p, uint32_t prim) {
    if (!prepare_pipeline(r, p, prim)) return false;
    lagfx_vk_render_target_t ext = { .width = r->rt_w, .height = r->rt_h };   /* set_region only needs the extent */
    lagfx_vk_cmd_set_region(lagfx_a64_res_cmdbuf(r->x), &ext, &r->region);   /* existing viewport/scissor helper of draw_record.c (Y-flip, clamping) */
    if (r->pipe->src.desc.n_vtx_inputs) {
        uint32_t s = r->pipe->src.vertex_buffer_index;
        if (s >= A64_MAX_VB || !r->vb[s].b || r->vb[s].off >= r->vb[s].b->length) { CNT(p).no_vertex_buf++; CNT(p).draws_skipped++; return false; }
        lagfx_translate_render_bind_vertex_buffer(&r->rs, r->vb[s].b->buf, r->vb[s].off);
    }
    return true;
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
    case 114: case 112: {                    /* {index, count}, refs: fragment textures / samplers */
        if (n < 8u) { CNT(p).bad_rec++; tag_unsup(p, tag); break; }
        uint32_t index = R32(0), count = R32(4), lim = tag == 114 ? A64_MAX_FB : A64_MAX_FSAMP;
        if (count > lim || (uint64_t)n < 8u + 4ull * count) { CNT(p).bad_rec++; tag_unsup(p, tag); break; }
        for (uint32_t i = 0; i < count && index + i < lim; i++) {
            if (tag == 114) {   /* only textures the mirror already holds (the exec's resource list): creating + uploading one now would record copies inside the pass */
                lagfx_a64_obj_t *t = lagfx_a64_res_lookup(p, r->task->id, R32(8 + i * 4));
                r->ftex[index + i] = t && t->kind == LAGFX_A64_TEXTURE ? t : NULL;
            } else r->fsamp[index + i] = get_kind(r, p, r->task, R32(8 + i * 4), K_SAMP);
        }
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
        tag_done(p, tag);
        break;
    }
    case 1: {                                /* {primitive u32, start u16, count u16} (Apple) */
        if (n < 8u) { CNT(p).bad_rec++; tag_unsup(p, tag); break; }
        if (!draw_ready(r, p)) { tag_partial(p, tag); break; }
        if (R32(0) > 4u || !R16(6)) { CNT(p).prim_unsupported++; CNT(p).draws_skipped++; tag_partial(p, tag); break; }
        if (!bind_for_draw(r, p, R32(0))) { tag_partial(p, tag); break; }
        if (lagfx_translate_render_draw(&r->rs, R16(6), 1, R16(4), 0) != LAGFX_OK) { CNT(p).draws_skipped++; tag_partial(p, tag); break; }
        CNT(p).draws++;
        tag_done(p, tag);
        break;
    }
    case 7: {                                /* {primitive u16, index type u16, ref u32, count u16, offset u16} (Apple) */
        if (n < 12u) { CNT(p).bad_rec++; tag_unsup(p, tag); break; }
        if (!draw_ready(r, p)) { tag_partial(p, tag); break; }
        if (R16(0) > 4u || !R16(8)) { CNT(p).prim_unsupported++; CNT(p).draws_skipped++; tag_partial(p, tag); break; }
        uint32_t itype = R16(2), count = R16(8), off = R16(10);
        uint32_t isz = itype == 1u ? 4u : 2u;
        lagfx_a64_obj_t *ib = lagfx_a64_res_object(r->x, R32(4));
        if (itype > 1u || !ib || ib->kind != LAGFX_A64_BUFFER || (uint64_t)off + (uint64_t)count * isz > ib->length) { CNT(p).no_index_buf++; CNT(p).draws_skipped++; tag_partial(p, tag); break; }
        if (!bind_for_draw(r, p, R16(0))) { tag_partial(p, tag); break; }
        lagfx_translate_render_bind_index_buffer(&r->rs, ib->buf, off, itype == 1u ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        if (lagfx_translate_render_draw_indexed(&r->rs, count, 1, 0, 0, 0) != LAGFX_OK) { CNT(p).draws_skipped++; tag_partial(p, tag); break; }
        CNT(p).draws++;
        tag_done(p, tag);
        break;
    }
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
    switch (tag) { case 26: case 116: case 125: case 110: case 126: case 111: case 114: case 112: case 117: case 130: case 7: case 1: return true; default: return false; }
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
        } else if (!supported_tag(tag)) {
            tag_unsup(p, tag);
        } else if (tag != 26u && !r->pass_open) {
            CNT(p).no_pass++;
            tag_unsup(p, tag);
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
