/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — display gamma LUT application (CPU, on the present readback)
 * src/vulkan/gamma_apply.h
 *
 * The op 0x08 gamma table (3 planes x count float32, entry i = the output for input i / (count - 1); protocol/gamma_diag.h) that the checksum selection ACCEPTED is applied to the
 * 8-bit scanout bytes just before they are written back to the guest-visible scanout. OFF unless LAGFX_GAMMA_APPLY=1 (only the string "1").
 * Evidence: [V] the table shape and checksum (identity in every capture so far), [N] the plane order (R,G,B is ASSUMED: the captured planes were identical), [N] whether Apple applies it
 * in the display pipe or the guest expects the host to (no non-identity table has been captured), [N] the colour space the table is defined in. An identity table changes nothing,
 * which is also what every real capture so far would produce: a non-identity effect is NOT device-verified.
 */
#ifndef LAGFX_VULKAN_GAMMA_APPLY_H
#define LAGFX_VULKAN_GAMMA_APPLY_H

#include <stddef.h>
#include <stdint.h>

typedef enum { LAGFX_GAMMA_APPLIED = 0, LAGFX_GAMMA_IDENTITY = 1 /* EXACT identity: every entry equals its input */, LAGFX_GAMMA_BAD_TABLE = 2, LAGFX_GAMMA_BAD_ARGS = 3,
               LAGFX_GAMMA_NEAR_IDENTITY = 4 /* differs from identity by at most ONE level somewhere: a real (tiny) transform, deliberately NOT applied; counted apart from the exact identity */ } lagfx_gamma_result_t;

typedef struct { uint64_t applied, applied_srgb, identity_exact, identity_near, bad_table, not_selected, disabled, bad_format, table_builds, cache_hits; } lagfx_gamma_stats_t;

/* Builds three 256-entry byte tables from the planar float table (linear interpolation between entries, clamped to [0,1], rounded). tab[c][v] is the output for input byte v on
 * plane c. Returns BAD_TABLE for count < 2 / count > 4096 / a non-finite value, IDENTITY when no entry differs from v by more than one level, else APPLIED. */
lagfx_gamma_result_t lagfx_gamma_build(const float *planar, uint32_t count, uint8_t tab[3][256]);

/* Applies the tables to `npx` 4-byte pixels. `bgra` != 0: byte order B,G,R,A (plane 0 = R is byte 2); else R,G,B,A. Alpha is never touched. */
void lagfx_gamma_apply8(uint8_t *px, size_t npx, int bgra, const uint8_t tab[3][256]);

/* The present paths' entry point (display_blit.c AND display.c's clear write-back): `vkfmt` is the render target's VkFormat as an integer (37 R8G8B8A8_UNORM, 43 R8G8B8A8_SRGB, 44 B8G8R8A8_UNORM,
 * 50 B8G8R8A8_SRGB; anything else is left alone and counted). Reads the accepted selection (gamma_diag) when its generation changed (the 256-entry tables are rebuilt only then) and applies it
 * when LAGFX_GAMMA_APPLY=1. UNORM and SRGB bytes get the SAME transform: the colour space the table is defined in is NOT known. Counted in the stats; never fails the present. */
lagfx_gamma_result_t lagfx_gamma_apply_present_fmt(uint8_t *px, size_t npx, int vkfmt);
/* Same with an explicit byte order (tests): bgra != 0 -> B,G,R,A. */
lagfx_gamma_result_t lagfx_gamma_apply_present(uint8_t *px, size_t npx, int bgra);

void lagfx_gamma_stats_get(lagfx_gamma_stats_t *s);
void lagfx_gamma_test_reset(void);

#endif
