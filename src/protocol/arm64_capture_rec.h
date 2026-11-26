/*
 * libapplegfx-vulkan — render replay capture: recorder interface used by arm64_exec_run.c (Vulkan builds only). Format: arm64_capture.h.
 * All functions accept cap == NULL (capture off) and do nothing then. The recorder holds copies of everything it needs; nothing refers to executor memory after the call.
 */
#ifndef LAGFX_PROTOCOL_ARM64_CAPTURE_REC_H
#define LAGFX_PROTOCOL_ARM64_CAPTURE_REC_H

#include "arm64_capture.h"
#ifdef LAGFX_HAVE_VULKAN
#include <vulkan/vulkan.h>
#include "arm64_resources.h"
#include "arm64_limits.h"

typedef struct lagfx_a64_cap lagfx_a64_cap_t;

typedef struct { lagfx_a64_obj_t *o; uint32_t slot, load; float clear[4]; } lagfx_a64_cap_att_in_t;
typedef struct {
    uint32_t w, h; bool depth, stencil; uint32_t depth_ref, depth_vkfmt, depth_load, stencil_load, stencil_clear; float depth_clear;
} lagfx_a64_cap_pass_in_t;

typedef struct { uint32_t ref; uint64_t gen; VkSamplerCreateInfo ci; bool mip; float lod_lo, lod_hi; } lagfx_a64_cap_samp_in_t;
typedef struct {
    uint32_t binding, kind, stage, idx, writable;      /* kind = LAGFX_A64_BIND_*, stage 0 vertex / 1 fragment */
    lagfx_a64_obj_t *obj; uint64_t off, range;         /* buffer / texture object (NULL for samplers and unresolved) */
    bool has_samp; lagfx_a64_cap_samp_in_t samp;
} lagfx_a64_cap_bind_in_t;

typedef struct {
    uint32_t tag, prim, indexed; uint64_t count, instances, first_vertex, base_instance; int64_t base_vertex; uint32_t index_start, index_type; uint64_t index_off;
    lagfx_a64_obj_t *ib;
    uint32_t pipe_ref; uint64_t pipe_gen, pipe_hash;
    const uint32_t *vs, *fs; size_t vs_bytes, fs_bytes;
    uint32_t vtx_slot; lagfx_a64_obj_t *vb; uint64_t vb_off; uint32_t vtx_stride;
    uint32_t n_vtx_in, vtx_in_loc[8], vtx_in_comp[8], n_attr, attr_fmt[8], attr_off[8];
    float vp[6]; uint32_t sc[4]; uint32_t cull, winding; float bias[3];
    uint32_t blend_set, blend, blend_rgb[3], blend_a[3], write_mask, ncolor_fmts, color_fmts[4];
    uint32_t depth_test, depth_write, depth_cmp, stencil_test, sref[2];
    uint32_t nbind; lagfx_a64_cap_bind_in_t binds[LAGFX_A64_MAX_BINDINGS];
} lagfx_a64_cap_draw_in_t;

/* NULL unless capture is enabled and x is valid. Installs the finish hook: the capture owns itself from here and is written + freed when the exec's fence
 * signalled (or dropped if the exec dies). The caller must forget the pointer at exec end. */
lagfx_a64_cap_t *lagfx_a64_cap_begin(lagfx_protocol_t *p, lagfx_a64_rexec_t *x, uint32_t task, uint32_t exec_seq);
/* A pass is about to begin (call before the attachments leave GENERAL): false = no draw of it can be selected (draw range already exceeded / draw cap reached). */
bool lagfx_a64_cap_pass_wanted(lagfx_a64_cap_t *cap);
void lagfx_a64_cap_pre(lagfx_a64_cap_t *cap, lagfx_a64_obj_t *o);                           /* snapshot (tight) of a candidate colour attachment BEFORE the pass */
void lagfx_a64_cap_pass_open(lagfx_a64_cap_t *cap, const lagfx_a64_cap_pass_in_t *pi, const lagfx_a64_cap_att_in_t *atts, uint32_t natt);
void lagfx_a64_cap_pass_close(lagfx_a64_cap_t *cap);                                       /* after the pass ended and its attachments are back in GENERAL */
/* One draw reaches the executor (executed or skipped): assigns its process-wide number; *selected = the filters want its data. */
uint32_t lagfx_a64_cap_draw_enter(lagfx_a64_cap_t *cap, uint32_t pipe_ref, bool *selected);
/* skip == NULL: the draw executed; else the skip reason (recorded only with LAGFX_ARM64_DUMP_DATA_FAIL). Records state + binds at draw time. */
void lagfx_a64_cap_draw(lagfx_a64_cap_t *cap, uint32_t n, const char *skip, const lagfx_a64_cap_draw_in_t *d);
#endif
#endif
