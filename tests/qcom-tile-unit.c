/* SPDX-License-Identifier: MIT */
/* VK_QCOM_tile_shading opt-in (src/vulkan/qcom_tile.c, instance.c): (A) the pure decision / mask / line logic on injected capability records (the Adreno 840 of the
 * device log and degraded variants), (B) lagfx_vk_init on a real ICD (lavapipe has no VK_QCOM_tile_shading): flag off = nothing requested, flag on = fail-closed, and the
 * LAGFX_QCOM_TILE_INJECT=adreno840 seam, which makes the library request an extension the ICD does not have and so exercises the vkCreateDevice refusal -> retry -> fallback path.
 * NOT verified here: anything on a device that has the extension (see docs/qcom-tile-shading.md, experiments). Exit 77 = no usable Vulkan device (part B only). */
#define _GNU_SOURCE
#include "vulkan/qcom_tile.h"
#include "vulkan/instance.h"
#include "libapplegfx-vulkan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s (line %d)\n", m, __LINE__); g_fail++; } } while (0)

static void part_a(void) {
    lagfx_qcomtile_caps_t c; lagfx_qcomtile_caps_adreno840(&c);
    CHECK(!lagfx_qcomtile_flag(NULL) && !lagfx_qcomtile_flag("") && !lagfx_qcomtile_flag("0") && !lagfx_qcomtile_flag("11") && !lagfx_qcomtile_flag("true") && !lagfx_qcomtile_flag(" 1") && lagfx_qcomtile_flag("1"), "flag is exactly \"1\"");
    CHECK(lagfx_qcomtile_decide(false, true, &c) == LAGFX_QCT_OFF_FLAG, "flag off: never");
    CHECK(lagfx_qcomtile_decide(false, true, NULL) == LAGFX_QCT_OFF_FLAG, "flag off, no caps");
    CHECK(lagfx_qcomtile_decide(true, true, &c) == LAGFX_QCT_ENABLE, "adreno 840 record + flag: enable");
    CHECK(lagfx_qcomtile_decide(true, true, NULL) != LAGFX_QCT_ENABLE, "no caps: fail closed");
    CHECK(lagfx_qcomtile_decide(true, false, &c) == LAGFX_QCT_NO_DYNAMIC_RENDERING, "no dynamic rendering");
    lagfx_qcomtile_caps_t d = c; d.ext_tile_shading = 0;
    CHECK(lagfx_qcomtile_decide(true, true, &d) == LAGFX_QCT_NO_EXT_TILE_SHADING, "extension absent");
    d = c; d.ext_tile_properties = 0;
    CHECK(lagfx_qcomtile_decide(true, true, &d) == LAGFX_QCT_NO_EXT_TILE_PROPERTIES, "tile_properties (a dependency in vk.xml) absent");
    const unsigned req[3] = { LAGFX_QCT_F_TILE_SHADING, LAGFX_QCT_F_COLOR, LAGFX_QCT_F_PER_TILE_DISPATCH };
    for (unsigned i = 0; i < 3; i++) { d = c; d.feat &= ~LAGFX_QCT_BIT(req[i]); CHECK(lagfx_qcomtile_decide(true, true, &d) == LAGFX_QCT_NO_FEATURE, "a required feature missing"); }
    d = c; d.feat &= ~(LAGFX_QCT_BIT(LAGFX_QCT_F_DISPATCH_TILE) | LAGFX_QCT_BIT(LAGFX_QCT_F_ATOMIC_OPS) | LAGFX_QCT_BIT(LAGFX_QCT_F_INPUT) | LAGFX_QCT_BIT(LAGFX_QCT_F_SAMPLED));
    CHECK(lagfx_qcomtile_decide(true, true, &d) == LAGFX_QCT_ENABLE, "optional features missing: still enable (the mask shrinks)");
    CHECK(lagfx_qcomtile_enable_mask(&d) == LAGFX_QCT_REQUIRED, "mask of that record = the required three");
    d = c; d.gran_w = 0;
    CHECK(lagfx_qcomtile_decide(true, true, &d) == LAGFX_QCT_BAD_PROPS, "zero granularity");
    d = c; d.rate_h = 0;
    CHECK(lagfx_qcomtile_decide(true, true, &d) == LAGFX_QCT_BAD_PROPS, "zero max rate");
    /* the mask: reported AND wanted; never apron / depth / stencil / per-tile draw / fragment stage / image processing */
    uint32_t m = lagfx_qcomtile_enable_mask(&c);
    CHECK(m == LAGFX_QCT_WANTED, "adreno mask = wanted set");
    CHECK(!(m & (LAGFX_QCT_BIT(LAGFX_QCT_F_APRON) | LAGFX_QCT_BIT(LAGFX_QCT_F_ANISO_APRON) | LAGFX_QCT_BIT(LAGFX_QCT_F_DEPTH) | LAGFX_QCT_BIT(LAGFX_QCT_F_STENCIL) |
                  LAGFX_QCT_BIT(LAGFX_QCT_F_PER_TILE_DRAW) | LAGFX_QCT_BIT(LAGFX_QCT_F_FRAGMENT_STAGE) | LAGFX_QCT_BIT(LAGFX_QCT_F_IMAGE_PROCESSING))), "never-enabled features stay off");
    d = c; d.feat = LAGFX_QCT_REQUIRED;
    CHECK(lagfx_qcomtile_enable_mask(&d) == LAGFX_QCT_REQUIRED, "a feature the device does not report is never requested");
    lagfx_qct_feat_t f; lagfx_qct_bool_feat_t tp;
    lagfx_qcomtile_fill_features(&c, &f, &tp);
    CHECK(f.sType == 1000309000 && tp.sType == 1000484000, "sType numbers (vk.xml: ext 310 / 485)");
    CHECK(f.tileShading && f.tileShadingColorAttachments && f.tileShadingPerTileDispatch && f.tileShadingDispatchTile && f.tileShadingInputAttachments && f.tileShadingSampledAttachments && f.tileShadingAtomicOps, "wanted members set");
    CHECK(!f.tileShadingFragmentStage && !f.tileShadingDepthAttachments && !f.tileShadingStencilAttachments && !f.tileShadingPerTileDraw && !f.tileShadingApron && !f.tileShadingAnisotropicApron && !f.tileShadingImageProcessing, "other members clear");
    CHECK(tp.value == 0, "tileProperties feature not requested when the record does not say it is supported");
    CHECK(LAGFX_QCT_ST_TILE_MEMORY_HEAP_FEATURES == 1000547000 && LAGFX_QCT_ST_TILE_MEMORY_HEAP_PROPERTIES == 1000547001, "heap sTypes (vk.xml ext 548)");
    char line[900];
    size_t n = lagfx_qcomtile_format_line(line, sizeof line, true, false, &c, LAGFX_QCT_ENABLE, m, true, LAGFX_QCT_FB_NONE, 0);
    CHECK(n && n == strlen(line) && n < 800, "line fits");
    CHECK(strstr(line, "g7-gpu-qcomtile: flag=1 inject=0") && strstr(line, "granularity=16x16 maxRate=8x8 maxApron=8 preferNonCoherent=1") && strstr(line, "perTileDispatch=1 dispatchTile=1 apron=0 anisoApron=0 atomics=1 imageProc=0") &&
          strstr(line, "decision=enable") && strstr(line, "enabled=1") && strstr(line, "chosen_path=compute_emulation") && strstr(line, "qcom_native_path=not_implemented") && strstr(line, "fallback=none"), "line content (adreno)");
    lagfx_qcomtile_format_line(line, sizeof line, true, true, &c, LAGFX_QCT_ENABLE, m, false, LAGFX_QCT_FB_DEVICE_CREATE_FAILED, -7);
    CHECK(strstr(line, "inject=1") && strstr(line, "enabled=0") && strstr(line, "create_result=-7") && strstr(line, "fallback=device_create_failed(retried_without)"), "line content (fallback)");
    char tiny[16];
    size_t tn = lagfx_qcomtile_format_line(tiny, sizeof tiny, false, false, &c, LAGFX_QCT_OFF_FLAG, 0, false, LAGFX_QCT_FB_NONE, 0);
    CHECK(tn == sizeof tiny - 1u && tiny[tn] == 0, "truncation is bounded + terminated");
    for (int r = 0; r < LAGFX_QCT_REASON_COUNT; r++) CHECK(strcmp(lagfx_qcomtile_reason_name((lagfx_qcomtile_reason_t)r), "?") != 0, "every reason has a name");
}

#ifdef LAGFX_HAVE_VULKAN
static int init_with(const char *flag, const char *inject, struct lagfx_vk_state **vk) {
    if (flag) setenv("LAGFX_QCOM_TILE", flag, 1); else unsetenv("LAGFX_QCOM_TILE");
    if (inject) setenv("LAGFX_QCOM_TILE_INJECT", inject, 1); else unsetenv("LAGFX_QCOM_TILE_INJECT");
    lagfx_device_descriptor_t dd; memset(&dd, 0, sizeof dd);
    *vk = NULL;
    return lagfx_vk_init(vk, &dd) == LAGFX_OK && *vk && (*vk)->initialized;
}

static int part_b(void) {
    struct lagfx_vk_state *vk;
    if (!init_with(NULL, NULL, &vk)) { printf("no usable Vulkan device: skip part B\n"); return 77; }
    const bool real_ext = vk->qcomtile.caps.ext_tile_shading != 0;
    CHECK(!vk->qcomtile.flag && !vk->qcomtile.enabled && vk->qcomtile.reason == LAGFX_QCT_OFF_FLAG && vk->qcomtile.fallback == LAGFX_QCT_FB_NONE && !vk->qcomtile.enable_mask, "flag unset: nothing requested");
    CHECK(!strstr(vk->dev_exts, "VK_QCOM_tile"), "flag unset: device extension list has no QCOM tile entry");
    CHECK(strstr(vk->qcomtile.line, "g7-gpu-qcomtile: flag=0") && strstr(vk->qcomtile.line, "decision=flag_off") && strstr(vk->qcomtile.line, "chosen_path=compute_emulation"), "flag unset: diagnostic line");
    lagfx_vk_shutdown(vk);

    if (!init_with("0", "adreno840", &vk)) { CHECK(0, "init with flag=0"); return 1; }
    CHECK(!vk->qcomtile.flag && !vk->qcomtile.injected && !vk->qcomtile.enabled && vk->qcomtile.reason == LAGFX_QCT_OFF_FLAG, "flag=0: the inject seam is ignored");
    lagfx_vk_shutdown(vk);

    if (!init_with("1", NULL, &vk)) { CHECK(0, "init with flag=1"); return 1; }
    CHECK(vk->qcomtile.flag && !vk->qcomtile.injected, "flag=1 recorded");
    if (!real_ext) {
        CHECK(vk->qcomtile.reason == LAGFX_QCT_NO_EXT_TILE_SHADING && !vk->qcomtile.enabled && vk->qcomtile.fallback == LAGFX_QCT_FB_NONE && !strstr(vk->dev_exts, "VK_QCOM_tile"), "flag=1 on a device without the extension: fail closed, nothing requested");
        CHECK(strstr(vk->qcomtile.line, "decision=no_ext_tile_shading") && strstr(vk->qcomtile.line, "enabled=0"), "flag=1: diagnostic line");
    }
    lagfx_vk_shutdown(vk);

    if (!real_ext) {   /* the device refuses VK_QCOM_tile_properties / VK_QCOM_tile_shading: the first vkCreateDevice fails, the retry without them must succeed and be recorded */
        if (!init_with("1", "adreno840", &vk)) { CHECK(0, "injected: the retry did not produce a device"); return 1; }
        CHECK(vk->qcomtile.flag && vk->qcomtile.injected && vk->qcomtile.reason == LAGFX_QCT_ENABLE, "injected adreno record: decision enable");
        CHECK(!vk->qcomtile.enabled && vk->qcomtile.fallback == LAGFX_QCT_FB_DEVICE_CREATE_FAILED && vk->qcomtile.create_result != 0 && !vk->qcomtile.enable_mask, "refused -> retried without, fallback recorded, enabled=0");
        CHECK(!strstr(vk->dev_exts, "VK_QCOM_tile") && vk->device != VK_NULL_HANDLE, "the device that exists was created without the QCOM extensions");
        CHECK(strstr(vk->qcomtile.line, "fallback=device_create_failed(retried_without)") && strstr(vk->qcomtile.line, "inject=1") && strstr(vk->qcomtile.line, "decision=enable"), "fallback diagnostic line");
        lagfx_vk_shutdown(vk);
        printf("part B: injected-refusal fallback exercised\n");
    } else printf("part B: the ICD has VK_QCOM_tile_shading: injection case skipped\n");
    return 0;
}
#endif

int main(void) {
    char logpath[] = "/tmp/qcomtile-unit-XXXXXX";
    int lfd = mkstemp(logpath);
    if (lfd >= 0) close(lfd), setenv("LAGFX_LOG_FILE", logpath, 1);
    part_a();
    int rc_b = 0;
#ifdef LAGFX_HAVE_VULKAN
    rc_b = part_b();
#endif
    if (lfd >= 0) unlink(logpath);
    if (g_fail) { printf("qcom-tile-unit: %d FAILED\n", g_fail); return 1; }
    printf("qcom-tile-unit: OK (part A pure%s)\n", rc_b == 77 ? "; part B skipped: no Vulkan device" : " + part B");
    return 0;
}
