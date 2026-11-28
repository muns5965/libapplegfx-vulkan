/*
 * libapplegfx-vulkan — Vulkan instance + device + queue init (Phase 1.B)
 * src/vulkan/instance.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * === Scope =====================================================
 *
 * Minimum-viable Vulkan init path: VkInstance, VkPhysicalDevice
 * selection (preferring lavapipe — VK_PHYSICAL_DEVICE_TYPE_CPU),
 * VkDevice with the feature set libapplegfx-vulkan relies on
 * (dynamicRendering, synchronization2, timelineSemaphore, descriptor
 * indexing), and retrieval of one graphics+compute+transfer queue.
 *
 * No rendering, no swapchain, no command buffers — that lands in
 * later Phase 1.B increments (command pool setup) and Phase 2
 * (actual render encoder plumbing).
 *
 * === Graceful degradation ======================================
 *
 * When the host lacks a Vulkan SDK at build time (meson's
 * dependency('vulkan', required:false) returns .found() == false),
 * this file compiles WITHOUT the LAGFX_HAVE_VULKAN define. In that
 * mode lagfx_vk_init is a no-op that succeeds and returns a tiny
 * state struct with initialized=false. This keeps the rest of the
 * library — lifecycle, protocol decode, MMIO dispatch — fully
 * functional so tests that don't actually render still pass.
 *
 * On Darwin dev hosts without a Vulkan loader (the typical case
 * when working on this codebase from a Mac) we take the no-Vulkan
 * path even when <vulkan/vulkan.h> is available via a third-party
 * SDK — meson's dependency() is the single source of truth.
 *
 * === LP_NUM_THREADS ordering ===================================
 *
 * Mesa lavapipe reads LP_NUM_THREADS at ICD init, which is
 * triggered by the first Vulkan call in the process (vkCreateInstance).
 * device.c calls lagfx_apply_thread_count_env BEFORE invoking
 * lagfx_vk_init, so by the time we hit vkCreateInstance below the
 * env var is in place. See the note in device.c near the setenv.
 *
 * === Vulkan validation layers ===================================
 *
 * Validation layers are disabled by default (no perf cost in steady-state).
 * Set LAGFX_VK_VALIDATION=1 environment variable to opt-in for debugging
 * Stage 80+ issues. Layers enabled: VK_LAYER_KHRONOS_validation.
 * Debug messenger installed with VERBOSE+WARNING+ERROR severity.
 */

#include "instance.h"
#include "devinfo.h"
#include "pipeline_matrix.h"
#include "command.h"
#include "pipeline.h"
#include "composite.h"
#include "render_pass.h"
#include "msaa_host.h"
#include "filter_cubic.h"
#include "common/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef LAGFX_HAVE_VULKAN

/* --- Helpers ---------------------------------------------------- */

/* Return true if `name` is in the extension-property array. */
static bool has_ext(const VkExtensionProperties *avail, uint32_t count,
                    const char *name) {
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(avail[i].extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

/* Pretty-print physical device type for logging. */
static const char *phys_type_str(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_OTHER:          return "other";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated-GPU";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete-GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual-GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU (lavapipe-like)";
    default:                                     return "?";
    }
}

/* Pick a queue family with graphics+compute+transfer. Lavapipe
 * exposes one family with all three; hardware drivers typically
 * have at least one such family too. Returns UINT32_MAX on
 * miss. */
static uint32_t pick_queue_family(VkPhysicalDevice phys) {
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, NULL);
    if (n == 0) {
        return UINT32_MAX;
    }
    VkQueueFamilyProperties *props = calloc(n, sizeof(*props));
    if (!props) {
        return UINT32_MAX;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, props);

    const VkQueueFlags want = VK_QUEUE_GRAPHICS_BIT
                            | VK_QUEUE_COMPUTE_BIT
                            | VK_QUEUE_TRANSFER_BIT;
    uint32_t found = UINT32_MAX;
    for (uint32_t i = 0; i < n; ++i) {
        if ((props[i].queueFlags & want) == want) {
            found = i;
            break;
        }
    }
    /* Fallback: any graphics family. */
    if (found == UINT32_MAX) {
        for (uint32_t i = 0; i < n; ++i) {
            if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                found = i;
                break;
            }
        }
    }
    free(props);
    return found;
}

/* Debug message callback for validation layers — prints VUIDs and warnings. */
/* Number of non-verbose validation messages seen (tests assert it stays 0). */
uint64_t lagfx_vk_validation_msg_count;
static int lagfx_env_is_one(const char *n) { const char *v = getenv(n); return v && v[0] == '1' && !v[1]; }   /* only the string "1" turns an opt-in on */
/* Test hook: called with the text of every counted message (not for the filtered ones below). tests/arm64-render-compat-unit.c keeps a per-test list of them and compares it with a baseline file. */
void (*lagfx_vk_validation_hook)(const char *msg);
/* Of those, the messages about a shader / descriptor-write DESCRIPTOR TYPE that disagrees with the pipeline layout or the layout binding (VUID-VkGraphicsPipelineCreateInfo-layout-07988 / -07990 /
 * ..., VUID-VkWriteDescriptorSet-descriptorType-*): the class a set+binding holding two descriptor types ends in. Separate so a test can stay strict about it while an older layer reports other things. */
uint64_t lagfx_vk_validation_layout_msg_count;

static VkBool32 VKAPI_PTR lagfx_debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
    void *pUserData) {
    (void)pUserData;
    (void)messageType;
    if (messageSeverity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
        return VK_FALSE;
    }
    /* A validation layer older than VK_KHR_dynamic_rendering_local_read (< header 1.3.287) does not know its struct type / layout / extension: that is the layer, not us. */
    if (strstr(pCallbackData->pMessage, "unknown VkStructureType (1000232000)") ||
        strstr(pCallbackData->pMessage, "VkPhysicalDeviceProperties2-pNext-pNext") && strstr(pCallbackData->pMessage, "unknown VkStructureType (56)") ||   /* VkPhysicalDeviceVulkan14Properties queried below with a layer older than header 1.4.x */
        (strstr(pCallbackData->pMessage, "VK_KHR_dynamic_rendering_local_read is not supported by this layer")) ||
        strstr(pCallbackData->pMessage, "(1000232000) does not fall within the begin..end range of the core VkImageLayout")) return VK_FALSE;
    lagfx_vk_validation_msg_count++;
    if (lagfx_vk_validation_hook) lagfx_vk_validation_hook(pCallbackData->pMessage);
    if (strstr(pCallbackData->pMessage, "VUID-VkGraphicsPipelineCreateInfo-layout-") || strstr(pCallbackData->pMessage, "uses descriptor slot") || strstr(pCallbackData->pMessage, "VUID-VkWriteDescriptorSet-descriptorType-"))
        lagfx_vk_validation_layout_msg_count++;
    if (getenv("LAGFX_VK_VALIDATION_PRINT")) fprintf(stderr, "validation: %s\n", pCallbackData->pMessage);
    LAGFX_WARN("validation: %s", pCallbackData->pMessage);  /* stderr is dropped in-container */
    return VK_FALSE;
}

/* Choose physical device: prefer CPU-type (lavapipe) for our use
 * case — we're targeting headless-CPU rendering. Fall back to the
 * first device with a graphics queue family. */
static VkPhysicalDevice pick_phys_device(VkInstance inst,
                                          uint32_t *qfam_out) {
    uint32_t n = 0;
    if (vkEnumeratePhysicalDevices(inst, &n, NULL) != VK_SUCCESS || n == 0) {
        LAGFX_ERR("vk_init: no Vulkan physical devices enumerable");
        return VK_NULL_HANDLE;
    }
    VkPhysicalDevice *devs = calloc(n, sizeof(*devs));
    if (!devs) {
        return VK_NULL_HANDLE;
    }
    vkEnumeratePhysicalDevices(inst, &n, devs);

    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    uint32_t         chosen_qf = UINT32_MAX;

    /* Pass 1: CPU-type. */
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
            uint32_t qf = pick_queue_family(devs[i]);
            if (qf != UINT32_MAX) {
                chosen = devs[i];
                chosen_qf = qf;
                break;
            }
        }
    }

    /* Pass 2: any device with a graphics queue. */
    if (chosen == VK_NULL_HANDLE) {
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t qf = pick_queue_family(devs[i]);
            if (qf != UINT32_MAX) {
                chosen = devs[i];
                chosen_qf = qf;
                break;
            }
        }
    }

    free(devs);
    *qfam_out = chosen_qf;
    return chosen;
}

/* --- Public entry points --------------------------------------- */

lagfx_status_t lagfx_vk_init(struct lagfx_vk_state **out,
                              const lagfx_device_descriptor_t *desc) {
    if (!out) {
        return LAGFX_ERR_INVALID_ARG;
    }
    *out = NULL;

    if (desc && desc->shell_vulkan_instance) {
        /* Future: share an externally-provided VkInstance. Today we
         * just log and proceed to create our own. */
        LAGFX_LOG("vk_init: shell_vulkan_instance provided but reuse "
                  "not yet implemented; ignoring");
    }

    struct lagfx_vk_state *s = calloc(1, sizeof(*s));
    if (!s) {
        return LAGFX_ERR_OUT_OF_MEMORY;
    }

    /* === Validation layers (optional, gated by env var) ========== */
    const char *validation_env = getenv("LAGFX_VK_VALIDATION");
    bool enable_validation = validation_env && strcmp(validation_env, "1") == 0;

    if (enable_validation) {
        LAGFX_LOG("vk_init: enabling Vulkan validation layers (LAGFX_VK_VALIDATION=1)");
    }

    /* === Instance =================================================
     * Request portability-enumeration + debug-utils opportunistically;
     * omit any that the loader doesn't advertise rather than failing.
     * ------------------------------------------------------------- */
    const char *wanted_inst_exts[] = {
        "VK_KHR_portability_enumeration",
        "VK_EXT_debug_utils",
    };
    const size_t wanted_inst_n = sizeof(wanted_inst_exts)
                               / sizeof(wanted_inst_exts[0]);

    uint32_t avail_n = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &avail_n, NULL);
    VkExtensionProperties *avail = NULL;
    if (avail_n > 0) {
        avail = calloc(avail_n, sizeof(*avail));
        if (!avail) {
            free(s);
            return LAGFX_ERR_OUT_OF_MEMORY;
        }
        vkEnumerateInstanceExtensionProperties(NULL, &avail_n, avail);
    }

    const char *use_inst_exts[2] = {0};
    uint32_t    use_inst_n       = 0;
    VkInstanceCreateFlags inst_flags = 0;
    for (size_t i = 0; i < wanted_inst_n; ++i) {
        if (has_ext(avail, avail_n, wanted_inst_exts[i])) {
            use_inst_exts[use_inst_n++] = wanted_inst_exts[i];
            if (strcmp(wanted_inst_exts[i],
                       "VK_KHR_portability_enumeration") == 0) {
                inst_flags |= 0x00000001; /* VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR */
            }
        } else {
            LAGFX_LOG("vk_init: instance ext '%s' not present — skipping",
                      wanted_inst_exts[i]);
        }
    }
    free(avail);

    /* === Validation layers (optional) ============================ */
    const char *validation_layers[] = {"VK_LAYER_KHRONOS_validation"};
    const char *all_layers[2] = {0};
    uint32_t layer_count = 0;
    
    if (enable_validation) {
        /* Check if validation layer is available */
        uint32_t check_n = 0;
        vkEnumerateInstanceLayerProperties(&check_n, NULL);
        VkLayerProperties *layers = NULL;
        bool found = false;
        
        if (check_n > 0) {
            layers = calloc(check_n, sizeof(*layers));
            if (layers) {
                vkEnumerateInstanceLayerProperties(&check_n, layers);
                for (uint32_t i = 0; i < check_n; ++i) {
                    if (strcmp(layers[i].layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                        found = true;
                        break;
                    }
                }
                free(layers);
            }
        }
        
        if (!found) {
            LAGFX_WARN("vk_init: LAGFX_VK_VALIDATION=1 but VK_LAYER_KHRONOS_validation not available — skipping");
            enable_validation = false;
        } else {
            all_layers[0] = validation_layers[0];
            layer_count = 1;
            LAGFX_LOG("vk_init: validation layer 'VK_LAYER_KHRONOS_validation' enabled");
        }
    }

    VkApplicationInfo app = {
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName   = "libapplegfx-vulkan",
        .applicationVersion = 0,
        .pEngineName        = "libapplegfx-vulkan",
        .engineVersion      = 0,
        /* 1.3 is safer than 1.4 for max ICD compatibility; our host
         * runs 1.4 but lots of deploy targets (older Mesa on older
         * distros) cap at 1.3. We don't need 1.4 features. */
        .apiVersion         = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo ici = {
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .flags                   = inst_flags,
        .pApplicationInfo        = &app,
        .enabledLayerCount       = layer_count,
        .ppEnabledLayerNames     = layer_count > 0 ? all_layers : NULL,
        .enabledExtensionCount   = use_inst_n,
        .ppEnabledExtensionNames = use_inst_n ? use_inst_exts : NULL,
    };
    VkResult vr = vkCreateInstance(&ici, NULL, &s->instance);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("vk_init: vkCreateInstance failed (VkResult=%d)", (int)vr);
        free(s);
        return LAGFX_ERR_VULKAN_INIT;
    }

    /* === Debug messenger (optional, gated by env var) ========== */
    if (enable_validation) {
        s->create_debug_messenger_fn = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(s->instance, "vkCreateDebugUtilsMessengerEXT");
        s->destroy_debug_messenger_fn = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(s->instance, "vkDestroyDebugUtilsMessengerEXT");

        if (s->create_debug_messenger_fn) {
            VkDebugUtilsMessengerCreateInfoEXT mc = {
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                                 | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                             | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                .pfnUserCallback = lagfx_debug_callback,
            };

            vr = s->create_debug_messenger_fn(s->instance, &mc, NULL, &s->debug_messenger);
            if (vr != VK_SUCCESS) {
                LAGFX_WARN("vk_init: vkCreateDebugUtilsMessengerEXT failed (%d) — validation messages will not be captured", (int)vr);
                s->debug_messenger = VK_NULL_HANDLE;
            } else {
                LAGFX_LOG("vk_init: debug messenger installed");
            }
        } else {
            LAGFX_WARN("vk_init: vkCreateDebugUtilsMessengerEXT not found — validation messages will not be captured");
        }
    }

    /* === Physical device ========================================= */
    s->phys_device = pick_phys_device(s->instance,
                                      &s->graphics_queue_family);
    if (s->phys_device == VK_NULL_HANDLE) {
        LAGFX_ERR("vk_init: no suitable physical device");
        vkDestroyInstance(s->instance, NULL);
        free(s);
        return LAGFX_ERR_VULKAN_INIT;
    }
    vkGetPhysicalDeviceProperties(s->phys_device, &s->phys_props);
    {   /* TEST KNOB (host tests only, never set in production): LAGFX_VK_TEST_API_MINOR=1 reports Vulkan 1.1 from here on, so the pre-1.2 init paths run (extension enabling, KHR entry points) on a host
         * driver that is really newer. It approximates, it does not replace, a real 1.1 driver. */
        const char *cap = getenv("LAGFX_VK_TEST_API_MINOR");
        if (cap && cap[0] >= '0' && cap[0] <= '9' && VK_VERSION_MINOR(s->phys_props.apiVersion) > (uint32_t)(cap[0] - '0') && VK_VERSION_MAJOR(s->phys_props.apiVersion) == 1u)
            s->phys_props.apiVersion = VK_MAKE_API_VERSION(0, 1, (uint32_t)(cap[0] - '0'), 128);
    }
    LAGFX_LOG("vk_init: phys device '%s' type=%s api=%u.%u.%u qf=%u",
              s->phys_props.deviceName,
              phys_type_str(s->phys_props.deviceType),
              VK_VERSION_MAJOR(s->phys_props.apiVersion),
              VK_VERSION_MINOR(s->phys_props.apiVersion),
              VK_VERSION_PATCH(s->phys_props.apiVersion),
              s->graphics_queue_family);

    /* === Device extensions ======================================= */
    uint32_t dext_n = 0;
    vkEnumerateDeviceExtensionProperties(s->phys_device, NULL, &dext_n, NULL);
    VkExtensionProperties *dext = NULL;
    if (dext_n > 0) {
        dext = calloc(dext_n, sizeof(*dext));
        if (!dext) {
            vkDestroyInstance(s->instance, NULL);
            free(s);
            return LAGFX_ERR_OUT_OF_MEMORY;
        }
        vkEnumerateDeviceExtensionProperties(s->phys_device, NULL,
                                              &dext_n, dext);
    }

    /* Required (core-1.3 promoted but still enumerated as extensions
     * on some ICDs; ask explicitly for widest compatibility). */
    const char *wanted_dev_exts[24];
    uint32_t    wanted_dev_n = 0;
    bool want_dyn_rendering =
        has_ext(dext, dext_n, "VK_KHR_dynamic_rendering");
    bool want_shader_object =
        has_ext(dext, dext_n, "VK_EXT_shader_object");
    bool want_eds3 =
        has_ext(dext, dext_n, "VK_EXT_extended_dynamic_state3");

    if (want_dyn_rendering) {
        wanted_dev_exts[wanted_dev_n++] = "VK_KHR_dynamic_rendering";
    } else {
        LAGFX_LOG("vk_init: VK_KHR_dynamic_rendering missing — relying on "
                  "Vulkan 1.3 core promotion");
    }
    if (want_shader_object) {
        wanted_dev_exts[wanted_dev_n++] = "VK_EXT_shader_object";
    } else {
        LAGFX_LOG("vk_init: VK_EXT_shader_object missing — will use "
                  "pipeline fallback in later phases");
    }
    if (want_eds3) {
        wanted_dev_exts[wanted_dev_n++] = "VK_EXT_extended_dynamic_state3";
    } else {
        LAGFX_LOG("vk_init: VK_EXT_extended_dynamic_state3 missing — "
                  "later phases may need fallbacks");
    }
    /* On a device below Vulkan 1.2 the features of these extensions (shaderFloat16/Int8, 8/16-bit storage, variable pointers) were not yet core: using them through the
     * Vulkan11/12 feature structs without enabling the extension is invalid usage. Enable each one the device offers (>= 1.2 devices keep their exact previous set). */
    if (s->phys_props.apiVersion < VK_API_VERSION_1_2) {
        static const char *const khr11[] = { "VK_KHR_shader_float16_int8", "VK_KHR_8bit_storage", "VK_KHR_16bit_storage", "VK_KHR_variable_pointers", "VK_KHR_storage_buffer_storage_class" };
        for (size_t i = 0; i < sizeof khr11 / sizeof khr11[0]; i++)
            if (has_ext(dext, dext_n, khr11[i]) && wanted_dev_n < 20) wanted_dev_exts[wanted_dev_n++] = khr11[i];
    }
    /* Depth / stencil resolve below Vulkan 1.2 needs both extensions enabled (their dependencies, multiview and maintenance2, are core 1.1). Properties only: no feature struct. */
    bool dsres_ext_on = false;
    if (s->phys_props.apiVersion < VK_API_VERSION_1_2 && has_ext(dext, dext_n, "VK_KHR_depth_stencil_resolve") && has_ext(dext, dext_n, "VK_KHR_create_renderpass2")
        && wanted_dev_n + 2u <= 20u && !getenv("LAGFX_VK_NO_DS_RESOLVE")) {
        wanted_dev_exts[wanted_dev_n++] = "VK_KHR_create_renderpass2";
        wanted_dev_exts[wanted_dev_n++] = "VK_KHR_depth_stencil_resolve";
        dsres_ext_on = true;
    }
    bool have_spirv14_ext = has_ext(dext, dext_n, "VK_KHR_spirv_1_4") && has_ext(dext, dext_n, "VK_KHR_shader_float_controls");
    const bool ext_lr = has_ext(dext, dext_n, "VK_KHR_dynamic_rendering_local_read");
    const bool ext_eds1 = has_ext(dext, dext_n, "VK_EXT_extended_dynamic_state");   /* evaluated here: dext is freed below */
    const bool ext_sgc = has_ext(dext, dext_n, "VK_EXT_subgroup_size_control");   /* pre-1.3 form of subgroup size control (same feature/property layout as the 1.3 core structs) */
    const bool ext_demote = has_ext(dext, dext_n, "VK_EXT_shader_demote_to_helper_invocation");   /* the pre-1.3 form of DemoteToHelperInvocation (same feature, same SPIR-V capability; the SPIR-V side declares SPV_EXT_demote_to_helper_invocation below 1.6) */
    /* Opt-in cubic filtering (filter_cubic.h): only requested with LAGFX_CUBIC=1; the extension has no feature struct. The EXT form is preferred, the IMG alias is used on a device that lacks it. */
    const char *cubic_ext = !lagfx_cubic_optin() ? NULL : has_ext(dext, dext_n, "VK_EXT_filter_cubic") ? "VK_EXT_filter_cubic" : has_ext(dext, dext_n, "VK_IMG_filter_cubic") ? "VK_IMG_filter_cubic" : NULL;
    typedef struct { VkStructureType sType; void *pNext; VkBool32 dynamicRenderingLocalRead; } local_read_feat_t;   /* older headers lack the type */
    const bool ext_ro_ext = has_ext(dext, dext_n, "VK_EXT_rasterization_order_attachment_access");
    const bool ext_ro_arm = has_ext(dext, dext_n, "VK_ARM_rasterization_order_attachment_access");
    const bool ext_af = has_ext(dext, dext_n, "VK_EXT_shader_atomic_float");   /* float atomics: queried below, enabled per feature only when reported (caps.h LAGFX_VK_CAP_ATOMIC_F32_*) */
    const bool ext_qct_ts = has_ext(dext, dext_n, "VK_QCOM_tile_shading"), ext_qct_tp = has_ext(dext, dext_n, "VK_QCOM_tile_properties"), ext_qct_heap = has_ext(dext, dext_n, "VK_QCOM_tile_memory_heap");
    const bool ext_f16i8 = has_ext(dext, dext_n, "VK_KHR_shader_float16_int8"), ext_8bit = has_ext(dext, dext_n, "VK_KHR_8bit_storage");
    const bool ext_bda_khr = has_ext(dext, dext_n, "VK_KHR_buffer_device_address"), ext_sync2_khr = has_ext(dext, dext_n, "VK_KHR_synchronization2");   /* for the g7-probe line */
    free(dext);

    /* === Device features =========================================
     *
     * Audit fix (#18): query the ICD's actual feature support BEFORE
     * requesting features in vkCreateDevice. Today lavapipe always
     * advertises 1.3 with dynamicRendering=VK_TRUE so the previous
     * unconditional request worked; on a Vulkan 1.2 ICD or any
     * adapter that ships these as optional, vkCreateDevice fails
     * with a cryptic VK_ERROR_FEATURE_NOT_PRESENT. Probe + downgrade.
     *
     * Promoted-to-core features (1.3): dynamicRendering, synchronization2.
     * If the ICD reports these as supported, request them via the
     * VkPhysicalDeviceVulkan13Features chain. If not (Vulkan 1.2 ICD),
     * fall back to the VK_KHR_dynamic_rendering / KHR_synchronization2
     * extension chain — same shape on the wire, different sType.
     * ------------------------------------------------------------- */
    VkPhysicalDeviceVulkan13Features probe13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
    };
    /* variablePointersStorageBuffer (core 1.1): the AIR→SPIR-V translator
     * passes StorageBuffer interior pointers as helper-function call args
     * (`constant float4*` colour-matrix params) under OpCapability
     * VariablePointersStorageBuffer — the device must enable the feature
     * or pipeline builds reject those modules. lavapipe advertises it. */
    VkPhysicalDeviceVulkan11Features probe11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext = &probe13,
    };
    VkPhysicalDeviceVulkan12Features probe12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &probe11,
    };
    VkPhysicalDeviceShaderObjectFeaturesEXT probe_so = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT,
        .pNext = &probe12,
    };
    VkPhysicalDeviceFeatures2 probe2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = want_shader_object ? (void *)&probe_so : (void *)&probe12,
    };
    vkGetPhysicalDeviceFeatures2(s->phys_device, &probe2);

    /* Below Vulkan 1.2 the unified Vulkan11/12/13Features structs do not exist for the driver (it may ignore them or leave them at 0 while the
     * extension features are real). There, ignore their values and query the individual per-feature structs instead (core 1.1 / KHR), then
     * fold the answers into probe11/probe12 so everything downstream (caps, enable values, diagnostics) keeps one source. Features whose
     * individual path is not implemented here (dynamic rendering, synchronization2, timeline semaphore, descriptor indexing, subgroup size
     * control, demote) stay 0 = not requested. Devices >= 1.2 never enter this block. */
    /* LAGFX_VK_FORCE_LEGACY_FEATURES=1 (test seam): take this < 1.2 path on a newer device, so the individual-struct chain can be exercised on lavapipe / the S26 without a 1.1 driver. */
    const char *force_legacy_env = getenv("LAGFX_VK_FORCE_LEGACY_FEATURES");
    const bool legacy_feat = s->phys_props.apiVersion < VK_API_VERSION_1_2 || (force_legacy_env && !strcmp(force_legacy_env, "1"));
    VkPhysicalDeviceShaderFloat16Int8Features   lq_f16i8 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES };
    VkPhysicalDevice8BitStorageFeatures         lq_8bit  = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES };
    VkPhysicalDevice16BitStorageFeatures        lq_16bit = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES };
    VkPhysicalDeviceVariablePointersFeatures    lq_vptr  = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES };
    VkPhysicalDeviceBufferDeviceAddressFeatures lq_bda   = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };
    if (legacy_feat) {
        const bool lq_has_f16i8 = ext_f16i8, lq_has_8bit = ext_8bit, lq_has_bda = ext_bda_khr;
        void *chain = NULL;
        if (lq_has_f16i8) { lq_f16i8.pNext = chain; chain = &lq_f16i8; }
        if (lq_has_8bit)  { lq_8bit.pNext  = chain; chain = &lq_8bit; }
        lq_16bit.pNext = chain; chain = &lq_16bit;
        lq_vptr.pNext = chain; chain = &lq_vptr;
        if (lq_has_bda)   { lq_bda.pNext   = chain; chain = &lq_bda; }
        VkPhysicalDeviceFeatures2 lq = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = chain };
        vkGetPhysicalDeviceFeatures2(s->phys_device, &lq);
        { void *pn = probe13.pNext; probe13 = (VkPhysicalDeviceVulkan13Features){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = pn }; }
        { void *pn = probe12.pNext; probe12 = (VkPhysicalDeviceVulkan12Features){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = pn }; }
        { void *pn = probe11.pNext; probe11 = (VkPhysicalDeviceVulkan11Features){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = pn }; }
        probe12.shaderFloat16 = lq_has_f16i8 ? lq_f16i8.shaderFloat16 : VK_FALSE;
        probe12.shaderInt8 = lq_has_f16i8 ? lq_f16i8.shaderInt8 : VK_FALSE;
        probe12.storageBuffer8BitAccess = lq_has_8bit ? lq_8bit.storageBuffer8BitAccess : VK_FALSE;
        probe12.uniformAndStorageBuffer8BitAccess = lq_has_8bit ? lq_8bit.uniformAndStorageBuffer8BitAccess : VK_FALSE;
        probe12.storagePushConstant8 = lq_has_8bit ? lq_8bit.storagePushConstant8 : VK_FALSE;
        probe12.bufferDeviceAddress = lq_has_bda ? lq_bda.bufferDeviceAddress : VK_FALSE;
        probe11.storageBuffer16BitAccess = lq_16bit.storageBuffer16BitAccess;
        probe11.uniformAndStorageBuffer16BitAccess = lq_16bit.uniformAndStorageBuffer16BitAccess;
        probe11.storagePushConstant16 = lq_16bit.storagePushConstant16;
        probe11.storageInputOutput16 = lq_16bit.storageInputOutput16;
        probe11.variablePointers = lq_vptr.variablePointers;
        probe11.variablePointersStorageBuffer = lq_vptr.variablePointersStorageBuffer;
    }

    /* Re-check ICD-advertised support. The "we'd like" set is fixed;
     * if the ICD says no, we still try to create the device with the
     * remaining features (vkCreateDevice will report which one
     * actually fails). */
    bool have_dyn_rendering_feature   = (probe13.dynamicRendering   == VK_TRUE);
    bool have_synchronization2        = (probe13.synchronization2   == VK_TRUE);
    bool have_timeline_semaphore_feat = (probe12.timelineSemaphore  == VK_TRUE);
    bool have_descriptor_indexing     = (probe12.descriptorIndexing == VK_TRUE);
    /* M2 host-flattening: bufferDeviceAddress lets the host hand the shader
     * real device addresses for the resources a Metal argument buffer points
     * to (via SPIR-V PhysicalStorageBuffer), so arg-buffer [[id(n)]] pointer
     * members can be dereferenced. Core 1.2; lavapipe advertises it. */
    /* vkGetBufferDeviceAddress is a Vulkan 1.2 core entry point; on a 1.1 device
     * (VK_KHR_buffer_device_address would have to be enabled and the KHR entry
     * point used) it is not resolvable, so only report BDA on >= 1.2 devices. */
    bool have_buffer_device_address   = (probe12.bufferDeviceAddress == VK_TRUE) &&
        (s->phys_props.apiVersion >= VK_API_VERSION_1_2 || ext_bda_khr);   /* < 1.2: only through VK_KHR_buffer_device_address (feature queried on its own above) */
    bool have_varptr_storage_buffer   =
        (probe11.variablePointersStorageBuffer == VK_TRUE);

    if (!have_dyn_rendering_feature) {
        LAGFX_WARN("vk_init: dynamicRendering feature not advertised by ICD; "
                   "draw paths that depend on it will fail. Vulkan 1.3 core "
                   "promotion missing — falling back to KHR_dynamic_rendering "
                   "extension if present (want_dyn_rendering=%d).",
                   (int)want_dyn_rendering);
    }
    if (!have_synchronization2) {
        LAGFX_WARN("vk_init: synchronization2 feature not advertised; "
                   "fence/timeline paths may need pre-1.3 fallback.");
    }
    if (!have_timeline_semaphore_feat) {
        LAGFX_WARN("vk_init: timelineSemaphore feature not advertised; "
                   "deferring downgrade — vkCreateDevice will report.");
    }
    if (!have_descriptor_indexing) {
        LAGFX_WARN("vk_init: descriptorIndexing feature not advertised; "
                   "bindless paths will need rewiring.");
    }
    if (!have_buffer_device_address) {
        LAGFX_WARN("vk_init: bufferDeviceAddress not advertised; M2 arg-buffer "
                   "host-flattening (PhysicalStorageBuffer) will be unavailable.");
    }
    if (!have_varptr_storage_buffer) {
        LAGFX_WARN("vk_init: variablePointersStorageBuffer not advertised; "
                   "shaders with helper-fn data-pointer params will fail "
                   "pipeline build.");
    }

    VkPhysicalDeviceShaderObjectFeaturesEXT feat_so = {
        .sType     = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT,
        .shaderObject = VK_TRUE,
    };
    VkPhysicalDeviceVulkan13Features feat13 = {
        .sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .dynamicRendering  = have_dyn_rendering_feature  ? VK_TRUE : VK_FALSE,
        .synchronization2  = have_synchronization2       ? VK_TRUE : VK_FALSE,
        .subgroupSizeControl = probe13.subgroupSizeControl,   /* request only what the device reports (queried above); lets a compute pipeline pin requiredSubgroupSize */
        .pNext             = want_shader_object ? &feat_so : NULL,
    };
    VkPhysicalDeviceVulkan11Features feat11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .variablePointersStorageBuffer =
            have_varptr_storage_buffer ? VK_TRUE : VK_FALSE,
        .pNext = &feat13,
    };
    VkPhysicalDeviceVulkan12Features feat12 = {
        .sType                                      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .timelineSemaphore                          = have_timeline_semaphore_feat ? VK_TRUE : VK_FALSE,
        .descriptorIndexing                         = have_descriptor_indexing     ? VK_TRUE : VK_FALSE,
        .runtimeDescriptorArray                     = have_descriptor_indexing     ? VK_TRUE : VK_FALSE,
        .shaderSampledImageArrayNonUniformIndexing  = have_descriptor_indexing     ? VK_TRUE : VK_FALSE,
        .bufferDeviceAddress                        = have_buffer_device_address   ? VK_TRUE : VK_FALSE,
        .samplerMirrorClampToEdge                   = probe12.samplerMirrorClampToEdge ? VK_TRUE : VK_FALSE,   /* MTLSamplerAddressModeMirrorClampToEdge */
        .pNext                                      = &feat11,
    };
    /* shaderInt64: our AIR→SPIR-V translator emits OpCapability Int64
     * (i64 GEP indices / lifetime sizes) on essentially every shader, so
     * the device MUST enable shaderInt64 or vkCreate{ShaderModule,Graphics
     * Pipelines} rejects the module (VUID-...-pCode-08740 → -13). lavapipe
     * advertises it; enable it when the ICD does. */
    bool have_shader_int64 = (probe2.features.shaderInt64 == VK_TRUE);
    if (!have_shader_int64) {
        LAGFX_WARN("vk_init: shaderInt64 not advertised by ICD; AIR shaders "
                   "that declare OpCapability Int64 will fail pipeline build.");
    }
    /* Capabilities of the generated SPIR-V (caps.h): enable ONLY what the physical device reports. LAGFX_VK_CAPS_OFF=int64,int8,... (test seam) forces some
     * of them off even if reported. Adreno reports several of these as optional. */
    /* Raw probe values, printed before anything is derived from them: on a < 1.2 device the Vulkan11/12/13Features structs chained above may be ignored by the
     * driver (left at 0) while the diagnostics read the same features through other structs; this line shows which side the "supported 0 / enabled 1" mismatch is on. */
    fprintf(stderr, "g7-probe: api=%u.%u.%u v12{f16=%d i8=%d sb8=%d bda=%d} v11{sb16=%d vptr=%d vptrsb=%d} v13{dynr=%d sync2=%d} ext{dynr=%d bda=%d sync2=%d}\n",
            VK_VERSION_MAJOR(s->phys_props.apiVersion), VK_VERSION_MINOR(s->phys_props.apiVersion), VK_VERSION_PATCH(s->phys_props.apiVersion),
            (int)probe12.shaderFloat16, (int)probe12.shaderInt8, (int)probe12.storageBuffer8BitAccess, (int)probe12.bufferDeviceAddress,
            (int)probe11.storageBuffer16BitAccess, (int)probe11.variablePointers, (int)probe11.variablePointersStorageBuffer, (int)probe13.dynamicRendering, (int)probe13.synchronization2,
            (int)want_dyn_rendering, (int)ext_bda_khr, (int)ext_sync2_khr);
    const char *caps_off_env = getenv("LAGFX_VK_CAPS_OFF");
    uint32_t caps_off = caps_off_env ? lagfx_vk_caps_parse(caps_off_env) : 0u;
    uint32_t caps_en = 0;
    if (have_shader_int64 && !(caps_off & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT64))) caps_en |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT64);
    if (probe2.features.shaderInt16) caps_en |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT16);
    if (probe12.shaderInt8) caps_en |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT8);
    if (probe12.shaderFloat16) caps_en |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_FLOAT16);
    if (probe12.storageBuffer8BitAccess) caps_en |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SB8);
    if (probe12.uniformAndStorageBuffer8BitAccess) caps_en |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_UB8);
    if (probe12.storagePushConstant8) caps_en |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_PC8);
    if (probe11.storageBuffer16BitAccess) caps_en |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SB16);
    if (probe11.uniformAndStorageBuffer16BitAccess) caps_en |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_UB16);
    if (probe11.storagePushConstant16) caps_en |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_PC16);
    if (probe11.storageInputOutput16) caps_en |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_IO16);
    if (have_varptr_storage_buffer) caps_en |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_VARPTR_SB);
    if (probe11.variablePointers) caps_en |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_VARPTR);
    {   /* atomics: 64-bit integer (VkPhysicalDeviceVulkan12Features::shaderBufferInt64Atomics for device memory, shaderSharedInt64Atomics for threadgroup memory, SEPARATE caps; both need shaderInt64) and
         * 32-bit float (VK_EXT_shader_atomic_float, queried with its own pNext struct; five features, five caps). lagfx_vk_atomic_feat_inject (tests) can only turn features off. */
        VkPhysicalDeviceShaderAtomicFloatFeaturesEXT paf = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT };
        if (ext_af) {
            VkPhysicalDeviceFeatures2 q = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &paf };
            vkGetPhysicalDeviceFeatures2(s->phys_device, &q);
        }
        lagfx_vk_atomic_feat_t af = { .shader_int64 = have_shader_int64, .buffer_int64_atomics = probe12.shaderBufferInt64Atomics == VK_TRUE, .shared_int64_atomics = probe12.shaderSharedInt64Atomics == VK_TRUE,
            .af_ext = ext_af, .buffer_f32 = ext_af && paf.shaderBufferFloat32Atomics, .buffer_f32_add = ext_af && paf.shaderBufferFloat32AtomicAdd,
            .shared_f32 = ext_af && paf.shaderSharedFloat32Atomics, .shared_f32_add = ext_af && paf.shaderSharedFloat32AtomicAdd, .image_f32 = ext_af && paf.shaderImageFloat32Atomics };
        const lagfx_vk_atomic_feat_t *inj = lagfx_vk_atomic_feat_inject;
        if (inj) {
            af.shader_int64 &= inj->shader_int64; af.buffer_int64_atomics &= inj->buffer_int64_atomics; af.shared_int64_atomics &= inj->shared_int64_atomics; af.af_ext &= inj->af_ext;
            af.buffer_f32 &= inj->buffer_f32; af.buffer_f32_add &= inj->buffer_f32_add; af.shared_f32 &= inj->shared_f32; af.shared_f32_add &= inj->shared_f32_add; af.image_f32 &= inj->image_f32;
        }
        caps_en |= lagfx_vk_atomic_caps(&af);
    }
    bool demote_ext_ok = false;
    if (!probe13.shaderDemoteToHelperInvocation && s->phys_props.apiVersion < VK_API_VERSION_1_3 && ext_demote && !getenv("LAGFX_VK_NO_DEMOTE_EXT")) {   /* Vulkan 1.1 / 1.2 device (S8: Adreno 730): through the extension */
        VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT qd = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES_EXT };
        VkPhysicalDeviceFeatures2 qf = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &qd };
        vkGetPhysicalDeviceFeatures2(s->phys_device, &qf);
        if (qd.shaderDemoteToHelperInvocation) demote_ext_ok = true, caps_en |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_DEMOTE);
    }
    if (probe13.shaderDemoteToHelperInvocation) caps_en |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_DEMOTE);   /* core 1.3 feature (VK_EXT_shader_demote_to_helper_invocation promoted); a 1.2 device with only the extension is not used: discard falls back to OpKill */
    bool spirv14_core = VK_VERSION_MAJOR(s->phys_props.apiVersion) > 1u || (VK_VERSION_MAJOR(s->phys_props.apiVersion) == 1u && VK_VERSION_MINOR(s->phys_props.apiVersion) >= 2u);
    if (spirv14_core || have_spirv14_ext) caps_en |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SPIRV14);
    bool sgc_ext_ok = false;
    {   /* simd-group (subgroup) operations a compute shader may use: Metal's simd_* intrinsics map to GroupNonUniform*; the device must offer them in the COMPUTE stage */
        VkPhysicalDeviceVulkan13Properties sgp13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES };
        VkPhysicalDeviceVulkan11Properties sgp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES };
        const bool core13p = VK_VERSION_MAJOR(s->phys_props.apiVersion) > 1u || (VK_VERSION_MAJOR(s->phys_props.apiVersion) == 1u && VK_VERSION_MINOR(s->phys_props.apiVersion) >= 3u);
        if (core13p) sgp.pNext = &sgp13;
        VkPhysicalDeviceProperties2 pp2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sgp };
        vkGetPhysicalDeviceProperties2(s->phys_device, &pp2);
        if ((sgp.subgroupSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT) && (sgp.subgroupSupportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT)) {
            caps_en |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SUBGROUP);
            if (sgp.subgroupSupportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT) caps_en |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SUBGROUP_SHUFFLE);
            if (sgp.subgroupSupportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT) caps_en |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SUBGROUP_SHUFFLE_RELATIVE);
        }
        s->sg_size = sgp.subgroupSize, s->sg_min = s->sg_max = sgp.subgroupSize;
        if (core13p) {   /* VK_EXT_subgroup_size_control (core 1.3): a compute pipeline may pin the subgroup size inside [min, max] when the feature + stage allow it */
            s->sg_min = sgp13.minSubgroupSize, s->sg_max = sgp13.maxSubgroupSize;
            s->sg_required_stages = sgp13.requiredSubgroupSizeStages;
            s->sg_control = probe13.subgroupSizeControl == VK_TRUE && (sgp13.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0;
            s->sg_full = probe13.computeFullSubgroups == VK_TRUE;
        } else if (ext_sgc && !getenv("LAGFX_VK_NO_SGC_EXT")) {   /* Vulkan 1.1 / 1.2 device (S8 Adreno 730 lists the extension): its own property + feature structs; the 1.3 ones are not valid here */
            VkPhysicalDeviceSubgroupSizeControlPropertiesEXT sp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES_EXT };
            VkPhysicalDeviceProperties2 pq = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sp };
            vkGetPhysicalDeviceProperties2(s->phys_device, &pq);
            VkPhysicalDeviceSubgroupSizeControlFeaturesEXT sf = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT };
            VkPhysicalDeviceFeatures2 fq = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &sf };
            vkGetPhysicalDeviceFeatures2(s->phys_device, &fq);
            s->sg_min = sp.minSubgroupSize, s->sg_max = sp.maxSubgroupSize;
            s->sg_required_stages = sp.requiredSubgroupSizeStages;
            sgc_ext_ok = sf.subgroupSizeControl == VK_TRUE && (sp.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0 && wanted_dev_n < 22u;
            s->sg_control = sgc_ext_ok;
            s->sg_full = sf.computeFullSubgroups == VK_TRUE;
            s->sg_ext_queried = true;
        }
    }
    if (legacy_feat && have_buffer_device_address && wanted_dev_n < 20) wanted_dev_exts[wanted_dev_n++] = "VK_KHR_buffer_device_address";
    caps_en &= ~caps_off;
    if (!(caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT64)))   /* 64-bit atomics need shaderInt64 enabled (also when LAGFX_VK_CAPS_OFF turned int64 off) */
        caps_en &= ~(LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC64_BUF) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC64_SHARED));
    if (!spirv14_core && have_spirv14_ext && (caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SPIRV14))) {
        wanted_dev_exts[wanted_dev_n++] = "VK_KHR_spirv_1_4";
        wanted_dev_exts[wanted_dev_n++] = "VK_KHR_shader_float_controls";
    }
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT demote_en = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES_EXT, .shaderDemoteToHelperInvocation = VK_TRUE };
    if (demote_ext_ok && (caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_DEMOTE)) && wanted_dev_n < 22u) {   /* before the legacy chain is linked below: it takes feat13.pNext as its tail */
        demote_en.pNext = feat13.pNext; feat13.pNext = &demote_en;
        wanted_dev_exts[wanted_dev_n++] = "VK_EXT_shader_demote_to_helper_invocation";
    } else if (demote_ext_ok) caps_en &= ~LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_DEMOTE);   /* cannot enable the extension: do not advertise the capability */
    VkPhysicalDeviceSubgroupSizeControlFeaturesEXT sgc_en = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT, .subgroupSizeControl = VK_TRUE };
    if (sgc_ext_ok && wanted_dev_n < 22u) {   /* linked before the legacy chain takes feat13.pNext as its tail */
        sgc_en.pNext = feat13.pNext; feat13.pNext = &sgc_en;
        wanted_dev_exts[wanted_dev_n++] = "VK_EXT_subgroup_size_control";
    } else if (sgc_ext_ok) s->sg_control = false;
    feat12.shaderInt8 = (caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT8)) ? VK_TRUE : VK_FALSE;
    feat12.shaderFloat16 = (caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_FLOAT16)) ? VK_TRUE : VK_FALSE;
    feat12.shaderBufferInt64Atomics = (caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC64_BUF)) ? VK_TRUE : VK_FALSE;      /* each feature follows its own cap: a device with only one of them is still created */
    feat12.shaderSharedInt64Atomics = (caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC64_SHARED)) ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT af_en = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT,
        .shaderBufferFloat32Atomics = (caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_BUF)) ? VK_TRUE : VK_FALSE,
        .shaderBufferFloat32AtomicAdd = (caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_BUF_ADD)) ? VK_TRUE : VK_FALSE,
        .shaderSharedFloat32Atomics = (caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_SHARED)) ? VK_TRUE : VK_FALSE,
        .shaderSharedFloat32AtomicAdd = (caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_SHARED_ADD)) ? VK_TRUE : VK_FALSE,
        .shaderImageFloat32Atomics = (caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_IMAGE)) ? VK_TRUE : VK_FALSE,
    };
    if (caps_en & (LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_BUF) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_BUF_ADD) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_SHARED) |
                   LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_SHARED_ADD) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_IMAGE))) {   /* only a supported feature is requested; the extension only when one is */
        af_en.pNext = feat13.pNext; feat13.pNext = &af_en;
        wanted_dev_exts[wanted_dev_n++] = "VK_EXT_shader_atomic_float";
    }
    feat12.storageBuffer8BitAccess = (caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SB8)) ? VK_TRUE : VK_FALSE;
    feat12.uniformAndStorageBuffer8BitAccess = (caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_UB8)) ? VK_TRUE : VK_FALSE;
    feat12.storagePushConstant8 = (caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_PC8)) ? VK_TRUE : VK_FALSE;
    feat13.shaderDemoteToHelperInvocation = (caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_DEMOTE)) ? VK_TRUE : VK_FALSE;
    feat11.storageBuffer16BitAccess = (caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SB16)) ? VK_TRUE : VK_FALSE;
    feat11.uniformAndStorageBuffer16BitAccess = (caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_UB16)) ? VK_TRUE : VK_FALSE;
    feat11.storagePushConstant16 = (caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_PC16)) ? VK_TRUE : VK_FALSE;
    feat11.storageInputOutput16 = (caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_IO16)) ? VK_TRUE : VK_FALSE;
    feat11.variablePointers = (caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_VARPTR)) ? VK_TRUE : VK_FALSE;
    feat11.variablePointersStorageBuffer = (caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_VARPTR_SB)) ? VK_TRUE : VK_FALSE;
    /* Below Vulkan 1.2: create the device with the individual feature structs (same values as feat11/feat12 hold) instead of the unified ones. Never both:
     * a unified struct next to an individual one for the same feature is invalid usage. The shader-atomic-float struct (feat13.pNext) stays at the tail. */
    VkPhysicalDeviceShaderFloat16Int8Features   le_f16i8 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, .shaderFloat16 = feat12.shaderFloat16, .shaderInt8 = feat12.shaderInt8 };
    VkPhysicalDevice8BitStorageFeatures         le_8bit  = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, .storageBuffer8BitAccess = feat12.storageBuffer8BitAccess,
                                                             .uniformAndStorageBuffer8BitAccess = feat12.uniformAndStorageBuffer8BitAccess, .storagePushConstant8 = feat12.storagePushConstant8 };
    VkPhysicalDevice16BitStorageFeatures        le_16bit = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, .storageBuffer16BitAccess = feat11.storageBuffer16BitAccess,
                                                             .uniformAndStorageBuffer16BitAccess = feat11.uniformAndStorageBuffer16BitAccess, .storagePushConstant16 = feat11.storagePushConstant16, .storageInputOutput16 = feat11.storageInputOutput16 };
    VkPhysicalDeviceVariablePointersFeatures    le_vptr  = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES, .variablePointers = feat11.variablePointers, .variablePointersStorageBuffer = feat11.variablePointersStorageBuffer };
    VkPhysicalDeviceBufferDeviceAddressFeatures le_bda   = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, .bufferDeviceAddress = have_buffer_device_address ? VK_TRUE : VK_FALSE };
    void *legacy_head = NULL;
    if (legacy_feat) {
        void *tail = feat13.pNext;   /* NULL or af_en */
        le_bda.pNext = tail; le_vptr.pNext = &le_bda; le_16bit.pNext = &le_vptr;
        le_8bit.pNext = &le_16bit; le_f16i8.pNext = &le_8bit;
        legacy_head = &le_f16i8;
    }
    VkPhysicalDeviceFeatures2 feat2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .features = { .shaderInt64 = (caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT64)) ? VK_TRUE : VK_FALSE,
                      .shaderInt16 = (caps_en & LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT16)) ? VK_TRUE : VK_FALSE,
                      .independentBlend = probe2.features.independentBlend ? VK_TRUE : VK_FALSE,
                      .depthBiasClamp = probe2.features.depthBiasClamp ? VK_TRUE : VK_FALSE },
        .pNext = legacy_feat ? legacy_head : (void *)&feat12,
    };

    /* Queue create info — one graphics+compute+transfer queue. */
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = s->graphics_queue_family,
        .queueCount       = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo dci = {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                   = &feat2,
        .queueCreateInfoCount    = 1,
        .pQueueCreateInfos       = &qci,
        .enabledExtensionCount   = wanted_dev_n,
        .ppEnabledExtensionNames = wanted_dev_n ? wanted_dev_exts : NULL,
    };
    s->have_independent_blend = probe2.features.independentBlend == VK_TRUE;
    VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT ro_en = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT, .rasterizationOrderColorAttachmentAccess = VK_TRUE };
    feat2.features.samplerAnisotropy = probe2.features.samplerAnisotropy ? VK_TRUE : VK_FALSE;   /* sampler anisotropy (Metal maxAnisotropy) */
    s->have_sampler_anisotropy = feat2.features.samplerAnisotropy == VK_TRUE;
    s->have_sampler_mirror_clamp = feat12.samplerMirrorClampToEdge == VK_TRUE;
    /* Shader stores: storage-image writes without a format (texture2d<T, access::write>) and buffer/image writes from the fragment and vertex stages. */
    feat2.features.shaderStorageImageWriteWithoutFormat = probe2.features.shaderStorageImageWriteWithoutFormat ? VK_TRUE : VK_FALSE;
    feat2.features.fragmentStoresAndAtomics = probe2.features.fragmentStoresAndAtomics ? VK_TRUE : VK_FALSE;
    feat2.features.vertexPipelineStoresAndAtomics = probe2.features.vertexPipelineStoresAndAtomics ? VK_TRUE : VK_FALSE;
    s->have_storage_img_write = feat2.features.shaderStorageImageWriteWithoutFormat && feat2.features.fragmentStoresAndAtomics;
    feat2.features.shaderStorageImageReadWithoutFormat = probe2.features.shaderStorageImageReadWithoutFormat ? VK_TRUE : VK_FALSE;   /* unformatted storage reads: the tile kernel's BGRA8 imageblock (arm64_tile.c) */
    s->have_storage_img_rw_nofmt = feat2.features.shaderStorageImageReadWithoutFormat && feat2.features.shaderStorageImageWriteWithoutFormat;
    s->have_vertex_stores = feat2.features.vertexPipelineStoresAndAtomics == VK_TRUE;
    s->have_depth_bias_clamp = probe2.features.depthBiasClamp == VK_TRUE;
    feat2.features.sampleRateShading = probe2.features.sampleRateShading ? VK_TRUE : VK_FALSE;   /* per-sample shading (sampleShadingEnable / SampleId): src/vulkan/msaa_host.h, unwired from the guest */
    s->have_sample_rate_shading = feat2.features.sampleRateShading == VK_TRUE;
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds_en = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, .extendedDynamicState = VK_TRUE };
    bool eds_ext_on = false;
    local_read_feat_t lr_en = { .sType = (VkStructureType)1000232000, .dynamicRenderingLocalRead = VK_TRUE };
    if (cubic_ext) {
        wanted_dev_exts[wanted_dev_n++] = cubic_ext;
        dci.enabledExtensionCount = wanted_dev_n, dci.ppEnabledExtensionNames = wanted_dev_exts;
        s->have_filter_cubic = true;
    }
    {   /* Framebuffer-fetch / blend diagnostics, queried only (never enabled here): printed once as the g7-gpu-feat line. Own struct for local read: older headers lack it. */
        const bool lr_core = VK_VERSION_MAJOR(s->phys_props.apiVersion) > 1u || (VK_VERSION_MAJOR(s->phys_props.apiVersion) == 1u && VK_VERSION_MINOR(s->phys_props.apiVersion) >= 4u);
        local_read_feat_t lr = { .sType = (VkStructureType)1000232000 };   /* VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES */
        VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT ro = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT };
        void *chain = NULL;
        if (lr_core || ext_lr) { lr.pNext = chain; chain = &lr; }
        if (ext_ro_ext || ext_ro_arm) { ro.pNext = chain; chain = &ro; }
        if (chain) {
            VkPhysicalDeviceFeatures2 q = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = chain };
            vkGetPhysicalDeviceFeatures2(s->phys_device, &q);
        }
        if (ext_lr && lr.dynamicRenderingLocalRead && !getenv("LAGFX_VK_NO_LOCAL_READ")) {   /* framebuffer fetch (input attachment reads of the colour attachments) */
            lr_en.pNext = feat13.pNext; feat13.pNext = &lr_en;
            wanted_dev_exts[wanted_dev_n++] = "VK_KHR_dynamic_rendering_local_read";
            dci.enabledExtensionCount = wanted_dev_n, dci.ppEnabledExtensionNames = wanted_dev_exts;
            s->have_local_read = true;
        }
        if (s->have_local_read && lr_core) {   /* Vulkan 1.4 reports whether local reads also work on MULTISAMPLED attachments (VkPhysicalDeviceVulkan14Properties::dynamicRenderingLocalReadMultisampledAttachments).
             * Older headers lack the struct: a zeroed buffer larger than it, sType 56 (PHYSICAL_DEVICE_VULKAN_1_4_PROPERTIES), the member read at its offset (sType, pNext, then u32 lineSubPixelPrecisionBits,
             * maxVertexAttribDivisor, supportsNonZeroFirstInstance, maxPushDescriptors, dynamicRenderingLocalReadDepthStencilAttachments, dynamicRenderingLocalReadMultisampledAttachments). */
            union { VkBaseOutStructure h; uint64_t align[128]; } v14;
            memset(&v14, 0, sizeof v14);
            v14.h.sType = (VkStructureType)56;
            VkPhysicalDeviceProperties2 pq = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &v14 };
            vkGetPhysicalDeviceProperties2(s->phys_device, &pq);
            uint32_t ms_lr; memcpy(&ms_lr, (const uint8_t *)&v14 + sizeof(VkBaseOutStructure) + 5u * sizeof(uint32_t), sizeof ms_lr);
            s->have_local_read_ms = ms_lr == VK_TRUE;
        }
        if (s->have_local_read && !s->have_local_read_ms && lagfx_env_is_one("LAGFX_VK_FORCE_MS_LOCAL_READ"))   /* opt-in for a device that renders it correctly without advertising it (lavapipe: property 0, works): outside the spec, at the user's risk */
            s->have_local_read_ms = true;
        if ((ext_ro_ext || ext_ro_arm) && ro.rasterizationOrderColorAttachmentAccess && !getenv("LAGFX_VK_NO_RASTER_ORDER")) {   /* same-draw ordering of input-attachment reads/writes */
            ro_en.pNext = feat13.pNext; feat13.pNext = &ro_en;
            wanted_dev_exts[wanted_dev_n++] = ext_ro_ext ? "VK_EXT_rasterization_order_attachment_access" : "VK_ARM_rasterization_order_attachment_access";
            dci.enabledExtensionCount = wanted_dev_n, dci.ppEnabledExtensionNames = wanted_dev_exts;
            s->have_raster_order = true;
        }
        if (s->phys_props.apiVersion < VK_API_VERSION_1_3 && ext_eds1 && !getenv("LAGFX_VK_NO_EDS1") && wanted_dev_n < 23u) {   /* cull / front face / depth / stencil as dynamic state below 1.3 */
            VkPhysicalDeviceExtendedDynamicStateFeaturesEXT q1 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT };
            VkPhysicalDeviceFeatures2 q = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &q1 };
            vkGetPhysicalDeviceFeatures2(s->phys_device, &q);
            if (q1.extendedDynamicState) {
                eds_en.pNext = feat13.pNext; feat13.pNext = &eds_en;
                wanted_dev_exts[wanted_dev_n++] = "VK_EXT_extended_dynamic_state";
                dci.enabledExtensionCount = wanted_dev_n, dci.ppEnabledExtensionNames = wanted_dev_exts;
                eds_ext_on = true;
            }
        }
        static bool feat_printed;
        if (!feat_printed) {
            feat_printed = true;
            char fl[400];
            snprintf(fl, sizeof fl, "g7-gpu-feat: local_read_enabled=%d raster_order_enabled=%d independentBlend=%d dynamicRenderingLocalRead=%d(core1.4=%d ext=%d) rasterOrderColorAttachmentAccess=%d(ext_EXT=%d ext_ARM=%d) rasterOrderDepthAccess=%d rasterOrderStencilAccess=%d",
                     (int)s->have_local_read, (int)s->have_raster_order, (int)probe2.features.independentBlend, (int)((lr_core || ext_lr) ? lr.dynamicRenderingLocalRead : 0), (int)lr_core, (int)ext_lr,
                     (int)((ext_ro_ext || ext_ro_arm) ? ro.rasterizationOrderColorAttachmentAccess : 0), (int)ext_ro_ext, (int)ext_ro_arm,
                     (int)((ext_ro_ext || ext_ro_arm) ? ro.rasterizationOrderDepthAttachmentAccess : 0), (int)((ext_ro_ext || ext_ro_arm) ? ro.rasterizationOrderStencilAttachmentAccess : 0));
            fprintf(stderr, "%s\n", fl);
            LAGFX_LOG("%s", fl);
        }
    }
    /* VK_QCOM_tile_shading (opt-in, LAGFX_QCOM_TILE=1; src/vulkan/qcom_tile.h). Enabling it does not change what any pipeline does: no tile-shading command is recorded anywhere (the Metal tile path stays the
     * compute emulation). It only lets on-device experiments (docs/qcom-tile-shading.md) run against a device that was created with the extension. Fail-closed: any missing piece -> not requested;
     * vkCreateDevice refusing the chain -> retried once without it (fallback recorded). */
    lagfx_qcomtile_caps_t qct;
    lagfx_qcomtile_probe(s->phys_device, ext_qct_ts, ext_qct_tp, ext_qct_heap, &qct);
    const bool qct_flag = lagfx_qcomtile_flag(getenv("LAGFX_QCOM_TILE"));
    bool qct_inject = false;
    if (qct_flag) {   /* test seam, only with the flag on: pretend the device is the Adreno 840 of the device log (lavapipe then refuses the extension, which exercises the fallback) */
        const char *inj = getenv("LAGFX_QCOM_TILE_INJECT");
        if (inj && !strcmp(inj, "adreno840")) lagfx_qcomtile_caps_adreno840(&qct), qct_inject = true;
    }
    lagfx_qcomtile_reason_t qct_reason = lagfx_qcomtile_decide(qct_flag, feat13.dynamicRendering == VK_TRUE, &qct);
    uint32_t qct_mask = 0;
    bool qct_enabled = false;
    lagfx_qct_feat_t qct_feat; lagfx_qct_bool_feat_t qct_tpf;
    void *qct_saved_chain = feat13.pNext; const uint32_t qct_saved_n = wanted_dev_n;
    if (qct_reason == LAGFX_QCT_ENABLE && wanted_dev_n + 2u <= (uint32_t)(sizeof wanted_dev_exts / sizeof wanted_dev_exts[0])) {
        qct_mask = lagfx_qcomtile_enable_mask(&qct);
        lagfx_qcomtile_fill_features(&qct, &qct_feat, &qct_tpf);
        qct_feat.pNext = feat13.pNext; qct_tpf.pNext = &qct_feat; feat13.pNext = &qct_tpf;
        wanted_dev_exts[wanted_dev_n++] = "VK_QCOM_tile_properties";
        wanted_dev_exts[wanted_dev_n++] = "VK_QCOM_tile_shading";
        dci.enabledExtensionCount = wanted_dev_n, dci.ppEnabledExtensionNames = wanted_dev_exts;
        qct_enabled = true;
    }
    if (legacy_feat) le_bda.pNext = feat13.pNext;   /* the legacy chain ends in whatever hangs off feat13.pNext NOW (demote / EDS1 / local read / raster order / tile structs were linked after it was built) */
    vr = vkCreateDevice(s->phys_device, &dci, NULL, &s->device);
    lagfx_qcomtile_fallback_t qct_fb = LAGFX_QCT_FB_NONE;
    int qct_create = (int)vr;
    if (vr != VK_SUCCESS && qct_enabled) {   /* the driver refused the extension chain: back to exactly what the default path sends */
        feat13.pNext = qct_saved_chain, wanted_dev_n = qct_saved_n;
        dci.enabledExtensionCount = wanted_dev_n, dci.ppEnabledExtensionNames = wanted_dev_n ? wanted_dev_exts : NULL;
        qct_enabled = false, qct_mask = 0, qct_fb = LAGFX_QCT_FB_DEVICE_CREATE_FAILED;
        if (legacy_feat) le_bda.pNext = feat13.pNext;
        vr = vkCreateDevice(s->phys_device, &dci, NULL, &s->device);
    }
    s->qcomtile.flag = qct_flag, s->qcomtile.injected = qct_inject, s->qcomtile.reason = qct_reason, s->qcomtile.enabled = qct_enabled, s->qcomtile.fallback = qct_fb;
    s->qcomtile.enable_mask = qct_mask, s->qcomtile.create_result = qct_create, s->qcomtile.caps = qct;
    lagfx_qcomtile_format_line(s->qcomtile.line, sizeof s->qcomtile.line, qct_flag, qct_inject, &qct, qct_reason, qct_mask, qct_enabled, qct_fb, qct_create);
    {   /* once per process */
        static bool qct_printed;
        if (!qct_printed) { qct_printed = true; fprintf(stderr, "%s\n", s->qcomtile.line); LAGFX_LOG("%s", s->qcomtile.line); }
    }
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("vk_init: vkCreateDevice failed (VkResult=%d)", (int)vr);
        vkDestroyInstance(s->instance, NULL);
        free(s);
        return LAGFX_ERR_VULKAN_INIT;
    }

    s->caps_enabled = caps_en, s->caps_missing = LAGFX_VK_CAP_ALL & ~caps_en;
    lagfx_pipeline_matrix_set_device(s->have_local_read, s->have_raster_order, s->phys_props.limits.maxColorAttachments, s->phys_props.limits.maxPerStageDescriptorInputAttachments);   /* only read by the opt-in LAGFX_PIPELINE_MATRIX diagnostics */
    {   /* record what the device was created with (read back by the pipeline failure bundle) */
        size_t eo = 0;
        s->dev_exts[0] = 0;
        for (uint32_t i = 0; i < wanted_dev_n; i++) { int w = snprintf(s->dev_exts + eo, sizeof s->dev_exts - eo, "%s%s", i ? "," : "", wanted_dev_exts[i]); if (w < 0 || (size_t)w >= sizeof s->dev_exts - eo) break; eo += (size_t)w; }
        snprintf(s->dev_features, sizeof s->dev_features,
                 "shaderInt64=%d shaderInt16=%d independentBlend=%d depthBiasClamp=%d samplerAnisotropy=%d shaderStorageImageWriteWithoutFormat=%d fragmentStoresAndAtomics=%d vertexPipelineStoresAndAtomics=%d "
                 "shaderInt8=%d shaderFloat16=%d storageBuffer8BitAccess=%d uniformAndStorageBuffer8BitAccess=%d storagePushConstant8=%d storageBuffer16BitAccess=%d uniformAndStorageBuffer16BitAccess=%d storagePushConstant16=%d "
                 "storageInputOutput16=%d variablePointers=%d variablePointersStorageBuffer=%d timelineSemaphore=%d descriptorIndexing=%d bufferDeviceAddress=%d dynamicRendering=%d synchronization2=%d subgroupSizeControl=%d "
                 "dynamicRenderingLocalRead=%d rasterizationOrderColorAttachmentAccess=%d",
                 (int)feat2.features.shaderInt64, (int)feat2.features.shaderInt16, (int)feat2.features.independentBlend, (int)feat2.features.depthBiasClamp, (int)feat2.features.samplerAnisotropy,
                 (int)feat2.features.shaderStorageImageWriteWithoutFormat, (int)feat2.features.fragmentStoresAndAtomics, (int)feat2.features.vertexPipelineStoresAndAtomics,
                 (int)feat12.shaderInt8, (int)feat12.shaderFloat16, (int)feat12.storageBuffer8BitAccess, (int)feat12.uniformAndStorageBuffer8BitAccess, (int)feat12.storagePushConstant8,
                 (int)feat11.storageBuffer16BitAccess, (int)feat11.uniformAndStorageBuffer16BitAccess, (int)feat11.storagePushConstant16, (int)feat11.storageInputOutput16,
                 (int)feat11.variablePointers, (int)feat11.variablePointersStorageBuffer, (int)feat12.timelineSemaphore, (int)feat12.descriptorIndexing, (int)feat12.bufferDeviceAddress,
                 (int)feat13.dynamicRendering, (int)feat13.synchronization2, (int)feat13.subgroupSizeControl, (int)s->have_local_read, (int)s->have_raster_order);
        VkPhysicalDeviceDriverProperties drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        VkPhysicalDeviceProperties2 dp2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv };
        vkGetPhysicalDeviceProperties2(s->phys_device, &dp2);
        snprintf(s->driver_name, sizeof s->driver_name, "%s", drv.driverName), snprintf(s->driver_info, sizeof s->driver_info, "%s", drv.driverInfo);
    }
    if (feat13.subgroupSizeControl != VK_TRUE && !s->sg_ext_queried) s->sg_control = false;   /* the pre-1.3 path enabled it through sgc_en */
    {   /* what vkCreateDevice was actually given: the feature VALUES from the structs chained into dci (feat2 / feat11 / feat12 / feat13, lr_en and ro_en only when chained: have_local_read / have_raster_order),
         * and the real wanted_dev_exts array. VK_EXT_shader_tile_image is never requested here (no such extension name in wanted_dev_exts, no tile-image feature struct in the chain) -> 0. */
        const lagfx_vkdev_enabled_t en = {
            .exts = wanted_dev_exts, .n_exts = wanted_dev_n,
            .dynamicRendering = feat13.dynamicRendering, .dynamicRenderingLocalRead = s->have_local_read ? (int32_t)lr_en.dynamicRenderingLocalRead : 0,
            .rasterizationOrderColorAttachmentAccess = s->have_raster_order ? (int32_t)ro_en.rasterizationOrderColorAttachmentAccess : 0,
            .shaderTileImageColorReadAccess = 0, .shaderTileImageDepthReadAccess = 0, .shaderTileImageStencilReadAccess = 0,
            .subgroupSizeControl = feat13.subgroupSizeControl, .computeFullSubgroups = feat13.computeFullSubgroups,
            .shaderFloat16 = feat12.shaderFloat16, .sampleRateShading = feat2.features.sampleRateShading, .shaderInt64 = feat2.features.shaderInt64, .shaderInt8 = feat12.shaderInt8,
            .storageBuffer16BitAccess = feat11.storageBuffer16BitAccess, .storageBuffer8BitAccess = feat12.storageBuffer8BitAccess,
            .variablePointers = feat11.variablePointers, .variablePointersStorageBuffer = feat11.variablePointersStorageBuffer };
        lagfx_vk_devinfo_log_once_ex(s->phys_device, &en);   /* read-only g7-vkdev diagnostic (devinfo.h), WARN, once per process */
    }
    {   /* printed once per process: Metal's simdgroup is 32 lanes; a kernel that maps lanes to pixels needs a 32-wide subgroup (pinned when the device allows it) */
        static bool sg_printed;
        if (!sg_printed) {
            sg_printed = true;
            char sl[300];
            snprintf(sl, sizeof sl, "g7-gpu-feat: subgroup size=%u min=%u max=%u via=%s subgroupSizeControl=%d requiredSubgroupSizeStages=0x%x computeFullSubgroups(supported)=%d computeFullSubgroups(enabled)=0 pin32=%s",
                     s->sg_size, s->sg_min, s->sg_max, s->sg_ext_queried ? "EXT" : (s->phys_props.apiVersion >= VK_API_VERSION_1_3 ? "core13" : "none"), (int)s->sg_control, (unsigned)s->sg_required_stages, (int)s->sg_full,
                     s->sg_size == 32u ? "native" : lagfx_vk_can_pin_subgroup32(s) ? "required_size" : "no(lane-mapped tile kernels are skipped)");
            fprintf(stderr, "%s\n", sl);
            LAGFX_LOG("%s", sl);
        }
    }
    {   /* printed once per process */
        static bool caps_printed;
        if (!caps_printed) {
            caps_printed = true;
            char cl[512];
            lagfx_vk_caps_line(s, cl, sizeof cl);
            fprintf(stderr, "%s\n", cl);
            LAGFX_LOG("%s", cl);
        }
    }
    s->have_dynamic_rendering      = have_dyn_rendering_feature;
    s->render_backend              = (int)lagfx_vk_render_backend_select(have_dyn_rendering_feature);
    { const char *lf = getenv("LAGFX_VK_LEGACY_FETCH"); s->legacy_fetch = !(lf && !strcmp(lf, "0")); }
    if (s->render_backend == LAGFX_RENDER_LEGACY) {   /* dynamic local read is not used on the legacy backend; legacy fetch uses input attachments (render_pass.c). Framebuffer fetch needs input attachments inside a VkRenderPass: not implemented, so such draws are refused (counted as stub_fbfetch), never run with the wrong layout */
        s->have_local_read = false;
        s->have_local_read_ms = false;
    }
    s->have_synchronization2       = have_synchronization2;
    s->have_timeline_semaphore     = have_timeline_semaphore_feat;
    s->have_descriptor_indexing    = have_descriptor_indexing;
    s->have_shader_object          = want_shader_object;
    s->have_extended_dynamic_state3= want_eds3;
    s->have_eds = false; s->eds_ext = false; memset(&s->eds, 0, sizeof s->eds);
    {   /* the dynamic encoder state: core names on >= 1.3, the EXT names when the extension was enabled */
        const bool core = s->phys_props.apiVersion >= VK_API_VERSION_1_3;
        if (core || eds_ext_on) {
            const char *sfx = core ? "" : "EXT";
            char nm[48];
#define LAGFX_EDS_LOAD(field, base) do { snprintf(nm, sizeof nm, "%s%s", base, sfx); *(PFN_vkVoidFunction *)&s->eds.field = vkGetDeviceProcAddr(s->device, nm); } while (0)
            LAGFX_EDS_LOAD(cull, "vkCmdSetCullMode"); LAGFX_EDS_LOAD(front_face, "vkCmdSetFrontFace"); LAGFX_EDS_LOAD(depth_test, "vkCmdSetDepthTestEnable");
            LAGFX_EDS_LOAD(depth_write, "vkCmdSetDepthWriteEnable"); LAGFX_EDS_LOAD(depth_compare, "vkCmdSetDepthCompareOp"); LAGFX_EDS_LOAD(stencil_test, "vkCmdSetStencilTestEnable");
            LAGFX_EDS_LOAD(stencil_op, "vkCmdSetStencilOp");
#undef LAGFX_EDS_LOAD
            s->have_eds = s->eds.cull && s->eds.front_face && s->eds.depth_test && s->eds.depth_write && s->eds.depth_compare && s->eds.stencil_test && s->eds.stencil_op;
            s->eds_ext = s->have_eds && !core;
        }
    }
    LAGFX_LOG("vk_init: dynamic encoder state (cull / front face / depth / stencil) %s", s->have_eds ? (s->eds_ext ? "via VK_EXT_extended_dynamic_state" : "via Vulkan 1.3 core") : "unavailable: state ignored, depth / stencil attachments refused");
    s->have_ds_resolve = false; s->create_renderpass2 = NULL;    s->have_ds_resolve = false; s->create_renderpass2 = NULL;
    if ((s->phys_props.apiVersion >= VK_API_VERSION_1_2 || dsres_ext_on) && !getenv("LAGFX_VK_NO_DS_RESOLVE")) {
        lagfx_msaa_dsres_caps_t dc;
        const bool probed = lagfx_msaa_dsres_probe(s->phys_device, &dc);   /* the shared probe (src/vulkan/msaa_host.c), core 1.2 or KHR on 1.1 */
        s->create_renderpass2 = (PFN_vkCreateRenderPass2)vkGetDeviceProcAddr(s->device, s->phys_props.apiVersion >= VK_API_VERSION_1_2 ? "vkCreateRenderPass2" : "vkCreateRenderPass2KHR");
        s->dsres_depth_modes = probed ? dc.depth_modes : 0, s->dsres_stencil_modes = probed ? dc.stencil_modes : 0;
        s->dsres_independent_none = probed && dc.independent_none, s->dsres_independent = probed && dc.independent;
        s->have_ds_resolve = s->create_renderpass2 != NULL && (s->dsres_depth_modes | s->dsres_stencil_modes) != 0;
        LAGFX_LOG("vk_init: depth/stencil resolve %s (%s) depth_modes=0x%x stencil_modes=0x%x independent_none=%d independent=%d", s->have_ds_resolve ? "available" : "unavailable",
                  s->phys_props.apiVersion >= VK_API_VERSION_1_2 ? "core" : "KHR", (unsigned)s->dsres_depth_modes, (unsigned)s->dsres_stencil_modes, (int)s->dsres_independent_none, (int)s->dsres_independent);
    }
    s->get_buffer_address = NULL;
    if (have_buffer_device_address) {
        s->get_buffer_address = (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(s->device, s->phys_props.apiVersion >= VK_API_VERSION_1_2 ? "vkGetBufferDeviceAddress" : "vkGetBufferDeviceAddressKHR");
        if (!s->get_buffer_address) {
            LAGFX_WARN("vk_init: bufferDeviceAddress feature enabled but the entry point did not resolve; BDA disabled");
            have_buffer_device_address = false;
        }
    }
    s->have_buffer_device_address  = have_buffer_device_address;
    LAGFX_LOG("vk_init: bufferDeviceAddress=%d (M2 host-flattening %s)",
              (int)have_buffer_device_address,
              have_buffer_device_address ? "available" : "UNAVAILABLE");

    LAGFX_STAGE("vk_init:get_queue");
    vkGetDeviceQueue(s->device, s->graphics_queue_family, 0,
                     &s->graphics_queue);
    if (s->graphics_queue == VK_NULL_HANDLE) {
        LAGFX_ERR("vk_init: failed to retrieve queue");
        vkDestroyDevice(s->device, NULL);
        vkDestroyInstance(s->instance, NULL);
        free(s);
        return LAGFX_ERR_VULKAN_INIT;
    }

    /* Every render/pipeline path (passthrough, cursor, draw) builds
     * pipelines with VkPipelineRenderingCreateInfo + renderPass=NULL and
     * records via vkCmdBeginRendering. On a driver without dynamic
     * rendering (e.g. Adreno 730 / Vulkan 1.1.128 on Galaxy Tab S8) that
     * null-derefs inside the driver (SIGSEGV fault_addr=0xe8 right after
     * cmd_pool_create). Refuse cleanly so the caller (lagfx_device_new ->
     * NULL) can fall back to its built-in gfx device. */
    /* Every call site now has a legacy VkRenderPass path (render_pass.c), for a limited set of shapes (colour attachments without gaps, optional depth/stencil, 1x samples,
     * no resolve, no framebuffer fetch; the rest is refused and counted). That path is verified on lavapipe only, so it stays OFF by default: LAGFX_VK_LEGACY_RENDERPASS_GPU=1
     * lets a device without dynamic rendering initialise anyway (the test path for such a device). */
    if (!have_dyn_rendering_feature && !lagfx_env_is_one("LAGFX_VK_LEGACY_RENDERPASS_GPU")) {
        fprintf(stderr, "lagfx: Vulkan dynamicRendering unsupported by this "
                "GPU/driver; refusing init (LAGFX_ERR_VULKAN_INIT)\n");
        LAGFX_ERR("vk_init: dynamicRendering unsupported; refusing init so "
                  "the host can fall back to its built-in gfx device");
        vkDestroyDevice(s->device, NULL);
        vkDestroyInstance(s->instance, NULL);
        free(s);
        return LAGFX_ERR_VULKAN_INIT;
    }

    s->initialized = true;
    LAGFX_LOG("vk_init: device=%p queue=%p (fam=%u) shader_object=%d "
              "eds3=%d", (void *)s->device, (void *)s->graphics_queue,
              s->graphics_queue_family,
              (int)s->have_shader_object,
              (int)s->have_extended_dynamic_state3);

    /* Phase 1.B.2: create the command pool AFTER the device + queue are
     * live. Failure here unwinds the whole Vulkan state — a device
     * without a command pool can't make progress in Phase 2. */
    LAGFX_STAGE("vk_init:command_pool");
    lagfx_status_t cp_st = lagfx_vk_command_pool_create(s);
    if (cp_st != LAGFX_OK) {
        LAGFX_ERR("vk_init: lagfx_vk_command_pool_create failed (status=%d)",
                  (int)cp_st);
        vkDestroyDevice(s->device, NULL);
        vkDestroyInstance(s->instance, NULL);
        free(s);
        return LAGFX_ERR_VULKAN_INIT;
    }

    /* Phase 3.E: create the passthrough pipeline, frame image, and
     * dummy vertex buffer. Failure is non-fatal for device init but
     * draw calls will lack a real pipeline. */
    LAGFX_STAGE("vk_init:pipeline_init");
    lagfx_status_t pp_st = lagfx_vk_pipeline_init(s);
    if (pp_st != LAGFX_OK) {
        LAGFX_WARN("vk_init: lagfx_vk_pipeline_init failed (status=%d) "
                   "— draw calls will lack passthrough pipeline",
                   (int)pp_st);
    }

    /* Stage 65d Option 3 — create the shared empty pipeline layout for
     * substitute triangle pipelines. Vulkan rejects layout=VK_NULL_HANDLE
     * in VkGraphicsPipelineCreateInfo, so even shaders with zero
     * descriptor bindings need a valid (empty) layout. */
    {
        VkPipelineLayoutCreateInfo el_plci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        };
        VkResult lr = vkCreatePipelineLayout(s->device, &el_plci, NULL,
                                             &s->empty_layout);
        if (lr != VK_SUCCESS) {
            LAGFX_WARN("vk_init: vkCreatePipelineLayout(empty_layout) failed "
                       "(%d) — Stage 65d Option 3 pipeline builds will fail",
                       (int)lr);
            s->empty_layout = VK_NULL_HANDLE;
        }
    }

    LAGFX_STAGE("vk_init:done");
    *out = s;
    return LAGFX_OK;
}

void lagfx_vk_shutdown(struct lagfx_vk_state *state) {
    if (!state) {
        return;
    }
    if (state->frame_in_progress) {
        vkEndCommandBuffer(state->frame_cmdbuf);
        state->frame_in_progress = false;
    }
    if (state->frame_fence != VK_NULL_HANDLE && state->device != VK_NULL_HANDLE) {
        vkDestroyFence(state->device, state->frame_fence, NULL);
        state->frame_fence = VK_NULL_HANDLE;
    }
    if (state->empty_layout != VK_NULL_HANDLE && state->device != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(state->device, state->empty_layout, NULL);
        state->empty_layout = VK_NULL_HANDLE;
    }
    lagfx_vk_composite_shutdown(state);
    lagfx_vk_pipeline_shutdown(state);
    lagfx_vk_render_pass_shutdown(state);
    lagfx_vk_command_pool_destroy(state);
    if (state->device != VK_NULL_HANDLE) {
        vkDestroyDevice(state->device, NULL);
    }
    
    /* Destroy debug messenger before instance */
    if (state->instance != VK_NULL_HANDLE && state->debug_messenger != VK_NULL_HANDLE
        && state->destroy_debug_messenger_fn) {
        state->destroy_debug_messenger_fn(state->instance, state->debug_messenger, NULL);
        state->debug_messenger = VK_NULL_HANDLE;
    }
    
    if (state->instance != VK_NULL_HANDLE) {
        vkDestroyInstance(state->instance, NULL);
    }
    memset(state, 0, sizeof(*state));
    free(state);
}

#else  /* !LAGFX_HAVE_VULKAN -------------------------------------- */

/* No-vulkan build: provide stubs that keep the rest of the library
 * functional for scaffolding + lifecycle tests. */

lagfx_status_t lagfx_vk_init(struct lagfx_vk_state **out,
                             const lagfx_device_descriptor_t *desc) {
    (void)desc;
    if (!out) {
        return LAGFX_ERR_INVALID_ARG;
    }
    struct lagfx_vk_state *s = calloc(1, sizeof(*s));
    if (!s) {
        return LAGFX_ERR_OUT_OF_MEMORY;
    }
    s->initialized = false;
    LAGFX_LOG("vk_init: built without Vulkan — init is a no-op");
    *out = s;
    return LAGFX_OK;
}

void lagfx_vk_shutdown(struct lagfx_vk_state *state) {
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    free(state);
}

#endif /* LAGFX_HAVE_VULKAN */
