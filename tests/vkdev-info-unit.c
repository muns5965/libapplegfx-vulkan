/*
 * libapplegfx-vulkan — read-only device diagnostic `g7-vkdev:` (src/vulkan/devinfo.c) on the available ICD (lavapipe in CI)
 * tests/vkdev-info-unit.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Checks the STRUCTURE of the output (tag, line length cap, line-count cap, the extension list equals an independent vkEnumerateDeviceExtensionProperties,
 * extension-gated lines say ext=0 exactly when the extension is absent, the two format lines exist, the production once-per-process log) - never device-specific values.
 * Skips (exit 77) when no Vulkan device is usable.
 */
#include "libapplegfx-vulkan.h"
#include "vulkan/devinfo.h"
#include "vulkan/instance.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)

#define MAXL 64
static char g_lines[MAXL][LAGFX_VKDEV_LINE_MAX + 64];
static size_t g_n;
static void grab(void *u, const char *l) { (void)u; if (g_n < MAXL) snprintf(g_lines[g_n], sizeof g_lines[0], "%s", l); g_n++; }

static const char *find_line(const char *key) {
    char k[80];
    snprintf(k, sizeof k, "g7-vkdev: %s", key);
    for (size_t i = 0; i < g_n && i < MAXL; i++) if (!strncmp(g_lines[i], k, strlen(k))) return g_lines[i];
    return NULL;
}

int main(void) {
    struct lagfx_vk_state *vk = NULL;
    lagfx_device_descriptor_t dd;
    memset(&dd, 0, sizeof dd);
    char logpath[] = "/tmp/vkdev-unit-XXXXXX";
    int lfd = mkstemp(logpath);
    CHECK(lfd >= 0, "mkstemp");
    close(lfd);
    setenv("LAGFX_LOG_FILE", logpath, 1);
    if (lagfx_vk_init(&vk, &dd) != LAGFX_OK || !vk || !vk->initialized) { printf("no usable Vulkan device: skip\n"); unlink(logpath); return 77; }
    VkPhysicalDevice pd = vk->phys_device;

    /* the production hook already ran inside lagfx_vk_init: the WARN log holds the lines exactly once */
    {
        FILE *f = fopen(logpath, "r");
        CHECK(f, "log file");
        char buf[2048];
        int ids = 0, fmts = 0, longest = 0;
        while (fgets(buf, sizeof buf, f)) {
            if (!strstr(buf, "g7-vkdev: ")) continue;
            if (strstr(buf, "g7-vkdev: id ")) ids++;
            if (strstr(buf, "g7-vkdev: fmt mtl=")) fmts++;
            if (!strstr(buf, "WARN")) { printf("FAIL: g7-vkdev line not WARN level: %.120s\n", buf); fclose(f); return 1; }
            int l = (int)strlen(strstr(buf, "g7-vkdev: ")); if (l > longest) longest = l;
        }
        fclose(f);
        CHECK(ids == 1, "id line must be logged exactly once per process, got %d", ids);
        CHECK(fmts == 2, "two fmt lines expected, got %d", fmts);
        CHECK(longest <= (int)LAGFX_VKDEV_LINE_MAX + 1, "logged line longer than the cap: %d", longest);
        CHECK(lagfx_vk_devinfo_log_once(pd) == 0, "second log_once must be a no-op");
    }

    size_t n = lagfx_vk_devinfo_collect(pd, grab, NULL);
    CHECK(n == g_n && n >= 8 && n <= LAGFX_VKDEV_MAX_LINES + 1, "line count %zu out of range", n);
    for (size_t i = 0; i < n; i++) {
        CHECK(!strncmp(g_lines[i], "g7-vkdev: ", 10), "line %zu lacks the tag", i);
        CHECK(strlen(g_lines[i]) < LAGFX_VKDEV_LINE_MAX, "line %zu is %zu chars (cap %u)", i, strlen(g_lines[i]), LAGFX_VKDEV_LINE_MAX);
        CHECK(!strchr(g_lines[i], '\n'), "line %zu has a newline", i);
    }
    CHECK(find_line("id name='"), "id line");

    /* extension list == independent enumeration (names), over all parts */
    uint32_t ne = 0;
    CHECK(vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL) == VK_SUCCESS, "enum");
    VkExtensionProperties *ex = calloc(ne ? ne : 1, sizeof *ex);
    CHECK(ex && vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ex) == VK_SUCCESS, "enum2");
    uint32_t seen = 0, parts_seen = 0, parts_decl = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned fn = 0, pi = 0, pk = 0;
        int off = 0;
        if (sscanf(g_lines[i], "g7-vkdev: exts n=%u part=%u/%u %n", &fn, &pi, &pk, &off) < 3) continue;
        CHECK(fn == ne, "exts n=%u != enumerated %u", fn, ne);
        parts_seen++, parts_decl = pk;
        CHECK(pi == parts_seen, "parts out of order");
        for (const char *p = g_lines[i] + off; *p;) {
            const char *e = strchr(p, ' ');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            char tok[300];
            CHECK(len < sizeof tok, "token too long");
            memcpy(tok, p, len), tok[len] = 0;
            char *sl = strrchr(tok, '/');
            CHECK(sl, "token without /specVersion: %s", tok);
            *sl = 0;
            int ok = 0;
            for (uint32_t k = 0; k < ne; k++) if (!strcmp(ex[k].extensionName, tok) && ex[k].specVersion == (uint32_t)atoi(sl + 1)) ok = 1;
            CHECK(ok, "extension token not in the enumeration: %s", tok);
            seen++;
            if (!e) break;
            p = e + 1;
        }
    }
    CHECK(seen == ne, "listed %u of %u extensions", seen, ne);
    CHECK(ne == 0 || parts_seen == parts_decl, "part count %u != declared %u", parts_seen, parts_decl);

    /* extension-gated lines: ext=0 iff the extension is absent */
    struct { const char *key, *ext; } gate[] = { { "shader_tile_image ", "VK_EXT_shader_tile_image" }, { "qcom_tile_shading ", "VK_QCOM_tile_shading" } };
    for (size_t g = 0; g < 2; g++) {
        const char *l = find_line(gate[g].key);
        int present = 0;
        for (uint32_t k = 0; k < ne; k++) if (!strcmp(ex[k].extensionName, gate[g].ext)) present = 1;
        CHECK(l, "missing line %s", gate[g].key);
        CHECK(!!strstr(l, "ext=1") == present && !!strstr(l, "ext=0") == !present, "%s: line disagrees with the extension list: %s", gate[g].key, l);
    }
    free(ex);
    CHECK(find_line("filter_cubic "), "filter_cubic line");
    CHECK(find_line("dyn_local_read "), "dyn_local_read line");
    CHECK(find_line("raster_order "), "raster_order line");
    CHECK(find_line("subgroup "), "subgroup line");
    const char *f80 = find_line("fmt mtl=80 vk=44 "), *f115 = find_line("fmt mtl=115 vk=97 ");
    CHECK(f80 && f115, "format lines");
    CHECK(strstr(f80, "SAMPLED_IMAGE_FILTER_CUBIC=") && strstr(f80, "TRANSFER_DST=") && strstr(f115, "COLOR_ATTACHMENT_BLEND="), "format flag names");
    for (size_t i = 0; i < n; i++) printf("%s\n", g_lines[i]);
    unlink(logpath);
    lagfx_vk_shutdown(vk);
    printf("vkdev-info-unit: ok (%zu lines, %u extensions)\n", n, ne);
    return 0;
}
