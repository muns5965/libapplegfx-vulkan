/*
 * libapplegfx-vulkan — ARM64 exec: guest-memory-backed object mirror + Blit (segment type 2) executor.
 * src/protocol/arm64_resources.h
 *
 * Active ONLY for protocol_mode LAGFX_PROTO_ARM64_VMAPPLE and ONLY when the environment has LAGFX_ARM64_EXEC=1 (default off: the
 * 0x43/0x45 handler then stays decode-only, exactly as before). x86 mode never reaches this code.
 *
 * Reference: Apple's devices/pvg_metal.m exec (resource flags, refresh/page:toGuest:) and blit().
 *
 * ---- Object mirror ------------------------------------------------------------------------------------------------------
 * A guest ref names an object-table entry {type | size << 8, addr lo, addr hi} (task->objects_addr + ref * 12). Supported:
 *   type 1 and 19  buffer   {u64 length, u32 vpage, u32 flags}; the guest bytes live at task VA vpage << 14, length rounded up to 16 KiB
 *   type 2         texture  {u64 length, u32 vpage, layout{u16 mips, u16 slices, u64 base, u64 stride, mip table of 36 bytes}, description}
 *                  uncompressed, non-MSAA 2D / 2D array / 3D, formats in the table of arm64_resources.c
 *                type 8/9 linear 2D texture {tag, size, ref, description} without guest pages: a device image only (render target / blit), no write-back
 *   type 11        mapped texture {u64 mapping id, u32 tag, u32 size, u32 ref, description}: the pixels live in a guest display SURFACE whose descriptor the shell
 *                  supplies (lagfx_shell_callbacks_t.get_surface_desc; B8G8R8A8 -> B8G8R8A8, 'RGhA' -> R16G16B16A16_SFLOAT, 8 bytes per pixel, raw copies). ONE device image of the SURFACE's size per mapping id, shared by every ref /
 *                  task naming that id, GENERAL layout, usable as colour attachment and sampled. Guest -> image: the surface pages are read (shell read_memory, honouring the
 *                  row pitch and first-byte offset) into a staging buffer on the flag rules below; image -> guest: flag 0x04 / sync ops write the pixels back (shell
 *                  write_memory), missing pages are skipped and counted. Counters: diag2.exec.run.mapped_*.
 * Not supported (resolve returns NULL, blit using them is counted as 'bad_object'): heaps (13/14),
 * cube/1D/MSAA/compressed textures, type 6/7 objects; depth / stencil textures (type 2, formats 250 252 253 255 260) are device-only render attachments (lagfx_a64_res_ds_begin), mapped surfaces that are not BGRA.
 * Each supported object owns a host-visible "guest copy" VkBuffer holding the bytes in the GUEST layout (for a buffer it IS the object),
 * and a texture additionally a device VkImage kept in VK_IMAGE_LAYOUT_GENERAL between uses.
 *
 * ---- Flag rules (per resource of an exec, Apple order) -------------------------------------------------------------------
 *   0x01 guest wrote it : guest_stale = false, host_valid = false     -> the guest pages are uploaded before this exec runs
 *   host_valid == false : upload the guest pages unless guest_stale   -> a stale guest copy NEVER overwrites GPU output
 *   0x02 GPU changes it : guest_stale = true (after the uploads)      -> later execs without 0x01 do not upload
 *   0x04 sync back      : after the GPU work completed, the object is written to the guest pages, guest_stale = false
 * Textures follow this exactly (guest_backed). Buffers emulate Apple's zero-copy shared mapping: they are re-read from the guest at the start
 * of every exec unless guest_stale (the guest sees GPU writes only after 0x04 or lagfx_a64_res_sync_to_guest; stale protects them till then).
 * Missing/unmapped guest pages read as zero and are skipped on write-back (counted in stats.unmapped_pages).
 *
 * ---- Interface for the Render / Sync agents -------------------------------------------------------------------------------
 *   x = lagfx_a64_res_begin(p, task, res, nres)   res = the wire array {u32 ref, u32 flags}[nres]; applies the flag rules, records texture
 *                                                  uploads into x's command buffer. NULL if disabled / not arm64 / no Vulkan / no task.
 *   lagfx_a64_res_cmdbuf(x)                       the recording VkCommandBuffer (render work may be recorded into it AFTER begin;
 *                                                  objects are in GENERAL layout, memory barriers are the recorder's job)
 *   lagfx_a64_res_object(x, ref)                  resolve a ref (also for refs outside the resource list; created + uploaded on first use)
 *   lagfx_a64_res_blit_segment(x, body, len)      body = bytes after the segment header(s); each command goes through the EXISTING lagfx_blit_inner_dispatch
                                                  (blit_inner_ops.c) whose handlers 0x12c-0x12f/0x132 call lagfx_a64_blit_op in arm64 mode
 *   lagfx_a64_res_end(x)                          records flag-0x04 write-backs, submits, waits (bounded) for the fence; DONE -> writes the guest pages, frees x.
 *                                                  Complete the stamp only on DONE; PENDING -> keep x, poll (see lagfx_a64_status_t).
 *   lagfx_a64_res_sync_to_guest(p, task, refs, n)  download objects to the guest now (Apple's synchronize(): host_valid objects only)
 * One exec at a time (the compute channel is serial); not thread safe.
 */
#ifndef LAGFX_PROTOCOL_ARM64_RESOURCES_H
#define LAGFX_PROTOCOL_ARM64_RESOURCES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "state.h"
#include "opcodes.h"
#include "../vulkan/instance.h"

#define LAGFX_A64_MAX_MIPS    16u
#define LAGFX_A64_MAX_LAYERS  2048u
#define LAGFX_A64_MAX_OBJ_BYTES (256ull << 20)

typedef struct {
    uint64_t execs, res, uploads, downloads, stale_skips, unmapped_pages, bad_object;
    uint64_t sync_cmds, sync_objs, sync_skipped, inv_cmds, inv_objs, inv_missing, hint_cmds, res_cmd_bad;   /* ops 0x35/0x3e, 0x34, 0x3f */
    uint64_t gpu_timeout, gpu_fail, busy_skipped;   /* gpu_timeout: bounded wait ran out (pending, not failed); gpu_fail: a Vulkan call failed or the device was lost; busy_skipped: begin refused while the previous exec was still pending */
    uint64_t blit_seen, blit_done, blit_rejected, blit_noop, blit_unsupported;   /* per command */
    uint64_t linear_made, linear_unsupported, linear_resync;   /* type 8/9 linear textures mirrored (device image only) / that could not be */
    uint64_t obj_slots, obj_table_full;   /* object table size in slots (holes of released objects are reused) / creations refused because all 4096 slots were live */
    uint64_t created, destroyed, deferred_frees;   /* mirror objects built / destroyed; frees parked until the in-flight fence signalled */
    uint64_t blit_tag[16];   /* done, indexed tag - 300 (300..315) */
} lagfx_a64_stats_t;

typedef struct lagfx_a64_rexec lagfx_a64_rexec_t;

/* DONE: the GPU finished (guest write-backs done). PENDING: submitted, fence not signalled within LAGFX_GPU_WAIT_MS (default 2000) - NOT a failure:
 * the exec object and every resource it uses are kept alive and untouched, nothing is written to the guest; the caller must not complete the stamp
 * and calls lagfx_a64_res_poll() until DONE. FAILED: a Vulkan failure (before submit nothing is in flight; after a DEVICE_LOST the resources are leaked). */
typedef enum { LAGFX_A64_DONE = 0, LAGFX_A64_PENDING = 1, LAGFX_A64_FAILED = 2 } lagfx_a64_status_t;
typedef struct lagfx_a64_obj lagfx_a64_obj_t;
struct lagfx_a64_msurf;   /* shared device image of one guest surface (mapping id), private to arm64_resources.c */

#ifdef LAGFX_HAVE_VULKAN
enum { LAGFX_A64_BUFFER = 1, LAGFX_A64_TEXTURE = 2 };

struct lagfx_a64_obj {
    uint32_t task, ref, entry[3];
    int      kind;
    uint64_t gen;                       /* unique per creation (a replaced object is a new one): cache keys of the Render executor include it */
    bool     leak;                      /* creation hit a GPU timeout: never freed */
    bool     host_valid, guest_stale;
    bool     has_pages;                 /* vpage != 0 */
    struct lagfx_a64_msurf *ms; uint32_t mapping;       /* type 11: the shared surface image (host_valid / guest_stale of the object live in ms); has_pages stays false */
    bool     linear; uint32_t lin_buf; uint64_t lin_off, lin_row;   /* type 8/9 over buffer ref lin_buf: bytes at lin_off, row pitch lin_row (see lin_xfer) */
    uint64_t wgen;                      /* buffer: bumped at every write the mirror knows of (guest upload, blit / fill / texture->buffer copy, linear push, shader-writable bind, tile kernel) */
    uint64_t rd_epoch; bool gpu_dirty;  /* buffer: exec epoch of the last read from the guest's pages; written by the GPU (blit / push / shader / tile) since that read */
    VkImageView view3;                  /* sampled VK_IMAGE_VIEW_TYPE_3D view of a 3D texture (level 0); lazily made */
    VkImageView sview;                  /* sampled view when it differs from view (A8Unorm: components 0,0,0,R); lazily made */
    uint64_t lin_seen; bool lin_pulled; /* linear texture: wgen of the backing buffer when the image last took (pulled) or gave (pushed) the buffer's bytes; image is current iff lin_pulled && lin_seen == buffer wgen */
    uint64_t length;                    /* bytes, rounded up to 16 KiB */
    uint64_t vpage;                     /* guest VA = vpage << 14 */
    VkBuffer buf; VkDeviceMemory buf_mem; uint8_t *map;     /* guest-layout copy (the object itself for a buffer) */
    VkImage  img; VkDeviceMemory img_mem;                   /* textures */
    uint8_t  ds_aspects;                                    /* depth / stencil attachment texture (bit 0 depth, bit 1 stencil): a device image only, see lagfx_a64_res_ds_begin */
    uint8_t  samples, samples_req;                          /* multisample (arm64_msaa.c): samples = VkSampleCountFlagBits value of the image (0 = 1), samples_req = the guest descriptor's count (> 1 marks a device-only multisample texture, also when it fell back to 1x) */
    VkFormat format; VkImageType img_type; VkImageUsageFlags usage; VkImageView view;   /* view: lazily made, 2D layer 0 level 0 (render target) */
    uint32_t bpp, width, height, depth, mips, layers;
    uint32_t mtl_format;                                    /* Metal pixel format number of the guest descriptor (0 unknown); diagnostics / capture only */
    uint64_t tex_base, tex_stride; uint32_t tex_block_h, tex_mip_count;
    struct { uint64_t off, row; uint32_t w, h, d; } mip[LAGFX_A64_MAX_MIPS];
};

VkCommandBuffer lagfx_a64_res_cmdbuf(lagfx_a64_rexec_t *x);
/* Render targets: the Render executor draws into the SAME mirror texture the blits use. target_begin records the image-layout transition GENERAL ->
 * COLOR_ATTACHMENT_OPTIMAL (+ memory barrier: earlier transfer/shader writes -> attachment access) and returns the view and the level-0 extent; false if the
 * texture cannot be a colour target (3D, no attachment usage). target_end records COLOR_ATTACHMENT_OPTIMAL -> GENERAL (attachment write -> transfer / shader
 * read+write), so a following Blit (or a later Render pass) in the same command buffer sees it. */
bool lagfx_a64_res_target_begin(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o, VkImageView *view, uint32_t *w, uint32_t *h);
/* Depth / stencil render targets (Metal 250 / 252 / 253 / 255 / 260 textures): a device image only (no guest pages, no blit, no sampling, no readback). ds_begin
 * returns the view over all aspects of the format and the level-0 extent and orders earlier writes before attachment access; the image stays in GENERAL. */
bool lagfx_a64_res_ds_begin(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o, VkImageView *view, uint32_t *w, uint32_t *h);
/* The VkFormats the mirror can create, with their Metal pixel-format numbers (colour table, then depth / stencil). Returns the total count, copies at most `cap`. */
size_t lagfx_a64_res_format_list(VkFormat *vk, uint32_t *mtl, size_t cap);
void lagfx_a64_res_ds_end(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o);
/* The 2D layer-0 level-0 view of a sampled texture (GENERAL layout, usable as SAMPLED_IMAGE in the shaders); VK_NULL_HANDLE if the format cannot be sampled / 3D. */
/* Linear (buffer backed) textures, sampled: Metal reflects later buffer changes in the texture, the mirror image is a copy. lagfx_a64_res_sync_linear re-pulls every linear
 * texture of the exec's resource list whose backing buffer changed since its last pull (call OUTSIDE a render pass: it records copies); lagfx_a64_res_linear_fresh says
 * whether the image is current right now (a change inside the open pass makes it stale until the next sync: the draw is skipped, never drawn with old bytes);
 * lagfx_a64_res_buf_written records a write the mirror cannot see happening (shader-writable binding, tile kernel). */
/* lagfx_a64_res_prepare_sampled: a texture a render encoder binds for sampling that the exec's resource list did not name. Call OUTSIDE a render pass (it records
 * copies): resolves (creates) the object, takes the guest's bytes on first use, and for a linear texture makes the image current: the backing buffer is re-read from
 * the guest's pages once per exec unless the GPU wrote it since (assumption, unverified on a device: the guest may change a shared buffer at any time) and the image is
 * pulled when the buffer changed. false = no usable texture (the creation failure is memoed for lagfx_a64_res_diagnose). */
bool lagfx_a64_res_prepare_sampled(lagfx_a64_rexec_t *x, uint32_t ref);
void lagfx_a64_res_sync_linear(lagfx_a64_rexec_t *x);
bool lagfx_a64_res_linear_fresh(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o);
void lagfx_a64_res_buf_written(lagfx_a64_obj_t *b);
VkImageView lagfx_a64_res_sample_view(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o);   /* 2D textures only (a 3D one yields VK_NULL_HANDLE) */
/* The sampled view of a 3D mirror texture (VK_IMAGE_VIEW_TYPE_3D, level 0): feeds a texture3d (Dim3D) descriptor. VK_NULL_HANDLE for a non-3D object or one without sampled usage. */
VkImageView lagfx_a64_res_sample_view3d(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o);
bool lagfx_a64_res_is_3d(const lagfx_a64_obj_t *o);
VkImageView lagfx_a64_res_storage_view(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o);
void lagfx_a64_res_target_end(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o);
/* A host-visible storage buffer (zeroed, persistently mapped) owned by the exec and freed with it, e.g. an aligned copy of a misaligned buffer range. */
bool lagfx_a64_res_scratch(lagfx_a64_rexec_t *x, VkDeviceSize size, VkBuffer *buf, uint8_t **map);
/* ---- render replay capture support (arm64_capture.c, LAGFX_ARM64_DUMP_DATA=1 only: nothing here is called otherwise) ----
 * Snapshots are GPU copies recorded into the exec's command buffer (so they see what earlier execs and earlier passes of this exec rendered, never the stale CPU
 * mirror); the host reads them only after the fence signalled, from the finish hook. The recorder must be OUTSIDE a render pass (images are in GENERAL layout there). */
typedef struct { const uint8_t *data; uint64_t bytes, full, off; } lagfx_a64_snap_t;   /* data valid until the finish hook returns */
/* buffer bytes [off, off+min(max_bytes, length-off)) */
bool lagfx_a64_res_snap_buffer(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o, uint64_t off, uint64_t max_bytes, lagfx_a64_snap_t *s);
/* texture: tight = level 0 / layer 0, rows packed (w*bpp); else the whole object in its guest (mirror) layout, prefix of max_bytes (needs guest pages: false otherwise) */
bool lagfx_a64_res_snap_texture(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o, bool tight, uint64_t max_bytes, lagfx_a64_snap_t *s);
/* fn(ctx, true) runs once when the exec's fence signalled (before its resources are released, snapshots readable); fn(ctx, false) when the exec is dropped without that. */
void lagfx_a64_res_set_finish_hook(lagfx_a64_rexec_t *x, void (*fn)(void *ctx, bool done), void *ctx);
/* The task entry's mirror object (NULL if never created); no side effects. For tests / diagnostics. */
lagfx_a64_obj_t *lagfx_a64_res_lookup(lagfx_protocol_t *p, uint32_t task, uint32_t ref);
/* Test / debug: level 0, layer 0 of a mirrored texture as linear rows (width*bpp) via a one-shot copy; false while an exec records / is pending. */
bool lagfx_a64_res_readback(lagfx_protocol_t *p, uint32_t task, uint32_t ref, void *dst, size_t dst_size, uint32_t *w, uint32_t *h);
#endif

/* Why a texture handle (task, ref) given to setFragmentTexture / setVertexTexture has no usable mirror texture. Classified from the guest's object table entry
 * (re-read, no side effects) and the memo of failed creations; LAGFX_A64_TEXWHY_COUNT in state.h must equal TEXWHY_COUNT. */
typedef enum {
    TEXWHY_SLOT_UNBOUND = 0,   /* no texture record (tag 114 / 129) named this slot since the encoder began (0 so a cleared table means this) */
    TEXWHY_SLOT_RANGE,         /* slot index >= 32 (A64_MAX_FB) or the shader's index is negative: the table cannot hold it */
    TEXWHY_RECORD_BAD,         /* the texture record's count was above the table size: the whole record was refused (counted bad_rec) */
    TEXWHY_REF_ZERO,           /* the record named ref 0 (a nil texture) */
    TEXWHY_NO_TABLE,           /* task unknown / object lists not set / ref beyond the object table length: the index is not in this exec's object table */
    TEXWHY_ENTRY_READ,         /* the table entry could not be read from guest memory */
    TEXWHY_ENTRY_EMPTY,        /* the entry is all zero / type 0: the guest has not filled it in (yet): texture created later than the draw, or already deleted */
    TEXWHY_NOT_TEXTURE,        /* the entry (or its mirror object) is a buffer */
    TEXWHY_UNKNOWN_TYPE,       /* entry type is not buffer / texture / linear texture / mapped texture (function, pipeline, sampler, heap, ...) */
    TEXWHY_NOT_MIRRORED,       /* a texture entry the mirror has no object for and no failed creation on record: not in any exec's resource list, or released by 0x25 / task drop */
    TEXWHY_TABLE_FULL,         /* creation refused: the mirror's object table is full */
    TEXWHY_C_PAYLOAD,          /* creation failed: payload unreadable or inconsistent (size, length, mip count) */
    TEXWHY_C_FORMAT,           /* creation failed: Metal pixel format has no Vulkan mapping in the table (compressed, packed 16 bit, ...) */
    TEXWHY_C_TYPE,             /* creation failed: texture type is not 2D / 2D array / 3D (cube, 1D, ...), or the linear-texture description tag is unknown */
    TEXWHY_C_MSAA,             /* creation failed: multisampled texture */
    TEXWHY_C_DIMS,             /* creation failed: zero or over-large extent, mip / layer count, depth-stencil shape */
    TEXWHY_C_VULKAN,           /* creation failed: the device refused the image / staging buffer (format features missing, allocation) */
    TEXWHY_C_LINEAR,           /* creation failed: linear texture whose buffer ref / row / offset is not usable */
    TEXWHY_C_MAPPED_NODESC,    /* creation failed: mapped texture whose display surface has no descriptor in the shell (surface not created yet / unknown mapping id) */
    TEXWHY_C_MAPPED_BAD,       /* creation failed: mapped texture whose surface does not decode / is not BGRA|RGhA / too large / image unusable / registry full */
    TEXWHY_COUNT
} lagfx_a64_texwhy_t;
typedef struct { lagfx_a64_texwhy_t why; uint32_t type, mtl_format, width, height, mapping; } lagfx_a64_texdiag_t;   /* type: object table entry type (0 unknown); the rest best effort from the payload (0 unknown) */
bool lagfx_a64_res_diagnose(lagfx_protocol_t *p, uint32_t task, uint32_t ref, lagfx_a64_texdiag_t *out);   /* classifies one unresolved texture ref; always fills out->why; false if nothing could be said (no mirror manager) */
const char *lagfx_a64_texwhy_name(unsigned why);
_Static_assert((int)TEXWHY_COUNT == LAGFX_A64_TEXWHY_COUNT, "state.h LAGFX_A64_TEXWHY_COUNT out of step with lagfx_a64_texwhy_t");

/* LAGFX_ARM64_EXEC=1 (read at every call). */
bool lagfx_a64_res_enabled(void);

lagfx_a64_rexec_t *lagfx_a64_res_begin(lagfx_protocol_t *p, uint32_t task, const uint8_t *res, uint32_t nres);
lagfx_a64_obj_t  *lagfx_a64_res_object(lagfx_a64_rexec_t *x, uint32_t ref);
void              lagfx_a64_res_blit_segment(lagfx_a64_rexec_t *x, const uint8_t *body, uint32_t len);
lagfx_a64_status_t lagfx_a64_res_end(lagfx_a64_rexec_t *x);   /* x is freed unless PENDING */
/* Polls the pending exec of this protocol (waits at most wait_ms; 0 = just check). DONE also when nothing is pending. While pending, begin() returns NULL. */
lagfx_a64_status_t lagfx_a64_res_poll(lagfx_protocol_t *p, uint32_t wait_ms);
extern int lagfx_a64_test_hook;   /* tests only (see arm64_resources.c) */
int  lagfx_a64_test_ms_down_pending(lagfx_protocol_t *p, uint32_t mapping);   /* tests only */
void lagfx_a64_test_ms_force_pending(lagfx_protocol_t *p, uint32_t mapping);  /* tests only */
uint32_t          lagfx_a64_res_display_swap(lagfx_protocol_t *p);   /* LAGFX_ARM64_MAPPED_WRITEBACK=swap: pay the owed mapped surfaces (before a display command is forwarded) */
bool              lagfx_a64_res_is_pending(lagfx_protocol_t *p);
/* Classifies the surface a display swap names (call after lagfx_a64_res_display_swap, before the command is forwarded). Returns a mask of LAGFX_A64_DSP_*; no side effects. */
#define LAGFX_A64_DSP_RESOLVED 1u   /* the mapping id has a mirror surface */
#define LAGFX_A64_DSP_GPU      2u   /* ... that holds meaningful pixels (host_valid) */
#define LAGFX_A64_DSP_WRITTEN  4u   /* ... and the guest's pages are current (no flag-2 render since the last write-back / upload) */
uint32_t          lagfx_a64_res_display_note(lagfx_protocol_t *p, uint64_t mapping);
/* Counts one ARM64 display command (ops 1 / 2 / 8) into p->diag2.disp: op seen, and for a swap whether its surface resolved / holds GPU pixels / is current in the guest. */
/* Diagnostics only: size, fourcc, Metal format number and VkFormat number of the mirror surface for `mapping` (false: none). */
bool              lagfx_a64_res_display_owner(lagfx_protocol_t *p, uint64_t mapping, uint32_t *task);
bool              lagfx_a64_res_display_info(lagfx_protocol_t *p, uint64_t mapping, uint32_t *w, uint32_t *h, uint32_t *fourcc, uint32_t *mtl_format, int32_t *vk_format);
void              lagfx_a64_display_count(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);

/* Task teardown (CmdDeleteTask): the task's mirror objects go; their Vulkan objects are freed only after any exec that may use them has finished. */
void lagfx_a64_res_drop_task(lagfx_protocol_t *p, uint32_t task);
/* An exec is recording or its fence has not signalled. */
bool lagfx_a64_res_busy(lagfx_protocol_t *p);
/* live (in the table), created and destroyed mirror objects (destroyed counts real frees, not parked ones). */
void lagfx_a64_res_counts(lagfx_protocol_t *p, uint32_t *live, uint64_t *created, uint64_t *destroyed);

/* ARM64 + LAGFX_ARM64_EXEC=1 only (else returns false and the dispatcher behaves as before): 0x35 / 0x3e SYNCHRONIZE {task,count,refs}: host-valid objects
 * are written to the guest pages and no longer stale; 0x34 INVALIDATE {task,count,{ref, host invalid, host valid, guest invalid, guest valid}}: flips
 * host_valid / guest_stale; 0x3f purgeable hint: counted, bytes stay. Returns true if the command was consumed (the caller still completes the stamp).
 * Guest memory is touched only through shell.read_memory / write_memory (the shell has no separate 'DMA written' notification). */
bool lagfx_a64_resource_cmd(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
/* 0x25 in arm64 mode removed a ref from the task's object set: drop its mirror too. */
void lagfx_a64_res_forget(lagfx_protocol_t *p, uint32_t task, uint32_t ref);

/* Download the listed (already host_valid) objects to the guest pages and clear their stale mark. Returns how many were written. */
/* Called by the existing blit dispatch (src/handlers/compute/blit_inner_ops.c) in arm64 mode for opcodes 0x12c-0x12f, 0x132 (== tags 300-303, 306). */
int lagfx_a64_blit_op(lagfx_protocol_t *p, uint32_t tag, const uint8_t *payload, size_t len);

uint32_t lagfx_a64_res_sync_to_guest(lagfx_protocol_t *p, uint32_t task, const uint32_t *refs, uint32_t n);

const lagfx_a64_stats_t *lagfx_a64_res_stats(lagfx_protocol_t *p);   /* never NULL */
void lagfx_a64_res_destroy(lagfx_protocol_t *p);                     /* safe on NULL / nothing allocated */

#endif
