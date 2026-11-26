/*
 * libapplegfx-vulkan — ARM64 GPU exec (0x43 / 0x45): execution of Render segments on Vulkan. See arm64_exec_run.h.
 */
#include "arm64_exec_run.h"
#include "arm64_objects.h"
#include "arm64_resources.h"
#include "arm64_completion.h"
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

enum { K_PIPE = 3 };

typedef struct { uint32_t kind; } objhdr_t;
typedef struct {
    objhdr_t h;
    lagfx_a64_pipeline_src_t src;
    VkPipeline pipe; VkFormat fmt;   /* built for the colour format of the pass that set it; rebuilt when that changes */
} a64_pipe_t;

typedef struct { int kind; void *obj; VkPipeline pipe; } deferred_t;   /* kind 0 = whole object, 1 = a VkPipeline */

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
    lagfx_a64_obj_t *rt; uint32_t rt_w, rt_h;   /* the mirror texture being drawn into */
    a64_pipe_t *pipe; bool pipe_set;
    const lagfx_task_entry_t *task;  /* the exec's task */
    bool dispatching, consumed;      /* inside lagfx_render_inner_dispatch for a tag of this module */
    lagfx_translate_render_state_t rs;   /* the existing render encoder: begin / bind / draw / end of the open pass */
    struct { lagfx_a64_obj_t *b; uint64_t off; } vb[A64_MAX_VB];
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
        destroy_pipeline_vk(r->vk, pp->pipe);
        if (pp->src.release) pp->src.release(pp->src.user);
        free(pp);
    } else free(obj);
}
static void defer(a64_run_t *r, int kind, void *obj, VkPipeline pl) {
    if (r->ndef == r->capdef) {
        uint32_t nc = r->capdef ? r->capdef * 2u : 64u;
        deferred_t *n = realloc(r->def, nc * sizeof *n);
        if (!n) return;   /* cannot park it: leaking one object is the safe choice (freeing under a recording command buffer is not) */
        r->def = n, r->capdef = nc;
    }
    r->def[r->ndef++] = (deferred_t){ kind, obj, pl };
}
static void obj_destroy_cb(void *ctx, void *obj) {
    a64_run_t *r = ctx;
    if (lagfx_a64_res_busy(r->p)) defer(r, 0, obj, VK_NULL_HANDLE); else destroy_obj_now(r, obj);   /* in use by recorded or still-running work */
}
/* Only once no exec records and none is in flight (its fence signalled). */
static void flush_deferred(a64_run_t *r) {
    if (lagfx_a64_res_busy(r->p)) return;
    for (uint32_t i = 0; i < r->ndef; i++) {
        if (r->def[i].kind) destroy_pipeline_vk(r->vk, r->def[i].pipe); else destroy_obj_now(r, r->def[i].obj);
    }
    r->ndef = 0;
}

/* ---- object creation: only pipelines live in this table; buffers and textures are the mirror's (arm64_resources.c) ---- */

static void *make_pipeline(a64_run_t *r, lagfx_protocol_t *p, const lagfx_task_entry_t *t, uint32_t ref, const uint32_t e[3]) {
    if (!r->resolver) { CNT(p).pipeline_unresolved++; return NULL; }
    a64_pipe_t *pp = calloc(1, sizeof *pp);
    if (!pp) return NULL;
    pp->h.kind = K_PIPE;
    if (!r->resolver(r->resolver_ctx, p, t, ref, e, &pp->src)) { CNT(p).pipeline_unresolved++; free(pp); return NULL; }
    return pp;
}

static void *obj_create_cb(void *ctx, lagfx_protocol_t *p, const lagfx_task_entry_t *t, uint32_t ref, const uint32_t e[3]) {
    a64_run_t *r = ctx;
    if ((e[0] & 0xffu) == LAGFX_A64_OBJ_SERIALIZED) return make_pipeline(r, p, t, ref, e);
    CNT(p).obj_unsupported++;
    return NULL;
}

static a64_run_t *run_get(lagfx_protocol_t *p, bool create) {
    if (p->a64_exec || !create) return RUN(p);
    a64_run_t *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->p = p;
    lagfx_a64_obj_ops_t ops = { r, obj_create_cb, obj_destroy_cb };
    r->objs = lagfx_a64_objects_new(&ops);
    if (!r->objs) { free(r); return NULL; }
    p->a64_exec = r;
    return r;
}

static a64_pipe_t *get_pipe(a64_run_t *r, lagfx_protocol_t *p, const lagfx_task_entry_t *t, uint32_t ref) {
    void *o = lagfx_a64_objects_get(r->objs, p, t, ref, NULL);
    return (o && ((objhdr_t *)o)->kind == K_PIPE) ? o : NULL;
}

static void pass_end(a64_run_t *r);

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
    for (uint32_t i = 0; i < r->ndef; i++) { if (r->def[i].kind) destroy_pipeline_vk(r->vk, r->def[i].pipe); else destroy_obj_now(r, r->def[i].obj); }
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
    if (!r->vk) { CNT(p).no_vulkan++; return false; }
    flush_deferred(r);   /* an earlier submission may have finished */
    r->task = task;
    memset(r->vb, 0, sizeof r->vb);
    memset(&r->region, 0, sizeof r->region);
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
    return true;
}

static void pass_end(a64_run_t *r) {
    if (r->rendering) {
        lagfx_translate_render_end(&r->rs);
        lagfx_a64_res_target_end(r->x, r->rt);   /* COLOR_ATTACHMENT_OPTIMAL -> GENERAL + barrier: a following blit / pass sees the result */
    }
    r->pass_open = r->rendering = false;
    r->rt = NULL, r->pipe = NULL, r->pipe_set = false;
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
    lagfx_render_pass_full_desc_t d;
    if (n < LAGFX_RENDER_PASS_WIRE_SIZE || lagfx_parse_render_pass_descriptor(rec, n, &d) != 0) { CNT(p).bad_rec++; tag_unsup(p, 26); return; }
    if (d.color_attachment_count > 1u || d.has_depth || d.has_stencil) CNT(p).extra_attach_ignored++;
    /* the SAME mirror object a blit / sync / write-back of this exec resolves for this ref */
    uint64_t lin0 = lagfx_a64_res_stats(p)->linear_unsupported;
    lagfx_a64_obj_t *t = R32(0x4c) ? lagfx_a64_res_object(r->x, d.colors[0].texture_ref) : NULL;   /* the parser compacts: only index 0 counts here */
    if (lagfx_a64_res_stats(p)->linear_unsupported != lin0) CNT(p).obj_unsupported_linear++;      /* a type 8/9 render target the mirror could not build */
    if (!t || t->kind != LAGFX_A64_TEXTURE) { CNT(p).no_target++; tag_partial(p, 26); return; }
    uint32_t load = d.colors[0].load_action;   /* MTLLoadAction: 0 DontCare, 1 Load, 2 Clear */
    VkAttachmentLoadOp lop = load == 2u ? VK_ATTACHMENT_LOAD_OP_CLEAR : load == 1u ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    VkClearValue cv;
    memset(&cv, 0, sizeof cv);
    if (lop == VK_ATTACHMENT_LOAD_OP_CLEAR) for (int c = 0; c < 4; c++) cv.color.float32[c] = (float)d.colors[0].clear_color[c];
    VkImageView view; uint32_t w, h;
    if (!lagfx_a64_res_target_begin(r->x, t, &view, &w, &h)) { CNT(p).no_target++; tag_partial(p, 26); return; }   /* layout GENERAL -> COLOR_ATTACHMENT_OPTIMAL */
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = lop, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = cv };
    if (lagfx_translate_render_begin(r->vk, &r->rs, lagfx_a64_res_cmdbuf(r->x), t->img, w, h, NULL, &ca, 1) != LAGFX_OK) {
        lagfx_a64_res_target_end(r->x, t);
        CNT(p).no_target++; tag_partial(p, 26); return;
    }
    r->rendering = true, r->rt = t, r->rt_w = w, r->rt_h = h;
    tag_done(p, 26);
}

static void set_pipeline(a64_run_t *r, lagfx_protocol_t *p, const uint8_t *rec, uint32_t n) {
    if (n < 4u) { CNT(p).bad_rec++; tag_unsup(p, 116); return; }
    a64_pipe_t *pp = get_pipe(r, p, r->task, R32(0));
    r->pipe_set = false, r->pipe = NULL;     /* Apple: Metal crashes on a missing pipeline, and on drawing without one */
    if (!pp) { tag_partial(p, 116); return; }
    if (r->rt) {
        VkFormat f = r->rt->format;
        if (!pp->pipe || pp->fmt != f) {
            if (pp->pipe) defer(r, 1, NULL, pp->pipe);   /* an earlier draw (this exec or a pending one) may use it */
            pp->pipe = VK_NULL_HANDLE;
            lagfx_pipeline_desc_t d = pp->src.desc;
            d.color_format = f;
            if (lagfx_pipeline_build(r->vk->device, &d, &pp->pipe) != LAGFX_OK) pp->pipe = VK_NULL_HANDLE;
            pp->fmt = f;
        }
        if (!pp->pipe || lagfx_translate_render_bind_vk_pipeline(&r->rs, pp->pipe, pp->src.desc.layout) != LAGFX_OK) { CNT(p).no_pipeline++; tag_partial(p, 116); return; }
    }
    r->pipe = pp, r->pipe_set = true;
    tag_done(p, 116);
}

static bool draw_ready(a64_run_t *r, lagfx_protocol_t *p) {
    if (!r->pipe_set || !r->pipe) { CNT(p).no_pipeline++; CNT(p).draws_skipped++; return false; }   /* Apple: no pipeline, no draw */
    if (!r->rendering) { CNT(p).no_target++; CNT(p).draws_skipped++; return false; }
    return true;
}

static bool bind_for_draw(a64_run_t *r, lagfx_protocol_t *p) {
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
        if (tag == 110) { tag_partial(p, tag); break; }
        for (uint32_t i = 0; i < count && index + i < A64_MAX_VB; i++) {
            lagfx_a64_obj_t *vb = lagfx_a64_res_object(r->x, R32(8 + i * 12));
            r->vb[index + i].b = vb && vb->kind == LAGFX_A64_BUFFER ? vb : NULL;
            r->vb[index + i].off = lagfx_le64(rec + 12 + i * 12);
        }
        tag_done(p, tag);
        break;
    }
    case 126: case 111:                      /* {index, offset u64} */
        if (n < 12u) { CNT(p).bad_rec++; tag_unsup(p, tag); break; }
        if (tag == 126) {
            uint32_t idx = R32(0);
            if (idx < A64_MAX_VB) r->vb[idx].off = lagfx_le64(rec + 4);
            tag_done(p, tag);
        } else tag_partial(p, tag);
        break;
    case 114: case 112: tag_partial(p, tag); break;   /* fragment textures / samplers: recorded by the guest, not bound (no descriptor sets) */
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
        if (R32(0) != 3u || !R16(6)) { CNT(p).prim_unsupported++; CNT(p).draws_skipped++; tag_partial(p, tag); break; }
        if (!bind_for_draw(r, p)) { tag_partial(p, tag); break; }
        if (lagfx_translate_render_draw(&r->rs, R16(6), 1, R16(4), 0) != LAGFX_OK) { CNT(p).draws_skipped++; tag_partial(p, tag); break; }
        CNT(p).draws++;
        tag_done(p, tag);
        break;
    }
    case 7: {                                /* {primitive u16, index type u16, ref u32, count u16, offset u16} (Apple) */
        if (n < 12u) { CNT(p).bad_rec++; tag_unsup(p, tag); break; }
        if (!draw_ready(r, p)) { tag_partial(p, tag); break; }
        if (R16(0) != 3u || !R16(8)) { CNT(p).prim_unsupported++; CNT(p).draws_skipped++; tag_partial(p, tag); break; }
        uint32_t itype = R16(2), count = R16(8), off = R16(10);
        uint32_t isz = itype == 1u ? 4u : 2u;
        lagfx_a64_obj_t *ib = lagfx_a64_res_object(r->x, R32(4));
        if (itype > 1u || !ib || ib->kind != LAGFX_A64_BUFFER || (uint64_t)off + (uint64_t)count * isz > ib->length) { CNT(p).no_index_buf++; CNT(p).draws_skipped++; tag_partial(p, tag); break; }
        if (!bind_for_draw(r, p)) { tag_partial(p, tag); break; }
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
