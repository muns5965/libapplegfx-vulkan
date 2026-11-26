/* SPDX-License-Identifier: MIT */
#include "pipeline_build.h"
#include "pipeline_bundle.h"
#include "common/log.h"
#include "common/policy.h"
#ifdef LAGFX_HAVE_VULKAN
#include "pipeline_matrix.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FNV-1a 64 (with this repo's historical offset basis 1469598103934665603, not the textbook one) of a byte range: the "hash=" printed by the "pipeline-build-fail:" log line and the name of the spv_<hash>_vs/fs.spv dump files. */
uint64_t lagfx_pipeline_spv_hash(const void *data, size_t n) {
    const uint8_t *b = data;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

#ifdef LAGFX_HAVE_VULKAN

static const char *vr_name(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_INVALID_SHADER_NV: return "VK_ERROR_INVALID_SHADER_NV";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    case VK_PIPELINE_COMPILE_REQUIRED: return "VK_PIPELINE_COMPILE_REQUIRED";
    default: return "VK_ERROR_(other)";
    }
}

static uint64_t fnv64(const void *data, size_t n) { return lagfx_pipeline_spv_hash(data, n); }

/* One diagnostic line per DISTINCT failure key (bounded to 8 keys per process; a repeat of a key is silent, the 9th distinct key says once that the rest is
 * suppressed). Called only after vkCreateGraphicsPipelines failed; creates the two shader modules separately (and destroys them again) to tell which one the driver
 * rejects. Builds nothing that is used afterwards. */
#define FAIL_KEYS_MAX 8u
static void log_build_failure(VkDevice device, const lagfx_pipeline_desc_t *desc, const VkPipelineVertexInputStateCreateInfo *vin, VkPrimitiveTopology topo, VkFormat color_fmt, VkResult vr) {
    static struct { uint64_t key; uint64_t count; } seen[FAIL_KEYS_MAX];
    static uint32_t nseen;
    static uint64_t suppressed;
    uint64_t vh = desc->vertex_spv ? fnv64(desc->vertex_spv, desc->vertex_spv_bytes) : 0, fh = desc->fragment_spv ? fnv64(desc->fragment_spv, desc->fragment_spv_bytes) : 0;
    char attrs[256];
    size_t ao = 0;
    attrs[0] = 0;
    for (uint32_t i = 0; i < vin->vertexAttributeDescriptionCount && ao + 40 < sizeof attrs; i++) {
        const VkVertexInputAttributeDescription *a = &vin->pVertexAttributeDescriptions[i];
        ao += (size_t)snprintf(attrs + ao, sizeof attrs - ao, "%sloc%u:fmt%d@%u", i ? "," : "", a->location, (int)a->format, a->offset);
    }
    uint64_t key = vh * 1099511628211ull ^ fh;
    key = (key ^ (uint64_t)(int64_t)vr) * 1099511628211ull;
    key = (key ^ (uint64_t)color_fmt) * 1099511628211ull;
    key = (key ^ (uint64_t)topo) * 1099511628211ull;
    key = (key ^ fnv64(attrs, strlen(attrs))) * 1099511628211ull;
    key = (key ^ (uint64_t)vin->vertexBindingDescriptionCount ^ ((uint64_t)(desc->vertex_entry_point ? fnv64(desc->vertex_entry_point, strlen(desc->vertex_entry_point)) : 0) << 1)) * 1099511628211ull;
    for (uint32_t i = 0; i < nseen; i++) if (seen[i].key == key) { seen[i].count++; return; }
    if (nseen >= FAIL_KEYS_MAX) {
        if (!suppressed++) LAGFX_ERR("pipeline-build-fail: more than %u distinct failure keys; further failures are not logged", FAIL_KEYS_MAX);
        return;
    }
    seen[nseen].key = key, seen[nseen].count = 1, nseen++;
    /* each module on its own */
    const char *vm = "not-checked(no spirv given)", *fm = "not-checked(no spirv given)";
    char vb[48], fb[48];
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m = VK_NULL_HANDLE;
    if (desc->vertex_spv && desc->vertex_spv_bytes && !(desc->vertex_spv_bytes & 3u)) {
        ci.codeSize = desc->vertex_spv_bytes, ci.pCode = desc->vertex_spv;
        VkResult r = vkCreateShaderModule(device, &ci, NULL, &m);
        snprintf(vb, sizeof vb, "%s", r == VK_SUCCESS ? "accepted" : vr_name(r)), vm = vb;
        if (m) vkDestroyShaderModule(device, m, NULL);
        m = VK_NULL_HANDLE;
    }
    if (desc->fragment_spv && desc->fragment_spv_bytes && !(desc->fragment_spv_bytes & 3u)) {
        ci.codeSize = desc->fragment_spv_bytes, ci.pCode = desc->fragment_spv;
        VkResult r = vkCreateShaderModule(device, &ci, NULL, &m);
        snprintf(fb, sizeof fb, "%s", r == VK_SUCCESS ? "accepted" : vr_name(r)), fm = fb;
        if (m) vkDestroyShaderModule(device, m, NULL);
    }
    LAGFX_ERR("pipeline-build-fail: result=%s(%d) vs[bytes=%zu hash=%016llx module=%s entry=%s] fs[bytes=%zu hash=%016llx module=%s entry=%s] "
              "vertex_bindings=%u vertex_attrs=%u[%s] stride=%u color_format=%d topology=%d dynamic_rendering=1(VkPipelineRenderingCreateInfo,no-renderpass) "
              "caps_mask=0x%x distinct_failures=%u",
              vr_name(vr), (int)vr, desc->vertex_spv_bytes, (unsigned long long)vh, vm, desc->vertex_entry_point ? desc->vertex_entry_point : "main",
              desc->fragment_spv_bytes, (unsigned long long)fh, fm, desc->fragment_entry_point ? desc->fragment_entry_point : "main",
              vin->vertexBindingDescriptionCount, vin->vertexAttributeDescriptionCount, attrs,
              vin->vertexBindingDescriptionCount ? vin->pVertexBindingDescriptions[0].stride : 0u, (int)color_fmt, (int)topo, desc->caps_enabled, nseen);
}

/* Failure bundle, informational part: the Vulkan create-info state exactly as built above (one fact per line; the bundle prefixes each line with "# "). */
static void describe_state(char *buf, size_t cap, const VkGraphicsPipelineCreateInfo *g, const VkPipelineRenderingCreateInfo *ri) {
    size_t o = 0;
#define W(...) do { if (o + 1u < cap) { int w_ = snprintf(buf + o, cap - o, __VA_ARGS__); if (w_ > 0) o = (size_t)w_ >= cap - o ? cap - 1u : o + (size_t)w_; } } while (0)
    const VkPipelineVertexInputStateCreateInfo *vi = g->pVertexInputState;
    for (uint32_t i = 0; i < vi->vertexBindingDescriptionCount; i++) W("vertex_binding %u stride=%u rate=%d\n", vi->pVertexBindingDescriptions[i].binding, vi->pVertexBindingDescriptions[i].stride, (int)vi->pVertexBindingDescriptions[i].inputRate);
    for (uint32_t i = 0; i < vi->vertexAttributeDescriptionCount; i++) W("vertex_attr loc=%u binding=%u format=%d offset=%u\n", vi->pVertexAttributeDescriptions[i].location, vi->pVertexAttributeDescriptions[i].binding, (int)vi->pVertexAttributeDescriptions[i].format, vi->pVertexAttributeDescriptions[i].offset);
    W("topology %d primitiveRestart %u\n", (int)g->pInputAssemblyState->topology, g->pInputAssemblyState->primitiveRestartEnable);
    W("rendering viewMask=%u colorAttachments=%u depthFormat=%d stencilFormat=%d\n", ri->viewMask, ri->colorAttachmentCount, (int)ri->depthAttachmentFormat, (int)ri->stencilAttachmentFormat);
    for (uint32_t i = 0; i < ri->colorAttachmentCount; i++) W("color_format[%u] %d\n", i, (int)ri->pColorAttachmentFormats[i]);
    const VkPipelineColorBlendStateCreateInfo *cb = g->pColorBlendState;
    W("blend flags=0x%x logicOpEnable=%u attachments=%u\n", (unsigned)cb->flags, cb->logicOpEnable, cb->attachmentCount);
    for (uint32_t i = 0; i < cb->attachmentCount; i++) { const VkPipelineColorBlendAttachmentState *a = &cb->pAttachments[i];
        W("blend_att[%u] enable=%u color=%d/%d/%d alpha=%d/%d/%d writeMask=0x%x\n", i, a->blendEnable, (int)a->srcColorBlendFactor, (int)a->dstColorBlendFactor, (int)a->colorBlendOp, (int)a->srcAlphaBlendFactor, (int)a->dstAlphaBlendFactor, (int)a->alphaBlendOp, (unsigned)a->colorWriteMask); }
    const VkPipelineRasterizationStateCreateInfo *r = g->pRasterizationState;
    W("raster polygon=%d cull=%d front=%d depthClamp=%u discard=%u depthBias=%u lineWidth=%g\n", (int)r->polygonMode, (int)r->cullMode, (int)r->frontFace, r->depthClampEnable, r->rasterizerDiscardEnable, r->depthBiasEnable, (double)r->lineWidth);
    W("multisample samples=%d sampleShading=%u\n", (int)g->pMultisampleState->rasterizationSamples, g->pMultisampleState->sampleShadingEnable);
    const VkPipelineDepthStencilStateCreateInfo *d = g->pDepthStencilState;
    W("depth_stencil(static) depthTest=%u depthWrite=%u compare=%d stencilTest=%u (dynamic when listed below)\n", d->depthTestEnable, d->depthWriteEnable, (int)d->depthCompareOp, d->stencilTestEnable);
    W("viewport(static,ignored: dynamic) x=%g y=%g w=%g h=%g\n", (double)g->pViewportState->pViewports[0].x, (double)g->pViewportState->pViewports[0].y, (double)g->pViewportState->pViewports[0].width, (double)g->pViewportState->pViewports[0].height);
    W("dynamic_states");
    for (uint32_t i = 0; i < g->pDynamicState->dynamicStateCount; i++) W(" %d", (int)g->pDynamicState->pDynamicStates[i]);
    W("\n");
    for (uint32_t i = 0; i < g->stageCount; i++) W("stage %d module=handle entry=%s specialization=none\n", (int)g->pStages[i].stage, g->pStages[i].pName);
#undef W
    buf[o < cap ? o : cap - 1u] = 0;
}

static _Thread_local int32_t g_last_vr;   /* VkResult of this thread's last vkCreateGraphicsPipelines in lagfx_pipeline_build() (the repro reads it) */
int32_t lagfx_pipeline_build_last_result(void) { return g_last_vr; }
static _Thread_local int g_quiet;
void lagfx_pipeline_build_quiet(int on) { g_quiet = on; }

/* MTLVertexFormat (public Metal enum) -> VkFormat for the formats the PSO attribute decode may carry; VK_FORMAT_UNDEFINED = not handled (the caller falls back to
 * the reflected float tight-pack). 37 is UInt2 (UInt is 36) and 52 is ShortNormalized (UChar4Normalized_BGRA is 42); the earlier table had 37 as a one-component UInt and 52 as
 * BGRA8, which are other enum values. Neither 37 nor 52 appears in the device dumps of runs 231501/230109 (only 27 and 29 do), so this is a table correction against the
 * public enum, not a fix of an observed symptom. */
VkFormat lagfx_pso_vertex_format_to_vk(uint32_t f) {
    switch (f) {
    case 3:  return VK_FORMAT_R8G8B8A8_UINT;          /* UChar4 */
    case 9:  return VK_FORMAT_B8G8R8A8_UNORM;         /* UChar4Normalized — CA pool colors are BGRA-ordered (the PBGRA pipes); R8G8B8A8 rendered blue UI as red */
    case 27: return VK_FORMAT_R16G16B16A16_SFLOAT;    /* Half4: dump pso 93 family, colour @32 of a 48 byte vertex */
    case 28: return VK_FORMAT_R32_SFLOAT;             /* Float */
    case 29: return VK_FORMAT_R32G32_SFLOAT;          /* Float2 */
    case 30: return VK_FORMAT_R32G32B32_SFLOAT;       /* Float3 */
    case 31: return VK_FORMAT_R32G32B32A32_SFLOAT;    /* Float4 */
    case 37: return VK_FORMAT_R32G32_UINT;            /* UInt2 */
    case 52: return VK_FORMAT_R16_SNORM;              /* ShortNormalized */
    default: return VK_FORMAT_UNDEFINED;
    }
}

lagfx_status_t lagfx_pipeline_build(VkDevice device,
                                    const lagfx_pipeline_desc_t *desc,
                                    VkPipeline *out_pipeline) {
    if (!device || !desc || !desc->vertex_shader || !desc->fragment_shader
        || !out_pipeline) {
        return LAGFX_ERR_INVALID_ARG;
    }
    if (desc->layout == VK_NULL_HANDLE) {
        /* Vulkan spec (VUID-VkGraphicsPipelineCreateInfo-layout-06602):
         * layout must be valid even when shaders declare zero descriptor
         * bindings. Callers should pass the device's empty_layout for
         * the substitute triangle pipeline. */
        return LAGFX_ERR_INVALID_ARG;
    }

    /* Vertex input. Empty by default (procedural/vertex_id shaders). When the
     * reflected vertex shader declares stage-in attributes, build a non-empty
     * state (binding 0, tightly-packed R32..A32_SFLOAT) so the shader's
     * positions come from the bound guest vertex buffer instead of unbound
     * (= 0 = degenerate = black). Gated by the caller setting n_vtx_inputs. */
    VkVertexInputBindingDescription   vbind = {0};
    VkVertexInputAttributeDescription vattr[8];
    VkPipelineVertexInputStateCreateInfo vin = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    if (desc->n_vtx_inputs > 0u) {
        uint32_t nvi = desc->n_vtx_inputs > 8u ? 8u : desc->n_vtx_inputs;
        uint32_t off = 0u;
        /* GOAL-M2z: PSO-decoded attribute formats+offsets are authoritative
         * when present for every reflected attr (matched by order — both are
         * offset/location ordered). MTLVertexFormat -> VkFormat for the
         * observed set; unknown formats fall back to the reflected SFLOAT
         * tight-pack (and log, gated). The killer this fixes: fmt 9
         * (UChar4Normalized rgba8 color @32) was bound R32G32B32A32_SFLOAT ->
         * NaN color/alpha -> invisible panel fills. */
        bool use_pso_attrs = desc->n_pso_attrs >= nvi;
        for (uint32_t a = 0; use_pso_attrs && a < nvi; a++) {
            if (lagfx_pso_vertex_format_to_vk(desc->pso_attr_fmt[a]) == VK_FORMAT_UNDEFINED) use_pso_attrs = false;
        }
        for (uint32_t a = 0; a < nvi; a++) {
            uint32_t c = desc->vtx_in_comp[a]; if (c < 1u) c = 1u; if (c > 4u) c = 4u;
            VkFormat fmt = (c == 1u) ? VK_FORMAT_R32_SFLOAT :
                           (c == 2u) ? VK_FORMAT_R32G32_SFLOAT :
                           (c == 3u) ? VK_FORMAT_R32G32B32_SFLOAT :
                                       VK_FORMAT_R32G32B32A32_SFLOAT;
            uint32_t aoff = off;
            if (use_pso_attrs) {
                aoff = desc->pso_attr_off[a];
                fmt = lagfx_pso_vertex_format_to_vk(desc->pso_attr_fmt[a]);   /* the gate above guarantees a known format */
            }
            vattr[a] = (VkVertexInputAttributeDescription){
                .location = desc->vtx_in_loc[a], .binding = 0u, .format = fmt, .offset = aoff,
            };
            off += c * 4u;
        }
        /* Vertex stride, in priority order:
         *  1. desc->vtx_stride — the REAL stride decoded from the PSO's serialized
         *     MTLVertexDescriptor (lagfx_parse_pso_vertex_stride). Authoritative
         *     and per-pipeline (CoreAnimation composites = 48, fullscreen quad = 24).
         *  2. LAGFX_VTX_STRIDE env override (diagnostic / global probe).
         *  3. round8(sum of attr sizes) — the legacy heuristic; correct only when
         *     there is no trailing padding, and WRONG for the CA composites (tight
         *     sum rounds to 24 while the real stride is 48 → every other vertex
         *     misread → horizontal-band smear). Fallback of last resort. */
        uint32_t stride = (off + 7u) & ~7u;
        /* The decoded PSO stride is AUTHORITATIVE whenever present and sane.
         * lldb ground truth (GOAL-M2x, 2026-07-21, WindowServer
         * PGSerializerRenderCommandEncoder): the indexed-quad composite draws
         * (0x23/0x28/0x2c) bind a 48-byte-stride pool buffer — float4
         * SCREEN-SPACE pos@0 (w=1), float2 normalized texcoord@16, rgba8
         * color@32 — while the shader reflects only 2 attrs (extent 32). The
         * former "+8 B of the reflected extent" clamp rejected the real 48 →
         * stride 32 → v0 correct, v1/v2 read from attr padding → the
         * degenerate (v1==v2) triangles. The reflected extent is the shader's
         * VIEW of the vertex, not the buffer stride; only the PSO's serialized
         * MTLVertexDescriptor knows the buffer layout. */
        if (desc->vtx_stride >= off && desc->vtx_stride <= 256u)
            stride = desc->vtx_stride;
        const char *vs = getenv("LAGFX_VTX_STRIDE");
        if (vs) { unsigned long s = strtoul(vs, NULL, 0); if (s >= off && s <= 256u) stride = (uint32_t)s; }
        vbind = (VkVertexInputBindingDescription){
            .binding = 0u, .stride = stride, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        };
        vin.vertexBindingDescriptionCount   = 1u;
        vin.pVertexBindingDescriptions      = &vbind;
        vin.vertexAttributeDescriptionCount = nvi;
        vin.pVertexAttributeDescriptions    = vattr;
        if (getenv("LAGFX_DUMP_SPV")) {
            char ab[128]; size_t an = 0;
            for (uint32_t a = 0; a < nvi && an + 24 < sizeof(ab); a++)
                an += (size_t)snprintf(ab+an, sizeof(ab)-an, "loc%u:c%u@%u ",
                                       vattr[a].location, desc->vtx_in_comp[a], vattr[a].offset);
            LAGFX_LOG("VTXINPUT n=%u stride=%u attrs: %s", nvi, stride, ab);
        }
    }

    /* Input assembly */
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = desc->topology_plus1 ? (VkPrimitiveTopology)(desc->topology_plus1 - 1u) : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };

    /* Viewport with the Metal→Vulkan Y-FLIP (negative height, origin at
     * y=height). Metal clip space is Y-UP; Vulkan is Y-DOWN — without the flip
     * the guest's composite geometry renders vertically MIRRORED (proven live:
     * the cursor drew huge + UPSIDE-DOWN and UI rows collapsed into horizontal
     * bands, while our upright display test-boxes confirmed the display path
     * itself is correct). A negative-height viewport (Vulkan 1.1 / KHR_
     * maintenance1) is the canonical fix. Kill-switch: LAGFX_DISABLE_YFLIP. */
    bool yflip = (getenv("LAGFX_DISABLE_YFLIP") == NULL);
    VkViewport vp = {
        .x = 0.0f, .y = yflip ? 1080.0f : 0.0f,
        .width = 1920.0f, .height = yflip ? -1080.0f : 1080.0f,
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };
    VkRect2D sc = { .offset = {0, 0}, .extent = { .width = 1920, .height = 1080 } };
    VkPipelineViewportStateCreateInfo vpst = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp,
        .scissorCount = 1, .pScissors = &sc,
    };

    /* Rasterization state — hardcoded to triangle test values */
    VkPipelineRasterizationStateCreateInfo rsst = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };

    /* Multisample state */
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    /* Color blend attachment — PREMULTIPLIED src-over (CoreAnimation's
     * universal convention: MTLBlendFactorOne / OneMinusSourceAlpha). The
     * earlier non-premultiplied SRC_ALPHA factor multiplied the fragment's
     * already-premultiplied RGB by alpha AGAIN — for the material passes that
     * legitimately emit alpha≈0 with nonzero RGB (additive glow/blur terms),
     * that dropped the contribution entirely: the 2026-07-23 A/B
     * (LAGFX_DISABLE_M2_SRCOVER=1) showed per-pass RT 0xb go 0→18.8% nonblack
     * the moment blending stopped scaling RGB by alpha. Premultiplied
     * src-over keeps alpha-0 pixels preserving the target AND lets alpha-0
     * nonzero-RGB terms add, exactly as CA composites.
     * Kill-switch: LAGFX_DISABLE_M2_SRCOVER → no blending. */
    VkPipelineColorBlendAttachmentState blend_att = {
        .blendEnable         = LAGFX_POLICY("M2_SRCOVER") ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp        = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp        = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT
                        | VK_COLOR_COMPONENT_G_BIT
                        | VK_COLOR_COMPONENT_B_BIT
                        | VK_COLOR_COMPONENT_A_BIT,
    };
    if (desc->blend_set) {
        blend_att.blendEnable = desc->blend_enable ? VK_TRUE : VK_FALSE;
        blend_att.srcColorBlendFactor = (VkBlendFactor)desc->blend_src_rgb, blend_att.dstColorBlendFactor = (VkBlendFactor)desc->blend_dst_rgb;
        blend_att.colorBlendOp = (VkBlendOp)desc->blend_op_rgb;
        blend_att.srcAlphaBlendFactor = (VkBlendFactor)desc->blend_src_a, blend_att.dstAlphaBlendFactor = (VkBlendFactor)desc->blend_dst_a;
        blend_att.alphaBlendOp = (VkBlendOp)desc->blend_op_a;
        blend_att.colorWriteMask = (VkColorComponentFlags)desc->color_write_mask;
        if (!LAGFX_POLICY("M2_SRCOVER")) blend_att.blendEnable = VK_FALSE;   /* LAGFX_DISABLE_M2_SRCOVER stays a diagnostic switch for ALL blending, decoded state included */
    }
    VkPipelineColorBlendAttachmentState blend_multi[4];
    VkFormat color_fmts[4];
    const uint32_t ncol = (desc->n_color >= 2u && desc->n_color <= 4u) ? desc->n_color : 1u;
    if (ncol > 1u) {
        for (uint32_t i = 0; i < ncol; i++) {
            blend_multi[i] = blend_att;
            color_fmts[i] = (VkFormat)desc->color_formats[i];
            if (!desc->independent_blend) continue;   /* without the feature every element must be identical (VUID 00605), unused slots included */
            if (color_fmts[i] == VK_FORMAT_UNDEFINED) { blend_multi[i].blendEnable = VK_FALSE; blend_multi[i].colorWriteMask = 0; continue; }
            blend_multi[i].colorWriteMask = desc->masks_valid ? (VkColorComponentFlags)desc->color_write_masks[i] : 0xfu;
            if (i >= 1u && desc->blend_set && desc->slot_blend[i].set) {
                blend_multi[i].blendEnable = desc->slot_blend[i].enable && LAGFX_POLICY("M2_SRCOVER") ? VK_TRUE : VK_FALSE;
                blend_multi[i].srcColorBlendFactor = (VkBlendFactor)desc->slot_blend[i].src_rgb, blend_multi[i].dstColorBlendFactor = (VkBlendFactor)desc->slot_blend[i].dst_rgb;
                blend_multi[i].colorBlendOp = (VkBlendOp)desc->slot_blend[i].op_rgb;
                blend_multi[i].srcAlphaBlendFactor = (VkBlendFactor)desc->slot_blend[i].src_a, blend_multi[i].dstAlphaBlendFactor = (VkBlendFactor)desc->slot_blend[i].dst_a;
                blend_multi[i].alphaBlendOp = (VkBlendOp)desc->slot_blend[i].op_a;
            }
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .flags = desc->raster_order_access ? 0x1u /* VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT (= the ARM bit) */ : 0u,
        .attachmentCount = ncol,
        .pAttachments = ncol > 1u ? blend_multi : &blend_att,
    };

    /* Shader stages — entry-point names default to "main" but can be
     * overridden via desc. The Stage 65d Option 3 substitute triangle
     * SPVs use "triangle_vertex" / "triangle_fragment" because they're
     * produced by the AIR-to-SPIRV translator, not glslang. */
    const char *v_entry = desc->vertex_entry_point   ? desc->vertex_entry_point   : "main";
    const char *f_entry = desc->fragment_entry_point ? desc->fragment_entry_point : "main";
    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = desc->vertex_shader,
            .pName = v_entry,
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = desc->fragment_shader,
            .pName = f_entry,
        },
    };

    /* Dynamic rendering pNext chain (VK_KHR_dynamic_rendering) */
    const VkFormat color_fmt = desc->no_color ? VK_FORMAT_UNDEFINED
                               : desc->color_format != VK_FORMAT_UNDEFINED
                               ? desc->color_format
                               : VK_FORMAT_B8G8R8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = ncol,
        .pColorAttachmentFormats = ncol > 1u ? color_fmts : &color_fmt,
        .depthAttachmentFormat = desc->dyn_state ? desc->ds_depth_format : VK_FORMAT_UNDEFINED,
        .stencilAttachmentFormat = desc->dyn_state ? desc->ds_stencil_format : VK_FORMAT_UNDEFINED,
        .pNext = desc->diag_gpci_pnext,   /* DIAGNOSTIC hook of the -13 matrix; NULL in production */
    };

    /* Depth/stencil — NULL as in triangle test (no depth) */
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    };

    /* DYNAMIC viewport + scissor: the same pipeline draws into targets of
     * DIFFERENT sizes (the 1920×1080 scanout AND 1280×1024 per-pass surfaces).
     * A pipeline-baked 1920×1080 viewport mis-scaled the 1280×1024 per-pass
     * draws (the "zoom"). The draw site sets the viewport per-draw to the ACTUAL
     * render-target dims (with the Y-flip). Static vp/sc above become ignored
     * placeholders. */
    VkDynamicState dyn_states[16] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
    };
    uint32_t n_dyn = 2;
    if (desc->dyn_state) {   /* Metal keeps these on the encoder, not in the pipeline: the executor sets them before each draw */
        dyn_states[n_dyn++] = VK_DYNAMIC_STATE_CULL_MODE;
        dyn_states[n_dyn++] = VK_DYNAMIC_STATE_FRONT_FACE;
        dyn_states[n_dyn++] = VK_DYNAMIC_STATE_DEPTH_BIAS;
        rsst.depthBiasEnable = VK_TRUE;   /* zero constant / slope = no bias */
        if (desc->ds_depth_format != VK_FORMAT_UNDEFINED || desc->ds_stencil_format != VK_FORMAT_UNDEFINED) {
            dyn_states[n_dyn++] = VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE;
            dyn_states[n_dyn++] = VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE;
            dyn_states[n_dyn++] = VK_DYNAMIC_STATE_DEPTH_COMPARE_OP;
            dyn_states[n_dyn++] = VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE;
            dyn_states[n_dyn++] = VK_DYNAMIC_STATE_STENCIL_OP;
            dyn_states[n_dyn++] = VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK;
            dyn_states[n_dyn++] = VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
            dyn_states[n_dyn++] = VK_DYNAMIC_STATE_STENCIL_REFERENCE;
        }
    }
    VkPipelineDynamicStateCreateInfo dyn = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = n_dyn, .pDynamicStates = dyn_states,
    };

    /* Graphics pipeline create info */
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = desc->diag_render_pass ? NULL : &rendering_info,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vin,
        .pInputAssemblyState = &ia,
        .pViewportState = &vpst,
        .pRasterizationState = &rsst,
        .pMultisampleState = &ms,
        .pDepthStencilState = &ds,
        .pColorBlendState = &cb,
        .pDynamicState = &dyn,
        .layout = desc->layout,
        .renderPass = desc->diag_render_pass ? (VkRenderPass)(uintptr_t)desc->diag_render_pass : VK_NULL_HANDLE,   /* DIAGNOSTIC hook of the -13 matrix; VK_NULL_HANDLE in production */
        .subpass = desc->diag_render_pass ? desc->diag_subpass : 0,
    };

    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult vr = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci,
                                            NULL, &pipe);
    g_last_vr = (int32_t)vr;
    if (vr != VK_SUCCESS && g_quiet) return LAGFX_ERR_BACKEND;
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("lagfx_pipeline_build: vkCreateGraphicsPipelines failed (%d)",
                  (int)vr);
        log_build_failure(device, desc, &vin, ia.topology, color_fmt, vr);
        if (lagfx_pipeline_bundle_dir()) {   /* option, default off: a self-contained bundle of this failure (src/vulkan/pipeline_bundle.h) */
            char local[6144];
            describe_state(local, sizeof local, &gpci, &rendering_info);
            lagfx_pipeline_bundle_write(desc, vr, local, NULL, 0);
        }
        lagfx_pipeline_matrix_on_failure(device, desc, vr);   /* option LAGFX_PIPELINE_MATRIX, default off: a no-op that returns at once */
        return LAGFX_ERR_BACKEND;
    }

    *out_pipeline = pipe;
    return LAGFX_OK;
}

#else  /* !LAGFX_HAVE_VULKAN */

/* Vulkan-disabled stub build: NULL out_pipeline + return error. */
lagfx_status_t lagfx_pipeline_build(VkDevice device,
                                    const lagfx_pipeline_desc_t *desc,
                                    VkPipeline *out_pipeline) {
    (void)device; (void)desc;
    if (out_pipeline) *out_pipeline = NULL;
    return LAGFX_ERR_NOT_FOUND;
}

#endif  /* LAGFX_HAVE_VULKAN */
