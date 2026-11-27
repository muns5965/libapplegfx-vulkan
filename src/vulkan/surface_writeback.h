/*
 * libapplegfx-vulkan — write rendered pixels into a guest display surface (ARM64 vmapple swap path)
 * src/vulkan/surface_writeback.h
 *
 * Guest surface descriptor (Apple Henge devices/pvg_lite.c decode_surface; the shell owns the mapping-id -> descriptor table):
 *   +0x00 u32 page list: bit 0 set, (value & ~1) << 12 = GPA of the page list
 *   +0x04 u32 fourcc ('BGRA' = 0x42475241; others refused)
 *   +0x18 u64 allocation bytes, +0x20 u64 width, +0x28 u64 height, +0x30 u64 bytes per row, +0x60 u32 first-page byte offset
 *   page list: ceil((alloc + first) / 16 KiB) u32 entries (<= 4096); entry bit 0 = present, (entry & ~1) << 12 = GPA of one 16 KiB page.
 *   pixel (x, y) lives at surface byte offset first + y * row + 4 * x; the surface is the 16 KiB pages laid back to back.
 * The same bounds as Henge are enforced (w,h <= 8192, row >= w*4 and <= 0x10000, alloc <= 64 MiB, the last pixel inside the pages).
 *
 * Threading: no state. The shell's read_memory / write_memory are called on the caller's thread, and no library lock is held across them.
 */
#ifndef LAGFX_VULKAN_SURFACE_WRITEBACK_H
#define LAGFX_VULKAN_SURFACE_WRITEBACK_H

#include "instance.h"
#include "render_target.h"
#include "libapplegfx-vulkan.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LAGFX_SURF_DESC_BYTES   0x64u
#define LAGFX_SURF_PAGE_SHIFT   14u
#define LAGFX_FOURCC_BGRA       0x42475241u
#define LAGFX_FOURCC_RGHA       0x52476841u   /* 'RGhA': RGBA16Float, 8 bytes per pixel (the guest's main display surface) */

typedef struct {
    uint32_t fourcc, width, height, first_offset, npages;
    uint32_t bpp;   /* bytes per pixel: 4 (BGRA) or 8 (RGhA, only from lagfx_guest_surface_decode_any) */
    uint64_t row_bytes, alloc, page_list_gpa;
} lagfx_guest_surface_t;

/* Decode + bounds-check a descriptor. LAGFX_ERR_INVALID_ARG on anything Henge's decode_surface rejects or a fourcc other than BGRA. */
lagfx_status_t lagfx_guest_surface_decode(const uint8_t *desc, size_t desc_len, lagfx_guest_surface_t *out);

/* Like lagfx_guest_surface_decode but also accepts 'RGhA' (bpp 8; same row / first-offset / page bounds with width * 8). Only the mirror of mapped textures uses it; the
 * display swap path (write_bgra) keeps refusing everything but BGRA. */
lagfx_status_t lagfx_guest_surface_decode_any(const uint8_t *desc, size_t desc_len, lagfx_guest_surface_t *out);

/* Write w x h BGRA pixels (src, src_stride bytes per row) at the top-left of the surface: min(w, surface width) x min(h, surface height).
 * Reads the page list with shell->read_memory, then writes row by row with shell->write_memory (a row may straddle pages). Every page that will be
 * touched is checked first: a missing page or unreadable list fails with nothing written. Bytes outside the written width stay as they were. */
lagfx_status_t lagfx_guest_surface_write_bgra(const lagfx_shell_callbacks_t *shell, const lagfx_guest_surface_t *s,
                                              const uint8_t *src, uint32_t w, uint32_t h, size_t src_stride);

/* (w x h pixels of s->bpp bytes; `stride` >= w * bpp.) Tolerant row-by-row transfer for the ARM64 exec mirror of mapped (type 11) textures. Moves min(w, surface width) x min(h, surface height) BGRA pixels between
 * `buf` (tightly strided by `stride` bytes) and the guest pages at pixel (0, 0): surface byte offset first_offset + y * row_bytes + 4 * x. Unlike write_bgra a
 * missing / non-present / unreadable page does NOT fail the transfer: the bytes of that page are skipped (to_guest: not written, guest keeps what it had; from
 * guest: the buffer bytes are zeroed) and the page is counted once in st->pages_skipped. Every chunk is bounds-checked against the descriptor's page count. Only
 * an unreadable page list (or bad arguments) fails, with nothing transferred. st may be NULL. */
typedef struct { uint64_t bytes, pages_skipped, chunks_skipped; } lagfx_guest_surface_xfer_stats_t;
lagfx_status_t lagfx_guest_surface_xfer(const lagfx_shell_callbacks_t *shell, const lagfx_guest_surface_t *s, uint8_t *buf, uint32_t w, uint32_t h, size_t stride,
                                        bool to_guest, lagfx_guest_surface_xfer_stats_t *st);

/* READ-ONLY writeback verification (diagnostic, opt-in by LAGFX_WB_VERIFY=1; see lagfx_wbv_*). Reads the guest pages back (xfer, to_guest=false, into a temporary tight
 * buffer) and compares the valid pixels (w * bpp bytes a row, row padding excluded) with `buf` (stride bytes a row). BGRA only (bpp 4); anything else is refused.
 * Pixel bytes are taken as b, g, r, a at +0..+3 (INFER: from the fourcc 'BGRA' little-endian word, alpha in the high byte). A pixel whose b/g/r differ counts in rgb_mismatch
 * (whatever its alpha); one whose b/g/r are equal but alpha differs counts in alpha_only_mismatch. hash_* = Henge pvgpix' hash: FNV-1a basis 1469598103934665603, prime
 * 1099511628211, one step per whole little-endian 32-bit pixel ((h ^ px) * prime), every pixel of w x h, alpha included, not sampled. The guest is read twice;
 * stable = both reads hashed equal (a mismatch with stable=0 means the guest wrote meanwhile; with stable=1 a copy error or a guest write BEFORE the first read). */
typedef struct {
    uint32_t w, h, bpp;
    uint64_t pixels, rgb_mismatch, alpha_only_mismatch, pages_skipped;
    int32_t first_x, first_y;                  /* first differing pixel (row-major), -1 if none */
    uint64_t hash_buf, hash_guest, hash_guest2;
    bool stable;
} lagfx_wbv_result_t;
lagfx_status_t lagfx_guest_surface_verify(const lagfx_shell_callbacks_t *shell, const lagfx_guest_surface_t *s, const uint8_t *buf, uint32_t w, uint32_t h, size_t stride, lagfx_wbv_result_t *r);

/* Gate: opt-in env var, at most LAGFX_WBV_MAX_COMPARES per state, LAGFX_WBV_MIN_GAP_NS between two (the first is immediate). now_ns = a monotonic clock. */
#define LAGFX_WBV_MAX_COMPARES 8u
#define LAGFX_WBV_MIN_GAP_NS   10000000000ull
typedef struct { uint32_t done; uint64_t last_ns; bool capped_logged; } lagfx_wbv_gate_t;
bool lagfx_wbv_enabled(void);                                   /* getenv("LAGFX_WB_VERIFY") == "1" */
bool lagfx_wbv_admit(lagfx_wbv_gate_t *g, uint64_t now_ns);     /* true: do one compare now (counts it) */
uint64_t lagfx_wbv_now_ns(void);

#ifdef LAGFX_HAVE_VULKAN
/* Read `rt` (B8G8R8A8 or R8G8B8A8, converted to BGRA) back from the GPU and write it into the guest surface described by desc. */
lagfx_status_t lagfx_vk_render_target_to_guest_surface(struct lagfx_vk_state *vk, lagfx_vk_render_target_t *rt,
                                                       const lagfx_shell_callbacks_t *shell, const uint8_t *desc, size_t desc_len);
#endif

#endif
