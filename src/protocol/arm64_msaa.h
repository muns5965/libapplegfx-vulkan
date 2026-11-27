/*
 * libapplegfx-vulkan — ARM64 exec: guest-visible multisample (MSAA) path (texture gate, attachment resolve, deferred store actions, counters).
 * src/protocol/arm64_msaa.h
 *
 * SPDX-License-Identifier: MIT
 *
 * New code lives here so the shared hotspots (arm64_resources.c, arm64_exec_run.c) keep small hooks. Vulkan side: src/vulkan/msaa.{h,c}. Counters: p->diag2.exec.msaa (g7-gpu-msaa).
 *
 * Evidence tags used in the comments of this module:
 *   [V] verified against decoded guest data in the dumps in hand (/tmp/x231501, /tmp/vm2, /tmp/run3/z/dump);
 *   [I] inferred (the field is read from a descriptor layout that was decoded from the arm64e disassembly or from the Metal API's own documented ordering) - NO guest capture of a
 *       multisampled pass exists as of this commit, so every guest-field statement about MSAA is [I] at best;
 *   [N] not decoded / not implemented, counted when it would matter.
 * Official basis: Apple Metal (MTLTextureDescriptor.sampleCount / textureType = MTLTextureType2DMultisample, MTLRenderPassColorAttachmentDescriptor.storeAction / resolveTexture,
 * MTLStoreAction, MTLRenderPipelineDescriptor.rasterSampleCount) and the Khronos Vulkan specification (dynamic-rendering resolve, vkCmdResolveImage). No GPL / LGPL code is a basis.
 */
#ifndef LAGFX_PROTOCOL_ARM64_MSAA_H
#define LAGFX_PROTOCOL_ARM64_MSAA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "state.h"
#include "render_pass.h"

#ifdef LAGFX_HAVE_VULKAN
#include <vulkan/vulkan.h>
#include "arm64_resources.h"
#include "../vulkan/format_caps.h"
#include "../vulkan/msaa.h"

/* MTLTextureType numbering of the guest descriptor ([I]: the same numbering the mirror already relies on for 2 = 2D, 3 = 2D array, 7 = 3D) */
#define LAGFX_A64_MTL_TEX_2D_MS       4u
#define LAGFX_A64_MTL_TEX_2D_MS_ARRAY 8u

enum { LAGFX_A64_MSAA_PLAIN = 0, LAGFX_A64_MSAA_YES, LAGFX_A64_MSAA_REFUSE };

/* The shape gate of texture creation. mtype / samples / mips / layers are the decoded descriptor fields (samples: u16 after the mip count, before the array length [V]: a real
 * descriptor of the mapped display surface reads mips = 1, samples = 1, array = 1 in that order; the 1x value says nothing about the multisample value itself [I]).
 *   PLAIN  a single-sample texture of any other type: the caller goes on as before.
 *   YES    a 2D multisample texture (type 4) with samples > 1, one mip, one layer: *req = the sample count asked for.
 *   REFUSE a shape the mirror does not model (counted msaa_refused + ms_tex_bad_shape): samples > 1 with another type, type 4 with <= 1 sample, type 8 (2D multisample array), mips or
 *          layers above 1 on a multisample texture. */
int lagfx_a64_msaa_tex_gate(lagfx_protocol_t *p, uint32_t mtype, uint32_t samples, uint32_t mips, uint32_t layers, uint32_t *req);

/* Sample count and usage mask of the image of guest texture `o` (o->samples_req > 1): colour or depth / stencil; the device's answer is asked for the whole mask. false = no image
 * can be made at all (counted ms_tex_refused). On true: *ns = the granted count (lower than asked only in COMPAT mode: counted ms_compat_clamped, or 1 = ms_compat_1x; in EXACT mode, the default, a count that is not offered exactly returns false and is counted ms_exact_refused), cu->usage the mask. */
bool lagfx_a64_msaa_plan_image(lagfx_protocol_t *p, const lagfx_fmtcap_backend_t *be, lagfx_a64_obj_t *o, VkSampleCountFlagBits *ns, lagfx_fmtcap_usage_t *cu);

/* Exact-mode guard of a pass attachment: false (counted ms_pass_refused) when the guest texture is multisampled but the host image has another sample count (only possible when the
 * image was made in compat mode and the mode was switched afterwards): an exact-mode pass never renders into an approximation. Always true for a plain texture and in compat mode. */
bool lagfx_a64_msaa_attach_ok(lagfx_protocol_t *p, const lagfx_a64_obj_t *t);

/* A pass names `ref` and the mirror has no such texture: counts ms_pass_refused when the creation was refused by the multisample plan / shape gate (the attachment is skipped). */
void lagfx_a64_msaa_note_missing(lagfx_protocol_t *p, lagfx_a64_rexec_t *x, uint32_t ref);

/* ---- one render pass ---- */
typedef struct {
    uint32_t samples;                          /* sample count of the pass's attachments (0 / 1 = single sample) */
    bool st_set[8], res_set[8];                /* deferred store action / resolve texture named by tags 0x66 / 0x96 before the pass's first draw, by colour index [I] */
    uint32_t st_act[8], res_ref[8];
    lagfx_a64_obj_t *res[4];                   /* resolve targets begun for this pass, by attachment slot (ended at the end of the pass) */
    lagfx_a64_obj_t *copy_src[4];              /* slot's 1x-fallback attachment whose "resolve" is a copy into res[slot] at the end of the pass */
    uint32_t nres;
    const char *fail;                          /* EXACT mode: set by lagfx_a64_msaa_color when a resolve the store action requires could not be set up (static string; NULL = fine) */
} lagfx_a64_msaa_pass_t;

void lagfx_a64_msaa_pass_reset(lagfx_a64_msaa_pass_t *ms);

/* Reads the records after a tag 26 up to the next tag 26 (or `lim`): tags 0x66 (SetColorStoreAction {u32 action, u32 index}) and 0x96 (SetColorResolveTexture {u32 ref, u32 slot,
 * ...}) are collected by colour index. The payload layouts are those of the x86-era table in render_inner_ops.c; NOT verified on ARM64 [I]. Metal applies the last call, so does this. */
void lagfx_a64_msaa_prescan(lagfx_protocol_t *p, lagfx_a64_msaa_pass_t *ms, const uint8_t *recs, uint64_t lim);

/* The sample count an attachment object contributes (1 for a plain texture). */
static inline uint32_t lagfx_a64_msaa_obj_samples(const lagfx_a64_obj_t *o) { return o && o->samples > 1u ? o->samples : 1u; }

/* Fills the store / resolve half of colour attachment `slot` (the caller has set view, load op, clear, layout) for the pass's multisampled attachment `t`. `store_action` and
 * `resolve_ref` come from the pass descriptor (overridden by the deferred tags of the same colour index `cidx`). The resolve target is looked up, validated (single sample, same
 * format and extent, colour attachment, not an attachment of the pass), begun with lagfx_a64_res_target_begin and remembered in ms->res[slot]; a 1x-fallback attachment gets a copy at
 * the end of the pass instead of a resolve. Every refusal is counted and leaves the safe default (STORE, nothing resolved). Returns true when a resolve will happen. */
bool lagfx_a64_msaa_color(lagfx_protocol_t *p, lagfx_a64_rexec_t *x, lagfx_a64_msaa_pass_t *ms, uint32_t slot, uint32_t cidx, lagfx_a64_obj_t *t, uint32_t store_action, uint32_t resolve_ref,
                          bool resolve_sub_nonzero, const lagfx_a64_obj_t *const attached[4], VkRenderingAttachmentInfo *ca);

/* ---- EXACT-mode whole-pass rejection ----
 * In exact mode a pass that names ANY multisampled attachment (colour, depth, stencil; or one whose creation the multisample plan refused) is all-or-nothing: if ANY attachment the pass
 * names (colour 0..3, depth, stencil) or any resolve target its store action requires is refused or inconsistent (missing / refused texture, host sample count != guest's, mixed sample
 * counts, extent mismatch, duplicate image, depth / stencil split or level, depth / stencil resolve (not wired), resolve target missing / wrong shape / part of the pass), the WHOLE pass is
 * rejected: nothing is begun (no layout change, no clear / load op), no draw of the pass is recorded, no attachment is written. Counted ms_pass_refused (once per pass) and logged with the
 * slot and the reason. A pass naming no multisampled attachment, and COMPAT mode (clamp / 1x copy, per-attachment skipping), keep their behaviour. */
typedef struct { const char *why; char what[16]; uint32_t ref; } lagfx_a64_msaa_veto_t;   /* what: "colour0".."colour3", "depth", "stencil", "resolve0".. (resolve target of colour slot n) */
/* The objects the pass descriptor names, resolved ONCE by lagfx_a64_msaa_pass_gate (lagfx_a64_res_object may create the mirror texture, a failed creation is counted: never ask twice). */
typedef struct {
    lagfx_a64_obj_t *c[LAGFX_RENDER_PASS_MAX_COLOR_ATTACHMENTS];   /* colour attachment k of the descriptor (NULL: not mirrored) */
    bool lin[LAGFX_RENDER_PASS_MAX_COLOR_ATTACHMENTS];             /* the lookup raised stats->linear_unsupported (a type 8/9 render target the mirror could not build) */
    lagfx_a64_obj_t *ds; bool ds_looked;                           /* depth / stencil texture (looked up only when the device can render depth / stencil) */
} lagfx_a64_msaa_look_t;
/* Validates the pass descriptor `d` BEFORE any attachment is begun. *strict = the pass is subject to whole-pass rejection (exact mode and at least one multisampled / msaa-refused
 * attachment). Returns false only when *strict and a veto was found (*v filled). `ds_ok` = the device can render depth / stencil (core 1.3 dynamic rendering). */
bool lagfx_a64_msaa_pass_gate(lagfx_protocol_t *p, lagfx_a64_rexec_t *x, const lagfx_a64_msaa_pass_t *ms, const lagfx_render_pass_full_desc_t *d, bool ds_ok, lagfx_a64_msaa_look_t *look, bool *strict, lagfx_a64_msaa_veto_t *v);
/* Counts ms_pass_refused and logs the reason (rate limited). */
void lagfx_a64_msaa_reject(lagfx_protocol_t *p, const lagfx_a64_msaa_veto_t *v);
/* Tests only: the reason of the newest rejection ("<what>: <why>", "" when none). */
const char *lagfx_a64_msaa_last_reject(void);

/* After vkCmdEndRendering and the attachments' target_end: ends the resolve targets (layout back to GENERAL) and records the copies of the 1x fallback. */
void lagfx_a64_msaa_pass_end(lagfx_protocol_t *p, lagfx_a64_rexec_t *x, lagfx_a64_msaa_pass_t *ms);
/* Same without recording copies (pass setup failed after attachments were begun). */
void lagfx_a64_msaa_pass_abort(lagfx_a64_rexec_t *x, lagfx_a64_msaa_pass_t *ms);

#endif /* LAGFX_HAVE_VULKAN */
#endif
