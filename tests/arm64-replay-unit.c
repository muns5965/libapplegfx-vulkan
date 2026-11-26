/* render-replay (examples/render-replay.c) self tests. Always run (no dump needed): fp16 conversion, CRC / PNG writer, the CPU model's coverage classification and its negative
 * behaviour, the per-stage binding cap of the resolver's binding map (exactly LAGFX_A64_MAX_REFLECT = 32 bindings map, 33 are rejected) on synthetic SPIR-V.
 * With LAGFX_TEST_DUMP=<dump dir> (real device shaders; not redistributable, so not in the repo) it also runs every synthetic scene on lavapipe: pixel checks against the CPU
 * models with their negative tests, and the capture self test (render with capture on, replay the capture, byte compare, corrupted post image must be detected).
 * SPDX-License-Identifier: MIT */
#define RR_NO_MAIN
#include "../examples/render-replay.c"
#include "air2spv/spv_reflect.h"
#include <assert.h>

#ifdef LAGFX_HAVE_VULKAN
static void test_f16(void) {
    for (uint32_t h = 0; h < 65536; h++) { if ((h & 0x7c00) == 0x7c00 && (h & 0x3ff)) continue; assert(f2h(h2f((uint16_t)h)) == h); }
    assert(f2h(1.0f) == 0x3c00 && f2h(0.5f) == 0x3800 && f2h(65520.0f) == 0x7c00 && f2h(-0.0f) == 0x8000);
    assert(f2h(1.0f + 1.0f / 2048.0f) == 0x3c00);          /* tie: round to even */
    assert(f2h(1.0f + 3.0f / 2048.0f) == 0x3c02);          /* tie: round to even (up) */
    assert(f2h(5.9604645e-8f) == 0x0001);                   /* smallest subnormal */
    puts("PASS: fp16 conversion round trips and rounds to nearest even");
}
static void test_png(void) {
    assert(crc32_((const uint8_t *)"123456789", 9, 0) == 0xcbf43926u);
    uint16_t px[4 * 4 * 4]; for (int i = 0; i < 64; i++) px[i] = f2h(i % 4 == 3 ? 1.0f : 0.5f);
    char path[] = "/tmp/rr-png-XXXXXX"; int fd = mkstemp(path); assert(fd >= 0); close(fd);
    assert(save_png(path, (const uint8_t *)px, 4, 4, 115));
    FILE *f = fopen(path, "rb"); uint8_t b[64]; assert(f && fread(b, 1, 33, f) == 33); fclose(f); unlink(path);
    assert(!memcmp(b, "\x89PNG\r\n\x1a\n", 8) && !memcmp(b + 12, "IHDR", 4) && b[19] == 4 && b[23] == 4 && b[24] == 8 && b[25] == 6);
    uint8_t crc[4]; be32(crc, crc32_(b + 12, 17, 0)); assert(!memcmp(crc, b + 29, 4));
    puts("PASS: PNG writer (signature, IHDR, CRC)");
}
static void test_model(void) {
    mv_t v[3] = { { { -0.5f, -0.5f, 0, 1 }, { 0 } }, { { 0.5f, -0.5f, 0, 1 }, { 0 } }, { { 0.0f, 0.5f, 0, 1 }, { 0 } } };
    v[0].a[0] = 0; v[1].a[0] = 1; v[2].a[0] = 0.5f;
    float a[2];
    assert(model_tri(v, 32, 32, 16, 20, 0.75f, 1, a) == MT_IN && fabsf(a[0] - 0.5f) < 0.3f);       /* inside; y up: row 20 is below the centre */
    assert(model_tri(v, 32, 32, 2, 2, 0.75f, 1, a) == MT_OUT && model_tri(v, 32, 32, 16, 2, 0.75f, 1, a) == MT_OUT);
    assert(model_tri(v, 32, 32, 16, 24, 0.75f, 1, a) == MT_EDGE || model_tri(v, 32, 32, 16, 25, 0.75f, 1, a) == MT_EDGE);   /* the bottom edge is at y = 24 */
    /* perspective: w = 2 at one vertex halves its weight */
    v[2].clip[3] = 2.0f; v[2].clip[0] *= 2.0f; v[2].clip[1] *= 2.0f;
    assert(model_tri(v, 32, 32, 16, 20, 0.75f, 1, a) == MT_IN);
    puts("PASS: CPU model: coverage classification (inside / outside / edge) and perspective interpolation");
}
/* a synthetic fragment-like module with n storage buffer bindings 0..n-1 (set 0) */
static size_t mk_spv(uint32_t *w, uint32_t n) {
    size_t o = 0; uint32_t bound = 4u + n;
    w[o++] = 0x07230203u; w[o++] = 0x00010400u; w[o++] = 0; w[o++] = bound; w[o++] = 0;
    w[o++] = 2u << 16 | 17u; w[o++] = 1;                       /* OpCapability Shader */
    w[o++] = 3u << 16 | 14u; w[o++] = 0; w[o++] = 1;           /* OpMemoryModel Logical GLSL450 */
    w[o++] = 3u << 16 | 71u; w[o++] = 2; w[o++] = 2;           /* OpDecorate %2 Block */
    w[o++] = 5u << 16 | 72u; w[o++] = 2; w[o++] = 0; w[o++] = 35; w[o++] = 0;   /* OpMemberDecorate %2 0 Offset 0 */
    for (uint32_t i = 0; i < n; i++) { w[o++] = 4u << 16 | 71u; w[o++] = 4 + i; w[o++] = 34; w[o++] = 0; w[o++] = 4u << 16 | 71u; w[o++] = 4 + i; w[o++] = 33; w[o++] = i; }
    w[o++] = 4u << 16 | 21u; w[o++] = 1; w[o++] = 32; w[o++] = 0;     /* %1 = OpTypeInt 32 0 */
    w[o++] = 3u << 16 | 30u; w[o++] = 2; w[o++] = 1;                  /* %2 = OpTypeStruct %1 */
    w[o++] = 4u << 16 | 32u; w[o++] = 3; w[o++] = 12; w[o++] = 2;     /* %3 = OpTypePointer StorageBuffer %2 */
    for (uint32_t i = 0; i < n; i++) { w[o++] = 4u << 16 | 59u; w[o++] = 3; w[o++] = 4 + i; w[o++] = 12; }   /* OpVariable %3 %id StorageBuffer */
    return o * 4;
}
static void test_binding_cap(void) {
    static uint32_t w[1024]; lagfx_air_arg_binding_t ab[64]; lagfx_a64_bind_t bind[LAGFX_A64_MAX_BINDINGS];
    for (uint32_t i = 0; i < 64; i++) ab[i] = (lagfx_air_arg_binding_t){ .kind = 1, .metal_index = (int16_t)(i % 32), .written = 0 };
    for (uint32_t n = 31; n <= 33; n++) {
        size_t len = mk_spv(w, n); uint32_t nb = 0; int why = 0;
        lagfx_spv_binding_t tmp[LAGFX_A64_MAX_REFLECT]; assert(lagfx_spv_reflect_bindings((const uint8_t *)w, len, tmp, LAGFX_A64_MAX_REFLECT) == n);   /* the TOTAL, also above the buffer */
        int rc = lagfx_a64_map_stage_bindings((const uint8_t *)w, len, 0, ab, 32, true, true, bind, &nb, &why);
        if (n <= LAGFX_A64_MAX_REFLECT) assert(rc == LAGFX_A64_MAP_OK && nb == n); else assert(rc == LAGFX_A64_MAP_FAIL && nb == 0);
    }
    /* aliased reflected entries of ONE binding (byte-buffer views) count once */
    { size_t len = mk_spv(w, 2); uint32_t nb = 0; int why = 0; assert(lagfx_a64_map_stage_bindings((const uint8_t *)w, len, 0, ab, 1, true, true, bind, &nb, &why) == LAGFX_A64_MAP_FAIL); /* 2 buffers, 1 argument: rejected */ }
    puts("PASS: per-stage reflection cap: 31 and exactly 32 bindings map, 33 are rejected (synthetic SPIR-V)");
}
int main(void) {
    test_f16(); test_png(); test_model(); test_binding_cap();
    const char *dumpdir = getenv("LAGFX_TEST_DUMP");
    if (!dumpdir) { puts("SKIP (scenes + capture): LAGFX_TEST_DUMP not set (real device shaders are not in the repo)"); return 0; }
    snprintf(g_dumpdir, sizeof g_dumpdir, "%s", dumpdir);
    if (!rr_open()) { puts("SKIP (scenes): no Vulkan device"); return 0; }
    static dump_t d; if (!dump_load(&d, dumpdir)) { puts("SKIP (scenes): dump unreadable"); rr_close(); return 0; }
    int bad = 0;
    for (size_t i = 0; i < sizeof g_scenes / sizeof g_scenes[0]; i++) { int rc = run_scene(&g_scenes[i], &d, NULL); if (rc == 1) bad++; }
    if (capture_selftest(&d, NULL) == 1) bad++;
    rr_close();
    assert(!bad);
    puts("PASS: synthetic scenes + capture self test");
    return 0;
}
#else
int main(void) { puts("SKIP: built without Vulkan"); return 77; }
#endif
