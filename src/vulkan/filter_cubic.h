/*
 * libapplegfx-vulkan — opt-in cubic (bicubic) texture filtering: the selection logic
 * src/vulkan/filter_cubic.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * WHAT THIS IS, AND IS NOT. A Metal sampler state word whose min / mag filter field holds 2 (a constexpr word: bits 9..10 / 11..12; a runtime serialized state: bits [1:0] / [3:2])
 * is outside MTLSamplerMinMagFilter (nearest = 0, linear = 1: developer.apple.com MTLSamplerMinMagFilter; Metal Shading Language Specification Table 2.7: `filter`, `mag_filter`,
 * `min_filter` take nearest | linear only) [V]. That 2 means "bicubic" is an inference from the field layout of an external decoder [I]; no capture of ours contains a
 * filter field above 1 (179 runtime samplers of the device dump of run 231501: min / mag in {0, 1}) [N]. The default therefore stays: map the value to LINEAR and count it
 * (ext_samp_filter_unknown, cxs_state_builds_bicubic_as_linear).
 *
 * The opt-in: with LAGFX_CUBIC=1 in the environment the device enables VK_EXT_filter_cubic (VK_IMG_filter_cubic on a device that only has the older alias; the extensions add
 * VK_FILTER_CUBIC_EXT and the format feature SAMPLED_IMAGE_FILTER_CUBIC) and a sampler whose filter field is 2 is created with VK_FILTER_CUBIC_EXT for that filter -- at the
 * moment of a draw, when every condition below holds; otherwise LINEAR is used and the reason is counted. Which of the draw's samplers is paired with which texture is the
 * shader's business, so the conditions are checked for EVERY sampled texture of the sampler's stage (conservative).
 *
 * NOT verified (and not claimed): that Metal would filter such a sampler bicubically at all; what kernel Metal would use (B-spline, Catmull-Rom, Mitchell: Vulkan's cubic is
 * implementation defined within the extension's description); that the Adreno 840 driver's cubic result matches anything; the view-type / anisotropy / unnormalized
 * restrictions beyond the conservative ones coded here (VK_EXT_filter_cubic also has a per image-format-and-view-type query, VkFilterCubicImageViewImageFormatPropertiesEXT,
 * that is not used: the format feature bit plus a 2D / 2D-array view is what is checked).
 */
#ifndef LIBAPPLEGFX_VULKAN_FILTER_CUBIC_H
#define LIBAPPLEGFX_VULKAN_FILTER_CUBIC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef LAGFX_HAVE_VULKAN
#include <vulkan/vulkan.h>

/* VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_CUBIC_BIT_EXT (= ..._BIT_IMG), spelled out so older headers build. */
#define LAGFX_FMT_FEATURE_FILTER_CUBIC 0x00002000u
/* VK_FILTER_CUBIC_EXT (= VK_FILTER_CUBIC_IMG). */
#define LAGFX_FILTER_CUBIC ((VkFilter)1000015000)

/* Why a cubic request was (not) honoured. LAGFX_CUBIC_USE = it was; everything else = LINEAR was used instead, and the exec counts the reason. */
typedef enum {
    LAGFX_CUBIC_USE = 0,
    LAGFX_CUBIC_NO_OPTIN,      /* LAGFX_CUBIC=1 not set: the default behaviour */
    LAGFX_CUBIC_NO_EXT,        /* the device does not have (or did not enable) VK_EXT_filter_cubic / VK_IMG_filter_cubic */
    LAGFX_CUBIC_NO_FORMAT,     /* a sampled texture's format lacks SAMPLED_IMAGE_FILTER_CUBIC */
    LAGFX_CUBIC_NO_VIEWTYPE,   /* a sampled texture is not a 2D / 2D-array view (3D: Vulkan cubic filtering is not defined for it) */
    LAGFX_CUBIC_NO_COMBO,      /* unnormalized coordinates, anisotropy, or a sampler variant table that is full: conservative exclusions */
    LAGFX_CUBIC_NR
} lagfx_cubic_why_t;

typedef struct { bool optin, ext_enabled; } lagfx_cubic_ctx_t;

/* LAGFX_CUBIC=1 (exactly "1"). Read at each call; the device enables the extension only at creation, so a later change only turns the use off. */
bool lagfx_cubic_optin(void);

/* The decision for one sampler paired with one texture: `fmt_features` = optimalTilingFeatures of the texture's VkFormat, `view_2d` = a 2D / 2D-array sampled view, `combo_ok` = false
 * when unnormalized coordinates or anisotropy are on. Pure. Checked in the order of the enum (the first failing reason is reported). */
lagfx_cubic_why_t lagfx_cubic_decide(const lagfx_cubic_ctx_t *c, uint32_t fmt_features, bool view_2d, bool combo_ok);
const char *lagfx_cubic_why_name(lagfx_cubic_why_t w);
/* The sampler create info of a cubic twin: VK_FILTER_CUBIC_EXT for minFilter (if `min`) and magFilter (if `mag`); everything else untouched. Pure. */
void lagfx_cubic_apply(VkSamplerCreateInfo *ci, bool min, bool mag);

#endif /* LAGFX_HAVE_VULKAN */
#endif
