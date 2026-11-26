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

#ifdef LAGFX_HAVE_VULKAN
/* Read `rt` (B8G8R8A8 or R8G8B8A8, converted to BGRA) back from the GPU and write it into the guest surface described by desc. */
lagfx_status_t lagfx_vk_render_target_to_guest_surface(struct lagfx_vk_state *vk, lagfx_vk_render_target_t *rt,
                                                       const lagfx_shell_callbacks_t *shell, const uint8_t *desc, size_t desc_len);
#endif

#endif
