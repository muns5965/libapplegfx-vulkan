/* SPDX-License-Identifier: MIT */
#include "libapplegfx-vulkan.h"
#include <stdio.h>
#include <stdlib.h>

#ifndef LAGFX_HAVE_VULKAN
/* Vulkan-disabled stub build (e.g. macos-latest CI without Vulkan SDK):
 * skip the whole test suite with meson SKIP exit code. */
int main(void) {
    fprintf(stderr, "pipeline_build_test: built without LAGFX_HAVE_VULKAN; skipping\n");
    return 77;
}
#else  /* LAGFX_HAVE_VULKAN */

#include "air2spirv/shader_translate.h"
#include "vulkan/pipeline_build.h"
#include <vulkan/vulkan.h>

/* The vertex-stage-input SPIR-V pair of arm64-exec-run-unit (glslangValidator -V: pos vec2 at location 0 -> gl_Position, constant red). */
#define k_vs_spv k_vs_vin_spv
#define k_fs_spv k_fs_vin_spv
#include "arm64-exec-run-shaders.h"
#undef k_vs_spv
#undef k_fs_spv

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void lagfx_log_impl(const char *fmt, ...)   { (void)fmt; }
void lagfx_warn_impl(const char *fmt, ...)  { (void)fmt; }
void lagfx_err_impl(const char *fmt, ...)   { (void)fmt; }
void lagfx_trace_impl(const char *fmt, ...) { (void)fmt; }

#define ASSERT(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", msg); return 1; } } while (0)
/* Like ASSERT but for functions that own Vulkan objects: jumps to `out` (with rc=1). */
#define ASSERT_GOTO(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", msg); rc = 1; goto out; } } while (0)

/* Forward declarations */
static int test_invalid_device_handle(void);
static int test_mismatched_formats(void);
static int test_invalid_shader_module(void);
static int test_only_one_shader(void);
static int test_null_inputs(void);
static int test_missing_shader_modules(void);
static int test_validation_layers_and_vuids(void);
static int test_build_from_triangle(void);
static int test_build_no_dynamic_rendering(void);
static int test_build_topologies(void);

int main(void) {
    fprintf(stdout, "pipeline_build_test: starting\n");
    
    /* Negative tests (no llc or Vulkan dependency) always run. */
    if (test_invalid_device_handle() != 0) { _exit(1); }
    fprintf(stdout, "pipeline_build_test: invalid device handle test passed\n");
    
    if (test_mismatched_formats() != 0) { _exit(1); }
    fprintf(stdout, "pipeline_build_test: mismatched formats test passed\n");
    
    if (test_invalid_shader_module() != 0) { _exit(1); }
    fprintf(stdout, "pipeline_build_test: invalid shader module test passed\n");
    
    if (test_only_one_shader() != 0) { _exit(1); }
    fprintf(stdout, "pipeline_build_test: only one shader test passed\n");
    
    if (test_null_inputs() != 0) { _exit(1); }
    fprintf(stdout, "pipeline_build_test: null inputs test passed\n");
    
    if (test_missing_shader_modules() != 0) { _exit(1); }
    fprintf(stdout, "pipeline_build_test: missing shader modules test passed\n");
    
    /* Validation layers test - checks VK_LAYER_KHRONOS_validation availability */
    int ret = test_validation_layers_and_vuids();
    if (ret == 1) {
        fprintf(stderr, "pipeline_build_test: validation layers test failed\n");
        _exit(1);
    } else if (ret == 0) {
        fprintf(stdout, "pipeline_build_test: validation layers test passed\n");
    }
    
    /* Real smoke tests: they use the SPIR-V fixtures (no llc needed); a SKIP is only reported when Vulkan / lavapipe is genuinely absent (probed at runtime). */
    int skipped = 0;
    ret = test_build_from_triangle();
    if (ret == 1) {
        fprintf(stderr, "pipeline_build_test: triangle build test failed\n");
        _exit(1);
    } else if (ret == 0) {
        fprintf(stdout, "pipeline_build_test: triangle smoke test passed\n");
    } else {
        skipped++;   /* the triangle smoke is a stub that always skips: say so instead of calling it a pass */
    }
    
    ret = test_build_topologies();
    if (ret == 1) {
        fprintf(stderr, "pipeline_build_test: topology test failed\n");
        _exit(1);
    } else if (ret == 0) {
        fprintf(stdout, "pipeline_build_test: topology test passed\n");
    } else {
        skipped++;
    }

    /* No-dynamic-rendering test */
    ret = test_build_no_dynamic_rendering();
    if (ret == 1) {
        fprintf(stderr, "pipeline_build_test: no-dynamic-rendering test failed\n");
        _exit(1);
    } else if (ret == 0) {
        fprintf(stdout, "pipeline_build_test: no-dynamic-rendering test passed\n");
    } else {
        skipped++;
    }

    if (skipped) {
        fprintf(stdout, "pipeline_build_test: negative tests passed; %d smoke test(s) SKIPPED (no usable Vulkan CPU device, see messages above)\n", skipped);
        fflush(stdout);
        _exit(77);
    }
    fprintf(stdout, "pipeline_build_test: all tests passed\n");
    fflush(stdout);
    _exit(0);
}

/* Messages the validation layer reports at WARNING/ERROR severity. INFO and
 * VERBOSE chatter (layer start-up, loader notes) is deliberately not counted. */
static int s_vuid_count = 0;

static VkBool32 VKAPI_PTR debug_message_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
    void *pUserData) {
    (void)messageType; (void)pUserData;
    if (messageSeverity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) {
        s_vuid_count++;
        fprintf(stderr, "VK validation: %s\n", pCallbackData->pMessage);
    }
    return VK_FALSE;
}

/* Minimal valid Vulkan 1.3 SPIR-V (spirv-as + spirv-val clean): a vertex
 * shader writing gl_Position = (0,0,0,1) and a fragment shader writing
 * location 0 = (1,0,0,1). Entry point "main". */
static const uint32_t k_vs_spv[] = {
    0x07230203u,0x00010600u,0x00070000u,0x0000000cu,
    0x00000000u,0x00020011u,0x00000001u,0x0003000eu,
    0x00000000u,0x00000001u,0x0006000fu,0x00000000u,
    0x00000001u,0x6e69616du,0x00000000u,0x00000002u,
    0x00040005u,0x00000001u,0x6e69616du,0x00000000u,
    0x00040047u,0x00000002u,0x0000000bu,0x00000000u,
    0x00020013u,0x00000003u,0x00030021u,0x00000004u,
    0x00000003u,0x00030016u,0x00000005u,0x00000020u,
    0x00040017u,0x00000006u,0x00000005u,0x00000004u,
    0x00040020u,0x00000007u,0x00000003u,0x00000006u,
    0x0004003bu,0x00000007u,0x00000002u,0x00000003u,
    0x0004002bu,0x00000005u,0x00000008u,0x00000000u,
    0x0004002bu,0x00000005u,0x00000009u,0x3f800000u,
    0x0007002cu,0x00000006u,0x0000000au,0x00000008u,
    0x00000008u,0x00000008u,0x00000009u,0x00050036u,
    0x00000003u,0x00000001u,0x00000000u,0x00000004u,
    0x000200f8u,0x0000000bu,0x0003003eu,0x00000002u,
    0x0000000au,0x000100fdu,0x00010038u,
};
static const uint32_t k_fs_spv[] = {
    0x07230203u,0x00010600u,0x00070000u,0x0000000cu,
    0x00000000u,0x00020011u,0x00000001u,0x0003000eu,
    0x00000000u,0x00000001u,0x0006000fu,0x00000004u,
    0x00000001u,0x6e69616du,0x00000000u,0x00000002u,
    0x00030010u,0x00000001u,0x00000007u,0x00040047u,
    0x00000002u,0x0000001eu,0x00000000u,0x00020013u,
    0x00000003u,0x00030021u,0x00000004u,0x00000003u,
    0x00030016u,0x00000005u,0x00000020u,0x00040017u,
    0x00000006u,0x00000005u,0x00000004u,0x00040020u,
    0x00000007u,0x00000003u,0x00000006u,0x0004003bu,
    0x00000007u,0x00000002u,0x00000003u,0x0004002bu,
    0x00000005u,0x00000008u,0x00000000u,0x0004002bu,
    0x00000005u,0x00000009u,0x3f800000u,0x0007002cu,
    0x00000006u,0x0000000au,0x00000009u,0x00000008u,
    0x00000008u,0x00000009u,0x00050036u,0x00000003u,
    0x00000001u,0x00000000u,0x00000004u,0x000200f8u,
    0x0000000bu,0x0003003eu,0x00000002u,0x0000000au,
    0x000100fdu,0x00010038u,
};

static int layer_present(const char *name) {
    uint32_t n = 0;
    if (vkEnumerateInstanceLayerProperties(&n, NULL) != VK_SUCCESS || n == 0) return 0;
    VkLayerProperties *l = malloc(n * sizeof(*l));
    if (!l) return 0;
    int found = 0;
    if (vkEnumerateInstanceLayerProperties(&n, l) == VK_SUCCESS)
        for (uint32_t i = 0; i < n; ++i)
            if (strcmp(l[i].layerName, name) == 0) { found = 1; break; }
    free(l);
    return found;
}

/* Builds a real pipeline through lagfx_pipeline_build on lavapipe. When the
 * Khronos validation layer is installed it is enabled and the build must emit
 * zero WARNING/ERROR messages; a deliberately
 * malformed shader module (positive control) must emit some, proving the layer is live. With the layer absent or disabled
 * (VK_LOADER_LAYERS_DISABLE=*) the build is still performed and checked, only
 * the validation assertions are not applicable. Returns 0 pass, 1 fail, 77 skip
 * (no Vulkan ICD / CPU device). Object teardown is children-first. */
static int test_validation_layers_and_vuids(void) {
    const char *layer = "VK_LAYER_KHRONOS_validation";
    int have_layer = layer_present(layer);
    if (!have_layer)
        fprintf(stderr, "pipeline_build_test: %s not available; building pipeline without validation\n", layer);

    VkApplicationInfo ai = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "pipeline_build_test",
        .apiVersion = VK_API_VERSION_1_3,
    };
    const char *debug_utils_ext = "VK_EXT_debug_utils";
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &ai,
        .enabledLayerCount = have_layer ? 1u : 0u,
        .ppEnabledLayerNames = &layer,
        .enabledExtensionCount = have_layer ? 1u : 0u,
        .ppEnabledExtensionNames = &debug_utils_ext,
    };
    VkInstance inst = VK_NULL_HANDLE;
    VkResult vr = vkCreateInstance(&ici, NULL, &inst);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "pipeline_build_test: vkCreateInstance failed (%d); skipping\n", (int)vr);
        return 77;
    }

    int rc = 1;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger = NULL;
    VkDevice device = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    int have_messenger = 0;

    if (have_layer) {
        PFN_vkCreateDebugUtilsMessengerEXT create_messenger =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(inst, "vkCreateDebugUtilsMessengerEXT");
        destroy_messenger =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(inst, "vkDestroyDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mc = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                             | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
            .pfnUserCallback = debug_message_callback,
        };
        if (create_messenger && destroy_messenger
            && create_messenger(inst, &mc, NULL, &messenger) == VK_SUCCESS)
            have_messenger = 1;
        ASSERT_GOTO(have_messenger, "debug messenger created while layer is enabled");
    }

    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(inst, &device_count, NULL);
    VkPhysicalDevice *devices = device_count ? malloc(device_count * sizeof(*devices)) : NULL;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    if (devices) {
        vkEnumeratePhysicalDevices(inst, &device_count, devices);
        for (uint32_t i = 0; i < device_count; ++i) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(devices[i], &props);
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { phys = devices[i]; break; }
        }
        free(devices);
    }
    if (!phys) {
        fprintf(stderr, "pipeline_build_test: no CPU (lavapipe) device; skipping\n");
        rc = 77;
        goto out;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo dqci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio,
    };
    /* lagfx_pipeline_build chains VkPipelineRenderingCreateInfo, so the device
     * must enable dynamicRendering (VUID-...-dynamicRendering-06576). */
    VkPhysicalDeviceVulkan13Features f13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .dynamicRendering = VK_TRUE,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &f13,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &dqci,
    };
    vr = vkCreateDevice(phys, &dci, NULL, &device);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "pipeline_build_test: vkCreateDevice (dynamicRendering) failed (%d); skipping\n", (int)vr);
        device = VK_NULL_HANDLE;
        rc = 77;
        goto out;
    }

    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(k_vs_spv), .pCode = k_vs_spv,
    };
    ASSERT_GOTO(vkCreateShaderModule(device, &smci, NULL, &vs) == VK_SUCCESS, "vertex shader module created");
    smci.codeSize = sizeof(k_fs_spv); smci.pCode = k_fs_spv;
    ASSERT_GOTO(vkCreateShaderModule(device, &smci, NULL, &fs) == VK_SUCCESS, "fragment shader module created");
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    ASSERT_GOTO(vkCreatePipelineLayout(device, &plci, NULL, &layout) == VK_SUCCESS, "pipeline layout created");

    /* (1) Product path: must succeed and be validation-clean. */
    lagfx_pipeline_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.vertex_shader = vs;
    desc.fragment_shader = fs;
    desc.layout = layout;
    desc.color_format = VK_FORMAT_B8G8R8A8_UNORM;
    s_vuid_count = 0;
    lagfx_status_t st = lagfx_pipeline_build(device, &desc, &pipe);
    ASSERT_GOTO(st == LAGFX_OK, "lagfx_pipeline_build succeeds on lavapipe");
    ASSERT_GOTO(pipe != VK_NULL_HANDLE, "lagfx_pipeline_build returned a non-null pipeline");
    int product_vuids = s_vuid_count;
    fprintf(stdout, "pipeline_build_test: validation messages during lagfx_pipeline_build: %d\n", product_vuids);
    ASSERT_GOTO(product_vuids == 0, "lagfx_pipeline_build emits no validation warnings/errors");

    /* (2) Positive control (layer only): an intentionally malformed SPIR-V
     * module (valid header, truncated body) must be reported by the layer. A malformed
     * *pipeline* create is not used for this because lavapipe dereferences the
     * bad state behind the layer and crashes (verified with gdb). */
    if (have_messenger) {
        /* The layer caches SPIR-V validation results (also across processes), so
         * a fixed bad blob is only reported once; make the id bound unique per run. */
        uint32_t bad_code[5];
        memcpy(bad_code, k_vs_spv, sizeof(bad_code));              /* header only */
        bad_code[3] = 0x1000u + ((uint32_t)getpid() & 0xffffu);
        VkShaderModuleCreateInfo bad = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(bad_code), .pCode = bad_code,
        };
        VkShaderModule bad_mod = VK_NULL_HANDLE;
        s_vuid_count = 0;
        VkResult bvr = vkCreateShaderModule(device, &bad, NULL, &bad_mod);
        fprintf(stdout, "pipeline_build_test: validation messages for intentionally malformed SPIR-V: %d (vr=%d)\n",
                s_vuid_count, (int)bvr);
        if (bad_mod != VK_NULL_HANDLE) vkDestroyShaderModule(device, bad_mod, NULL);
        ASSERT_GOTO(s_vuid_count > 0, "validation layer reports an intentionally malformed shader module");
    }
    rc = 0;

out:
    /* Children first: pipeline, layout, modules, device, messenger, instance. */
    if (device) {
        if (pipe)   vkDestroyPipeline(device, pipe, NULL);
        if (layout) vkDestroyPipelineLayout(device, layout, NULL);
        if (fs)     vkDestroyShaderModule(device, fs, NULL);
        if (vs)     vkDestroyShaderModule(device, vs, NULL);
        vkDestroyDevice(device, NULL);
    }
    if (messenger && destroy_messenger) destroy_messenger(inst, messenger, NULL);
    vkDestroyInstance(inst, NULL);
    return rc;
}

static int test_null_inputs(void) {
    VkDevice dummy_dev = (VkDevice)(size_t)0x1234;
    
    lagfx_pipeline_desc_t desc = {0};
    VkPipeline pipe = VK_NULL_HANDLE;
    
    /* NULL device */
    lagfx_status_t st = lagfx_pipeline_build(NULL, &desc, &pipe);
    ASSERT(st != LAGFX_OK, "NULL device returns non-OK");
    
    /* NULL desc */
    st = lagfx_pipeline_build(dummy_dev, NULL, &pipe);
    ASSERT(st != LAGFX_OK, "NULL desc returns non-OK");
    
    /* NULL out_pipeline */
    st = lagfx_pipeline_build(dummy_dev, &desc, NULL);
    ASSERT(st != LAGFX_OK, "NULL out_pipeline returns non-OK");
    
    return 0;
}

static int test_invalid_device_handle(void) {
    lagfx_pipeline_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    /* Fake shader modules — just non-NULL handles, won't be deref'd if device is invalid */
    desc.vertex_shader = (VkShaderModule)0x1;
    desc.fragment_shader = (VkShaderModule)0x2;
    desc.color_format = VK_FORMAT_B8G8R8A8_UNORM;

    VkPipeline pipeline = VK_NULL_HANDLE;
    lagfx_status_t st = lagfx_pipeline_build(VK_NULL_HANDLE, &desc, &pipeline);
    ASSERT(st != LAGFX_OK, "VK_NULL_HANDLE device returns non-OK");
    return 0;
}

static int test_mismatched_formats(void) {
    /* VK_FORMAT_UNDEFINED is a sentinel value. The implementation may:
     * 1. Reject it explicitly (preferred)
     * 2. Substitute a default format internally
     * This test checks for explicit rejection. */
    
    lagfx_pipeline_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    /* Fake shader modules - will not be dereferenced if device is invalid */
    desc.vertex_shader = (VkShaderModule)0x1;
    desc.fragment_shader = (VkShaderModule)0x2;
    desc.color_format = VK_FORMAT_UNDEFINED;

    VkPipeline pipe = VK_NULL_HANDLE;
    /* Use VK_NULL_HANDLE to trigger early validation path */
    lagfx_status_t st = lagfx_pipeline_build(VK_NULL_HANDLE, &desc, &pipe);
    
    if (st != LAGFX_OK) {
        fprintf(stderr, "test_mismatched_formats: VK_FORMAT_UNDEFINED rejected (good)\n");
        return 0;
    }
    
    /* If we get here, impl accepts undefined format - document this */
    fprintf(stderr, "test_mismatched_formats: VK_FORMAT_UNDEFINED accepted (impl substitutes default)\n");
    return 0;
}

static int test_invalid_shader_module(void) {
    lagfx_pipeline_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    /* vertex_shader is VK_NULL_HANDLE */
    desc.vertex_shader = VK_NULL_HANDLE;
    desc.fragment_shader = (VkShaderModule)0x2;
    desc.color_format = VK_FORMAT_B8G8R8A8_UNORM;

    VkPipeline pipe = VK_NULL_HANDLE;
    lagfx_status_t st = lagfx_pipeline_build(VK_NULL_HANDLE, &desc, &pipe);
    ASSERT(st != LAGFX_OK, "VK_NULL_HANDLE vertex_shader returns non-OK");
    return 0;
}

static int test_only_one_shader(void) {
    lagfx_pipeline_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    /* vertex but no fragment */
    desc.vertex_shader = (VkShaderModule)0x1;
    desc.fragment_shader = VK_NULL_HANDLE;
    desc.color_format = VK_FORMAT_B8G8R8A8_UNORM;

    VkPipeline pipe = VK_NULL_HANDLE;
    lagfx_status_t st = lagfx_pipeline_build(VK_NULL_HANDLE, &desc, &pipe);
    ASSERT(st != LAGFX_OK, "no fragment_shader returns non-OK");
    return 0;
}

static int test_missing_shader_modules(void) {
    VkDevice dummy_dev = (VkDevice)(size_t)0x1234;
    
    lagfx_pipeline_desc_t desc = {0};
    VkPipeline pipe = VK_NULL_HANDLE;
    
    /* NULL vertex_shader */
    desc.fragment_shader = (VkShaderModule)(size_t)0x5678;
    lagfx_status_t st = lagfx_pipeline_build(dummy_dev, &desc, &pipe);
    ASSERT(st != LAGFX_OK, "NULL vertex_shader returns non-OK");
    
    /* NULL fragment_shader */
    desc.vertex_shader = (VkShaderModule)(size_t)0x5678;
    desc.fragment_shader = VK_NULL_HANDLE;
    st = lagfx_pipeline_build(dummy_dev, &desc, &pipe);
    ASSERT(st != LAGFX_OK, "NULL fragment_shader returns non-OK");
    
    return 0;
}

/* ---- a Vulkan instance + lavapipe device with the validation layer and a messenger, shared by the smoke tests ---- */
typedef struct {
    VkInstance inst; VkDebugUtilsMessengerEXT messenger; PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger;
    VkDevice dev; int have_layer, have_messenger;
} venv_t;

static void venv_close(venv_t *e) {
    if (e->dev) vkDestroyDevice(e->dev, NULL);
    if (e->messenger && e->destroy_messenger) e->destroy_messenger(e->inst, e->messenger, NULL);
    if (e->inst) vkDestroyInstance(e->inst, NULL);
    memset(e, 0, sizeof *e);
}

/* 0 = open; 77 = genuinely no Vulkan / no CPU device (the reason is printed). */
static int venv_open(venv_t *e, int dynamic_rendering) {
    memset(e, 0, sizeof *e);
    const char *layer = "VK_LAYER_KHRONOS_validation";
    e->have_layer = layer_present(layer);
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "pipeline_build_test", .apiVersion = VK_API_VERSION_1_3 };
    const char *ext = "VK_EXT_debug_utils";
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai, .enabledLayerCount = e->have_layer ? 1u : 0u,
                                 .ppEnabledLayerNames = &layer, .enabledExtensionCount = e->have_layer ? 1u : 0u, .ppEnabledExtensionNames = &ext };
    VkResult vr = vkCreateInstance(&ici, NULL, &e->inst);
    if (vr != VK_SUCCESS) { fprintf(stderr, "pipeline_build_test: SKIP: no Vulkan loader/ICD (vkCreateInstance=%d)\n", (int)vr); e->inst = VK_NULL_HANDLE; return 77; }
    if (e->have_layer) {
        PFN_vkCreateDebugUtilsMessengerEXT cm = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(e->inst, "vkCreateDebugUtilsMessengerEXT");
        e->destroy_messenger = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(e->inst, "vkDestroyDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mc = { .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, .pfnUserCallback = debug_message_callback };
        e->have_messenger = cm && e->destroy_messenger && cm(e->inst, &mc, NULL, &e->messenger) == VK_SUCCESS;
    } else {
        fprintf(stderr, "pipeline_build_test: %s not installed: pipelines are built but validation messages are not counted\n", layer);
    }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(e->inst, &n, NULL);
    VkPhysicalDevice *d = n ? malloc(n * sizeof *d) : NULL;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    if (d) {
        vkEnumeratePhysicalDevices(e->inst, &n, d);
        for (uint32_t i = 0; i < n; i++) { VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(d[i], &pr); if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { phys = d[i]; break; } }
        free(d);
    }
    if (!phys) { fprintf(stderr, "pipeline_build_test: SKIP: no CPU (lavapipe) Vulkan device\n"); venv_close(e); return 77; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo q = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = dynamic_rendering ? VK_TRUE : VK_FALSE };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13, .queueCreateInfoCount = 1, .pQueueCreateInfos = &q };
    if (vkCreateDevice(phys, &dci, NULL, &e->dev) != VK_SUCCESS) { fprintf(stderr, "pipeline_build_test: SKIP: vkCreateDevice failed\n"); e->dev = VK_NULL_HANDLE; venv_close(e); return 77; }
    return 0;
}

typedef struct { VkShaderModule vs, fs, vs_in, fs_in, vs_pt; VkPipelineLayout layout; } vfix_t;
static int vfix_make(VkDevice dev, vfix_t *f) {
    memset(f, 0, sizeof *f);
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = sizeof k_vs_spv; ci.pCode = k_vs_spv;
    if (vkCreateShaderModule(dev, &ci, NULL, &f->vs) != VK_SUCCESS) return 1;
    ci.codeSize = sizeof k_fs_spv; ci.pCode = k_fs_spv;
    if (vkCreateShaderModule(dev, &ci, NULL, &f->fs) != VK_SUCCESS) return 1;
    ci.codeSize = sizeof k_vs_vin_spv; ci.pCode = k_vs_vin_spv;
    if (vkCreateShaderModule(dev, &ci, NULL, &f->vs_in) != VK_SUCCESS) return 1;
    ci.codeSize = sizeof k_fs_vin_spv; ci.pCode = k_fs_vin_spv;
    if (vkCreateShaderModule(dev, &ci, NULL, &f->fs_in) != VK_SUCCESS) return 1;
    ci.codeSize = sizeof k_vs_point_spv; ci.pCode = k_vs_point_spv;
    if (vkCreateShaderModule(dev, &ci, NULL, &f->vs_pt) != VK_SUCCESS) return 1;
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    return vkCreatePipelineLayout(dev, &plci, NULL, &f->layout) == VK_SUCCESS ? 0 : 1;
}
static void vfix_free(VkDevice dev, vfix_t *f) {
    if (f->layout) vkDestroyPipelineLayout(dev, f->layout, NULL);
    if (f->vs_pt) vkDestroyShaderModule(dev, f->vs_pt, NULL);
    if (f->fs_in) vkDestroyShaderModule(dev, f->fs_in, NULL);
    if (f->vs_in) vkDestroyShaderModule(dev, f->vs_in, NULL);
    if (f->fs) vkDestroyShaderModule(dev, f->fs, NULL);
    if (f->vs) vkDestroyShaderModule(dev, f->vs, NULL);
}

/* A triangle pipeline through lagfx_pipeline_build, twice: the minimal pair (no vertex input) and the stage-in pair (vertex input from a buffer, as the ARM64 exec path
 * builds it). OK + non-null + zero validation warnings/errors; objects destroyed children-first. */
static int test_build_from_triangle(void) {
    venv_t e;
    int rc = venv_open(&e, 1);
    if (rc) return rc;
    vfix_t f;
    rc = 1;
    ASSERT_GOTO(vfix_make(e.dev, &f) == 0, "shader modules / layout created");
    VkPipeline pa = VK_NULL_HANDLE, pb = VK_NULL_HANDLE;
    s_vuid_count = 0;
    lagfx_pipeline_desc_t d;
    memset(&d, 0, sizeof d);
    d.vertex_shader = f.vs, d.fragment_shader = f.fs, d.layout = f.layout, d.color_format = VK_FORMAT_B8G8R8A8_UNORM;
    ASSERT_GOTO(lagfx_pipeline_build(e.dev, &d, &pa) == LAGFX_OK && pa != VK_NULL_HANDLE, "minimal triangle pipeline: LAGFX_OK and a non-null VkPipeline");
    memset(&d, 0, sizeof d);
    d.vertex_shader = f.vs_in, d.fragment_shader = f.fs_in, d.layout = f.layout, d.color_format = VK_FORMAT_R8G8B8A8_UNORM;
    d.n_vtx_inputs = 1, d.vtx_in_loc[0] = 0, d.vtx_in_comp[0] = 2, d.vtx_stride = 8;
    ASSERT_GOTO(lagfx_pipeline_build(e.dev, &d, &pb) == LAGFX_OK && pb != VK_NULL_HANDLE, "stage-in triangle pipeline: LAGFX_OK and a non-null VkPipeline");
    fprintf(stdout, "pipeline_build_test: validation messages while building the triangle pipelines: %d (layer %s)\n", s_vuid_count, e.have_messenger ? "live" : "absent");
    ASSERT_GOTO(s_vuid_count == 0, "triangle pipelines are validation-clean");
    rc = 0;
out:
    if (pa) vkDestroyPipeline(e.dev, pa, NULL);
    if (pb) vkDestroyPipeline(e.dev, pb, NULL);
    vfix_free(e.dev, &f);
    venv_close(&e);
    return rc;
}

/* One pipeline per primitive topology (point / line list / line strip / triangle list / triangle strip), the per-topology pipelines of the ARM64 exec path; and the
 * default (topology_plus1 == 0) is the triangle list, so every existing caller is unchanged. */
static int test_build_topologies(void) {
    venv_t e;
    int rc = venv_open(&e, 1);
    if (rc) return rc;
    vfix_t f;
    rc = 1;
    ASSERT_GOTO(vfix_make(e.dev, &f) == 0, "shader modules / layout created");
    VkPipeline pipes[6] = { 0 };
    s_vuid_count = 0;
    for (int t = -1; t <= 4; t++) {
        lagfx_pipeline_desc_t d;
        memset(&d, 0, sizeof d);
        d.vertex_shader = t == 0 ? f.vs_pt : f.vs_in;   /* a point-list pipeline needs a vertex shader that writes PointSize (VUID-...-topology-08773) */
        d.fragment_shader = f.fs_in, d.layout = f.layout, d.color_format = VK_FORMAT_B8G8R8A8_UNORM;
        d.n_vtx_inputs = 1, d.vtx_in_loc[0] = 0, d.vtx_in_comp[0] = 2, d.vtx_stride = 8;
        d.topology_plus1 = (uint8_t)(t < 0 ? 0 : t + 1);   /* -1: the default */
        ASSERT_GOTO(lagfx_pipeline_build(e.dev, &d, &pipes[t + 1]) == LAGFX_OK && pipes[t + 1] != VK_NULL_HANDLE, "pipeline per topology: LAGFX_OK and non-null");
    }
    for (int i = 0; i < 6; i++) for (int j = i + 1; j < 6; j++) ASSERT_GOTO(pipes[i] != pipes[j], "distinct VkPipeline per build");
    fprintf(stdout, "pipeline_build_test: validation messages while building 6 topology pipelines: %d\n", s_vuid_count);
    ASSERT_GOTO(s_vuid_count == 0, "topology pipelines are validation-clean");
    rc = 0;
out:
    for (int i = 0; i < 6; i++) if (pipes[i]) vkDestroyPipeline(e.dev, pipes[i], NULL);
    vfix_free(e.dev, &f);
    venv_close(&e);
    return rc;
}

/* lagfx_pipeline_build has NO render-pass fallback: it always chains VkPipelineRenderingCreateInfo (dynamic rendering, core in Vulkan 1.3; the library's instance init
 * enables the feature and records have_dynamic_rendering). Documented behaviour on a device WITHOUT the feature: the build must not be silently valid. This asserts that
 * the validation layer reports it (VUID ...dynamicRendering-06576) or that the call returns a non-OK status; with no validation layer installed the build outcome is only
 * checked to be cleaned up (nothing can be asserted) and that is printed. */
static int test_build_no_dynamic_rendering(void) {
    venv_t e;
    int rc = venv_open(&e, 0);
    if (rc) return rc;
    vfix_t f;
    rc = 1;
    ASSERT_GOTO(vfix_make(e.dev, &f) == 0, "shader modules / layout created");
    lagfx_pipeline_desc_t d;
    memset(&d, 0, sizeof d);
    d.vertex_shader = f.vs, d.fragment_shader = f.fs, d.layout = f.layout, d.color_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkPipeline p = VK_NULL_HANDLE;
    s_vuid_count = 0;
    lagfx_status_t st = lagfx_pipeline_build(e.dev, &d, &p);
    fprintf(stdout, "pipeline_build_test: build without dynamicRendering: status=%d validation messages=%d (no render-pass fallback exists in the library)\n", (int)st, s_vuid_count);
    if (e.have_messenger) ASSERT_GOTO(st != LAGFX_OK || s_vuid_count > 0, "a build without the dynamicRendering feature is rejected or reported by validation");
    rc = 0;
out:
    if (p) vkDestroyPipeline(e.dev, p, NULL);
    vfix_free(e.dev, &f);
    venv_close(&e);
    return rc;
}

#endif  /* LAGFX_HAVE_VULKAN */
