/*
 * libapplegfx-vulkan — independent GPU readback of a mapped display surface (F-2 diagnostic)
 * src/vulkan/gpu_readback.h
 *
 * Question it answers: is the pixel data that reaches the guest pages (write-back path) bit-identical to what the GPU image really holds, for the SAME swap?
 * The existing LAGFX_WB_VERIFY compare only checks staging buffer (B) against the guest pages (C); nothing ever looked at the GPU image (A) of the 1920x1080
 * RGBA16Float display surface. This module adds that observation, read-only:
 *
 *   A  the GPU image, copied with one vkCmdCopyImageToBuffer (same VkFormat, no blit / shader / conversion; aspect COLOR, mip 0, layer 0, full extent,
 *      bufferRowLength = bufferImageHeight = 0, i.e. tight rows: pitch = width * bytes-per-texel, 15360 for 1920 x RGBA16F) into its OWN host-visible buffer,
 *      pre-filled with a poison pattern so a copy that never ran cannot pass. The copy is recorded in the command buffer that holds the staging copy, right
 *      after it (same image state, same fence, same swap / mapping / generation); the image stays in GENERAL (the library never leaves it, no transition is
 *      made, so there is nothing to restore). A transfer -> host barrier follows and, for non-coherent memory, vkInvalidateMappedMemoryRanges runs before reading.
 *   B  the existing staging buffer (ms->stage_map), the bytes the write-back wrote to the guest.
 *   C  a fresh read of the guest pages (lagfx_guest_surface_xfer, to_guest = false) taken after the write-back.
 *
 * One line "g7-gpu-readback:" per sampled swap says PASS / FAIL / SKIP and why. FAIL and SKIP are counted and NEVER count as a pass. Default OFF.
 *
 * Activation (split; the two switches are independent):
 *   neither                        nothing: no candidate counted, no GPU copy, no buffer, no line, no wait accounting.
 *   LAGFX_WB_VERIFY=1 only         the existing staging(B) -> guest(C) wbverify line ONLY. The independent GPU readback does NOT run (it used to be enabled by this flag; it is not any more).
 *   LAGFX_GPU_READBACK=1 only      the independent readback, mode=AB: GPU image (A) vs the staging bytes (B) the write-back wrote. The guest pages are NOT re-read (no C), no wbverify line.
 *   both                           mode=ABC: A vs B vs a fresh guest-page read (C), plus the wbverify line. This is the full comparison.
 * Sampling per surface class (BGRA / RGhA): the first 4 candidates, then every LAGFX_GPU_READBACK_EVERY-th (default 16), plus any candidate at least 30 s after the
 * previous sample (catches the first frame after a long silence), at most 60 lines. A "candidate" is one image -> staging copy recording: one per exec that renders
 * into the surface in the immediate write-back mode (LAGFX_ARM64_MAPPED_WRITEBACK=1), one per display swap in =swap mode.
 * Cost when on: one extra GPU copy (in the SAME command buffer, so under the SAME fence) + one extra host buffer of the surface size per surface (allocated at the
 * first sample), + (mode ABC) two guest-page reads. Off: nothing.
 *
 * TIMING SIDE EFFECT (read this before comparing ON and OFF runs): the extra copy lengthens the GPU work that the exec / swap fence wait covers, so every sampled
 * submission waits longer than it would without readback. That wait is counted (totals waits= / wait_ms_total=, only for submissions that carried a readback copy). A
 * readback run can therefore HIDE a frame-freshness race (a late frame that appears one swap later when the GPU is slow looks fine when the wait is longer) or
 * change the sequence of swaps. Every line carries readback_sync=on and wbv=on|off so a log shows which run it came from; compare behaviour only between runs that
 * state the same setting.
 *
 * Checks done before a sample is trusted (a failed check is a SKIP with the reason, never a pass, and changes nothing else): the sample is for the SAME swap
 * (finish swap >= record swap), mapping id, mapping generation (identity of the guest page list), write-back generation, image handle (not replaced / destroyed) and
 * subresource (mip 0, layer 0, the only one copied); the image was in GENERAL when recorded (the library never leaves it; no transition is recorded, so the layout is
 * the same after: layout=GENERAL->GENERAL); a signalled fence wait happened between record and finish (fence_seq); the transfer -> host barrier is recorded after the
 * copy; non-coherent memory is invalidated before the read; the row pitch is w * bpp (tight; bufferRowLength = 0) and a buffer that does not hold (h-1) * pitch + w * bpp
 * is an error. A surface destroyed while a sample is pending frees the slot (totals destroyed_pending=) and the finish never runs.
 *
 * Threading: called from the exec / swap paths, which already run one at a time under the protocol's execution lock. The counters are plain statics.
 * SPDX-License-Identifier: MIT
 */
#ifndef LAGFX_VULKAN_GPU_READBACK_H
#define LAGFX_VULKAN_GPU_READBACK_H

#include "instance.h"
#include "surface_writeback.h"
#include "libapplegfx-vulkan.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- pure part (no Vulkan): compare / classify / hash / format. Unit tested without a GPU. ---- */

/* One image in memory: `p` rows of `stride` bytes, `len` bytes valid in total (so a stride that does not fit is detected, not read past). */
typedef struct { const uint8_t *p; size_t stride, len; } lagfx_rb_img_t;

typedef struct {
    uint32_t w, h, bpp;
    uint64_t bytes;                              /* w * h * bpp: the bytes compared per pair */
    uint64_t ab_px, bc_px, ac_px;                /* pixels that differ: A vs B, B vs C, A vs C (C pairs only when C was given) */
    int32_t ab_x, ab_y, bc_x, bc_y;              /* first differing pixel (row-major), -1 if none */
    uint64_t hash_a, hash_b, hash_c;             /* FNV-1a, basis 1469598103934665603, prime 1099511628211, one step per whole LE pixel (u64 for bpp 8, u32 for bpp 4) = wbverify's hash_written for B */
    uint64_t nan_px, inf_px, neg_px, gt1_px;     /* bpp 8 only, on A: pixels with any NaN / any +-Inf / any negative non-zero finite / any finite channel above 1.0 */
    bool half_stats;
    uint32_t bad_img;                            /* on error: bit 0 = A, 1 = B, 2 = C had a stride < width*bpp or a length too short for its stride */
} lagfx_rb_cmp_t;

typedef enum { LAGFX_RB_OK = 0, LAGFX_RB_ERR_ARG = -1, LAGFX_RB_ERR_STRIDE = -2 } lagfx_rb_err_t;

/* c may be NULL (then bc / ac / hash_c are 0). Returns LAGFX_RB_ERR_STRIDE (out->bad_img says which image) for a stride below w * bpp or a buffer shorter than
 * (h - 1) * stride + w * bpp: a wrong stride is an error, never a read past the buffer and never a silent pass. */
lagfx_rb_err_t lagfx_rb_compare(const lagfx_rb_img_t *a, const lagfx_rb_img_t *b, const lagfx_rb_img_t *c, uint32_t w, uint32_t h, uint32_t bpp, lagfx_rb_cmp_t *out);

/* Bytes per texel of the VkFormats a mapped surface uses (R16G16B16A16_SFLOAT 8; B8G8R8A8_* / R8G8B8A8_* 4); 0 = unknown. */
uint32_t lagfx_rb_format_bpp(int32_t vk_format);

typedef enum { LAGFX_RB_PASS = 0, LAGFX_RB_FAIL = 1, LAGFX_RB_SKIP = 2 } lagfx_rb_verdict_kind_t;
typedef struct {
    bool valid;
    lagfx_rb_verdict_kind_t kind;
    const char *skip;                            /* reason for SKIP, or the FAIL reason ("mismatch", "stride", "no_c"), NULL for PASS */
    uint64_t swap, swap_fin, gen;
    uint32_t mapping, w, h, fourcc, bpp;
    uint64_t pitch, pages_skipped;
    bool c_stable, coherent;
    bool full;                                   /* mode ABC (LAGFX_WB_VERIFY=1 too: C is read) vs mode AB; set when the line is emitted */
    lagfx_rb_cmp_t cmp;
} lagfx_rb_verdict_t;
typedef struct { uint64_t candidates, sampled, pass, fail, skip, abandoned, not_sampled, invalidates, lines;
                 uint64_t waits, wait_ns, host_barriers, destroyed_pending, fence_seq; } lagfx_rb_totals_t;

size_t lagfx_rb_format_line(char *buf, size_t cap, const lagfx_rb_verdict_t *v, const lagfx_rb_totals_t *t);

bool lagfx_rb_enabled(void);                     /* LAGFX_GPU_READBACK == "1" ONLY (LAGFX_WB_VERIFY does not enable the independent readback) */
bool lagfx_rb_full_mode(void);                   /* both LAGFX_GPU_READBACK == "1" and LAGFX_WB_VERIFY == "1": mode ABC */

/* Identity of a sample, recorded with the copy and re-checked at the finish (pure; unit tested). */
typedef struct { uint64_t swap, gen, ident, img, fence_seq; uint32_t mapping, w, h, bpp, mip, layer; } lagfx_rb_ident_t;
/* NULL = the sample may be trusted; else the SKIP reason: "gen_mismatch", "mapping_mismatch", "mapping_generation_changed", "image_replaced", "desc_changed",
 * "subresource", "swap_went_back", "fence_not_signalled". `now.fence_seq` is the signalled-wait counter at the finish. */
const char *lagfx_rb_identity_check(const lagfx_rb_ident_t *rec, const lagfx_rb_ident_t *now);
/* NULL = the copy may be recorded; else the SKIP reason ("no_transfer_src", "unknown_format", "bpp_format_mismatch", "bad_extent", "layout_not_general"). */
const char *lagfx_rb_precheck(uint32_t usage_flags, int32_t vk_format, uint32_t bpp, uint32_t w, uint32_t h, int32_t layout);
/* Wait accounting: call with the duration of a fence wait. Counted only while a readback copy is waiting for its fence (recorded, not yet signalled); ok = the wait
 * ended with the fence signalled (advances fence_seq). No-op when the readback is off. */
void lagfx_rb_note_wait(uint64_t ns, bool ok);
const lagfx_rb_verdict_t *lagfx_rb_last_verdict(void);
const lagfx_rb_totals_t *lagfx_rb_totals(void);

/* Sampling gate (pure; the clock is passed in). `every` 0 means the default of 16. */
typedef struct { uint64_t seen, last_ns, taken; } lagfx_rb_gate_t;
bool lagfx_rb_gate_admit(lagfx_rb_gate_t *g, uint64_t now_ns, uint32_t every);

/* Tests only. hook: 0 none; 1 flip one bit of staging (B) before the compare; 2 flip one bit of the GPU copy (A); 3 compare B with a stride of width*bpp + 8 (a wrong
 * stride); 4 compare with C read from a corrupted copy (flip one bit of C; mode ABC); 5 treat the memory as non-coherent (invalidate runs even on coherent memory);
 * 6 the readback buffer allocation fails; 7 like 5 and the invalidate fails.
 * lagfx_rb_test_reset clears the totals, the last verdict and the gates. */
extern int lagfx_rb_test_hook;
void lagfx_rb_test_reset(void);

#ifdef LAGFX_HAVE_VULKAN
typedef struct lagfx_rb_slot lagfx_rb_slot_t;   /* per surface: the independent buffer + the pending sample */

/* Record the independent copy (if enabled, the gate admits this candidate and the preconditions hold) into `cb`, right after the staging copy of the same image.
 * Precondition checked here: usage has TRANSFER_SRC, format is a known texel size equal to `bpp`, the image is host_valid (the caller only calls for those) and in `layout` GENERAL. A skipped
 * candidate that was admitted logs one SKIP line with the reason. Never fails the caller. */
void lagfx_rb_record(struct lagfx_vk_state *vk, lagfx_rb_slot_t **slot, VkCommandBuffer cb, VkImage img, VkFormat fmt, VkImageUsageFlags usage,
                     VkImageLayout layout, uint32_t w, uint32_t h, uint32_t bpp, uint32_t mapping, uint64_t ident, uint64_t gen, uint64_t swap);
/* After the fence signalled and the write-back ran: identity checks, then compare A (own buffer) with B (`stage`, tight rows of w*bpp) and, in mode ABC, with C (fresh
 * guest read of `gs`); log the line. No-op if nothing is pending for `gen`. `img` / `ident` / `mapping` are the surface's CURRENT values. */
void lagfx_rb_finish(struct lagfx_vk_state *vk, lagfx_rb_slot_t **slot, const lagfx_shell_callbacks_t *shell, const lagfx_guest_surface_t *gs,
                     const uint8_t *stage, VkImage img, uint32_t mapping, uint64_t ident, uint64_t gen, uint64_t swap_now);
void lagfx_rb_slot_free(struct lagfx_vk_state *vk, lagfx_rb_slot_t **slot);
bool lagfx_rb_slot_allocated(const lagfx_rb_slot_t *slot);   /* a host buffer exists (tests: must stay false while the readback is off) */
#endif

#endif
