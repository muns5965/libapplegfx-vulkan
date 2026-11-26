/*
 * libapplegfx-vulkan — ARM64 GPU exec (0x43 / 0x45): execution of Render segments on Vulkan. See arm64_exec_run.h.
 */
#include "arm64_exec_run.h"
#include "arm64_objects.h"
#include "../device.h"
#include "../common/le.h"
#include "../common/log.h"

#include <stdlib.h>
#include <string.h>

#ifndef LAGFX_HAVE_VULKAN

bool lagfx_a64_exec_enabled(const lagfx_protocol_t *p) { (void)p; return false; }
void lagfx_a64_exec_set_pipeline_resolver(lagfx_protocol_t *p, lagfx_a64_pipeline_resolver_t fn, void *ctx) { (void)p; (void)fn; (void)ctx; }
void lagfx_a64_exec_begin(lagfx_protocol_t *p, lagfx_task_entry_t *task, const uint8_t *res, uint32_t nres) { (void)p; (void)task; (void)res; (void)nres; }
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
#include "../vulkan/render_target.h"
#include "../vulkan/draw_record.h"
#include "../translate/render_encoder.h"
#include "../handlers/compute/render_inner_ops.h"
#include "render_pass.h"
#include <vulkan/vulkan.h>

#define A64_BUF_CAP   (16u << 20)    /* bytes of a guest buffer mirrored on the host */
#define A64_TEX_MAXDIM 8192u
#define A64_MAX_VB    31u
#define A64_WAIT_MS_DEFAULT 2000u   /* env LAGFX_GPU_WAIT_MS (initial test value) */

enum { K_BUF = 1, K_TEX = 2, K_PIPE = 3 };

typedef struct { uint32_t kind; } objhdr_t;
typedef struct {
    objhdr_t h;
    VkBuffer buf; VkDeviceMemory mem; void *map;
    uint64_t size, va;               /* host size; guest virtual address of the contents */
} a64_buf_t;
typedef struct { objhdr_t h; lagfx_vk_render_target_t rt; } a64_tex_t;
typedef struct {
    objhdr_t h;
    lagfx_a64_pipeline_src_t src;
    VkPipeline pipe; VkFormat fmt;   /* built for the colour format of the pass that set it; rebuilt when that changes */
} a64_pipe_t;

typedef struct { VkCommandBuffer cb; VkFence fence; } pending_t;      /* a submission whose fence had not signalled when the wait ended */
typedef struct { int kind; void *obj; VkPipeline pipe; } deferred_t;   /* kind 0 = whole object, 1 = a VkPipeline */

typedef struct {
    lagfx_a64_objects_t *objs;
    struct lagfx_vk_state *vk;
    lagfx_a64_pipeline_resolver_t resolver; void *resolver_ctx;
    /* the exec being recorded */
    bool in_exec, recording;
    VkCommandBuffer cb;
    bool pass_open;                  /* a tag 26 was seen and the segment chain has not ended it */
    bool rendering;                  /* the render encoder has a vkCmdBeginRendering open (pass_open without it = a pass with no usable target) */
    a64_tex_t *rt;
    a64_pipe_t *pipe; bool pipe_set;
    const lagfx_task_entry_t *task;  /* the exec's task */
    bool dispatching, consumed;      /* inside lagfx_render_inner_dispatch for a tag of this module */
    lagfx_translate_render_state_t rs;   /* the existing render encoder: begin / bind / draw / end of the open pass */
    struct { a64_buf_t *b; uint64_t off; } vb[A64_MAX_VB];
    lagfx_draw_region_t region;
    deferred_t *def; uint32_t ndef, capdef;
    pending_t *pend; uint32_t npend, cappend;
} a64_run_t;

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

/* ---- destruction (deferred while a command buffer that may reference the object is being recorded) ---- */

static void destroy_buf(struct lagfx_vk_state *vk, a64_buf_t *b) {
    if (vk) {
        if (b->map) vkUnmapMemory(vk->device, b->mem);
        if (b->buf) vkDestroyBuffer(vk->device, b->buf, NULL);
        if (b->mem) vkFreeMemory(vk->device, b->mem, NULL);
    }
    free(b);
}
static void destroy_pipeline_vk(struct lagfx_vk_state *vk, VkPipeline pl) { if (vk && pl) vkDestroyPipeline(vk->device, pl, NULL); }
static void destroy_obj_now(a64_run_t *r, void *obj) {
    objhdr_t *h = obj;
    switch (h->kind) {
    case K_BUF: destroy_buf(r->vk, obj); break;
    case K_TEX: { a64_tex_t *t = obj; if (r->vk) lagfx_vk_render_target_destroy(r->vk, &t->rt); free(t); break; }
    case K_PIPE: {
        a64_pipe_t *pp = obj;
        destroy_pipeline_vk(r->vk, pp->pipe);
        if (pp->src.release) pp->src.release(pp->src.user);
        free(pp);
        break;
    }
    default: free(obj); break;
    }
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
    if (r->recording || r->npend) defer(r, 0, obj, VK_NULL_HANDLE); else destroy_obj_now(r, obj);   /* in use by recorded or still-running work */
}
static void flush_deferred(a64_run_t *r) {
    for (uint32_t i = 0; i < r->ndef; i++) {
        if (r->def[i].kind) destroy_pipeline_vk(r->vk, r->def[i].pipe); else destroy_obj_now(r, r->def[i].obj);
    }
    r->ndef = 0;
}

/* ---- object creation ---- */

static uint32_t find_mem(struct lagfx_vk_state *vk, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(vk->phys_device, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static bool buf_fill(lagfx_protocol_t *p, const lagfx_task_entry_t *t, a64_buf_t *b) {
    memset(b->map, 0, b->size);
    return lagfx_arm64_task_read(p, t, b->va, b->map, b->size);
}

/* {u64 length, u32 first virtual page (16 KiB), u32 flags} at the entry's address; contents at page << 14 (Apple makeBuffer / map_pages). */
static void *make_buffer(a64_run_t *r, lagfx_protocol_t *p, const lagfx_task_entry_t *t, const uint32_t e[3]) {
    struct lagfx_vk_state *vk = r->vk;
    uint8_t h[16];
    uint64_t at = (uint64_t)e[2] << 32 | e[1];
    if (!vk || !lagfx_arm64_task_read(p, t, at, h, sizeof h)) return NULL;
    uint64_t len = lagfx_le64(h);
    if (!len) return NULL;
    if (len > A64_BUF_CAP) len = A64_BUF_CAP;
    a64_buf_t *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->h.kind = K_BUF, b->size = len, b->va = (uint64_t)lagfx_le32(h + 8) << 14;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = len,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkMemoryRequirements mr;
    if (vkCreateBuffer(vk->device, &bi, NULL, &b->buf) != VK_SUCCESS) goto fail;
    vkGetBufferMemoryRequirements(vk->device, b->buf, &mr);
    uint32_t mt = find_mem(vk, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) goto fail;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt };
    if (vkAllocateMemory(vk->device, &ai, NULL, &b->mem) != VK_SUCCESS) goto fail;
    if (vkBindBufferMemory(vk->device, b->buf, b->mem, 0) != VK_SUCCESS) goto fail;
    if (vkMapMemory(vk->device, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map) != VK_SUCCESS) { b->map = NULL; goto fail; }
    buf_fill(p, t, b);   /* an unreadable page leaves zeros: the buffer still exists, like Apple's mapping of unmapped pages */
    return b;
fail:
    destroy_buf(vk, b);
    return NULL;
}

static VkFormat vk_format_of(uint32_t mtl) {
    switch (mtl) {
    case 10:  return VK_FORMAT_R8_UNORM;
    case 70:  return VK_FORMAT_R8G8B8A8_UNORM;
    case 71:  return VK_FORMAT_R8G8B8A8_SRGB;
    case 80:  return VK_FORMAT_B8G8R8A8_UNORM;
    case 81:  return VK_FORMAT_B8G8R8A8_SRGB;
    case 115: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default:  return VK_FORMAT_UNDEFINED;
    }
}

/* The two serialized texture descriptions (Apple descriptor_v1 / descriptor_v2): only what a 2D single-sample render target needs. */
static bool tex_desc(const uint8_t *d, size_t n, bool v2, uint32_t *w, uint32_t *h, uint32_t *fmt) {
    uint32_t type = d[0] & 0xfu, samples;
    if (v2) {
        if (n < 0x17) return false;
        *fmt = d[1] | (uint32_t)d[2] << 8, *w = lagfx_le32(d + 7), *h = lagfx_le32(d + 11);
        samples = (uint32_t)d[21] | (uint32_t)d[22] << 8;
    } else {
        if (n < 20) return false;
        *fmt = d[2] | (uint32_t)d[3] << 8, *w = lagfx_le32(d + 4), *h = lagfx_le32(d + 8);
        samples = (uint32_t)d[18] | (uint32_t)d[19] << 8;
    }
    return type == 2u && samples <= 1u;
}

static void *make_texture(a64_run_t *r, lagfx_protocol_t *p, const lagfx_task_entry_t *t, const uint32_t e[3]) {
    uint32_t type = e[0] & 0xffu, size = e[0] >> 8;
    uint64_t at = (uint64_t)e[2] << 32 | e[1];
    uint8_t d[512];
    if (!r->vk || size < 16u || size > sizeof d) return NULL;
    memset(d, 0, sizeof d);
    if (!lagfx_arm64_task_read(p, t, at, d, size)) return NULL;
    const uint8_t *desc;
    size_t dn;
    bool v2;
    if (type == LAGFX_A64_OBJ_TEXTURE) {   /* {length, page}, layout table, {tag, size, ref}, description */
        uint32_t mips = (uint32_t)(d[12] | d[13] << 8) & 0x3fffu, table = mips * 36u;
        uint32_t rec = 12u + 0x18u + table;
        if (rec + 12u > size) return NULL;
        uint32_t tag = lagfx_le32(d + rec);
        v2 = tag == 0x34u || tag == 0x39u;
        desc = d + rec + 12u, dn = size - rec - 12u;
    } else {                               /* type 8/9: {tag, size, ref, description} without guest memory is the only form handled */
        uint32_t tag = lagfx_le32(d);
        if (tag != 0x34u && tag != 0x14u && tag != 1u) return NULL;
        v2 = tag == 0x34u;
        desc = d + 12, dn = size - 12u;
    }
    uint32_t w, h, mtl;
    if (!tex_desc(desc, dn, v2, &w, &h, &mtl) || !w || !h || w > A64_TEX_MAXDIM || h > A64_TEX_MAXDIM) return NULL;
    VkFormat f = vk_format_of(mtl);
    if (f == VK_FORMAT_UNDEFINED) return NULL;
    a64_tex_t *tx = calloc(1, sizeof *tx);
    if (!tx) return NULL;
    tx->h.kind = K_TEX;
    if (lagfx_vk_render_target_create(r->vk, w, h, f, &tx->rt) != LAGFX_OK) { free(tx); return NULL; }
    return tx;
}

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
    switch (e[0] & 0xffu) {
    case LAGFX_A64_OBJ_BUFFER: case LAGFX_A64_OBJ_BUFFER_V2: return make_buffer(r, p, t, e);
    case LAGFX_A64_OBJ_TEXTURE: case LAGFX_A64_OBJ_LINEAR_TEX: case 9: {
        void *o = make_texture(r, p, t, e);
        if (!o) CNT(p).obj_unsupported++;
        return o;
    }
    case LAGFX_A64_OBJ_SERIALIZED: return make_pipeline(r, p, t, ref, e);
    default: CNT(p).obj_unsupported++; return NULL;
    }
}

static a64_run_t *run_get(lagfx_protocol_t *p, bool create) {
    if (p->a64_exec || !create) return RUN(p);
    a64_run_t *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    lagfx_a64_obj_ops_t ops = { r, obj_create_cb, obj_destroy_cb };
    r->objs = lagfx_a64_objects_new(&ops);
    if (!r->objs) { free(r); return NULL; }
    p->a64_exec = r;
    return r;
}

static void *get_obj(a64_run_t *r, lagfx_protocol_t *p, const lagfx_task_entry_t *t, uint32_t ref, int kind) {
    void *o = lagfx_a64_objects_get(r->objs, p, t, ref, NULL);
    return (o && ((objhdr_t *)o)->kind == (uint32_t)kind) ? o : NULL;
}

static uint32_t gpu_wait_ms(void);
static void reap(a64_run_t *r);

/* ---- API: configuration and lifetime ---- */

void lagfx_a64_exec_set_pipeline_resolver(lagfx_protocol_t *p, lagfx_a64_pipeline_resolver_t fn, void *ctx) {
    if (!p) return;
    a64_run_t *r = run_get(p, true);
    if (r) r->resolver = fn, r->resolver_ctx = ctx;
}

void lagfx_a64_exec_drop_task(lagfx_protocol_t *p, uint32_t task_id) {
    a64_run_t *r = p ? RUN(p) : NULL;
    if (!r) return;
    r->vk = get_vk(p);
    lagfx_a64_objects_drop_task(r->objs, task_id);
}

void lagfx_a64_exec_free(lagfx_protocol_t *p) {
    a64_run_t *r = p ? RUN(p) : NULL;
    if (!r) return;
    r->vk = get_vk(p);
    if (r->recording) {                       /* an exec was abandoned half-way: nothing was submitted, just release the buffer */
        if (r->rendering) lagfx_translate_render_end(&r->rs);
        vkEndCommandBuffer(r->cb);
        lagfx_vk_cmdbuf_free(r->vk, r->cb);
        r->recording = false;
    }
    if (r->vk && r->npend) {                  /* give submissions that are still running one bounded wait; what is not done is leaked, never freed under the GPU */
        for (uint32_t i = 0; i < r->npend; i++) vkWaitForFences(r->vk->device, 1, &r->pend[i].fence, VK_TRUE, (uint64_t)gpu_wait_ms() * 1000000ull);
        reap(r);
    }
    lagfx_a64_objects_free(r->objs);          /* destroys the objects through obj_destroy_cb (parked while work is outstanding) */
    if (!r->npend) flush_deferred(r);
    free(r->pend);
    free(r->def);
    free(r);
    p->a64_exec = NULL;
}

void lagfx_a64_exec_object_counts(const lagfx_protocol_t *p, uint32_t *live, uint64_t *created, uint64_t *destroyed) {
    lagfx_a64_obj_stats_t st;
    a64_run_t *r = p ? RUN(p) : NULL;
    lagfx_a64_objects_stats(r ? r->objs : NULL, &st);
    if (live) *live = st.live;
    if (created) *created = st.created;
    if (destroyed) *destroyed = st.destroyed;
}

bool lagfx_a64_exec_readback_texture(lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint32_t ref, void *dst, size_t dst_size, uint32_t *width, uint32_t *height) {
    a64_run_t *r = p ? RUN(p) : NULL;
    if (!r) return false;
    r->vk = get_vk(p);
    a64_tex_t *t = r->vk ? get_obj(r, p, task, ref, K_TEX) : NULL;
    if (!t) return false;
    size_t stride = 0;
    if (lagfx_vk_render_target_readback(r->vk, &t->rt, dst, dst_size, &stride) != LAGFX_OK) return false;   /* 1 s bounded wait, result checked */
    if (width) *width = t->rt.width;
    if (height) *height = t->rt.height;
    return true;
}

/* ---- the exec ---- */

static void tag_done(lagfx_protocol_t *p, uint32_t tag) { if (tag < 512u) CNT(p).tag_done[tag]++; else CNT(p).tag_over++; }
static void tag_partial(lagfx_protocol_t *p, uint32_t tag) { if (tag < 512u) CNT(p).tag_partial[tag]++; else CNT(p).tag_over++; }
static void tag_unsup(lagfx_protocol_t *p, uint32_t tag) { if (tag < 512u) CNT(p).tag_unsup[tag]++; else CNT(p).tag_over++; }

void lagfx_a64_exec_begin(lagfx_protocol_t *p, lagfx_task_entry_t *task, const uint8_t *res, uint32_t nres) {
    if (!p || !lagfx_a64_exec_enabled(p)) return;
    a64_run_t *r = run_get(p, true);
    if (!r) return;
    CNT(p).execs++;
    r->vk = get_vk(p);
    if (!r->vk) { CNT(p).no_vulkan++; r->in_exec = false; return; }
    reap(r);   /* earlier pending submissions may have finished */
    r->in_exec = true;
    r->task = task;
    r->pass_open = r->rendering = false, r->rt = NULL, r->pipe = NULL, r->pipe_set = false;
    memset(r->vb, 0, sizeof r->vb);
    memset(&r->region, 0, sizeof r->region);
    if (!task || !res) return;
    for (uint32_t i = 0; i < nres; i++) {
        uint32_t ref = lagfx_le32(res + 8u * i), fl = lagfx_le32(res + 8u * i + 4);
        void *o = lagfx_a64_objects_get(r->objs, p, task, ref, NULL);
        if (o && ((objhdr_t *)o)->kind == K_BUF && (fl & 1u)) buf_fill(p, task, o);   /* flag 1: the guest wrote it since */
    }
}

static bool ensure_cb(a64_run_t *r) {
    if (r->recording) return true;
    if (lagfx_vk_cmdbuf_alloc(r->vk, &r->cb) != LAGFX_OK) return false;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (vkBeginCommandBuffer(r->cb, &bi) != VK_SUCCESS) { lagfx_vk_cmdbuf_free(r->vk, r->cb); return false; }
    r->recording = true;
    return true;
}

static void pass_end(a64_run_t *r) {
    if (r->rendering) lagfx_translate_render_end(&r->rs);
    r->pass_open = r->rendering = false;
    r->rt = NULL, r->pipe = NULL, r->pipe_set = false;
}

#define R32(o) lagfx_le32(rec + (o))
#define R16(o) ((uint32_t)rec[(o)] | (uint32_t)rec[(o) + 1] << 8)
static double rd_f64(const uint8_t *b) { uint64_t u = lagfx_le64(b); double d; memcpy(&d, &u, 8); return d; }

/* tag 26 (inner opcode 0x1a): the 584-byte pass descriptor, parsed by the existing lagfx_parse_render_pass_descriptor (render_pass.c). Only colour
 * attachment 0 is rendered to; begin/end of the dynamic rendering go through the existing render encoder (translate/render_encoder.c). */
static void begin_pass(a64_run_t *r, lagfx_protocol_t *p, const uint8_t *rec, uint32_t n) {
    const lagfx_task_entry_t *task = r->task;
    pass_end(r);
    CNT(p).passes++;
    r->pass_open = true;                        /* even without a usable target: the following tags belong to this pass */
    memset(r->vb, 0, sizeof r->vb);
    memset(&r->region, 0, sizeof r->region);
    lagfx_render_pass_full_desc_t d;
    if (n < LAGFX_RENDER_PASS_WIRE_SIZE || lagfx_parse_render_pass_descriptor(rec, n, &d) != 0) { CNT(p).bad_rec++; tag_unsup(p, 26); return; }
    if (d.color_attachment_count > 1u || d.has_depth || d.has_stencil) CNT(p).extra_attach_ignored++;
    a64_tex_t *t = R32(0x4c) ? get_obj(r, p, task, d.colors[0].texture_ref, K_TEX) : NULL;   /* the parser compacts: only index 0 counts here */
    if (!t) { CNT(p).no_target++; tag_partial(p, 26); return; }
    if (!ensure_cb(r)) { CNT(p).no_vulkan++; tag_partial(p, 26); return; }
    uint32_t load = d.colors[0].load_action;   /* MTLLoadAction: 0 DontCare, 1 Load, 2 Clear */
    VkAttachmentLoadOp lop = load == 2u ? VK_ATTACHMENT_LOAD_OP_CLEAR : load == 1u ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    VkClearValue cv;
    memset(&cv, 0, sizeof cv);
    if (lop == VK_ATTACHMENT_LOAD_OP_LOAD && t->rt.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        /* nothing was ever drawn or uploaded: Load would show garbage, so start from transparent black (guest upload of texture contents is not done) */
        lop = VK_ATTACHMENT_LOAD_OP_CLEAR;
        CNT(p).load_uninit_cleared++;
    } else if (lop == VK_ATTACHMENT_LOAD_OP_CLEAR) {
        for (int c = 0; c < 4; c++) cv.color.float32[c] = (float)d.colors[0].clear_color[c];
    }
    lagfx_vk_cmd_rt_to_color(r->cb, &t->rt);   /* existing barrier helper of draw_record.c */
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = t->rt.view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = lop, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = cv };
    if (lagfx_translate_render_begin(r->vk, &r->rs, r->cb, t->rt.image, t->rt.width, t->rt.height, NULL, &ca, 1) != LAGFX_OK) {
        CNT(p).no_target++; tag_partial(p, 26); return;
    }
    r->rendering = true, r->rt = t;
    tag_done(p, 26);
}

static void set_pipeline(a64_run_t *r, lagfx_protocol_t *p, const uint8_t *rec, uint32_t n) {
    if (n < 4u) { CNT(p).bad_rec++; tag_unsup(p, 116); return; }
    a64_pipe_t *pp = get_obj(r, p, r->task, R32(0), K_PIPE);
    r->pipe_set = false, r->pipe = NULL;     /* Apple: Metal crashes on a missing pipeline, and on drawing without one */
    if (!pp) { tag_partial(p, 116); return; }
    if (r->rt) {
        VkFormat f = r->rt->rt.format;
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
    lagfx_vk_cmd_set_region(r->cb, &r->rt->rt, &r->region);   /* existing viewport/scissor helper of draw_record.c (Y-flip, clamping) */
    if (r->pipe->src.desc.n_vtx_inputs) {
        uint32_t s = r->pipe->src.vertex_buffer_index;
        if (s >= A64_MAX_VB || !r->vb[s].b || r->vb[s].off >= r->vb[s].b->size) { CNT(p).no_vertex_buf++; CNT(p).draws_skipped++; return false; }
        lagfx_translate_render_bind_vertex_buffer(&r->rs, r->vb[s].b->buf, r->vb[s].off);
    }
    return true;
}

/* One supported tag, already inside an open pass. Reached from the existing render_inner_ops.c handlers through lagfx_a64_exec_render_op. */
static void render_tag(a64_run_t *r, lagfx_protocol_t *p, uint32_t tag, const uint8_t *rec, uint32_t n) {
    const lagfx_task_entry_t *task = r->task;
    switch (tag) {
    case 26: begin_pass(r, p, rec, n); break;
    case 116: set_pipeline(r, p, rec, n); break;
    case 125: case 110: {                    /* {index, count}, {ref, offset u64}... */
        if (n < 8u) { CNT(p).bad_rec++; tag_unsup(p, tag); break; }
        uint32_t index = R32(0), count = R32(4);
        if (count > A64_MAX_VB || (uint64_t)n < 8u + 12ull * count) { CNT(p).bad_rec++; tag_unsup(p, tag); break; }
        if (tag == 110) { tag_partial(p, tag); break; }
        for (uint32_t i = 0; i < count && index + i < A64_MAX_VB; i++) {
            r->vb[index + i].b = get_obj(r, p, task, R32(8 + i * 12), K_BUF);
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
        a64_buf_t *ib = get_obj(r, p, task, R32(4), K_BUF);
        if (itype > 1u || !ib || (uint64_t)off + (uint64_t)count * isz > ib->size) { CNT(p).no_index_buf++; CNT(p).draws_skipped++; tag_partial(p, tag); break; }
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
    if (!r || !r->in_exec || !buf || !s) return;
    (void)len;
    r->task = task;
    if (!s->cont) pass_end(r);                                  /* Apple: a segment that does not continue closes the encoder */
    if (s->type != 0u) { CNT(p).seg_unsupported++; return; }    /* compute (1), blit (2), information (4) and the rest: counted, not executed */
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

/* ---- submission: bounded wait; "not finished yet" is PENDING, not failed ---- */

static uint32_t gpu_wait_ms(void) {
    const char *e = getenv("LAGFX_GPU_WAIT_MS");   /* default 2000 (initial test value) */
    if (e && e[0]) { unsigned long v = strtoul(e, NULL, 10); if (v) return (uint32_t)v; }
    return A64_WAIT_MS_DEFAULT;
}

/* Frees the submissions whose fence has signalled; once none is outstanding, the parked objects can go too. */
static void reap(a64_run_t *r) {
    uint32_t keep = 0;
    for (uint32_t i = 0; i < r->npend; i++) {
        VkResult s = vkGetFenceStatus(r->vk->device, r->pend[i].fence);
        if (s == VK_NOT_READY) { r->pend[keep++] = r->pend[i]; continue; }
        vkDestroyFence(r->vk->device, r->pend[i].fence, NULL);
        lagfx_vk_cmdbuf_free(r->vk, r->pend[i].cb);
    }
    r->npend = keep;
    if (!r->npend && !r->recording) flush_deferred(r);
}

uint32_t lagfx_a64_exec_poll(lagfx_protocol_t *p) {
    a64_run_t *r = p ? RUN(p) : NULL;
    if (!r) return 0;
    r->vk = get_vk(p);
    if (r->vk) reap(r);
    return r->npend;
}

lagfx_a64_exec_status_t lagfx_a64_exec_end(lagfx_protocol_t *p) {
    a64_run_t *r = p ? RUN(p) : NULL;
    if (!r || !r->in_exec) return LAGFX_A64_EXEC_NONE;
    r->in_exec = false;
    pass_end(r);
    if (!r->recording) { reap(r); return LAGFX_A64_EXEC_NONE; }
    uint64_t ms = gpu_wait_ms();
    lagfx_a64_exec_status_t st = LAGFX_A64_EXEC_DONE;
    VkFence fence = VK_NULL_HANDLE;
    bool submitted = false;
    if (vkEndCommandBuffer(r->cb) == VK_SUCCESS) {
        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &r->cb };
        submitted = vkCreateFence(r->vk->device, &fci, NULL, &fence) == VK_SUCCESS && vkQueueSubmit(r->vk->graphics_queue, 1, &si, fence) == VK_SUCCESS;
    }
    r->recording = false;
    if (!submitted) {                         /* only a failure BEFORE the work is on the queue is "failed" */
        if (fence) vkDestroyFence(r->vk->device, fence, NULL);
        lagfx_vk_cmdbuf_free(r->vk, r->cb);
        CNT(p).submit_failed++;
        LAGFX_WARN("arm64 exec: Vulkan record/submit failed before the GPU saw the work");
        reap(r);
        return LAGFX_A64_EXEC_FAILED;
    }
    CNT(p).submits++;
    VkResult wr = vkWaitForFences(r->vk->device, 1, &fence, VK_TRUE, ms * 1000000ull);   /* bounded, result checked */
    if (wr == VK_SUCCESS) {
        vkDestroyFence(r->vk->device, fence, NULL);
        lagfx_vk_cmdbuf_free(r->vk, r->cb);
    } else {
        /* Not finished (or the wait itself errored): the work may still be running, so the command buffer, the fence and everything the work uses
         * (the deferred objects included) stay alive until the fence signals; lagfx_a64_exec_poll / the next exec reaps them. Not a failure. */
        if (r->npend == r->cappend) {
            uint32_t nc = r->cappend ? r->cappend * 2u : 8u;
            pending_t *n = realloc(r->pend, nc * sizeof *n);
            if (n) r->pend = n, r->cappend = nc;
        }
        if (r->npend < r->cappend) r->pend[r->npend++] = (pending_t){ r->cb, fence };
        else CNT(p).pending_leaked++;         /* out of memory: leaking is the only safe choice */
        CNT(p).submit_pending++;
        st = LAGFX_A64_EXEC_PENDING;
        LAGFX_LOG("arm64 exec: not finished after %llu ms (wait result %d): kept pending", (unsigned long long)ms, (int)wr);
    }
    reap(r);
    return st;
}

#endif /* LAGFX_HAVE_VULKAN */
