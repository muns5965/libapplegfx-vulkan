/*
 * libapplegfx-vulkan — descriptor binding table -> VkDescriptorSetLayout / VkPipelineLayout / VkPipeline / descriptor writes (lavapipe + validation layer)
 * tests/vk-binding-layout-lavapipe.c
 *
 * SPDX-License-Identifier: MIT
 *
 * spirv-val accepts two descriptor types on one (set, binding); the Vulkan objects cannot represent that. This test checks the binding table at the Vulkan level,
 * with the Khronos validation layer (LAGFX_VK_VALIDATION=1; the message counter lagfx_vk_validation_msg_count must not move):
 *   1. synthetic modules: a sampler and an input attachment on one binding, VS + FS with different types on one binding -> layout creation FAILS (it used to keep the
 *      first and drop the other without a word); VS + FS with the SAME type on one binding -> ONE layout binding with VERTEX|FRAGMENT; a vertex-only binding is
 *      VERTEX only; 100 bindings of one stage -> all kept (it used to stop at 32 silently);
 *   2. translated fixture pairs (24 / 60 textures + constexpr samplers + input attachments): the derived layout creates, a graphics pipeline built from the pair
 *      validates, and descriptor writes of the type the layout says for EVERY binding validate.
 * Without a Vulkan device the test skips (77); without the validation layer it says so and still runs every other check.
 */
#include "libapplegfx-vulkan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LAGFX_HAVE_VULKAN
int main(void) { return 77; }
#else
#include "air/bitcode_reader.h"
#include "air2spv/spv_reflect.h"
#include "air2spv/translate.h"
#include "vulkan/descriptor_layout.h"
#include "vulkan/instance.h"
#include "vulkan/pipeline_build.h"

extern uint64_t lagfx_vk_validation_msg_count, lagfx_vk_validation_layout_msg_count;

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

/* ---- a minimal SPIR-V emitter for modules that declare chosen descriptor variables and statically use every one of them ---- */
typedef struct { uint32_t *w; size_t n, cap; } wbuf_t;
static void put(wbuf_t *b, uint32_t op, size_t nops, const uint32_t *ops) {
    if (b->n + nops + 1 > b->cap) { b->cap = (b->n + nops + 1) * 2 + 64; b->w = realloc(b->w, b->cap * 4); }
    b->w[b->n++] = (uint32_t)((nops + 1) << 16) | op;
    for (size_t i = 0; i < nops; i++) b->w[b->n++] = ops[i];
}
#define P(b, op, ...) do { const uint32_t o_[] = { __VA_ARGS__ }; put((b), (op), sizeof o_ / sizeof o_[0], o_); } while (0)
#define P0(b, op) put((b), (op), 0, NULL)

typedef struct { uint32_t set, binding; lagfx_spv_binding_kind_t kind; uint32_t ia_index; } vdecl_t;

/* stage: 0 vertex, 1 fragment. Returns malloc'd bytes. */
static uint8_t *make_module(int stage, const vdecl_t *d, size_t nd, size_t *out_bytes) {
    wbuf_t b = { 0 };
    bool ia = false;
    for (size_t i = 0; i < nd; i++) if (d[i].kind == LAGFX_SPV_BINDING_INPUT_ATTACHMENT) ia = true;
    enum { T_VOID = 1, T_FN, T_F32, T_V4, T_POUT, T_OUT, T_SAMP, T_PSAMP, T_IMG, T_PIMG, T_IA, T_PIA, T_RT, T_STRUCT, T_PSB, T_NULL, T_MAIN, T_LABEL, T_U32, T_FIRSTVAR = 32 };
    uint32_t bound = T_FIRSTVAR + (uint32_t)nd * 2 + 8;
    b.cap = 1024; b.w = malloc(b.cap * 4);
    b.w[0] = 0x07230203u; b.w[1] = 0x00010300u; b.w[2] = 0; b.w[3] = bound; b.w[4] = 0; b.n = 5;
    P(&b, 17, 1);                       /* OpCapability Shader */
    if (ia) P(&b, 17, 40);              /* InputAttachment */
    P(&b, 14, 0, 1);                    /* OpMemoryModel Logical GLSL450 */
    P(&b, 15, stage == 0 ? 0u : 4u, T_MAIN, 0x6e69616du, 0u, T_OUT);   /* OpEntryPoint Vertex/Fragment %main "main" %out */
    if (stage == 1) P(&b, 16, T_MAIN, 7);   /* OriginUpperLeft */
    if (stage == 0) P(&b, 71, T_OUT, 11, 0);   /* BuiltIn Position */
    else P(&b, 71, T_OUT, 30, 0);              /* Location 0 */
    for (size_t i = 0; i < nd; i++) {
        uint32_t v = T_FIRSTVAR + (uint32_t)i;
        P(&b, 71, v, 34, d[i].set);
        P(&b, 71, v, 33, d[i].binding);
        if (d[i].kind == LAGFX_SPV_BINDING_INPUT_ATTACHMENT) P(&b, 71, v, 43, d[i].ia_index);
    }
    P(&b, 71, T_RT, 6, 4);              /* ArrayStride 4 */
    P(&b, 72, T_STRUCT, 0, 35, 0);      /* MemberDecorate Offset 0 */
    P(&b, 71, T_STRUCT, 2);             /* Block */
    P(&b, 19, T_VOID);
    P(&b, 33, T_FN, T_VOID);
    P(&b, 22, T_F32, 32);
    P(&b, 21, T_U32, 32, 0);
    P(&b, 23, T_V4, T_F32, 4);
    P(&b, 32, T_POUT, 3, T_V4);
    P(&b, 59, T_POUT, T_OUT, 3);
    P(&b, 26, T_SAMP);
    P(&b, 32, T_PSAMP, 0, T_SAMP);
    P(&b, 25, T_IMG, T_F32, 1, 0, 0, 0, 1, 0);        /* 2D sampled */
    P(&b, 32, T_PIMG, 0, T_IMG);
    if (ia) {
        P(&b, 25, T_IA, T_F32, 6, 0, 0, 0, 2, 0);     /* SubpassData */
        P(&b, 32, T_PIA, 0, T_IA);
    }
    P(&b, 29, T_RT, T_F32);
    P(&b, 30, T_STRUCT, T_RT);
    P(&b, 32, T_PSB, 12, T_STRUCT);
    P(&b, 46, T_V4, T_NULL);
    for (size_t i = 0; i < nd; i++) {
        uint32_t ptr = d[i].kind == LAGFX_SPV_BINDING_SAMPLER ? T_PSAMP : d[i].kind == LAGFX_SPV_BINDING_SAMPLED_IMAGE ? T_PIMG : d[i].kind == LAGFX_SPV_BINDING_INPUT_ATTACHMENT ? T_PIA : T_PSB;
        P(&b, 59, ptr, T_FIRSTVAR + (uint32_t)i, d[i].kind == LAGFX_SPV_BINDING_STORAGE_BUFFER ? 12u : 0u);
    }
    P(&b, 54, T_VOID, T_MAIN, 0, T_FN);
    P(&b, 248, T_LABEL);
    for (size_t i = 0; i < nd; i++) {   /* static use of every variable */
        uint32_t res = T_FIRSTVAR + (uint32_t)nd + (uint32_t)i, var = T_FIRSTVAR + (uint32_t)i;
        switch (d[i].kind) {
        case LAGFX_SPV_BINDING_SAMPLER: P(&b, 61, T_SAMP, res, var); break;
        case LAGFX_SPV_BINDING_SAMPLED_IMAGE: P(&b, 61, T_IMG, res, var); break;
        case LAGFX_SPV_BINDING_INPUT_ATTACHMENT: P(&b, 61, T_IA, res, var); break;
        default: P(&b, 68, T_U32, res, var, 0); break;   /* OpArrayLength */
        }
    }
    P(&b, 62, T_OUT, T_NULL);
    P0(&b, 253);   /* OpReturn */
    P0(&b, 56);    /* OpFunctionEnd */
    *out_bytes = b.n * 4;
    return (uint8_t *)b.w;
}

/* ---- fixtures ---- */
static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)l + 1);
    if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
    fclose(f);
    *n = (size_t)l;
    return b;
}
static uint8_t *fixture(const char *name, size_t *n) {
    char path[512];
    const char *dirs[] = { SRCDIR "/fixtures/", "tests/fixtures/", "../tests/fixtures/" };
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; d++) { snprintf(path, sizeof path, "%s%s", dirs[d], name); uint8_t *b = slurp(path, n); if (b) return b; }
    return NULL;
}
#define FRAG_BASE 16u
/* the fragment fixture translated with the keys of the UberComposite vertex shader (as the resolver does), bindings offset */
static uint8_t *translate_fs(const char *fs_name, size_t *fsn) {
    size_t vn = 0, fn = 0;
    uint8_t *vbc = fixture("login_ubercomposite_vert.air.bc", &vn), *fbc = fixture(fs_name, &fn), *fs = NULL;
    lagfx_air_module_t *vm = NULL, *fm = NULL;
    lagfx_stage_keys_t k;
    memset(&k, 0, sizeof k);
    if (!vbc || !fbc || lagfx_air_module_open(vbc, vn, &vm) != LAGFX_OK || lagfx_air_module_open(fbc, fn, &fm) != LAGFX_OK) return NULL;
    if (!lagfx_air_stage_keys(vm, true, &k)) memset(&k, 0, sizeof k);
    if (lagfx_air2spv_translate_module_ex(fm, &k, &fs, fsn) != LAGFX_OK) fs = NULL;
    if (fs) lagfx_spv_offset_bindings(fs, *fsn, FRAG_BASE);
    lagfx_air_module_free(vm); lagfx_air_module_free(fm); free(vbc); free(fbc);
    return fs;
}

static VkShaderModule module(struct lagfx_vk_state *vk, const uint8_t *spv, size_t n) {
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = (const uint32_t *)spv };
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(vk->device, &ci, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

/* ---- layout-level checks on synthetic modules ---- */
static void synthetic(struct lagfx_vk_state *vk) {
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    size_t n[3];
    uint8_t *m[3];
    VkDescriptorSetLayoutBinding binds[160];
    const uint8_t *blobs[2];
    size_t lens[2];

    /* 1. a sampler and an input attachment on one binding (the device defect): the layout cannot hold both */
    { const vdecl_t d[] = { { 0, 40, LAGFX_SPV_BINDING_SAMPLER, 0 }, { 0, 40, LAGFX_SPV_BINDING_INPUT_ATTACHMENT, 0 }, { 0, 41, LAGFX_SPV_BINDING_INPUT_ATTACHMENT, 1 } };
      m[0] = make_module(1, d, 3, &n[0]);
      blobs[0] = m[0], lens[0] = n[0];
      lagfx_status_t st = lagfx_build_pipeline_layout_from_spv(vk->device, blobs, lens, 1, &dsl, &pl);
      CHECK(st != LAGFX_OK, "sampler + input attachment on set 0 binding 40: layout creation must report the collision (got %d)", (int)st);
      CHECK(dsl == VK_NULL_HANDLE && pl == VK_NULL_HANDLE, "no layout is returned for a colliding table");
      if (pl) vkDestroyPipelineLayout(vk->device, pl, NULL);
      if (dsl) vkDestroyDescriptorSetLayout(vk->device, dsl, NULL);
      dsl = VK_NULL_HANDLE, pl = VK_NULL_HANDLE;
      free(m[0]); }

    /* 2. VS and FS with DIFFERENT types on one binding */
    { const vdecl_t dv[] = { { 0, 0, LAGFX_SPV_BINDING_STORAGE_BUFFER, 0 } }, df[] = { { 0, 0, LAGFX_SPV_BINDING_SAMPLED_IMAGE, 0 } };
      m[0] = make_module(0, dv, 1, &n[0]); m[1] = make_module(1, df, 1, &n[1]);
      blobs[0] = m[0], lens[0] = n[0], blobs[1] = m[1], lens[1] = n[1];
      lagfx_status_t st = lagfx_build_pipeline_layout_from_spv(vk->device, blobs, lens, 2, &dsl, &pl);
      CHECK(st != LAGFX_OK, "VS storage buffer + FS sampled image on binding 0 (no stage offset): must be reported (got %d)", (int)st);
      if (pl) vkDestroyPipelineLayout(vk->device, pl, NULL);
      if (dsl) vkDestroyDescriptorSetLayout(vk->device, dsl, NULL);
      dsl = VK_NULL_HANDLE, pl = VK_NULL_HANDLE;
      free(m[0]); free(m[1]); }

    /* 3. VS and FS with the SAME type on one binding: one binding, both stages; a vertex-only binding is vertex only */
    { const vdecl_t dv[] = { { 0, 0, LAGFX_SPV_BINDING_STORAGE_BUFFER, 0 }, { 0, 1, LAGFX_SPV_BINDING_STORAGE_BUFFER, 0 } }, df[] = { { 0, 0, LAGFX_SPV_BINDING_STORAGE_BUFFER, 0 }, { 0, 2, LAGFX_SPV_BINDING_SAMPLED_IMAGE, 0 } };
      m[0] = make_module(0, dv, 2, &n[0]); m[1] = make_module(1, df, 2, &n[1]);
      blobs[0] = m[0], lens[0] = n[0], blobs[1] = m[1], lens[1] = n[1];
      uint32_t nb = lagfx_layout_bindings_from_spv(blobs, lens, 2, binds);
      CHECK(nb == 3, "shared binding 0 + vertex binding 1 + fragment binding 2 = 3 layout bindings (got %u)", nb);
      for (uint32_t i = 0; i < nb && i < 3; i++) {
          VkShaderStageFlags want = binds[i].binding == 0 ? (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT) : binds[i].binding == 1 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
          CHECK(binds[i].stageFlags == want, "binding %u stage flags 0x%x, want 0x%x", binds[i].binding, binds[i].stageFlags, want);
      }
      CHECK(lagfx_build_pipeline_layout_from_spv(vk->device, blobs, lens, 2, &dsl, &pl) == LAGFX_OK && dsl && pl, "the layout of the shared table is created");
      if (pl) vkDestroyPipelineLayout(vk->device, pl, NULL);
      if (dsl) vkDestroyDescriptorSetLayout(vk->device, dsl, NULL);
      dsl = VK_NULL_HANDLE, pl = VK_NULL_HANDLE;
      free(m[0]); free(m[1]); }

    /* 4. growth: 100 bindings of one stage (it used to stop at 32 without a word) */
    { vdecl_t d[100];
      for (unsigned i = 0; i < 100; i++) d[i] = (vdecl_t){ 0, i, i % 3 == 0 ? LAGFX_SPV_BINDING_STORAGE_BUFFER : i % 3 == 1 ? LAGFX_SPV_BINDING_SAMPLED_IMAGE : LAGFX_SPV_BINDING_SAMPLER, 0 };
      m[0] = make_module(1, d, 100, &n[0]);
      blobs[0] = m[0], lens[0] = n[0];
      uint32_t nb = lagfx_layout_bindings_from_spv(blobs, lens, 1, binds);
      CHECK(nb == 100, "100 declared bindings -> 100 layout bindings (got %u)", nb);
      lagfx_status_t st = lagfx_build_pipeline_layout_from_spv(vk->device, blobs, lens, 1, &dsl, &pl);
      CHECK(st == LAGFX_OK && dsl && pl, "a 100-binding layout is created (status %d)", (int)st);
      if (pl) vkDestroyPipelineLayout(vk->device, pl, NULL);
      if (dsl) vkDestroyDescriptorSetLayout(vk->device, dsl, NULL);
      dsl = VK_NULL_HANDLE, pl = VK_NULL_HANDLE;
      free(m[0]); }
}

/* device-local-or-host memory for a buffer / image (lavapipe: any type works) */
static VkDeviceMemory bind_mem(struct lagfx_vk_state *vk, VkMemoryRequirements rq, VkBuffer buf, VkImage img) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(vk->phys_device, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (!(rq.memoryTypeBits & (1u << i))) continue;
        VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = i };
        VkDeviceMemory m = VK_NULL_HANDLE;
        if (vkAllocateMemory(vk->device, &ai, NULL, &m) != VK_SUCCESS) continue;
        if ((buf ? vkBindBufferMemory(vk->device, buf, m, 0) : vkBindImageMemory(vk->device, img, m, 0)) == VK_SUCCESS) return m;
        vkFreeMemory(vk->device, m, NULL);
    }
    return VK_NULL_HANDLE;
}

/* local copies of the VK_KHR_dynamic_rendering_local_read structs (the Vulkan headers of the build predate them) */
typedef struct { VkStructureType sType; const void *pNext; uint32_t colorAttachmentCount; const uint32_t *pColorAttachmentLocations; } loc_info_t;
typedef struct { VkStructureType sType; const void *pNext; uint32_t colorAttachmentCount; const uint32_t *pColorAttachmentInputIndices; const uint32_t *pDepthInputAttachmentIndex; const uint32_t *pStencilInputAttachmentIndex; } idx_info_t;

/* ---- translated pair: layout, pipeline, descriptor writes ---- */
static void translated(struct lagfx_vk_state *vk, const char *fs_name, bool with_fetch) {
    size_t fsn = 0, vsn = 0;
    uint8_t *fs = translate_fs(fs_name, &fsn);
    const vdecl_t none[1] = { { 0, 0, LAGFX_SPV_BINDING_STORAGE_BUFFER, 0 } };   /* a vertex module that writes only Position and has one buffer at binding 0 */
    uint8_t *vs = make_module(0, none, 1, &vsn);
    printf("pair %s:\n", fs_name);
    CHECK(fs != NULL, "%s translates", fs_name);
    if (!fs) { free(vs); return; }
    const uint8_t *blobs[2] = { vs, fs };
    const size_t lens[2] = { vsn, fsn };
    uint64_t v0 = lagfx_vk_validation_msg_count, l0 = lagfx_vk_validation_layout_msg_count;
    VkDescriptorSetLayoutBinding binds[160];
    uint32_t nb = lagfx_layout_bindings_from_spv(blobs, lens, 2, binds);
    lagfx_spv_binding_t rf[160];
    size_t nrf = lagfx_spv_reflect_bindings(fs, fsn, rf, 160);
    printf("  layout: %u bindings (fragment variables: %zu)\n", nb, nrf);
    CHECK(nb == nrf + 1, "every fragment variable owns a layout binding (+1 vertex buffer): %u vs %zu + 1", nb, nrf);
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    CHECK(lagfx_build_pipeline_layout_from_spv(vk->device, blobs, lens, 2, &dsl, &pl) == LAGFX_OK && dsl && pl, "layout created");
    /* every reflected variable is declared by the layout with its own descriptor type */
    for (size_t i = 0; i < nrf; i++) {
        VkDescriptorType want = rf[i].kind == LAGFX_SPV_BINDING_SAMPLED_IMAGE ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : rf[i].kind == LAGFX_SPV_BINDING_SAMPLER ? VK_DESCRIPTOR_TYPE_SAMPLER :
                                rf[i].kind == LAGFX_SPV_BINDING_INPUT_ATTACHMENT ? VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bool found = false;
        for (uint32_t k = 0; k < nb; k++) if (binds[k].binding == rf[i].binding) { found = true; CHECK(binds[k].descriptorType == want, "binding %u declared as type %d, shader uses %d", rf[i].binding, (int)binds[k].descriptorType, (int)want); }
        CHECK(found, "binding %u (kind %d) is in the layout", rf[i].binding, (int)rf[i].kind);
    }
    /* the pipeline built from the pair validates against that layout */
    VkShaderModule vm = module(vk, vs, vsn), fm = module(vk, fs, fsn);
    CHECK(vm && fm, "shader modules");
    if (vm && fm && pl && (!with_fetch || vk->have_local_read)) {
        lagfx_pipeline_desc_t d;
        memset(&d, 0, sizeof d);
        d.vertex_shader = vm, d.fragment_shader = fm, d.layout = pl, d.color_format = VK_FORMAT_R8G8B8A8_UNORM, d.caps_enabled = vk->caps_enabled;
        d.n_color = with_fetch ? 2 : 1;
        const uint32_t ident[4] = { 0, 1, 2, 3 };
        loc_info_t li = { .sType = (VkStructureType)1000232001, .colorAttachmentCount = 2, .pColorAttachmentLocations = ident };
        idx_info_t ii = { .sType = (VkStructureType)1000232002, .pNext = NULL, .colorAttachmentCount = 2, .pColorAttachmentInputIndices = ident };
        if (with_fetch) {   /* input attachments under dynamic rendering need the local-read mapping structs (identity: input attachment n = colour attachment n) */
            d.color_formats[0] = d.color_formats[1] = VK_FORMAT_R8G8B8A8_UNORM;
            li.pNext = &ii;
            d.diag_gpci_pnext = &li;
        }
        VkPipeline p = VK_NULL_HANDLE;
        lagfx_status_t st = lagfx_pipeline_build(vk->device, &d, &p);
        CHECK(st == LAGFX_OK && p, "pipeline built (status %d)", (int)st);
        if (p) vkDestroyPipeline(vk->device, p, NULL);
    } else if (with_fetch && !vk->have_local_read) printf("  (no VK_KHR_dynamic_rendering_local_read: the input-attachment pipeline is not built, layout and writes are checked)\n");
    /* descriptor writes of the layout's type for every binding */
    if (dsl) {
        VkDescriptorPoolSize ps[] = { { VK_DESCRIPTOR_TYPE_SAMPLER, 256 }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256 }, { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 16 } };
        VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 4, .pPoolSizes = ps };
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkBuffer buf = VK_NULL_HANDLE; VkImage img = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkSampler smp = VK_NULL_HANDLE;
        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { 4, 4, 1 }, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        VkDeviceMemory mb = VK_NULL_HANDLE, mi = VK_NULL_HANDLE;
        bool ok = vkCreateDescriptorPool(vk->device, &pci, NULL, &pool) == VK_SUCCESS && vkCreateBuffer(vk->device, &bci, NULL, &buf) == VK_SUCCESS && vkCreateImage(vk->device, &ici, NULL, &img) == VK_SUCCESS && vkCreateSampler(vk->device, &sci, NULL, &smp) == VK_SUCCESS;
        if (ok) {
            VkMemoryRequirements rq;
            vkGetBufferMemoryRequirements(vk->device, buf, &rq); mb = bind_mem(vk, rq, buf, VK_NULL_HANDLE);
            vkGetImageMemoryRequirements(vk->device, img, &rq); mi = bind_mem(vk, rq, VK_NULL_HANDLE, img);
            ok = mb && mi;
        }
        VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        ok = ok && vkCreateImageView(vk->device, &vci, NULL, &view) == VK_SUCCESS;
        VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
        ok = ok && vkAllocateDescriptorSets(vk->device, &ai, &set) == VK_SUCCESS;
        CHECK(ok, "descriptor pool / set / resources for the writes");
        if (ok) {
            VkWriteDescriptorSet w[160];
            VkDescriptorBufferInfo bi = { buf, 0, 256 };
            VkDescriptorImageInfo ii_img = { VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, ii_smp = { smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED }, ii_ia = { VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL };
            for (uint32_t i = 0; i < nb; i++) {
                memset(&w[i], 0, sizeof w[i]);
                w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, w[i].dstSet = set, w[i].dstBinding = binds[i].binding, w[i].descriptorCount = 1, w[i].descriptorType = binds[i].descriptorType;
                if (binds[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) w[i].pBufferInfo = &bi;
                else if (binds[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) w[i].pImageInfo = &ii_smp;
                else if (binds[i].descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) w[i].pImageInfo = &ii_ia;
                else w[i].pImageInfo = &ii_img;
            }
            vkUpdateDescriptorSets(vk->device, nb, w, 0, NULL);
        }
        if (view) vkDestroyImageView(vk->device, view, NULL);
        if (img) vkDestroyImage(vk->device, img, NULL);
        if (buf) vkDestroyBuffer(vk->device, buf, NULL);
        if (smp) vkDestroySampler(vk->device, smp, NULL);
        if (pool) vkDestroyDescriptorPool(vk->device, pool, NULL);
        if (mb) vkFreeMemory(vk->device, mb, NULL);
        if (mi) vkFreeMemory(vk->device, mi, NULL);
    }
    CHECK(lagfx_vk_validation_layout_msg_count == l0, "%llu descriptor-type / layout validation message(s) for the pair (LAGFX_VK_VALIDATION_PRINT=1 shows them)", (unsigned long long)(lagfx_vk_validation_layout_msg_count - l0));
    /* every other message must be absent too, except what a layer older than VK_KHR_dynamic_rendering_local_read says about the input-attachment pipeline of the fetch pairs (it does not know the mapping structs) */
    if (!with_fetch) CHECK(lagfx_vk_validation_msg_count == v0, "%llu validation message(s) for the pair", (unsigned long long)(lagfx_vk_validation_msg_count - v0));
    else printf("  (%llu other validation message(s): the layer predates local read, they are about the fetch pipeline state, not the bindings)\n", (unsigned long long)(lagfx_vk_validation_msg_count - v0 - (lagfx_vk_validation_layout_msg_count - l0)));
    if (vm) vkDestroyShaderModule(vk->device, vm, NULL);
    if (fm) vkDestroyShaderModule(vk->device, fm, NULL);
    if (pl) vkDestroyPipelineLayout(vk->device, pl, NULL);
    if (dsl) vkDestroyDescriptorSetLayout(vk->device, dsl, NULL);
    free(vs); free(fs);
}

/* the canary: a layout that really disagrees with a statically used binding must make the layer speak, otherwise "0 messages" above proves nothing */
static bool canary(struct lagfx_vk_state *vk) {
    const vdecl_t dv[] = { { 0, 0, LAGFX_SPV_BINDING_STORAGE_BUFFER, 0 } }, df[] = { { 0, 1, LAGFX_SPV_BINDING_SAMPLED_IMAGE, 0 } };
    size_t vn, fn;
    uint8_t *v = make_module(0, dv, 1, &vn), *f = make_module(1, df, 1, &fn);
    VkDescriptorSetLayoutBinding b[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL }, { 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };   /* binding 1 declared as a sampler, used as an image */
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b };
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(vk->device, &dli, NULL, &dsl);
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
    vkCreatePipelineLayout(vk->device, &pli, NULL, &pl);
    VkShaderModule vm = module(vk, v, vn), fm = module(vk, f, fn);
    uint64_t c0 = lagfx_vk_validation_layout_msg_count;
    lagfx_pipeline_desc_t d;
    memset(&d, 0, sizeof d);
    d.vertex_shader = vm, d.fragment_shader = fm, d.layout = pl, d.color_format = VK_FORMAT_R8G8B8A8_UNORM, d.caps_enabled = vk->caps_enabled;
    VkPipeline p = VK_NULL_HANDLE;
    lagfx_pipeline_build_quiet(1);
    (void)lagfx_pipeline_build(vk->device, &d, &p);
    lagfx_pipeline_build_quiet(0);
    bool spoke = lagfx_vk_validation_layout_msg_count > c0;
    if (p) vkDestroyPipeline(vk->device, p, NULL);
    vkDestroyShaderModule(vk->device, vm, NULL); vkDestroyShaderModule(vk->device, fm, NULL);
    vkDestroyPipelineLayout(vk->device, pl, NULL); vkDestroyDescriptorSetLayout(vk->device, dsl, NULL);
    free(v); free(f);
    return spoke;
}

int main(void) {
    setenv("LAGFX_VK_VALIDATION", "1", 1);
    struct lagfx_vk_state *vk = NULL;
    lagfx_device_descriptor_t desc;
    memset(&desc, 0, sizeof desc);
    if (lagfx_vk_init(&vk, &desc) != LAGFX_OK || !vk || !vk->initialized) { printf("no usable Vulkan device: skip\n"); return 77; }
    bool layer = canary(vk);
    printf("validation layer: %s\n", layer ? "ACTIVE (a deliberately wrong layout is reported)" : "NOT active in this environment: the Vulkan-level message checks below prove nothing, only the status / table checks do");
    synthetic(vk);
    translated(vk, "bind_nofetch_fs.air.bc", false);
    translated(vk, "bind_many_fs.air.bc", true);
    translated(vk, "bind_wide_fs.air.bc", true);
    if (g_fail) { printf("%d check(s) failed\n", g_fail); return 1; }
    printf("vk binding layout: ok\n");
    return 0;
}
#endif
