/* Cursor composition (lagfx_vk_cursor_upload_glyph + lagfx_vk_cursor_draw) over a cleared render target, read back and compared pixel by pixel. Lavapipe, both render backends.
 * The glyph here is a TEST image in the byte order the upload takes (B,G,R,A as stored); nothing is assumed about Apple's own glyph format (still unknown: display_cursor_glyph captures
 * no pixels yet). What is checked: opaque pixels replace the background, alpha 0 leaves it, half alpha blends (src-alpha over), the hotspot shifts the glyph, a glyph that runs off any
 * edge is clipped without touching other pixels, and no glyph / not valid draws nothing. */
#include "vulkan/instance.h"
#include "vulkan/render_target.h"
#include "vulkan/render_pass.h"
#include "vulkan/command.h"
#include "vulkan/draw_record.h"
#include "vulkan/cursor.h"
#include "libapplegfx-vulkan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL (line %d): ", __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

#define TW 32
#define TH 24
#define GW 8
#define GH 8
static const uint8_t BG[4] = { 40, 80, 120, 255 };   /* B,G,R,A of the cleared target */

typedef struct { lagfx_vk_render_target_t *rt; int x, y, hx, hy; bool draw; } job_t;

static bool one_shot(struct lagfx_vk_state *vk, job_t *j) {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (lagfx_vk_cmdbuf_alloc(vk, &cb) != LAGFX_OK) return false;
    bool ok = false; VkFence f = VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) goto out;
    lagfx_vk_cmd_rt_to_color(cb, j->rt);
    lagfx_render_color_t c = { .view = j->rt->view, .format = j->rt->format, .width = j->rt->width, .height = j->rt->height, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               .load = VK_ATTACHMENT_LOAD_OP_CLEAR, .store = VK_ATTACHMENT_STORE_OP_STORE,
                               .clear = { .float32 = { BG[2] / 255.0f, BG[1] / 255.0f, BG[0] / 255.0f, 1.0f } } };
    if (lagfx_vk_render_begin(vk, cb, &c) != LAGFX_OK) { vkEndCommandBuffer(cb); goto out; }
    lagfx_status_t cs = j->draw ? lagfx_vk_cursor_draw(vk, cb, TW, TH, (int16_t)j->x, (int16_t)j->y, (uint16_t)j->hx, (uint16_t)j->hy) : LAGFX_OK;
    lagfx_vk_render_end(vk, cb);
    if (cs != LAGFX_OK || vkEndCommandBuffer(cb) != VK_SUCCESS) goto out;
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateFence(vk->device, &fci, NULL, &f) != VK_SUCCESS) goto out;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    if (vkQueueSubmit(vk->graphics_queue, 1, &si, f) != VK_SUCCESS) goto out;
    ok = vkWaitForFences(vk->device, 1, &f, VK_TRUE, 5000000000ull) == VK_SUCCESS;
out:
    if (f) vkDestroyFence(vk->device, f, NULL);
    lagfx_vk_cmdbuf_free(vk, cb);
    return ok;
}

static uint8_t g_img[TW * TH * 4];
static bool render(struct lagfx_vk_state *vk, lagfx_vk_render_target_t *rt, int x, int y, int hx, int hy, bool draw) {
    job_t j = { rt, x, y, hx, hy, draw };
    size_t stride = 0;
    return one_shot(vk, &j) && lagfx_vk_render_target_readback(vk, rt, g_img, sizeof g_img, &stride) == LAGFX_OK && stride == TW * 4u;
}
static const uint8_t *px(int x, int y) { return g_img + ((size_t)y * TW + (size_t)x) * 4u; }
static bool is_bg(int x, int y) { return !memcmp(px(x, y), BG, 4); }

/* glyph: 8 x 8, B,G,R,A. (0..3, 0..3) opaque red; (4..7, 0..3) alpha 0 (any colour); (0..3, 4..7) half-alpha blue (A = 128); the rest alpha 0 */
static uint8_t g_glyph[GW * GH * 4];
static void make_glyph(void) {
    memset(g_glyph, 0, sizeof g_glyph);
    for (int y = 0; y < GH; y++) for (int x = 0; x < GW; x++) {
        uint8_t *p = g_glyph + ((size_t)y * GW + (size_t)x) * 4u;
        if (x < 4 && y < 4) { p[0] = 0; p[1] = 0; p[2] = 255; p[3] = 255; }
        else if (x >= 4 && y < 4) { p[0] = 255; p[1] = 255; p[2] = 255; p[3] = 0; }
        else if (x < 4 && y >= 4) { p[0] = 255; p[1] = 0; p[2] = 0; p[3] = 128; }
    }
}

static void run_backend(int legacy) {
    const char *bn = legacy ? "legacy" : "dynamic";
    if (legacy) setenv("LAGFX_VK_FORCE_LEGACY_RENDERPASS", "1", 1); else unsetenv("LAGFX_VK_FORCE_LEGACY_RENDERPASS");
    struct lagfx_vk_state *vk = NULL; lagfx_device_descriptor_t dd; memset(&dd, 0, sizeof dd);
    if (lagfx_vk_init(&vk, &dd) != LAGFX_OK || !vk || !vk->initialized) { printf("SKIP: no Vulkan device (%s)\n", bn); exit(77); }
    lagfx_vk_render_target_t rt; memset(&rt, 0, sizeof rt);
    CHECK(lagfx_vk_render_target_create(vk, TW, TH, VK_FORMAT_B8G8R8A8_UNORM, &rt) == LAGFX_OK, "%s: target", bn);
    make_glyph();

    /* no glyph uploaded yet: nothing is drawn, the draw call is not an error */
    CHECK(render(vk, &rt, 10, 6, 0, 0, true), "%s: render without glyph", bn);
    int stray = 0; for (int y = 0; y < TH; y++) for (int x = 0; x < TW; x++) stray += !is_bg(x, y);
    CHECK(stray == 0, "%s: no glyph -> background only (%d stray pixels)", bn, stray);

    CHECK(lagfx_vk_cursor_upload_glyph(vk, g_glyph, GW, GH, GW * 4u) == LAGFX_OK, "%s: upload glyph", bn);

    /* position (10,6), hotspot (0,0): glyph pixel (gx,gy) lands at (10+gx, 6+gy) */
    CHECK(render(vk, &rt, 10, 6, 0, 0, true), "%s: render", bn);
    CHECK(!memcmp(px(10, 6), (uint8_t[]){0, 0, 255, 255}, 4) && !memcmp(px(13, 9), (uint8_t[]){0, 0, 255, 255}, 4), "%s: opaque red block replaces the background: (%u,%u,%u,%u)", bn, px(10, 6)[0], px(10, 6)[1], px(10, 6)[2], px(10, 6)[3]);
    CHECK(is_bg(14, 6) && is_bg(17, 9), "%s: alpha-0 glyph pixels leave the background", bn);
    {   /* half alpha blue over BG (src-alpha over): out = src*a + dst*(1-a), a = 128/255 */
        const uint8_t *p = px(10, 10); float a = 128.0f / 255.0f;
        float eb = 255.0f * a + BG[0] * (1.0f - a), eg = 0.0f * a + BG[1] * (1.0f - a), er = 0.0f * a + BG[2] * (1.0f - a);
        CHECK(p[3] >= 253, "%s: a translucent cursor pixel leaves the target alpha opaque (alpha %u)", bn, p[3]);
        CHECK(abs((int)p[0] - (int)(eb + 0.5f)) <= 2 && abs((int)p[1] - (int)(eg + 0.5f)) <= 2 && abs((int)p[2] - (int)(er + 0.5f)) <= 2,
              "%s: half alpha blends over the background: got (%u,%u,%u) want about (%.0f,%.0f,%.0f)", bn, p[0], p[1], p[2], (double)eb, (double)eg, (double)er);
    }
    CHECK(is_bg(9, 6) && is_bg(10, 5) && is_bg(14, 14) && is_bg(10, 14), "%s: pixels around the glyph box are untouched", bn);

    /* hotspot (2,3): the glyph moves by (-2,-3): the red block starts at (8,3) */
    CHECK(render(vk, &rt, 10, 6, 2, 3, true), "%s: render hot", bn);
    CHECK(!memcmp(px(8, 3), (uint8_t[]){0, 0, 255, 255}, 4) && is_bg(7, 3) && is_bg(8, 2), "%s: hotspot (2,3) shifts the glyph to (8,3)", bn);

    /* clipping: top-left (-2,-2) shows glyph pixels 2.. of the red block at target (0..1, 0..1); nothing else changes */
    CHECK(render(vk, &rt, -2, -2, 0, 0, true), "%s: render clipped top-left", bn);
    CHECK(!memcmp(px(0, 0), (uint8_t[]){0, 0, 255, 255}, 4) && !memcmp(px(1, 1), (uint8_t[]){0, 0, 255, 255}, 4) && is_bg(2, 0) && !is_bg(0, 2) && is_bg(TW - 1, TH - 1),
          "%s: a glyph partly above/left of the target is clipped (2x2 red, then the half-alpha blue rows below)", bn);
    /* bottom-right: glyph origin (TW-2, TH-2) shows only glyph pixels (0..1, 0..1): red */
    CHECK(render(vk, &rt, TW - 2, TH - 2, 0, 0, true), "%s: render clipped bottom-right", bn);
    CHECK(!memcmp(px(TW - 1, TH - 1), (uint8_t[]){0, 0, 255, 255}, 4) && !memcmp(px(TW - 2, TH - 2), (uint8_t[]){0, 0, 255, 255}, 4) && is_bg(TW - 3, TH - 3) && is_bg(0, 0),
          "%s: a glyph running off the bottom-right is clipped (2x2 red, then the half-alpha blue rows below)", bn);
    /* fully outside: nothing changes */
    CHECK(render(vk, &rt, TW + 10, TH + 10, 0, 0, true), "%s: render outside", bn);
    stray = 0; for (int y = 0; y < TH; y++) for (int x = 0; x < TW; x++) stray += !is_bg(x, y);
    CHECK(stray == 0, "%s: a glyph fully outside draws nothing (%d stray)", bn, stray);

    lagfx_vk_render_target_destroy(vk, &rt);
    lagfx_vk_shutdown(vk);
    printf("%s: cursor composition checked\n", bn);
}

int main(void) {
    run_backend(0);
    run_backend(1);
    if (g_fail) { printf("FAILED: %d\n", g_fail); return 1; }
    puts("PASS cursor-composite-unit");
    return 0;
}
