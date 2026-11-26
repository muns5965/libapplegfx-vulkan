/* SPDX-License-Identifier: MIT */
/* libapplegfx-vulkan — the -13 pipeline-creation matrix (see pipeline_matrix.h). DIAGNOSTIC only, opt-in, never changes the normal build. */
#include "pipeline_matrix.h"

#ifdef LAGFX_HAVE_VULKAN

#include "common/log.h"
#include "descriptor_layout.h"
#include "pipeline_bundle.h"
#include "spv_iface.h"
#include "spv_variants.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* local copies of the VK_KHR_dynamic_rendering_local_read structs (the Vulkan headers this library is built with predate them; sType values from the registry) */
#define STYPE_RENDERING_ATTACHMENT_LOCATION_INFO ((VkStructureType)1000232001)
#define STYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO ((VkStructureType)1000232002)
typedef struct { VkStructureType sType; const void *pNext; uint32_t colorAttachmentCount; const uint32_t *pColorAttachmentLocations; } loc_info_t;
typedef struct { VkStructureType sType; const void *pNext; uint32_t colorAttachmentCount; const uint32_t *pColorAttachmentInputIndices; const uint32_t *pDepthInputAttachmentIndex; const uint32_t *pStencilInputAttachmentIndex; } idx_info_t;

/* recipe bits (pipeline_matrix.h LAGFX_MX_*): every row is a set of these; a single-bit row is an INDEPENDENT variant, M10 combines the single-bit rows that changed the outcome */
enum {
    R_NORO = LAGFX_MX_NORO, R_V13 = LAGFX_MX_V13, R_BLOCK = LAGFX_MX_BLOCK, R_IAFIRST = LAGFX_MX_IAFIRST, R_IASET1 = LAGFX_MX_IASET1, R_STAGES = LAGFX_MX_STAGES,
    R_MAPBOTH = LAGFX_MX_MAPBOTH, R_MAPIDX = LAGFX_MX_MAPIDX, R_MAPLOC = LAGFX_MX_MAPLOC, R_LEGACY = LAGFX_MX_LEGACY, R_M8 = LAGFX_MX_M8, R_M8C = LAGFX_MX_M8C,
    R_UG = LAGFX_MX_UG, R_UH = LAGFX_MX_UH, R_UI = LAGFX_MX_UI, R_UJ = LAGFX_MX_UJ, R_UK = LAGFX_MX_UK,
};
#define R_LAYOUT_BITS (R_BLOCK | R_IAFIRST | R_IASET1 | R_STAGES)
#define R_MAP_BITS (R_MAPBOTH | R_MAPIDX | R_MAPLOC)
#define R_UNDEF_BITS (R_UG | R_UH | R_UI | R_UJ | R_UK)

typedef struct {
    VkDevice dev;
    const lagfx_matrix_env_t *env;
    lagfx_pipeline_desc_t d0;
    const uint32_t *vs, *fs;
    size_t vsn, fsn;                         /* bytes */
    const VkDescriptorSetLayoutBinding *bind;
    uint32_t nbind;
    lagfx_spv_iface_t *fsi;
    uint64_t vh, fh;
    uint32_t ok, fail, na, combos;
    char ids_ok[160], ids_fail[160], ids_na[160];
    struct { uint32_t mask; VkResult r; bool applicable; char id[8]; } single[24];   /* single-bit rows, for M10 */
    uint32_t nsingle;
    VkResult r0;
    uint32_t cap_lines, lines;               /* bounded output */
} mctx_t;

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6; }

static void line(mctx_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void line(mctx_t *c, const char *fmt, ...) {
    char buf[600];
    va_list ap;
    if (c->lines >= c->cap_lines) return;
    c->lines++;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (c->env->emit) c->env->emit(c->env->user, buf);
}

static void add_id(char *list, size_t cap, const char *id) {
    size_t n = strlen(list);
    if (n + strlen(id) + 2 >= cap) { if (n + 4 < cap) strcat(list, ",.."); return; }
    if (n) list[n++] = ',';
    strcpy(list + n, id);
}

static uint32_t ncol_of(const lagfx_pipeline_desc_t *d) { return (d->n_color >= 2u && d->n_color <= 4u) ? d->n_color : 1u; }

/* ---- layouts ---- */
typedef struct { uint32_t set0, bind0, set1, bind1; } lagfx_spv_rebind_t;   /* (set, binding) -> (set, binding) of one layout binding */
#define MAXB 64u
typedef struct { VkDescriptorSetLayout dsl[2]; VkPipelineLayout pl; uint32_t nset; } lay_t;

static void lay_destroy(VkDevice dev, lay_t *l) {
    if (l->pl) vkDestroyPipelineLayout(dev, l->pl, NULL);
    for (uint32_t i = 0; i < l->nset; i++) if (l->dsl[i]) vkDestroyDescriptorSetLayout(dev, l->dsl[i], NULL);
    memset(l, 0, sizeof *l);
}

static bool is_ia_binding(const mctx_t *c, uint32_t set, uint32_t b, uint32_t *index) {
    for (uint32_t i = 0; c->fsi && i < c->fsi->n_ia; i++) if (c->fsi->ia[i].set == set && c->fsi->ia[i].binding == b) { if (index) *index = c->fsi->ia[i].has_index ? c->fsi->ia[i].index : i; return true; }
    return false;
}

/* The renumbering the recipe applies to (set 0, binding) pairs, as a SPIR-V rebind map. Returns the number of entries. */
static uint32_t make_map(const mctx_t *c, uint32_t mask, lagfx_spv_rebind_t *m) {
    uint32_t n = 0, nia = c->fsi ? c->fsi->n_ia : 0;
    for (uint32_t i = 0; i < c->nbind && n < MAXB; i++) {
        uint32_t b = c->bind[i].binding, idx = 0;
        bool ia = is_ia_binding(c, 0, b, &idx);
        lagfx_spv_rebind_t e = { 0, b, 0, b };
        if (mask & R_IAFIRST) e.bind1 = ia ? idx : b + nia;
        if (mask & R_IASET1) { if (ia) e.set1 = 1, e.bind1 = idx; }
        m[n++] = e;
    }
    return n;
}

/* Builds the layout of a recipe from the original bindings. Returns a VkResult; *note describes what was done. */
static VkResult make_layout(mctx_t *c, uint32_t mask, lay_t *l, char *note, size_t ncap) {
    VkDescriptorSetLayoutBinding sb[2][MAXB + 8];
    uint32_t ns[2] = { 0, 0 };
    lagfx_spv_rebind_t map[MAXB];
    uint32_t nm = make_map(c, mask, map);
    note[0] = 0;
    for (uint32_t i = 0; i < c->nbind; i++) {
        VkDescriptorSetLayoutBinding b = c->bind[i];
        uint32_t set = 0;
        if (nm) { b.binding = map[i].bind1; set = map[i].set1; }
        if ((mask & R_STAGES) && b.descriptorType != VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;   /* an input attachment must stay FRAGMENT-only (VUID 01510) */
        sb[set][ns[set]++] = b;
    }
    if (mask & R_BLOCK) {
        uint32_t base = ~0u, nia = c->fsi ? c->fsi->n_ia : 0;
        for (uint32_t i = 0; i < nia; i++) if (c->fsi->ia[i].binding < base) base = c->fsi->ia[i].binding;
        uint32_t want = c->env->max_color_att ? c->env->max_color_att : 8u;
        if (want > 8u) want = 8u;
        if (c->env->max_per_stage_input_att && want > c->env->max_per_stage_input_att) want = c->env->max_per_stage_input_att;
        uint32_t added = 0;
        for (uint32_t k = 0; k < want; k++) {
            bool have = false;
            for (uint32_t i = 0; i < ns[0]; i++) have |= sb[0][i].binding == base + k;
            if (have) continue;
            if (ns[0] >= MAXB) break;
            sb[0][ns[0]++] = (VkDescriptorSetLayoutBinding){ .binding = base + k, .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
            added++;
        }
        snprintf(note, ncap, "%u unused INPUT_ATTACHMENT binding(s) added so bindings %u..%u are all declared", added, base, base + want - 1u);
    }
    l->nset = ns[1] ? 2u : 1u;
    for (uint32_t s = 0; s < l->nset; s++) {
        VkDescriptorSetLayoutCreateInfo dl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = ns[s], .pBindings = sb[s] };
        VkResult r = vkCreateDescriptorSetLayout(c->dev, &dl, NULL, &l->dsl[s]);
        if (r != VK_SUCCESS) { uint32_t made = s; l->nset = made; lay_destroy(c->dev, l); return r; }
    }
    VkPipelineLayoutCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = l->nset, .pSetLayouts = l->dsl };
    VkResult r = vkCreatePipelineLayout(c->dev, &pi, NULL, &l->pl);
    if (r != VK_SUCCESS) lay_destroy(c->dev, l);
    return r;
}

/* ---- legacy render pass (M6) ---- */
static VkRenderPass make_legacy_rp(mctx_t *c, const lagfx_pipeline_desc_t *d, bool ro_flag, VkResult *rr) {
    const uint32_t ncol = ncol_of(d);
    VkAttachmentDescription ad[4];
    VkAttachmentReference cref[4], iref[16];
    uint32_t nin = 0;
    for (uint32_t i = 0; i < ncol; i++) {
        VkFormat f = (VkFormat)(ncol > 1 ? d->color_formats[i] : (d->color_format ? d->color_format : VK_FORMAT_B8G8R8A8_UNORM));
        ad[i] = (VkAttachmentDescription){ .format = f == VK_FORMAT_UNDEFINED ? VK_FORMAT_R16G16B16A16_SFLOAT : f, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
        cref[i] = (VkAttachmentReference){ f == VK_FORMAT_UNDEFINED ? VK_ATTACHMENT_UNUSED : i, VK_IMAGE_LAYOUT_GENERAL };
    }
    for (uint32_t i = 0; c->fsi && i < c->fsi->n_ia; i++) if (c->fsi->ia[i].has_index && c->fsi->ia[i].index + 1 > nin) nin = c->fsi->ia[i].index + 1;
    if (nin > 16) nin = 16;
    for (uint32_t k = 0; k < nin; k++) {
        iref[k] = (VkAttachmentReference){ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL };
        for (uint32_t i = 0; i < c->fsi->n_ia; i++) if (c->fsi->ia[i].has_index && c->fsi->ia[i].index == k && k < ncol) iref[k].attachment = k;
    }
    VkSubpassDescription sp = { .flags = ro_flag ? 0x10u /* VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT */ : 0u, .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .inputAttachmentCount = nin, .pInputAttachments = iref, .colorAttachmentCount = ncol, .pColorAttachments = cref };
    VkSubpassDependency dep = { 0, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_INPUT_ATTACHMENT_READ_BIT, VK_DEPENDENCY_BY_REGION_BIT };
    VkRenderPassCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = ncol, .pAttachments = ad, .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = nin ? 1u : 0u, .pDependencies = &dep };
    VkRenderPass rp = VK_NULL_HANDLE;
    *rr = vkCreateRenderPass(c->dev, &ri, NULL, &rp);
    return rp;
}

static uint32_t *slurp_words(const char *path, size_t *bytes) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *w = n > 0 && n % 4 == 0 && n <= (16l << 20) ? malloc((size_t)n) : NULL;
    if (!w || fread(w, 1, (size_t)n, f) != (size_t)n) { free(w); w = NULL; }
    fclose(f);
    if (w) *bytes = (size_t)n;
    return w;
}

typedef int (*rw_fn)(const uint32_t *, size_t, uint32_t **, size_t *, char *, size_t);

/* Applies the SPIR-V transforms of `mask` (in a fixed order) to a copy of one module. *cur / *curn: the module the next transform reads (starts as the original); `own` frees the previous.
 * Returns 1 when at least one transform applied, 0 when none did (the module is the original), -1 on error. Appends to note. */
static int xform_module(mctx_t *c, uint32_t mask, const uint32_t *orig, size_t orign, uint32_t **outw, size_t *outn, char *note, size_t ncap, bool is_fs) {
    uint32_t *cur = NULL;
    size_t curn = orign;
    const uint32_t *in = orig;
    int applied = 0;
    char info[200];
    struct { uint32_t bit; rw_fn fn; const char *tag; } steps[] = {
        { R_V13, lagfx_spv_variant_downgrade13, "v1.3" }, { R_UG, lagfx_spv_variant_undef_composites, "undef-g" }, { R_UH, lagfx_spv_variant_undef_struct_ptr, "undef-h" },
        { R_UI, lagfx_spv_variant_drop_dead_undef, "undef-i" }, { R_UJ, lagfx_spv_variant_undef_struct, "undef-j" }, { R_UK, lagfx_spv_variant_undef_returns, "undef-k" } };
    for (size_t s = 0; s < sizeof steps / sizeof steps[0]; s++) {
        if (!(mask & steps[s].bit)) continue;
        uint32_t *o = NULL;
        size_t on = 0;
        info[0] = 0;
        int n = steps[s].fn(in, curn, &o, &on, info, sizeof info);
        if (n < 0) { free(cur); return -1; }
        if (n > 0) {
            free(cur);
            cur = o, in = o, curn = on, applied++;
        }
        if (is_fs || n > 0) { size_t l = strlen(note); snprintf(note + l, ncap - l, "%s%s %s: %s", l ? " | " : "", is_fs ? "fs" : "vs", steps[s].tag, info[0] ? info : (n > 0 ? "applied" : "not applicable")); }
    }
    if (mask & (R_IAFIRST | R_IASET1)) {
        uint32_t *o = NULL;
        size_t on = 0;
        info[0] = 0;
        int n = lagfx_spv_variant_ia_rebind(in, curn, (mask & R_IASET1) ? 1u : 0u, c->fsi ? c->fsi->n_ia : 0u, &o, &on, info, sizeof info);
        if (n < 0) { free(cur); return -1; }
        if (n > 0) { free(cur); cur = o, in = o, curn = on, applied++; }
        if (is_fs || n > 0) { size_t l = strlen(note); snprintf(note + l, ncap - l, "%s%s rebind: %s", l ? " | " : "", is_fs ? "fs" : "vs", info); }
    }
    if (!applied) { *outw = NULL, *outn = 0; return 0; }
    *outw = cur, *outn = curn;
    return 1;
}

/* Builds `d` with the given modules / layout; the result is the VkResult (VK_SUCCESS = built). Quiet: no failure log, no bundle, no recursion into the matrix. */
static VkResult build_pipeline(mctx_t *c, const lagfx_pipeline_desc_t *dsrc, const uint32_t *vs, size_t vsn, const uint32_t *fs, size_t fsn, VkPipelineLayout layout) {
    lagfx_pipeline_desc_t d = *dsrc;
    VkShaderModule vm = VK_NULL_HANDLE, fm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = vsn, .pCode = vs };
    VkResult r = vkCreateShaderModule(c->dev, &ci, NULL, &vm);
    if (r == VK_SUCCESS) { ci.codeSize = fsn, ci.pCode = fs; r = vkCreateShaderModule(c->dev, &ci, NULL, &fm); }
    if (r == VK_SUCCESS) {
        d.vertex_shader = vm, d.fragment_shader = fm, d.layout = layout;
        d.vertex_spv = vs, d.vertex_spv_bytes = vsn, d.fragment_spv = fs, d.fragment_spv_bytes = fsn, d.diag = NULL;
        VkPipeline pl = VK_NULL_HANDLE;
        lagfx_pipeline_bundle_suppress(1);
        lagfx_pipeline_build_quiet(1);
        lagfx_status_t st = lagfx_pipeline_build(c->dev, &d, &pl);
        lagfx_pipeline_build_quiet(0);
        lagfx_pipeline_bundle_suppress(0);
        r = st == LAGFX_OK ? VK_SUCCESS : (VkResult)lagfx_pipeline_build_last_result();
        if (st != LAGFX_OK && r == VK_SUCCESS) r = VK_ERROR_UNKNOWN;   /* refused before vkCreateGraphicsPipelines */
        if (pl) vkDestroyPipeline(c->dev, pl, NULL);
    }
    if (vm) vkDestroyShaderModule(c->dev, vm, NULL);
    if (fm) vkDestroyShaderModule(c->dev, fm, NULL);
    return r;
}

static const char *res_name(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "OK";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INVALID_SHADER_NV: return "VK_ERROR_INVALID_SHADER_NV";
    case VK_PIPELINE_COMPILE_REQUIRED: return "VK_PIPELINE_COMPILE_REQUIRED";
    default: return "VK_ERROR_(other)";
    }
}

/* One row. Returns VK_SUCCESS / the failing VkResult; *applicable = false for n/a. `mask` is the recipe, `legacy_ro` selects the M6 subpass flag. */
static VkResult run_recipe(mctx_t *c, uint32_t mask, bool *applicable, char *why, size_t wcap) {
    *applicable = true;
    why[0] = 0;
    lagfx_pipeline_desc_t d = c->d0;
    const uint32_t *vs = c->vs, *fs = c->fs;
    size_t vsn = c->vsn, fsn = c->fsn;
    uint32_t *vo = NULL, *fo = NULL, *m8v = NULL, *m8f = NULL;
    size_t von = 0, fon = 0;
    lay_t lay = { 0 };
    VkRenderPass rp = VK_NULL_HANDLE;
    VkResult r = VK_SUCCESS;
    const uint32_t ncol = ncol_of(&d);
    uint32_t ident[4] = { 0, 1, 2, 3 };
    loc_info_t li = { .sType = STYPE_RENDERING_ATTACHMENT_LOCATION_INFO, .colorAttachmentCount = ncol, .pColorAttachmentLocations = ident };
    idx_info_t ii = { .sType = STYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO, .colorAttachmentCount = ncol, .pColorAttachmentInputIndices = ident };
    char note[300];
    note[0] = 0;
    /* applicability */
    if ((mask & R_NORO) && !c->d0.raster_order_access) { *applicable = false; snprintf(why, wcap, "the failing state has no rasterization-order flag"); goto out; }
    if ((mask & (R_MAP_BITS)) && !c->env->have_local_read) { *applicable = false; snprintf(why, wcap, "dynamicRenderingLocalRead not enabled on this device (the mapping structs need it)"); goto out; }
    if ((mask & R_LEGACY) && !c->fsi->n_ia) { *applicable = false; snprintf(why, wcap, "the fragment shader has no input attachment"); goto out; }
    if ((mask & R_LEGACY) && !(mask & R_NORO) && c->d0.raster_order_access && !c->env->have_raster_order) { *applicable = false; snprintf(why, wcap, "rasterization-order access not enabled on this device (M6a)"); goto out; }
    if ((mask & (R_IAFIRST | R_IASET1 | R_BLOCK)) && !c->fsi->n_ia) { *applicable = false; snprintf(why, wcap, "the fragment shader has no input attachment"); goto out; }
    if ((mask & R_LAYOUT_BITS) && c->nbind > MAXB) { *applicable = false; snprintf(why, wcap, "more than %u layout bindings", MAXB); goto out; }
    if ((mask & R_LAYOUT_BITS) && (!c->bind || !c->nbind)) { *applicable = false; snprintf(why, wcap, "the layout bindings are unknown"); goto out; }
    if ((mask & R_IASET1) && c->fsi->n_ia) { for (uint32_t i = 0; i < c->fsi->n_ia; i++) if (!c->fsi->ia[i].has_index) { *applicable = false; snprintf(why, wcap, "an input attachment has no InputAttachmentIndex"); goto out; } }
    if (mask & (R_M8 | R_M8C)) {
        char path[1100];
        const char *sfx = (mask & R_M8C) ? "m8c" : "m8";
        const char *dir = c->env->m8_dir;
        if (!dir || !dir[0]) { *applicable = false; snprintf(why, wcap, "no LAGFX_PIPELINE_MATRIX_DIR / bundle dir given"); goto out; }
        size_t n8 = 0;
        snprintf(path, sizeof path, "%s/%016llx.%s.fs.spv", dir, (unsigned long long)c->fh, sfx);
        m8f = slurp_words(path, &n8);
        if (!m8f) { *applicable = false; snprintf(why, wcap, "no %016llx.%s.fs.spv in the directory (made offline: m8 = research/texture3d merged, m8c = the same translator without it)", (unsigned long long)c->fh, sfx); goto out; }
        fs = m8f, fsn = n8;
        snprintf(path, sizeof path, "%s/%016llx.%s.vs.spv", dir, (unsigned long long)c->vh, sfx);
        size_t nv8 = 0;
        m8v = slurp_words(path, &nv8);
        if (m8v) vs = m8v, vsn = nv8;
        snprintf(note, sizeof note, "fs from file (%zu bytes)%s", n8, m8v ? ", vs from file" : "");
    }
    /* SPIR-V transforms */
    if (mask & (R_V13 | R_UNDEF_BITS | R_IAFIRST | R_IASET1)) {
        char vn[300] = "", fnn[300] = "";
        int a = xform_module(c, mask, vs, vsn / 4, &vo, &von, vn, sizeof vn, false), b = xform_module(c, mask, fs, fsn / 4, &fo, &fon, fnn, sizeof fnn, true);
        if (a < 0 || b < 0) { r = VK_ERROR_OUT_OF_HOST_MEMORY; snprintf(why, wcap, "SPIR-V rewrite failed (malformed module / out of memory)"); goto out; }
        if (!a && !b) { *applicable = false; snprintf(why, wcap, "%s", fnn[0] ? fnn : "the rewrite changes nothing in this module"); goto out; }
        if ((mask & (R_V13 | R_UNDEF_BITS)) && !b) { *applicable = false; snprintf(why, wcap, "%s", fnn[0] ? fnn : "the rewrite does not apply to the fragment shader"); goto out; }
        if (a) vs = vo, vsn = von * 4;
        if (b) fs = fo, fsn = fon * 4;
        size_t l = strlen(note);
        snprintf(note + l, sizeof note - l, "%s%s%s%s", l ? " | " : "", vn, vn[0] && fnn[0] ? " | " : "", fnn);
    }
    /* state */
    if (mask & R_NORO) d.raster_order_access = 0;
    if (mask & R_MAP_BITS) {
        idx_info_t *ip = &ii;
        loc_info_t *lp = &li;
        if (mask & R_MAPBOTH) li.pNext = &ii, d.diag_gpci_pnext = &li;
        else if (mask & R_MAPIDX) d.diag_gpci_pnext = ip;
        else d.diag_gpci_pnext = lp;
    }
    if (mask & R_LEGACY) {
        bool ro = !(mask & R_NORO) && c->d0.raster_order_access;
        rp = make_legacy_rp(c, &d, ro, &r);
        if (!rp) { *applicable = false; snprintf(why, wcap, "legacy render pass could not be created (%d)", (int)r); r = VK_SUCCESS; goto out; }
        d.diag_render_pass = (uint64_t)(uintptr_t)rp, d.diag_subpass = 0, d.ds_depth_format = d.ds_stencil_format = VK_FORMAT_UNDEFINED;
    }
    /* layout */
    VkPipelineLayout layout = c->d0.layout;
    if (mask & R_LAYOUT_BITS) {
        char ln[160];
        r = make_layout(c, mask, &lay, ln, sizeof ln);
        if (r != VK_SUCCESS) { snprintf(why, wcap, "the variant layout could not be created (%d %s)", (int)r, res_name(r)); goto out; }
        layout = lay.pl;
        size_t l = strlen(note);
        snprintf(note + l, sizeof note - l, "%slayout: %u set(s)%s%s", l ? " | " : "", lay.nset, ln[0] ? ", " : "", ln);
    }
    r = build_pipeline(c, &d, vs, vsn, fs, fsn, layout);
    snprintf(why, wcap, "%s", note);
out:
    if (rp) vkDestroyRenderPass(c->dev, rp, NULL);
    lay_destroy(c->dev, &lay);
    free(vo); free(fo); free(m8v); free(m8f);
    return r;
}

/* a row: run, log one line, account */
static VkResult row(mctx_t *c, const char *id, const char *what, uint32_t mask, bool single) {
    bool app;
    char why[400];
    double t0 = now_ms();
    VkResult r = run_recipe(c, mask, &app, why, sizeof why);
    if (app && c->env->test_override) r = c->env->test_override(c->env->user, mask, r);   /* test seam: NULL in production */
    double ms = now_ms() - t0;
    if (!app) { c->na++; add_id(c->ids_na, sizeof c->ids_na, id); line(c, "pipeline-matrix: row=%s result=n/a ms=0 [%s] %s", id, what, why); }
    else if (r == VK_SUCCESS) { c->ok++; add_id(c->ids_ok, sizeof c->ids_ok, id); line(c, "pipeline-matrix: row=%s result=BUILT vk=0 ms=%.1f [%s]%s%s", id, ms, what, why[0] ? " " : "", why); }
    else { c->fail++; add_id(c->ids_fail, sizeof c->ids_fail, id); line(c, "pipeline-matrix: row=%s result=FAIL vk=%d(%s) ms=%.1f [%s]%s%s", id, (int)r, res_name(r), ms, what, why[0] ? " " : "", why); }
    if (single && c->nsingle < sizeof c->single / sizeof c->single[0]) { c->single[c->nsingle].mask = mask, c->single[c->nsingle].r = r, c->single[c->nsingle].applicable = app, snprintf(c->single[c->nsingle].id, sizeof c->single[c->nsingle].id, "%s", id), c->nsingle++; }
    return app ? r : (VkResult)0x7fffffff;
}

static bool compatible(uint32_t m) {
    if (__builtin_popcount(m & R_MAP_BITS) > 1) return false;
    if ((m & R_IAFIRST) && (m & R_IASET1)) return false;
    if ((m & R_BLOCK) && (m & (R_IAFIRST | R_IASET1))) return false;
    if ((m & R_LEGACY) && (m & R_MAP_BITS)) return false;   /* the mapping structs belong to dynamic rendering */
    uint32_t u = m & R_UNDEF_BITS;
    if (__builtin_popcount(u) > 1) return false;           /* one OpUndef rewrite at a time (g subsumes h / j; i / k are independent but kept simple) */
    return true;
}

void lagfx_pipeline_matrix_run(VkDevice dev, const lagfx_pipeline_desc_t *d0, const VkDescriptorSetLayoutBinding *bind, uint32_t nbind, const lagfx_matrix_env_t *env, lagfx_matrix_result_t *res) {
    lagfx_matrix_result_t rs;
    memset(&rs, 0, sizeof rs);
    if (res) *res = rs;
    if (!dev || !d0 || !env || !d0->vertex_spv || !d0->fragment_spv || !d0->layout) return;
    mctx_t *c = calloc(1, sizeof *c);
    if (!c) return;
    c->dev = dev, c->env = env, c->d0 = *d0, c->bind = bind, c->nbind = nbind, c->cap_lines = 80;
    c->vs = d0->vertex_spv, c->vsn = d0->vertex_spv_bytes, c->fs = d0->fragment_spv, c->fsn = d0->fragment_spv_bytes;
    c->vh = lagfx_pipeline_spv_hash(c->vs, c->vsn), c->fh = lagfx_pipeline_spv_hash(c->fs, c->fsn);
    c->d0.diag_gpci_pnext = NULL, c->d0.diag_render_pass = 0;
    c->fsi = calloc(1, sizeof *c->fsi);
    if (!c->fsi || !lagfx_spv_iface_reflect(c->fs, c->fsn / 4, 4, c->fsi)) { if (c->fsi) c->fsi->ok = false, c->fsi->n_ia = 0; }
    line(c, "pipeline-matrix: begin pair vs=%016llx fs=%016llx vs_bytes=%zu fs_bytes=%zu colour=%u raster_order=%d input_attachments=%u device_local_read=%d device_raster_order=%d spirv=%u.%u",
         (unsigned long long)c->vh, (unsigned long long)c->fh, c->vsn, c->fsn, ncol_of(&c->d0), (int)c->d0.raster_order_access, c->fsi ? c->fsi->n_ia : 0, (int)env->have_local_read, (int)env->have_raster_order,
         c->fsi && c->fsi->ok ? (c->fsi->version >> 16) & 0xff : 0u, c->fsi && c->fsi->ok ? (c->fsi->version >> 8) & 0xff : 0u);
    c->r0 = row(c, "M0", "original", 0, false);
    if (c->r0 == VK_SUCCESS) line(c, "pipeline-matrix: note M0 BUILT here: the original pair does not fail in this process/context, the matrix rows below are informative only");
    row(c, "M1", "rasterization-order flag removed (non-coherent fetch; needs a by-region colour-write->input-read barrier at draw time)", R_NORO, true);
    row(c, "M2", "SPIR-V 1.4 -> 1.3 (no 1.4-only feature used)", R_V13, true);
    row(c, "M3a", "layout declares the contiguous input-attachment block (ANGLE style)", R_BLOCK, true);
    row(c, "M3b", "input attachments renumbered to bindings 0..n-1 (binding = base 0 + index), the rest shifted", R_IAFIRST, true);
    row(c, "M4a", "input attachments in their own descriptor set 1 (binding = index)", R_IASET1, true);
    row(c, "M4b", "every non-input-attachment layout binding VERTEX|FRAGMENT", R_STAGES, true);
    row(c, "M5a", "explicit VkRenderingAttachmentLocationInfo + VkRenderingInputAttachmentIndexInfo (identity)", R_MAPBOTH, true);
    row(c, "M5b", "explicit VkRenderingInputAttachmentIndexInfo only (identity)", R_MAPIDX, true);
    row(c, "M5c", "explicit VkRenderingAttachmentLocationInfo only (identity)", R_MAPLOC, true);
    row(c, "M6a", "legacy VkRenderPass + subpass input attachments + self-dependency, rast-order as recorded", R_LEGACY, true);
    row(c, "M6b", "legacy VkRenderPass + self-dependency, no rast-order flag", R_LEGACY | R_NORO, false);
    row(c, "M8", "Dim3D fragment module from research/texture3d (pipeline creation only)", R_M8, true);
    row(c, "M8c", "control for M8: same translator revision without the 3D change (fragment module from file)", R_M8C, true);
    row(c, "M9g", "OpUndef composites -> OpConstantNull", R_UG, true);
    row(c, "M9h", "OpUndef struct/pointer -> OpConstantNull", R_UH, true);
    row(c, "M9i", "unreferenced OpUndef dropped", R_UI, true);
    row(c, "M9j", "OpUndef struct -> OpConstantNull", R_UJ, true);
    row(c, "M9k", "only OpReturnValue(OpUndef) operands -> OpConstantNull", R_UK, true);
    /* M10: the largest compatible set of the single rows whose outcome differs from M0 (BUILT where M0 failed, or another VkResult), then pairs of them (at most 8 rows in all) */
    {
        uint32_t chg[24], nchg = 0;
        for (uint32_t i = 0; i < c->nsingle; i++) if (c->single[i].applicable && c->single[i].r != c->r0) chg[nchg++] = i;
        if (nchg < 2) line(c, "pipeline-matrix: row=M10 result=n/a ms=0 [combinations] %u row(s) changed the outcome vs M0: combinations need at least 2", nchg);
        else {
            uint32_t done = 0, allmask = 0;
            char allid[100] = "";
            for (uint32_t k = 0; k < nchg; k++) {
                uint32_t m = c->single[chg[k]].mask;
                if (!compatible(allmask | m)) continue;
                allmask |= m;
                size_t l = strlen(allid);
                if (l + 8 < sizeof allid) snprintf(allid + l, sizeof allid - l, "%s%s", l ? "+" : "", c->single[chg[k]].id);
            }
            if (__builtin_popcount(allmask) >= 2) { char id[120]; snprintf(id, sizeof id, "M10:%s", allid); row(c, id, "all compatible rows that changed the outcome, together", allmask, false); done++; }
            for (uint32_t i = 0; i < nchg && done < 8; i++)
                for (uint32_t j = i + 1; j < nchg && done < 8; j++) {
                    uint32_t m = c->single[chg[i]].mask | c->single[chg[j]].mask;
                    if (!compatible(m) || m == allmask) continue;
                    char id[40];
                    snprintf(id, sizeof id, "M10:%s+%s", c->single[chg[i]].id, c->single[chg[j]].id);
                    row(c, id, "pair of rows that changed the outcome", m, false);
                    done++;
                }
            c->combos = done;
            if (!done) line(c, "pipeline-matrix: row=M10 result=n/a ms=0 [combinations] the %u rows that changed the outcome are not combinable", nchg);
        }
    }
    line(c, "pipeline-matrix: summary pair vs=%016llx fs=%016llx m0=%d BUILT=[%s] FAIL=[%s] N/A=[%s] built=%u failed=%u na=%u", (unsigned long long)c->vh, (unsigned long long)c->fh,
         (int)c->r0, c->ids_ok, c->ids_fail, c->ids_na, c->ok, c->fail, c->na);
    rs.m0_result = (int32_t)c->r0, rs.built = c->ok, rs.failed = c->fail, rs.na = c->na, rs.combos = c->combos;
    snprintf(rs.built_ids, sizeof rs.built_ids, "%s", c->ids_ok);
    if (res) *res = rs;
    free(c->fsi);
    free(c);
}

/* ---- production hook ---- */
static struct { bool lr, ro; uint32_t maxcol, maxia; } g_dev;
void lagfx_pipeline_matrix_set_device(bool lr, bool ro, uint32_t maxcol, uint32_t maxia) { g_dev.lr = lr, g_dev.ro = ro, g_dev.maxcol = maxcol, g_dev.maxia = maxia; }

static void emit_warn(void *u, const char *l) { (void)u; LAGFX_WARN("%s", l); }

void lagfx_pipeline_matrix_on_failure(VkDevice dev, const lagfx_pipeline_desc_t *failed, VkResult vr) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static int limit = -1;
    static uint64_t seen[64];
    static uint32_t nseen, expanded, suppressed_lines;
    if (limit == 0) return;
    if (limit < 0) {
        const char *e = getenv("LAGFX_PIPELINE_MATRIX");
        int v = e && e[0] ? atoi(e) : 0;
        limit = v <= 0 ? 0 : v == 1 ? 2 : v > 8 ? 8 : v;
        if (!limit) return;
        LAGFX_WARN("pipeline-matrix: ENABLED (LAGFX_PIPELINE_MATRIX=%s): the first %d distinct failing (vs,fs) pairs get the -13 matrix; every row builds a COPY and destroys it; rows are `pipeline-matrix: row=<id> result=BUILT|FAIL|n/a ...`", e, limit);
    }
    if (!dev || !failed) return;
    if (!failed->vertex_spv || !failed->fragment_spv || !failed->vertex_spv_bytes || !failed->fragment_spv_bytes || (failed->vertex_spv_bytes & 3u) || (failed->fragment_spv_bytes & 3u)) {
        LAGFX_WARN("pipeline-matrix: n/a for a failing pipeline (vk=%d): the caller gave no SPIR-V words in the descriptor (vertex_spv / fragment_spv)", (int)vr);
        return;
    }
    uint64_t vh = lagfx_pipeline_spv_hash(failed->vertex_spv, failed->vertex_spv_bytes), fh = lagfx_pipeline_spv_hash(failed->fragment_spv, failed->fragment_spv_bytes), key = vh * 1099511628211ull ^ fh;
    pthread_mutex_lock(&mu);
    for (uint32_t i = 0; i < nseen; i++) if (seen[i] == key) { pthread_mutex_unlock(&mu); return; }
    if (nseen < 64) seen[nseen++] = key;
    if (expanded >= (uint32_t)limit) {
        if (suppressed_lines < 8) { suppressed_lines++; LAGFX_WARN("pipeline-matrix: pair vs=%016llx fs=%016llx vk=%d NOT expanded (limit %d pairs per process; LAGFX_PIPELINE_MATRIX=<n> raises it)", (unsigned long long)vh, (unsigned long long)fh, (int)vr, limit); }
        pthread_mutex_unlock(&mu);
        return;
    }
    expanded++;
    VkDescriptorSetLayoutBinding binds[32];
    const uint8_t *blobs[2] = { (const uint8_t *)failed->vertex_spv, (const uint8_t *)failed->fragment_spv };
    size_t lens[2] = { failed->vertex_spv_bytes, failed->fragment_spv_bytes };
    uint32_t nb = lagfx_layout_bindings_from_spv(blobs, lens, 2, binds);
    lagfx_matrix_env_t env = { .have_local_read = g_dev.lr, .have_raster_order = g_dev.ro, .max_color_att = g_dev.maxcol, .max_per_stage_input_att = g_dev.maxia, .m8_dir = getenv("LAGFX_PIPELINE_MATRIX_DIR"), .emit = emit_warn };
    lagfx_matrix_result_t res;
    lagfx_pipeline_matrix_run(dev, failed, binds, nb, &env, &res);
    pthread_mutex_unlock(&mu);
}

void lagfx_matrix_cmd_fetch_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_DEPENDENCY_BY_REGION_BIT, 1, &mb, 0, NULL, 0, NULL);
}

#endif /* LAGFX_HAVE_VULKAN */
