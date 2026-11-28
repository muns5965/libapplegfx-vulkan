/*
 * libapplegfx-vulkan — Vulkan init (Phase 1.B)
 * src/vulkan/instance.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Internal header. Not installed.
 *
 * Defines struct lagfx_vk_state and the init/shutdown entry points
 * consumed by src/device.c. See src/vulkan/instance.c for the
 * rationale around optional compilation when vulkan_dep is absent.
 */

#ifndef LIBAPPLEGFX_VULKAN_INSTANCE_H
#define LIBAPPLEGFX_VULKAN_INSTANCE_H

#include "qcom_tile.h"
#include <stddef.h>
#include <stdint.h>
#include "caps.h"

#include "libapplegfx-vulkan.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef LAGFX_HAVE_VULKAN
#  include <vulkan/vulkan.h>
#  include "vulkan/sampler_cache.h"
#endif

/* Internal Vulkan state. Opaque to callers outside src/vulkan/.
 *
 * When the library is built WITHOUT vulkan (LAGFX_HAVE_VULKAN unset),
 * the struct is still defined but fields are minimal — it exists only
 * so the device.c lifecycle can hold the pointer uniformly. */
struct lagfx_vk_state {
    /* LAGFX_VK_CAP_* masks (caps.h): what the device was created with / what the generated SPIR-V may use but the physical device does not report. */
    uint32_t caps_enabled, caps_missing;
    /* Subgroup properties (device creation): size and the VK_EXT_subgroup_size_control range; sg_control = the feature is ENABLED and COMPUTE may carry a required size */
    uint32_t sg_size, sg_min, sg_max, sg_required_stages;
    bool sg_control, sg_full;
    /* VK_QCOM_tile_shading opt-in (LAGFX_QCOM_TILE=1), see qcom_tile.h: what the device was asked for and what came of it. Read-only after init. */
    struct {
        bool flag, injected, enabled;                 /* enabled: the device WAS created with VK_QCOM_tile_properties + VK_QCOM_tile_shading + the features of enable_mask */
        lagfx_qcomtile_reason_t reason; lagfx_qcomtile_fallback_t fallback;
        uint32_t enable_mask; int create_result;      /* create_result: the first vkCreateDevice's VkResult */
        lagfx_qcomtile_caps_t caps;
        char line[768];                               /* the g7-gpu-qcomtile base line (printed once at init, repeated with counters by the diag dump) */
    } qcomtile;
#ifdef LAGFX_HAVE_VULKAN
    VkInstance       instance;
    VkPhysicalDevice phys_device;
    VkDevice         device;
    VkQueue          graphics_queue;
    uint32_t         graphics_queue_family;

    /* Selected physical-device properties, cached at init for logging
     * + later extension decisions. */
    VkPhysicalDeviceProperties phys_props;
    /* What vkCreateDevice was given, as text (pipeline failure bundles, src/vulkan/pipeline_bundle.c): device extensions, the enabled feature bits that matter to
     * the generated SPIR-V / pipeline state, and the driver identification (VkPhysicalDeviceDriverProperties). Filled once at init. */
    char dev_exts[512], dev_features[1024], driver_name[VK_MAX_DRIVER_NAME_SIZE], driver_info[VK_MAX_DRIVER_INFO_SIZE];

    /* Feature-enable flags — reflect what was actually requested +
     * accepted at vkCreateDevice time. Used by later phases to decide
     * whether to take a fast path (shader_object, dynamic_rendering)
     * or fall back. */
    bool have_dynamic_rendering;
    bool have_raster_order;        /* rasterizationOrderColorAttachmentAccess enabled: input-attachment reads inside ONE draw see earlier overlapping fragments in primitive order */
    bool have_local_read;          /* VK_KHR_dynamic_rendering_local_read enabled: colour attachments can be read as input attachments (framebuffer fetch) */
    bool have_local_read_ms;       /* Vulkan 1.4 property dynamicRenderingLocalReadMultisampledAttachments: input-attachment reads of multisampled colour attachments (multisampled framebuffer fetch needs it; false below 1.4) */
    bool have_storage_img_write;   /* shaderStorageImageWriteWithoutFormat + fragmentStoresAndAtomics enabled (OpImageWrite on an unformatted storage image from a fragment shader) */
    bool have_storage_img_rw_nofmt; /* shaderStorageImageReadWithoutFormat AND shaderStorageImageWriteWithoutFormat enabled (unformatted storage images: the tile BGRA8 imageblock) */
    bool have_vertex_stores;       /* vertexPipelineStoresAndAtomics enabled */
    bool have_sampler_anisotropy;  /* enabled device feature samplerAnisotropy */
    bool have_filter_cubic;        /* LAGFX_CUBIC=1 AND the device has VK_EXT_filter_cubic (or the older VK_IMG_filter_cubic): the extension was enabled, VK_FILTER_CUBIC_EXT may be used (filter_cubic.h) */
    bool have_sampler_mirror_clamp; /* enabled device feature samplerMirrorClampToEdge (Vulkan 1.2) */
    bool have_independent_blend;   /* enabled device feature: per-attachment blend state / write masks may differ */
    bool have_sample_rate_shading; /* enabled device feature sampleRateShading: pipelines may set sampleShadingEnable (multisample sample id / per-sample shading) */
    bool have_depth_bias_clamp;    /* enabled device feature depthBiasClamp: vkCmdSetDepthBias may carry a non-zero clamp */
    bool have_synchronization2;
    bool have_timeline_semaphore;
    bool have_descriptor_indexing;
    bool have_shader_object;
    bool have_extended_dynamic_state3;
    bool have_buffer_device_address;  /* M2 host-flattening (PhysicalStorageBuffer) */
    bool device_lost;     /* sticky: a call saw VK_ERROR_DEVICE_LOST. Objects may still be destroyed, but NO new GPU work is recorded or submitted (render_begin and draw_record refuse with LAGFX_ERR_BACKEND) */
    int render_backend;   /* lagfx_render_backend_t, fixed at init (render_pass.h) */
    bool legacy_fetch;    /* legacy backend: input-attachment framebuffer fetch enabled; decided ONCE at init from LAGFX_VK_LEGACY_FETCH ("0" disables), never re-read per call */
    struct lagfx_render_cache *render_cache;   /* render_pass.c: cached VkRenderPass / VkFramebuffer for the legacy backend */
    PFN_vkGetBufferDeviceAddress get_buffer_address;   /* core vkGetBufferDeviceAddress (>= 1.2) or the KHR entry point (1.1 + VK_KHR_buffer_device_address); NULL when BDA is unavailable */

    /* Phase 1.B.2: command pool bound to the graphics queue family.
     * Created at lagfx_vk_init completion, destroyed in
     * lagfx_vk_shutdown BEFORE the VkDevice. Owned by src/vulkan/command.c;
     * this struct just carries the handle so the init/shutdown plumbing
     * in instance.c can reach it. */
    VkCommandPool    cmd_pool;

    VkCommandBuffer  frame_cmdbuf;
    VkFence          frame_fence;
    bool             frame_in_progress;

    VkFence          pending_fence;
    bool             pending_fence_valid;
    VkCommandBuffer  pending_cmdbuf;

    VkPipeline       passthrough_pipeline;
    VkPipelineLayout passthrough_layout;
    VkDescriptorSetLayout passthrough_dsl;

    /* Stage 65d Option 3 — empty pipeline layout (no sets, no push
     * constants) for the substitute triangle pipeline whose shaders
     * declare zero descriptor bindings. vkCreateGraphicsPipelines
     * rejects layout=VK_NULL_HANDLE per the spec; this is the minimal
     * valid layout shared by every triangle-substitute pipeline build. */
    VkPipelineLayout empty_layout;

    VkImage          frame_image;
    VkImageView      frame_image_view;
    VkDeviceMemory   frame_image_mem;
    uint32_t         frame_image_w;
    uint32_t         frame_image_h;
    VkFormat         frame_image_fmt;
    VkImageLayout    frame_image_layout;

    VkBuffer         dummy_vb;
    VkDeviceMemory   dummy_vb_mem;

    VkDescriptorPool      fallback_desc_pool;
    VkDescriptorSet       fallback_desc_set;
    VkBuffer              fallback_ubo;
    VkDeviceMemory        fallback_ubo_mem;

    /* Stage 85b — general per-draw descriptor pool for translated
     * resource-using pipelines. Sized for many sets/bindings across a frame;
     * FREE_DESCRIPTOR_SET so each draw frees its set after submit. Holds
     * uniform + storage buffers + combined image samplers + (M1 c) separate
     * sampled images + samplers. */
    VkDescriptorPool      draw_desc_pool;

    /* M1 (c): shared default sampler (linear/clamp) for texture-sampling
     * translated compositor pipelines. */
    VkSampler             default_sampler;   /* owned by sampler_cache */
    /* Samplers by state (constexpr samplers, the default above). */
    lagfx_sampler_cache_t sampler_cache;
    /* GOAL-M2x: unnormalizedCoordinates sampler for Metal coord::pixel
     * (CA glyph-atlas composites; pixel-space texcoords like (70,22)). */
    VkSampler             unnorm_sampler;

    /* M3/perf (env LAGFX_PERF): frame-time instrumentation accumulators,
     * reset per frame at readback. Feeds the lavapipe-vs-GPU decision. */
    uint64_t              perf_last_frame_ns;
    uint64_t              perf_frame_draws;
    uint64_t              perf_frame_draw_ns;

    /* KICKOFF-shading-throughput: fence-wait duration of the most recent
     * draw submit (set unconditionally in draw_record_and_submit, including
     * the timeout path). draw_emit reads it after each translated draw to
     * decide whether to flush the RAM bind-window snapshot to disk
     * (LAGFX_DUMP_SLOW_BINDS). slow_bind_dumps caps total dump sets/boot. */
    uint32_t              last_draw_wait_ms;
    uint32_t              slow_bind_dumps;

    VkPipeline       cursor_pipeline;
    VkPipelineLayout cursor_layout;
    VkDescriptorSetLayout cursor_dsl;
    VkDescriptorPool      cursor_desc_pool;
    VkDescriptorSet       cursor_desc_set;

    /* Layer compositor (MAX-blend overlay of per-pass surfaces onto rt). */
    VkPipeline            composite_pipeline;
    VkPipelineLayout      composite_layout;
    VkDescriptorSetLayout composite_dsl;
    VkDescriptorPool      composite_pool;
    VkDescriptorSet       composite_set;
    VkSampler             composite_sampler;
    bool                  composite_ready;

    VkImage          cursor_glyph_image;
    VkImageView      cursor_glyph_view;
    VkDeviceMemory   cursor_glyph_mem;
    bool             cursor_glyph_coherent;
    VkImageLayout    cursor_glyph_layout;
    bool             cursor_sync_failed;
    VkSampler        cursor_sampler;
    VkBuffer         cursor_ubo;
    VkDeviceMemory   cursor_ubo_mem;
    bool             cursor_glyph_valid;
    uint32_t         cursor_glyph_w;
    uint32_t         cursor_glyph_h;

    /* Per-device push-descriptor dispatch cache —
     * vkCmdPushDescriptorSetKHR resolved via vkGetDeviceProcAddr.
     * Lives on the vk state (not in a file-scope static) so multiple
     * VkDevices can each carry their own pfn. Populated lazily by
     * resolve_push_desc() in src/translate/render_encoder.c. */
    PFN_vkCmdPushDescriptorSetKHR push_desc_pfn;

    /* Vulkan validation layer debug messenger (optional, gated by
     * LAGFX_VK_VALIDATION=1 at runtime). Destroyed before vkDestroyInstance. */
    VkDebugUtilsMessengerEXT debug_messenger;
    PFN_vkCreateDebugUtilsMessengerEXT create_debug_messenger_fn;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug_messenger_fn;
#else
    /* Pad so sizeof(struct) > 0 on no-vulkan builds. */
    int _placeholder;
#endif
    /* Keep this flag in both paths so callers can cheaply ask
     * "did we actually init Vulkan?" without #ifdefs. */
    bool initialized;
};

/* Create VkInstance + select VkPhysicalDevice + create VkDevice +
 * retrieve VkQueue. Stores the whole lot in *out.
 *
 * On success returns LAGFX_OK. On failure returns LAGFX_ERR_VULKAN_INIT
 * (or LAGFX_ERR_OUT_OF_MEMORY) and *out is left NULL. The desc is used
 * for:
 *   - desc->shell_vulkan_instance: if non-NULL, reuse the caller's
 *     VkInstance rather than creating our own (future work — currently
 *     logged and ignored in Phase 1.B).
 *
 * The caller is responsible for having already applied LP_NUM_THREADS
 * via setenv before calling this function — Mesa reads that var once
 * at ICD init triggered by vkCreateInstance. */
lagfx_status_t lagfx_vk_init(struct lagfx_vk_state **out,
                             const lagfx_device_descriptor_t *desc);

/* Tear down: pipeline resources + command pool + device + instance.
 * Safe on NULL. */
void lagfx_vk_shutdown(struct lagfx_vk_state *state);

#ifdef LAGFX_HAVE_VULKAN
/* Stage 85b — create a host-visible STORAGE buffer and upload `data` (NULL ⇒
 * zero-init). Caller destroys *out_buf + frees *out_mem after the draw submits. */
lagfx_status_t lagfx_vk_make_host_storage_buffer(struct lagfx_vk_state *vk,
                                                 const void *data,
                                                 VkDeviceSize size,
                                                 VkBuffer *out_buf,
                                                 VkDeviceMemory *out_mem);

/* M2: storage buffer of alloc_size bytes, first data_len from `data`, rest
 * zero-padded — so a buffer can be sized to the guest's DECLARED size (dynamic
 * index stays in bounds) with only data_len bytes of real content. */
lagfx_status_t lagfx_vk_make_host_storage_buffer_padded(struct lagfx_vk_state *vk,
                                                        const void *data,
                                                        VkDeviceSize data_len,
                                                        VkDeviceSize alloc_size,
                                                        VkBuffer *out_buf,
                                                        VkDeviceMemory *out_mem);

/* M2 host-flatten step 2: host-visible storage buffer that also exposes a
 * VkDeviceAddress (for PhysicalStorageBuffer arg-buffer flattening). Requires
 * vk->have_buffer_device_address. */
lagfx_status_t lagfx_vk_make_device_address_buffer(struct lagfx_vk_state *vk,
                                                   const void *data,
                                                   VkDeviceSize size,
                                                   VkBuffer *out_buf,
                                                   VkDeviceMemory *out_mem,
                                                   VkDeviceAddress *out_addr);
#endif

/* The g7-gpu-caps diagnostic line for this device (also printed once by lagfx_vk_init); returns buf. */
/* Metal's simdgroup is 32 lanes. True when a compute pipeline can run with exactly 32 lanes per subgroup: the device's native size is 32, or
 * VK_EXT_subgroup_size_control is enabled, COMPUTE accepts a required size and 32 lies in [min, max]. Kernels that map simd lanes to pixels (simd_shuffle on
 * a tile) are only built when this holds. Fallback documented in arm64_tile.c: otherwise the tile dispatch is skipped and counted (tile_no_sg32). */
bool lagfx_vk_can_pin_subgroup32(const struct lagfx_vk_state *vk);
const char *lagfx_vk_caps_line(const struct lagfx_vk_state *vk, char *buf, size_t n);
/* TEST SEAM ('feature missing'): marks caps as missing on an existing device (the Vulkan device itself is not changed; only the gate sees it).
 * At init the same is possible through LAGFX_VK_CAPS_OFF=int64,int8,... which also keeps those features OFF at vkCreateDevice. */
void lagfx_vk_caps_inject_missing(struct lagfx_vk_state *vk, uint32_t mask);

#endif /* LIBAPPLEGFX_VULKAN_INSTANCE_H */
