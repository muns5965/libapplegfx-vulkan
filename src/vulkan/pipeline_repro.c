/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — rebuild a failed pipeline from a failure bundle (see include/libapplegfx-vulkan.h, src/vulkan/pipeline_bundle.h)
 * src/vulkan/pipeline_repro.c
 *
 * The as-is rebuild goes through lagfx_pipeline_build() itself (descriptor rebuilt from the bundle's `desc.*` lines), on a device made by lagfx_vk_init(): the same code
 * as production. No process-global state: every object lives in the call's own context and is destroyed before it returns.
 * The VARIANT MATRIX (opt-in) rebuilds COPIES of the state / SPIR-V; every variant is DIAGNOSTIC only and the report says which of them change the meaning of the program.
 */
#include "libapplegfx-vulkan.h"

#include <stdlib.h>
#include <string.h>

#ifndef LAGFX_HAVE_VULKAN
lagfx_status_t lagfx_pipeline_repro_run(const char *bundle_dir, const lagfx_pipeline_repro_opts_t *opts, lagfx_pipeline_repro_result_t *result, char **report) {
    (void)bundle_dir; (void)opts;
    if (result) memset(result, 0, sizeof *result);
    if (report) *report = strdup("lagfx_pipeline_repro_run: this build has no Vulkan\n");
    return LAGFX_ERR_NOT_FOUND;
}
void lagfx_pipeline_repro_free_report(char *report) { free(report); }
#else

#include "caps.h"
#include "common/log.h"
#include "descriptor_layout.h"
#include "instance.h"
#include "pipeline_build.h"
#include "pipeline_bundle.h"
#include "pipeline_matrix.h"
#include "pipeline_reduce.h"
#include "spv_variants.h"

#include <stdarg.h>
#include <stdio.h>

typedef struct { char *p; size_t n, cap; } rep_t;
static void rp(rep_t *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void rp(rep_t *r, const char *fmt, ...) {
    va_list ap;
    for (int pass = 0; pass < 2; pass++) {
        va_start(ap, fmt);
        int w = vsnprintf(r->p ? r->p + r->n : NULL, r->p ? r->cap - r->n : 0, fmt, ap);
        va_end(ap);
        if (w < 0) return;
        if (r->p && (size_t)w < r->cap - r->n) { r->n += (size_t)w; return; }
        size_t need = r->n + (size_t)w + 1u, c = r->cap ? r->cap : 2048;
        while (c < need) c *= 2;
        char *q = realloc(r->p, c);
        if (!q) return;
        r->p = q, r->cap = c;
    }
}

typedef struct {
    struct lagfx_vk_state *vk;
    VkDevice dev;
    const lagfx_bundle_t *b;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    rep_t *rep;
    uint32_t ok, fail, na;
} ctx_t;

/* One build: modules from the given words, descriptor `d` (copied by the caller), the bundle's layout. Returns the VkResult (VK_SUCCESS = built). */
static VkResult build_once(ctx_t *c, const lagfx_pipeline_desc_t *dsrc, const uint32_t *vs, size_t vsn, const uint32_t *fs, size_t fsn, const char *ventry, const char *fentry) {
    lagfx_pipeline_desc_t d = *dsrc;
    VkShaderModule vm = VK_NULL_HANDLE, fm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = vsn, .pCode = vs };
    VkResult r = vkCreateShaderModule(c->dev, &ci, NULL, &vm);
    if (r == VK_SUCCESS) { ci.codeSize = fsn, ci.pCode = fs; r = vkCreateShaderModule(c->dev, &ci, NULL, &fm); }
    if (r == VK_SUCCESS) {
        d.vertex_shader = vm, d.fragment_shader = fm, d.layout = c->layout, d.vertex_entry_point = ventry, d.fragment_entry_point = fentry;
        d.vertex_spv = vs, d.vertex_spv_bytes = vsn, d.fragment_spv = fs, d.fragment_spv_bytes = fsn, d.diag = NULL;
        VkPipeline pl = VK_NULL_HANDLE;
        lagfx_pipeline_bundle_suppress(1);
        lagfx_status_t st = lagfx_pipeline_build(c->dev, &d, &pl);
        lagfx_pipeline_bundle_suppress(0);
        r = st == LAGFX_OK ? VK_SUCCESS : (VkResult)lagfx_pipeline_build_last_result();
        if (st != LAGFX_OK && r == VK_SUCCESS) r = VK_ERROR_UNKNOWN;   /* failed before vkCreateGraphicsPipelines (invalid argument) */
        if (pl) vkDestroyPipeline(c->dev, pl, NULL);
    }
    if (vm) vkDestroyShaderModule(c->dev, vm, NULL);
    if (fm) vkDestroyShaderModule(c->dev, fm, NULL);
    return r;
}

static void res_str(char *out, size_t n, VkResult r) { if (r == VK_SUCCESS) snprintf(out, n, "OK"); else snprintf(out, n, "FAIL(%d %s)", (int)r, lagfx_vk_result_name(r)); }

/* a variant line */
static void variant(ctx_t *c, const char *id, const char *what, const char *tag, VkResult r, bool applicable, const char *note) {
    char rs[96];
    if (!applicable) { c->na++; rp(c->rep, "  %-4s %-58s n/a%s%s\n", id, what, note && note[0] ? " - " : "", note ? note : ""); return; }
    res_str(rs, sizeof rs, r);
    if (r == VK_SUCCESS) c->ok++; else c->fail++;
    rp(c->rep, "  %-4s %-58s %s%s%s%s%s\n", id, what, rs, tag && tag[0] ? "  [" : "", tag ? tag : "", tag && tag[0] ? "]" : "", note && note[0] ? "" : "");
    if (note && note[0]) rp(c->rep, "       %s\n", note);
}

#define DIAG_MEANING "DIAGNOSTIC: changes meaning"

static void run_matrix(ctx_t *c, const lagfx_pipeline_desc_t *d0, const uint32_t *vs, size_t vsn, const uint32_t *fs, size_t fsn, const char *ve, const char *fe) {
    rep_t *rep = c->rep;
    rp(rep, "\n-- variant matrix (DIAGNOSTIC ONLY: every rebuild below uses a COPY of the state / SPIR-V; nothing here is ever applied by the normal path) --\n");
    VkResult r;
    r = build_once(c, d0, vs, vsn, fs, fsn, ve, fe);
    variant(c, "a", "as-is", "", r, true, NULL);

    /* (b) each colour format alone: the recorded ones, then two formats the device log named (97 = R16G16B16A16_SFLOAT, 44 = B8G8R8A8_UNORM) */
    VkFormat fmts[8];
    unsigned nf = 0;
    const VkFormat rec[5] = { (VkFormat)d0->color_format, (VkFormat)d0->color_formats[0], (VkFormat)d0->color_formats[1], (VkFormat)d0->color_formats[2], (VkFormat)d0->color_formats[3] };
    for (unsigned i = 0; i < 5; i++) {
        if (rec[i] == VK_FORMAT_UNDEFINED || (i && d0->n_color < 2)) continue;
        bool dup = false;
        for (unsigned k = 0; k < nf; k++) dup |= fmts[k] == rec[i];
        if (!dup && nf < 8) fmts[nf++] = rec[i];
    }
    if (!nf) fmts[nf++] = VK_FORMAT_B8G8R8A8_UNORM;
    unsigned nrec = nf;
    static const VkFormat extra[] = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
    for (unsigned i = 0; i < 3; i++) { bool dup = false; for (unsigned k = 0; k < nf; k++) dup |= fmts[k] == extra[i]; if (!dup && nf < 8) fmts[nf++] = extra[i]; }
    for (unsigned i = 0; i < nf; i++) {
        lagfx_pipeline_desc_t d = *d0;
        d.n_color = 1, d.no_color = 0, d.color_format = fmts[i];
        char w[96]; snprintf(w, sizeof w, "colour format %d alone (%s)", (int)fmts[i], i < nrec ? "recorded" : "extra");
        variant(c, "b", w, "DIAGNOSTIC: changes the render target", build_once(c, &d, vs, vsn, fs, fsn, ve, fe), true, NULL);
    }
    /* (c) without blend / depth state */
    {
        lagfx_pipeline_desc_t d = *d0;
        d.blend_set = 1, d.blend_enable = 0, d.color_write_mask = 0xfu, memset(d.slot_blend, 0, sizeof d.slot_blend);
        variant(c, "c1", "blending off", DIAG_MEANING, build_once(c, &d, vs, vsn, fs, fsn, ve, fe), true, NULL);
        d = *d0, d.dyn_state = 0, d.ds_depth_format = d.ds_stencil_format = VK_FORMAT_UNDEFINED;
        variant(c, "c2", "no depth / stencil formats, no dynamic encoder state", DIAG_MEANING, build_once(c, &d, vs, vsn, fs, fsn, ve, fe), true, NULL);
        d.blend_set = 1, d.blend_enable = 0, d.color_write_mask = 0xfu, memset(d.slot_blend, 0, sizeof d.slot_blend);
        variant(c, "c3", "both of the above", DIAG_MEANING, build_once(c, &d, vs, vsn, fs, fsn, ve, fe), true, NULL);
        if (d0->raster_order_access) { d = *d0, d.raster_order_access = 0; variant(c, "c4", "without the rasterization-order attachment access flag", DIAG_MEANING, build_once(c, &d, vs, vsn, fs, fsn, ve, fe), true, NULL); }
    }
    /* (d) which stage: stubs (valid, no inputs / outputs) for one or both stages */
    variant(c, "d0", "both stages stubbed (does the STATE alone build?)", "DIAGNOSTIC: changes meaning (no real shader)", build_once(c, d0, lagfx_stub_vs_spv, sizeof lagfx_stub_vs_spv, lagfx_stub_fs_spv, sizeof lagfx_stub_fs_spv, "main", "main"), true, NULL);
    variant(c, "d1", "real VERTEX + stub fragment (the VS fails if this fails)", "DIAGNOSTIC: changes meaning (stub stage)", build_once(c, d0, vs, vsn, lagfx_stub_fs_spv, sizeof lagfx_stub_fs_spv, ve, "main"), true, NULL);
    variant(c, "d2", "stub vertex + real FRAGMENT (the FS fails if this fails)", "DIAGNOSTIC: changes meaning (stub stage)", build_once(c, d0, lagfx_stub_vs_spv, sizeof lagfx_stub_vs_spv, fs, fsn, "main", fe), true, NULL);
    /* (e) .. (i): rewrites of both modules */
    struct { const char *id, *what, *tag; int (*fn)(const uint32_t *, size_t, uint32_t **, size_t *, char *, size_t); } rw[7] = {
        { "e", "64-bit dynamic vector index -> 32-bit", "DIAGNOSTIC: rewrites instructions, value-preserving for an in-range index", lagfx_spv_variant_index32 },
        { "f", "drop unused Int8/16/Float16/Int64 capability decls", "DIAGNOSTIC: only removes what nothing uses", lagfx_spv_variant_strip_caps },
        { "g", "OpUndef composites -> OpConstantNull", DIAG_MEANING " (undef -> zero)", lagfx_spv_variant_undef_composites },
        { "h", "OpUndef struct / pointer ONLY -> OpConstantNull", DIAG_MEANING " (undef -> zero, struct / pointer typed only)", lagfx_spv_variant_undef_struct_ptr },
        { "i", "drop unreferenced OpUndef", "DIAGNOSTIC: value-preserving (nothing used them)", lagfx_spv_variant_drop_dead_undef },
        { "j", "OpUndef struct ONLY -> OpConstantNull", DIAG_MEANING " (undef -> zero, struct typed only)", lagfx_spv_variant_undef_struct },
        { "n", "D2 candidate: GLSL FMin/FMax/FClamp -> NMin/NMax/NClamp", DIAG_MEANING " (only for NaN operands: the other operand becomes the result)", lagfx_spv_variant_nanmax } };
    for (int i = 0; i < 7; i++) {
        uint32_t *vo = NULL, *fo = NULL;
        size_t von = 0, fon = 0;
        char vi[160] = "", fi[160] = "", note[400];
        int nv = rw[i].fn(vs, vsn / 4, &vo, &von, vi, sizeof vi), nfx = rw[i].fn(fs, fsn / 4, &fo, &fon, fi, sizeof fi);
        snprintf(note, sizeof note, "vs: %s | fs: %s", nv < 0 ? "malformed module" : vi, nfx < 0 ? "malformed module" : fi);
        if (nv <= 0 && nfx <= 0) variant(c, rw[i].id, rw[i].what, NULL, VK_SUCCESS, false, note);
        else variant(c, rw[i].id, rw[i].what, rw[i].tag, build_once(c, d0, nv > 0 ? vo : vs, nv > 0 ? von * 4 : vsn, nfx > 0 ? fo : fs, nfx > 0 ? fon * 4 : fsn, ve, fe), true, note);
        free(vo); free(fo);
    }
}

static void m13_emit(void *u, const char *l) { rp((rep_t *)u, "%s\n", l); }

static void reduce_cfg_from(lagfx_reduce_cfg_t *cfg, const lagfx_pipeline_repro_opts_t *opts, const lagfx_bundle_t *b, const char *bundle_dir, rep_t *rep, lagfx_reduce_target_t *t) {
    const char *m8 = getenv("LAGFX_PIPELINE_MATRIX_DIR");
    memset(cfg, 0, sizeof *cfg);
    memset(t, 0, sizeof *t);
    t->d0 = b->desc, t->d0.layout = VK_NULL_HANDLE, t->bind = b->n_sets && b->n_dsl ? b->dsl : NULL, t->nbind = b->n_sets ? b->n_dsl : 0, t->push = b->push, t->npush = b->n_sets && b->n_dsl ? b->n_push : 0, t->fs_entry = b->fs_entry;
    cfg->fs = (const uint32_t *)b->fs_spv, cfg->fs_bytes = b->fs_bytes, cfg->vs = (const uint32_t *)b->vs_spv, cfg->vs_bytes = b->vs_bytes;
    cfg->file_dir = m8 && m8[0] ? m8 : NULL, cfg->file_dir2 = bundle_dir, cfg->emit = m13_emit, cfg->user = rep, cfg->target = t;
    if (opts && opts->struct_size >= sizeof *opts) cfg->only = opts->reduce_only, cfg->from = opts->reduce_from, cfg->timeout_ms = opts->reduce_timeout_ms, cfg->list_only = (int)opts->reduce_list_only;
    lagfx_pipeline_reduce_cfg_env(cfg);
}


lagfx_status_t lagfx_pipeline_repro_run(const char *bundle_dir, const lagfx_pipeline_repro_opts_t *opts, lagfx_pipeline_repro_result_t *result, char **report) {
    lagfx_pipeline_repro_result_t res;
    memset(&res, 0, sizeof res);
    if (result) *result = res;
    if (report) *report = NULL;
    if (!bundle_dir) return LAGFX_ERR_INVALID_ARG;
    uint32_t flags = opts && opts->struct_size >= sizeof(uint32_t) * 2u ? opts->flags : 0u;
    rep_t rep = { 0 };
#ifdef LAGFX_NO_PROCESS
    flags &= ~LAGFX_PIPELINE_REPRO_ISOLATE;   /* no fork in this build: rows run in-process; the caller isolates by starting one process per row */
#endif
    lagfx_status_t status = LAGFX_OK;
    lagfx_bundle_t b;
    char err[256] = "";
    rp(&rep, "== lagfx pipeline repro ==\nbundle: %s\nrepro build: %s\n", bundle_dir, lagfx_build_info());
    if (!lagfx_bundle_load(bundle_dir, &b, err, sizeof err)) {
        rp(&rep, "cannot load the bundle: %s\n", err);
        status = LAGFX_ERR_NOT_FOUND;
        goto out;
    }
    if ((flags & LAGFX_PIPELINE_REPRO_REDUCE) && (flags & LAGFX_PIPELINE_REPRO_ISOLATE)) {   /* the parent creates NO Vulkan object: every row runs in a forked child with its own instance */
        lagfx_reduce_cfg_t cfg;
        lagfx_reduce_target_t tg;
        rp(&rep, "recorded: result=%d (%s) translator=\"%s\" origin=\"%s\" fs_fn=%s; isolated reduction ladder (fork per row, own Vulkan instance in each child)\n", b.result, lagfx_vk_result_name((VkResult)b.result), b.translator, b.origin, b.fs_fn);
        reduce_cfg_from(&cfg, opts, &b, bundle_dir, &rep, &tg);
        cfg.isolate = 1;
        lagfx_reduce_result_t rr;
        lagfx_pipeline_reduce_run(&cfg, &rr);
        res.recorded_result = b.result, res.vk_result = rr.r0 == 0 ? 0 : -13;
        lagfx_bundle_free(&b);
        goto out;
    }
    struct lagfx_vk_state *vk = NULL;
    lagfx_device_descriptor_t dd;
    memset(&dd, 0, sizeof dd);
    if (lagfx_vk_init(&vk, &dd) != LAGFX_OK || !vk || !vk->initialized) {
        rp(&rep, "no usable Vulkan device on this host: cannot rebuild\n");
        status = LAGFX_ERR_VULKAN_INIT;
        lagfx_bundle_free(&b);
        goto out;
    }
    ctx_t c = { .vk = vk, .dev = vk->device, .b = &b, .rep = &rep };
    res.recorded_result = b.result;
    rp(&rep, "recorded: result=%d (%s) translator=\"%s\" origin=\"%s\" vs_fn=%s fs_fn=%s\n", b.result, lagfx_vk_result_name((VkResult)b.result), b.translator, b.origin, b.vs_fn, b.fs_fn);
    if (!b.have_dev) rp(&rep, "recorded device: unknown (bundle has no dev.* lines)\n");
    else {
        rp(&rep, "recorded device: %s vendor=0x%x device=0x%x driver_version=0x%x api=%u.%u.%u driver=\"%s\" \"%s\" caps_enabled=0x%x\n  extensions: %s\n  features: %s\n", b.dev_name, b.dev_vendor, b.dev_device, b.dev_driver_version,
           VK_VERSION_MAJOR(b.dev_api_version), VK_VERSION_MINOR(b.dev_api_version), VK_VERSION_PATCH(b.dev_api_version), b.dev_driver_name, b.dev_driver_info, b.dev_caps, b.dev_exts, b.dev_features);
    }
    rp(&rep, "this device: %s vendor=0x%x device=0x%x driver_version=0x%x api=%u.%u.%u driver=\"%s\" \"%s\" caps_enabled=0x%x\n  extensions: %s\n  features: %s\n", vk->phys_props.deviceName, vk->phys_props.vendorID,
       vk->phys_props.deviceID, vk->phys_props.driverVersion, VK_VERSION_MAJOR(vk->phys_props.apiVersion), VK_VERSION_MINOR(vk->phys_props.apiVersion), VK_VERSION_PATCH(vk->phys_props.apiVersion), vk->driver_name,
       vk->driver_info, vk->caps_enabled, vk->dev_exts, vk->dev_features);
    if (b.have_dev) {
        bool same = !strcmp(b.dev_name, vk->phys_props.deviceName) && b.dev_driver_version == vk->phys_props.driverVersion && b.dev_api_version == vk->phys_props.apiVersion;
        rp(&rep, "device match: %s", same ? "same device and driver" : "DIFFERENT device / driver: the failure may not reproduce here");
        if (b.dev_caps != vk->caps_enabled) rp(&rep, "; capability mask differs (recorded 0x%x, here 0x%x)", b.dev_caps, vk->caps_enabled);
        if (strcmp(b.dev_features, vk->dev_features)) rp(&rep, "; enabled feature list differs");
        if (strcmp(b.dev_exts, vk->dev_exts)) rp(&rep, "; enabled extension list differs");
        rp(&rep, "\n");
    }
    for (uint32_t i = 0; i < b.n_env; i++) {   /* "NAME=value" */
        char name[96];
        snprintf(name, sizeof name, "%s", b.env[i]);
        char *eq = strchr(name, '=');
        if (!eq) continue;
        *eq = 0;
        const char *cur = getenv(name), *rec = eq + 1;
        if (strcmp(rec, cur ? cur : "(unset)")) rp(&rep, "ENV DIFFERENCE: %s recorded %s, now %s (the builder reads it; it is NOT applied by the repro)\n", name, rec, cur ? cur : "(unset)");
    }
    {   /* shaders and the capability gate */
        uint32_t nv = 0, nfc = 0;
        bool okv = lagfx_spv_required_caps_ex(b.vs_spv, b.vs_bytes, &nv), okf = lagfx_spv_required_caps_ex(b.fs_spv, b.fs_bytes, &nfc);
        rp(&rep, "vs: %zu bytes hash=%016llx (bundle says %016llx) entry=%s needs_caps=0x%x\nfs: %zu bytes hash=%016llx (bundle says %016llx) entry=%s needs_caps=0x%x\n", b.vs_bytes,
           (unsigned long long)lagfx_pipeline_spv_hash(b.vs_spv, b.vs_bytes), (unsigned long long)b.vs_hash, b.vs_entry, okv ? nv : 0xffffffffu, b.fs_bytes, (unsigned long long)lagfx_pipeline_spv_hash(b.fs_spv, b.fs_bytes),
           (unsigned long long)b.fs_hash, b.fs_entry, okf ? nfc : 0xffffffffu);
        if (!okv || !okf) rp(&rep, "NOTE: a module could not be analysed for capabilities\n");
        else if ((nv | nfc) & ~vk->caps_enabled) rp(&rep, "NOTE: the modules need capabilities this device does not have enabled (missing mask 0x%x): the resolver would not have built this pipeline here\n", (nv | nfc) & ~vk->caps_enabled);
        if (b.vs_bc_bytes || b.fs_bc_bytes) rp(&rep, "AIR bitcode in the bundle: vs %zu bytes, fs %zu bytes\n", b.vs_bc_bytes, b.fs_bc_bytes);
    }
    /* layout from the bundle */
    VkResult lr = VK_SUCCESS;
    if (b.n_sets && b.n_dsl) {
        VkDescriptorSetLayoutCreateInfo dl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = b.n_dsl, .pBindings = b.dsl };
        lr = vkCreateDescriptorSetLayout(c.dev, &dl, NULL, &c.dsl);
        if (lr == VK_SUCCESS) {
            VkPipelineLayoutCreateInfo pl = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &c.dsl, .pushConstantRangeCount = b.n_push, .pPushConstantRanges = b.push };
            lr = vkCreatePipelineLayout(c.dev, &pl, NULL, &c.layout);
        }
    } else c.layout = vk->empty_layout;
    rp(&rep, "layout: %u set(s), %u binding(s), %u push range(s): %s\n", b.n_sets, b.n_dsl, b.n_push, lr == VK_SUCCESS ? "created" : "FAILED to create");
    {   /* the binding table of the bundle's SPIR-V, derived NOW by the shared derivation (air2spv/binding_alloc.h): the recorded layout above is what the writer saw */
        VkDescriptorSetLayoutBinding nb[LAGFX_LAYOUT_MAX_BINDINGS];
        const uint8_t *bl[2] = { b.vs_spv, b.fs_spv };
        const size_t ln[2] = { b.vs_bytes, b.fs_bytes };
        uint32_t nn = 0;
        lagfx_layout_info_t li;
        lagfx_status_t ds = lagfx_layout_derive(bl, ln, 2, nb, LAGFX_LAYOUT_MAX_BINDINGS, &nn, &li);
        if (ds != LAGFX_OK) {
            rp(&rep, "BINDING TABLE INVALID (%u conflict(s), %u overflow(s)): two descriptor types on one set+binding cannot be a layout; a layout derived by the old builder kept ONE of them (recorded layout: %u binding(s), %s):\n",
               li.n_conflict, li.n_overflow, b.n_dsl, b.layout_error ? "writer already flagged layout.error" : "no layout.error line: bundle from before the check");
            for (const char *q = li.report; *q;) { const char *e = strchr(q, '\n'); size_t l = e ? (size_t)(e - q) : strlen(q); rp(&rep, "  %.*s\n", (int)l, q); q += l + (e ? 1 : 0); }
        } else {
            rp(&rep, "binding table: %u binding(s) derived now, key %016llx (bundle %s %016llx)%s\n", nn, (unsigned long long)li.key, b.layout_key ? "says" : "has no key; ", (unsigned long long)b.layout_key,
               nn != b.n_dsl ? "; DIFFERS from the recorded layout (count)" : (b.layout_key && b.layout_key != li.key ? "; DIFFERS from the recorded key" : ""));
        }
    }
    if (lr != VK_SUCCESS) { rp(&rep, "cannot create the recorded pipeline layout (%d): stopping\n", (int)lr); status = LAGFX_OK; goto cleanup; }
    {
        const uint32_t *vs = (const uint32_t *)b.vs_spv, *fs = (const uint32_t *)b.fs_spv;
        VkResult rr[3];
        char s0[96], s1[96], s2[96];
        for (int i = 0; i < 3; i++) rr[i] = build_once(&c, &b.desc, vs, b.vs_bytes, fs, b.fs_bytes, b.vs_entry, b.fs_entry);
        res.vk_result = (int32_t)rr[2];
        res.as_is_consistent = rr[0] == rr[1] && rr[1] == rr[2];
        res.reproduced = res.as_is_consistent && (int32_t)rr[2] == b.result;
        res_str(s0, sizeof s0, rr[0]), res_str(s1, sizeof s1, rr[1]), res_str(s2, sizeof s2, rr[2]);
        rp(&rep, "\nAS-IS REBUILD x3: %s, %s, %s -> %s\n", s0, s1, s2, res.as_is_consistent ? "consistent" : "NOT consistent");
        rp(&rep, "RESULT: %s (recorded %d)\n", res.reproduced ? (b.result == 0 ? "the recorded SUCCESS reproduces (the pipeline builds)" : "the recorded failure REPRODUCES") : rr[2] == VK_SUCCESS ? "the pipeline builds here: does NOT reproduce" : (int32_t)rr[2] == b.result ? "same VkResult but not consistent" : "fails differently from the recorded result", b.result);
        if (flags & LAGFX_PIPELINE_REPRO_MATRIX) run_matrix(&c, &b.desc, vs, b.vs_bytes, fs, b.fs_bytes, b.vs_entry, b.fs_entry);
        if (flags & LAGFX_PIPELINE_REPRO_M13) {
            const char *m8 = getenv("LAGFX_PIPELINE_MATRIX_DIR");
            lagfx_matrix_env_t me = { .have_local_read = vk->have_local_read, .have_raster_order = vk->have_raster_order, .max_color_att = vk->phys_props.limits.maxColorAttachments,
                                      .max_per_stage_input_att = vk->phys_props.limits.maxPerStageDescriptorInputAttachments, .m8_dir = m8 && m8[0] ? m8 : bundle_dir, .emit = m13_emit, .user = &rep };
            lagfx_pipeline_desc_t md = b.desc;
            md.layout = c.layout, md.vertex_spv = vs, md.vertex_spv_bytes = b.vs_bytes, md.fragment_spv = fs, md.fragment_spv_bytes = b.fs_bytes, md.vertex_entry_point = b.vs_entry, md.fragment_entry_point = b.fs_entry;
            rp(&rep, "\n-- the -13 matrix (DIAGNOSTIC: every row builds a COPY; rows M0..M10) --\n");
            lagfx_matrix_result_t mr;
            lagfx_pipeline_matrix_run(c.dev, &md, b.dsl, b.n_dsl, &me, &mr);
        }
        if (flags & LAGFX_PIPELINE_REPRO_REDUCE) {
            lagfx_reduce_cfg_t cfg;
            lagfx_reduce_target_t tg;
            reduce_cfg_from(&cfg, opts, &b, bundle_dir, &rep, &tg);
            cfg.dev = c.dev, cfg.layout = c.layout, cfg.isolate = 0;
            rp(&rep, "\n-- the fragment-module reduction ladder (DIAGNOSTIC: every row builds a rewritten COPY; in-process: a compiler crash ends the process, use ISOLATE) --\n");
            lagfx_reduce_result_t rr;
            lagfx_pipeline_reduce_run(&cfg, &rr);
        }
        res.variants_ok = c.ok, res.variants_failed = c.fail, res.variants_na = c.na;
        if (flags & LAGFX_PIPELINE_REPRO_MATRIX) rp(&rep, "variant-matrix a..j + n (the OLD variant rows, n = the D2 NaN-min/max candidate; includes the d0/d1 stub-stage CONTROLS that build by design, NOT the -13 matrix M0..M10, whose own line is 'pipeline-matrix: summary'): %u built, %u failed, %u not applicable\n", c.ok, c.fail, c.na);
    }
cleanup:
    if (c.layout && c.layout != vk->empty_layout) vkDestroyPipelineLayout(c.dev, c.layout, NULL);
    if (c.dsl) vkDestroyDescriptorSetLayout(c.dev, c.dsl, NULL);
    lagfx_vk_shutdown(vk);
    lagfx_bundle_free(&b);
out:
    if (result) *result = res;
    if (report) *report = rep.p; else free(rep.p);
    return status;
}

void lagfx_pipeline_repro_free_report(char *report) { free(report); }

#endif /* LAGFX_HAVE_VULKAN */
