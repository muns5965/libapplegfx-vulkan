/*
 * libapplegfx-vulkan — device capabilities the generated SPIR-V relies on (Adreno and other mobile ICDs report several of them as optional).
 * src/vulkan/caps.h. Pure C, no Vulkan headers needed.
 *
 * The AIR->SPIR-V translator emits Int64 / Int16 / Int8 / Float16 / 8- and 16-bit storage / VariablePointers capabilities and (with Vulkan 1.2) SPIR-V 1.4.
 * lagfx_vk_init enables ONLY what vkGetPhysicalDeviceFeatures2 (+ Vulkan11/12 feature structs) reports and records it in vk->caps_enabled / caps_missing.
 * The pipeline resolver checks every translated module with lagfx_spv_required_caps(): a module that needs a capability that is not enabled is NOT turned
 * into a shader module / pipeline (that would be an invalid Vulkan call); it is counted (stub_missing_cap_<name>) and the draw is skipped.
 */
#ifndef LAGFX_VULKAN_CAPS_H
#define LAGFX_VULKAN_CAPS_H

#include <stddef.h>
#include <stdint.h>

enum {
    LAGFX_VK_CAP_INT64 = 0, LAGFX_VK_CAP_INT16, LAGFX_VK_CAP_INT8, LAGFX_VK_CAP_FLOAT16,
    /* Each Vulkan feature on its own (they are separate device features; a driver may have one without the others): */
    LAGFX_VK_CAP_SB8,            /* StorageBuffer8BitAccess            <- storageBuffer8BitAccess */
    LAGFX_VK_CAP_UB8,            /* UniformAndStorageBuffer8BitAccess  <- uniformAndStorageBuffer8BitAccess */
    LAGFX_VK_CAP_PC8,            /* StoragePushConstant8               <- storagePushConstant8 */
    LAGFX_VK_CAP_SB16,           /* StorageBuffer16BitAccess           <- storageBuffer16BitAccess */
    LAGFX_VK_CAP_UB16,           /* UniformAndStorageBuffer16BitAccess <- uniformAndStorageBuffer16BitAccess */
    LAGFX_VK_CAP_PC16,           /* StoragePushConstant16              <- storagePushConstant16 */
    LAGFX_VK_CAP_IO16,           /* StorageInputOutput16               <- storageInputOutput16 */
    LAGFX_VK_CAP_VARPTR_SB,      /* VariablePointersStorageBuffer */
    LAGFX_VK_CAP_VARPTR,         /* VariablePointers */
    LAGFX_VK_CAP_SPIRV14,        /* module version > 1.3 */
    LAGFX_VK_CAP_SUBGROUP,       /* GroupNonUniform (SubgroupSize / SubgroupLocalInvocationId builtins)  <- VkPhysicalDeviceVulkan11Properties::subgroupSupportedOperations BASIC + COMPUTE stage */
    LAGFX_VK_CAP_SUBGROUP_SHUFFLE, /* GroupNonUniformShuffle (simd_shuffle*)                              <- subgroupSupportedOperations SHUFFLE + COMPUTE stage */
    LAGFX_VK_CAP_SUBGROUP_SHUFFLE_RELATIVE, /* GroupNonUniformShuffleRelative (shuffle_up/down)                  <- subgroupSupportedOperations SHUFFLE_RELATIVE + COMPUTE stage (a separate Vulkan feature bit) */
    LAGFX_VK_CAP_DEMOTE,         /* DemoteToHelperInvocation (OpDemoteToHelperInvocation: air.discard_fragment)    <- VkPhysicalDeviceVulkan13Features::shaderDemoteToHelperInvocation (VK_EXT_shader_demote_to_helper_invocation) */
    /* Atomics. SPIR-V has ONE capability per type (Int64Atomics, AtomicFloat32AddEXT) but Vulkan gates them per storage class, so the caps below follow the Vulkan features and the
     * scan (lagfx_spv_required_caps_ex) attributes each use to its pointer's storage class (StorageBuffer -> *_BUF, Workgroup -> *_SHARED, Image -> *_IMAGE); a capability declared
     * with no attributable use is charged to every class (fail closed). 32-bit integer atomics need no capability. */
    LAGFX_VK_CAP_ATOMIC64_BUF,        /* Int64Atomics on StorageBuffer (device memory: air.atomic.global.*.i64)  <- VkPhysicalDeviceVulkan12Features::shaderBufferInt64Atomics AND shaderInt64 */
    LAGFX_VK_CAP_ATOMIC64_SHARED,     /* Int64Atomics on Workgroup memory (air.atomic.local.*.i64)              <- shaderSharedInt64Atomics AND shaderInt64 */
    LAGFX_VK_CAP_ATOMIC_F32_BUF,      /* float32 atomic load / store / exchange on StorageBuffer   <- VK_EXT_shader_atomic_float shaderBufferFloat32Atomics (no use emitted: float load / store / exchange are not implemented) */
    LAGFX_VK_CAP_ATOMIC_F32_BUF_ADD,  /* AtomicFloat32AddEXT (+ SPV_EXT_shader_atomic_float_add) on StorageBuffer <- shaderBufferFloat32AtomicAdd */
    LAGFX_VK_CAP_ATOMIC_F32_SHARED,   /* float32 atomic load / store / exchange on Workgroup memory <- shaderSharedFloat32Atomics (no use emitted) */
    LAGFX_VK_CAP_ATOMIC_F32_SHARED_ADD, /* AtomicFloat32AddEXT on Workgroup memory <- shaderSharedFloat32AtomicAdd */
    LAGFX_VK_CAP_ATOMIC_F32_IMAGE,    /* float32 atomics on storage images <- shaderImageFloat32Atomics (no use emitted: texture atomics are not implemented) */
    LAGFX_VK_CAP_COUNT
};
#define LAGFX_VK_CAP_BIT(c) (1u << (c))
#define LAGFX_VK_CAP_ALL    ((1u << LAGFX_VK_CAP_COUNT) - 1u)

const char *lagfx_vk_cap_name(unsigned cap);                          /* "int64", "int16", ... ("?" out of range) */
unsigned    lagfx_vk_caps_parse(const char *csv);                    /* "int64,int8" -> mask; unknown names ignored */

/* Mask of LAGFX_VK_CAP_BIT()s the SPIR-V module declares (OpCapability scan + version word). 0 for a module that is too short / has no magic number. */
uint32_t lagfx_spv_required_caps(const void *spv, size_t bytes);
#include <stdbool.h>
/* Fail-closed form: returns false when the module could not be analysed (bad magic, truncated, id outside the bound, bound too large, allocation failure,
 * types nested too deep); *mask is then LAGFX_VK_CAP_ALL so a gate that ANDs it with the enabled set rejects the module. */
bool lagfx_spv_required_caps_ex(const void *spv, size_t bytes, uint32_t *mask);

/* Declares (OpCapability, plus OpExtension SPV_KHR_8bit_storage below SPIR-V 1.5) the 8/16-bit storage capabilities the module USES but does not declare, so a
 * device that has the feature accepts it. *spv is a malloc'd blob and may be replaced (realloc'd). Returns false only on allocation failure (blob unchanged). */
#include <stdbool.h>
bool lagfx_spv_declare_used_caps(uint8_t **spv, size_t *bytes);

/* Device-feature inputs of the atomic capabilities (what lagfx_vk_init reads from VkPhysicalDeviceFeatures::shaderInt64, VkPhysicalDeviceVulkan12Features::shaderBufferInt64Atomics /
 * shaderSharedInt64Atomics and VkPhysicalDeviceShaderAtomicFloatFeaturesEXT). A plain struct so a unit test can fake any device. af_ext: VK_EXT_shader_atomic_float is advertised (the
 * float fields only count then). */
typedef struct {
    uint8_t shader_int64, buffer_int64_atomics, shared_int64_atomics;
    uint8_t af_ext, buffer_f32, buffer_f32_add, shared_f32, shared_f32_add, image_f32;
} lagfx_vk_atomic_feat_t;
/* TEST INJECTION (set by tests only, never from env or guest data): when non-NULL, lagfx_vk_init ANDs the probed features with this struct before it decides the caps and the device
 * features, so it can only turn features off. */
extern const lagfx_vk_atomic_feat_t *lagfx_vk_atomic_feat_inject;
/* The LAGFX_VK_CAP_BIT()s of the atomic capabilities a device with these features offers; the rule lagfx_vk_init applies. 64-bit atomics need shaderInt64 in addition. The per-cap
 * refusal counters are the pipeline gate's stub_missing_cap_<name> (need & ~enabled), one per missing cap. */
uint32_t lagfx_vk_atomic_caps(const lagfx_vk_atomic_feat_t *f);
/* The mask of the atomic capability bits (everything lagfx_vk_atomic_caps can return). */
#define LAGFX_VK_CAPS_ATOMIC_ALL (LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC64_BUF) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC64_SHARED) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_BUF) | \
    LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_BUF_ADD) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_SHARED) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_SHARED_ADD) | LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_ATOMIC_F32_IMAGE))

#endif
