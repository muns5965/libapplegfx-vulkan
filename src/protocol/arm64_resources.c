/*
 * libapplegfx-vulkan — ARM64 exec: guest-memory-backed object mirror + Blit executor. See arm64_resources.h for the rules.
 * SPDX-License-Identifier: MIT
 */
#include "arm64_resources.h"
#include "arm64_exec_decode.h"
#include "display_diag.h"
#include "../device.h"
#include "../common/le.h"
#include "../common/log.h"
#include "../vulkan/command.h"
#include "../vulkan/devinfo.h"
#include "../vulkan/format_caps.h"
#include "../vulkan/render_target.h"
#include "../vulkan/surface_writeback.h"
#include "../translate/blit_encoder.h"
#include "../handlers/compute/blit_inner_ops.h"
#include "opcodes.h"

#include <stdlib.h>
#include <string.h>

bool lagfx_a64_res_enabled(void) {
    const char *e = getenv("LAGFX_ARM64_EXEC");
    return e && e[0] == '1' && e[1] == 0;
}

/* EXPERIMENT (default off): write a mapped display surface back to the guest after every exec that rendered into it (flag 2), not only on flag 4 / a
 * synchronize. Apple's GPU shares memory with the guest, so a rendered surface is visible without a copy; here nothing reaches the guest unless told. Whether the
 * real guest relies on this is unverified — hence the switch. */
static bool mapped_wb_on_render(void) {
    const char *e = getenv("LAGFX_ARM64_MAPPED_WRITEBACK");
    return e && e[0] == '1' && e[1] == 0;
}
/* EXPERIMENT (default off), =swap: a surface a flag-2 exec rendered into is only marked OWED; the write-back to the guest happens when a display command
 * arrives (lagfx_a64_res_display_swap), so many execs per frame cost one copy. Exclusive with the immediate mode above (=1), which stays for comparison. */
static bool mapped_wb_on_swap(void) {
    const char *e = getenv("LAGFX_ARM64_MAPPED_WRITEBACK");
    return e && !strcmp(e, "swap");
}

const char *lagfx_a64_texwhy_name(unsigned why) {
    static const char *const nm[TEXWHY_COUNT] = { "slot_unbound", "slot_range", "record_bad", "ref_zero", "no_table", "entry_read", "entry_empty", "not_texture", "unknown_type", "not_mirrored",
        "table_full", "c_payload", "c_format", "c_type", "c_msaa", "c_dims", "c_vulkan", "c_linear", "c_mapped_nodesc", "c_mapped_bad" };
    return why < TEXWHY_COUNT ? nm[why] : "?";
}

#ifndef LAGFX_HAVE_VULKAN
/* No Vulkan: nothing can be executed. */
lagfx_a64_rexec_t *lagfx_a64_res_begin(lagfx_protocol_t *p, uint32_t task, const uint8_t *res, uint32_t nres) { (void)p; (void)task; (void)res; (void)nres; return NULL; }
lagfx_a64_obj_t *lagfx_a64_res_object(lagfx_a64_rexec_t *x, uint32_t ref) { (void)x; (void)ref; return NULL; }
void lagfx_a64_res_blit_segment(lagfx_a64_rexec_t *x, const uint8_t *body, uint32_t len) { (void)x; (void)body; (void)len; }
lagfx_a64_status_t lagfx_a64_res_end(lagfx_a64_rexec_t *x) { (void)x; return LAGFX_A64_FAILED; }
lagfx_a64_status_t lagfx_a64_res_poll(lagfx_protocol_t *p, uint32_t w) { (void)p; (void)w; return LAGFX_A64_DONE; }
bool lagfx_a64_res_is_pending(lagfx_protocol_t *p) { (void)p; return false; }
uint32_t lagfx_a64_res_display_swap(lagfx_protocol_t *p) { (void)p; return 0; }
uint32_t lagfx_a64_res_display_note(lagfx_protocol_t *p, uint64_t mapping) { (void)p; (void)mapping; return 0; }
bool lagfx_a64_res_display_owner(lagfx_protocol_t *p, uint64_t mapping, uint32_t *task) { (void)p; (void)mapping; (void)task; return false; }
bool lagfx_a64_res_display_info(lagfx_protocol_t *p, uint64_t mapping, uint32_t *w, uint32_t *h, uint32_t *fourcc, uint32_t *mtl_format, int32_t *vk_format) { (void)p; (void)mapping; (void)w; (void)h; (void)fourcc; (void)mtl_format; (void)vk_format; return false; }
int lagfx_a64_blit_op(lagfx_protocol_t *p, uint32_t tag, const uint8_t *payload, size_t len) { (void)p; (void)tag; (void)payload; (void)len; return 0; }
uint32_t lagfx_a64_res_sync_to_guest(lagfx_protocol_t *p, uint32_t task, const uint32_t *refs, uint32_t n) { (void)p; (void)task; (void)refs; (void)n; return 0; }
bool lagfx_a64_resource_cmd(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) { (void)p; (void)hdr; return false; }
void lagfx_a64_res_forget(lagfx_protocol_t *p, uint32_t task, uint32_t ref) { (void)p; (void)task; (void)ref; }
void lagfx_a64_res_drop_task(lagfx_protocol_t *p, uint32_t task) { (void)p; (void)task; }
bool lagfx_a64_res_busy(lagfx_protocol_t *p) { (void)p; return false; }
void lagfx_a64_res_counts(lagfx_protocol_t *p, uint32_t *l, uint64_t *c, uint64_t *d) { (void)p; if (l) *l = 0; if (c) *c = 0; if (d) *d = 0; }
lagfx_a64_obj_t *lagfx_a64_res_lookup(lagfx_protocol_t *p, uint32_t task, uint32_t ref) { (void)p; (void)task; (void)ref; return NULL; }
bool lagfx_a64_res_diagnose(lagfx_protocol_t *p, uint32_t task, uint32_t ref, lagfx_a64_texdiag_t *o) { (void)p; (void)task; (void)ref; if (o) memset(o, 0, sizeof *o); return false; }
bool lagfx_a64_res_prepare_sampled(lagfx_a64_rexec_t *x, uint32_t ref) { (void)x; (void)ref; return false; }
void lagfx_a64_res_sync_linear(lagfx_a64_rexec_t *x) { (void)x; }
bool lagfx_a64_res_linear_fresh(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o) { (void)x; (void)o; return false; }
void lagfx_a64_res_buf_written(lagfx_a64_obj_t *b) { (void)b; }
static lagfx_a64_stats_t g_zero;
const lagfx_a64_stats_t *lagfx_a64_res_stats(lagfx_protocol_t *p) { (void)p; return &g_zero; }
void lagfx_a64_res_destroy(lagfx_protocol_t *p) { (void)p; }
#else

#define MAX_OBJS 4096u
#define MAX_TMP  64u

typedef struct { lagfx_protocol_t *p; lagfx_a64_stats_t st; lagfx_a64_obj_t **objs; uint32_t n, cap; lagfx_a64_rexec_t *cur;      /* recording */
    lagfx_a64_rexec_t *pending;  /* submitted, fence not signalled yet: its resources stay alive and untouched */
    lagfx_a64_obj_t **dead; uint32_t ndead, capdead;   /* replaced / forgotten / dropped while an exec was recording or in flight: freed after its fence signalled */
    bool quiet_kinds;            /* begin(): refs of the resource list that are not buffers/textures (pipelines...) are not 'bad objects' */
    struct lagfx_a64_msurf **ms; uint32_t nms;   /* shared surface images of type 11 mapped textures, newest last (not owning: an entry leaves when its last object is freed) */
    uint64_t sw_gen;   /* write-back generation of the parked swap copy */
    VkCommandBuffer sw_cb; VkFence sw_fence; struct lagfx_a64_msurf *sw_sel[256]; uint32_t sw_n;   /* a display-swap write-back whose GPU copy has not been seen finished: its images / staging buffers are in use */
    uint64_t down_gen_seq;       /* last write-back generation handed out: one per exec / sync / swap copy (a mapped surface's down_gen names the recording its staging bytes belong to) */
    uint64_t epoch;              /* bumped by every exec begin / sync: a mapped surface is uploaded / read back at most once per epoch */
    lagfx_a64_texwhy_t mk_why;   /* why the last obj_make returned NULL (TEXWHY_*; 0 = not a texture failure) */
    struct { uint32_t task, ref, entry[3]; uint32_t why; } fail[128]; uint32_t nfail;   /* memo of the newest failed creations (ring), keyed by (task, ref, entry): lagfx_a64_res_diagnose */
} mgr2_t;
int lagfx_a64_test_hook;   /* tests only: 1 = the next swap submit fails, 2 = the next swap fence check reports a timeout without looking, 3 = the next exec submit fails, 4 = the next mapped write-back finish finds the descriptor unreadable, 5 = the next exec fence wait (res_end) fails (device lost), 6 = the next res_poll fence check fails, 7 = the next exec fence wait (res_end) times out and stays timed out (hook becomes 10) until the test sets it back to 0 or to 6, 8 = the next sync_to_guest submit fails, 9 = the next sync_to_guest wait is not DONE. Plain global, default 0, set by tests only (never from env or protocol input) */
#define MRUN(p) ((p)->diag2.exec.run)
#define BUSY(m) ((m)->cur || (m)->pending || (m)->sw_fence)
#define MAX_MS 256u
#define OT(m) ((m)->p->diag2.exec.pipe)   /* object table counters */
#define MKFAIL(w) do { m->mk_why = (w); } while (0)

/* One guest display surface (mapping id) mirrored as ONE device image + a host-visible staging buffer in the guest's pixel layout (tight rows, w * h * 4 bytes).
 * Every mapped-texture object (any ref / task) naming the same mapping id and size holds a reference; host_valid / guest_stale are the surface's, not the object's. */
typedef struct lagfx_a64_msurf {
    mgr2_t *m;
    uint32_t mapping, w, h, refs, fourcc, bpp, mtl_fmt;   /* mtl_fmt: the Metal pixel format number of the guest description (diagnostics only) */
    VkFormat format; VkImageUsageFlags usage;
    VkImage img; VkDeviceMemory img_mem;
    VkBuffer stage; VkDeviceMemory stage_mem; uint8_t *stage_map;
    bool host_valid, guest_stale, down_pending, owed, id_set;
    uint64_t up_epoch, down_epoch, down_gen;   /* down_gen: the exec / sync / swap copy that recorded the pending image -> staging copy (0 = none) */
    uint64_t id_pl, id_alloc, id_row, id_hash; uint32_t id_first, id_np;   /* which guest pages the mirror was last taken from (the descriptor has no generation): a different page list = the guest re-mapped it */
} msurf_t;

static mgr2_t *mgr_get(lagfx_protocol_t *p, bool create) {
    if (!p->a64_res && create) { p->a64_res = calloc(1, sizeof(mgr2_t)); if (p->a64_res) ((mgr2_t *)p->a64_res)->p = p; }
    return p->a64_res;
}

static struct lagfx_vk_state *vk_of(lagfx_protocol_t *p) {
    lagfx_device_t *dev = p ? (lagfx_device_t *)p->dev : NULL;
    return dev && dev->vk && dev->vk->initialized && dev->vk->device != VK_NULL_HANDLE && dev->vk->cmd_pool != VK_NULL_HANDLE ? dev->vk : NULL;
}

/* ---- Vulkan helpers ---- */

static uint32_t mem_type(struct lagfx_vk_state *vk, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(vk->phys_device, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

/* host-visible, coherent, persistently mapped buffer (zeroed) */
static bool make_buf(struct lagfx_vk_state *vk, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *b, VkDeviceMemory *m, uint8_t **map) {
    *b = VK_NULL_HANDLE, *m = VK_NULL_HANDLE, *map = NULL;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (vkCreateBuffer(vk->device, &bi, NULL, b) != VK_SUCCESS) { *b = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements rq;
    vkGetBufferMemoryRequirements(vk->device, *b, &rq);
    uint32_t mt = mem_type(vk, rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    lagfx_vk_devinfo_note_staging(vk->phys_device, mt, rq.memoryTypeBits, size, (uint32_t)usage);   /* diagnostic only: logs the chosen type once per distinct index */
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = mt };
    void *mp = NULL;
    if (mt == UINT32_MAX || vkAllocateMemory(vk->device, &ai, NULL, m) != VK_SUCCESS) { *m = VK_NULL_HANDLE; goto fail_buf; }
    if (vkBindBufferMemory(vk->device, *b, *m, 0) != VK_SUCCESS || vkMapMemory(vk->device, *m, 0, VK_WHOLE_SIZE, 0, &mp) != VK_SUCCESS || !mp) goto fail_mem;
    memset(mp, 0, (size_t)size);
    *map = mp;
    return true;
fail_mem:
    vkFreeMemory(vk->device, *m, NULL);
fail_buf:
    vkDestroyBuffer(vk->device, *b, NULL);
    *b = VK_NULL_HANDLE, *m = VK_NULL_HANDLE;
    return false;
}

static void free_buf(struct lagfx_vk_state *vk, VkBuffer b, VkDeviceMemory m) {
    if (b) vkDestroyBuffer(vk->device, b, NULL);
    if (m) vkFreeMemory(vk->device, m, NULL);   /* freeing unmaps */
}

static uint64_t wait_ns(void) {
    const char *e = getenv("LAGFX_GPU_WAIT_MS");
    long ms = e ? strtol(e, NULL, 10) : 0;
    return (uint64_t)(ms > 0 ? ms : 2000) * 1000000ull;
}

/* Submit one command buffer with a fresh fence. Failure here is a real failure (nothing is in flight): the fence is destroyed. */
static bool submit_fence(struct lagfx_vk_state *vk, mgr2_t *m, VkCommandBuffer cb, VkFence *out) {
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    *out = VK_NULL_HANDLE;
    VkResult r = vkCreateFence(vk->device, &fi, NULL, out);
    if (r != VK_SUCCESS) { m->st.gpu_fail++; *out = VK_NULL_HANDLE; return false; }
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    r = vkQueueSubmit(vk->graphics_queue, 1, &si, *out);
    if (r != VK_SUCCESS) { m->st.gpu_fail++; vkDestroyFence(vk->device, *out, NULL); *out = VK_NULL_HANDLE; LAGFX_WARN("arm64 exec: vkQueueSubmit failed (%d)", (int)r); return false; }
    return true;
}

/* One-shot (object creation): submit + bounded wait. Returns LAGFX_A64_DONE only if the GPU finished; on PENDING/FAILED-after-submit *cb is set to NULL
 * and the fence is leaked, because the work may still be in flight (callers must then leak whatever the command buffer touches). Wait timeout: LAGFX_GPU_WAIT_MS. */
static lagfx_a64_status_t submit_wait(struct lagfx_vk_state *vk, mgr2_t *m, VkCommandBuffer *cb) {
    VkFence f;
    if (!submit_fence(vk, m, *cb, &f)) return LAGFX_A64_FAILED;
    VkResult r = vkWaitForFences(vk->device, 1, &f, VK_TRUE, wait_ns());
    if (r == VK_SUCCESS) { vkDestroyFence(vk->device, f, NULL); return LAGFX_A64_DONE; }
    *cb = VK_NULL_HANDLE;
    if (r == VK_TIMEOUT) { m->st.gpu_timeout++; return LAGFX_A64_PENDING; }
    m->st.gpu_fail++;
    LAGFX_WARN("arm64 exec: vkWaitForFences failed (%d)", (int)r);
    return LAGFX_A64_FAILED;
}

static bool cb_begin(struct lagfx_vk_state *vk, VkCommandBuffer *cb) {
    if (lagfx_vk_cmdbuf_alloc(vk, cb) != LAGFX_OK) return false;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (vkBeginCommandBuffer(*cb, &bi) != VK_SUCCESS) { lagfx_vk_cmdbuf_free(vk, *cb); *cb = VK_NULL_HANDLE; return false; }
    return true;
}

static void barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

/* After a copy whose destination the HOST reads once the fence has signalled: the transfer writes become available to / visible for host reads
 * (srcStage TRANSFER / TRANSFER_WRITE -> dstStage HOST / HOST_READ, the docs.vulkan.org "CPU read back" pattern). A fence wait alone is an execution dependency only.
 * Recorded in addition to barrier(), never instead of it. */
static void host_barrier(mgr2_t *m, VkCommandBuffer cb) {
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    if (m && m->p) MRUN(m->p).wb_host_barriers_recorded++;
}

/* ---- Metal pixel formats we mirror (uncompressed colour; src/vulkan/iosurface.c's mapper only knows 70/80 and falls back to BGRA8, so it cannot be used) ---- */

static const struct { uint32_t mtl; VkFormat vk; uint32_t bpp; } k_fmt[] = {
    {1, VK_FORMAT_R8_UNORM, 1}, {10, VK_FORMAT_R8_UNORM, 1}, {11, VK_FORMAT_R8_SRGB, 1}, {12, VK_FORMAT_R8_SNORM, 1}, {13, VK_FORMAT_R8_UINT, 1}, {14, VK_FORMAT_R8_SINT, 1},
    {20, VK_FORMAT_R16_UNORM, 2}, {22, VK_FORMAT_R16_SNORM, 2}, {23, VK_FORMAT_R16_UINT, 2}, {24, VK_FORMAT_R16_SINT, 2}, {25, VK_FORMAT_R16_SFLOAT, 2},
    {30, VK_FORMAT_R8G8_UNORM, 2}, {31, VK_FORMAT_R8G8_SRGB, 2}, {32, VK_FORMAT_R8G8_SNORM, 2}, {33, VK_FORMAT_R8G8_UINT, 2}, {34, VK_FORMAT_R8G8_SINT, 2},
    {53, VK_FORMAT_R32_UINT, 4}, {54, VK_FORMAT_R32_SINT, 4}, {55, VK_FORMAT_R32_SFLOAT, 4},
    {60, VK_FORMAT_R16G16_UNORM, 4}, {62, VK_FORMAT_R16G16_SNORM, 4}, {63, VK_FORMAT_R16G16_UINT, 4}, {64, VK_FORMAT_R16G16_SINT, 4}, {65, VK_FORMAT_R16G16_SFLOAT, 4},
    {70, VK_FORMAT_R8G8B8A8_UNORM, 4}, {71, VK_FORMAT_R8G8B8A8_SRGB, 4}, {72, VK_FORMAT_R8G8B8A8_SNORM, 4}, {73, VK_FORMAT_R8G8B8A8_UINT, 4}, {74, VK_FORMAT_R8G8B8A8_SINT, 4},
    {80, VK_FORMAT_B8G8R8A8_UNORM, 4}, {81, VK_FORMAT_B8G8R8A8_SRGB, 4},
    {90, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4}, {92, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4}, {93, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4},
    {103, VK_FORMAT_R32G32_UINT, 8}, {104, VK_FORMAT_R32G32_SINT, 8}, {105, VK_FORMAT_R32G32_SFLOAT, 8},
    {110, VK_FORMAT_R16G16B16A16_UNORM, 8}, {112, VK_FORMAT_R16G16B16A16_SNORM, 8}, {113, VK_FORMAT_R16G16B16A16_UINT, 8}, {114, VK_FORMAT_R16G16B16A16_SINT, 8}, {115, VK_FORMAT_R16G16B16A16_SFLOAT, 8},
    {123, VK_FORMAT_R32G32B32A32_UINT, 16}, {124, VK_FORMAT_R32G32B32A32_SINT, 16}, {125, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
};

/* Depth / stencil pixel formats (MTLPixelFormat: Depth16Unorm 250, Depth32Float 252, Stencil8 253, Depth24Unorm_Stencil8 255, Depth32Float_Stencil8 260). Such a
 * texture is a device image only, used as a render attachment: no guest copy, no upload or write-back, no blit and no sampling (all refused, counted as bad_object
 * by the callers). aspects: bit 0 depth, bit 1 stencil. 255 is macOS-only in Metal and optional in Vulkan: create_image falls back to D32_SFLOAT_S8_UINT
 * (a superset of the 24-bit unorm range) where D24_UNORM_S8_UINT cannot be an attachment. Not mapped: 261 / 262 (stencil views of a combined texture), 20x / 26x others. */
static const struct { uint32_t mtl; VkFormat vk; uint8_t aspects; } k_dfmt[] = {
    {250, VK_FORMAT_D16_UNORM, 1}, {252, VK_FORMAT_D32_SFLOAT, 1}, {253, VK_FORMAT_S8_UINT, 2},
    {255, VK_FORMAT_D24_UNORM_S8_UINT, 3}, {260, VK_FORMAT_D32_SFLOAT_S8_UINT, 3},
};

/* The VkFormats the mirror can create (colour table, then depth / stencil), with their Metal numbers: the list the capability report (g7-fmtcap) and the format tests walk.
 * Returns the total count; copies at most `cap` entries. */
size_t lagfx_a64_res_format_list(VkFormat *vk, uint32_t *mtl, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < sizeof k_fmt / sizeof *k_fmt; i++, n++) if (n < cap) { if (vk) vk[n] = k_fmt[i].vk; if (mtl) mtl[n] = k_fmt[i].mtl; }
    for (size_t i = 0; i < sizeof k_dfmt / sizeof *k_dfmt; i++, n++) if (n < cap) { if (vk) vk[n] = k_dfmt[i].vk; if (mtl) mtl[n] = k_dfmt[i].mtl; }
    return n;
}

/* ---- object lifetime ---- */

static void msurf_unref(struct lagfx_vk_state *vk, msurf_t *ms) {
    if (!ms || --ms->refs) return;
    mgr2_t *m = ms->m;
    for (uint32_t i = 0; m && i < m->nms; i++) if (m->ms[i] == ms) { memmove(&m->ms[i], &m->ms[i + 1], (m->nms - i - 1u) * sizeof *m->ms); m->nms--; break; }
    if (ms->img) vkDestroyImage(vk->device, ms->img, NULL);
    if (ms->img_mem) vkFreeMemory(vk->device, ms->img_mem, NULL);
    free_buf(vk, ms->stage, ms->stage_mem);
    free(ms);
}

static void obj_free(struct lagfx_vk_state *vk, lagfx_a64_obj_t *o) {
    if (!o) return;
    if (o->ms) {   /* the image belongs to the shared surface */
        if (o->view) vkDestroyImageView(vk->device, o->view, NULL);
        msurf_unref(vk, o->ms);
        free(o);
        return;
    }
    if (o->view) vkDestroyImageView(vk->device, o->view, NULL);
    if (o->sview) vkDestroyImageView(vk->device, o->sview, NULL);
    if (o->view3) vkDestroyImageView(vk->device, o->view3, NULL);
    if (o->img) vkDestroyImage(vk->device, o->img, NULL);
    if (o->img_mem) vkFreeMemory(vk->device, o->img_mem, NULL);
    free_buf(vk, o->buf, o->buf_mem);
    free(o);
}

static bool pending_wait(struct lagfx_vk_state *vk, mgr2_t *m);
void lagfx_a64_res_destroy(lagfx_protocol_t *p) {
    if (!p || !p->a64_res) return;
    mgr2_t *m = p->a64_res;
    struct lagfx_vk_state *vk = vk_of(p);
    bool busy = m->pending != NULL || m->sw_fence != VK_NULL_HANDLE;
    if (busy && vk) {   /* give an unfinished exec one bounded chance; else leak its objects rather than free what the GPU may still use */
        if (!m->pending || pending_wait(vk, m)) busy = false;
        if (!busy && m->sw_fence && lagfx_vk_wait_bounded(vk, m->sw_fence, 0) != LAGFX_VK_WAIT_OK) busy = true;
    }
    if (!busy && vk) vkDeviceWaitIdle(vk->device);
    if (!busy && vk && m->sw_fence) { vkDestroyFence(vk->device, m->sw_fence, NULL); if (m->sw_cb) lagfx_vk_cmdbuf_free(vk, m->sw_cb); m->sw_fence = VK_NULL_HANDLE, m->sw_cb = VK_NULL_HANDLE; }   /* an abandoned swap copy (its fence was waited for above) */
    for (uint32_t i = 0; i < m->n; i++) { if (vk && !busy) obj_free(vk, m->objs[i]); else if (!vk) free(m->objs[i]); }
    for (uint32_t i = 0; i < m->ndead; i++) { if (vk && !busy) obj_free(vk, m->dead[i]); else if (!vk) free(m->dead[i]); }
    free(m->dead);
    free(m->objs);
    free(m->ms);
    free(m);
    p->a64_res = NULL;
}

const lagfx_a64_stats_t *lagfx_a64_res_stats(lagfx_protocol_t *p) {
    static lagfx_a64_stats_t zero;
    mgr2_t *m = p ? p->a64_res : NULL;
    return m ? &m->st : &zero;
}

/* An object leaves the table (replaced entry, 0x25, task teardown): its Vulkan objects may still be referenced by the command buffer being recorded or by
 * a submission whose fence has not signalled, so they are parked and destroyed only after that fence signalled (exec_finish / destroy). */
static void obj_release(mgr2_t *m, struct lagfx_vk_state *vk, lagfx_a64_obj_t *o) {
    if (!o) return;
    OT(m).obj_destroyed++; if (OT(m).obj_live) OT(m).obj_live--;
    if (BUSY(m)) {
        OT(m).obj_deferred++;
        if (m->ndead == m->capdead) {
            uint32_t nc = m->capdead ? m->capdead * 2u : 16u;
            lagfx_a64_obj_t **n = realloc(m->dead, nc * sizeof *n);
            if (!n) return;   /* cannot park it: leaking beats freeing under the GPU */
            m->dead = n, m->capdead = nc;
        }
        m->dead[m->ndead++] = o;
        m->st.deferred_frees++;
        return;
    }
    obj_free(vk, o);
    m->st.destroyed++;
}
static void dead_flush(mgr2_t *m, struct lagfx_vk_state *vk) {
    if (BUSY(m)) return;
    for (uint32_t i = 0; i < m->ndead; i++) { obj_free(vk, m->dead[i]); m->st.destroyed++; }
    m->ndead = 0;
}

static bool is_tex3d(const lagfx_a64_obj_t *o) { return o->img_type == VK_IMAGE_TYPE_3D; }
static uint32_t lvl_dim(uint32_t v, uint32_t l) { v >>= l; return v ? v : 1u; }

/* guest pages <-> guest copy (16 KiB at a time; an unmapped page reads as zero and is not written back) */
static void guest_upload(lagfx_protocol_t *p, mgr2_t *m, const lagfx_task_entry_t *t, lagfx_a64_obj_t *o) {
    if (!o->has_pages) return;
    for (uint64_t off = 0; off < o->length; off += 0x4000u) {
        uint8_t *d = o->map + off;
        if (!lagfx_arm64_task_read(p, t, (o->vpage << 14) + off, d, 0x4000u)) memset(d, 0, 0x4000u), m->st.unmapped_pages++;
    }
    o->wgen++;
    o->rd_epoch = m->epoch, o->gpu_dirty = false;
    m->st.uploads++;
}

static void guest_download(lagfx_protocol_t *p, mgr2_t *m, const lagfx_task_entry_t *t, lagfx_a64_obj_t *o) {
    if (!o->has_pages) return;
    for (uint64_t off = 0; off < o->length; off += 0x4000u)
        if (!lagfx_arm64_task_write(p, t, (o->vpage << 14) + off, o->map + off, 0x4000u)) m->st.unmapped_pages++;
    m->st.downloads++;
}

/* texture sub-resource <-> guest copy: one region per (slice, mip), as Apple's page:toGuest: */
static bool tex_region(const lagfx_a64_obj_t *o, uint32_t s, uint32_t l, VkBufferImageCopy *r) {
    if (l >= o->tex_mip_count || l >= o->mips) return false;
    uint32_t w = o->mip[l].w, h = o->mip[l].h, d = o->mip[l].d;
    uint64_t row = o->mip[l].row;
    if (!w || !h || !d || w > lvl_dim(o->width, l) || h > lvl_dim(o->height, l) || d > lvl_dim(o->depth, l)) return false;
    if (!row || row % o->bpp || row / o->bpp < w || row > (1ull << 31)) return false;
    unsigned __int128 from128 = (unsigned __int128)o->tex_base + (unsigned __int128)o->tex_stride * s + o->mip[l].off, image = (unsigned __int128)row * h;
    if (from128 + image * d > o->length) return false;   /* Apple skips a region that does not fit */
    uint64_t from = (uint64_t)from128;
    if (from % o->bpp) return false;
    memset(r, 0, sizeof *r);
    r->bufferOffset = from;
    r->bufferRowLength = (uint32_t)(row / o->bpp);
    r->bufferImageHeight = d > 1 ? h : 0;
    r->imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, l, is_tex3d(o) ? 0 : s, 1 };
    r->imageExtent = (VkExtent3D){ w, h, d };
    return true;
}

static void tex_record(VkCommandBuffer cb, lagfx_a64_obj_t *o, bool down) {
    uint32_t layers = is_tex3d(o) ? 1u : o->layers;
    for (uint32_t s = 0; s < layers; s++)
        for (uint32_t l = 0; l < o->mips; l++) {
            VkBufferImageCopy r;
            if (!tex_region(o, s, l, &r)) continue;
            lagfx_translate_blit_cmd_buffer_image(cb, o->buf, o->img, &r, !down);
        }
}

static VkImageAspectFlags ds_aspect_mask(const lagfx_a64_obj_t *o) {
    return ((o->ds_aspects & 1u) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0u) | ((o->ds_aspects & 2u) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
}

/* ---- object creation from the guest's table entry ---- */

/* Usage trim bookkeeping: index into diag2.exec.fmt.dropped[] (0 storage, 1 input attachment, 2 colour attachment, 3 sampled, 4 transfer dst, 5 transfer src). */
static void fmt_note_dropped(lagfx_protocol_t *p, VkImageUsageFlags dropped) {
    static const VkImageUsageFlags k[6] = { VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
    for (int i = 0; i < 6; i++) if (dropped & k[i]) p->diag2.exec.fmt.dropped[i]++;
}

static bool create_image(struct lagfx_vk_state *vk, mgr2_t *m, lagfx_a64_obj_t *o) {
    lagfx_fmtcap_backend_t be_store;
    const lagfx_fmtcap_backend_t *be = lagfx_fmtcap_active(vk->phys_device, &be_store);
    const bool ds = o->ds_aspects != 0;
    if (ds && o->format == VK_FORMAT_D24_UNORM_S8_UINT && !(lagfx_fmtcap_features(be, o->format) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) o->format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    /* What the image can be used for is asked of the device as ONE mask (vkGetPhysicalDeviceImageFormatProperties2): a usage that is missing or refused in the combination is
     * dropped and COUNTED (fmt.dropped / fmt.img_trimmed) instead of failing the texture or leaving the bit out unseen. Required: the transfers that mirror the guest bytes (a
     * depth / stencil attachment only needs to be an attachment). input attachment = framebuffer fetch reads of a colour attachment (VUID 00338: the view usage must include
     * it); it rides on COLOR_ATTACHMENT. */
    const VkImageUsageFlags req = ds ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    const VkImageUsageFlags opt = ds ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    lagfx_fmtcap_usage_t cu;
    if (!lagfx_fmtcap_image_usage(be, o->format, o->img_type, o->width, o->height, o->depth, o->mips, is_tex3d(o) ? 1u : o->layers, VK_SAMPLE_COUNT_1_BIT, req, opt, &cu)) {
        m->p->diag2.exec.fmt.refused[cu.why]++;
        return false;
    }
    VkImageUsageFlags u = cu.usage;
    m->p->diag2.exec.fmt.img_created++;
    if (cu.dropped) { m->p->diag2.exec.fmt.img_trimmed++; fmt_note_dropped(m->p, cu.dropped); }
    o->usage = u;
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = o->img_type, .format = o->format,
        .extent = { o->width, o->height, o->depth }, .mipLevels = o->mips, .arrayLayers = is_tex3d(o) ? 1u : o->layers, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = u, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (vkCreateImage(vk->device, &ci, NULL, &o->img) != VK_SUCCESS) { o->img = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements rq;
    vkGetImageMemoryRequirements(vk->device, o->img, &rq);
    uint32_t mt = mem_type(vk, rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) mt = mem_type(vk, rq.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = mt };
    if (mt == UINT32_MAX || vkAllocateMemory(vk->device, &ai, NULL, &o->img_mem) != VK_SUCCESS) { o->img_mem = VK_NULL_HANDLE; return false; }
    if (vkBindImageMemory(vk->device, o->img, o->img_mem, 0) != VK_SUCCESS) return false;
    /* UNDEFINED -> GENERAL and a defined (zero) content */
    VkCommandBuffer cb;
    if (!cb_begin(vk, &cb)) return false;
    VkImageSubresourceRange rg = { ds ? ds_aspect_mask(o) : VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = o->img, .subresourceRange = rg };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    if (ds) {
        if (u & VK_IMAGE_USAGE_TRANSFER_DST_BIT) { VkClearDepthStencilValue zz = { 0.0f, 0u }; vkCmdClearDepthStencilImage(cb, o->img, VK_IMAGE_LAYOUT_GENERAL, &zz, 1, &rg); }
    } else {
        VkClearColorValue z; memset(&z, 0, sizeof z);
        vkCmdClearColorImage(cb, o->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg);
    }
    barrier(cb);
    bool ok = vkEndCommandBuffer(cb) == VK_SUCCESS;
    if (ok) {
        lagfx_a64_status_t st = submit_wait(vk, m, &cb);
        ok = st == LAGFX_A64_DONE;
        if (!cb && st != LAGFX_A64_FAILED) o->leak = true;   /* possibly still running on o->img: never free it */
        if (!cb && st == LAGFX_A64_FAILED && m->st.gpu_fail) o->leak = true;
    }
    if (cb) lagfx_vk_cmdbuf_free(vk, cb);
    return ok;
}

/* ---- mapped textures (type 11): pixels live in a guest display surface, descriptor from the shell ---- */

static bool o_hv(const lagfx_a64_obj_t *o) { return o->ms ? o->ms->host_valid : o->host_valid; }
static void o_set_hv(lagfx_a64_obj_t *o, bool v) { if (o->ms) o->ms->host_valid = v; else o->host_valid = v; }
static bool o_stale(const lagfx_a64_obj_t *o) { return o->ms ? o->ms->guest_stale : o->guest_stale; }
static void o_set_stale(lagfx_a64_obj_t *o, bool v) { if (o->ms) o->ms->guest_stale = v; else o->guest_stale = v; }

/* The shell's descriptor of `mapping`, decoded (BGRA or RGhA). Counts mapped_no_desc (no callback / unknown id) or mapped_unsupported (does not decode / not BGRA). */
static bool ms_desc(lagfx_protocol_t *p, uint32_t mapping, lagfx_guest_surface_t *gs) {
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    const lagfx_shell_callbacks_t *sh = dev ? &dev->desc.shell : NULL;
    uint8_t d[256];
    size_t n = 0;
    memset(d, 0, sizeof d);
    if (!sh || !sh->get_surface_desc || !sh->get_surface_desc(sh->opaque, mapping, d, sizeof d, &n) || n < LAGFX_SURF_DESC_BYTES || n > sizeof d) { MRUN(p).mapped_no_desc++; return false; }
    if (lagfx_guest_surface_decode_any(d, n, gs) != LAGFX_OK) { MRUN(p).mapped_unsupported++; return false; }
    return true;
}

static const lagfx_shell_callbacks_t *ms_shell(lagfx_protocol_t *p) { lagfx_device_t *dev = (lagfx_device_t *)p->dev; return dev ? &dev->desc.shell : NULL; }

/* guest surface pages -> staging buffer -> image (recorded into cb; the caller adds the barrier). False (counted) if the surface cannot be read: nothing recorded. */
/* FNV-1a over the surface's page list (the guest's actual pfn entries): a re-map onto other pages behind the same list address changes it. 0 = unreadable. */
static uint64_t ms_list_hash(lagfx_protocol_t *p, const lagfx_guest_surface_t *gs) {
    const lagfx_shell_callbacks_t *sh = ms_shell(p);
    if (!sh || !sh->read_memory || !gs->npages || gs->npages > 65536u) return 0;
    uint64_t h = 1469598103934665603ull;
    uint32_t buf[1024];
    for (uint32_t i = 0; i < gs->npages; ) {
        uint32_t n = gs->npages - i < 1024u ? gs->npages - i : 1024u;
        if (!sh->read_memory(sh->opaque, gs->page_list_gpa + 4ull * i, 4u * n, buf)) return 0;
        for (uint32_t k = 0; k < n; k++) { h ^= buf[k]; h *= 1099511628211ull; }
        i += n;
    }
    return h ? h : 1;
}
static bool ms_pull(lagfx_protocol_t *p, mgr2_t *m, VkCommandBuffer cb, msurf_t *ms) {
    if (ms->up_epoch == m->epoch) return true;   /* already taken in this exec */
    lagfx_guest_surface_t gs;
    if (!ms_desc(p, ms->mapping, &gs)) return false;
    if (gs.width != ms->w || gs.height != ms->h || gs.fourcc != ms->fourcc) { MRUN(p).mapped_unsupported++; return false; }   /* the guest re-made the surface with another size */
    lagfx_guest_surface_xfer_stats_t st;
    memset(&st, 0, sizeof st);
    if (lagfx_guest_surface_xfer(ms_shell(p), &gs, ms->stage_map, ms->w, ms->h, (size_t)ms->w * ms->bpp, false, &st) != LAGFX_OK) { MRUN(p).mapped_no_desc++; return false; }
    MRUN(p).mapped_uploaded++, MRUN(p).mapped_bytes_up += st.bytes, MRUN(p).mapped_pages_skipped += st.pages_skipped;
    VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { ms->w, ms->h, 1 } };
    lagfx_translate_blit_cmd_buffer_image(cb, ms->stage, ms->img, &r, true);
    ms->up_epoch = m->epoch;
    ms->id_pl = gs.page_list_gpa, ms->id_alloc = gs.alloc, ms->id_first = gs.first_offset, ms->id_np = gs.npages, ms->id_row = gs.row_bytes, ms->id_hash = ms_list_hash(p, &gs), ms->id_set = true;
    ms->owed = false;   /* the guest's newer pixels replace whatever the GPU owed */
    return true;
}

/* image -> staging buffer (recorded); only a surface that holds meaningful pixels (host_valid). Once per epoch. */
static bool ms_record_down(mgr2_t *m, VkCommandBuffer cb, msurf_t *ms, uint64_t gen) {
    if (!ms->host_valid) return false;
    if (ms->down_gen == gen && ms->down_pending) return true;
    barrier(cb);
    VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { ms->w, ms->h, 1 } };
    lagfx_translate_blit_cmd_buffer_image(cb, ms->stage, ms->img, &r, false);
    host_barrier(m, cb);
    barrier(cb);
    ms->down_pending = true, ms->down_epoch = m->epoch, ms->down_gen = gen;
    return true;
}

/* LAGFX_WB_VERIFY=1 only: read-only check that the guest pages hold what was just written (see lagfx_guest_surface_verify). Never changes ms or the guest. */
static void ms_wb_verify(lagfx_protocol_t *p, msurf_t *ms, const lagfx_guest_surface_t *gs) {
    static lagfx_wbv_gate_t gates[2];   /* [0] BGRA, [1] RGhA: the big display surface has its own budget, the many small BGRA ones must not use it up */
    static uint32_t lines;
    if (gs->bpp != 4u && gs->bpp != 8u) return;
    lagfx_wbv_gate_t *gate_p = &gates[gs->bpp == 8u];
    if (!lagfx_wbv_admit(gate_p, lagfx_wbv_now_ns())) {
        if (gate_p->done >= LAGFX_WBV_MAX_COMPARES && !gate_p->capped_logged && lines < 40u) gate_p->capped_logged = true, lines++, LAGFX_WARN("wbverify cap reached (%u compares, bpp %u)", LAGFX_WBV_MAX_COMPARES, gs->bpp);
        return;
    }
    lagfx_wbv_result_t r;
    lagfx_status_t st = lagfx_guest_surface_verify(ms_shell(p), gs, ms->stage_map, ms->w, ms->h, (size_t)ms->w * ms->bpp, &r);
    if (lines >= 40u) return;
    lines++;
    unsigned long long swap = (unsigned long long)p->diag2.disp.op_swap;
    if (st != LAGFX_OK) { LAGFX_WARN("wbverify swap=%llu ms=%u %ux%u row=%llu first=%u npages=%u fourcc=%08x: read-back failed (status %d)", swap, ms->mapping, ms->w, ms->h, (unsigned long long)gs->row_bytes, gs->first_offset, gs->npages, gs->fourcc, (int)st); return; }
    LAGFX_WARN("wbverify swap=%llu ms=%u %ux%u row=%llu first=%u npages=%u fourcc=%08x bpp=%u rgb_mismatch=%llu alpha_only_mismatch=%llu of %llu first_diff=%d,%d pages_skipped=%llu hash_algo=%s hash_written=%016llx hash_guest=%016llx hash_guest2=%016llx stable=%d half_nonfinite_px=%llu half_negative_px=%llu half_above_one_px=%llu (raw guest-page round trip only: no GPU image, no colour)",
               swap, ms->mapping, r.w, r.h, (unsigned long long)gs->row_bytes, gs->first_offset, gs->npages, gs->fourcc, r.bpp, (unsigned long long)r.rgb_mismatch, (unsigned long long)r.alpha_only_mismatch, (unsigned long long)r.pixels,
               r.first_x, r.first_y, (unsigned long long)r.pages_skipped, r.bpp == 8u ? "u64_fnv1a_full" : "pvgpix_u32_fnv1a_full", (unsigned long long)r.hash_buf, (unsigned long long)r.hash_guest, (unsigned long long)r.hash_guest2, r.stable ? 1 : 0,
               (unsigned long long)r.half_nonfinite, (unsigned long long)r.half_negative, (unsigned long long)r.half_above_one);
}

/* after the fence: staging buffer -> guest pages (rows at first_offset + y * row; absent pages skipped and counted).
 * Returns 1 written, 0 not written (descriptor unreadable: counted, owed again in swap mode only; transfer failed; or this generation recorded nothing for the surface), -1 the guest re-mapped the surface since the pixels were taken (dropped). */
static int ms_finish_down(lagfx_protocol_t *p, msurf_t *ms, uint64_t gen) {
    if (!ms->down_pending || ms->down_gen != gen) { if (ms->down_gen != gen) MRUN(p).wb_down_skipped_not_recorded++; return 0; }   /* (same generation, already finished: a second ref of the same surface in one exec, not counted) */   /* this exec / sync / swap copy did not record the copy for this surface: its staging bytes are not ours to write (a pending recording of another generation is left alone) */
    ms->down_pending = false;
    lagfx_guest_surface_t gs;
    if (lagfx_a64_test_hook == 4 ? (lagfx_a64_test_hook = 0, true) : !ms_desc(p, ms->mapping, &gs)) { if (mapped_wb_on_swap()) ms->owed = true; MRUN(p).wb_finish_desc_unreadable_owed++; return 0; }   /* descriptor unreadable: counted. Swap mode only: the surface is owed again and paid by the next display swap. Immediate mode: diagnostic only, NOT recovered (display_swap does nothing outside =swap), so the flag is not set there */
    if (gs.width != ms->w || gs.height != ms->h || gs.fourcc != ms->fourcc) { MRUN(p).mapped_unsupported++; return -1; }
    if (ms->id_set && (gs.page_list_gpa != ms->id_pl || gs.alloc != ms->id_alloc || gs.first_offset != ms->id_first || gs.npages != ms->id_np || gs.row_bytes != ms->id_row ||
                      (ms->id_hash && ms_list_hash(p, &gs) != ms->id_hash))) { MRUN(p).mapped_remapped++; return -1; }
    lagfx_guest_surface_xfer_stats_t st;
    memset(&st, 0, sizeof st);
    if (lagfx_guest_surface_xfer(ms_shell(p), &gs, ms->stage_map, ms->w, ms->h, (size_t)ms->w * ms->bpp, true, &st) != LAGFX_OK) { MRUN(p).mapped_no_desc++; return 0; }
    MRUN(p).mapped_written_back++, MRUN(p).mapped_bytes_down += st.bytes, MRUN(p).mapped_pages_skipped += st.pages_skipped;
    if (lagfx_wbv_enabled()) ms_wb_verify(p, ms, &gs);
    ms->guest_stale = false;
    ms->owed = false;   /* whoever paid it (flag 4, synchronize, swap): the guest is current */
    return 1;
}

/* Builds o as a type 11 object over {u64 mapping, u32 tag, u32 size, u32 ref, description}. False (counted, o still freeable) if it cannot be. */
static bool mapped_make(lagfx_protocol_t *p, mgr2_t *m, struct lagfx_vk_state *vk, const lagfx_task_entry_t *t, lagfx_a64_obj_t *o, uint64_t at, uint32_t size) {
    uint8_t h[20 + 64];
    memset(h, 0, sizeof h);
    uint32_t n = size < sizeof h ? size : (uint32_t)sizeof h;
    if (size < 20u + 4u || (!lagfx_arm64_task_read(p, t, at, h, n) && (n <= 52u || !lagfx_arm64_task_read(p, t, at, h, 52u)))) { MRUN(p).mapped_unsupported++; MKFAIL(TEXWHY_C_PAYLOAD); return false; }
    uint64_t mapping = lagfx_le64(h);
    uint32_t tag = lagfx_le32(h + 8);
    const uint8_t *d = h + 20;
    uint32_t fmt = tag == 0x39u ? (uint32_t)(d[1] | d[2] << 8) : (uint32_t)(d[2] | d[3] << 8);   /* v2 / v1 description (Henge makeMappedTexture) */
    if (mapping > 0xffffffffull) { MRUN(p).mapped_unsupported++; return false; }
    lagfx_guest_surface_t gs;
    { uint64_t nd = MRUN(p).mapped_no_desc;
      if (!ms_desc(p, (uint32_t)mapping, &gs)) { MKFAIL(MRUN(p).mapped_no_desc != nd ? TEXWHY_C_MAPPED_NODESC : TEXWHY_C_MAPPED_BAD); return false; } }
    if ((uint64_t)gs.width * gs.height * gs.bpp > LAGFX_A64_MAX_OBJ_BYTES) { MRUN(p).mapped_unsupported++; return false; }
    msurf_t *ms = NULL;
    for (uint32_t i = m->nms; i-- > 0;) if (m->ms[i]->mapping == (uint32_t)mapping && m->ms[i]->w == gs.width && m->ms[i]->h == gs.height && m->ms[i]->fourcc == gs.fourcc) { ms = m->ms[i]; break; }
    if (!ms) {
        if (m->nms >= MAX_MS) { MRUN(p).mapped_unsupported++; return false; }
        if (m->nms == 0 || (m->nms & (m->nms - 1u)) == 0) {   /* grow the registry on powers of two */
            uint32_t nc = m->nms ? m->nms * 2u : 8u;
            struct lagfx_a64_msurf **nn = realloc(m->ms, nc * sizeof *nn);
            if (!nn) { MRUN(p).mapped_unsupported++; return false; }
            m->ms = nn;
        }
        /* the image format follows the SURFACE's fourcc (not the description): BGRA -> B8G8R8A8, 'RGhA' -> R16G16B16A16_SFLOAT (raw half floats, no conversion) */
        o->format = gs.fourcc == LAGFX_FOURCC_RGHA ? VK_FORMAT_R16G16B16A16_SFLOAT : fmt == 81u ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
        o->img_type = VK_IMAGE_TYPE_2D, o->width = gs.width, o->height = gs.height, o->depth = 1, o->mips = 1, o->layers = 1, o->bpp = gs.bpp;
        if (!create_image(vk, m, o)) { MRUN(p).mapped_unsupported++; return false; }
        if (!(o->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || !(o->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) { MRUN(p).mapped_unsupported++; return false; }   /* o frees its own image */
        ms = calloc(1, sizeof *ms);
        if (!ms) { MRUN(p).mapped_unsupported++; return false; }
        if (!make_buf(vk, (VkDeviceSize)gs.width * gs.height * gs.bpp, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &ms->stage, &ms->stage_mem, &ms->stage_map)) {
            free(ms); MRUN(p).mapped_unsupported++; return false;
        }
        ms->m = m, ms->mapping = (uint32_t)mapping, ms->w = gs.width, ms->h = gs.height, ms->fourcc = gs.fourcc, ms->bpp = gs.bpp, ms->format = o->format, ms->usage = o->usage, ms->mtl_fmt = fmt;
        ms->img = o->img, ms->img_mem = o->img_mem;   /* ownership moves to the surface */
        o->img_mem = VK_NULL_HANDLE;
        m->ms[m->nms++] = ms;
        MRUN(p).mapped_created++;
        LAGFX_LOG("arm64 mapped texture: mapping %u surface %ux%u row %llu first %u, description tag 0x%x format %u", (unsigned)mapping, gs.width, gs.height, (unsigned long long)gs.row_bytes, gs.first_offset, tag, fmt);
    } else {
        o->format = ms->format, o->usage = ms->usage, o->img = ms->img;
        o->img_type = VK_IMAGE_TYPE_2D, o->width = ms->w, o->height = ms->h, o->depth = 1, o->mips = 1, o->layers = 1, o->bpp = ms->bpp;
        MRUN(p).mapped_shared++;
    }
    ms->refs++;
    o->mtl_format = fmt;
    o->ms = ms, o->mapping = (uint32_t)mapping;
    o->kind = LAGFX_A64_TEXTURE, o->has_pages = false, o->length = 0, o->tex_mip_count = 1;
    o->mip[0].w = o->width, o->mip[0].h = o->height, o->mip[0].d = 1, o->mip[0].row = (uint64_t)o->width * o->bpp;
    return true;
}

static lagfx_a64_obj_t *obj_make(lagfx_protocol_t *p, mgr2_t *m, struct lagfx_vk_state *vk, const lagfx_task_entry_t *t, uint32_t ref, const uint32_t e[3]) {
    uint32_t type = e[0] & 0xffu, size = e[0] >> 8;
    uint64_t at = (uint64_t)e[2] << 32 | e[1];
    m->mk_why = 0;
    lagfx_a64_obj_t *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->task = t->id, o->ref = ref, memcpy(o->entry, e, 12);
    if (type == 1 || type == 19) {
        uint8_t h[16];
        if (!lagfx_arm64_task_read(p, t, at, h, 16)) goto bad;
        uint64_t len = lagfx_le64(h);
        if (!len || len > LAGFX_A64_MAX_OBJ_BYTES) goto bad;
        o->kind = LAGFX_A64_BUFFER, o->length = (len + 0x3fffu) & ~0x3fffull, o->vpage = lagfx_le32(h + 8), o->has_pages = o->vpage != 0;
        if (!make_buf(vk, o->length, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &o->buf, &o->buf_mem, &o->map)) goto bad;
    } else if (type == 2) {
        if (size < 12 + 0x18 + 12 + 28 || size > 8192u) { MKFAIL(TEXWHY_C_PAYLOAD); goto bad; }
        uint8_t *d = malloc(size);
        if (!d) { MKFAIL(TEXWHY_C_PAYLOAD); goto bad; }
        if (!lagfx_arm64_task_read(p, t, at, d, size)) { free(d); MKFAIL(TEXWHY_C_PAYLOAD); goto bad; }
        uint64_t len = lagfx_le64(d);
        const uint8_t *lay = d + 12;
        uint32_t mips = lagfx_le16(lay) & 0x3fffu;
        o->vpage = lagfx_le32(d + 8), o->has_pages = o->vpage != 0;
        o->tex_base = lagfx_le64(lay + 4), o->tex_stride = lagfx_le64(lay + 12), o->tex_block_h = lay[0x15];
        bool ok = len && len <= LAGFX_A64_MAX_OBJ_BYTES && mips >= 1 && mips <= LAGFX_A64_MAX_MIPS && 12ull + 0x18 + 36ull * mips + 12 + 28 <= size && o->tex_block_h <= 1;
        const uint8_t *desc = NULL;
        uint32_t tag = 0;
        if (ok) {
            const uint8_t *rec = lay + 0x18 + 36u * mips;
            tag = lagfx_le32(rec), desc = rec + 12;
            o->tex_mip_count = mips;
            for (uint32_t i = 0; i < mips; i++) {
                const uint8_t *mp = lay + 0x18 + 36u * i;
                o->mip[i].off = lagfx_le64(mp), o->mip[i].row = lagfx_le64(mp + 0x10);
                o->mip[i].w = lagfx_le32(mp + 0x18), o->mip[i].h = lagfx_le32(mp + 0x1c), o->mip[i].d = lagfx_le32(mp + 0x20);
            }
        }
        uint32_t mtype = 0, fmt = 0, samples = 1, array = 1, dmips = 1;
        if (ok) {
            if (tag == 0x34 || tag == 0x39) {
                mtype = desc[0] & 0xfu, fmt = desc[1] | desc[2] << 8;
                o->width = lagfx_le32(desc + 7), o->height = lagfx_le32(desc + 11), o->depth = lagfx_le32(desc + 15);
                dmips = lagfx_le16(desc + 19), samples = lagfx_le16(desc + 21), array = lagfx_le16(desc + 23);
            } else {
                mtype = desc[0] & 0xfu, fmt = desc[2] | desc[3] << 8;
                o->width = lagfx_le32(desc + 4), o->height = lagfx_le32(desc + 8), o->depth = lagfx_le32(desc + 12);
                dmips = lagfx_le16(desc + 16), samples = lagfx_le16(desc + 18), array = lagfx_le16(desc + 20);
            }
        }
        free(d);
        o->mtl_format = fmt;
        o->format = VK_FORMAT_UNDEFINED;
        for (size_t i = 0; ok && i < sizeof k_fmt / sizeof *k_fmt; i++) if (k_fmt[i].mtl == fmt) o->format = k_fmt[i].vk, o->bpp = k_fmt[i].bpp;
        for (size_t i = 0; ok && o->format == VK_FORMAT_UNDEFINED && i < sizeof k_dfmt / sizeof *k_dfmt; i++)
            if (k_dfmt[i].mtl == fmt) o->format = k_dfmt[i].vk, o->ds_aspects = k_dfmt[i].aspects, o->bpp = 4;   /* bpp only keeps divisions safe: no guest copy is ever made */
        /* MTLTextureType 2 = 2D, 3 = 2D array, 7 = 3D */
        o->img_type = mtype == 7 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        if (!ok) { MKFAIL(o->tex_block_h > 1 ? TEXWHY_C_FORMAT : TEXWHY_C_PAYLOAD); goto bad; }   /* block height > 1: a block-compressed layout */
        if (o->format == VK_FORMAT_UNDEFINED) { MKFAIL(TEXWHY_C_FORMAT); goto bad; }
        if (mtype != 2 && mtype != 3 && mtype != 7) { MKFAIL(TEXWHY_C_TYPE); goto bad; }
        if (samples > 1) { MKFAIL(TEXWHY_C_MSAA); p->diag2.exec.fmt.msaa_refused++; goto bad; }
        if (!o->width || !o->height || o->width > 16384 || o->height > 16384) { MKFAIL(TEXWHY_C_DIMS); goto bad; }
        if (mtype == 7) { if (!o->depth || o->depth > 2048) { MKFAIL(TEXWHY_C_DIMS); goto bad; } } else o->depth = 1;
        if (!dmips || dmips > LAGFX_A64_MAX_MIPS) { MKFAIL(TEXWHY_C_DIMS); goto bad; }
        o->mips = dmips;
        o->layers = mtype == 3 ? array : 1u;
        if (!o->layers || o->layers > LAGFX_A64_MAX_LAYERS) { MKFAIL(TEXWHY_C_DIMS); goto bad; }
        o->kind = LAGFX_A64_TEXTURE, o->length = (len + 0x3fffu) & ~0x3fffull;
        if (o->ds_aspects) {   /* depth / stencil attachment: a plain 2D image of one level, content only on the device */
            if (mtype != 2 || o->mips != 1) { MKFAIL(TEXWHY_C_DIMS); goto bad; }
            o->has_pages = false, o->length = 0, o->tex_mip_count = 0;
            if (!create_image(vk, m, o)) { MKFAIL(TEXWHY_C_VULKAN); goto bad; }
            return o;
        }
        if (!make_buf(vk, o->length, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &o->buf, &o->buf_mem, &o->map)) { MKFAIL(TEXWHY_C_VULKAN); goto bad; }
        if (!create_image(vk, m, o)) { MKFAIL(TEXWHY_C_VULKAN); goto bad; }
    } else if (type == 8 || type == 9) {
        /* linear texture without guest pages ({tag, size, ref, description}): a device image only (no guest copy, no write-back), enough for render targets
         * and blits of the uncompressed formats of the table; 2D, one mip, one sample. A linear texture that cannot be mirrored is counted in
         * stats.linear_unsupported (diagnostics) and Render then skips the pass (no_target). */
        uint8_t d[512];
        if (size < 12 + 20 || size > sizeof d || !lagfx_arm64_task_read(p, t, at, d, size)) { MKFAIL(TEXWHY_C_PAYLOAD); goto bad_linear; }
        uint32_t tag = lagfx_le32(d), mtype, fmt, samples;
        const uint8_t *desc = d + 12;
        if (tag == 9 || tag == 0x37) {   /* over a buffer (Henge makeLinearTexture): {tag, size, ref, buffer ref, u64 offset, u64 row, description} */
            if (size < 0x20 + 0x17) { MKFAIL(TEXWHY_C_PAYLOAD); goto bad_linear; }
            o->linear = true, o->lin_buf = lagfx_le32(d + 12), o->lin_off = lagfx_le64(d + 16), o->lin_row = lagfx_le64(d + 24);
            desc = d + 0x20;
            if (tag == 0x37) { mtype = desc[0] & 0xfu, fmt = desc[1] | desc[2] << 8, o->width = lagfx_le32(desc + 7), o->height = lagfx_le32(desc + 11), samples = lagfx_le16(desc + 21); }
            else             { mtype = desc[0] & 0xfu, fmt = desc[2] | desc[3] << 8, o->width = lagfx_le32(desc + 4), o->height = lagfx_le32(desc + 8), samples = lagfx_le16(desc + 18); }
        } else if (tag == 0x34 || tag == 0x39) {
            if (size < 12 + 0x17) { MKFAIL(TEXWHY_C_PAYLOAD); goto bad_linear; }
            mtype = desc[0] & 0xfu, fmt = desc[1] | desc[2] << 8, o->width = lagfx_le32(desc + 7), o->height = lagfx_le32(desc + 11), samples = lagfx_le16(desc + 21);
        } else if (tag == 0x14 || tag == 1) {
            mtype = desc[0] & 0xfu, fmt = desc[2] | desc[3] << 8, o->width = lagfx_le32(desc + 4), o->height = lagfx_le32(desc + 8), samples = lagfx_le16(desc + 18);
        } else { MKFAIL(TEXWHY_C_TYPE); goto bad_linear; }
        o->mtl_format = fmt;
        o->format = VK_FORMAT_UNDEFINED;
        for (size_t i = 0; i < sizeof k_fmt / sizeof *k_fmt; i++) if (k_fmt[i].mtl == fmt) o->format = k_fmt[i].vk, o->bpp = k_fmt[i].bpp;
        if (mtype != 2) { MKFAIL(TEXWHY_C_TYPE); goto bad_linear; }
        if (samples > 1) { MKFAIL(TEXWHY_C_MSAA); p->diag2.exec.fmt.msaa_refused++; goto bad_linear; }
        if (o->format == VK_FORMAT_UNDEFINED) { MKFAIL(TEXWHY_C_FORMAT); goto bad_linear; }
        if (!o->width || !o->height || o->width > 16384 || o->height > 16384) { MKFAIL(TEXWHY_C_DIMS); goto bad_linear; }
        o->img_type = VK_IMAGE_TYPE_2D, o->depth = 1, o->mips = 1, o->layers = 1, o->tex_mip_count = 1;
        o->mip[0].w = o->width, o->mip[0].h = o->height, o->mip[0].d = 1, o->mip[0].row = o->linear ? o->lin_row : (uint64_t)o->width * o->bpp;
        o->kind = LAGFX_A64_TEXTURE, o->has_pages = false, o->length = 0;
        if (o->linear && (!o->lin_buf || !o->lin_row || o->lin_row % o->bpp || o->lin_row / o->bpp < o->width || o->lin_off % o->bpp || o->lin_row > (1ull << 31))) { MKFAIL(TEXWHY_C_LINEAR); goto bad_linear; }
        if (!create_image(vk, m, o)) { MKFAIL(TEXWHY_C_VULKAN); goto bad_linear; }
        m->st.linear_made++;
    } else if (type == 11) {   /* mapped texture over a guest display surface */
        if (!mapped_make(p, m, vk, t, o, at, size)) { if (!m->mk_why) MKFAIL(TEXWHY_C_MAPPED_BAD); goto bad; }
    } else { if (m->quiet_kinds) { free(o); return NULL; } goto bad; }
    return o;
bad_linear:
    m->st.linear_unsupported++;
bad:
    m->st.bad_object++;
    if (!o->leak) obj_free(vk, o);
    return NULL;
}

/* ---- the exec context ---- */

typedef struct { lagfx_a64_obj_t *o; uint32_t flags; } xres_t;

struct lagfx_a64_rexec {
    lagfx_protocol_t *p;
    mgr2_t *m;
    struct lagfx_vk_state *vk;
    const lagfx_task_entry_t *t;
    VkCommandBuffer cb;
    xres_t *res; uint32_t nres;
    VkFence fence;
    uint64_t gen;   /* write-back generation of this exec (see msurf_t.down_gen) */
    VkBuffer tmp_b[MAX_TMP]; VkDeviceMemory tmp_m[MAX_TMP]; uint32_t ntmp;
    struct { VkBuffer b; VkDeviceMemory m; } *snap; uint32_t nsnap, capsnap;   /* capture snapshots (TRANSFER_DST), freed with the exec */
    void (*hook)(void *, bool); void *hook_ctx;
};

static bool lin_pull_x(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o);
static bool lin_push_x(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o);
VkCommandBuffer lagfx_a64_res_cmdbuf(lagfx_a64_rexec_t *x) { return x ? x->cb : VK_NULL_HANDLE; }

static void memo_fail(mgr2_t *m, uint32_t task, uint32_t ref, const uint32_t e[3], uint32_t why) {
    for (uint32_t i = 0; i < 128u && i < m->nfail; i++)   /* the same (task, ref) keeps one memo */
        if (m->fail[i].task == task && m->fail[i].ref == ref) { memcpy(m->fail[i].entry, e, 12), m->fail[i].why = why; return; }
    uint32_t k = m->nfail++ % 128u;
    m->fail[k].task = task, m->fail[k].ref = ref, memcpy(m->fail[k].entry, e, 12), m->fail[k].why = why;
}

/* find or (re)create the object for ref in this task; a changed table entry replaces it (Apple: a reused ref is noticed) */
static lagfx_a64_obj_t *obj_resolve(lagfx_protocol_t *p, mgr2_t *m, struct lagfx_vk_state *vk, const lagfx_task_entry_t *t, uint32_t ref, bool allow_replace) {
    if (!ref || !t->lists_set || !t->objects_addr || (t->objects_len && ((uint64_t)ref + 1u) * 12u > t->objects_len)) return NULL;
    uint8_t eb[12];
    if (!lagfx_arm64_task_read(p, t, t->objects_addr + (uint64_t)ref * 12u, eb, 12)) return NULL;
    uint32_t e[3] = { lagfx_le32(eb), lagfx_le32(eb + 4), lagfx_le32(eb + 8) };
    uint32_t slot = m->n, reused = 0;
    for (uint32_t i = 0; i < m->n; i++)
        if (m->objs[i] && m->objs[i]->task == t->id && m->objs[i]->ref == ref) { slot = i; break; }
    if (slot < m->n) {
        lagfx_a64_obj_t *o = m->objs[slot];
        if (!memcmp(o->entry, e, 12)) return o;
        if (!allow_replace) { m->st.bad_object++; return NULL; }   /* commands may already refer to it in this exec */
        obj_release(m, vk, o);   /* deferred while an exec records / is in flight */
        m->objs[slot] = NULL;
    } else {
        /* A slot freed by a release (0x25 forget, task drop, a replaced entry whose new object failed) is a hole: reuse it. Without this the table only grew, and after
         * MAX_OBJS creations over a run every new object was refused (silently) although few were alive. */
        for (uint32_t i = 0; i < m->n; i++) if (!m->objs[i]) { slot = i; reused = 1; break; }
        if (slot == m->n && m->n >= MAX_OBJS) { m->st.obj_table_full++, OT(m).obj_full++; memo_fail(m, t->id, ref, e, TEXWHY_TABLE_FULL); return NULL; }
    }
    lagfx_a64_obj_t *o = obj_make(p, m, vk, t, ref, e);
    if (!o) { if (m->mk_why) memo_fail(m, t->id, ref, e, m->mk_why); return NULL; }   /* nothing remembered but the memo: a later exec retries (the entry may be filled in later) */
    if (slot == m->n) {
        if (m->n == m->cap) {
            uint32_t nc = m->cap ? m->cap * 2 : 64;
            lagfx_a64_obj_t **nn = realloc(m->objs, nc * sizeof *nn);
            if (!nn) { obj_free(vk, o); return NULL; }
            m->objs = nn, m->cap = nc;
        }
        m->n++;
    }
    m->objs[slot] = o;
    m->st.obj_slots = m->n;
    OT(m).obj_created++, OT(m).obj_live++, OT(m).obj_reused += reused;
    if (OT(m).obj_live > OT(m).obj_peak_live) OT(m).obj_peak_live = OT(m).obj_live;
    if (m->n > OT(m).obj_high_water) OT(m).obj_high_water = m->n;
    for (uint32_t i = 0; i < 128u && i < m->nfail; i++) if (m->fail[i].task == t->id && m->fail[i].ref == ref) m->fail[i].task = UINT32_MAX;   /* created now: the failure memo is obsolete */
    o->gen = ++m->st.created;
    return o;
}

lagfx_a64_obj_t *lagfx_a64_res_object(lagfx_a64_rexec_t *x, uint32_t ref) {
    if (!x) return NULL;
    lagfx_a64_obj_t *o = obj_resolve(x->p, x->m, x->vk, x->t, ref, false);
    if (o && !o_hv(o)) {   /* first use outside the resource list: take the guest's bytes (never over a stale mark) */
        bool ok = true;
        if (o->ms) {
            if (!o_stale(o)) { ok = ms_pull(x->p, x->m, x->cb, o->ms); if (ok) barrier(x->cb); }
        } else if (!o->guest_stale) {
            guest_upload(x->p, x->m, x->t, o);
            if (o->kind == LAGFX_A64_TEXTURE) tex_record(x->cb, o, false), barrier(x->cb);
        }
        if (ok) o_set_hv(o, true);   /* a surface that could not be read stays 'not valid': the next use tries again */
        if (o->linear) lin_pull_x(x, o);
    }
    return o;
}

lagfx_a64_rexec_t *lagfx_a64_res_begin(lagfx_protocol_t *p, uint32_t task, const uint8_t *res, uint32_t nres) {
    if (!lagfx_a64_res_enabled() || !lagfx_proto_arm64(p)) return NULL;
    struct lagfx_vk_state *vk = vk_of(p);
    lagfx_task_entry_t *t = lagfx_protocol_find_task(p, task);
    if (!vk || !t || !t->root_page_pfn || (nres && !res)) return NULL;
    mgr2_t *m = mgr_get(p, true);
    if (!m || m->cur) return NULL;   /* one exec at a time */
    if ((m->pending || m->sw_fence) && lagfx_a64_res_poll(p, 0) == LAGFX_A64_PENDING) { m->st.busy_skipped++; return NULL; }
    if (m->sw_fence) { m->st.busy_skipped++; return NULL; }   /* a swap copy whose fence could not be checked (FAILED): its images stay untouched, no new exec */
    dead_flush(m, vk);   /* GPU still on the last one: never touch its resources */
    lagfx_a64_rexec_t *x = calloc(1, sizeof *x);
    if (!x) return NULL;
    x->p = p, x->m = m, x->vk = vk, x->t = t;
    if (!cb_begin(vk, &x->cb)) { free(x); return NULL; }
    x->res = calloc(nres ? nres : 1, sizeof *x->res);
    if (!x->res) { lagfx_vk_cmdbuf_free(vk, x->cb); free(x); return NULL; }
    m->st.execs++;
    m->epoch++;
    x->gen = ++m->down_gen_seq;
    m->cur = x;
    m->quiet_kinds = true;
    /* pass 1: flag 1, upload where the host copy is not valid and the guest copy is not stale */
    for (uint32_t i = 0; i < nres; i++) {
        uint32_t ref = lagfx_le32(res + 8u * i), fl = lagfx_le32(res + 8u * i + 4);
        m->st.res++;
        lagfx_a64_obj_t *o = obj_resolve(p, m, vk, t, ref, true);
        if (!o) continue;
        x->res[x->nres].o = o, x->res[x->nres].flags = fl, x->nres++;
        if (fl & 1u) o_set_stale(o, false), o_set_hv(o, false);
        if (o->kind == LAGFX_A64_BUFFER) {   /* shared-mapping emulation: re-read every exec unless the GPU's result is newer */
            if (!o->guest_stale) o->host_valid = false; else m->st.stale_skips++;
        }
        if (!o_hv(o)) {
            bool ok = true;
            if (!o_stale(o)) {
                if (o->ms) ok = ms_pull(p, m, x->cb, o->ms);   /* unreadable surface (no descriptor yet ...): stays not valid, retried by the next exec */
                else {
                    guest_upload(p, m, t, o);
                    if (o->kind == LAGFX_A64_TEXTURE) tex_record(x->cb, o, false);
                }
            } else m->st.stale_skips++;
            if (ok) o_set_hv(o, true);
        }
    }
    m->quiet_kinds = false;
    for (uint32_t i = 0; i < x->nres; i++) if (x->res[i].o->linear) lin_pull_x(x, x->res[i].o);   /* shared memory: the image takes what the buffer holds now */
    barrier(x->cb);
    /* pass 2: flag 2 -> the guest's copy is out of date from here on */
    for (uint32_t i = 0; i < x->nres; i++) if (x->res[i].flags & 2u) o_set_stale(x->res[i].o, true);
    return x;
}

static lagfx_a64_obj_t *obj_find(mgr2_t *m, uint32_t task, uint32_t ref) {
    for (uint32_t i = 0; i < m->n; i++) if (m->objs[i] && m->objs[i]->task == task && m->objs[i]->ref == ref) return m->objs[i];
    return NULL;
}

/* ---- linear textures over a buffer (type 8/9, tags 9 / 0x37): an optimal-tiling VkImage whose bytes live in the backing buffer's guest-layout copy (offset,
 * row pitch). The image is pulled from the buffer before it is read (render Load, blit source, begin of an exec) and pushed back after it was written (render pass,
 * blit destination, flag 0x04), so Render, Blit and the guest pages (written through the BUFFER's pages) see one memory, as on Apple's shared mapping. ---- */

static lagfx_a64_obj_t *lin_backing(lagfx_protocol_t *p, mgr2_t *m, struct lagfx_vk_state *vk, const lagfx_task_entry_t *t, const lagfx_a64_obj_t *o, bool create) {
    lagfx_a64_obj_t *b = create ? obj_resolve(p, m, vk, t, o->lin_buf, false) : obj_find(m, o->task, o->lin_buf);
    return b && b->kind == LAGFX_A64_BUFFER ? b : NULL;
}

static bool lin_region(const lagfx_a64_obj_t *o, const lagfx_a64_obj_t *b, VkBufferImageCopy *r) {
    unsigned __int128 end = (unsigned __int128)o->lin_off + (unsigned __int128)o->lin_row * (o->height - 1) + (unsigned __int128)o->width * o->bpp;
    if (end > b->length) return false;
    memset(r, 0, sizeof *r);
    r->bufferOffset = o->lin_off, r->bufferRowLength = (uint32_t)(o->lin_row / o->bpp);
    r->imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r->imageExtent = (VkExtent3D){ o->width, o->height, 1 };
    return true;
}

/* to_image: buffer -> image (pull); else image -> buffer (push). false + counted if the backing buffer is missing / too small. */
static bool lin_xfer(VkCommandBuffer cb, lagfx_a64_obj_t *b, lagfx_a64_obj_t *o, mgr2_t *m, bool to_image) {
    VkBufferImageCopy r;
    if (!b || !lin_region(o, b, &r)) { m->st.linear_unsupported++; return false; }
    barrier(cb);
    lagfx_translate_blit_cmd_buffer_image(cb, b->buf, o->img, &r, to_image);
    if (!to_image) host_barrier(m, cb);   /* image -> guest-backed buffer: read by the host after the fence */
    barrier(cb);
    return true;
}
static bool lin_pull_x(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o) {
    if (!o->linear) return true;
    lagfx_a64_obj_t *b = lin_backing(x->p, x->m, x->vk, x->t, o, true);
    if (b && !b->host_valid) { if (!b->guest_stale) guest_upload(x->p, x->m, x->t, b); b->host_valid = true; }
    bool ok = lin_xfer(x->cb, b, o, x->m, true);
    if (ok) o->lin_pulled = true, o->lin_seen = b->wgen;
    return ok;
}
static bool lin_push_x(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o) {
    if (!o->linear) return true;
    lagfx_a64_obj_t *b = lin_backing(x->p, x->m, x->vk, x->t, o, true);
    bool ok = lin_xfer(x->cb, b, o, x->m, false);
    if (ok) { b->host_valid = true; b->wgen++, b->gpu_dirty = true; o->lin_pulled = true, o->lin_seen = b->wgen; }   /* the pushing image is the buffer's new content; another image over it is now stale */
    return ok;
}

/* ---- blit commands (validation against the mirrored objects; the Vulkan recording is src/translate/blit_encoder.c) ---- */

static uint64_t u64at(const uint8_t *r, uint32_t o) { return lagfx_le64(r + o); }

/* bytes a buffer<->image transfer touches; false if it does not fit `len` */
static bool xfer_fits(uint64_t off, uint64_t row, uint64_t img, uint64_t w, uint64_t h, uint64_t d, uint32_t bpp, uint64_t len) {
    unsigned __int128 end = (unsigned __int128)off + (unsigned __int128)(d - 1) * img + (unsigned __int128)(h - 1) * row + (unsigned __int128)w * bpp;
    return end <= len;
}

typedef struct { uint64_t x, y, z, w, h, d; uint32_t slice, level; } box_t;

static bool box_ok(const lagfx_a64_obj_t *t, const box_t *b) {
    if (t->ds_aspects) return false;   /* depth / stencil images take part in no copy */
    if (!b->w || !b->h || !b->d || b->w > 65536 || b->h > 65536 || b->d > 65536 || b->x > 65536 || b->y > 65536 || b->z > 65536) return false;
    if (b->level >= t->mips) return false;
    if (is_tex3d(t) ? b->slice != 0 : (b->slice >= t->layers || b->d != 1 || b->z != 0)) return false;
    return b->x + b->w <= lvl_dim(t->width, b->level) && b->y + b->h <= lvl_dim(t->height, b->level) && b->z + b->d <= lvl_dim(t->depth, b->level);
}

static void rd_box(const uint8_t *r, uint32_t org, uint32_t sz, uint32_t sl_off, uint32_t lv_off, box_t *b) {
    b->x = u64at(r, org), b->y = u64at(r, org + 8), b->z = u64at(r, org + 16);
    b->w = u64at(r, sz), b->h = u64at(r, sz + 8), b->d = u64at(r, sz + 16);
    b->slice = lagfx_le16(r + sl_off), b->level = lagfx_le16(r + lv_off);
}

static bool buf_tex_copy(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *b, lagfx_a64_obj_t *t, uint64_t off, uint64_t row, uint64_t img, const box_t *bx, bool to_tex) {
    if (b->kind != LAGFX_A64_BUFFER || t->kind != LAGFX_A64_TEXTURE || !box_ok(t, bx)) return false;
    if (!row || row % t->bpp || row / t->bpp < bx->w || off % t->bpp || row > (1ull << 31)) return false;
    if (bx->d > 1 && (img < row * bx->h || img % row)) return false;
    if (!xfer_fits(off, row, bx->d > 1 ? img : 0, bx->w, bx->h, bx->d, t->bpp, b->length)) return false;
    VkBufferImageCopy r = { .bufferOffset = off, .bufferRowLength = (uint32_t)(row / t->bpp), .bufferImageHeight = bx->d > 1 ? (uint32_t)(img / row) : 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, bx->level, bx->slice, 1 },
        .imageOffset = { (int32_t)bx->x, (int32_t)bx->y, (int32_t)bx->z }, .imageExtent = { (uint32_t)bx->w, (uint32_t)bx->h, (uint32_t)bx->d } };
    if (!to_tex && !lin_pull_x(x, t)) return false;   /* a linear texture is read: take the buffer's current bytes first */
    lagfx_translate_blit_cmd_buffer_image(x->cb, b->buf, t->img, &r, to_tex);
    if (!to_tex) host_barrier(x->m, x->cb), b->wgen++, b->gpu_dirty = true;   /* image -> buffer: the host may read the buffer (flag 4 / gathers) after the fence */
    if (to_tex) lin_push_x(x, t);                       /* ... written: back into the buffer (and so the guest pages of the buffer) */
    return true;
}

static bool blit_300_302(lagfx_a64_rexec_t *x, uint32_t tag, const uint8_t *r) {
    box_t bx;
    if (tag == 300) {   /* buffer to texture: {buf, tex, srcOff, srcRow, srcImage, size[3], origin[3], slice u16, level u16, options} */
        lagfx_a64_obj_t *b = lagfx_a64_res_object(x, lagfx_le32(r)), *t = lagfx_a64_res_object(x, lagfx_le32(r + 4));
        if (!b || !t) return false;
        rd_box(r, 0x38, 0x20, 0x50, 0x52, &bx);
        return buf_tex_copy(x, b, t, u64at(r, 8), u64at(r, 0x10), u64at(r, 0x18), &bx, true);
    }
    /* 302 texture to buffer: {tex, buf, origin[3], size[3], slice, level, dstOff, dstRow, dstImage} */
    lagfx_a64_obj_t *t = lagfx_a64_res_object(x, lagfx_le32(r)), *b = lagfx_a64_res_object(x, lagfx_le32(r + 4));
    if (!b || !t) return false;
    rd_box(r, 8, 0x20, 0x50, 0x52, &bx);
    return buf_tex_copy(x, b, t, u64at(r, 0x38), u64at(r, 0x40), u64at(r, 0x48), &bx, false);
}

static bool blit_301(lagfx_a64_rexec_t *x, const uint8_t *r) {
    lagfx_a64_obj_t *a = lagfx_a64_res_object(x, lagfx_le32(r)), *b = lagfx_a64_res_object(x, lagfx_le32(r + 4));
    uint64_t so = u64at(r, 8), dof = u64at(r, 0x10), n = u64at(r, 0x18);
    if (!a || !b || a->kind != LAGFX_A64_BUFFER || b->kind != LAGFX_A64_BUFFER) return false;
    if (so > a->length || n > a->length - so || dof > b->length || n > b->length - dof) return false;
    if (!n) return true;
    if (a == b && so < dof + n && dof < so + n) return false;   /* overlapping copy within one buffer */
    VkBufferCopy c = { so, dof, n };
    lagfx_translate_blit_cmd_buffer_buffer(x->cb, a->buf, b->buf, &c);
    b->wgen++, b->gpu_dirty = true;
    return true;
}

static bool blit_303(lagfx_a64_rexec_t *x, const uint8_t *r) {
    lagfx_a64_obj_t *a = lagfx_a64_res_object(x, lagfx_le32(r)), *b = lagfx_a64_res_object(x, lagfx_le32(r + 4));
    if (!a || !b || a->kind != LAGFX_A64_TEXTURE || b->kind != LAGFX_A64_TEXTURE || a->format != b->format || a->img_type != b->img_type) return false;
    box_t s, d;
    rd_box(r, 8, 0x20, 0x50, 0x52, &s);
    d.x = u64at(r, 0x38), d.y = u64at(r, 0x40), d.z = u64at(r, 0x48), d.w = s.w, d.h = s.h, d.d = s.d;
    d.slice = lagfx_le16(r + 0x54), d.level = lagfx_le16(r + 0x56);
    if (!box_ok(a, &s) || !box_ok(b, &d)) return false;
    if (a == b && s.slice == d.slice && s.level == d.level && s.x < d.x + d.w && d.x < s.x + s.w && s.y < d.y + d.h && d.y < s.y + s.h && s.z < d.z + d.d && d.z < s.z + s.d) return false;
    VkImageCopy c = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, s.level, s.slice, 1 }, .srcOffset = { (int32_t)s.x, (int32_t)s.y, (int32_t)s.z },
                      .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, d.level, d.slice, 1 }, .dstOffset = { (int32_t)d.x, (int32_t)d.y, (int32_t)d.z },
                      .extent = { (uint32_t)s.w, (uint32_t)s.h, (uint32_t)s.d } };
    if (!lin_pull_x(x, a)) return false;
    lagfx_translate_blit_cmd_image_image(x->cb, a->img, b->img, &c);
    lin_push_x(x, b);
    return true;
}

/* 306 fill {ref, location u64, length u64, value u8}: the 4-aligned middle by vkCmdFillBuffer, unaligned edges by a copy from a tiny staging buffer */
static bool blit_306(lagfx_a64_rexec_t *x, const uint8_t *r) {
    lagfx_a64_obj_t *b = lagfx_a64_res_object(x, lagfx_le32(r));
    uint64_t loc = u64at(r, 4), n = u64at(r, 12);
    uint8_t v = r[20];
    if (!b || b->kind != LAGFX_A64_BUFFER || loc > b->length || n > b->length - loc) return false;
    if (!n) return true;
    uint64_t end = loc + n, a0 = (loc + 3) & ~3ull, a1 = end & ~3ull;
    if (a0 >= a1) a0 = a1 = end;   /* no aligned middle: everything goes through the staging copy (n < 8 then) */
    uint64_t head = a0 - loc, tail = end - a1;
    b->wgen++, b->gpu_dirty = true;
    if (a0 < a1) lagfx_translate_blit_cmd_fill_buffer(x->cb, b->buf, a0, a1 - a0, v);
    if (head || tail) {
        if (x->ntmp >= MAX_TMP) return false;
        uint8_t *mp; VkBuffer sb; VkDeviceMemory sm;
        if (!make_buf(x->vk, 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &sb, &sm, &mp)) return false;
        memset(mp, v, 8);
        x->tmp_b[x->ntmp] = sb, x->tmp_m[x->ntmp] = sm, x->ntmp++;
        if (head) { VkBufferCopy c = { 0, loc, head }; lagfx_translate_blit_cmd_buffer_buffer(x->cb, sb, b->buf, &c); }
        if (tail) { VkBufferCopy c = { 0, a1, tail }; lagfx_translate_blit_cmd_buffer_buffer(x->cb, sb, b->buf, &c); }
    }
    return true;
}

/* 319 (0x13f) fillBuffer:range:pattern4: {ref, location u64, length u64, pattern u32}, same layout as 306 with a 32-bit pattern (reference derivation:
 * pvg-vulkan-wire ops/blit.rs). The pattern repeats from the range start in memory (little endian) order; the location must be a multiple of 4. The 4-aligned
 * middle goes through vkCmdFillBuffer, a 1..3 byte tail from the first bytes of the pattern through a tiny staging copy. */
static bool blit_319(lagfx_a64_rexec_t *x, const uint8_t *r) {
    lagfx_a64_obj_t *b = lagfx_a64_res_object(x, lagfx_le32(r));
    uint64_t loc = u64at(r, 4), n = u64at(r, 12);
    uint32_t pat = lagfx_le32(r + 20);
    if (!b || b->kind != LAGFX_A64_BUFFER || loc % 4u || loc > b->length || n > b->length - loc) return false;
    if (!n) return true;
    uint64_t mid = n & ~3ull, tail = n & 3ull;
    b->wgen++, b->gpu_dirty = true;
    if (mid) vkCmdFillBuffer(x->cb, b->buf, loc, mid, pat);
    if (tail) {
        if (x->ntmp >= MAX_TMP) return false;
        uint8_t *mp; VkBuffer sb; VkDeviceMemory sm;
        if (!make_buf(x->vk, 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &sb, &sm, &mp)) return false;
        for (int i = 0; i < 4; i++) mp[i] = (uint8_t)(pat >> (8 * i));
        x->tmp_b[x->ntmp] = sb, x->tmp_m[x->ntmp] = sm, x->ntmp++;
        VkBufferCopy c = { 0, loc + mid, tail };
        lagfx_translate_blit_cmd_buffer_buffer(x->cb, sb, b->buf, &c);
    }
    return true;
}

/* Entry from src/handlers/compute/blit_inner_ops.c (arm64 mode only): opcode == the wire tag (0x12c == 300 ...), payload = after the 8-byte header.
 * Returns 1 if the command was executed (recorded), 0 if rejected. */
int lagfx_a64_blit_op(lagfx_protocol_t *p, uint32_t tag, const uint8_t *r, size_t pl) {
    mgr2_t *m = p ? p->a64_res : NULL;
    lagfx_a64_rexec_t *x = m ? m->cur : NULL;
    if (!x) return 0;
    uint32_t need = 0;
    switch (tag) {
    case 300: case 303: need = 0x58; break;
    case 302: need = 0x56; break;
    case 301: need = 0x20; break;
    case 306: need = 21; break;
    case 319: need = 24; break;
    default: return 0;
    }
    bool ok = false;
    if (pl >= need) {
        switch (tag) {
        case 300: case 302: ok = blit_300_302(x, tag, r); break;
        case 301: ok = blit_301(x, r); break;
        case 303: ok = blit_303(x, r); break;
        case 306: ok = blit_306(x, r); break;
        case 319: ok = blit_319(x, r); break;
        }
    }
    if (ok) { m->st.blit_done++; if (tag - 300u < 16u) m->st.blit_tag[tag - 300]++; barrier(x->cb); }
    else m->st.blit_rejected++;
    return ok;
}

/* Walks the commands of a Blit segment body and sends each through the existing blit dispatch table (lagfx_blit_inner_dispatch; tag == opcode). */
void lagfx_a64_res_blit_segment(lagfx_a64_rexec_t *x, const uint8_t *body, uint32_t len) {
    if (!x || !body) return;
    mgr2_t *m = x->m;
    uint32_t q = 0, n = 0;
    while (q + 8 <= len && n++ < LAGFX_EXEC_MAX_INNER) {
        uint32_t tag = lagfx_le32(body + q), sz = lagfx_le32(body + q + 4);
        if (sz < 8 || sz > len - q) break;
        m->st.blit_seen++;
        if (tag == 300 || tag == 301 || tag == 302 || tag == 303 || tag == 306) lagfx_blit_inner_dispatch(x->p, tag, body + q + 8, sz - 8);
        else if (tag == 319) lagfx_a64_blit_op(x->p, tag, body + q + 8, sz - 8);               /* fill buffer with a 32-bit pattern: no x86-era table entry, executed directly */
        else if ((tag >= 308 && tag <= 311) || tag == 314 || tag == 315) m->st.blit_noop++;   /* hints: Apple does nothing either */
        else if (tag == 316 || tag == 317 || tag == 322 || tag == 323) m->st.blit_noop++;    /* blit fences (one in-order command buffer) and invalidate-compressed-texture (compressed textures are not mirrored) */
        else m->st.blit_unsupported++;                                                         /* 307 (generate mipmaps) and unknown tags */
        q += sz;
    }
}

/* A host-visible storage buffer that lives as long as this exec (freed with it); false when the exec's scratch slots are used up or the allocation fails. */
bool lagfx_a64_res_scratch(lagfx_a64_rexec_t *x, VkDeviceSize size, VkBuffer *buf, uint8_t **map) {
    if (!x || !size || x->ntmp >= MAX_TMP) return false;
    VkBuffer b; VkDeviceMemory m; uint8_t *mp;
    if (!make_buf(x->vk, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &b, &m, &mp)) return false;
    x->tmp_b[x->ntmp] = b, x->tmp_m[x->ntmp] = m, x->ntmp++;
    *buf = b, *map = mp;
    return true;
}

/* ---- render replay capture snapshots (see arm64_resources.h) ---- */

void lagfx_a64_res_set_finish_hook(lagfx_a64_rexec_t *x, void (*fn)(void *, bool), void *ctx) { if (x) x->hook = fn, x->hook_ctx = ctx; }

static bool snap_alloc(lagfx_a64_rexec_t *x, VkDeviceSize size, VkBuffer *b, uint8_t **map) {
    if (x->nsnap == x->capsnap) {
        uint32_t nc = x->capsnap ? x->capsnap * 2u : 32u;
        void *n = realloc(x->snap, nc * sizeof *x->snap);
        if (!n) return false;
        x->snap = n, x->capsnap = nc;
    }
    VkDeviceMemory m;
    if (!make_buf(x->vk, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, b, &m, map)) return false;
    x->snap[x->nsnap].b = *b, x->snap[x->nsnap].m = m, x->nsnap++;
    return true;
}

bool lagfx_a64_res_snap_buffer(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o, uint64_t off, uint64_t max_bytes, lagfx_a64_snap_t *s) {
    if (!x || !o || !s || o->kind != LAGFX_A64_BUFFER || !o->buf || off >= o->length || !max_bytes) return false;
    uint64_t len = o->length - off < max_bytes ? o->length - off : max_bytes;
    VkBuffer b; uint8_t *mp;
    if (!snap_alloc(x, len, &b, &mp)) return false;
    barrier(x->cb);   /* earlier GPU writes (draws' storage writes, blit fills, transfers) -> this copy */
    VkBufferCopy r = { off, 0, len };
    vkCmdCopyBuffer(x->cb, o->buf, b, 1, &r);
    host_barrier(x->m, x->cb);
    barrier(x->cb);
    s->data = mp, s->bytes = len, s->full = o->length, s->off = off;
    return true;
}

bool lagfx_a64_res_snap_texture(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o, bool tight, uint64_t max_bytes, lagfx_a64_snap_t *s) {
    if (!x || !o || !s || o->kind != LAGFX_A64_TEXTURE || o->ds_aspects || !o->img || !max_bytes) return false;
    if (!tight && (!o->has_pages || !o->length)) return false;
    uint64_t full = tight ? (uint64_t)o->width * o->height * (is_tex3d(o) ? o->depth : 1u) * o->bpp : o->length;
    uint64_t sz = full < max_bytes ? full : max_bytes;
    if (!sz) return false;
    VkBuffer b; uint8_t *mp;
    if (!snap_alloc(x, sz, &b, &mp)) return false;
    barrier(x->cb);
    if (tight) {
        if (sz == full) {
            VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { o->width, o->height, is_tex3d(o) ? o->depth : 1u } };
            lagfx_translate_blit_cmd_buffer_image(x->cb, b, o->img, &r, false);
        } else {   /* cut by the cap: whole rows only (rows that fit in the prefix) */
            uint64_t rows = sz / ((uint64_t)o->width * o->bpp);
            if (rows > o->height) rows = o->height;
            if (rows) {
                VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { o->width, (uint32_t)rows, 1 } };
                lagfx_translate_blit_cmd_buffer_image(x->cb, b, o->img, &r, false);
            }
        }
    } else {
        uint32_t layers = is_tex3d(o) ? 1u : o->layers;
        for (uint32_t sl = 0; sl < layers; sl++)
            for (uint32_t l = 0; l < o->mips; l++) {
                VkBufferImageCopy r;
                if (!tex_region(o, sl, l, &r)) continue;
                uint64_t end = r.bufferOffset + (uint64_t)o->mip[l].row * o->mip[l].h * o->mip[l].d;
                if (end > sz) continue;   /* beyond the cap: the prefix keeps only regions that fit completely */
                lagfx_translate_blit_cmd_buffer_image(x->cb, b, o->img, &r, false);
            }
    }
    host_barrier(x->m, x->cb);
    barrier(x->cb);
    s->data = mp, s->bytes = sz, s->full = full, s->off = 0;
    return true;
}

/* ---- completion ---- */

static void exec_hook(lagfx_a64_rexec_t *x, bool done) {
    if (!x->hook) return;
    void (*fn)(void *, bool) = x->hook;
    x->hook = NULL;
    fn(x->hook_ctx, done);
}

/* The exec's GPU work will never be read back (submit failed / fence failed): the staging copies it recorded are not pending any more, so a later exec cannot write their
 * stale bytes to the guest. Only surfaces whose pending copy belongs to THIS exec (down_gen) are cleared. */
static void exec_clear_down(lagfx_a64_rexec_t *x) {
    for (uint32_t i = 0; i < x->nres; i++) {
        msurf_t *ms = x->res[i].o ? x->res[i].o->ms : NULL;
        if (ms && ms->down_pending && ms->down_gen == x->gen) { ms->down_pending = false; MRUN(x->p).wb_down_pending_cleared_on_fail++; }
    }
}

static void exec_free(lagfx_a64_rexec_t *x) {
    exec_hook(x, false);
    for (uint32_t i = 0; i < x->nsnap; i++) free_buf(x->vk, x->snap[i].b, x->snap[i].m);
    free(x->snap);
    for (uint32_t i = 0; i < x->ntmp; i++) free_buf(x->vk, x->tmp_b[i], x->tmp_m[i]);
    if (x->fence) vkDestroyFence(x->vk->device, x->fence, NULL);
    if (x->cb) lagfx_vk_cmdbuf_free(x->vk, x->cb);
    free(x->res);
    free(x);
}

/* The GPU is done with x: bring flag-4 resources back to the guest and release x. */
static lagfx_a64_status_t exec_finish(lagfx_a64_rexec_t *x) {
    const lagfx_task_entry_t *t = lagfx_protocol_find_task(x->p, x->t->id);   /* the task may have been torn down while the GPU worked: then there is nowhere to write */
    for (uint32_t i = 0; i < x->nres; i++) {
        lagfx_a64_obj_t *o = x->res[i].o;
        if (o->ms && (x->res[i].flags & 2u) && mapped_wb_on_swap() && !(x->res[i].flags & 4u)) { if (o->ms->host_valid) o->ms->owed = true; continue; }   /* the GPU is done: the surface is now owed to the guest, paid at the next display command */
        if (o->ms && (x->res[i].flags & 2u) && mapped_wb_on_render()) { ms_finish_down(x->p, o->ms, x->gen); continue; }
        if (!(x->res[i].flags & 4u)) continue;
        if (o->ms) { ms_finish_down(x->p, o->ms, x->gen); continue; }   /* a surface's pixels go through the shell, not through the task */
        if (t && t->root_page_pfn && t == x->t) {
            if (o->linear) { lagfx_a64_obj_t *b = obj_find(x->m, o->task, o->lin_buf); if (b) guest_download(x->p, x->m, t, b), b->guest_stale = false; }   /* a linear texture's bytes are its buffer's pages */
            else guest_download(x->p, x->m, t, o);
        }
        o->guest_stale = false;
    }
    mgr2_t *m = x->m;
    struct lagfx_vk_state *vk = x->vk;
    exec_hook(x, true);   /* the GPU is done: capture snapshots are readable now */
    if (m->pending == x) m->pending = NULL;
    exec_free(x);
    dead_flush(m, vk);   /* the fence signalled: objects replaced / dropped meanwhile can go now */
    return LAGFX_A64_DONE;
}

/* fence state without blocking beyond wait_ms (0 = status poll only) */
static lagfx_vk_wait_result_t fence_wait(struct lagfx_vk_state *vk, VkFence f, uint32_t wait_ms) {
    if (wait_ms) return lagfx_vk_wait_bounded(vk, f, (uint64_t)wait_ms * 1000000ull);
    VkResult r = lagfx_vk_fence_status_raw(vk, f, 0);
    if (r == VK_SUCCESS) return LAGFX_VK_WAIT_OK;
    return r == VK_NOT_READY || r == VK_TIMEOUT ? LAGFX_VK_WAIT_TIMEOUT : LAGFX_VK_WAIT_FAILED;
}

/* tests only: the mapped surface's pending-copy state (-1 unknown mapping, else down_pending) / mark it pending under a generation no exec ever had */
int lagfx_a64_test_ms_down_pending(lagfx_protocol_t *p, uint32_t mapping) {
    mgr2_t *m = p ? p->a64_res : NULL;
    for (uint32_t i = 0; m && i < m->nms; i++) if (m->ms[i] && m->ms[i]->mapping == mapping) return m->ms[i]->down_pending;
    return -1;
}
void lagfx_a64_test_ms_force_pending(lagfx_protocol_t *p, uint32_t mapping) {
    mgr2_t *m = p ? p->a64_res : NULL;
    for (uint32_t i = 0; m && i < m->nms; i++) if (m->ms[i] && m->ms[i]->mapping == mapping) m->ms[i]->down_pending = true, m->ms[i]->down_gen = ~0ull;
}

lagfx_a64_status_t lagfx_a64_res_end(lagfx_a64_rexec_t *x) {
    if (!x) return LAGFX_A64_FAILED;
    mgr2_t *m = x->m;
    bool host_read = x->nsnap > 0;
    for (uint32_t i = 0; i < x->nres; i++) {   /* flag 4: bring the GPU's result back after this exec */
        lagfx_a64_obj_t *o = x->res[i].o;
        if ((x->res[i].flags & 4u) && o->kind == LAGFX_A64_TEXTURE && o->has_pages) tex_record(x->cb, o, true), host_read = true;
        if ((x->res[i].flags & 4u) && o->linear) lin_push_x(x, o), host_read = true;
        if ((x->res[i].flags & 4u) && o->ms) ms_record_down(m, x->cb, o->ms, x->gen);
        else if ((x->res[i].flags & 2u) && o->ms && mapped_wb_on_render()) ms_record_down(m, x->cb, o->ms, x->gen);
    }
    barrier(x->cb);
    if (host_read) host_barrier(m, x->cb);   /* every image / buffer copy of this exec the host reads after the fence (tex_record down, linear push, capture snapshots) */
    m->cur = NULL;   /* recording is over */
    bool sub_ok = vkEndCommandBuffer(x->cb) == VK_SUCCESS && lagfx_a64_test_hook != 3 && submit_fence(x->vk, m, x->cb, &x->fence);
    if (lagfx_a64_test_hook == 3) lagfx_a64_test_hook = 0;
    if (!sub_ok) {   /* failed BEFORE submit: nothing in flight */
        exec_clear_down(x);
        m->st.gpu_fail++;
        LAGFX_WARN("arm64 exec: could not submit; guest copies of this exec's resources are not updated (the exec still completes)");
        exec_free(x);
        return LAGFX_A64_FAILED;
    }
    lagfx_vk_wait_result_t r = lagfx_vk_wait_bounded(x->vk, x->fence, 0);   /* env LAGFX_GPU_WAIT_MS, default 2000 */
    if (lagfx_a64_test_hook == 5) lagfx_a64_test_hook = 0, r = LAGFX_VK_WAIT_FAILED;
    else if (lagfx_a64_test_hook == 7) lagfx_a64_test_hook = 10, r = LAGFX_VK_WAIT_TIMEOUT;   /* 10 = hold: the pending exec keeps reporting a timeout until the test resets the hook */
    if (r == LAGFX_VK_WAIT_OK) return exec_finish(x);
    if (r == LAGFX_VK_WAIT_TIMEOUT) {   /* NOT finished, NOT failed: keep everything alive, the caller polls */
        m->st.gpu_timeout++;
        m->pending = x;
        return LAGFX_A64_PENDING;
    }
    /* DEVICE_LOST etc.: counted failure; the work's state is unknown so its resources are leaked, not freed */
    m->st.gpu_fail++;
    LAGFX_WARN("arm64 exec: fence wait failed (device lost?); resources leaked");
    exec_clear_down(x);
    x->cb = VK_NULL_HANDLE, x->fence = VK_NULL_HANDLE, x->ntmp = 0;
    exec_hook(x, false); x->nsnap = 0;   /* snapshots leaked with the rest */
    free(x->res); free(x);
    return LAGFX_A64_FAILED;
}

static bool pending_wait(struct lagfx_vk_state *vk, mgr2_t *m) { return lagfx_a64_test_hook != 10 && m->pending->fence && lagfx_vk_wait_bounded(vk, m->pending->fence, 0) == LAGFX_VK_WAIT_OK; }

static lagfx_a64_status_t swap_poll(lagfx_protocol_t *p, mgr2_t *m, uint32_t wait_ms);
lagfx_a64_status_t lagfx_a64_res_poll(lagfx_protocol_t *p, uint32_t wait_ms) {
    mgr2_t *m = p ? p->a64_res : NULL;
    lagfx_a64_rexec_t *x = m ? m->pending : NULL;
    if (m && !x && m->sw_fence) return swap_poll(p, m, wait_ms);
    if (!x) return LAGFX_A64_DONE;
    lagfx_vk_wait_result_t r = fence_wait(x->vk, x->fence, wait_ms);
    if (lagfx_a64_test_hook == 10) r = LAGFX_VK_WAIT_TIMEOUT;
    if (lagfx_a64_test_hook == 6) lagfx_a64_test_hook = 0, r = LAGFX_VK_WAIT_FAILED;
    if (r == LAGFX_VK_WAIT_OK) return exec_finish(x);
    if (r == LAGFX_VK_WAIT_TIMEOUT) return LAGFX_A64_PENDING;
    m->st.gpu_fail++;
    m->pending = NULL;
    exec_clear_down(x);
    x->cb = VK_NULL_HANDLE, x->fence = VK_NULL_HANDLE, x->ntmp = 0;   /* leaked */
    exec_hook(x, false); x->nsnap = 0;
    free(x->res); free(x);
    return LAGFX_A64_FAILED;
}

bool lagfx_a64_res_is_pending(lagfx_protocol_t *p) { mgr2_t *m = p ? p->a64_res : NULL; return m && (m->pending || m->sw_fence); }

uint32_t lagfx_a64_res_sync_to_guest(lagfx_protocol_t *p, uint32_t task, const uint32_t *refs, uint32_t n) {
    mgr2_t *m = p ? p->a64_res : NULL;
    struct lagfx_vk_state *vk = vk_of(p);
    lagfx_task_entry_t *t = p ? lagfx_protocol_find_task(p, task) : NULL;
    if (!m || !vk || !t || !refs || BUSY(m)) return 0;
    VkCommandBuffer cb;
    if (!cb_begin(vk, &cb)) return 0;
    lagfx_a64_obj_t *sel[64];
    uint32_t ns = 0;
    m->epoch++;
    uint64_t gen = ++m->down_gen_seq;
    for (uint32_t i = 0; i < n && ns < 64; i++)
        for (uint32_t k = 0; k < m->n; k++) {
            lagfx_a64_obj_t *o = m->objs[k];
            if (o && o->task == task && o->ref == refs[i] && o->ms) { if (ms_record_down(m, cb, o->ms, gen)) sel[ns++] = o; break; }
            if (o && o->task == task && o->ref == refs[i] && o->host_valid && o->linear) {
                lagfx_a64_obj_t *b = obj_find(m, task, o->lin_buf);
                if (b && b->has_pages && lin_xfer(cb, b, o, m, false)) sel[ns++] = b;
                break;
            }
            if (o && o->task == task && o->ref == refs[i] && o->host_valid && o->has_pages) { sel[ns++] = o; if (o->kind == LAGFX_A64_TEXTURE) tex_record(cb, o, true); break; }
        }
    barrier(cb);
    if (ns) host_barrier(m, cb);   /* tex_record down / linear push copies (ms surfaces carry their own) */
    bool ok = vkEndCommandBuffer(cb) == VK_SUCCESS && lagfx_a64_test_hook != 8 && submit_wait(vk, m, &cb) == LAGFX_A64_DONE;   /* pending/failed: nothing is written to the guest */
    if (lagfx_a64_test_hook == 8) lagfx_a64_test_hook = 0;
    else if (lagfx_a64_test_hook == 9) lagfx_a64_test_hook = 0, ok = false;
    if (cb) lagfx_vk_cmdbuf_free(vk, cb);
    if (!ok) {   /* the recorded copies will not be read: not pending (a timed-out one is parked by submit_wait only for the fence, its staging bytes are never written) */
        for (uint32_t i = 0; i < ns; i++) if (sel[i]->ms && sel[i]->ms->down_pending && sel[i]->ms->down_gen == gen) { sel[i]->ms->down_pending = false; MRUN(p).wb_down_pending_cleared_on_fail++; }
        return 0;
    }
    for (uint32_t i = 0; i < ns; i++) { if (sel[i]->ms) ms_finish_down(p, sel[i]->ms, gen); else guest_download(p, m, t, sel[i]), sel[i]->guest_stale = false; }
    return ns;
}


/* The swap copy's fence signalled: pay the surfaces it carried. Only a surface actually written leaves the debt (swap_flushed); a re-mapped one is dropped
 * (swap_dropped); an unreadable one stays owed (swap_failed) for the next display command. */
static void swap_done(lagfx_protocol_t *p, mgr2_t *m, struct lagfx_vk_state *vk) {
    for (uint32_t i = 0; i < m->sw_n; i++) {
        msurf_t *ms = m->sw_sel[i];
        uint64_t sk = MRUN(p).mapped_pages_skipped;
        int r = ms_finish_down(p, ms, m->sw_gen);
        if (r > 0) { ms->owed = false, MRUN(p).swap_flushed++; if (MRUN(p).mapped_pages_skipped != sk) MRUN(p).swap_partial++; }   /* absent guest pages cannot be written now; counted, not retried */
        else if (r < 0) ms->owed = false, ms->host_valid = false, ms->guest_stale = false, ms->id_set = false, MRUN(p).swap_dropped++;   /* the pixels belong to a mapping that is gone: the image is not a copy of anything */
        else MRUN(p).swap_failed++;
    }
    vkDestroyFence(vk->device, m->sw_fence, NULL);
    lagfx_vk_cmdbuf_free(vk, m->sw_cb);
    m->sw_fence = VK_NULL_HANDLE, m->sw_cb = VK_NULL_HANDLE, m->sw_n = 0;
    dead_flush(m, vk);
}
static lagfx_a64_status_t swap_poll(lagfx_protocol_t *p, mgr2_t *m, uint32_t wait_ms) {
    struct lagfx_vk_state *vk = vk_of(p);
    if (!m->sw_fence || !vk) return LAGFX_A64_DONE;
    if (lagfx_a64_test_hook == 2) { lagfx_a64_test_hook = 0; return LAGFX_A64_PENDING; }
    lagfx_vk_wait_result_t r = fence_wait(vk, m->sw_fence, wait_ms);
    if (r == LAGFX_VK_WAIT_OK) { swap_done(p, m, vk); return LAGFX_A64_DONE; }
    return r == LAGFX_VK_WAIT_TIMEOUT ? LAGFX_A64_PENDING : LAGFX_A64_FAILED;   /* FAILED: device lost; everything stays parked (leaked) */
}

/* A display command is about to reach the shell: write every owed mapped surface to the guest first (one copy per surface, however many execs drew into it).
 * Returns the number written now. An exec in flight is waited for (bounded, LAGFX_GPU_WAIT_MS); if the GPU is still not done the surfaces stay owed
 * (swap_deferred) and the next display command pays them: the command itself is still forwarded (the shell owns it), so a late frame shows one swap later. A swap copy
 * that times out is parked in the manager (sw_*) like a pending exec: nothing it touches is freed or reused until its fence signalled. */
uint32_t lagfx_a64_res_display_swap(lagfx_protocol_t *p) {
    mgr2_t *m = p ? p->a64_res : NULL;
    if (!m || !mapped_wb_on_swap()) return 0;
    MRUN(p).swap_events++;
    struct lagfx_vk_state *vk = vk_of(p);
    if (!vk || m->cur) { for (uint32_t i = 0; i < m->nms; i++) if (m->ms[i] && m->ms[i]->owed) MRUN(p).swap_deferred++; return 0; }
    if (m->pending || m->sw_fence) lagfx_a64_res_poll(p, (uint32_t)(wait_ns() / 1000000ull));   /* bounded (LAGFX_GPU_WAIT_MS); 0 would be a status poll only */
    uint32_t nowed = 0;
    for (uint32_t i = 0; i < m->nms; i++) if (m->ms[i] && m->ms[i]->owed) nowed++;
    if (!nowed) return 0;
    if (m->pending || m->sw_fence) { MRUN(p).swap_deferred += nowed; return 0; }
    VkCommandBuffer cb;
    if (!cb_begin(vk, &cb)) { MRUN(p).swap_deferred += nowed; return 0; }
    msurf_t *sel[MAX_MS];
    uint32_t ns = 0;
    m->epoch++;
    uint64_t gen = ++m->down_gen_seq;
    for (uint32_t i = 0; i < m->nms && ns < MAX_MS; i++) {
        msurf_t *ms = m->ms[i];
        if (!ms || !ms->owed) continue;
        if (ms_record_down(m, cb, ms, gen)) sel[ns++] = ms; else ms->owed = false;   /* nothing meaningful on the GPU: not owed */
    }
    barrier(cb);
    VkFence f = VK_NULL_HANDLE;
    bool sub = vkEndCommandBuffer(cb) == VK_SUCCESS && lagfx_a64_test_hook != 1 && submit_fence(vk, m, cb, &f);
    if (lagfx_a64_test_hook == 1) lagfx_a64_test_hook = 0;
    if (!sub) { for (uint32_t i = 0; i < ns; i++) { if (sel[i]->down_pending && sel[i]->down_gen == gen) MRUN(p).wb_down_pending_cleared_on_fail++; sel[i]->down_pending = false; } lagfx_vk_cmdbuf_free(vk, cb); MRUN(p).swap_deferred += ns; return 0; }
    memcpy(m->sw_sel, sel, ns * sizeof *sel);
    m->sw_n = ns, m->sw_cb = cb, m->sw_fence = f, m->sw_gen = gen;
    uint64_t f0 = MRUN(p).swap_flushed;
    swap_poll(p, m, (uint32_t)(wait_ns() / 1000000ull));   /* done: paid now; timeout: stays parked (sw_fence set) and is finished by the next poll / swap */
    if (m->sw_fence) MRUN(p).swap_deferred += ns;
    return (uint32_t)(MRUN(p).swap_flushed - f0);
}

uint32_t lagfx_a64_res_display_note(lagfx_protocol_t *p, uint64_t mapping) {
    mgr2_t *m = p ? p->a64_res : NULL;
    if (!m || mapping > 0xffffffffull) return 0;
    for (uint32_t i = m->nms; i-- > 0;) {
        const msurf_t *ms = m->ms[i];
        if (!ms || ms->mapping != (uint32_t)mapping) continue;
        uint32_t r = LAGFX_A64_DSP_RESOLVED;
        if (ms->host_valid) { r |= LAGFX_A64_DSP_GPU; if (!ms->guest_stale) r |= LAGFX_A64_DSP_WRITTEN; }
        return r;
    }
    return 0;
}

/* Diagnostics only (no side effects): size / fourcc / Metal format number / VkFormat number of the mirror surface a swap names. */
/* The task that created the mapped-texture object bound to display mapping `mapping` (the first, i.e. earliest allocated, object holding that surface). */
bool lagfx_a64_res_display_owner(lagfx_protocol_t *p, uint64_t mapping, uint32_t *task) {
    mgr2_t *m = p ? p->a64_res : NULL;
    if (!m || !task || mapping > 0xffffffffull) return false;
    for (uint32_t i = 0; i < m->n; i++) {
        const lagfx_a64_obj_t *o = m->objs[i];
        if (o && o->ms && o->ms->mapping == (uint32_t)mapping) { *task = o->task; return true; }
    }
    return false;
}

bool lagfx_a64_res_display_info(lagfx_protocol_t *p, uint64_t mapping, uint32_t *w, uint32_t *h, uint32_t *fourcc, uint32_t *mtl_format, int32_t *vk_format) {
    mgr2_t *m = p ? p->a64_res : NULL;
    if (!m || mapping > 0xffffffffull) return false;
    for (uint32_t i = m->nms; i-- > 0;) {
        const msurf_t *ms = m->ms[i];
        if (!ms || ms->mapping != (uint32_t)mapping) continue;
        if (w) *w = ms->w;
        if (h) *h = ms->h;
        if (fourcc) *fourcc = ms->fourcc;
        if (mtl_format) *mtl_format = ms->mtl_fmt;
        if (vk_format) *vk_format = (int32_t)ms->format;
        return true;
    }
    return false;
}

/* ---- render targets and readback ---- */

static VkImageView tex_view(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o, VkImageUsageFlags need) {
    if (!x || !o || o->kind != LAGFX_A64_TEXTURE || is_tex3d(o) || !(o->usage & need)) return VK_NULL_HANDLE;
    if (!o->view) {
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = o->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = o->format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        if (vkCreateImageView(x->vk->device, &vi, NULL, &o->view) != VK_SUCCESS) { o->view = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
    }
    return o->view;
}

bool lagfx_a64_res_is_3d(const lagfx_a64_obj_t *o) { return o && o->kind == LAGFX_A64_TEXTURE && is_tex3d(o); }

/* The whole level 0 of a 3D mirror texture as a 3D view. Level 0 only, as the 2D view: the sampler's LOD range is clamped to it by the exec (a sampler with a mip
 * filter samples the base level). The A8Unorm component swizzle of the 2D path applies here too. */
VkImageView lagfx_a64_res_sample_view3d(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o) {
    if (!x || !o || o->kind != LAGFX_A64_TEXTURE || !is_tex3d(o) || o->ds_aspects || !(o->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) return VK_NULL_HANDLE;
    if (!o->view3) {
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = o->img, .viewType = VK_IMAGE_VIEW_TYPE_3D, .format = o->format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        if (o->mtl_format == 1u) vi.components = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R };
        if (vkCreateImageView(x->vk->device, &vi, NULL, &o->view3) != VK_SUCCESS) { o->view3 = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
    }
    return o->view3;
}

VkImageView lagfx_a64_res_sample_view(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o) {
    VkImageView v = tex_view(x, o, VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!v || o->mtl_format != 1u || o->ms) return v;
    /* MTLPixelFormatA8Unorm (1) samples as (0, 0, 0, a) (Metal's documented read of an alpha-only format); the image is R8_UNORM, so the view moves red into alpha.
     * From Metal's documentation, not checked against a device. */
    if (!o->sview) {
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = o->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = o->format,
            .components = { VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R }, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        if (vkCreateImageView(x->vk->device, &vi, NULL, &o->sview) != VK_SUCCESS) { o->sview = VK_NULL_HANDLE; return v; }
    }
    return o->sview;
}

/* The same whole-image view, for a storage-image (shader write) descriptor: needs the image created with STORAGE usage (formats without storage support get none). */
VkImageView lagfx_a64_res_storage_view(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o) { return tex_view(x, o, VK_IMAGE_USAGE_STORAGE_BIT); }

bool lagfx_a64_res_target_begin(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o, VkImageView *view, uint32_t *w, uint32_t *h) {
    if (!x || !o || o->kind != LAGFX_A64_TEXTURE || is_tex3d(o) || !(o->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return false;
    if (o->linear && !lin_pull_x(x, o)) return false;   /* Load / blend reads the buffer's bytes: counted linear_unsupported when the backing buffer is unusable */
    if (!o->view) {
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = o->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = o->format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        if (vkCreateImageView(x->vk->device, &vi, NULL, &o->view) != VK_SUCCESS) { o->view = VK_NULL_HANDLE; return false; }
    }
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = x->vk->have_local_read ? (VkImageLayout)1000232000 /* RENDERING_LOCAL_READ: attachment AND input-attachment readable */ : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = o->img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS } };
    vkCmdPipelineBarrier(x->cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    *view = o->view, *w = o->width, *h = o->height;
    return true;
}

void lagfx_a64_res_target_end(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o) {
    if (!x || !o) return;
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = x->vk->have_local_read ? (VkImageLayout)1000232000 : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = o->img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS } };
    vkCmdPipelineBarrier(x->cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    if (o->linear) lin_push_x(x, o);   /* the render result goes back into the buffer */
}

/* Depth / stencil attachment of a mirrored texture. The image stays in GENERAL (a layout dynamic rendering accepts for these attachments), so begin / end only
 * order the accesses. The view covers every aspect of the format. */
bool lagfx_a64_res_ds_begin(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o, VkImageView *view, uint32_t *w, uint32_t *h) {
    if (!x || !o || o->kind != LAGFX_A64_TEXTURE || !o->ds_aspects || !(o->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) return false;
    if (!o->view) {
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = o->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = o->format,
            .subresourceRange = { ds_aspect_mask(o), 0, 1, 0, 1 } };
        if (vkCreateImageView(x->vk->device, &vi, NULL, &o->view) != VK_SUCCESS) { o->view = VK_NULL_HANDLE; return false; }
    }
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = o->img, .subresourceRange = { ds_aspect_mask(o), 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(x->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    *view = o->view, *w = o->width, *h = o->height;
    return true;
}

void lagfx_a64_res_ds_end(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o) {
    if (!x || !o || !o->ds_aspects) return;
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = o->img, .subresourceRange = { ds_aspect_mask(o), 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(x->cb, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
}

bool lagfx_a64_res_readback(lagfx_protocol_t *p, uint32_t task, uint32_t ref, void *dst, size_t dst_size, uint32_t *w, uint32_t *h) {
    mgr2_t *m = p ? p->a64_res : NULL;
    struct lagfx_vk_state *vk = vk_of(p);
    lagfx_a64_obj_t *o = m ? obj_find(m, task, ref) : NULL;
    if (!vk || !o || o->kind != LAGFX_A64_TEXTURE || is_tex3d(o) || o->ds_aspects || BUSY(m)) return false;
    size_t need = (size_t)o->width * o->height * o->bpp;
    if (!dst || dst_size < need) return false;
    VkBuffer b; VkDeviceMemory bm; uint8_t *mp;
    if (!make_buf(vk, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &b, &bm, &mp)) return false;
    VkCommandBuffer cb;
    bool ok = cb_begin(vk, &cb);
    if (ok) {
        VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { o->width, o->height, 1 } };
        lagfx_translate_blit_cmd_buffer_image(cb, b, o->img, &r, false);
        host_barrier(m, cb);
        barrier(cb);
        ok = vkEndCommandBuffer(cb) == VK_SUCCESS && submit_wait(vk, m, &cb) == LAGFX_A64_DONE;
        if (cb) lagfx_vk_cmdbuf_free(vk, cb);
    }
    if (ok) { memcpy(dst, mp, need); if (w) *w = o->width; if (h) *h = o->height; free_buf(vk, b, bm); }
    return ok;   /* on a timeout the staging buffer is leaked: the copy may still run */
}

/* ---- explicit resource commands (Henge pvg_metal.m synchronize: / invalidate:) ---- */


/* 0x25 (arm64: remove ref from the task's object set): forget the mirror so a reused ref is rebuilt from its new table entry. */
void lagfx_a64_res_forget(lagfx_protocol_t *p, uint32_t task, uint32_t ref) {
    mgr2_t *m = p ? p->a64_res : NULL;
    struct lagfx_vk_state *vk = vk_of(p);
    if (!m || !vk) return;
    for (uint32_t i = 0; i < m->n; i++)
        if (m->objs[i] && m->objs[i]->task == task && m->objs[i]->ref == ref) { obj_release(m, vk, m->objs[i]); m->objs[i] = NULL; return; }
}

void lagfx_a64_res_drop_task(lagfx_protocol_t *p, uint32_t task) {
    mgr2_t *m = p ? p->a64_res : NULL;
    struct lagfx_vk_state *vk = vk_of(p);
    if (!m || !vk) return;
    for (uint32_t i = 0; i < m->n; i++)
        if (m->objs[i] && m->objs[i]->task == task) { obj_release(m, vk, m->objs[i]); m->objs[i] = NULL; }
}

bool lagfx_a64_res_busy(lagfx_protocol_t *p) { mgr2_t *m = p ? p->a64_res : NULL; return m && BUSY(m); }

void lagfx_a64_res_counts(lagfx_protocol_t *p, uint32_t *live, uint64_t *created, uint64_t *destroyed) {
    mgr2_t *m = p ? p->a64_res : NULL;
    uint32_t n = 0;
    for (uint32_t i = 0; m && i < m->n; i++) n += m->objs[i] != NULL;
    if (live) *live = n;
    if (created) *created = m ? m->st.created : 0;
    if (destroyed) *destroyed = m ? m->st.destroyed : 0;
}

lagfx_a64_obj_t *lagfx_a64_res_lookup(lagfx_protocol_t *p, uint32_t task, uint32_t ref) {
    mgr2_t *m = p ? p->a64_res : NULL;
    lagfx_a64_obj_t *o = m ? obj_find(m, task, ref) : NULL;
    if (m && !o) OT(m).obj_lookup_miss++;
    return o;
}

/* Names the reason texture handle (task, ref) has no usable mirror object. Read-only: the guest's object table entry is re-read and its payload peeked (format /
 * extent for the log; the offsets repeat obj_make's, which is the only place that decides what is creatable: the creation failure itself comes from the memo
 * obj_resolve keeps). Unverified assumption: an entry that is all zero is a slot the guest has not filled (or has deleted); nothing proves which of the two. */
bool lagfx_a64_res_diagnose(lagfx_protocol_t *p, uint32_t task, uint32_t ref, lagfx_a64_texdiag_t *out) {
    lagfx_a64_texdiag_t dg;
    mgr2_t *m = p ? p->a64_res : NULL;
    lagfx_task_entry_t *t = p ? lagfx_protocol_find_task(p, task) : NULL;
    if (!out) out = &dg;
    memset(out, 0, sizeof *out);
    if (!ref) { out->why = TEXWHY_REF_ZERO; return true; }
    if (!t || !t->lists_set || !t->objects_addr || (t->objects_len && ((uint64_t)ref + 1u) * 12u > t->objects_len)) { out->why = TEXWHY_NO_TABLE; return true; }
    uint8_t eb[12];
    if (!lagfx_arm64_task_read(p, t, t->objects_addr + (uint64_t)ref * 12u, eb, 12)) { out->why = TEXWHY_ENTRY_READ; return true; }
    uint32_t e[3] = { lagfx_le32(eb), lagfx_le32(eb + 4), lagfx_le32(eb + 8) };
    uint32_t type = e[0] & 0xffu, size = e[0] >> 8;
    uint64_t at = (uint64_t)e[2] << 32 | e[1];
    out->type = type;
    if (!type) { out->why = TEXWHY_ENTRY_EMPTY; return true; }
    if (type == 1 || type == 19) { out->why = TEXWHY_NOT_TEXTURE; return true; }
    if (type != 2 && type != 8 && type != 9 && type != 11) { out->why = TEXWHY_UNKNOWN_TYPE; return true; }
    /* peek: format and extent as obj_make reads them (best effort, only for the log) */
    uint8_t d[704];
    uint32_t n = size < sizeof d ? size : (uint32_t)sizeof d;
    if (n >= 36u && lagfx_arm64_task_read(p, t, at, d, n)) {
        if (type == 2) {
            uint32_t mips = lagfx_le16(d + 12) & 0x3fffu;
            uint32_t ro = 12u + 0x18u + 36u * mips;
            if (mips >= 1 && mips <= LAGFX_A64_MAX_MIPS && ro + 12u + 24u <= n) {
                uint32_t tag = lagfx_le32(d + ro);
                const uint8_t *ds = d + ro + 12;
                if (tag == 0x34 || tag == 0x39) out->mtl_format = ds[1] | ds[2] << 8, out->width = lagfx_le32(ds + 7), out->height = lagfx_le32(ds + 11);
                else out->mtl_format = ds[2] | ds[3] << 8, out->width = lagfx_le32(ds + 4), out->height = lagfx_le32(ds + 8);
            }
        } else if (type == 8 || type == 9) {
            uint32_t tag = lagfx_le32(d);
            const uint8_t *ds = d + 12;
            if ((tag == 9 || tag == 0x37) && n >= 0x20u + 0x17u) {
                ds = d + 0x20;
                if (tag == 0x37) out->mtl_format = ds[1] | ds[2] << 8, out->width = lagfx_le32(ds + 7), out->height = lagfx_le32(ds + 11);
                else out->mtl_format = ds[2] | ds[3] << 8, out->width = lagfx_le32(ds + 4), out->height = lagfx_le32(ds + 8);
            } else if ((tag == 0x34 || tag == 0x39) && n >= 12u + 0x17u) out->mtl_format = ds[1] | ds[2] << 8, out->width = lagfx_le32(ds + 7), out->height = lagfx_le32(ds + 11);
            else if ((tag == 0x14 || tag == 1) && n >= 12u + 20u) out->mtl_format = ds[2] | ds[3] << 8, out->width = lagfx_le32(ds + 4), out->height = lagfx_le32(ds + 8);
        } else if (n >= 24u) {   /* mapped: {u64 mapping, u32 tag, ..., description at +20}; the extent is the shell surface's (below) */
            uint32_t tag = lagfx_le32(d + 8);
            out->mapping = (uint32_t)lagfx_le64(d);
            out->mtl_format = tag == 0x39u ? (uint32_t)(d[21] | d[22] << 8) : (uint32_t)(d[22] | d[23] << 8);
        }
    }
    if (type == 11) {   /* the surface behind the mapping id */
        lagfx_device_t *dev = (lagfx_device_t *)p->dev;
        const lagfx_shell_callbacks_t *sh = dev ? &dev->desc.shell : NULL;
        uint8_t sd[256]; size_t sn = 0; lagfx_guest_surface_t gs;
        memset(sd, 0, sizeof sd);
        if (sh && sh->get_surface_desc && sh->get_surface_desc(sh->opaque, out->mapping, sd, sizeof sd, &sn) && sn >= LAGFX_SURF_DESC_BYTES && sn <= sizeof sd && lagfx_guest_surface_decode_any(sd, sn, &gs) == LAGFX_OK)
            out->width = gs.width, out->height = gs.height;
    }
    if (m) {
        lagfx_a64_obj_t *o = obj_find(m, task, ref);
        if (o && o->kind != LAGFX_A64_TEXTURE) { out->why = TEXWHY_NOT_TEXTURE; return true; }
        for (uint32_t i = 0; i < 128u && i < m->nfail; i++)
            if (m->fail[i].task == task && m->fail[i].ref == ref && !memcmp(m->fail[i].entry, e, 12)) { out->why = (lagfx_a64_texwhy_t)m->fail[i].why; return true; }
    }
    out->why = TEXWHY_NOT_MIRRORED;
    return m != NULL;
}

bool lagfx_a64_resource_cmd(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr || !lagfx_proto_arm64(p) || !lagfx_a64_res_enabled()) return false;
    uint32_t op = hdr->opcode;
    if (op != 0x34 && op != 0x35 && op != 0x3e && op != 0x3f) return false;
    mgr2_t *m = mgr_get(p, true);
    if (!m) return true;
    if (op == 0x3f) { m->st.hint_cmds++; return true; }   /* purgeable hint: the bytes just stay (Apple/Henge) */
    uint32_t skip = (uint32_t)hdr->arg_count_8b * 8u;
    if (!hdr->payload || hdr->payload_size < skip + 8u) { m->st.res_cmd_bad++; return true; }
    const uint8_t *a = hdr->payload + skip;
    uint32_t n = hdr->payload_size - skip, task = lagfx_le32(a), count = lagfx_le32(a + 4);
    if (op == 0x34) {   /* {task, count, {ref, host invalid, host valid, guest invalid, guest valid}} */
        if (count > LAGFX_EXEC_MAX_RES || 8ull + 8ull * count > n) { m->st.res_cmd_bad++; return true; }
        m->st.inv_cmds++;
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *e = a + 8 + 8u * i;
            lagfx_a64_obj_t *o = obj_find(m, task, lagfx_le32(e));
            if (!o) { m->st.inv_missing++; continue; }
            if (e[6]) o_set_stale(o, true);
            if (e[7]) o_set_stale(o, false);
            if (e[4]) o_set_hv(o, false);
            if (e[5]) o_set_hv(o, true);
            m->st.inv_objs++;
        }
        return true;
    }
    /* 0x35 / 0x3e synchronize {task, count, refs}: GPU -> guest pages for the host-valid ones, then no longer stale */
    if (count > LAGFX_EXEC_MAX_RES || 8ull + 4ull * count > n) { m->st.res_cmd_bad++; return true; }
    m->st.sync_cmds++;
    uint32_t refs[64];
    for (uint32_t done = 0; done < count; ) {
        uint32_t k = count - done < 64 ? count - done : 64;
        for (uint32_t i = 0; i < k; i++) refs[i] = lagfx_le32(a + 8 + 4u * (done + i));
        uint32_t w = lagfx_a64_res_sync_to_guest(p, task, refs, k);
        m->st.sync_objs += w;
        m->st.sync_skipped += k - w;   /* not mirrored (unsupported kind/format, never used), not host-valid, no guest pages, or the GPU is busy */
        done += k;
    }
    return true;
}

/* ---- linear texture sampling support (see arm64_resources.h) ---- */
void lagfx_a64_res_buf_written(lagfx_a64_obj_t *b) { if (b) b->wgen++, b->gpu_dirty = true; }

bool lagfx_a64_res_linear_fresh(lagfx_a64_rexec_t *x, lagfx_a64_obj_t *o) {
    if (!x || !o) return false;
    if (!o->linear) return true;
    lagfx_a64_obj_t *b = lin_backing(x->p, x->m, x->vk, x->t, o, false);
    return b && o->lin_pulled && o->lin_seen == b->wgen;
}

void lagfx_a64_res_sync_linear(lagfx_a64_rexec_t *x) {
    if (!x) return;
    bool any = false;
    for (uint32_t i = 0; i < x->nres; i++) {
        lagfx_a64_obj_t *o = x->res[i].o;
        if (o->kind != LAGFX_A64_TEXTURE || !o->linear || lagfx_a64_res_linear_fresh(x, o)) continue;
        any |= lin_pull_x(x, o);
    }
    if (any) x->m->st.linear_resync++;
}

bool lagfx_a64_res_prepare_sampled(lagfx_a64_rexec_t *x, uint32_t ref) {
    if (!x) return false;
    lagfx_a64_obj_t *o = lagfx_a64_res_object(x, ref);   /* created + first-use upload (+ first pull of a linear texture) */
    if (!o || o->kind != LAGFX_A64_TEXTURE) return false;
    if (o->linear) {
        lagfx_a64_obj_t *b = lin_backing(x->p, x->m, x->vk, x->t, o, true);
        /* The backing buffer is shared guest memory (as Apple's linear textures / buffers are). A listed buffer was re-read by begin(); one the list did not name is
         * re-read here, once per exec, unless the GPU wrote it since its last read (then the GPU's bytes are newer, as for a listed buffer marked stale).
         * Unverified on a device: that the guest changes unlisted buffers between execs. */
        if (b && b->has_pages && !b->guest_stale && !b->gpu_dirty && b->rd_epoch != x->m->epoch) guest_upload(x->p, x->m, x->t, b), b->host_valid = true;
        if (b && !lagfx_a64_res_linear_fresh(x, o)) { if (lin_pull_x(x, o)) x->m->st.linear_resync++; }
    }
    return true;
}

#endif /* LAGFX_HAVE_VULKAN */

/* The payload of an ARM64 display command: arg_count_8b barrier words (8 bytes each) first, then the arguments. Henge's decoder (pvg_lite.c) reads op 8 as 10 words
 * with the mapping id in words 2..3; ops 1 / 2 need 8 bytes. The meaning of op 1 / 2 / 8 is taken from there; nothing about Apple's behaviour is assumed here. */
void lagfx_a64_display_count(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) return;
    uint32_t op = hdr->opcode & 0xffu;
    if (op == 1u) p->diag2.disp.op_setpage++;
    else if (op == 2u) p->diag2.disp.op_ack++;
    else if (op != 8u) return;
    else {
        p->diag2.disp.op_swap++;
        uint32_t skip = (uint32_t)hdr->arg_count_8b * 8u;
        if (!hdr->payload || hdr->payload_size < skip + 40u) { p->diag2.disp.swap_short++; return; }
        uint64_t mapping_id = lagfx_le64(hdr->payload + skip + 8u);
        uint32_t r = lagfx_a64_res_display_note(p, mapping_id);
        {   /* g7-pvg-swap: diagnostics only */
            lagfx_dd_surface_t si; memset(&si, 0, sizeof si);
            si.resolved = (r & LAGFX_A64_DSP_RESOLVED) != 0u, si.gpu = (r & LAGFX_A64_DSP_GPU) != 0u, si.guest_current = (r & LAGFX_A64_DSP_WRITTEN) != 0u;
            if (si.resolved) lagfx_a64_res_display_info(p, mapping_id, &si.w, &si.h, &si.fourcc, &si.mtl_format, &si.vk_format);
            lagfx_dd_note_swap(mapping_id, &si);
        }
        if (!(r & LAGFX_A64_DSP_RESOLVED)) { p->diag2.disp.surface_unresolved++; return; }
        p->diag2.disp.surface_resolved++;
        if (!(r & LAGFX_A64_DSP_GPU)) return;
        p->diag2.disp.gpu_content++;
        if (r & LAGFX_A64_DSP_WRITTEN) p->diag2.disp.written_back++; else p->diag2.disp.gpu_unwritten++;
    }
}
