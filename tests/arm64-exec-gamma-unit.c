/* ARM64 render exec, sRGB colour targets, NUMERIC on lavapipe (LAGFX_ARM64_EXEC=1): a render target of Metal pixel format 81 (BGRA8Unorm_sRGB) is cleared and drawn,
 * and the stored bytes are compared with the sRGB transfer function of the Vulkan spec (c<=0.0031308 ? 12.92c : 1.055c^(1/2.4)-0.055) -- written here from the formula, not
 * taken from the library. The SAME clear into format 80 (BGRA8Unorm) is the negative control: it must store the linear value (0.5 -> 128), so a library that mapped 81 to
 * UNORM (no gamma encode) fails the first half and a library that mapped 80 to SRGB fails the second. Alpha is never encoded. Reuses the scaffolding of
 * arm64-exec-run-unit.c. lavapipe only: says nothing about how the Adreno driver or the display path treats the colour space. SPDX-License-Identifier: MIT */
#define main run_unit_main
#include "arm64-exec-run-unit.c"
#undef main

#ifdef LAGFX_HAVE_VULKAN
#include <math.h>
static double srgb_enc(double c) { return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
static int expect_byte(double c, int enc) { double v = (enc ? srgb_enc(c) : c) * 255.0; return (int)floor(v + 0.5); }
/* clear the REF_TEX target (format byte `fmt`) to `clr`, optionally draw the red triangle, return the pixel at (px_x, px_y) as {b,g,r,a} */
static void run_fmt(uint8_t fmt, const double clr[4], int draw, int x, int y, uint8_t out[4]) {
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    lagfx_task_entry_t *t = def_task(p, 1);
    build_objects();
    { uint32_t off = (12 + 0x18 + 36) + 12 + 2; va_write(VA_PAY + off, &fmt, 1); }   /* the v1 description's pixel-format byte of the texture built by build_objects() */
    lagfx_a64_exec_set_pipeline_resolver(p, resolver, NULL);
    cb_reset(); cb_pass(REF_TEX, 2, clr);
    if (draw) { cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, 0); cb_draw(0, 3); }
    exec(p, 1, cb_finish(), true);
    uint8_t img[RT_W * RT_H * 4]; uint32_t w, h;
    assert(lagfx_a64_exec_readback_texture(p, t, REF_TEX, img, sizeof img, &w, &h) && w == RT_W && h == RT_H);
    memcpy(out, img + ((uint32_t)y * RT_W + (uint32_t)x) * 4, 4);
    lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); free(p);
}
static int near(int a, int b) { return abs(a - b) <= 1; }
static void test_gamma(void) {
    const double c[4] = { 0.5, 0.25, 1.0, 0.5 };    /* R G B A as the guest writes the clear colour */
    const double lin[4] = { 0.5, 0.25, 0.0031308 / 2, 0.5 };   /* the last one is in the linear segment of the curve */
    uint8_t o[4];
    run_fmt(81, c, 0, 1, 1, o);   /* stored order B G R A */
    printf("  sRGB  clear: B=%d G=%d R=%d A=%d (expect R=%d G=%d B=%d A=%d)\n", o[0], o[1], o[2], o[3], expect_byte(c[0], 1), expect_byte(c[1], 1), expect_byte(c[2], 1), expect_byte(c[3], 0));
    assert(near(o[2], expect_byte(c[0], 1)) && near(o[1], expect_byte(c[1], 1)) && near(o[0], expect_byte(c[2], 1)) && near(o[3], expect_byte(c[3], 0)));
    run_fmt(81, lin, 0, 1, 1, o);
    assert(near(o[0], expect_byte(lin[2], 1)));                                    /* linear segment: 12.92 c */
    run_fmt(80, c, 0, 1, 1, o);   /* negative control: UNORM stores the linear value */
    printf("  UNORM clear: B=%d G=%d R=%d A=%d\n", o[0], o[1], o[2], o[3]);
    assert(near(o[2], expect_byte(c[0], 0)) && near(o[1], expect_byte(c[1], 0)) && near(o[0], expect_byte(c[2], 0)) && near(o[3], expect_byte(c[3], 0)));
    assert(abs(expect_byte(c[0], 1) - expect_byte(c[0], 0)) > 50);                 /* the two expectations really differ (a test that cannot fail proves nothing) */
    /* a draw of the trivial red fragment shader over the clear: full-coverage pixel (8, 8) = 255,0,0 in both formats (1.0 and 0.0 are fixed points of the curve) */
    run_fmt(81, c, 1, 8, 8, o);
    assert(o[2] == 255 && o[1] == 0 && o[0] == 0);
    puts("PASS: sRGB (81) render target stores gamma-encoded bytes (0.5 -> 188, 0.25 -> 137, linear segment, alpha unencoded); UNORM (80) control stays linear; draw 1.0/0.0 fixed points");
}
int main(void) {
    g_mem = calloc(1, MEM_SIZE);
    g_dev = make_device();
    assert(g_dev);
    if (!(g_dev->vk && g_dev->vk->initialized)) { puts("SKIP: no Vulkan device"); lagfx_device_free(g_dev); free(g_mem); return 77; }
    test_gamma();
    if (g_vs) vkDestroyShaderModule(g_dev->vk->device, g_vs, NULL);
    if (g_fs) vkDestroyShaderModule(g_dev->vk->device, g_fs, NULL);
    lagfx_device_free(g_dev);
    free(g_mem);
    return 0;
}
#else
int main(void) { puts("SKIP: built without Vulkan"); return 77; }
#endif
