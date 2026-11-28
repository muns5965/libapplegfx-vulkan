/*
 * libapplegfx-vulkan — ARM64 exec: Metal tile shaders (serialized tile pipelines, tag 0x33; tile bind / dispatch records 0x9b..0xa3).
 * src/protocol/arm64_tile.h
 *
 * A Metal tile pipeline runs a kernel function once per render-pass tile, INSIDE the render pass, against the tile's imageblock (the colour attachments
 * as tile memory). Vulkan cannot dispatch compute inside a dynamic-rendering instance, so the executor (arm64_exec_run.c) suspends the instance, this
 * module records the compute dispatch, and the executor resumes the instance with LOAD ops (attachment contents are preserved).
 *
 * Mapping (the translated kernel is produced by the air2spv COMPUTE stage, see src/air2spv/translate_function.c):
 *   - tile == one workgroup: the workgroup size (SpecId 0/1/2) is the dispatch's threads-per-tile, the tile grid covers the render area (or the region)
 *   - set 0: kernel buffers / textures / samplers at the bindings the translator chose (Metal indices from lagfx_air_arg_bindings), the imageblock of
 *     colour attachment N is a STORAGE_IMAGE at binding 40 + N (Rgba16f; the attachment must be R16G16B16A16_SFLOAT with storage usage, or B8G8R8A8_UNORM with the
 *     conditions below, else the dispatch is skipped and counted)
 *   - the tile origin the kernel adds is WorkgroupId * WorkgroupSize; a region dispatch shifts WorkgroupId with vkCmdDispatchBase
 *   - simd lanes: Metal's simdgroup is 32 lanes. A kernel that uses subgroup operations is only run with a 32-wide subgroup: native size 32, or
 *     requiredSubgroupSize = 32 through VK_EXT_subgroup_size_control (core 1.3) when the device allows it (vulkan/caps.c lagfx_vk_can_pin_subgroup32).
 *     Otherwise (fallback) the dispatch is NOT run and counted (tile.skip_no_sg32): a lane-to-pixel mapping on another subgroup width would produce wrong
 *     pixels, which is worse than leaving the attachment untouched. LAGFX_TILE_ANY_SUBGROUP=1 overrides the check for debugging / tests on devices whose
 *     subgroup size differs (the kernel is then only right if it does not depend on the lane count).
 *     Third choice, before skipping: a device whose subgroup is a multiple of 32 wider (Adreno: 64, size not controllable) runs the kernel translated for LOGICAL
 *     32-lane simdgroups inside the wide subgroup (air2spv lagfx_air2spv_set_simd_emulation: lane id / simdgroup size / index / shuffles are remapped, see
 *     translate_function.c t_simd_emul for the assumptions). The native-32 and pinned paths are unchanged and always preferred. tile.sg_emulated counts these
 *     dispatches. LAGFX_TILE_SIMD_EMULATE=<W> (W a power of two, 2 <= W <= the device subgroup size) forces emulation with logical width W on ANY device (test
 *     hook: the unit tests run W=16 on a 32-wide lavapipe, which exercises the same logical-group boundary logic as W=32 on a 64-wide device).
 *     A kernel that calls a SIMD-group / quad-group operation the translator has no implementation for (simd_sum, simd_ballot, simd_all / any, prefix sums, broadcast, is_first,
 *     quad_* ...) is NOT built: tile.pipe_unsupported_simd counts it and the first operation's name is logged (air2spv lagfx_air2spv_last_unsupported_simd).
 * Colour attachments: RGBA16F and BGRA8 (see lagfx_a64_tile_rt_classify); BGRA8 uses a second translation of the kernel (imageblock = unformatted storage image),
 *   built on first use per attachment mask and cached on the tile pipeline. Other formats are skipped (tile.skip_rt, skip_rt_why).
 * Not mapped (counted): tile samplers (tags 0x9f / 0xa0: the default sampler is used), threadgroup / imageblock memory (0x9c), the render-target array
 * index of 0xa3, imageblock formats other than Rgba16f, draws while a tile pipeline is bound.
 */
#ifndef LAGFX_PROTOCOL_ARM64_TILE_H
#define LAGFX_PROTOCOL_ARM64_TILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "state.h"
#include "arm64_pipeline.h"

struct lagfx_vk_state;
/* The logical simdgroup width tile kernels are translated for on this device: 0 native/pinned-32, 32 emulated inside a wider subgroup (fixed, or a range pinned to its smallest size), test hook LAGFX_TILE_SIMD_EMULATE. */
uint32_t lagfx_a64_tile_simd_emul_width(const struct lagfx_vk_state *vk);

#define LAGFX_A64_TILE_TAG 0x33u          /* serialized tile pipeline descriptor */
#define LAGFX_A64_TILE_MAX_SLOTS 31u      /* Metal buffer / texture argument table size */

typedef struct lagfx_a64_tile_pipe lagfx_a64_tile_pipe_t;   /* a resolved tile kernel (referenced from lagfx_a64_pipeline_src_t::tile) */
typedef struct lagfx_a64_tile lagfx_a64_tile_t;             /* the tile stage of one executor: bind tables, descriptor pools, default sampler */

/* Tile bind / dispatch tags (= opcodes). */
enum {
    LAGFX_A64_TAG_TILE_DISPATCH = 155, LAGFX_A64_TAG_TILE_TGMEM = 156, LAGFX_A64_TAG_TILE_BUFFER = 157, LAGFX_A64_TAG_TILE_BUFFER_OFFSET = 158,
    LAGFX_A64_TAG_TILE_SAMPLER = 159, LAGFX_A64_TAG_TILE_SAMPLER_LOD = 160, LAGFX_A64_TAG_TILE_TEXTURE = 161, LAGFX_A64_TAG_TILE_REGION = 162,
    LAGFX_A64_TAG_TILE_REGION_RT = 163, LAGFX_A64_TAG_TILE_DIMS = 164
};
static inline bool lagfx_a64_is_tile_tag(uint32_t tag) { return tag >= 155u && tag <= 164u; }

/* ---- imageblock attachment classes (pure; the dispatch uses it, tests call it) ----
 * A tile kernel's imageblock of colour attachment N is a storage image the executor feeds from the attachment's own view:
 *   RGBA16F  VK_FORMAT_R16G16B16A16_SFLOAT (97): the kernel's storage image is Rgba16f (the original path; unchanged).
 *   BGRA8    VK_FORMAT_B8G8R8A8_UNORM (44): the kernel is re-translated with the imageblock as an UNFORMATTED storage image (SPIR-V has no Bgra8 format; access
 *            WithoutFormat). Vulkan hands the shader the texel as (R, G, B, A) components of the VIEW FORMAT, so no byte swizzle is emitted, and the unorm8 <-> float
 *            conversion (round to nearest, clamp to [0, 1]) is the format's own. Needs, all checked and counted separately (skip_rt_why): STORAGE usage on the image
 *            (create_image only keeps it when the device accepts it for B8G8R8A8), the device features shaderStorageImageRead/WriteWithoutFormat, and the format's
 *            VkFormatProperties3 STORAGE_READ/WRITE_WITHOUT_FORMAT bits (Vulkan 1.3; an older device is refused: the per-format answer cannot be had).
 * why (skip_rt_why index): 0 no attachment, 1 format not supported by the tile path, 2 no STORAGE usage, 3 no view / linear, 4 BGRA8 but the device lacks
 * shaderStorageImageRead/WriteWithoutFormat, 5 BGRA8 but the format has no without-format storage access (or it could not be queried). */
#define LAGFX_A64_TILE_VKFMT_RGBA16F 97u
#define LAGFX_A64_TILE_VKFMT_BGRA8   44u
#define LAGFX_A64_TILE_USAGE_STORAGE 0x8u
#define LAGFX_A64_TILE_F3_STORAGE_READ_NOFMT  (1ull << 31)   /* VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT */
#define LAGFX_A64_TILE_F3_STORAGE_WRITE_NOFMT (1ull << 32)   /* VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT */
enum { LAGFX_A64_TILE_RT_REFUSED = 0, LAGFX_A64_TILE_RT_RGBA16F = 1, LAGFX_A64_TILE_RT_BGRA8 = 2 };
typedef struct {
    bool present;            /* an attachment object is bound at the slot */
    uint32_t format, usage;  /* VkFormat, VkImageUsageFlags of its image */
    bool has_view, linear;
    bool nofmt_device;       /* shaderStorageImageReadWithoutFormat and WriteWithoutFormat enabled on the device */
    bool f3_valid; uint64_t f3;   /* VkFormatProperties3::optimalTilingFeatures of the format (f3_valid = it could be queried) */
} lagfx_a64_tile_rt_probe_t;
static inline uint32_t lagfx_a64_tile_rt_classify(const lagfx_a64_tile_rt_probe_t *r, uint32_t *why) {
    uint32_t w = 0;
    if (!r->present) w = 0;
    else if (r->format != LAGFX_A64_TILE_VKFMT_RGBA16F && r->format != LAGFX_A64_TILE_VKFMT_BGRA8) w = 1;
    else if (!(r->usage & LAGFX_A64_TILE_USAGE_STORAGE)) w = 2;
    else if (!r->has_view || r->linear) w = 3;
    else if (r->format == LAGFX_A64_TILE_VKFMT_RGBA16F) return LAGFX_A64_TILE_RT_RGBA16F;
    else if (!r->nofmt_device) w = 4;
    else if (!r->f3_valid || (r->f3 & (LAGFX_A64_TILE_F3_STORAGE_READ_NOFMT | LAGFX_A64_TILE_F3_STORAGE_WRITE_NOFMT)) != (LAGFX_A64_TILE_F3_STORAGE_READ_NOFMT | LAGFX_A64_TILE_F3_STORAGE_WRITE_NOFMT)) w = 5;
    else return LAGFX_A64_TILE_RT_BGRA8;
    if (why) *why = w;
    return LAGFX_A64_TILE_RT_REFUSED;
}

#ifdef LAGFX_HAVE_VULKAN
#include "arm64_exec_run.h"
#include "arm64_resources.h"

/* Resolver branch for descriptor tag 0x33 (called by lagfx_a64_pipeline_resolve after the descriptor was decoded): finds the kernel function, translates it
 * (COMPUTE stage), applies the device-capability gate, builds the set layout and shader module. Fills out->tile, out->release / user, out->blob_hash.
 * The caller's counters (resolve_failed pre-incremented) are settled here. */
bool lagfx_a64_tile_resolve(lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint32_t ref, const uint8_t *blob, uint32_t blen, const lagfx_a64_pdesc_t *d,
                            lagfx_a64_pipeline_src_t *out);

lagfx_a64_tile_t *lagfx_a64_tile_new(struct lagfx_vk_state *vk);
void lagfx_a64_tile_free(lagfx_a64_tile_t *t);               /* the device must be idle / alive */
void lagfx_a64_tile_reset(lagfx_a64_tile_t *t);              /* a new encoder: bind tables cleared */
void lagfx_a64_tile_flush(lagfx_a64_tile_t *t);              /* nothing recorded or in flight uses the descriptor sets any more: pools are reset */

/* Bind records 0x9c..0xa0, 0xa1: returns 0 = applied, 1 = recorded but not applied (partial), 2 = unsupported / malformed. */
int lagfx_a64_tile_bind_tag(lagfx_a64_tile_t *t, lagfx_protocol_t *p, lagfx_a64_rexec_t *x, uint32_t task, uint32_t tag, const uint8_t *rec, uint32_t n);

/* A decoded dispatch record (0x9b whole target, 0xa2 / 0xa3 region). */
typedef struct { uint32_t tw, th, td; bool region; uint32_t ox, oy, rw, rh; uint32_t rt_index; bool has_rt_index; } lagfx_a64_tile_disp_t;
bool lagfx_a64_tile_decode_dispatch(uint32_t tag, const uint8_t *rec, uint32_t n, lagfx_a64_tile_disp_t *out);

/* What the dispatch needs from the open render pass. suspend() ends the dynamic-rendering instance and moves the attachments to GENERAL; resume() begins it
 * again with LOAD ops (false = could not, the pass then has no rendering). Both run only once everything else has been validated, so a skipped dispatch
 * leaves the pass untouched. */
typedef struct {
    lagfx_protocol_t *p; lagfx_a64_rexec_t *x;
    lagfx_a64_obj_t *rts[4]; uint32_t nslots; uint32_t tgt_w, tgt_h;
    void *ctx; void (*suspend)(void *ctx); bool (*resume)(void *ctx);
} lagfx_a64_tile_env_t;

/* True = the dispatch was recorded (rendering resumed); false = skipped (counted in p->diag2.exec.tile). */
bool lagfx_a64_tile_dispatch(lagfx_a64_tile_t *t, lagfx_a64_tile_pipe_t *tp, const lagfx_a64_tile_env_t *env, const lagfx_a64_tile_disp_t *d);
#endif /* LAGFX_HAVE_VULKAN */

#endif
