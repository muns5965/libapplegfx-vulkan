/* SPDX-License-Identifier: MIT */
/* Vulkan < 1.2 feature path (instance.c): with LAGFX_VK_FORCE_LEGACY_FEATURES=1 a newer ICD (lavapipe) is made to take the path a 1.1 driver takes -- the individual
 * per-feature structs are probed and given to vkCreateDevice instead of the unified Vulkan11/12/13 ones, dynamic rendering is "unavailable" and lagfx_vk_init must refuse
 * cleanly (LAGFX_ERR_VULKAN_INIT, no crash, nothing leaked into *vk) AFTER vkCreateDevice succeeded (the refusal line is only printed past it). Also: the default path still
 * initialises and resolves get_buffer_address. NOT verified here: a real Vulkan 1.1 driver. Exit 77 = no usable Vulkan device. */
#define _GNU_SOURCE
#include "vulkan/instance.h"
#include "vulkan/render_pass.h"
#include "vulkan/render_target.h"
#include "vulkan/command.h"
#include "libapplegfx-vulkan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s (line %d)\n", m, __LINE__); g_fail++; } } while (0)

int main(void) {
#ifndef LAGFX_HAVE_VULKAN
    printf("no Vulkan build: skip\n"); return 77;
#else
    struct lagfx_vk_state *vk = NULL;
    lagfx_device_descriptor_t dd; memset(&dd, 0, sizeof dd);
    unsetenv("LAGFX_VK_FORCE_LEGACY_FEATURES");
    if (lagfx_vk_init(&vk, &dd) != LAGFX_OK || !vk || !vk->initialized) { printf("no usable Vulkan device: skip\n"); return 77; }
    CHECK(vk->have_dynamic_rendering, "default path: dynamic rendering");
    CHECK(vk->have_buffer_device_address == (vk->get_buffer_address != NULL), "default path: BDA flag and entry point agree");
    lagfx_vk_shutdown(vk);

    /* The default pipelines created against the legacy render pass: passthrough + cursor exist and the cached pass is used. */
    setenv("LAGFX_VK_FORCE_LEGACY_RENDERPASS", "1", 1);
    vk = NULL;
    if (lagfx_vk_init(&vk, &dd) == LAGFX_OK && vk && vk->initialized) {
        CHECK(lagfx_vk_render_backend(vk) == LAGFX_RENDER_LEGACY, "forced legacy render pass backend");
        CHECK(vk->passthrough_pipeline != VK_NULL_HANDLE, "passthrough pipeline built against a VkRenderPass");
        CHECK(vk->cursor_pipeline != VK_NULL_HANDLE, "cursor pipeline built against a VkRenderPass");
        CHECK(lagfx_vk_render_pass_get(vk, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) != VK_NULL_HANDLE, "render pass cache returns a pass");
        lagfx_vk_shutdown(vk);
    } else CHECK(0, "init with the legacy render-pass backend forced");
    unsetenv("LAGFX_VK_FORCE_LEGACY_RENDERPASS");

    /* Load/store semantics, backend fixed at init, framebuffer cache beyond 64 entries -- on the legacy backend. */
    setenv("LAGFX_VK_FORCE_LEGACY_RENDERPASS", "1", 1);
    vk = NULL;
    if (lagfx_vk_init(&vk, &dd) == LAGFX_OK && vk && vk->initialized) {
        unsetenv("LAGFX_VK_FORCE_LEGACY_RENDERPASS");   /* must not matter any more */
        CHECK(lagfx_vk_render_backend(vk) == LAGFX_RENDER_LEGACY, "backend stays legacy after the variable is gone");
        enum { NRT = 70 };
        lagfx_vk_render_target_t rt[NRT]; memset(rt, 0, sizeof rt);
        int made = 0;
        for (; made < NRT; made++) if (lagfx_vk_render_target_create(vk, 8u, 8u, VK_FORMAT_B8G8R8A8_UNORM, &rt[made]) != LAGFX_OK) break;
        CHECK(made == NRT, "70 render targets");
        VkCommandBuffer cb = VK_NULL_HANDLE;
        if (made == NRT && lagfx_vk_cmdbuf_alloc(vk, &cb) == LAGFX_OK) {
            VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
            vkBeginCommandBuffer(cb, &bi);
            const float red[4] = { 1, 0, 0, 1 };
            int ok = 1;
            for (int i = 0; i < NRT; i++) ok &= lagfx_vk_render_clear_color(vk, cb, &rt[i], red) == LAGFX_OK;   /* > 64 framebuffers recorded into ONE command buffer: none may be evicted */
            CHECK(ok, "70 clears recorded into one command buffer (framebuffer cache grows, no eviction)");
            /* LOAD must keep the red: begin/end with no draw on target 0 after it was cleared (barrier back to attachment is a no-op here: same layout) */
            lagfx_render_color_t keep = { .view = rt[0].view, .format = rt[0].format, .width = 8, .height = 8, .load = VK_ATTACHMENT_LOAD_OP_LOAD, .store = VK_ATTACHMENT_STORE_OP_STORE, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
            CHECK(lagfx_vk_render_begin(vk, cb, &keep) == LAGFX_OK, "LOAD pass begins");
            lagfx_vk_render_end(vk, cb);
            vkEndCommandBuffer(cb);
            VkFence f = VK_NULL_HANDLE; VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            vkCreateFence(vk->device, &fci, NULL, &f);
            VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
            CHECK(vkQueueSubmit(vk->graphics_queue, 1, &si, f) == VK_SUCCESS && vkWaitForFences(vk->device, 1, &f, VK_TRUE, 2000000000ull) == VK_SUCCESS, "submit + wait");
            vkDestroyFence(vk->device, f, NULL);
            lagfx_vk_cmdbuf_free(vk, cb);
            uint8_t px[8 * 8 * 4]; size_t stride = 0;
            CHECK(lagfx_vk_render_target_readback(vk, &rt[0], px, sizeof px, &stride) == LAGFX_OK && px[2] == 255 && px[0] == 0, "LOAD preserved the cleared colour (CLEAR was applied before)");
            CHECK(lagfx_vk_render_target_readback(vk, &rt[NRT - 1], px, sizeof px, &stride) == LAGFX_OK && px[2] == 255, "the 70th target holds its clear");
        } else CHECK(0, "command buffer for the load/store test");
        for (int i = 0; i < made; i++) lagfx_vk_render_target_destroy(vk, &rt[i]);
        lagfx_vk_shutdown(vk);
    } else CHECK(0, "init for the load/store test");
    unsetenv("LAGFX_VK_FORCE_LEGACY_RENDERPASS");

    char path[] = "/tmp/legacyfeat-XXXXXX"; int fd = mkstemp(path);
    if (fd < 0) { printf("mkstemp failed\n"); return 1; }
    fflush(stderr); int saved = dup(2); dup2(fd, 2);
    setenv("LAGFX_VK_FORCE_LEGACY_FEATURES", "1", 1);
    vk = NULL;
    lagfx_status_t st = lagfx_vk_init(&vk, &dd);
    fflush(stderr); dup2(saved, 2); close(saved);
    unsetenv("LAGFX_VK_FORCE_LEGACY_FEATURES");
    char buf[1 << 16]; ssize_t n = pread(fd, buf, sizeof buf - 1, 0); close(fd); unlink(path);
    buf[n > 0 ? n : 0] = 0;
    CHECK(st == LAGFX_ERR_VULKAN_INIT, "legacy path without dynamic rendering: refused with LAGFX_ERR_VULKAN_INIT");
    CHECK(vk == NULL || !vk->initialized, "no initialised state is handed out");
    CHECK(strstr(buf, "dynamicRendering unsupported"), "the refusal is explained on stderr (it is only reached after vkCreateDevice succeeded with the individual-struct chain)");
    CHECK(strstr(buf, "g7-probe:"), "the raw probe line is printed");
    if (g_fail) { printf("FAILED (%d)\n", g_fail); return 1; }
    printf("OK\n"); return 0;
#endif
}
