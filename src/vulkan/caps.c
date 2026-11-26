/* libapplegfx-vulkan — SPIR-V capability scan for the device-capability gate (see caps.h). */
#include "instance.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static const char *const k_names[LAGFX_VK_CAP_COUNT] = { "int64", "int16", "int8", "float16", "storage8", "storage16", "varptr_sb", "varptr", "spirv14" };

const char *lagfx_vk_cap_name(unsigned cap) { return cap < LAGFX_VK_CAP_COUNT ? k_names[cap] : "?"; }

unsigned lagfx_vk_caps_parse(const char *csv) {
    unsigned m = 0;
    while (csv && *csv) {
        const char *e = csv;
        while (*e && *e != ',') e++;
        size_t n = (size_t)(e - csv);
        for (unsigned i = 0; i < LAGFX_VK_CAP_COUNT; i++) if (strlen(k_names[i]) == n && !strncmp(csv, k_names[i], n)) m |= LAGFX_VK_CAP_BIT(i);
        csv = *e ? e + 1 : e;
    }
    return m;
}

uint32_t lagfx_spv_required_caps(const void *spv, size_t bytes) {
    if (!spv || bytes < 20u || (bytes & 3u)) return 0;
    const uint32_t *w = spv;
    size_t n = bytes / 4u;
    if (w[0] != 0x07230203u) return 0;
    uint32_t m = 0;
    if (w[1] > 0x00010300u) m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_SPIRV14);
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
        if (!len || i + len > n) break;
        if (op == 17u && len >= 2u) {   /* OpCapability */
            switch (w[i + 1]) {
            case 11: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT64); break;
            case 22: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT16); break;
            case 39: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_INT8); break;
            case 9:  m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_FLOAT16); break;
            case 4448: case 4449: case 4450: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_STORAGE8); break;
            case 4433: case 4434: case 4435: case 4436: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_STORAGE16); break;
            case 4441: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_VARPTR_SB); break;
            case 4442: m |= LAGFX_VK_CAP_BIT(LAGFX_VK_CAP_VARPTR); break;
            default: break;
            }
        } else if (op != 17u && i > 5u && op >= 71u) {   /* capabilities come first in a module: past the first decoration / type / function we are done */
            break;
        }
        i += len;
    }
    return m;
}

const char *lagfx_vk_caps_line(const struct lagfx_vk_state *vk, char *buf, size_t n) {
    char en[160] = "", ms[160] = "";
    size_t eo = 0, mo = 0;
    for (unsigned i = 0; vk && i < LAGFX_VK_CAP_COUNT; i++) {
        if (vk->caps_enabled & LAGFX_VK_CAP_BIT(i)) eo += (size_t)snprintf(en + eo, sizeof en - eo, "%s%s", eo ? "," : "", k_names[i]);
        else mo += (size_t)snprintf(ms + mo, sizeof ms - mo, "%s%s", mo ? "," : "", k_names[i]);
    }
    snprintf(buf, n, "g7-gpu-caps: enabled=[%s] missing=[%s] (shaders needing a missing capability are not built; their draws are skipped, stub_missing_cap_<name>)", en, ms);
    return buf;
}

void lagfx_vk_caps_inject_missing(struct lagfx_vk_state *vk, uint32_t mask) {
    if (!vk) return;
    vk->caps_enabled &= ~mask;
    vk->caps_missing |= mask & LAGFX_VK_CAP_ALL;
}
