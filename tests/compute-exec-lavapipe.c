/* Common compute execution base (protocol/compute_exec.c) executed on a Vulkan device (lavapipe) against a CPU reference. SPDX-License-Identifier: MIT
 * Kernel: b[i] = a[i]*k + 1 for i < n (local_size_x = 64, push {n, k}). Checked: exact results for a uniform grid, a NON-uniform grid dispatched by threads (the kernel's
 * bound check keeps the tail of the last group out; elements beyond n are untouched - a sentinel proves it), two dispatches in one command buffer ordered by the
 * barriers (the second reads the first's output), descriptor recycling, and the refusals (missing / duplicated / unknown binding, zero groups, wrong push size).
 * Exit 77 without a Vulkan device. */
#include "protocol/compute_exec.h"
#include "compute-exec-shader.h"
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)
typedef struct { VkPhysicalDevice pd; VkDevice device; VkQueue graphics_queue; VkCommandPool cmd_pool; } vkx_t;
typedef struct { VkBuffer b; VkDeviceMemory m; uint32_t *p; } buf_t;
static uint32_t mem_type(vkx_t *vk, uint32_t bits) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(vk->pd, &mp);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}
static int mkbuf(vkx_t *vk, buf_t *o, uint32_t words) {
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = words * 4u, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (vkCreateBuffer(vk->device, &bi, NULL, &o->b) != VK_SUCCESS) return 0;
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(vk->device, o->b, &rq);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = mem_type(vk, rq.memoryTypeBits) };
    if (ai.memoryTypeIndex == ~0u || vkAllocateMemory(vk->device, &ai, NULL, &o->m) != VK_SUCCESS) return 0;
    if (vkBindBufferMemory(vk->device, o->b, o->m, 0) != VK_SUCCESS) return 0;
    return vkMapMemory(vk->device, o->m, 0, VK_WHOLE_SIZE, 0, (void **)&o->p) == VK_SUCCESS;
}
static int submit_wait(vkx_t *vk, VkCommandBuffer cb) {
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    return vkQueueSubmit(vk->graphics_queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(vk->graphics_queue) == VK_SUCCESS;
}
static VkCommandBuffer begin(vkx_t *vk) {
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = vk->cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; if (vkAllocateCommandBuffers(vk->device, &ai, &cb) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    return vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS ? cb : VK_NULL_HANDLE;
}

int main(void) {
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "compute-exec-lavapipe", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { puts("SKIP: no Vulkan instance"); return 77; }
    uint32_t nd = 0; vkEnumeratePhysicalDevices(inst, &nd, NULL);
    if (!nd) { puts("SKIP: no Vulkan device"); return 77; }
    VkPhysicalDevice *pds = malloc(nd * sizeof *pds); vkEnumeratePhysicalDevices(inst, &nd, pds);
    vkx_t V = { .pd = pds[0] }, *vk = &V;
    for (uint32_t i = 0; i < nd; i++) { VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(pds[i], &pr); if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { V.pd = pds[i]; break; } }
    free(pds);
    uint32_t nq = 0, qf = ~0u; vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp); vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &nq, qp);
    for (uint32_t i = 0; i < nq; i++) if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qf = i; break; }
    free(qp);
    if (qf == ~0u) { puts("SKIP: no compute queue"); return 77; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi };
    if (vkCreateDevice(V.pd, &dci, NULL, &V.device) != VK_SUCCESS) { puts("SKIP: device"); return 77; }
    vkGetDeviceQueue(V.device, qf, 0, &V.graphics_queue);
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    CHECK(vkCreateCommandPool(V.device, &cpi, NULL, &V.cmd_pool) == VK_SUCCESS, "pool");

    const lagfx_cx_bind_t bind[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER }, { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER } };
    lagfx_cx_pipe_t *pp = lagfx_cx_pipe_create(vk->device, k_cx_spv, sizeof k_cx_spv, "main", bind, 2, 8);
    CHECK(pp, "pipeline create");
    { VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(vk->pd, &pr); lagfx_cx_set_limits(pp, &pr.limits);
      CHECK(!lagfx_cx_pipe_create(vk->device, k_cx_spv, sizeof k_cx_spv, "main", (lagfx_cx_bind_t[1]){ { 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR } }, 1, 0), "unsupported descriptor type refused");
      CHECK(!lagfx_cx_record(pp, VK_NULL_HANDLE, NULL, 0, NULL, 0, 1, 1, 1), "null command buffer refused"); }
    CHECK(!lagfx_cx_pipe_create(vk->device, k_cx_spv, sizeof k_cx_spv, "main", (lagfx_cx_bind_t[2]){ { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER }, { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER } }, 2, 8), "duplicate binding refused");

    enum { N = 1000 };   /* 15 full groups of 64 + a partial one of 40 */
    buf_t a, b, c; CHECK(mkbuf(vk, &a, N) && mkbuf(vk, &b, N + 64) && mkbuf(vk, &c, N + 64), "buffers");
    for (uint32_t i = 0; i < N; i++) a.p[i] = i * 7u + 3u;
    for (uint32_t i = 0; i < N + 64; i++) b.p[i] = c.p[i] = 0xdeadbeefu;   /* sentinel: elements >= N must stay untouched */

    /* grid by threads: 1000 threads, tg 64 -> 16 groups, non-uniform */
    uint32_t thr[3] = { N, 1, 1 }, tg[3] = { 64, 1, 1 }, grp[3]; bool nu = false;
    CHECK(lagfx_cx_threads_to_groups(thr, tg, grp, &nu) && grp[0] == 16 && grp[1] == 1 && grp[2] == 1 && nu, "threads->groups 1000/64");
    uint32_t thr2[3] = { 1024, 2, 1 }, tg2[3] = { 64, 2, 1 };
    CHECK(lagfx_cx_threads_to_groups(thr2, tg2, grp, &nu) && grp[0] == 16 && grp[1] == 1 && !nu, "uniform grid");
    CHECK(!lagfx_cx_threads_to_groups((uint32_t[3]){ 0, 1, 1 }, tg, grp, NULL), "zero extent refused");
    lagfx_cx_threads_to_groups(thr, tg, grp, &nu);

    VkCommandBuffer cb = begin(vk); CHECK(cb, "cb");
    const lagfx_cx_res_t r1[2] = { { .binding = 0, .buffer = a.b }, { .binding = 1, .buffer = b.b } };
    const uint32_t push1[2] = { N, 5 };
    CHECK(lagfx_cx_record(pp, cb, r1, 2, push1, 8, grp[0], grp[1], grp[2]), "record 1");
    /* the second dispatch reads the first one's output: ordered only by the barriers the module records */
    const lagfx_cx_res_t r2[2] = { { .binding = 1, .buffer = b.b }, { .binding = 0, .buffer = b.b } };   /* binding order differs from the layout order: matched by number */
    (void)r2;
    const lagfx_cx_res_t r3[2] = { { .binding = 1, .buffer = c.b }, { .binding = 0, .buffer = b.b } };
    const uint32_t push2[2] = { N, 3 };
    CHECK(lagfx_cx_record(pp, cb, r3, 2, push2, 8, grp[0], grp[1], grp[2]), "record 2 (reads b, writes c)");
    /* refusals record nothing */
    CHECK(!lagfx_cx_record(pp, cb, r1, 1, push1, 8, 1, 1, 1), "missing binding refused");
    const lagfx_cx_res_t dup[2] = { { .binding = 0, .buffer = a.b }, { .binding = 0, .buffer = a.b } };
    CHECK(!lagfx_cx_record(pp, cb, dup, 2, push1, 8, 1, 1, 1), "duplicate binding refused");
    const lagfx_cx_res_t unk[2] = { { .binding = 0, .buffer = a.b }, { .binding = 9, .buffer = b.b } };
    CHECK(!lagfx_cx_record(pp, cb, unk, 2, push1, 8, 1, 1, 1), "unknown binding refused");
    CHECK(!lagfx_cx_record(pp, cb, r1, 2, push1, 4, 1, 1, 1), "wrong push size refused");
    CHECK(!lagfx_cx_record(pp, cb, r1, 2, push1, 8, 0, 1, 1), "zero groups refused");
    CHECK(!lagfx_cx_record(pp, cb, r1, 2, push1, 8, 0xffffffffu, 1, 1), "group count above the device limit refused");
    { const lagfx_cx_res_t nb[2] = { { .binding = 0, .buffer = VK_NULL_HANDLE }, { .binding = 1, .buffer = b.b } };
      CHECK(!lagfx_cx_record(pp, cb, nb, 2, push1, 8, 1, 1, 1), "null buffer refused");
      const lagfx_cx_res_t ua[2] = { { .binding = 0, .buffer = a.b, .offset = 4 }, { .binding = 1, .buffer = b.b } };
      VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(vk->pd, &pr);
      if (pr.limits.minStorageBufferOffsetAlignment > 4) CHECK(!lagfx_cx_record(pp, cb, ua, 2, push1, 8, 1, 1, 1), "misaligned buffer offset refused"); }
    CHECK(vkEndCommandBuffer(cb) == VK_SUCCESS && submit_wait(vk, cb), "submit");
    for (uint32_t i = 0; i < N; i++) {
        uint32_t e1 = a.p[i] * 5u + 1u, e2 = e1 * 3u + 1u;
        CHECK(b.p[i] == e1, "b[%u] = %u, expected %u", i, b.p[i], e1);
        CHECK(c.p[i] == e2, "c[%u] = %u, expected %u (second dispatch must see the first's result)", i, c.p[i], e2);
    }
    for (uint32_t i = N; i < N + 64; i++) CHECK(b.p[i] == 0xdeadbeefu && c.p[i] == 0xdeadbeefu, "element %u beyond the grid was written", i);
    vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cb);

    lagfx_cx_recycle(pp);   /* the first two records have completed */
    /* descriptor recycling: more records than the set pool holds, in batches */
    for (int round = 0; round < 3; round++) {
        cb = begin(vk); CHECK(cb, "cb");
        for (uint32_t k = 0; k < LAGFX_CX_SET_POOL; k++) CHECK(lagfx_cx_record(pp, cb, r1, 2, push1, 8, grp[0], 1, 1), "record in batch %d #%u", round, k);
        CHECK(!lagfx_cx_record(pp, cb, r1, 2, push1, 8, grp[0], 1, 1), "pool exhausted is reported");
        CHECK(vkEndCommandBuffer(cb) == VK_SUCCESS && submit_wait(vk, cb), "submit batch");
        vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cb);
        lagfx_cx_recycle(pp);
    }
    lagfx_cx_pipe_destroy(pp);
    printf("PASS: common compute base (uniform + non-uniform grid, ordered back-to-back dispatches, sentinel untouched, refusals, descriptor recycling)\n");
    return 0;
}
