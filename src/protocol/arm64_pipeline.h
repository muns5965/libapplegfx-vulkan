/*
 * libapplegfx-vulkan — ARM64 pipeline-state resolver (guest serialized render pipeline -> shader modules + vertex layout + descriptor layout).
 * src/protocol/arm64_pipeline.h
 *
 * Reference: Apple's devices/pvg_metal.m makeSerialized (tag 0xe: {tag, size, ref, length, data}, data = Apple's private serialized
 * MTLRenderPipelineDescriptor, read by -newRenderPipelineDescriptorWithSerializedData:deserializationContext:) and makeFunction (a function
 * object, type 6: {u64 library VA, u32 length, u32 tag, u32 size}; the library is a metallib whose first function is used).
 *
 * What is NOT known: the descriptor body is read by a private Metal SPI, so its token layout is not documented by pvg_metal.m. This module reuses
 * what the x86 side learned of the same serialization (object_resolver.c: `04 <u32>` tokens; function refs are tokens whose value names a
 * function-typed object; vertex attributes `01 04 fmt 02 04 off 03 04 bufidx`; strides `04 <v> 02|01`), UNVERIFIED on real ARM64 guest data:
 * set LAGFX_ARM64_DUMP=1 (arm64_dump.c) to capture the real descriptors. Colour attachment formats, blend and depth/stencil are NOT decoded:
 * the pass's render-target format is used, and the rest are counted stubs (diag2.exec.pipe.stub_*). Depth/stencil and raster state are not pipeline
 * state in Metal: they are encoder state (arm64_depth_stencil.h, arm64_exec_run.c) and are applied as Vulkan dynamic state.
 */
#ifndef LAGFX_PROTOCOL_ARM64_PIPELINE_H
#define LAGFX_PROTOCOL_ARM64_PIPELINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "state.h"

#define LAGFX_A64_PIPE_MAX_BLOB   (256u << 10)   /* serialized descriptor bytes read */
#define LAGFX_A64_LIB_MAX         (4u << 20)     /* metallib bytes read per function object */
#define LAGFX_A64_PDESC_MAX_REFS  16u
#define LAGFX_A64_PDESC_MAX_ATTRS 8u
#include "arm64_limits.h"
#define LAGFX_A64_FRAG_BINDING_BASE 16u          /* fragment-stage SPIR-V bindings are offset by this (same as the x86 draw path) */

#define LAGFX_A64_FBF_BINDING_BASE 24u           /* the translator's input-attachment bindings: 24 + colour attachment index (before the fragment offset above) */

/* Why a framebuffer-fetch (input attachment) binding cannot be built; LAGFX_A64_FBF_OK = it can. Pure: shared by the resolver and tools/dump-bindings. */
enum { LAGFX_A64_FBF_OK = 0, LAGFX_A64_FBF_VERTEX_STAGE, LAGFX_A64_FBF_NO_LOCAL_READ, LAGFX_A64_FBF_BINDING_RANGE, LAGFX_A64_FBF_TOO_MANY, LAGFX_A64_FBF_REASONS };
/* spv_binding = the binding in the module AS RESOLVED (a fragment module has been offset by LAGFX_A64_FRAG_BINDING_BASE already); n_bind = bindings taken so far. */
int lagfx_a64_fbfetch_check(int stage, bool have_local_read, uint32_t spv_binding, uint32_t n_bind);
const char *lagfx_a64_fbfetch_reason_name(int reason);

typedef struct { uint32_t fmt, off, bufidx; } lagfx_a64_vattr_t;

/* Colour attachment 0 of the serialized descriptor (record `00 04 <idx 0>` followed by `kk 04 <u32>` tokens, keys in MTLRenderPipelineColorAttachmentDescriptor
 * declaration order: 1 pixelFormat, 2 blendingEnabled, 3 sourceRGB, 4 destinationRGB, 5 rgbOp, 6 sourceAlpha, 7 destinationAlpha, 8 alphaOp, 9 writeMask; a
 * property at its Metal default is not serialized). Evidence: all 43 descriptors of the device dump (pixelFormat values 70/80/115 are RGBA8Unorm/BGRA8Unorm/
 * RGBA16Float, the blend values are valid MTLBlendFactor values and the common 4=5,7=5 pair is the premultiplied source-over the compositor uses). Key 2 is blendingEnabled and an absent key means disabled
 * (13 descriptors carry factors without it): this matches an independent implementation of the same serialization that renders macOS screens (medfa12/
 * apple-silicon-anyos-project, GPL: only the token meanings were compared, no code taken). found = the record was recognised; otherwise every field is the Metal default and the caller keeps its fixed state. */
typedef struct { bool enabled_token, any_blend_token; uint8_t enabled_value, src_rgb, dst_rgb, op_rgb, src_a, dst_a, op_a; } lagfx_a64_att_blend_t;   /* one attachment's blend tokens (Metal defaults when absent) */
typedef struct {
    lagfx_a64_att_blend_t a1;                     /* attachment 1's blend tokens (keys 2..8 after its pixel format), valid when found1 */
    bool found, any_blend_token, enabled_token, found1;
    uint8_t enabled_value, mask1;                 /* key-2 value; attachment 1's writeMask (default 0xf) */
    uint32_t pixfmt, pixfmt1;                     /* attachment 1: `03 00 04 01 ...` record that follows attachment 0 (found1) */
    uint8_t src_rgb, dst_rgb, op_rgb, src_a, dst_a, op_a, mask;   /* MTLBlendFactor / MTLBlendOperation / MTLColorWriteMask values (defaults: 1,0,0,1,0,0,0xf) */
} lagfx_a64_blend_t;

static inline bool lagfx_a64_blend_on(const lagfx_a64_blend_t *b) { return b->enabled_token && b->enabled_value != 0u; }

typedef struct {
    uint32_t tag, size, ref, length;             /* header words 0..3 */
    uint32_t nrefs, refs[LAGFX_A64_PDESC_MAX_REFS];   /* function-object refs, first-seen order */
    uint32_t nattrs; lagfx_a64_vattr_t attrs[LAGFX_A64_PDESC_MAX_ATTRS];   /* sorted by offset */
    uint32_t stride;                             /* 0 = not found */
    lagfx_a64_blend_t color0;
} lagfx_a64_pdesc_t;

/* Pure (no guest memory, no Vulkan): decodes the header and scans the body. is_fn(ctx, ref) says whether `ref` names a function object (NULL = every
 * nonzero value counts). False if shorter than the 16-byte header. */
bool lagfx_a64_pdesc_decode(const uint8_t *blob, uint32_t n, bool (*is_fn)(void *ctx, uint32_t ref), void *ctx, lagfx_a64_pdesc_t *out);

/* FNV-1a 64 of a byte range (object and payload identity in caches and logs). */
uint64_t lagfx_a64_hash64(const void *data, size_t n);

/* Reads the serialized payload an object entry points at (size = entry[0] >> 8, capped at LAGFX_A64_PIPE_MAX_BLOB). Returns a malloc'd buffer (caller
 * frees) and its length, or NULL. *full_size gets the uncapped size. */
uint8_t *lagfx_a64_read_payload(lagfx_protocol_t *p, const lagfx_task_entry_t *task, const uint32_t entry[3], uint32_t *len, uint32_t *full_size);

/* Function object (type 6): reads the library it names. malloc'd, caller frees; *lib_len / *lib_va set. NULL on any failure. */
uint8_t *lagfx_a64_read_function_lib(lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint32_t ref, uint32_t *lib_len, uint64_t *lib_va);

/* True iff `ref` is a function-typed entry of the task's list (the is_fn callback for lagfx_a64_pdesc_decode; ctx = lagfx_a64_fnctx_t). */
typedef struct { lagfx_protocol_t *p; const lagfx_task_entry_t *task; } lagfx_a64_fnctx_t;
bool lagfx_a64_is_function_ref(void *ctx, uint32_t ref);

/* Hash of the payload currently at the object entry's address (what the pipeline was resolved from); false if unreadable. */
bool lagfx_a64_payload_hash(lagfx_protocol_t *p, const lagfx_task_entry_t *task, const uint32_t entry[3], uint64_t *hash);


#include "arm64_exec_run.h"
#include "../air2spv/translate.h"
/* Binding map of one stage (pure, no Vulkan): reflects the SPIR-V (fragment modules already offset by LAGFX_A64_FRAG_BINDING_BASE) and appends one lagfx_a64_bind_t
 * per set-0 binding to bind[*n_bind], matching each to the n-th Metal argument of its kind in ab[] (see the definition). Returns LAGFX_A64_MAP_OK or why not;
 * *fbf_why = the LAGFX_A64_FBF_* reason for LAGFX_A64_MAP_FBFETCH. */
enum { LAGFX_A64_MAP_OK = 0, LAGFX_A64_MAP_FAIL, LAGFX_A64_MAP_VERTEX_RES, LAGFX_A64_MAP_FBFETCH };
int lagfx_a64_map_stage_bindings(const uint8_t *spv, size_t len, int stage, const lagfx_air_arg_binding_t *ab, size_t nab, bool have_local_read, bool have_storage_img_write,
                                 lagfx_a64_bind_t *bind, uint32_t *n_bind, int *fbf_why);
/* The pipeline resolver the executor installs by default (ARM64 mode, LAGFX_ARM64_EXEC=1): decodes the guest's serialized pipeline, loads its function
 * libraries from guest memory, translates AIR -> SPIR-V (air2spv, the translator the x86 draw path uses; no external tools), reflects vertex inputs and
 * descriptor bindings (+ Metal indices from Apple's arg metadata), builds the shader modules and the set-0 layout. False = unresolved (counted by
 * cause in p->diag2.exec.pipe). Without Vulkan it always returns false. */
bool lagfx_a64_pipeline_resolve(void *ctx, lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint32_t ref, const uint32_t entry[3],
                                lagfx_a64_pipeline_src_t *out);

#endif /* LAGFX_PROTOCOL_ARM64_PIPELINE_H */
