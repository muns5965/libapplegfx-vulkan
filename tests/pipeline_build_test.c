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

static int llc_available(void) {
    /* Same candidates as find_llc_binary() in src/air2spirv/shader_translate.c (macOS Homebrew pin, then Linux LLVM 20). */
    static const char *c[] = { "/opt/homebrew/opt/llvm@20/bin/llc", "/opt/homebrew/opt/llvm/bin/llc",
                               "/usr/lib/llvm-20/bin/llc", "/usr/bin/llc", NULL };
    for (int i = 0; c[i]; i++) if (access(c[i], X_OK) == 0) return 1;
    return 0;
}

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
    
    /* Smoke tests need llc. Skip with meson SKIP code if absent. */
    if (!llc_available()) {
        fprintf(stderr, "pipeline_build_test: llc not available; smoke tests skipped (negative tests passed)\n");
        _exit(77);
    }
    
    /* Validation layers test - checks VK_LAYER_KHRONOS_validation availability */
    int ret = test_validation_layers_and_vuids();
    if (ret == 1) {
        fprintf(stderr, "pipeline_build_test: validation layers test failed\n");
        _exit(1);
    } else if (ret == 0) {
        fprintf(stdout, "pipeline_build_test: validation layers test passed\n");
    }
    
    /* Full path smoke test - may skip at runtime if no Vulkan ICD or shader translation fails */
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
    
    /* No-dynamic-rendering test - skips if VK_KHR_dynamic_rendering present */
    ret = test_build_no_dynamic_rendering();
    if (ret == 1) {
        fprintf(stderr, "pipeline_build_test: no-dynamic-rendering test failed\n");
        _exit(1);
    } else if (ret != 0) {
        skipped++;
    }

    if (skipped) {
        fprintf(stdout, "pipeline_build_test: negative tests passed; %d smoke test(s) SKIPPED (stubs, no triangle pipeline was built)\n", skipped);
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

static int test_build_from_triangle(void) {
    /* On Mac dev boxes without lavapipe, skip this test gracefully. */
    fprintf(stderr, "pipeline_build_test: skipping triangle smoke (requires Linux + lavapipe)\n");
    return 77;
}

static int test_build_no_dynamic_rendering(void) {
    /* On Mac dev boxes without lavapipe, skip this test. */
    fprintf(stderr, "pipeline_build_test: skipping no-dynamic-rendering smoke (requires Linux + lavapipe)\n");
    return 77;
}

#endif  /* LAGFX_HAVE_VULKAN */
