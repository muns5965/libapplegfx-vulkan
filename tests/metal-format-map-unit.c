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
