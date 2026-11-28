/* '422f' bi-planar surface mirror (src/vulkan/planar422.c) on a Vulkan device (lavapipe): layout checks, padding-preserving plane transfers, plane independence, dirty-gated
 * write-back, GPU fills, same-size and re-sized remaps, failed remap keeps the old state. The guest's descriptor is NOT involved: the plane geometry is the input.
 * SPDX-License-Identifier: MIT. Exit 77 (skip) without a Vulkan device. */
#include "vulkan/planar422.h"
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t *b; uint64_t n; } mem_t;
static bool mem_rw(void *c, uint64_t off, uint64_t len, void *buf, bool to_guest) {
    mem_t *m = c;
    if (off > m->n || len > m->n - off) return false;
    if (to_guest) memcpy(m->b + off, buf, (size_t)len); else memcpy(buf, m->b + off, (size_t)len);
    return true;
}
static uint32_t g_seed = 1;
static uint8_t rnd(void) { g_seed = g_seed * 1664525u + 1013904223u; return (uint8_t)(g_seed >> 24); }

/* a guest surface: planes filled with random payload (0 .. 249), everything else `fill` */
static mem_t mk(const lagfx_p422_layout_t *l, uint8_t fill, bool random_payload) {
    mem_t m = { malloc((size_t)l->alloc), l->alloc }; assert(m.b);
    memset(m.b, fill, (size_t)l->alloc);
    if (random_payload)
        for (uint32_t pl = 0; pl < 2; pl++) {
            uint64_t off = pl ? l->c_off : l->y_off, pitch = pl ? l->c_pitch : l->y_pitch, rb = lagfx_p422_row_bytes(l, pl);
            for (uint32_t y = 0; y < l->height; y++) for (uint64_t x = 0; x < rb; x++) m.b[off + y * pitch + x] = rnd() % 250u;
        }
    return m;
}
/* payload bytes of plane `pl` equal between two surfaces */
static bool plane_eq(const lagfx_p422_layout_t *l, uint32_t pl, const mem_t *a, const mem_t *b) {
    uint64_t off = pl ? l->c_off : l->y_off, pitch = pl ? l->c_pitch : l->y_pitch, rb = lagfx_p422_row_bytes(l, pl);
    for (uint32_t y = 0; y < l->height; y++) if (memcmp(a->b + off + y * pitch, b->b + off + y * pitch, (size_t)rb)) return false;
    return true;
}
/* every byte that is NOT payload of either plane still equals `v` */
static bool rest_is(const lagfx_p422_layout_t *l, const mem_t *m, uint8_t v) {
    uint8_t *own = calloc(1, (size_t)l->alloc); assert(own);
    for (uint32_t pl = 0; pl < 2; pl++) {
        uint64_t off = pl ? l->c_off : l->y_off, pitch = pl ? l->c_pitch : l->y_pitch, rb = lagfx_p422_row_bytes(l, pl);
        for (uint32_t y = 0; y < l->height; y++) memset(own + off + y * pitch, 1, (size_t)rb);
    }
    bool ok = true; for (uint64_t i = 0; i < l->alloc && ok; i++) if (!own[i] && m->b[i] != v) ok = false;
    free(own); return ok;
}
static bool plane_all(const lagfx_p422_layout_t *l, uint32_t pl, const mem_t *m, uint8_t lo, uint8_t hi, int odd_only) {
    uint64_t off = pl ? l->c_off : l->y_off, pitch = pl ? l->c_pitch : l->y_pitch, rb = lagfx_p422_row_bytes(l, pl);
    for (uint32_t y = 0; y < l->height; y++) for (uint64_t x = 0; x < rb; x++) {
        uint8_t v = m->b[off + y * pitch + x]; uint8_t a = lo, b = hi;
        if (odd_only >= 0) { if ((x & 1) == 0) { a = (uint8_t)odd_only, b = (uint8_t)odd_only; } }
        if (v < a || v > b) return false;
    }
    return true;
}

static void test_layout(void) {
    lagfx_p422_layout_t l = { .width = 7, .height = 5, .y_off = 16, .y_pitch = 32, .c_off = 16 + 32 * 5 + 24, .c_pitch = 24, .alloc = 4096 };
    char why[96];
    assert(lagfx_p422_layout_check(&l, why, sizeof why) == LAGFX_P422_OK);
    assert(lagfx_p422_row_bytes(&l, 0) == 7 && lagfx_p422_row_bytes(&l, 1) == 8);   /* odd width: 4 chroma samples, 8 bytes */
    lagfx_p422_layout_t b = l; b.width = 0;            assert(lagfx_p422_layout_check(&b, why, sizeof why) == LAGFX_P422_BAD_SIZE);
    b = l; b.height = 9000;                              assert(lagfx_p422_layout_check(&b, why, sizeof why) == LAGFX_P422_BAD_SIZE);
    b = l; b.y_pitch = 6;                                assert(lagfx_p422_layout_check(&b, why, sizeof why) == LAGFX_P422_BAD_PITCH);
    b = l; b.c_pitch = 7;                                assert(lagfx_p422_layout_check(&b, why, sizeof why) == LAGFX_P422_BAD_PITCH);   /* 8 payload bytes needed */
    b = l; b.alloc = b.c_off + b.c_pitch * 4 + 7;        assert(lagfx_p422_layout_check(&b, why, sizeof why) == LAGFX_P422_BAD_BOUNDS);   /* one byte short */
    b = l; b.alloc = b.c_off + b.c_pitch * 4 + 8;        assert(lagfx_p422_layout_check(&b, why, sizeof why) == LAGFX_P422_OK);           /* exactly fits */
    b = l; b.c_off = 16 + 32 * 4 + 7 - 3;                assert(lagfx_p422_layout_check(&b, why, sizeof why) == LAGFX_P422_BAD_OVERLAP);   /* CbCr starts inside Y's last row payload */
    b = l; b.y_off = (uint64_t)1 << 63; b.y_pitch = (uint64_t)1 << 62; assert(lagfx_p422_layout_check(&b, why, sizeof why) == LAGFX_P422_BAD_BOUNDS);   /* overflow, not wrap */
    b = l; b.c_off = 0; b.y_off = 100;                   assert(lagfx_p422_layout_check(&b, why, sizeof why) == LAGFX_P422_BAD_OVERLAP);   /* CbCr first in memory but running into Y */
    puts("PASS: 422f layout checks (odd width chroma, pitch, exact fit, overflow, overlap both orders)");
}

static void test_xfer_pure(void) {
    lagfx_p422_layout_t l = { .width = 7, .height = 5, .y_off = 16, .y_pitch = 32, .c_off = 16 + 32 * 5 + 24, .c_pitch = 24, .alloc = 4096 };
    mem_t g = mk(&l, 0xAA, true), orig = mk(&l, 0xAA, false); memcpy(orig.b, g.b, (size_t)l.alloc);
    uint8_t tight[8 * 5];
    assert(lagfx_p422_xfer_plane(mem_rw, &g, &l, LAGFX_P422_CBCR, tight, false));
    for (uint32_t y = 0; y < 5; y++) assert(!memcmp(tight + y * 8, g.b + l.c_off + y * 24, 8));   /* tight rows = the payload */
    memset(tight, 0x11, sizeof tight);
    assert(lagfx_p422_xfer_plane(mem_rw, &g, &l, LAGFX_P422_CBCR, tight, true));
    assert(plane_all(&l, 1, &g, 0x11, 0x11, -1) && plane_eq(&l, 0, &g, &orig) && rest_is(&l, &g, 0xAA));   /* only CbCr payload changed: Y and every padding byte as before */
    mem_t small = { g.b, 100 };
    assert(!lagfx_p422_xfer_plane(mem_rw, &small, &l, LAGFX_P422_Y, tight, false));              /* the callback refuses -> false */
    free(g.b); free(orig.b);
    puts("PASS: plane transfer is row by row on payload bytes: padding and the other plane never touched, a failing callback fails the transfer");
}

#define VKC(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s:%d %s = %d\n", __FILE__, __LINE__, #x, (int)r_); exit(1); } } while (0)

int main(void) {
    test_layout();
    test_xfer_pure();
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst; if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { puts("SKIP: no Vulkan instance"); return 77; }
    uint32_t nd = 0; vkEnumeratePhysicalDevices(inst, &nd, NULL); if (!nd) { puts("SKIP: no Vulkan device"); return 77; }
    VkPhysicalDevice *pds = malloc(nd * sizeof *pds); vkEnumeratePhysicalDevices(inst, &nd, pds);
    VkPhysicalDevice pd = pds[0];
    for (uint32_t i = 0; i < nd; i++) { VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(pds[i], &pr); if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { pd = pds[i]; break; } }
    free(pds);
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties *qp = malloc(nq * sizeof *qp); vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    uint32_t qf = ~0u; for (uint32_t i = 0; i < nq; i++) if (qp[i].queueFlags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { qf = i; break; }
    free(qp); assert(qf != ~0u);
    float prio = 1.f; VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev; VKC(vkCreateDevice(pd, &dci, NULL, &dev)); VkQueue q; vkGetDeviceQueue(dev, qf, 0, &q);

    char why[160];
    lagfx_p422_layout_t l = { .width = 7, .height = 5, .y_off = 16, .y_pitch = 32, .c_off = 16 + 32 * 5 + 24, .c_pitch = 24, .alloc = 4096 };
    lagfx_p422_layout_t badl = l; badl.c_pitch = 3;
    assert(!lagfx_p422_create(pd, dev, q, qf, &badl, why, sizeof why) && why[0]);
    lagfx_p422_t *p = lagfx_p422_create(pd, dev, q, qf, &l, why, sizeof why);
    if (!p) { printf("SKIP: R8 / R8G8 planes unavailable on this device (%s)\n", why); return 77; }
    printf("INFO: planes created: Y storage=%d CbCr storage=%d\n", lagfx_p422_has_storage(p, 0), lagfx_p422_has_storage(p, 1));
    assert(lagfx_p422_image(p, 0) && lagfx_p422_view(p, 1) && lagfx_p422_dirty(p) == 0);

    /* (1) guest -> images -> guest round trip: payload equal, padding and the rest untouched */
    mem_t A = mk(&l, 0xAA, true), B = mk(&l, 0x55, false);
    assert(lagfx_p422_upload(p, mem_rw, &A, LAGFX_P422_PLANES_ALL));
    lagfx_p422_mark_gpu_written(p, LAGFX_P422_PLANES_ALL);
    assert(lagfx_p422_download(p, mem_rw, &B, LAGFX_P422_PLANES_ALL, false) == LAGFX_P422_PLANES_ALL && lagfx_p422_dirty(p) == 0);
    assert(plane_eq(&l, 0, &A, &B) && plane_eq(&l, 1, &A, &B) && rest_is(&l, &B, 0x55));
    puts("PASS: upload -> GPU images -> download reproduces both planes byte for byte (odd width 7: 4 chroma samples), padding / gaps untouched");

    /* (2) plane independence: re-upload Y only with new data; CbCr image keeps the earlier bytes */
    mem_t C = mk(&l, 0xAA, true), D = mk(&l, 0x55, false);
    assert(lagfx_p422_upload(p, mem_rw, &C, LAGFX_P422_PLANE_Y));
    assert(lagfx_p422_download(p, mem_rw, &D, LAGFX_P422_PLANES_ALL, true) == LAGFX_P422_PLANES_ALL);
    assert(plane_eq(&l, 0, &C, &D) && plane_eq(&l, 1, &A, &D) && !plane_eq(&l, 0, &A, &D));
    puts("PASS: uploading one plane leaves the other plane's image as it was");

    /* (3) dirty gating: clean planes are not written back */
    mem_t E = mk(&l, 0x55, false);
    assert(lagfx_p422_download(p, mem_rw, &E, LAGFX_P422_PLANES_ALL, false) == 0 && rest_is(&l, &E, 0x55) && plane_all(&l, 0, &E, 0x55, 0x55, -1) && plane_all(&l, 1, &E, 0x55, 0x55, -1));
    /* a GPU write to Y only: only Y goes back */
    float half[2] = { 0.5f, 0 };
    assert(lagfx_p422_fill(p, LAGFX_P422_Y, half) && lagfx_p422_dirty(p) == LAGFX_P422_PLANE_Y);
    mem_t F = mk(&l, 0x55, false);
    assert(lagfx_p422_download(p, mem_rw, &F, LAGFX_P422_PLANES_ALL, false) == LAGFX_P422_PLANE_Y);
    assert(plane_all(&l, 0, &F, 127, 128, -1) && plane_all(&l, 1, &F, 0x55, 0x55, -1) && rest_is(&l, &F, 0x55) && lagfx_p422_dirty(p) == 0);
    float cc[2] = { 0.0f, 1.0f };   /* Cb = 0, Cr = 1: interleaved bytes 0, 255, 0, 255 ... */
    assert(lagfx_p422_fill(p, LAGFX_P422_CBCR, cc) && lagfx_p422_dirty(p) == LAGFX_P422_PLANE_CBCR);
    mem_t G = mk(&l, 0x55, false);
    assert(lagfx_p422_download(p, mem_rw, &G, LAGFX_P422_PLANES_ALL, false) == LAGFX_P422_PLANE_CBCR);
    for (uint32_t y = 0; y < l.height; y++) for (uint32_t x = 0; x < 8; x++) assert(G.b[l.c_off + y * l.c_pitch + x] == ((x & 1) ? 255 : 0));
    assert(plane_all(&l, 0, &G, 0x55, 0x55, -1) && rest_is(&l, &G, 0x55));
    /* the guest's newer bytes replace what the GPU owed: upload clears dirty */
    assert(lagfx_p422_fill(p, LAGFX_P422_Y, half) && lagfx_p422_dirty(p) == LAGFX_P422_PLANE_Y);
    assert(lagfx_p422_upload(p, mem_rw, &A, LAGFX_P422_PLANE_Y) && lagfx_p422_dirty(p) == 0);
    puts("PASS: dirty-gated write-back (clean = nothing, Y dirty = Y only, CbCr dirty = CbCr interleave 0/255 only); upload clears the dirty flag");

    /* (4) same-size remap: the new offsets / pitches decide where the next write-back lands; images and contents are kept */
    lagfx_p422_layout_t l2 = { .width = 7, .height = 5, .y_off = 100, .y_pitch = 48, .c_off = 100 + 48 * 5 + 64, .c_pitch = 40, .alloc = 8192 };
    assert(lagfx_p422_remap(p, &l2, why, sizeof why));
    mem_t H = mk(&l2, 0x77, false);
    lagfx_p422_mark_gpu_written(p, LAGFX_P422_PLANES_ALL);
    assert(lagfx_p422_download(p, mem_rw, &H, LAGFX_P422_PLANES_ALL, false) == LAGFX_P422_PLANES_ALL);
    mem_t A2 = mk(&l2, 0x77, false);
    { uint8_t t[8 * 5]; assert(lagfx_p422_xfer_plane(mem_rw, &A, &l, LAGFX_P422_Y, t, false)); /* Y plane of A = what the images hold */
      for (uint32_t y = 0; y < 5; y++) assert(!memcmp(H.b + l2.y_off + y * l2.y_pitch, t + y * 7, 7)); }
    assert(rest_is(&l2, &H, 0x77));
    (void)A2;
    puts("PASS: same-size remap keeps the images and moves the write-back to the new offsets / pitches");

    /* (5) another size: images re-made, dirty cleared, round trip still exact */
    lagfx_p422_layout_t l3 = { .width = 16, .height = 8, .y_off = 0, .y_pitch = 16, .c_off = 16 * 8, .c_pitch = 16, .alloc = 16 * 8 * 2 };
    lagfx_p422_mark_gpu_written(p, LAGFX_P422_PLANES_ALL);
    assert(lagfx_p422_remap(p, &l3, why, sizeof why) && lagfx_p422_dirty(p) == 0 && lagfx_p422_layout(p)->width == 16);
    mem_t I = mk(&l3, 0, true), J = mk(&l3, 0x55, false);
    assert(lagfx_p422_upload(p, mem_rw, &I, LAGFX_P422_PLANES_ALL) && lagfx_p422_download(p, mem_rw, &J, LAGFX_P422_PLANES_ALL, true) == LAGFX_P422_PLANES_ALL);
    assert(plane_eq(&l3, 0, &I, &J) && plane_eq(&l3, 1, &I, &J));
    puts("PASS: a re-sized remap re-makes both images (16x8, tightly packed planes) and the round trip is exact");

    /* (6) a refused remap keeps the old state */
    lagfx_p422_layout_t bad2 = l3; bad2.y_pitch = 1;
    assert(!lagfx_p422_remap(p, &bad2, why, sizeof why) && why[0] && lagfx_p422_layout(p)->width == 16 && lagfx_p422_layout(p)->y_pitch == 16);
    mem_t K = mk(&l3, 0x55, false);
    assert(lagfx_p422_download(p, mem_rw, &K, LAGFX_P422_PLANES_ALL, true) == LAGFX_P422_PLANES_ALL && plane_eq(&l3, 0, &I, &K));
    puts("PASS: a refused remap leaves the layout and the images as they were");

    /* (7) a failing guest write: the plane stays dirty and is not reported done */
    lagfx_p422_mark_gpu_written(p, LAGFX_P422_PLANE_Y);
    mem_t tiny = { K.b, 8 };
    assert(lagfx_p422_download(p, mem_rw, &tiny, LAGFX_P422_PLANES_ALL, false) == 0 && lagfx_p422_dirty(p) == LAGFX_P422_PLANE_Y);
    puts("PASS: a failed write-back keeps the plane dirty (retried later), nothing reported written");

    free(A.b); free(B.b); free(C.b); free(D.b); free(E.b); free(F.b); free(G.b); free(H.b); free(A2.b); free(I.b); free(J.b); free(K.b);
    lagfx_p422_free(p);
    vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
    puts("PASS: planar422 unit");
    return 0;
}
