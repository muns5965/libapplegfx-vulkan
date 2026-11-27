/*
 * libapplegfx-vulkan — ARM64 exec: Metal tile shaders. See arm64_tile.h for the mapping and its limits.
 * Layouts of the records (facts only, independently written): 0x9d {u32 first, u32 count, {u32 ref, u64 offset}*}, 0x9e {u32 index, u64 offset},
 * 0xa1 {u32 first, u32 count, u32 ref*}, 0x9b {u64 w, h, d}, 0xa2 {u64 w, h, d, origin x, y, z, size w, h, d} (+ 0xa3: u32 render-target array index).
 */
#include "arm64_tile.h"

#ifdef LAGFX_HAVE_VULKAN

#include "arm64_pipeline.h"
#include "../device.h"
#include "../common/le.h"
#include "../common/log.h"
#include "../vulkan/instance.h"
#include "../vulkan/caps.h"
#include "../air/bitcode_reader.h"
#include "../air2spv/translate.h"
#include "../air2spv/spv_reflect.h"
#include "../air2spirv/metallib_extract.h"
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define TILE(p) ((p)->diag2.exec.tile)
#define TILE_MAX_BIND 32u
#define TILE_RT_MAX 4u          /* imageblock images of colour attachments 0..3 are fed (the translator names each one "lagfx.imageblock.<n>"; its BINDING is whatever the allocator gave it) */
#define TILE_PIPE_CACHE 8u
#define TILE_VARIANTS 3u        /* kernel variants per pipeline with other imageblock formats (a mask of BGRA8 attachments; four attachments seldom mix) */
#define TILE_POOLS 16u

enum { TB_BUFFER = 1, TB_TEXTURE = 2, TB_SAMPLER = 3, TB_RT = 5 };

typedef struct { uint8_t binding, kind; int16_t metal_index; uint8_t written; } tile_bind_t;   /* written: the arg metadata says the kernel may write the buffer (air.write / air.read_write) */

struct lagfx_a64_tile_pipe {
    struct lagfx_vk_state *vk;
    VkDevice dev;
    VkShaderModule cs; char entry[64];
    uint8_t *bc; size_t bc_len;     /* a copy of the kernel's AIR bitcode: the BGRA8 imageblock variant is translated from it on first use */
    struct { uint32_t mask; VkShaderModule cs; } var[TILE_VARIANTS]; uint32_t nvar;   /* mask = the BGRA8 attachments; cs NULL = the build failed (cached) */
    VkDescriptorSetLayout dsl; VkPipelineLayout pl;
    uint32_t n_bind; tile_bind_t bind[TILE_MAX_BIND];
    uint32_t rt_mask;               /* bit n: the kernel writes the imageblock of colour attachment n (a STORAGE_IMAGE named lagfx.imageblock.n) */
    uint32_t simd_emul;             /* 0 = native simdgroup; W = the kernel was translated for logical W-lane simdgroups inside a wider subgroup */
    bool lane_mapped;               /* uses subgroup operations: Metal's 32-lane simdgroup semantics are assumed */
    struct { uint32_t l[3]; bool pin; uint32_t vmask; VkPipeline pl; } cache[TILE_PIPE_CACHE]; uint32_t ncache;
};

struct lagfx_a64_tile {
    struct lagfx_vk_state *vk;
    struct { lagfx_a64_obj_t *b; uint64_t off; } buf[LAGFX_A64_TILE_MAX_SLOTS];
    lagfx_a64_obj_t *tex[LAGFX_A64_TILE_MAX_SLOTS];
    VkSampler defsamp;
    bool f3_known; bool f3_valid; uint64_t f3_bgra8;   /* VkFormatProperties3::optimalTilingFeatures of B8G8R8A8_UNORM (queried once) */
    VkDescriptorPool pool[TILE_POOLS]; uint32_t npool, cur;
};

/* The logical simdgroup width to translate tile kernels for: 0 = native (the device's own 32-lane subgroup, or one pinned to 32 by the dispatch); 32 on a device whose
 * subgroup is wider (a multiple of 32) and cannot be pinned; the LAGFX_TILE_SIMD_EMULATE test hook forces any power of two <= the subgroup size. */
static uint32_t tile_simd_emul_width(const struct lagfx_vk_state *vk) {
    const char *ov = getenv("LAGFX_TILE_SIMD_EMULATE");
    if (ov && ov[0]) {
        char *end = NULL; unsigned long w = strtoul(ov, &end, 10);
        if (end && !*end && w >= 2ul && w <= 128ul && !(w & (w - 1ul)) && w <= vk->sg_size) return (uint32_t)w;
        return 0;
    }
    if (vk->sg_size == 32u && vk->sg_min == vk->sg_max) return 0;
    if (lagfx_vk_can_pin_subgroup32(vk)) return 0;
    if (vk->sg_size > 32u && !(vk->sg_size % 32u) && vk->sg_min == vk->sg_max) return 32u;   /* a fixed wide size; a range the driver may change per pipeline is not assumed */
    return 0;
}

/* ---- the translated kernel's descriptor bindings (a small SPIR-V scan: the shared reflector does not tell storage images from sampled ones) ---- */

typedef struct { uint32_t binding; uint8_t kind; uint32_t rt; } scan_bind_t;   /* rt: TB_RT only, the colour attachment of the imageblock image (its OpName) */

/* Fills out (binding order) with every set-0 variable; false = something the tile path cannot feed (another set, arrays, unknown resource type). */
static bool scan_bindings(const uint8_t *spv, size_t n, scan_bind_t *out, uint32_t cap, uint32_t *nout, char *entry, size_t entry_cap) {
    *nout = 0;
    if (n < 24u || (n & 3u) || lagfx_le32(spv) != 0x07230203u) return false;
    uint32_t bound = lagfx_le32(spv + 12);
    if (!bound || bound > (1u << 20)) return false;
    uint8_t *kind = calloc(bound, 1);           /* 1 sampled image, 2 storage image, 3 sampler, 4 struct */
    uint32_t *ptr_t = calloc(bound, 4), *ptr_sc = calloc(bound, 4), *dset = calloc(bound, 4), *dbind = calloc(bound, 4);
    uint8_t *hset = calloc(bound, 1), *hbind = calloc(bound, 1);
    uint32_t *var_t = calloc(bound, 4), *var_sc = calloc(bound, 4);
    uint8_t *isvar = calloc(bound, 1);
    uint32_t *rtn = calloc(bound, 4);          /* imageblock render target of a variable (OpName lagfx.imageblock.<n>) + 1; 0 = none */
    bool ok = kind && ptr_t && ptr_sc && dset && dbind && hset && hbind && var_t && var_sc && isvar && rtn, got_entry = false;
    for (size_t w = 5; ok && w * 4u + 4u <= n;) {
        uint32_t ins = lagfx_le32(spv + w * 4u), op = ins & 0xffffu, wc = ins >> 16;
        if (!wc || (w + wc) * 4u > n) { ok = false; break; }
#define W(i) lagfx_le32(spv + (w + (i)) * 4u)
        switch (op) {
        case 15: if (wc >= 4u && W(1) == 5u && !got_entry) {   /* OpEntryPoint GLCompute id name */
                size_t room = (size_t)(wc - 3u) * 4u; const char *s = (const char *)spv + (w + 3u) * 4u; size_t l = strnlen(s, room);
                if (l < entry_cap && l < room) memcpy(entry, s, l + 1u), got_entry = true;
            } break;
        case 71: if (wc >= 4u && W(1) < bound) { if (W(2) == 34u) hset[W(1)] = 1, dset[W(1)] = W(3); else if (W(2) == 33u) hbind[W(1)] = 1, dbind[W(1)] = W(3); } break;
        case 25: if (wc >= 8u && W(1) < bound && W(3) != 6u) kind[W(1)] = W(7) == 2u ? 2 : 1; break;   /* OpTypeImage: Sampled operand (1 sampled, 2 storage); SubpassData is not a tile resource */
        case 5: if (wc >= 3u && W(1) < bound) {   /* OpName "lagfx.imageblock.<n>" */
                const char *nm = (const char *)spv + (w + 2u) * 4u; size_t room = (size_t)(wc - 2u) * 4u, l = strnlen(nm, room), pl = sizeof LAGFX_IMAGEBLOCK_NAME_PREFIX - 1u;
                if (l > pl && l < room && !memcmp(nm, LAGFX_IMAGEBLOCK_NAME_PREFIX, pl)) { uint32_t v = 0; bool dig = true; for (size_t h = pl; h < l; h++) { if (nm[h] < '0' || nm[h] > '9' || v > 1000u) { dig = false; break; } v = v * 10u + (uint32_t)(nm[h] - '0'); } if (dig) rtn[W(1)] = v + 1u; }
            } break;
        case 26: if (wc >= 2u && W(1) < bound) kind[W(1)] = 3; break;
        case 28: case 29: if (wc >= 2u && W(1) < bound) kind[W(1)] = 9; break;      /* arrays of resources: not mapped */
        case 30: if (wc >= 2u && W(1) < bound) kind[W(1)] = 4; break;
        case 32: if (wc >= 4u && W(1) < bound) ptr_sc[W(1)] = W(2), ptr_t[W(1)] = W(3); break;
        case 59: if (wc >= 4u && W(2) < bound) isvar[W(2)] = 1, var_t[W(2)] = W(1), var_sc[W(2)] = W(3); break;
        default: break;
        }
#undef W
        w += wc;
    }
    if (ok && !got_entry) ok = false;
    for (uint32_t id = 0; ok && id < bound; id++) {
        if (!isvar[id] || !hset[id] || !hbind[id]) continue;
        uint32_t pt = var_t[id] < bound ? ptr_t[var_t[id]] : 0u, k = pt < bound ? kind[pt] : 0u;
        uint8_t bk;
        if (dset[id] != 0u || k == 9u) { ok = false; break; }
        if (k == 1u) bk = TB_TEXTURE; else if (k == 2u) bk = TB_RT; else if (k == 3u) bk = TB_SAMPLER;
        else if (k == 4u && var_sc[id] == 12u /* StorageBuffer */) bk = TB_BUFFER;
        else { ok = false; break; }
        if (*nout >= cap) { ok = false; break; }
        if (bk == TB_RT && !rtn[id]) { ok = false; break; }   /* an imageblock image the translator did not name: the attachment is unknown, not guessed */
        out[*nout].binding = dbind[id], out[*nout].kind = bk, out[*nout].rt = rtn[id] ? rtn[id] - 1u : 0u, (*nout)++;
    }
    for (uint32_t a = 1; ok && a < *nout; a++) { scan_bind_t x = out[a]; uint32_t b = a; while (b && out[b - 1].binding > x.binding) out[b] = out[b - 1], b--; out[b] = x; }
    free(kind); free(ptr_t); free(ptr_sc); free(dset); free(dbind); free(hset); free(hbind); free(var_t); free(var_sc); free(isvar); free(rtn);
    return ok;
}

static void tile_pipe_release(void *u) {
    lagfx_a64_tile_pipe_t *tp = u;
    if (!tp) return;
    for (uint32_t i = 0; i < tp->ncache; i++) if (tp->cache[i].pl) vkDestroyPipeline(tp->dev, tp->cache[i].pl, NULL);
    if (tp->pl) vkDestroyPipelineLayout(tp->dev, tp->pl, NULL);
    if (tp->dsl) vkDestroyDescriptorSetLayout(tp->dev, tp->dsl, NULL);
    if (tp->cs) vkDestroyShaderModule(tp->dev, tp->cs, NULL);
    for (uint32_t i = 0; i < tp->nvar; i++) if (tp->var[i].cs) vkDestroyShaderModule(tp->dev, tp->var[i].cs, NULL);
    free(tp->bc);
    free(tp);
}

static VkDescriptorType vk_type(uint8_t k) {
    return k == TB_BUFFER ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : k == TB_TEXTURE ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : k == TB_SAMPLER ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
}

bool lagfx_a64_tile_resolve(lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint32_t ref, const uint8_t *blob, uint32_t blen, const lagfx_a64_pdesc_t *d,
                            lagfx_a64_pipeline_src_t *out) {
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    struct lagfx_vk_state *vk = dev ? dev->vk : NULL;
    uint8_t *spv = NULL; size_t spv_n = 0;
    lagfx_a64_tile_pipe_t *tp = NULL;
    lagfx_air_arg_binding_t ab[16]; size_t nab = 0;
    uint32_t emul = 0;
    uint8_t *bc_copy = NULL; size_t bc_len = 0;   /* the kernel's bitcode, kept on the pipeline for the BGRA8 variant */
    if (!vk || !vk->initialized || vk->device == VK_NULL_HANDLE) { p->diag2.exec.pipe.rf_no_vulkan++; return false; }
    emul = tile_simd_emul_width(vk);
    /* the kernel: the first kernel-stage function of the libraries the descriptor's function tokens name */
    lagfx_air_module_t *m = NULL;
    uint8_t *lib = NULL;
    for (uint32_t i = 0; i < d->nrefs && !m; i++) {
        uint32_t len = 0;
        lib = lagfx_a64_read_function_lib(p, task, d->refs[i], &len, NULL);
        if (!lib) continue;
        lagfx_metallib_function_t fn[8]; size_t nfn = 0;
        if (lagfx_metallib_extract_functions(lib, len, fn, 8, &nfn) == LAGFX_OK)
            for (size_t k = 0; k < nfn && k < 8 && !m; k++)
                if (fn[k].bitcode && fn[k].bitcode_len && fn[k].stage == LAGFX_METALLIB_STAGE_KERNEL) {
                    lagfx_air_module_t *mm = NULL;
                    if (lagfx_air_module_open(fn[k].bitcode, fn[k].bitcode_len, &mm) == LAGFX_OK && mm) {
                        lagfx_air2spv_set_simd_emulation(emul);
                        lagfx_status_t xst = lagfx_air2spv_translate_module(mm, &spv, &spv_n);
                        lagfx_air2spv_set_simd_emulation(0);
                        if (xst == LAGFX_OK && spv && spv_n) {
                            nab = lagfx_air_arg_bindings(mm, ab, 16); if (nab > 16u) nab = 16u; m = mm;
                            bc_copy = malloc(fn[k].bitcode_len);   /* a failed copy only disables the BGRA8 variant (counted when it is needed) */
                            if (bc_copy) memcpy(bc_copy, fn[k].bitcode, fn[k].bitcode_len), bc_len = fn[k].bitcode_len;
                        }
                        else {
                            char un[80]; uint32_t nun = lagfx_air2spv_last_unsupported_simd(un, sizeof un);
                            if (nun) {   /* a SIMD-group / quad-group operation without an implementation: the kernel is not built (its result would be an undefined value) */
                                TILE(p).pipe_unsupported_simd++;
                                LAGFX_WARN("arm64 tile pipeline %u: kernel uses %u SIMD-group / quad-group operation(s) with no implementation (first: %s); pipeline not built", ref, nun, un);
                            }
                            free(spv); spv = NULL; p->diag2.exec.pipe.rf_translate++; lagfx_air_module_free(mm);
                        }
                    } else p->diag2.exec.pipe.rf_module++;
                }
        free(lib); lib = NULL;
    }
    if (!m) { TILE(p).pipe_no_kernel++; goto fail; }
    lagfx_air_module_free(m);
    p->diag2.exec.pipe.xlate_air2spv++;
    {   /* device-capability gate, as for the graphics pipelines */
        uint32_t need;
        if (!lagfx_spv_required_caps_ex(spv, spv_n, &need)) { TILE(p).pipe_missing_cap++; p->diag2.exec.pipe.rf_missing_cap++; goto fail; }
        uint32_t miss = need & ~vk->caps_enabled;
        if (miss) {
            for (unsigned c = 0; c < LAGFX_VK_CAP_COUNT; c++) if (miss & LAGFX_VK_CAP_BIT(c)) p->diag2.exec.pipe.stub_missing_cap[c]++;
            TILE(p).pipe_missing_cap++; p->diag2.exec.pipe.rf_missing_cap++;
            LAGFX_WARN("arm64 tile pipeline %u: kernel needs device capabilities that are not enabled (mask 0x%x); pipeline not built", ref, miss);
            goto fail;
        }
        tp = calloc(1, sizeof *tp);
        if (!tp) goto fail;
        tp->simd_emul = emul;
        tp->lane_mapped = (need & (LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SUBGROUP) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SUBGROUP_SHUFFLE) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SUBGROUP_SHUFFLE_RELATIVE))) != 0;
    }
    tp->vk = vk, tp->dev = vk->device;
    tp->bc = bc_copy, tp->bc_len = bc_len, bc_copy = NULL;
    scan_bind_t sb[TILE_MAX_BIND]; uint32_t nsb = 0;
    if (!scan_bindings(spv, spv_n, sb, TILE_MAX_BIND, &nsb, tp->entry, sizeof tp->entry)) { TILE(p).pipe_bindings++; goto fail; }
    {   /* each resource binding matched to the n-th Metal argument of its kind (buffer 1 / texture 2 / sampler 3); an inline sampler has no argument (-1 = default sampler) */
        uint32_t nth[4] = { 0, 0, 0, 0 };
        for (uint32_t i = 0; i < nsb; i++) {
            tile_bind_t *b = &tp->bind[tp->n_bind];
            b->binding = (uint8_t)sb[i].binding, b->kind = sb[i].kind, b->metal_index = -1, b->written = 1;   /* unknown access (no metadata) counts as written: never gathered */
            if (sb[i].binding > 255u) { TILE(p).pipe_bindings++; goto fail; }
            if (sb[i].kind == TB_RT) {
                if (sb[i].rt >= TILE_RT_MAX) { TILE(p).pipe_bindings++; goto fail; }
                b->metal_index = (int16_t)sb[i].rt;
                tp->rt_mask |= 1u << sb[i].rt;
            } else {
                uint32_t want = sb[i].kind, n = nth[want]++, seen = 0;
                for (size_t k = 0; k < nab; k++) if (ab[k].kind == want && seen++ == n) { b->metal_index = ab[k].metal_index; b->written = ab[k].written; break; }
                if (b->metal_index > 30 || (b->metal_index < 0 && sb[i].kind != TB_SAMPLER)) { TILE(p).pipe_bindings++; goto fail; }
            }
            tp->n_bind++;
        }
    }
    {
        VkDescriptorSetLayoutBinding lb[TILE_MAX_BIND];
        for (uint32_t i = 0; i < tp->n_bind; i++) lb[i] = (VkDescriptorSetLayoutBinding){ tp->bind[i].binding, vk_type(tp->bind[i].kind), 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = tp->n_bind, .pBindings = lb };
        if (vkCreateDescriptorSetLayout(vk->device, &dli, NULL, &tp->dsl) != VK_SUCCESS) { TILE(p).pipe_bindings++; goto fail; }
        VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &tp->dsl };
        if (vkCreatePipelineLayout(vk->device, &pli, NULL, &tp->pl) != VK_SUCCESS) { TILE(p).pipe_bindings++; goto fail; }
        VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = spv_n, .pCode = (const uint32_t *)(const void *)spv };
        if (vkCreateShaderModule(vk->device, &ci, NULL, &tp->cs) != VK_SUCCESS) { p->diag2.exec.pipe.rf_module++; TILE(p).pipe_failed++; goto fail; }
    }
    free(spv);
    memset(out, 0, sizeof *out);
    out->tile = tp;
    out->release = tile_pipe_release, out->user = tp;
    out->blob_hash = lagfx_a64_hash64(blob, blen);
    p->diag2.exec.pipe.resolve_failed--, p->diag2.exec.pipe.resolved++, TILE(p).pipes++;
    return true;
fail:
    TILE(p).pipe_failed++;
    free(spv); free(bc_copy);
    tile_pipe_release(tp);
    return false;
}

/* ---- the tile stage state ---- */

lagfx_a64_tile_t *lagfx_a64_tile_new(struct lagfx_vk_state *vk) {
    lagfx_a64_tile_t *t = calloc(1, sizeof *t);
    if (t) t->vk = vk;
    return t;
}
void lagfx_a64_tile_reset(lagfx_a64_tile_t *t) { if (t) memset(t->buf, 0, sizeof t->buf), memset(t->tex, 0, sizeof t->tex); }
void lagfx_a64_tile_flush(lagfx_a64_tile_t *t) {
    if (!t) return;
    for (uint32_t i = 0; i < t->npool; i++) vkResetDescriptorPool(t->vk->device, t->pool[i], 0);
    t->cur = 0;
}
void lagfx_a64_tile_free(lagfx_a64_tile_t *t) {
    if (!t) return;
    for (uint32_t i = 0; i < t->npool; i++) vkDestroyDescriptorPool(t->vk->device, t->pool[i], NULL);
    if (t->defsamp) vkDestroySampler(t->vk->device, t->defsamp, NULL);
    free(t);
}

/* ---- records ---- */

int lagfx_a64_tile_bind_tag(lagfx_a64_tile_t *t, lagfx_protocol_t *p, lagfx_a64_rexec_t *x, uint32_t task, uint32_t tag, const uint8_t *rec, uint32_t n) {
    switch (tag) {
    case LAGFX_A64_TAG_TILE_BUFFER: {
        if (n < 8u) return 2;
        uint32_t first = lagfx_le32(rec), count = lagfx_le32(rec + 4);
        if (count > LAGFX_A64_TILE_MAX_SLOTS || (uint64_t)n < 8u + 12ull * count) return 2;
        for (uint32_t i = 0; i < count && first + i < LAGFX_A64_TILE_MAX_SLOTS; i++) {
            lagfx_a64_obj_t *b = lagfx_a64_res_object(x, lagfx_le32(rec + 8 + i * 12));
            if (b && b->kind != LAGFX_A64_BUFFER) b = NULL;
            t->buf[first + i].b = b, t->buf[first + i].off = lagfx_le64(rec + 12 + i * 12);
        }
        TILE(p).binds_buf++;
        return 0;
    }
    case LAGFX_A64_TAG_TILE_BUFFER_OFFSET: {
        if (n < 12u) return 2;
        uint32_t idx = lagfx_le32(rec);
        if (idx < LAGFX_A64_TILE_MAX_SLOTS) t->buf[idx].off = lagfx_le64(rec + 4);
        TILE(p).binds_buf++;
        return 0;
    }
    case LAGFX_A64_TAG_TILE_TEXTURE: {
        if (n < 8u) return 2;
        uint32_t first = lagfx_le32(rec), count = lagfx_le32(rec + 4);
        if (count > LAGFX_A64_TILE_MAX_SLOTS || (uint64_t)n < 8u + 4ull * count) return 2;
        for (uint32_t i = 0; i < count && first + i < LAGFX_A64_TILE_MAX_SLOTS; i++) {   /* only textures the mirror already holds (the exec's resource list), as for fragment textures */
            lagfx_a64_obj_t *o = lagfx_a64_res_lookup(p, task, lagfx_le32(rec + 8 + i * 4));
            t->tex[first + i] = o && o->kind == LAGFX_A64_TEXTURE ? o : NULL;
        }
        TILE(p).binds_tex++;
        return 0;
    }
    case LAGFX_A64_TAG_TILE_SAMPLER: case LAGFX_A64_TAG_TILE_SAMPLER_LOD: case LAGFX_A64_TAG_TILE_TGMEM: case LAGFX_A64_TAG_TILE_DIMS:
        TILE(p).binds_ignored++;   /* samplers: the default sampler is used; threadgroup memory / tile dimensions: not mapped */
        return 1;
    default: return 2;
    }
}

bool lagfx_a64_tile_decode_dispatch(uint32_t tag, const uint8_t *rec, uint32_t n, lagfx_a64_tile_disp_t *o) {
    memset(o, 0, sizeof *o);
    uint64_t w, h, d;
    if (tag == LAGFX_A64_TAG_TILE_DISPATCH) { if (n < 24u) return false; }
    else if (tag == LAGFX_A64_TAG_TILE_REGION || tag == LAGFX_A64_TAG_TILE_REGION_RT) { if (n < 72u) return false; }
    else return false;
    w = lagfx_le64(rec), h = lagfx_le64(rec + 8), d = lagfx_le64(rec + 16);
    if (!w || !h || !d || w > 0xffffu || h > 0xffffu || d > 0xffffu) return false;
    o->tw = (uint32_t)w, o->th = (uint32_t)h, o->td = (uint32_t)d;
    if (tag != LAGFX_A64_TAG_TILE_DISPATCH) {
        uint64_t ox = lagfx_le64(rec + 24), oy = lagfx_le64(rec + 32), rw = lagfx_le64(rec + 48), rh = lagfx_le64(rec + 56);
        if (ox > 0xffffffffull || oy > 0xffffffffull || rw > 0xffffffffull || rh > 0xffffffffull) return false;
        o->region = true, o->ox = (uint32_t)ox, o->oy = (uint32_t)oy, o->rw = (uint32_t)rw, o->rh = (uint32_t)rh;
        if (tag == LAGFX_A64_TAG_TILE_REGION_RT && n >= 76u) o->rt_index = lagfx_le32(rec + 72), o->has_rt_index = true;
    }
    return true;
}

/* ---- dispatch ---- */

/* VkFormatProperties3 of B8G8R8A8_UNORM (Vulkan 1.3 core), once. f3_valid false = not queryable (an older device): the BGRA8 path is then refused (why 5). */
static void tile_query_bgra_f3(lagfx_a64_tile_t *t) {
    if (t->f3_known) return;
    t->f3_known = true;
    const uint32_t v = t->vk->phys_props.apiVersion;
    if (!(VK_VERSION_MAJOR(v) > 1u || (VK_VERSION_MAJOR(v) == 1u && VK_VERSION_MINOR(v) >= 3u)) || t->vk->phys_device == VK_NULL_HANDLE) return;
    VkFormatProperties3 f3 = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3 };
    VkFormatProperties2 f2 = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &f3 };
    vkGetPhysicalDeviceFormatProperties2(t->vk->phys_device, VK_FORMAT_B8G8R8A8_UNORM, &f2);
    t->f3_valid = true, t->f3_bgra8 = (uint64_t)f3.optimalTilingFeatures;
}

/* The kernel translated with the imageblocks in `mask` (the BGRA8 attachments) as unformatted storage images; built once per mask, a failure is cached (NULL). The result must
 * bind exactly like the base translation (same set 0 bindings, kinds and attachment names), else it is refused: the descriptor set built for the base would not fit. */
static VkShaderModule tile_variant_module(lagfx_a64_tile_pipe_t *tp, uint32_t mask, lagfx_protocol_t *p) {
    for (uint32_t i = 0; i < tp->nvar; i++) if (tp->var[i].mask == mask) return tp->var[i].cs;
    if (tp->nvar == TILE_VARIANTS) { TILE(p).bgra8_variant_failed++; return VK_NULL_HANDLE; }
    VkShaderModule cs = VK_NULL_HANDLE;
    uint8_t *spv = NULL; size_t spv_n = 0;
    lagfx_air_module_t *m = NULL;
    if (tp->bc && lagfx_air_module_open(tp->bc, tp->bc_len, &m) == LAGFX_OK && m) {
        lagfx_air2spv_set_simd_emulation(tp->simd_emul);
        lagfx_air2spv_set_imageblock_unformatted(mask);
        lagfx_status_t st = lagfx_air2spv_translate_module(m, &spv, &spv_n);
        lagfx_air2spv_set_imageblock_unformatted(0);
        lagfx_air2spv_set_simd_emulation(0);
        lagfx_air_module_free(m);
        if (st == LAGFX_OK && spv && spv_n) {
            scan_bind_t sb[TILE_MAX_BIND]; uint32_t nsb = 0; char entry[64];
            bool same = scan_bindings(spv, spv_n, sb, TILE_MAX_BIND, &nsb, entry, sizeof entry) && nsb == tp->n_bind && !strcmp(entry, tp->entry);
            for (uint32_t i = 0; same && i < nsb; i++) same = sb[i].binding == tp->bind[i].binding && sb[i].kind == tp->bind[i].kind && (sb[i].kind != TB_RT || sb[i].rt == (uint32_t)tp->bind[i].metal_index);
            uint32_t need = 0;
            if (same && lagfx_spv_required_caps_ex(spv, spv_n, &need) && !(need & ~tp->vk->caps_enabled)) {
                VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = spv_n, .pCode = (const uint32_t *)(const void *)spv };
                if (vkCreateShaderModule(tp->dev, &ci, NULL, &cs) != VK_SUCCESS) cs = VK_NULL_HANDLE;
            }
        }
        free(spv);
    }
    tp->var[tp->nvar].mask = mask, tp->var[tp->nvar].cs = cs, tp->nvar++;
    if (cs) TILE(p).bgra8_variants++; else TILE(p).bgra8_variant_failed++;
    return cs;
}

static VkPipeline tile_vk_pipeline(lagfx_a64_tile_pipe_t *tp, const uint32_t l[3], bool pin, uint32_t vmask, VkShaderModule cs, lagfx_protocol_t *p) {
    for (uint32_t i = 0; i < tp->ncache; i++)
        if (tp->cache[i].pin == pin && tp->cache[i].vmask == vmask && !memcmp(tp->cache[i].l, l, 12)) return tp->cache[i].pl;
    if (tp->ncache == TILE_PIPE_CACHE) return VK_NULL_HANDLE;   /* more distinct tile sizes than a kernel ever uses: not built (counted by the caller) */
    VkSpecializationMapEntry sme[3] = { { 0, 0, 4 }, { 1, 4, 4 }, { 2, 8, 4 } };
    VkSpecializationInfo si = { 3, sme, 12, l };
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = 32u };
    VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .flags = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT, .layout = tp->pl,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = pin ? &rs : NULL, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cs,
                   .pName = tp->entry, .pSpecializationInfo = &si } };
    VkPipeline pl = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(tp->dev, VK_NULL_HANDLE, 1, &ci, NULL, &pl) != VK_SUCCESS) pl = VK_NULL_HANDLE;
    memcpy(tp->cache[tp->ncache].l, l, 12), tp->cache[tp->ncache].pin = pin, tp->cache[tp->ncache].vmask = vmask, tp->cache[tp->ncache].pl = pl, tp->ncache++;   /* a failure is cached too */
    (void)p;
    return pl;
}

static VkDescriptorSet alloc_set(lagfx_a64_tile_t *t, VkDescriptorSetLayout dsl) {
    for (;;) {
        if (t->cur == t->npool) {
            if (t->npool == TILE_POOLS) return VK_NULL_HANDLE;
            VkDescriptorPoolSize ps[4] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256 }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 128 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 64 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64 } };
            VkDescriptorPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 64, .poolSizeCount = 4, .pPoolSizes = ps };
            if (vkCreateDescriptorPool(t->vk->device, &pi, NULL, &t->pool[t->npool]) != VK_SUCCESS) return VK_NULL_HANDLE;
            t->npool++;
        }
        VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = t->pool[t->cur], .descriptorSetCount = 1, .pSetLayouts = &dsl };
        VkDescriptorSet ds;
        VkResult r = vkAllocateDescriptorSets(t->vk->device, &ai, &ds);
        if (r == VK_SUCCESS) return ds;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) return VK_NULL_HANDLE;
        t->cur++;
    }
}


/* The first TILE_SEEN_MAX distinct geometries / unaligned binds are kept in the stats so the device log names them (state.h: tile.seen_*). */
static void tile_note_geom(lagfx_protocol_t *p, const lagfx_a64_tile_disp_t *d, uint32_t tgt_w, uint32_t tgt_h) {
    struct lagfx_tile_geom g; memset(&g, 0, sizeof g);
    g = (struct lagfx_tile_geom){ d->region, d->ox, d->oy, d->rw, d->rh, d->tw, d->th, d->td, tgt_w, tgt_h };
    for (uint32_t i = 0; i < TILE(p).n_geom; i++) if (!memcmp(&TILE(p).geom[i], &g, sizeof g)) return;
    if (TILE(p).n_geom < LAGFX_TILE_SEEN_MAX) TILE(p).geom[TILE(p).n_geom++] = g;
}
static void tile_note_unaligned(lagfx_protocol_t *p, uint32_t metal_index, uint64_t off, uint64_t len, uint64_t align, bool written) {
    struct lagfx_tile_unal u; memset(&u, 0, sizeof u);
    u = (struct lagfx_tile_unal){ metal_index, written, off, len, align };
    for (uint32_t i = 0; i < TILE(p).n_unal; i++) if (!memcmp(&TILE(p).unal[i], &u, sizeof u)) return;
    if (TILE(p).n_unal < LAGFX_TILE_SEEN_MAX) TILE(p).unal[TILE(p).n_unal++] = u;
}
#define TILE_GATHER_MAX (8ull << 20)
/* Metal accepts any 4-aligned buffer offset, Vulkan only multiples of minStorageBufferOffsetAlignment. A buffer the kernel never writes (arg metadata has no
 * air.write) is bound as an aligned host copy of [off, length) made now (a snapshot: GPU writes earlier in the same exec are not seen). Same technique as
 * arm64_exec_run.c gather_copy. A buffer the kernel may write cannot be copied (its writes would be lost): skipped. */
static VkBuffer tile_gather(lagfx_a64_rexec_t *x, lagfx_protocol_t *p, lagfx_a64_obj_t *o, uint64_t off, uint64_t range) {
    VkBuffer sb; uint8_t *mp;
    if (!o->map || range > TILE_GATHER_MAX || !lagfx_a64_res_scratch(x, range, &sb, &mp)) { TILE(p).gather_failed++; return VK_NULL_HANDLE; }
    memcpy(mp, o->map + off, (size_t)range);
    TILE(p).gathered++;
    return sb;
}

bool lagfx_a64_tile_dispatch(lagfx_a64_tile_t *t, lagfx_a64_tile_pipe_t *tp, const lagfx_a64_tile_env_t *e, const lagfx_a64_tile_disp_t *d) {
    lagfx_protocol_t *p = e->p;
    struct lagfx_vk_state *vk = t->vk;
    const VkPhysicalDeviceLimits *lim = &vk->phys_props.limits;
    tile_note_geom(p, d, e->tgt_w, e->tgt_h);
    if (d->has_rt_index && d->rt_index) { TILE(p).rt_index_ignored++; }
    if (d->tw > lim->maxComputeWorkGroupSize[0] || d->th > lim->maxComputeWorkGroupSize[1] || d->td > lim->maxComputeWorkGroupSize[2] ||
        (uint64_t)d->tw * d->th * d->td > lim->maxComputeWorkGroupInvocations) { TILE(p).skip_local++; return false; }
    /* simd lanes: a 32-wide subgroup or nothing (see the header) */
    bool pin = false;
    if (tp->simd_emul) {
        /* the kernel carries its own logical-group lane mapping: no subgroup size requirement */
        if (tp->lane_mapped && tp->simd_emul > vk->sg_size) { TILE(p).skip_no_sg32++; return false; }
    } else if (tp->lane_mapped && !(vk->sg_size == 32u && vk->sg_min == vk->sg_max)) {
        if (lagfx_vk_can_pin_subgroup32(vk)) pin = true;
        else {
            const char *ov = getenv("LAGFX_TILE_ANY_SUBGROUP");
            if (ov && ov[0] == '1' && !ov[1]) TILE(p).sg_override++;
            else { TILE(p).skip_no_sg32++; return false; }
        }
    }
    /* the imageblock attachments: Rgba16f storage images, or B8G8R8A8_UNORM through the unformatted-image kernel variant (lagfx_a64_tile_rt_classify) */
    uint32_t bgra_mask = 0;
    for (uint32_t i = 0; i < 4u; i++) {
        if (!(tp->rt_mask & (1u << i))) continue;
        lagfx_a64_obj_t *o = i < e->nslots ? e->rts[i] : NULL;
        lagfx_a64_tile_rt_probe_t pr = { .present = o != NULL, .format = o ? (uint32_t)o->format : 0u, .usage = o ? (uint32_t)o->usage : 0u, .has_view = o && o->view, .linear = o && o->linear,
                                         .nofmt_device = vk->have_storage_img_rw_nofmt };
        if (o && (uint32_t)o->format == LAGFX_A64_TILE_VKFMT_BGRA8) { tile_query_bgra_f3(t); pr.f3_valid = t->f3_valid, pr.f3 = t->f3_bgra8; }
        uint32_t why = 0, cls = lagfx_a64_tile_rt_classify(&pr, &why);
        if (cls == LAGFX_A64_TILE_RT_REFUSED) {
            TILE(p).skip_rt++;
            TILE(p).skip_rt_why[why]++;
            if (o && !TILE(p).skip_rt_first.valid) TILE(p).skip_rt_first.format = (uint32_t)o->format, TILE(p).skip_rt_first.usage = (uint32_t)o->usage, TILE(p).skip_rt_first.linear = o->linear ? 1 : 0, TILE(p).skip_rt_first.valid = 1;
            return false;
        }
        if (cls == LAGFX_A64_TILE_RT_BGRA8) bgra_mask |= 1u << i;
    }
    uint32_t gx0 = 0, gy0 = 0, gx1, gy1;
    if (d->region) {
        uint64_t ex = (uint64_t)d->ox + d->rw, ey = (uint64_t)d->oy + d->rh;
        if (ex > e->tgt_w) ex = e->tgt_w;
        if (ey > e->tgt_h) ey = e->tgt_h;
        if (d->ox >= ex || d->oy >= ey) { TILE(p).region_empty++; return true; }   /* nothing to dispatch is done, not skipped */
        if ((d->ox % d->tw) || (d->oy % d->th)) TILE(p).region_unaligned++;      /* whole tiles that intersect the region run (the kernel clips against its own bounds) */
        gx0 = d->ox / d->tw, gy0 = d->oy / d->th;
        gx1 = (uint32_t)((ex + d->tw - 1u) / d->tw), gy1 = (uint32_t)((ey + d->th - 1u) / d->th);
    } else {
        gx1 = (e->tgt_w + d->tw - 1u) / d->tw, gy1 = (e->tgt_h + d->th - 1u) / d->th;
    }
    if (gx1 - gx0 > lim->maxComputeWorkGroupCount[0] || gy1 - gy0 > lim->maxComputeWorkGroupCount[1]) { TILE(p).skip_local++; return false; }
    const uint32_t l[3] = { d->tw, d->th, d->td };
    VkShaderModule cs = tp->cs;
    if (bgra_mask) { cs = tile_variant_module(tp, bgra_mask, p); if (!cs) { TILE(p).skip_pipe_build++; return false; } }
    VkPipeline pl = tile_vk_pipeline(tp, l, pin, bgra_mask, cs, p);
    if (!pl) { TILE(p).skip_pipe_build++; return false; }
    /* descriptor set: every binding must be fed (Metal leaves unbound arguments undefined) */
    VkDescriptorSet ds = alloc_set(t, tp->dsl);
    if (!ds) { TILE(p).skip_desc++; return false; }
    VkDescriptorBufferInfo bi[TILE_MAX_BIND]; VkDescriptorImageInfo ii[TILE_MAX_BIND]; VkWriteDescriptorSet w[TILE_MAX_BIND];
    VkDeviceSize sb_align = lim->minStorageBufferOffsetAlignment ? lim->minStorageBufferOffsetAlignment : 1;
    for (uint32_t i = 0; i < tp->n_bind; i++) {
        const tile_bind_t *b = &tp->bind[i];
        uint32_t mi = (uint32_t)(int32_t)b->metal_index;
        memset(&w[i], 0, sizeof w[i]);
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, w[i].dstSet = ds, w[i].dstBinding = b->binding, w[i].descriptorCount = 1, w[i].descriptorType = vk_type(b->kind);
        if (b->kind == TB_BUFFER) {
            lagfx_a64_obj_t *o = t->buf[mi].b; uint64_t off = t->buf[mi].off;
            if (!o || off >= o->length) { TILE(p).skip_unbound++; return false; }
            VkDeviceSize range = o->length - off;
            if (lim->maxStorageBufferRange && range > lim->maxStorageBufferRange) range = lim->maxStorageBufferRange;
            VkBuffer dbuf = o->buf; VkDeviceSize doff = off;
            if (off % sb_align) {
                tile_note_unaligned(p, mi, off, o->length, sb_align, b->written);
                if (b->written) { TILE(p).skip_unaligned++, TILE(p).unaligned_written++; return false; }
                dbuf = tile_gather(e->x, p, o, off, range);
                if (!dbuf) { TILE(p).skip_unaligned++; return false; }
                doff = 0;
            }
            bi[i] = (VkDescriptorBufferInfo){ dbuf, doff, range }, w[i].pBufferInfo = &bi[i];
            lagfx_a64_res_buf_written(o);   /* the kernel may write it: a linear texture over it is stale from here on */
        } else if (b->kind == TB_TEXTURE) {
            lagfx_a64_obj_t *o = t->tex[mi];
            if (!o || o->linear) { TILE(p).skip_unbound++; return false; }
            for (uint32_t k = 0; k < 4u; k++) if (o == e->rts[k]) { TILE(p).skip_feedback++; return false; }
            VkImageView v = lagfx_a64_res_sample_view(e->x, o);
            if (!v) { TILE(p).skip_unbound++; return false; }
            ii[i] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL }, w[i].pImageInfo = &ii[i];
        } else if (b->kind == TB_SAMPLER) {
            if (!t->defsamp) {
                VkSamplerCreateInfo sc = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
                    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
                if (vkCreateSampler(vk->device, &sc, NULL, &t->defsamp) != VK_SUCCESS) { t->defsamp = VK_NULL_HANDLE; TILE(p).skip_unbound++; return false; }
            }
            ii[i] = (VkDescriptorImageInfo){ t->defsamp, VK_NULL_HANDLE, 0 }, w[i].pImageInfo = &ii[i];
        } else {   /* TB_RT: the imageblock of colour attachment metal_index (the OpName of the variable) */
            lagfx_a64_obj_t *o = e->rts[b->metal_index];
            ii[i] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, o->view, VK_IMAGE_LAYOUT_GENERAL }, w[i].pImageInfo = &ii[i];
        }
    }
    if (tp->n_bind) vkUpdateDescriptorSets(vk->device, tp->n_bind, w, 0, NULL);
    /* validated: end the rendering instance (attachments -> GENERAL), dispatch, resume with LOAD */
    e->suspend(e->ctx);
    VkCommandBuffer cb = lagfx_a64_res_cmdbuf(e->x);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl);
    if (tp->n_bind) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, tp->pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatchBase(cb, gx0, gy0, 0, gx1 - gx0, gy1 - gy0, 1);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    if (!e->resume(e->ctx)) TILE(p).resume_failed++;
    if (bgra_mask) TILE(p).bgra8_dispatches++;
    TILE(p).dispatches++, TILE(p).groups += (uint64_t)(gx1 - gx0) * (gy1 - gy0);
    if (d->region) TILE(p).region++; else TILE(p).whole++;
    if (pin) TILE(p).sg_pinned++;
    if (tp->simd_emul && tp->lane_mapped) TILE(p).sg_emulated++;
    p->diag2.exec.qcomtile.emul_dispatches++;
    if (vk->qcomtile.enabled) p->diag2.exec.qcomtile.emul_while_enabled++;
    return true;
}

#endif /* LAGFX_HAVE_VULKAN */
