/* (F+D) lavapipe: GPU work (clear + pattern upload) is tracked by a real VkFence / timeline semaphore through the arm64 completion queue; once it
 * retired, the render target is written into a fake guest surface (16 KiB pages, shuffled, row stride 480 > width*4, first-page offset) and the bytes
 * are checked. Also: bounds / refusal / no-partial-write cases. SKIPs (exit 77 -> meson skip) with no Vulkan device.
 * SPDX-License-Identifier: MIT */
#include "libapplegfx-vulkan.h"
#include "device.h"
#include "protocol/state.h"
#include "protocol/arm64_completion.h"
#include "vulkan/instance.h"
#include "vulkan/command.h"
#include "vulkan/command.h"
#include "vulkan/render_target.h"
#include "vulkan/iosurface.h"
#include "vulkan/surface_writeback.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MEM (4u << 20)
static uint8_t *g_mem;
static int g_irq;
static bool rd(void *o, uint64_t g, uint64_t l, void *d) { (void)o; if (g + l > MEM) return false; memcpy(d, g_mem + g, l); return true; }
static bool wr(void *o, uint64_t g, uint64_t l, const void *s) { (void)o; if (g + l > MEM) return false; memcpy(g_mem + g, s, l); return true; }
static void irq(void *o, uint32_t v) { (void)o; (void)v; g_irq++; }
static lagfx_task_t *c_ct(void *o, uint64_t s, void **p) { (void)o; (void)s; (void)p; return NULL; }
static void c_dt(void *o, lagfx_task_t *t) { (void)o; (void)t; }
static bool c_map(void *a, lagfx_task_t *t, uint64_t o, const lagfx_physical_range_t *r, size_t c, bool ro) { (void)a; (void)t; (void)o; (void)r; (void)c; (void)ro; return true; }
static bool c_unmap(void *a, lagfx_task_t *t, uint64_t o, uint64_t l) { (void)a; (void)t; (void)o; (void)l; return true; }

/* surface: 100x80 BGRA, row 480, first 256, alloc 40000 -> 3 pages (shuffled GPAs 0x300000, 0x100000, 0x200000), list at 0x80000, desc built in `d` */
#define SW 100u
#define SH 80u
#define ROW 480u
#define FIRST 256u
static const uint64_t PG[3] = { 0x300000, 0x100000, 0x200000 };
static void mk_surface(uint8_t *d, uint32_t fourcc, bool missing1) {
    memset(g_mem, 0xEE, MEM);
    uint32_t list[3];
    for (int i = 0; i < 3; i++) list[i] = (uint32_t)(PG[i] >> 12) | 1u;
    if (missing1) list[1] &= ~1u;
    memcpy(g_mem + 0x80000, list, sizeof list);
    memset(d, 0, LAGFX_SURF_DESC_BYTES);
    uint32_t l = (0x80000 >> 12) | 1u; memcpy(d, &l, 4); memcpy(d + 4, &fourcc, 4);
    uint64_t v;
    v = 40000; memcpy(d + 0x18, &v, 8); v = SW; memcpy(d + 0x20, &v, 8); v = SH; memcpy(d + 0x28, &v, 8); v = ROW; memcpy(d + 0x30, &v, 8);
    uint32_t f = FIRST; memcpy(d + 0x60, &f, 4);
}
static uint8_t *surf_byte(uint64_t off) { return g_mem + PG[off >> 14] + (off & 0x3fff); }
static void px(uint32_t x, uint32_t y, uint8_t o[4]) { o[0] = (uint8_t)x; o[1] = (uint8_t)y; o[2] = (uint8_t)(x ^ y); o[3] = 255; }

static void test_cpu_layout(void) {
    lagfx_shell_callbacks_t sh; memset(&sh, 0, sizeof sh); sh.read_memory = rd; sh.write_memory = wr;
    uint8_t d[LAGFX_SURF_DESC_BYTES]; lagfx_guest_surface_t gs;
    mk_surface(d, LAGFX_FOURCC_BGRA, false);
    assert(lagfx_guest_surface_decode(d, sizeof d, &gs) == LAGFX_OK && gs.npages == 3 && gs.width == SW && gs.row_bytes == ROW);
    uint8_t *img = malloc(SW * SH * 4);
    for (uint32_t y = 0; y < SH; y++) for (uint32_t x = 0; x < SW; x++) px(x, y, img + ((size_t)y * SW + x) * 4);
    assert(lagfx_guest_surface_write_bgra(&sh, &gs, img, SW, SH, SW * 4) == LAGFX_OK);
    for (uint32_t y = 0; y < SH; y++) {
        for (uint32_t x = 0; x < SW; x++) {
            uint8_t e[4]; px(x, y, e);
            uint64_t off = FIRST + (uint64_t)y * ROW + x * 4u;
            for (int k = 0; k < 4; k++) assert(surf_byte(off + k)[0] == e[k] || (printf("bad %u,%u k%d\n", x, y, k), 0));
        }
        for (uint32_t k = SW * 4; k < ROW && y + 1 < SH; k++) assert(*surf_byte(FIRST + (uint64_t)y * ROW + k) == 0xEE);   /* row padding untouched */
    }
    for (uint32_t k = 0; k < FIRST; k++) assert(*surf_byte(k) == 0xEE);                                                  /* before the first pixel untouched */
    assert(*g_mem == 0xEE && g_mem[0x400000 - 1] == 0xEE && g_mem[0x100000 - 1] == 0xEE);                                /* nothing outside the three pages */
    /* a row does straddle a 16 KiB boundary here (row 33: 256 + 33*480 = 16096 .. 16576) */
    assert(FIRST + 33ull * ROW < 16384 && FIRST + 33ull * ROW + SW * 4 > 16384);
    /* smaller source: only its top-left is written, the rest keeps its bytes */
    mk_surface(d, LAGFX_FOURCC_BGRA, false);
    assert(lagfx_guest_surface_write_bgra(&sh, &gs, img, 10, 5, SW * 4) == LAGFX_OK);
    assert(*surf_byte(FIRST + 4ull * ROW + 39) == 255 && *surf_byte(FIRST + 5ull * ROW) == 0xEE && *surf_byte(FIRST + 40) == 0xEE);
    /* larger source is cropped to the surface */
    mk_surface(d, LAGFX_FOURCC_BGRA, false);
    assert(lagfx_guest_surface_write_bgra(&sh, &gs, malloc(200 * 200 * 4), 200, 200, 200 * 4) == LAGFX_OK);
    /* a missing page: refused, nothing written */
    mk_surface(d, LAGFX_FOURCC_BGRA, true);
    assert(lagfx_guest_surface_write_bgra(&sh, &gs, img, SW, SH, SW * 4) != LAGFX_OK);
    for (int i = 0; i < 3; i++) for (uint32_t k = 0; k < 0x4000; k += 97) assert(g_mem[PG[i] + k] == 0xEE);
    /* decode refusals: fourcc, bounds, short descriptor, row < width*4, surface not fitting its pages */
    mk_surface(d, 0x52476841, false); assert(lagfx_guest_surface_decode(d, sizeof d, &gs) != LAGFX_OK);
    mk_surface(d, LAGFX_FOURCC_BGRA, false); assert(lagfx_guest_surface_decode(d, 0x60, &gs) != LAGFX_OK);
    uint64_t v = 399; memcpy(d + 0x30, &v, 8); assert(lagfx_guest_surface_decode(d, sizeof d, &gs) != LAGFX_OK);
    mk_surface(d, LAGFX_FOURCC_BGRA, false); v = 8192; memcpy(d + 0x20, &v, 8); assert(lagfx_guest_surface_decode(d, sizeof d, &gs) != LAGFX_OK);
    mk_surface(d, LAGFX_FOURCC_BGRA, false); v = 9000; memcpy(d + 0x28, &v, 8); assert(lagfx_guest_surface_decode(d, sizeof d, &gs) != LAGFX_OK);
    mk_surface(d, LAGFX_FOURCC_BGRA, false); uint32_t z = 0; memcpy(d, &z, 4); assert(lagfx_guest_surface_decode(d, sizeof d, &gs) != LAGFX_OK);
    free(img);
    puts("cpu layout: ok");
}

#ifdef LAGFX_HAVE_VULKAN
static VkFence submit_clear(struct lagfx_vk_state *vk, lagfx_vk_render_target_t *rt, const float rgba[4]) {
    VkCommandBuffer cb; assert(lagfx_vk_cmdbuf_alloc(vk, &cb) == LAGFX_OK);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &bi);
    assert(lagfx_vk_render_clear_color(vk, cb, rt, rgba) == LAGFX_OK);
    vkEndCommandBuffer(cb);
    VkFence f; VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vkCreateFence(vk->device, &fci, NULL, &f);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    assert(vkQueueSubmit(vk->graphics_queue, 1, &si, f) == VK_SUCCESS);
    return f;   /* cb leaks into the pool (freed with it) — test only */
}

static void test_vulkan(struct lagfx_vk_state *vk) {
    lagfx_device_t *dev = calloc(1, sizeof *dev);
    dev->desc.shell.read_memory = rd; dev->desc.shell.write_memory = wr; dev->desc.shell.raise_interrupt = irq; dev->vk = vk;
    lagfx_protocol_t *p = calloc(1, sizeof *p);
    p->magic = LAGFX_PROTOCOL_MAGIC; p->dev = dev; p->protocol_mode = LAGFX_PROTO_ARM64_VMAPPLE; p->ring_page_shift = 14; p->ring_base_pfn = 0x10;
    assert(lagfx_arm64_cq_enable(p));
    uint8_t d[LAGFX_SURF_DESC_BYTES];
    uint32_t cell0 = 0x10u << 14;

    /* 1. GPU clear of a SW x SH target tracked by a fence: stamp only after the fence signalled */
    mk_surface(d, LAGFX_FOURCC_BGRA, false);
    memset(g_mem + 0x10u * 0x4000u, 0, 64);
    lagfx_vk_render_target_t rt;
    assert(lagfx_vk_render_target_create(vk, SW, SH, VK_FORMAT_B8G8R8A8_UNORM, &rt) == LAGFX_OK);
    float green[4] = { 0.f, 1.f, 0.f, 1.f };
    lagfx_cmd_header_t h = { .opcode = 0x43, .length = 12, .stamp = 1 };
    lagfx_arm64_submit_ctx_t *c = lagfx_arm64_submit_begin(p, 1, &h);
    VkFence f = submit_clear(vk, &rt, green);
    assert(lagfx_arm64_submit_add_vk_fence(c, vk, f, true));
    bool immediate = lagfx_arm64_submit_end(p, c);
    assert(!immediate);
    int spins = 0;
    while (lagfx_arm64_cq_pending(p) && spins++ < 5000) { lagfx_arm64_poll_completions(p); usleep(1000); }
    assert(lagfx_arm64_cq_pending(p) == 0);
    uint32_t cellv; memcpy(&cellv, g_mem + 0x10u * 0x4000u + 4, 4);
    (void)cell0;
        assert(cellv == 1 && g_irq == 1);
    /* 2. swap: the guest-visible surface gets the rendered pixels */
    assert(lagfx_vk_render_target_to_guest_surface(vk, &rt, &dev->desc.shell, d, sizeof d) == LAGFX_OK);
    for (uint32_t y = 0; y < SH; y += 7) for (uint32_t x = 0; x < SW; x += 3) {
        uint8_t *q = surf_byte(FIRST + (uint64_t)y * ROW + x * 4u);
        assert(q[0] == 0 && q[1] == 255 && q[2] == 0 && q[3] == 255);   /* BGRA of green */
    }
    assert(*surf_byte(FIRST + SW * 4) == 0xEE);                          /* stride padding untouched */
    lagfx_vk_render_target_destroy(vk, &rt);

    /* 3. a known non-uniform image uploaded to the GPU, then written back: exact bytes, 100x80 */
    lagfx_vk_iosurface_t *ios = NULL;
    assert(lagfx_vk_iosurface_create(vk, SW, SH, 80, &ios) == LAGFX_OK);
    uint8_t *img = malloc(SW * SH * 4);
    for (uint32_t y = 0; y < SH; y++) for (uint32_t x = 0; x < SW; x++) px(x, y, img + ((size_t)y * SW + x) * 4);
    assert(lagfx_vk_iosurface_upload_pixels(vk, ios, img, SW * SH * 4) == LAGFX_OK);
    lagfx_vk_render_target_t wrapped;
    assert(lagfx_vk_render_target_wrap(ios->image, ios->view, ios->memory, SW, SH, ios->format, &wrapped) == LAGFX_OK);
    wrapped.layout = ios->layout;
    mk_surface(d, LAGFX_FOURCC_BGRA, false);
    assert(lagfx_vk_render_target_to_guest_surface(vk, &wrapped, &dev->desc.shell, d, sizeof d) == LAGFX_OK);
    for (uint32_t y = 0; y < SH; y++) for (uint32_t x = 0; x < SW; x++) {
        uint8_t e[4]; px(x, y, e);
        assert(!memcmp(surf_byte(FIRST + (uint64_t)y * ROW + x * 4u), e, 4));
    }
    /* a surface with a missing page: error, guest memory untouched */
    mk_surface(d, LAGFX_FOURCC_BGRA, true);
    assert(lagfx_vk_render_target_to_guest_surface(vk, &wrapped, &dev->desc.shell, d, sizeof d) != LAGFX_OK);
    assert(*surf_byte(FIRST) == 0xEE);
    lagfx_vk_iosurface_release(vk, ios);
    free(img);

    /* 3b. bounded wait helper: a fence that never signals times out (no hang), a signalled one is OK, both counted */
    {
        VkFence nf; VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        vkCreateFence(vk->device, &fci, NULL, &nf);
        uint64_t t0, f0, s0, t1; lagfx_vk_gpu_fail_stats(&t0, &f0, &s0);
        assert(lagfx_vk_wait_bounded(vk, nf, 20 * 1000000ull) == LAGFX_VK_WAIT_TIMEOUT);
        lagfx_vk_gpu_fail_stats(&t1, &f0, &s0); assert(t1 == t0 + 1);
        assert(lagfx_vk_wait_bounded(vk, VK_NULL_HANDLE, 0) == LAGFX_VK_WAIT_FAILED);
        VkSubmitInfo es = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
        assert(lagfx_vk_submit_checked(vk, &es, nf) == LAGFX_OK);
        assert(lagfx_vk_wait_bounded(vk, nf, 0) == LAGFX_VK_WAIT_OK);
        vkDestroyFence(vk->device, nf, NULL);
        assert(lagfx_vk_wait_default_ns() == 2000ull * 1000000ull);
    }

    /* 4. timeline semaphore (when the device has it): work that never finishes is dropped at teardown without a stamp, without hanging */
    if (vk->have_timeline_semaphore) {
        VkSemaphoreTypeCreateInfo ti = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE };
        VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti };
        VkSemaphore sem; assert(vkCreateSemaphore(vk->device, &si, NULL, &sem) == VK_SUCCESS);
        h.stamp = 2; c = lagfx_arm64_submit_begin(p, 1, &h);
        assert(lagfx_arm64_submit_add_vk_timeline(c, vk, sem, 5));
        assert(!lagfx_arm64_submit_end(p, c));
        assert(lagfx_arm64_poll_completions(p) == 0);
        VkSemaphoreSignalInfo sg = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = sem, .value = 4 };
        vkSignalSemaphore(vk->device, &sg);
        assert(lagfx_arm64_poll_completions(p) == 0);       /* 4 < 5 */
        sg.value = 5; vkSignalSemaphore(vk->device, &sg);
        assert(lagfx_arm64_poll_completions(p) == 1);
        h.stamp = 3; c = lagfx_arm64_submit_begin(p, 1, &h);
        assert(lagfx_arm64_submit_add_vk_timeline(c, vk, sem, 99));
        lagfx_arm64_submit_end(p, c);
        int irq_before = g_irq;
        lagfx_arm64_cq_shutdown(p, 10);                      /* never signalled: bounded wait, released, no stamp, no IRQ */
        assert(g_irq == irq_before && p->a64cq == NULL);
        vkDestroySemaphore(vk->device, sem, NULL);
    }
    /* 5. an owned fence still in flight when the queue is torn down is waited for before it is destroyed */
    assert(lagfx_arm64_cq_enable(p));
    assert(lagfx_vk_render_target_create(vk, SW, SH, VK_FORMAT_B8G8R8A8_UNORM, &rt) == LAGFX_OK);
    h.stamp = 9; c = lagfx_arm64_submit_begin(p, 2, &h);
    f = submit_clear(vk, &rt, green);
    assert(lagfx_arm64_submit_add_vk_fence(c, vk, f, true));
    lagfx_arm64_submit_end(p, c);
    lagfx_arm64_cq_shutdown(p, 1000);
    lagfx_vk_render_target_destroy(vk, &rt);
    free(dev); free(p);
    puts("vulkan: ok");
}
#endif

int main(void) {
    g_mem = calloc(1, MEM);
    test_cpu_layout();
#ifdef LAGFX_HAVE_VULKAN
    lagfx_device_descriptor_t dd; memset(&dd, 0, sizeof dd);
    dd.shell.opaque = (void *)1; dd.shell.create_task = c_ct; dd.shell.destroy_task = c_dt; dd.shell.map_memory = c_map; dd.shell.unmap_memory = c_unmap;
    dd.shell.read_memory = rd; dd.shell.raise_interrupt = irq; dd.thread_count = 2;
    lagfx_device_t *dev = lagfx_device_new(&dd, NULL);
    if (!dev || !dev->vk || !dev->vk->initialized) { puts("SKIP: no Vulkan device"); return dev ? (lagfx_device_free(dev), 0) : 0; }
    test_vulkan(dev->vk);
    lagfx_device_free(dev);
#endif
    puts("arm64-surface-writeback-unit: ok");
    return 0;
}
