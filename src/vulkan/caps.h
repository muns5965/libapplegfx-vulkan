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
    LAGFX_VK_CAP_STORAGE8,       /* StorageBuffer8BitAccess / UniformAndStorageBuffer8BitAccess / StoragePushConstant8 */
    LAGFX_VK_CAP_STORAGE16,      /* StorageBuffer16BitAccess / UniformAndStorageBuffer16BitAccess / StoragePushConstant16 / StorageInputOutput16 */
    LAGFX_VK_CAP_VARPTR_SB,      /* VariablePointersStorageBuffer */
    LAGFX_VK_CAP_VARPTR,         /* VariablePointers */
    LAGFX_VK_CAP_SPIRV14,        /* module version > 1.3 */
    LAGFX_VK_CAP_COUNT
};
#define LAGFX_VK_CAP_BIT(c) (1u << (c))
#define LAGFX_VK_CAP_ALL    ((1u << LAGFX_VK_CAP_COUNT) - 1u)

const char *lagfx_vk_cap_name(unsigned cap);                          /* "int64", "int16", ... ("?" out of range) */
unsigned    lagfx_vk_caps_parse(const char *csv);                    /* "int64,int8" -> mask; unknown names ignored */

/* Mask of LAGFX_VK_CAP_BIT()s the SPIR-V module declares (OpCapability scan + version word). 0 for a module that is too short / has no magic number. */
uint32_t lagfx_spv_required_caps(const void *spv, size_t bytes);

#endif
