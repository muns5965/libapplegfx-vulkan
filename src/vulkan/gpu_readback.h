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
 * Enable: LAGFX_WB_VERIFY=1 (same switch as wbverify) or LAGFX_GPU_READBACK=1 (this alone). Sampling per surface class (BGRA / RGhA): the first 4 candidates,
 * then every LAGFX_GPU_READBACK_EVERY-th (default 16), plus any candidate at least 30 s after the previous sample (catches the first frame after a long
 * silence), at most 60 lines. A "candidate" is one image -> staging copy recording: one per exec that renders into the surface in the immediate write-back mode
 * (LAGFX_ARM64_MAPPED_WRITEBACK=1), one per display swap in =swap mode.
 * Cost when on: one extra GPU copy + one extra host buffer of the surface size per surface (allocated at the first sample) + two guest-page reads. Off: nothing.
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
    lagfx_rb_cmp_t cmp;
} lagfx_rb_verdict_t;
typedef struct { uint64_t candidates, sampled, pass, fail, skip, abandoned, not_sampled, invalidates, lines; } lagfx_rb_totals_t;

size_t lagfx_rb_format_line(char *buf, size_t cap, const lagfx_rb_verdict_t *v, const lagfx_rb_totals_t *t);

bool lagfx_rb_enabled(void);                     /* LAGFX_WB_VERIFY == "1" or LAGFX_GPU_READBACK == "1" */
const lagfx_rb_verdict_t *lagfx_rb_last_verdict(void);
const lagfx_rb_totals_t *lagfx_rb_totals(void);

/* Sampling gate (pure; the clock is passed in). `every` 0 means the default of 16. */
typedef struct { uint64_t seen, last_ns, taken; } lagfx_rb_gate_t;
bool lagfx_rb_gate_admit(lagfx_rb_gate_t *g, uint64_t now_ns, uint32_t every);

/* Tests only. hook: 0 none; 1 flip one bit of staging (B) before the compare; 2 flip one bit of the GPU copy (A); 3 compare B with a stride of width*bpp + 8 (a wrong
 * stride); 4 compare with C read from a corrupted copy (flip one bit of C); 5 treat the memory as non-coherent (invalidate runs even on coherent memory).
 * lagfx_rb_test_reset clears the totals, the last verdict and the gates. */
extern int lagfx_rb_test_hook;
void lagfx_rb_test_reset(void);

#ifdef LAGFX_HAVE_VULKAN
typedef struct lagfx_rb_slot lagfx_rb_slot_t;   /* per surface: the independent buffer + the pending sample */

/* Record the independent copy (if enabled, the gate admits this candidate and the preconditions hold) into `cb`, right after the staging copy of the same image.
 * Precondition checked here: usage has TRANSFER_SRC, format is a known texel size equal to `bpp`, the image is host_valid (the caller only calls for those). A skipped
 * candidate that was admitted logs one SKIP line with the reason. Never fails the caller. */
void lagfx_rb_record(struct lagfx_vk_state *vk, lagfx_rb_slot_t **slot, VkCommandBuffer cb, VkImage img, VkFormat fmt, VkImageUsageFlags usage,
                     uint32_t w, uint32_t h, uint32_t bpp, uint32_t mapping, uint64_t gen, uint64_t swap);
/* After the fence signalled and the write-back ran: compare A (own buffer), B (`stage`, tight rows of w*bpp) and C (fresh guest read of `gs`), log the line.
 * No-op if nothing is pending for `gen`. */
void lagfx_rb_finish(struct lagfx_vk_state *vk, lagfx_rb_slot_t **slot, const lagfx_shell_callbacks_t *shell, const lagfx_guest_surface_t *gs,
                     const uint8_t *stage, uint64_t gen, uint64_t swap_now);
void lagfx_rb_slot_free(struct lagfx_vk_state *vk, lagfx_rb_slot_t **slot);
#endif

#endif
