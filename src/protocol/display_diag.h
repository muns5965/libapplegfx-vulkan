/*
 * libapplegfx-vulkan — display diagnostics (logging and counters only; nothing here changes behaviour)
 * src/protocol/display_diag.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Why: the guest's Apple logo / macOS screen does not appear and the logs cannot say whether the display path got
 * a surface, in which format, a gamma table, cursor traffic or vertical-blank deliveries. This module only READS what the
 * dispatchers already see and prints bounded, tagged lines. It never applies a gamma table to rendering.
 *
 * Log lines (all through LAGFX_LOG, i.e. at LAGFX_LOG_LEVEL=info like the g7-gpu-* lines; every one has a hard cap):
 *   g7-pvg-swap:   t=+<ms>ms op8#<n> mapping=<id> <w>x<h> fourcc=0x<hex> mtl_format=<n>(<name>) vk_format=<n>(<name>) gpu=<0|1> guest_current=<0|1> [first for this (mapping,w,h,format)]
 *                  one line per DISTINCT (mapping, size, mtl format) (max 16), then nothing for repeats; unresolved swaps: g7-pvg-swap: ... mapping=<id> unresolved (max 8 lines)
 *   g7-pvg-gamma:  op8 raw ...    the first 3 channel-4 op 0x08 payloads, first 96 bytes as hex, and where the four known words were found (INFER: the field positions are searched, not assumed)
 *                  region ...     one read-only copy attempt (max 3) of the 0x4000 bytes: first 8 / last 8 32-bit words and an FNV-1a 64 hash. Never applied.
 *                  op0a ...       the first 2 op 0x0a payloads: 9 words, and whether they are the float32 3x3 identity.
 *   g7-pvg-ops:    first op=0x.. ch=<n> t=+<ms>ms   the first sighting of ops 0x37, 0x22, 0x08, 0x0a (4 lines max)
 *                  t=+<s>s ...counters...           with each g7-gpu statistics block (max 40 lines + the final one)
 *   g7-pvg-cursor: first 8 cursor_show / cursor_glyph operations, then every 256th
 *   g7-pvg-vbl:    first 16 vblank timer ticks after an enable and the cumulative counters in g7-pvg-ops
 * Everything marked INFER is an unverified reading of the guest's data; no Apple GPU semantics are assumed beyond the public
 * Metal pixel format numbers and Vulkan format numbers used in the name tables.
 *
 * The pure helpers below are static inline so tests/display-diag-unit.c compiles them without Vulkan or the rest of the library.
 */
#ifndef LAGFX_PROTOCOL_DISPLAY_DIAG_H
#define LAGFX_PROTOCOL_DISPLAY_DIAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- pure helpers ---- */

/* Public MTLPixelFormat numbers (the set protocol/arm64_resources.c mirrors). "?" when not in the table. */
static inline const char *lagfx_dd_mtl_name(uint32_t f) {
    switch (f) {
        case 1: return "A8Unorm"; case 10: return "R8Unorm"; case 11: return "R8Unorm_sRGB"; case 12: return "R8Snorm"; case 13: return "R8Uint"; case 14: return "R8Sint";
        case 20: return "R16Unorm"; case 22: return "R16Snorm"; case 23: return "R16Uint"; case 24: return "R16Sint"; case 25: return "R16Float";
        case 30: return "RG8Unorm"; case 31: return "RG8Unorm_sRGB"; case 32: return "RG8Snorm"; case 33: return "RG8Uint"; case 34: return "RG8Sint";
        case 53: return "R32Uint"; case 54: return "R32Sint"; case 55: return "R32Float";
        case 60: return "RG16Unorm"; case 62: return "RG16Snorm"; case 63: return "RG16Uint"; case 64: return "RG16Sint"; case 65: return "RG16Float";
        case 70: return "RGBA8Unorm"; case 71: return "RGBA8Unorm_sRGB"; case 72: return "RGBA8Snorm"; case 73: return "RGBA8Uint"; case 74: return "RGBA8Sint";
        case 80: return "BGRA8Unorm"; case 81: return "BGRA8Unorm_sRGB";
        case 90: return "RGB10A2Unorm"; case 92: return "RG11B10Float"; case 93: return "RGB9E5Float";
        case 103: return "RG32Uint"; case 104: return "RG32Sint"; case 105: return "RG32Float";
        case 110: return "RGBA16Unorm"; case 112: return "RGBA16Snorm"; case 113: return "RGBA16Uint"; case 114: return "RGBA16Sint"; case 115: return "RGBA16Float";
        case 123: return "RGBA32Uint"; case 124: return "RGBA32Sint"; case 125: return "RGBA32Float";
        default: return "?";
    }
}

/* Public VkFormat numbers (the VkFormat values of the Vulkan specification, so no Vulkan header is needed). "?" when not in the table. */
static inline const char *lagfx_dd_vk_name(int32_t f) {
    switch (f) {
        case 9: return "R8_UNORM"; case 10: return "R8_SNORM"; case 13: return "R8_UINT"; case 14: return "R8_SINT"; case 15: return "R8_SRGB";
        case 16: return "R8G8_UNORM"; case 17: return "R8G8_SNORM"; case 20: return "R8G8_UINT"; case 21: return "R8G8_SINT"; case 22: return "R8G8_SRGB";
        case 37: return "R8G8B8A8_UNORM"; case 38: return "R8G8B8A8_SNORM"; case 41: return "R8G8B8A8_UINT"; case 42: return "R8G8B8A8_SINT"; case 43: return "R8G8B8A8_SRGB";
        case 44: return "B8G8R8A8_UNORM"; case 50: return "B8G8R8A8_SRGB"; case 64: return "A2B10G10R10_UNORM_PACK32";
        case 70: return "R16_UNORM"; case 71: return "R16_SNORM"; case 74: return "R16_UINT"; case 75: return "R16_SINT"; case 76: return "R16_SFLOAT";
        case 77: return "R16G16_UNORM"; case 78: return "R16G16_SNORM"; case 81: return "R16G16_UINT"; case 82: return "R16G16_SINT"; case 83: return "R16G16_SFLOAT";
        case 91: return "R16G16B16A16_UNORM"; case 92: return "R16G16B16A16_SNORM"; case 95: return "R16G16B16A16_UINT"; case 96: return "R16G16B16A16_SINT"; case 97: return "R16G16B16A16_SFLOAT";
        case 98: return "R32_UINT"; case 99: return "R32_SINT"; case 100: return "R32_SFLOAT"; case 101: return "R32G32_UINT"; case 102: return "R32G32_SINT"; case 103: return "R32G32_SFLOAT";
        case 107: return "R32G32B32A32_UINT"; case 108: return "R32G32B32A32_SINT"; case 109: return "R32G32B32A32_SFLOAT";
        case 122: return "B10G11R11_UFLOAT_PACK32"; case 123: return "E5B9G9R9_UFLOAT_PACK32";
        default: return "?";
    }
}

static inline uint32_t lagfx_dd_rd32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }

typedef struct {
    uint32_t nwords;        /* len / 4 */
    uint32_t nonzero;       /* words that are not zero */
    uint32_t first[8], last[8];   /* the first / last 8 little-endian 32-bit words (fewer than 8 words: the rest 0, both arrays overlap) */
    uint64_t hash;          /* FNV-1a 64 over all the bytes of the whole words */
} lagfx_dd_gamma_t;

/* Summary of a buffer; never writes it. Returns 0, or -1 for a NULL / shorter than one word buffer. */
static inline int lagfx_dd_gamma_summarize(const uint8_t *buf, size_t len, lagfx_dd_gamma_t *o) {
    if (!o) return -1;
    memset(o, 0, sizeof *o);
    o->hash = 1469598103934665603ull;
    if (!buf || len < 4u) return -1;
    size_t n = len / 4u;
    o->nwords = (uint32_t)n;
    for (size_t i = 0; i < n * 4u; i++) o->hash = (o->hash ^ buf[i]) * 1099511628211ull;
    for (size_t i = 0; i < n; i++) if (lagfx_dd_rd32(buf + i * 4u)) o->nonzero++;
    for (size_t i = 0; i < 8u && i < n; i++) o->first[i] = lagfx_dd_rd32(buf + i * 4u);
    for (size_t i = 0; i < 8u && i < n; i++) o->last[7u - i] = lagfx_dd_rd32(buf + (n - 1u - i) * 4u);   /* last[7] = the final word */
    return 0;
}

static inline int lagfx_dd_gamma_format(char *out, size_t cap, const lagfx_dd_gamma_t *g) {
    return snprintf(out, cap, "words=%u nonzero=%u first8=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x last8=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x fnv1a64=%016llx",
                    g->nwords, g->nonzero, g->first[0], g->first[1], g->first[2], g->first[3], g->first[4], g->first[5], g->first[6], g->first[7],
                    g->last[0], g->last[1], g->last[2], g->last[3], g->last[4], g->last[5], g->last[6], g->last[7], (unsigned long long)g->hash);
}

/* Index of the first little-endian 32-bit word equal to `w` in buf[0..len), or -1. Searches word positions 0,4,8... */
static inline int lagfx_dd_find_word(const uint8_t *buf, size_t len, uint32_t w) {
    if (!buf) return -1;
    for (size_t i = 0; i + 4u <= len; i += 4u) if (lagfx_dd_rd32(buf + i) == w) return (int)(i / 4u);
    return -1;
}

/* 1 when the 36 bytes are the float32 3x3 identity (1.0f = 0x3f800000 on the diagonal, 0 elsewhere; -0.0f accepted as 0), row- or column-major alike. */
static inline int lagfx_dd_is_identity3x3(const uint8_t *b, size_t len) {
    if (!b || len < 36u) return 0;
    for (int i = 0; i < 9; i++) {
        uint32_t w = lagfx_dd_rd32(b + 4 * i);
        if (i % 4 == 0) { if (w != 0x3f800000u) return 0; }
        else if (w != 0u && w != 0x80000000u) return 0;
    }
    return 1;
}

/* ---- stateful part (display_diag.c) ---- */
typedef struct lagfx_protocol lagfx_protocol_t;

/* Every command the three drain loops see (called from lagfx_diag_cmd): counts ops 0x37 / 0x22 / 0x08 / 0x0a with first-seen time, gamma capture on channel 4 op 0x08, op 0x0a identity check. */
void lagfx_dd_note_cmd(lagfx_protocol_t *p, int src, uint32_t chan, const uint8_t *raw, uint32_t length);
/* One op-0x08 swap named `mapping`; info is what the surface registry knows (resolved == false: nothing). */
typedef struct { bool resolved, gpu, guest_current; uint32_t w, h, fourcc, mtl_format; int32_t vk_format; } lagfx_dd_surface_t;
void lagfx_dd_note_swap(uint64_t mapping, const lagfx_dd_surface_t *s);
/* Cursor operations of the display dispatcher (show: x/y/visible; glyph: width/height). */
void lagfx_dd_note_cursor_show(uint32_t display_id, int x, int y, uint32_t visible);
void lagfx_dd_note_cursor_glyph(uint32_t display_id, uint32_t w, uint32_t h);
/* The vblank timer: `enabled_mask` = the guest's ss[+0x104] word, `raised` = this tick attempts the online-interrupt raise. */
void lagfx_dd_note_vbl_tick(unsigned display, uint32_t enabled_mask, bool raised, bool read_ok);
/* After a raise: the pending word the guest was given (ss[+0x100]) read back on a later tick (-1 = could not read); logged for the first 16 reads. */
void lagfx_dd_note_vbl_readback(unsigned display, int64_t pending_word);
/* The summary line (no newline) into out; returns its length. */
size_t lagfx_dd_stats_line(char *out, size_t cap);
/* Prints the summary through LAGFX_LOG (capped: 40 periodic lines, the final one always). */
void lagfx_dd_log_stats(bool final);

#endif
