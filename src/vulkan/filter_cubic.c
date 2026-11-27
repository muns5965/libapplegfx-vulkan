/*
 * libapplegfx-vulkan — opt-in cubic texture filtering: the selection logic (see filter_cubic.h)
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 */
#include "filter_cubic.h"

#ifdef LAGFX_HAVE_VULKAN
#include <stdlib.h>
#include <string.h>

bool lagfx_cubic_optin(void) {
    const char *e = getenv("LAGFX_CUBIC");
    return e && strcmp(e, "1") == 0;
}

lagfx_cubic_why_t lagfx_cubic_decide(const lagfx_cubic_ctx_t *c, uint32_t fmt_features, bool view_2d, bool combo_ok) {
    if (!c || !c->optin) return LAGFX_CUBIC_NO_OPTIN;
    if (!c->ext_enabled) return LAGFX_CUBIC_NO_EXT;
    if (!(fmt_features & LAGFX_FMT_FEATURE_FILTER_CUBIC)) return LAGFX_CUBIC_NO_FORMAT;
    if (!view_2d) return LAGFX_CUBIC_NO_VIEWTYPE;
    if (!combo_ok) return LAGFX_CUBIC_NO_COMBO;
    return LAGFX_CUBIC_USE;
}

const char *lagfx_cubic_why_name(lagfx_cubic_why_t w) {
    static const char *const n[LAGFX_CUBIC_NR] = { "use", "no_optin", "no_ext", "no_format", "no_viewtype", "no_combo" };
    return (unsigned)w < LAGFX_CUBIC_NR ? n[w] : "?";
}

void lagfx_cubic_apply(VkSamplerCreateInfo *ci, bool min, bool mag) {
    if (min) ci->minFilter = LAGFX_FILTER_CUBIC;
    if (mag) ci->magFilter = LAGFX_FILTER_CUBIC;
}
#endif
