/*
 * libapplegfx-vulkan — display diagnostics, pure helpers (standalone)
 * tests/display-diag-unit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Compiles only the static-inline helpers of src/protocol/display_diag.h: format-name mapping, the gamma region summarizer on a
 * synthetic buffer, word search, 3x3 identity check. No Vulkan, no device.
 */
#include "../src/protocol/display_diag.h"
#include <stdlib.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void put32(uint8_t *b, size_t i, uint32_t v) { b[i * 4] = (uint8_t)v; b[i * 4 + 1] = (uint8_t)(v >> 8); b[i * 4 + 2] = (uint8_t)(v >> 16); b[i * 4 + 3] = (uint8_t)(v >> 24); }

int main(void) {
    /* names */
    CHECK(strcmp(lagfx_dd_mtl_name(80), "BGRA8Unorm") == 0);
    CHECK(strcmp(lagfx_dd_mtl_name(81), "BGRA8Unorm_sRGB") == 0);
    CHECK(strcmp(lagfx_dd_mtl_name(115), "RGBA16Float") == 0);
    CHECK(strcmp(lagfx_dd_mtl_name(70), "RGBA8Unorm") == 0);
    CHECK(strcmp(lagfx_dd_mtl_name(0), "?") == 0 && strcmp(lagfx_dd_mtl_name(9999), "?") == 0);
    CHECK(strcmp(lagfx_dd_vk_name(44), "B8G8R8A8_UNORM") == 0);
    CHECK(strcmp(lagfx_dd_vk_name(50), "B8G8R8A8_SRGB") == 0);
    CHECK(strcmp(lagfx_dd_vk_name(97), "R16G16B16A16_SFLOAT") == 0);
    CHECK(strcmp(lagfx_dd_vk_name(37), "R8G8B8A8_UNORM") == 0);
    CHECK(strcmp(lagfx_dd_vk_name(0), "?") == 0 && strcmp(lagfx_dd_vk_name(-1), "?") == 0);

    /* gamma summarizer: 0x4000 bytes = 4096 words, word i = i + 1 except a zero at 100 */
    size_t len = 0x4000u; uint8_t *b = calloc(1, len);
    for (size_t i = 0; i < len / 4; i++) put32(b, i, (uint32_t)(i + 1));
    put32(b, 100, 0);
    uint8_t *copy = malloc(len); memcpy(copy, b, len);
    lagfx_dd_gamma_t g;
    CHECK(lagfx_dd_gamma_summarize(b, len, &g) == 0);
    CHECK(g.nwords == 4096u && g.nonzero == 4095u);
    CHECK(g.first[0] == 1u && g.first[7] == 8u);
    CHECK(g.last[0] == 4089u && g.last[7] == 4096u);
    CHECK(memcmp(b, copy, len) == 0);   /* read-only */
    lagfx_dd_gamma_t g2; lagfx_dd_gamma_summarize(b, len, &g2);
    CHECK(g.hash == g2.hash);
    b[5000] ^= 1u; lagfx_dd_gamma_summarize(b, len, &g2);
    CHECK(g.hash != g2.hash);
    char line[500];
    int n = lagfx_dd_gamma_format(line, sizeof line, &g);
    CHECK(n > 0 && (size_t)n < sizeof line && strstr(line, "words=4096 nonzero=4095 first8=00000001,00000002") && strstr(line, "last8=00000ff9") && strstr(line, "00001000 fnv1a64="));
    /* short buffers: fewer than 8 words, odd tail ignored, empty refused */
    uint8_t s[10] = { 1, 0, 0, 0, 2, 0, 0, 0, 9, 9 };
    CHECK(lagfx_dd_gamma_summarize(s, sizeof s, &g) == 0 && g.nwords == 2u && g.first[1] == 2u && g.last[7] == 2u && g.last[6] == 1u);
    CHECK(lagfx_dd_gamma_summarize(s, 3, &g) == -1 && lagfx_dd_gamma_summarize(NULL, 8, &g) == -1);
    /* known FNV-1a 64 of "a" (4-byte word 0x00000061 = bytes 61 00 00 00): differs from an empty-offset basis, and basis for zero words */
    uint8_t z[4] = { 0 };
    lagfx_dd_gamma_summarize(z, 4, &g);
    CHECK(g.hash == ((((1469598103934665603ull ^ 0) * 1099511628211ull ^ 0) * 1099511628211ull ^ 0) * 1099511628211ull ^ 0) * 1099511628211ull);

    /* word search */
    uint8_t pl[40] = { 0 };
    put32(pl, 3, 0x118000u); put32(pl, 4, 0x4000u); put32(pl, 7, 0x48400000u);
    CHECK(lagfx_dd_find_word(pl, sizeof pl, 0x118000u) == 3 && lagfx_dd_find_word(pl, sizeof pl, 0x4000u) == 4);
    CHECK(lagfx_dd_find_word(pl, sizeof pl, 257u) == -1 && lagfx_dd_find_word(pl, sizeof pl, 0x48400000u) == 7);
    CHECK(lagfx_dd_find_word(pl, 6, 0x4000u) == -1);   /* a partial word is not searched */

    /* 3x3 identity */
    uint8_t id[36] = { 0 };
    put32(id, 0, 0x3f800000u); put32(id, 4, 0x3f800000u); put32(id, 8, 0x3f800000u);
    CHECK(lagfx_dd_is_identity3x3(id, sizeof id) == 1);
    put32(id, 1, 0x80000000u); CHECK(lagfx_dd_is_identity3x3(id, sizeof id) == 1);   /* -0.0 */
    put32(id, 1, 0x3f800000u); CHECK(lagfx_dd_is_identity3x3(id, sizeof id) == 0);
    put32(id, 1, 0u); put32(id, 8, 0x40000000u); CHECK(lagfx_dd_is_identity3x3(id, sizeof id) == 0);
    CHECK(lagfx_dd_is_identity3x3(id, 35) == 0 && lagfx_dd_is_identity3x3(NULL, 36) == 0);

    free(b); free(copy);
    printf(fails ? "display-diag-unit: %d FAILED\n" : "display-diag-unit: all passed\n", fails);
    return fails != 0;
}
