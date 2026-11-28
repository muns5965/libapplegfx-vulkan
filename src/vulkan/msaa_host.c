/* libapplegfx-vulkan — host-side multisample machinery not reachable from the guest yet (see msaa_host.h). SPDX-License-Identifier: MIT */
#include "msaa_host.h"
#include "../air2spv/spv_builder.h"

#include <stdlib.h>
#include <string.h>

const lagfx_msaa_inject_t *lagfx_msaa_inject;

/* SPIR-V numbers used below (SPIR-V specification, "Binary Form"): the ones the builder header does not name */
enum {
    SPV_OP_TYPE_BOOL_ = 20, SPV_OP_IMAGE_FETCH = 95, SPV_OP_IMAGE_QUERY_SAMPLES = 107, SPV_OP_CONVERT_S_TO_F = 111, SPV_OP_CONVERT_U_TO_F = 112,
    SPV_CAP_SAMPLE_RATE_SHADING = 35, SPV_CAP_IMAGE_QUERY = 50,
    SPV_DECO_BLOCK = 2, SPV_DECO_ARRAY_STRIDE = 6, SPV_DECO_BUILTIN = 11, SPV_DECO_FLAT = 14, SPV_DECO_LOCATION = 30, SPV_DECO_BINDING = 33, SPV_DECO_SET = 34, SPV_DECO_OFFSET = 35,
    SPV_BUILTIN_SAMPLE_ID = 18, SPV_BUILTIN_SAMPLE_MASK = 20,
    SPV_IMGOP_SAMPLE = 0x40,
    SPV_SC_UC = 0, SPV_SC_INPUT = 1, SPV_SC_OUTPUT = 3, SPV_SC_SSBO = 12,
};

static void emit_(lagfx_spv_builder_t *b, bool *bad, uint32_t opc, const uint32_t *a, uint32_t n) { if (!lagfx_spv_builder_emit_op(b, opc, a, n)) *bad = true; }
#define OP(opc, ...) emit_(b, &bad, (opc), (const uint32_t[]){ __VA_ARGS__ }, (uint32_t)(sizeof((const uint32_t[]){ __VA_ARGS__ }) / sizeof(uint32_t)))
#define ID() lagfx_spv_builder_alloc_id(b)

static uint32_t *finish(lagfx_spv_builder_t *b, bool bad, size_t *bytes) {
    size_t n = 0; uint8_t *m = bad ? NULL : lagfx_spv_builder_finish(b, &n);
    lagfx_spv_builder_free(b);
    if (bytes) *bytes = m ? n : 0;
    return (uint32_t *)m;
}

uint32_t *lagfx_msaa_spv_fetch_cs(uint32_t width, uint32_t height, uint32_t samples, bool uint_texels, size_t *bytes) {
    if (bytes) *bytes = 0;
    if (!width || !height || !samples || samples > 16u) return NULL;
    lagfx_spv_builder_t *b = lagfx_spv_builder_create(512);
    if (!b) return NULL;
    bool bad = false;
    const uint32_t t_void = ID(), t_fn = ID(), t_float = ID(), t_uint = ID(), t_int = ID(), t_v2i = ID(), t_v3u = ID(), t_vec = ID(), t_img = ID(), p_img = ID(), v_img = ID(),
        t_rta = ID(), t_st = ID(), p_st = ID(), v_out = ID(), p_vec = ID(), p_in3 = ID(), v_gid = ID(), c_zero = ID(), c_w = ID(), c_n = ID(), c_q = ID(), fn = ID(), lbl = ID(), c_fzero = ID();
    uint32_t c_s[16];
    for (uint32_t s = 0; s < samples; s++) c_s[s] = ID();
    OP(LAGFX_SPV_OP_CAPABILITY, LAGFX_SPV_CAPABILITY_SHADER);
    OP(LAGFX_SPV_OP_CAPABILITY, SPV_CAP_IMAGE_QUERY);
    OP(LAGFX_SPV_OP_MEMORY_MODEL, LAGFX_SPV_ADDRESSING_MODEL_LOGICAL, LAGFX_SPV_MEMORY_MODEL_GLSL450);
    { const uint32_t pre[2] = { LAGFX_SPV_EXECUTION_MODEL_GLCOMPUTE, fn }, suf[3] = { v_img, v_out, v_gid };   /* SPIR-V 1.4: every global the entry point uses is in the interface */
      if (!lagfx_spv_builder_emit_op_string(b, LAGFX_SPV_OP_ENTRY_POINT, pre, 2, "main", suf, 3)) bad = true; }
    OP(LAGFX_SPV_OP_EXECUTION_MODE, fn, LAGFX_SPV_EXECUTION_MODE_LOCAL_SIZE, 1, 1, 1);
    OP(LAGFX_SPV_OP_DECORATE, v_gid, SPV_DECO_BUILTIN, LAGFX_SPV_BUILTIN_GLOBAL_INVOCATION_ID);
    OP(LAGFX_SPV_OP_DECORATE, v_img, SPV_DECO_SET, 0); OP(LAGFX_SPV_OP_DECORATE, v_img, SPV_DECO_BINDING, 0);
    OP(LAGFX_SPV_OP_DECORATE, v_out, SPV_DECO_SET, 0); OP(LAGFX_SPV_OP_DECORATE, v_out, SPV_DECO_BINDING, 1);
    OP(LAGFX_SPV_OP_DECORATE, t_rta, SPV_DECO_ARRAY_STRIDE, 16);
    OP(LAGFX_SPV_OP_DECORATE, t_st, SPV_DECO_BLOCK);
    OP(LAGFX_SPV_OP_MEMBER_DECORATE, t_st, 0, SPV_DECO_OFFSET, 0);
    OP(LAGFX_SPV_OP_TYPE_VOID, t_void);
    OP(LAGFX_SPV_OP_TYPE_FUNCTION, t_fn, t_void);
    OP(LAGFX_SPV_OP_TYPE_FLOAT, t_float, 32);
    OP(LAGFX_SPV_OP_TYPE_INT, t_uint, 32, 0);
    OP(LAGFX_SPV_OP_TYPE_INT, t_int, 32, 1);
    OP(LAGFX_SPV_OP_TYPE_VECTOR, t_v2i, t_int, 2);
    OP(LAGFX_SPV_OP_TYPE_VECTOR, t_v3u, t_uint, 3);
    const uint32_t t_texel = uint_texels ? t_uint : t_float;   /* the element type of the image's sampled type and of the output vector */
    OP(LAGFX_SPV_OP_TYPE_VECTOR, t_vec, t_texel, 4);
    /* OpTypeImage %sampledType 2D Depth=0 Arrayed=0 MS=1 Sampled=1 Unknown */
    OP(LAGFX_SPV_OP_TYPE_IMAGE, t_img, t_texel, LAGFX_SPV_DIM_2D, 0, 0, 1, 1, LAGFX_SPV_IMAGE_FORMAT_UNKNOWN);
    OP(LAGFX_SPV_OP_TYPE_POINTER, p_img, SPV_SC_UC, t_img);
    OP(LAGFX_SPV_OP_VARIABLE, p_img, v_img, SPV_SC_UC);
    OP(LAGFX_SPV_OP_TYPE_RUNTIME_ARRAY, t_rta, t_vec);
    OP(LAGFX_SPV_OP_TYPE_STRUCT, t_st, t_rta);
    OP(LAGFX_SPV_OP_TYPE_POINTER, p_st, SPV_SC_SSBO, t_st);
    OP(LAGFX_SPV_OP_VARIABLE, p_st, v_out, SPV_SC_SSBO);
    OP(LAGFX_SPV_OP_TYPE_POINTER, p_vec, SPV_SC_SSBO, t_vec);
    OP(LAGFX_SPV_OP_TYPE_POINTER, p_in3, SPV_SC_INPUT, t_v3u);
    OP(LAGFX_SPV_OP_VARIABLE, p_in3, v_gid, SPV_SC_INPUT);
    OP(LAGFX_SPV_OP_CONSTANT, t_uint, c_zero, 0);
    OP(LAGFX_SPV_OP_CONSTANT, t_uint, c_w, width);
    OP(LAGFX_SPV_OP_CONSTANT, t_uint, c_n, samples);
    OP(LAGFX_SPV_OP_CONSTANT, t_uint, c_q, width * height * samples);
    { const float z = 0.0f; uint32_t zb; memcpy(&zb, &z, 4); OP(LAGFX_SPV_OP_CONSTANT, t_float, c_fzero, zb); }
    for (uint32_t s = 0; s < samples; s++) OP(LAGFX_SPV_OP_CONSTANT, t_uint, c_s[s], s);
    OP(LAGFX_SPV_OP_FUNCTION, t_void, fn, LAGFX_SPV_FUNCTION_CONTROL_NONE, t_fn);
    OP(LAGFX_SPV_OP_LABEL, lbl);
    const uint32_t gidv = ID(), x = ID(), y = ID(), xi = ID(), yi = ID(), coord = ID(), im = ID(), y_w = ID(), pix = ID(), base = ID();
    OP(LAGFX_SPV_OP_LOAD, t_v3u, gidv, v_gid);
    OP(LAGFX_SPV_OP_COMPOSITE_EXTRACT, t_uint, x, gidv, 0);
    OP(LAGFX_SPV_OP_COMPOSITE_EXTRACT, t_uint, y, gidv, 1);
    OP(LAGFX_SPV_OP_BITCAST, t_int, xi, x);
    OP(LAGFX_SPV_OP_BITCAST, t_int, yi, y);
    OP(LAGFX_SPV_OP_COMPOSITE_CONSTRUCT, t_v2i, coord, xi, yi);
    OP(LAGFX_SPV_OP_LOAD, t_img, im, v_img);
    OP(LAGFX_SPV_OP_IMUL, t_uint, y_w, y, c_w);
    OP(LAGFX_SPV_OP_IADD, t_uint, pix, y_w, x);
    OP(LAGFX_SPV_OP_IMUL, t_uint, base, pix, c_n);
    for (uint32_t s = 0; s < samples; s++) {
        const uint32_t v = ID(), idx = ID(), ac = ID();
        OP(SPV_OP_IMAGE_FETCH, t_vec, v, im, coord, SPV_IMGOP_SAMPLE, c_s[s]);   /* OpImageFetch %vec %image %coord Sample %s */
        OP(LAGFX_SPV_OP_IADD, t_uint, idx, base, c_s[s]);
        OP(LAGFX_SPV_OP_ACCESS_CHAIN, p_vec, ac, v_out, c_zero, idx);
        OP(LAGFX_SPV_OP_STORE, ac, v);
    }
    { const uint32_t ns = ID(), nsv = ID(), qv = ID(), ac = ID();
      OP(SPV_OP_IMAGE_QUERY_SAMPLES, t_uint, ns, im);
      if (uint_texels) OP(LAGFX_SPV_OP_COMPOSITE_CONSTRUCT, t_vec, qv, ns, c_zero, c_zero, c_zero);
      else { OP(SPV_OP_CONVERT_U_TO_F, t_float, nsv, ns); OP(LAGFX_SPV_OP_COMPOSITE_CONSTRUCT, t_vec, qv, nsv, c_fzero, c_fzero, c_fzero); }
      OP(LAGFX_SPV_OP_ACCESS_CHAIN, p_vec, ac, v_out, c_zero, c_q);   /* one writer is enough: every invocation stores the same value */
      OP(LAGFX_SPV_OP_STORE, ac, qv); }
    emit_(b, &bad, LAGFX_SPV_OP_RETURN, NULL, 0);
    emit_(b, &bad, LAGFX_SPV_OP_FUNCTION_END, NULL, 0);
    return finish(b, bad, bytes);
}

uint32_t *lagfx_msaa_spv_frag(lagfx_msaa_frag_kind_t kind, uint32_t mask, float alpha, size_t *bytes) {
    if (bytes) *bytes = 0;
    if (kind < LAGFX_MSAA_FRAG_SAMPLE_ID || kind > LAGFX_MSAA_FRAG_ALPHA) return NULL;
    lagfx_spv_builder_t *b = lagfx_spv_builder_create(256);
    if (!b) return NULL;
    bool bad = false;
    const uint32_t t_void = ID(), t_fn = ID(), t_float = ID(), t_v4 = ID(), p_out4 = ID(), v_col = ID(), t_int = ID(), t_uint = ID(), c_f0 = ID(), c_f1 = ID(), fn = ID(), lbl = ID();
    uint32_t iface[3], niface = 0, c_extra = ID(), v_sys = 0, p_sys = ID(), t_arr = 0, c_len = 0, c_idx = 0, p_int = 0;
    iface[niface++] = v_col;
    OP(LAGFX_SPV_OP_CAPABILITY, LAGFX_SPV_CAPABILITY_SHADER);
    if (kind == LAGFX_MSAA_FRAG_SAMPLE_ID) OP(LAGFX_SPV_OP_CAPABILITY, SPV_CAP_SAMPLE_RATE_SHADING);
    OP(LAGFX_SPV_OP_MEMORY_MODEL, LAGFX_SPV_ADDRESSING_MODEL_LOGICAL, LAGFX_SPV_MEMORY_MODEL_GLSL450);
    v_sys = (kind == LAGFX_MSAA_FRAG_ALPHA) ? 0 : ID();
    if (v_sys) iface[niface++] = v_sys;
    { const uint32_t pre[2] = { LAGFX_SPV_EXECUTION_MODEL_FRAGMENT, fn };
      if (!lagfx_spv_builder_emit_op_string(b, LAGFX_SPV_OP_ENTRY_POINT, pre, 2, "main", iface, niface)) bad = true; }
    OP(LAGFX_SPV_OP_EXECUTION_MODE, fn, LAGFX_SPV_EXECUTION_MODE_ORIGIN_UPPER_LEFT);
    OP(LAGFX_SPV_OP_DECORATE, v_col, SPV_DECO_LOCATION, 0);
    if (kind == LAGFX_MSAA_FRAG_SAMPLE_ID) { OP(LAGFX_SPV_OP_DECORATE, v_sys, SPV_DECO_BUILTIN, SPV_BUILTIN_SAMPLE_ID); OP(LAGFX_SPV_OP_DECORATE, v_sys, SPV_DECO_FLAT); }
    if (kind == LAGFX_MSAA_FRAG_SAMPLE_MASK_IN) { OP(LAGFX_SPV_OP_DECORATE, v_sys, SPV_DECO_BUILTIN, SPV_BUILTIN_SAMPLE_MASK); OP(LAGFX_SPV_OP_DECORATE, v_sys, SPV_DECO_FLAT); }
    if (kind == LAGFX_MSAA_FRAG_SAMPLE_MASK_OUT) OP(LAGFX_SPV_OP_DECORATE, v_sys, SPV_DECO_BUILTIN, SPV_BUILTIN_SAMPLE_MASK);
    OP(LAGFX_SPV_OP_TYPE_VOID, t_void);
    OP(LAGFX_SPV_OP_TYPE_FUNCTION, t_fn, t_void);
    OP(LAGFX_SPV_OP_TYPE_FLOAT, t_float, 32);
    OP(LAGFX_SPV_OP_TYPE_VECTOR, t_v4, t_float, 4);
    OP(LAGFX_SPV_OP_TYPE_INT, t_int, 32, 1);
    OP(LAGFX_SPV_OP_TYPE_INT, t_uint, 32, 0);
    OP(LAGFX_SPV_OP_TYPE_POINTER, p_out4, SPV_SC_OUTPUT, t_v4);
    OP(LAGFX_SPV_OP_VARIABLE, p_out4, v_col, SPV_SC_OUTPUT);
    { const float z = 0.0f, o = 1.0f; uint32_t zb, ob; memcpy(&zb, &z, 4); memcpy(&ob, &o, 4); OP(LAGFX_SPV_OP_CONSTANT, t_float, c_f0, zb); OP(LAGFX_SPV_OP_CONSTANT, t_float, c_f1, ob); }
    switch (kind) {
    case LAGFX_MSAA_FRAG_SAMPLE_ID:
        OP(LAGFX_SPV_OP_TYPE_POINTER, p_sys, SPV_SC_INPUT, t_int);
        OP(LAGFX_SPV_OP_VARIABLE, p_sys, v_sys, SPV_SC_INPUT);
        { const float e = 0.125f; uint32_t eb; memcpy(&eb, &e, 4); OP(LAGFX_SPV_OP_CONSTANT, t_float, c_extra, eb); }
        break;
    case LAGFX_MSAA_FRAG_SAMPLE_MASK_OUT: case LAGFX_MSAA_FRAG_SAMPLE_MASK_IN:
        t_arr = ID(), c_len = ID(), c_idx = ID(), p_int = ID();
        OP(LAGFX_SPV_OP_CONSTANT, t_uint, c_len, 1);
        OP(LAGFX_SPV_OP_CONSTANT, t_uint, c_idx, 0);
        OP(LAGFX_SPV_OP_TYPE_ARRAY, t_arr, t_int, c_len);   /* SampleMask is an array of 32-bit ints, one word per 32 samples */
        OP(LAGFX_SPV_OP_TYPE_POINTER, p_sys, kind == LAGFX_MSAA_FRAG_SAMPLE_MASK_IN ? SPV_SC_INPUT : SPV_SC_OUTPUT, t_arr);
        OP(LAGFX_SPV_OP_TYPE_POINTER, p_int, kind == LAGFX_MSAA_FRAG_SAMPLE_MASK_IN ? SPV_SC_INPUT : SPV_SC_OUTPUT, t_int);
        OP(LAGFX_SPV_OP_VARIABLE, p_sys, v_sys, kind == LAGFX_MSAA_FRAG_SAMPLE_MASK_IN ? SPV_SC_INPUT : SPV_SC_OUTPUT);
        if (kind == LAGFX_MSAA_FRAG_SAMPLE_MASK_OUT) OP(LAGFX_SPV_OP_CONSTANT, t_int, c_extra, mask);
        break;
    case LAGFX_MSAA_FRAG_ALPHA: { uint32_t ab; memcpy(&ab, &alpha, 4); OP(LAGFX_SPV_OP_CONSTANT, t_float, c_extra, ab); } break;
    default: break;
    }
    OP(LAGFX_SPV_OP_FUNCTION, t_void, fn, LAGFX_SPV_FUNCTION_CONTROL_NONE, t_fn);
    OP(LAGFX_SPV_OP_LABEL, lbl);
    uint32_t col = ID();
    switch (kind) {
    case LAGFX_MSAA_FRAG_SAMPLE_ID: {
        const uint32_t s = ID(), sf = ID(), s1 = ID(), v = ID();
        OP(LAGFX_SPV_OP_LOAD, t_int, s, v_sys);
        OP(SPV_OP_CONVERT_S_TO_F, t_float, sf, s);
        OP(LAGFX_SPV_OP_FADD, t_float, s1, sf, c_f1);
        OP(LAGFX_SPV_OP_FMUL, t_float, v, s1, c_extra);   /* (SampleId + 1) / 8 */
        OP(LAGFX_SPV_OP_COMPOSITE_CONSTRUCT, t_v4, col, v, c_f0, c_f0, c_f1);
        break; }
    case LAGFX_MSAA_FRAG_SAMPLE_MASK_OUT: {
        const uint32_t ac = ID();
        OP(LAGFX_SPV_OP_ACCESS_CHAIN, p_int, ac, v_sys, c_idx);
        OP(LAGFX_SPV_OP_STORE, ac, c_extra);
        OP(LAGFX_SPV_OP_COMPOSITE_CONSTRUCT, t_v4, col, c_f1, c_f0, c_f0, c_f1);
        break; }
    case LAGFX_MSAA_FRAG_SAMPLE_MASK_IN: {
        const uint32_t ac = ID(), m = ID(), mf = ID();
        OP(LAGFX_SPV_OP_ACCESS_CHAIN, p_int, ac, v_sys, c_idx);
        OP(LAGFX_SPV_OP_LOAD, t_int, m, ac);
        OP(SPV_OP_CONVERT_S_TO_F, t_float, mf, m);
        OP(LAGFX_SPV_OP_COMPOSITE_CONSTRUCT, t_v4, col, mf, c_f0, c_f0, c_f1);
        break; }
    default:
        OP(LAGFX_SPV_OP_COMPOSITE_CONSTRUCT, t_v4, col, c_f1, c_f0, c_f0, c_extra);
        break;
    }
    OP(LAGFX_SPV_OP_STORE, v_col, col);
    emit_(b, &bad, LAGFX_SPV_OP_RETURN, NULL, 0);
    emit_(b, &bad, LAGFX_SPV_OP_FUNCTION_END, NULL, 0);
    return finish(b, bad, bytes);
}

bool lagfx_msaa_ms_sampled_view(VkDevice dev, VkImage img, VkFormat fmt, VkImageAspectFlags aspect, VkSampleCountFlagBits samples, VkImageUsageFlags usage, lagfx_msaa_stats_t *st, VkImageView *out) {
    *out = VK_NULL_HANDLE;
    if (!dev || !img || samples == VK_SAMPLE_COUNT_1_BIT || !(usage & VK_IMAGE_USAGE_SAMPLED_BIT)) { if (st) st->ms_fetch_refused++; return false; }   /* a 1x image is no multisample fetch source; without SAMPLED the image cannot be bound */
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .subresourceRange = { aspect, 0, 1, 0, 1 } };
    if (vkCreateImageView(dev, &vi, NULL, out) != VK_SUCCESS) { *out = VK_NULL_HANDLE; if (st) st->ms_fetch_refused++; return false; }
    if (st) st->ms_fetch_views++;
    return true;
}

/* ---- depth / stencil resolve ---- */
bool lagfx_msaa_dsres_probe(VkPhysicalDevice pd, lagfx_msaa_dsres_caps_t *c) {
    memset(c, 0, sizeof *c);
    VkPhysicalDeviceProperties base;
    vkGetPhysicalDeviceProperties(pd, &base);
    if (base.apiVersion < VK_API_VERSION_1_2) {   /* 1.1 (Adreno 730 on the S8): the struct is the KHR one, valid only when the device advertises VK_KHR_depth_stencil_resolve (and vkGetPhysicalDeviceProperties2 is core 1.1) */
        if (base.apiVersion < VK_API_VERSION_1_1) return false;
        uint32_t n = 0; bool found = false;
        if (vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL) != VK_SUCCESS || !n) return false;
        VkExtensionProperties *e = calloc(n, sizeof *e);
        if (!e) return false;
        if (vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e) == VK_SUCCESS)
            for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME)) found = true;
        free(e);
        if (!found) return false;
    }
    VkPhysicalDeviceDepthStencilResolveProperties ds = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &ds };
    vkGetPhysicalDeviceProperties2(pd, &p2);
    c->probed = true, c->depth_modes = ds.supportedDepthResolveModes, c->stencil_modes = ds.supportedStencilResolveModes;
    c->independent_none = ds.independentResolveNone == VK_TRUE, c->independent = ds.independentResolve == VK_TRUE;
    return true;
}

const char *lagfx_msaa_dsres_why_name(int why) {
    static const char *const n[] = { "ok", "not_probed", "unknown_filter", "mode_unsupported", "independent_resolve_unsupported", "no_aspect" };
    return why >= 0 && why < (int)(sizeof n / sizeof n[0]) ? n[why] : "?";
}

int lagfx_msaa_dsres_choose(const lagfx_msaa_dsres_caps_t *c, bool depth, uint32_t dfilter, bool stencil, uint32_t sfilter, VkResolveModeFlagBits *dmode, VkResolveModeFlagBits *smode) {
    VkResolveModeFlagBits dm = 0, sm = 0;
    *dmode = *smode = 0;
    if (!depth && !stencil) return LAGFX_MSAA_DSRES_NO_ASPECT;
    if (!c || !c->probed) return LAGFX_MSAA_DSRES_NOT_PROBED;
    if (depth) {
        dm = dfilter == LAGFX_MSAA_DEPTH_FILTER_SAMPLE0 ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : dfilter == LAGFX_MSAA_DEPTH_FILTER_MIN ? VK_RESOLVE_MODE_MIN_BIT
           : dfilter == LAGFX_MSAA_DEPTH_FILTER_MAX ? VK_RESOLVE_MODE_MAX_BIT : 0;
        if (!dm) return LAGFX_MSAA_DSRES_UNKNOWN_FILTER;
        if (!(c->depth_modes & dm)) return LAGFX_MSAA_DSRES_MODE_UNSUPPORTED;
    }
    if (stencil) {
        if (sfilter == LAGFX_MSAA_STENCIL_FILTER_SAMPLE0) sm = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        else if (sfilter == LAGFX_MSAA_STENCIL_FILTER_DEPTH_RESOLVED) { if (!depth) return LAGFX_MSAA_DSRES_UNKNOWN_FILTER; sm = dm; }   /* "the sample the depth resolve picked": only the same mode can say that */
        else return LAGFX_MSAA_DSRES_UNKNOWN_FILTER;
        if (!(c->stencil_modes & sm)) return LAGFX_MSAA_DSRES_MODE_UNSUPPORTED;
    }
    if (depth && stencil && dm != sm && !c->independent) {   /* VUID-VkRenderingInfo-pDepthAttachment-06104 family: different modes on one image need independentResolve (NONE needs independentResolveNone) */
        if (!(c->independent_none && (dm == VK_RESOLVE_MODE_NONE || sm == VK_RESOLVE_MODE_NONE))) return LAGFX_MSAA_DSRES_INDEPENDENT_UNSUPPORTED;
    }
    *dmode = dm, *smode = sm;
    return LAGFX_MSAA_DSRES_OK;
}

int lagfx_msaa_dsres_attachments(const lagfx_msaa_dsres_caps_t *c, bool depth, uint32_t dfilter, bool stencil, uint32_t sfilter, VkImageView rv, VkImageLayout rl,
                                 VkRenderingAttachmentInfo *dai, VkRenderingAttachmentInfo *sai, lagfx_msaa_stats_t *st) {
    VkResolveModeFlagBits dm, sm;
    const int why = lagfx_msaa_dsres_choose(c, depth, dfilter, stencil, sfilter, &dm, &sm);
    if (why != LAGFX_MSAA_DSRES_OK) { if (st) st->ds_resolve_refused++; return why; }
    if (depth && dai) dai->resolveMode = dm, dai->resolveImageView = rv, dai->resolveImageLayout = rl;
    if (stencil && sai) sai->resolveMode = sm, sai->resolveImageView = rv, sai->resolveImageLayout = rl;
    return LAGFX_MSAA_DSRES_OK;
}
