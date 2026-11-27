/* Metal pixel / vertex format -> Vulkan format tables against the public Metal enum values. SPDX-License-Identifier: MIT */
#include "vulkan/iosurface.h"
#include "vulkan/pipeline_build.h"
#include <stdio.h>

static int bad;
#define EQ(a, b, what) do { if ((a) != (b)) { printf("FAIL: %s (got %d, want %d)\n", what, (int)(a), (int)(b)); bad++; } } while (0)

int main(void) {
    /* MTLPixelFormat: R16Float = 25, Depth16Unorm = 250, Depth32Float = 252, RGBA16Float = 115, BGRA8Unorm = 80 */
    EQ(lagfx_metal_pixel_format_to_vk(25), VK_FORMAT_R16_SFLOAT, "pixel 25 (R16Float) is a colour float format, not depth");
    EQ(lagfx_metal_pixel_format_to_vk(250), VK_FORMAT_D16_UNORM, "pixel 250 (Depth16Unorm)");
    EQ(lagfx_metal_pixel_format_to_vk(252), VK_FORMAT_D32_SFLOAT, "pixel 252 (Depth32Float)");
    EQ(lagfx_metal_pixel_format_to_vk(115), VK_FORMAT_R16G16B16A16_SFLOAT, "pixel 115 (RGBA16Float)");
    EQ(lagfx_metal_pixel_format_to_vk(80), VK_FORMAT_B8G8R8A8_UNORM, "pixel 80 (BGRA8Unorm)");
    /* every MTLPixelFormat value that occurs in the device dumps / logs of runs 231501 and 230109 (PSO attachments 70/80/115, texture and render-target descriptors
     * 1/30/80/115) maps to its exact equivalent */
    EQ(lagfx_metal_pixel_format_to_vk(1), VK_FORMAT_R8_UNORM, "pixel 1 (A8Unorm, seen on device)");
    EQ(lagfx_metal_pixel_format_to_vk(30), VK_FORMAT_R8G8_UNORM, "pixel 30 (RG8Unorm, seen on device; was an unmapped BGRA8 fallback here)");
    EQ(lagfx_metal_pixel_format_to_vk(70), VK_FORMAT_R8G8B8A8_UNORM, "pixel 70 (RGBA8Unorm, seen in PSO attachments)");
    /* public-enum values (MTLPixelFormat.h, via objc2-metal bindings) and the Vulkan equivalents MoltenVK pairs with them */
    static const struct { uint32_t mtl; VkFormat vk; const char *name; } k[] = {
        {10, VK_FORMAT_R8_UNORM, "R8Unorm"}, {11, VK_FORMAT_R8_SRGB, "R8Unorm_sRGB"}, {12, VK_FORMAT_R8_SNORM, "R8Snorm"}, {13, VK_FORMAT_R8_UINT, "R8Uint"}, {14, VK_FORMAT_R8_SINT, "R8Sint"},
        {20, VK_FORMAT_R16_UNORM, "R16Unorm"}, {22, VK_FORMAT_R16_SNORM, "R16Snorm"}, {23, VK_FORMAT_R16_UINT, "R16Uint"}, {24, VK_FORMAT_R16_SINT, "R16Sint"},
        {31, VK_FORMAT_R8G8_SRGB, "RG8Unorm_sRGB"}, {32, VK_FORMAT_R8G8_SNORM, "RG8Snorm"}, {33, VK_FORMAT_R8G8_UINT, "RG8Uint"}, {34, VK_FORMAT_R8G8_SINT, "RG8Sint"},
        {53, VK_FORMAT_R32_UINT, "R32Uint"}, {54, VK_FORMAT_R32_SINT, "R32Sint"}, {55, VK_FORMAT_R32_SFLOAT, "R32Float"},
        {60, VK_FORMAT_R16G16_UNORM, "RG16Unorm"}, {62, VK_FORMAT_R16G16_SNORM, "RG16Snorm"}, {63, VK_FORMAT_R16G16_UINT, "RG16Uint"}, {64, VK_FORMAT_R16G16_SINT, "RG16Sint"}, {65, VK_FORMAT_R16G16_SFLOAT, "RG16Float"},
        {71, VK_FORMAT_R8G8B8A8_SRGB, "RGBA8Unorm_sRGB"}, {72, VK_FORMAT_R8G8B8A8_SNORM, "RGBA8Snorm"}, {73, VK_FORMAT_R8G8B8A8_UINT, "RGBA8Uint"}, {74, VK_FORMAT_R8G8B8A8_SINT, "RGBA8Sint"},
        {81, VK_FORMAT_B8G8R8A8_SRGB, "BGRA8Unorm_sRGB"},
        {90, VK_FORMAT_A2B10G10R10_UNORM_PACK32, "RGB10A2Unorm"}, {92, VK_FORMAT_B10G11R11_UFLOAT_PACK32, "RG11B10Float"}, {93, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, "RGB9E5Float"},
        {103, VK_FORMAT_R32G32_UINT, "RG32Uint"}, {104, VK_FORMAT_R32G32_SINT, "RG32Sint"}, {105, VK_FORMAT_R32G32_SFLOAT, "RG32Float"},
        {110, VK_FORMAT_R16G16B16A16_UNORM, "RGBA16Unorm"}, {112, VK_FORMAT_R16G16B16A16_SNORM, "RGBA16Snorm"}, {113, VK_FORMAT_R16G16B16A16_UINT, "RGBA16Uint"}, {114, VK_FORMAT_R16G16B16A16_SINT, "RGBA16Sint"},
        {123, VK_FORMAT_R32G32B32A32_UINT, "RGBA32Uint"}, {124, VK_FORMAT_R32G32B32A32_SINT, "RGBA32Sint"}, {125, VK_FORMAT_R32G32B32A32_SFLOAT, "RGBA32Float"},
    };
    for (unsigned i = 0; i < sizeof k / sizeof *k; i++) { char m[64]; snprintf(m, sizeof m, "pixel %u (%s)", k[i].mtl, k[i].name); EQ(lagfx_metal_pixel_format_to_vk(k[i].mtl), k[i].vk, m); }
    /* values that are not (or deliberately not) mapped here keep the reported BGRA8 fallback: 0 Invalid, 29 (a gap in the enum; also a MTLVertexFormat value), 253/255/260 (stencil / depth-stencil: not mapped here), a compressed format, an unknown number */
    EQ(lagfx_metal_pixel_format_to_vk(0), VK_FORMAT_B8G8R8A8_UNORM, "pixel 0 (Invalid) is reported + BGRA8 fallback");
    EQ(lagfx_metal_pixel_format_to_vk(29), VK_FORMAT_B8G8R8A8_UNORM, "pixel 29 (not a MTLPixelFormat) is reported + BGRA8 fallback");
    EQ(lagfx_metal_pixel_format_to_vk(255), VK_FORMAT_B8G8R8A8_UNORM, "pixel 255 (Depth24Unorm_Stencil8) is not mapped here");
    EQ(lagfx_metal_pixel_format_to_vk(253), VK_FORMAT_B8G8R8A8_UNORM, "pixel 253 (Stencil8) is not mapped here");
    EQ(lagfx_metal_pixel_format_to_vk(130), VK_FORMAT_B8G8R8A8_UNORM, "pixel 130 (BC1_RGBA) is not mapped here");
    /* MTLVertexFormat: UChar4 = 3, UChar4Normalized = 9, Half4 = 27, Float = 28..Float4 = 31, UInt2 = 37, ShortNormalized = 52 */
    EQ(lagfx_pso_vertex_format_to_vk(3), VK_FORMAT_R8G8B8A8_UINT, "vertex 3 (UChar4)");
    EQ(lagfx_pso_vertex_format_to_vk(9), VK_FORMAT_B8G8R8A8_UNORM, "vertex 9 (UChar4Normalized, observed on device)");
    EQ(lagfx_pso_vertex_format_to_vk(27), VK_FORMAT_R16G16B16A16_SFLOAT, "vertex 27 (Half4, observed on device)");
    EQ(lagfx_pso_vertex_format_to_vk(28), VK_FORMAT_R32_SFLOAT, "vertex 28 (Float)");
    EQ(lagfx_pso_vertex_format_to_vk(29), VK_FORMAT_R32G32_SFLOAT, "vertex 29 (Float2, observed on device)");
    EQ(lagfx_pso_vertex_format_to_vk(30), VK_FORMAT_R32G32B32_SFLOAT, "vertex 30 (Float3)");
    EQ(lagfx_pso_vertex_format_to_vk(31), VK_FORMAT_R32G32B32A32_SFLOAT, "vertex 31 (Float4)");
    EQ(lagfx_pso_vertex_format_to_vk(37), VK_FORMAT_R32G32_UINT, "vertex 37 (UInt2)");
    EQ(lagfx_pso_vertex_format_to_vk(52), VK_FORMAT_R16_SNORM, "vertex 52 (ShortNormalized)");
    EQ(lagfx_pso_vertex_format_to_vk(0), VK_FORMAT_UNDEFINED, "vertex 0 is not handled");
    EQ(lagfx_pso_vertex_format_to_vk(36), VK_FORMAT_UNDEFINED, "vertex 36 (UInt) is not handled here (never seen)");
    EQ(lagfx_pso_vertex_format_to_vk(999), VK_FORMAT_UNDEFINED, "an unknown vertex format is not handled");
    printf("%s metal-format-map-unit\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
