/* SPDX-License-Identifier: MIT */
#include "pipeline_bundle.h"
#include "common/log.h"
#include "descriptor_layout.h"
#include "instance.h"
#include "protocol/arm64_dump.h"

#ifdef LAGFX_HAVE_VULKAN

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STATE_MAX (64u * 1024u)
#define SPV_MAX   (8u << 20)
#define BC_MAX    (4u << 20)

/* the scalar fields of lagfx_pipeline_desc_t, written as "desc.<name> v0 v1 ..." (elem = 1 or 4 bytes, unsigned) */
typedef struct { const char *name; size_t off; unsigned elem, count; } field_t;
#define F1(f, n) { #f, offsetof(lagfx_pipeline_desc_t, f), 1u, n }
#define F4(f, n) { #f, offsetof(lagfx_pipeline_desc_t, f), 4u, n }
static const field_t k_fields[] = {
    F4(color_format, 1), F4(depth_format, 1), F1(vtx_in_loc, 8), F1(vtx_in_comp, 8), F1(n_vtx_inputs, 1), F1(vtx_in_src_known, 8), F1(vtx_in_src_unfed, 8), F4(vtx_in_src_off, 8), F1(vtx_in_buf_known, 8), F4(vtx_in_buf, 8), F4(pso_attr_buf, 8), F1(pso_attr_buf_known, 1), F4(vtx_stride, 1), F4(pso_attr_fmt, 8), F4(pso_attr_off, 8), F1(n_pso_attrs, 1), F1(pso_attrs_serialized, 1),
    F1(topology_plus1, 1), F4(caps_enabled, 1), F1(blend_set, 1), F1(blend_enable, 1), F4(blend_src_rgb, 1), F4(blend_dst_rgb, 1), F4(blend_op_rgb, 1), F4(blend_src_a, 1), F4(blend_dst_a, 1),
    F4(blend_op_a, 1), F4(color_write_mask, 1), F1(n_color, 1), F1(independent_blend, 1), F1(masks_valid, 1), F1(raster_order_access, 1), F4(color_formats, 4), F4(color_write_masks, 4),
    F4(ds_depth_format, 1), F4(ds_stencil_format, 1), F1(dyn_state, 1), F1(no_color, 1),
};
#define NFIELDS (sizeof k_fields / sizeof k_fields[0])
static const char *const k_env[] = { "LAGFX_VTX_STRIDE", "LAGFX_DISABLE_YFLIP", "LAGFX_DISABLE_M2_SRCOVER", "LAGFX_VK_CAPS_OFF", "LAGFX_VK_NO_LOCAL_READ", "LAGFX_VK_NO_RASTER_ORDER" };

const char *lagfx_vk_result_name(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    case VK_PIPELINE_COMPILE_REQUIRED: return "VK_PIPELINE_COMPILE_REQUIRED";
    default: return "VK_RESULT(other)";
    }
}

const char *lagfx_pipeline_bundle_dir(void) {
    const char *d = getenv("LAGFX_PIPEFAIL_BUNDLE_DIR");
    if (d && d[0]) return d;
    if (lagfx_a64_dump_enabled() && lagfx_a64_dump_dir_fd() >= 0) {   /* the existing dump option: same directory */
        d = getenv("LAGFX_ARM64_DUMP_DIR");
        if (d && d[0]) return d;
    }
    return NULL;
}

static struct { uint64_t key[LAGFX_BUNDLE_MAX_FAILURES]; uint32_t n; } g_seen;   /* the writer's bound: process-wide on purpose (it limits disk use); the REPRO keeps no global state */
static _Thread_local int g_suppress;
void lagfx_pipeline_bundle_suppress(int on) { g_suppress = on; }
void lagfx_pipeline_bundle_reset(void) { memset(&g_seen, 0, sizeof g_seen); }

typedef struct { char *p; size_t n, cap; } sb_t;
static void sb(sb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb(sb_t *b, const char *fmt, ...) {
    if (b->n + 1u >= b->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (w > 0) b->n = (size_t)w >= b->cap - b->n ? b->cap - 1u : b->n + (size_t)w;
}
static void sb_fields(sb_t *b, const lagfx_pipeline_desc_t *d) {
    for (size_t i = 0; i < NFIELDS; i++) {
        const field_t *f = &k_fields[i];
        sb(b, "desc.%s", f->name);
        for (unsigned k = 0; k < f->count; k++) {
            const uint8_t *q = (const uint8_t *)d + f->off + (size_t)k * f->elem;
            uint32_t v = 0;
            if (f->elem == 1u) v = *q; else memcpy(&v, q, 4);
            sb(b, " %u", v);
        }
        sb(b, "\n");
    }
    for (unsigned i = 0; i < 4; i++) {
        const __typeof__(d->slot_blend[0]) *s = &d->slot_blend[i];
        sb(b, "desc.slot_blend %u %u %u %u %u %u %u %u %u\n", i, s->set, s->enable, s->src_rgb, s->dst_rgb, s->op_rgb, s->src_a, s->dst_a, s->op_a);
    }
}

static bool write_file(const char *dir, const char *name, const void *data, size_t n) {
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const uint8_t *p = data;
    size_t off = 0;
    while (off < n) { ssize_t w = write(fd, p + off, n - off); if (w <= 0) { close(fd); return false; } off += (size_t)w; }
    return close(fd) == 0;
}

bool lagfx_pipeline_bundle_write(const lagfx_pipeline_desc_t *desc, VkResult vr, const char *derived, char *out_dir, size_t out_cap) {
    const char *base = lagfx_pipeline_bundle_dir();
    if (g_suppress) return false;
    if (!base || !desc || !desc->vertex_spv || !desc->fragment_spv || !desc->vertex_spv_bytes || !desc->fragment_spv_bytes) return false;
    if (desc->vertex_spv_bytes > SPV_MAX || desc->fragment_spv_bytes > SPV_MAX) return false;
    char *st = malloc(STATE_MAX);
    if (!st) return false;
    sb_t b = { st, 0, STATE_MAX };
    const lagfx_pipeline_diag_t *dg = desc->diag;
    uint64_t vh = lagfx_pipeline_spv_hash(desc->vertex_spv, desc->vertex_spv_bytes), fh = lagfx_pipeline_spv_hash(desc->fragment_spv, desc->fragment_spv_bytes);
    sb(&b, "lagfx-pipefail-bundle 1\nresult %d\ntranslator %s\norigin %s\n", (int)vr, lagfx_build_info(), dg && dg->origin ? dg->origin : "unknown");
    sb(&b, "vs_entry %s\nfs_entry %s\nvs_fn %s\nfs_fn %s\n", desc->vertex_entry_point ? desc->vertex_entry_point : "main", desc->fragment_entry_point ? desc->fragment_entry_point : "main",
       dg && dg->vs_fn ? dg->vs_fn : "?", dg && dg->fs_fn ? dg->fs_fn : "?");
    sb(&b, "vs_hash %016llx\nfs_hash %016llx\nvs_bytes %zu\nfs_bytes %zu\n", (unsigned long long)vh, (unsigned long long)fh, desc->vertex_spv_bytes, desc->fragment_spv_bytes);
    sb_fields(&b, desc);
    {   /* layout: what lagfx_build_pipeline_layout_from_spv() makes of these two modules (that is what the resolver builds the layout from) */
        VkDescriptorSetLayoutBinding binds[LAGFX_BUNDLE_MAX_DSL];
        const uint8_t *blobs[2] = { (const uint8_t *)desc->vertex_spv, (const uint8_t *)desc->fragment_spv };
        const size_t lens[2] = { desc->vertex_spv_bytes, desc->fragment_spv_bytes };
        uint32_t n = lagfx_layout_bindings_from_spv(blobs, lens, 2, binds);
        sb(&b, "layout.sets %u\n", n ? 1u : 0u);
        for (uint32_t i = 0; i < n; i++) sb(&b, "layout.binding %u %u %u %u\n", binds[i].binding, (unsigned)binds[i].descriptorType, binds[i].descriptorCount, (unsigned)binds[i].stageFlags);
        sb(&b, "# layout source: derived from the SPIR-V (lagfx_layout_bindings_from_spv); no push constant ranges are ever declared by lagfx_build_pipeline_layout_from_spv\n");
    }
    sb(&b, "spec_constants 0\n");
    if (dg && dg->vk) {
        const struct lagfx_vk_state *vk = dg->vk;
        sb(&b, "dev.name %s\ndev.driver_version %u\ndev.api_version %u\ndev.vendor_id %u\ndev.device_id %u\n", vk->phys_props.deviceName, vk->phys_props.driverVersion, vk->phys_props.apiVersion, vk->phys_props.vendorID, vk->phys_props.deviceID);
        sb(&b, "dev.driver_name %s\ndev.driver_info %s\ndev.extensions %s\ndev.features %s\ndev.caps_enabled %u\n", vk->driver_name, vk->driver_info, vk->dev_exts, vk->dev_features, vk->caps_enabled);
    } else sb(&b, "# dev.*: unknown (the caller gave no lagfx_pipeline_diag_t.vk)\n");
    for (size_t i = 0; i < sizeof k_env / sizeof k_env[0]; i++) { const char *e = getenv(k_env[i]); sb(&b, "env.%s %s\n", k_env[i], e ? e : "(unset)"); }
    if (derived) for (const char *q = derived; *q;) {   /* informational lines, each prefixed so the loader skips them */
        const char *nl = strchr(q, '\n');
        size_t l = nl ? (size_t)(nl - q) : strlen(q);
        sb(&b, "# %.*s\n", (int)l, q);
        q += l + (nl ? 1 : 0);
    }
    /* identity of the failure: both modules + the whole state text (distinct states of the same modules get their own bundle) */
    uint64_t key = lagfx_pipeline_spv_hash(st, b.n) ^ (vh * 1099511628211ull) ^ (fh * 0x9e3779b97f4a7c15ull);
    for (uint32_t i = 0; i < g_seen.n; i++) if (g_seen.key[i] == key) { free(st); return false; }
    if (g_seen.n >= LAGFX_BUNDLE_MAX_FAILURES) { free(st); return false; }
    g_seen.key[g_seen.n++] = key;
    char dir[1300];
    snprintf(dir, sizeof dir, "%s/pipefail_%016llx", base, (unsigned long long)key);
    bool ok = false;
    if (mkdir(dir, 0700) == 0) {
        ok = write_file(dir, "state", st, b.n) && write_file(dir, "vs.spv", desc->vertex_spv, desc->vertex_spv_bytes) && write_file(dir, "fs.spv", desc->fragment_spv, desc->fragment_spv_bytes);
        if (ok && dg && dg->vs_bc && dg->vs_bc_bytes && dg->vs_bc_bytes <= BC_MAX) write_file(dir, "vs.bc", dg->vs_bc, dg->vs_bc_bytes);
        if (ok && dg && dg->fs_bc && dg->fs_bc_bytes && dg->fs_bc_bytes <= BC_MAX) write_file(dir, "fs.bc", dg->fs_bc, dg->fs_bc_bytes);
        if (!ok) LAGFX_WARN("pipeline-build-fail: bundle %s incomplete: errno %d", dir, errno);
    } else if (errno != EEXIST) LAGFX_WARN("pipeline-build-fail: cannot create bundle dir %s: errno %d (%s)", dir, errno, strerror(errno));
    else LAGFX_LOG("pipeline-build-fail: bundle %s already exists", dir);
    free(st);
    if (ok) {
        LAGFX_ERR("pipeline-build-fail: failure bundle written to %s (rebuild offline: examples/pipeline-repro <dir>)", dir);
        if (out_dir) snprintf(out_dir, out_cap, "%s", dir);
    }
    return ok;
}

/* ---- loader ---- */

static uint8_t *slurp(const char *dir, const char *name, size_t max, size_t *n) {
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat sts;
    uint8_t *buf = NULL;
    if (fstat(fd, &sts) == 0 && S_ISREG(sts.st_mode) && sts.st_size >= 0 && (uint64_t)sts.st_size <= max && (buf = malloc((size_t)sts.st_size + 1u))) {
        size_t off = 0;
        while (off < (size_t)sts.st_size) { ssize_t r = read(fd, buf + off, (size_t)sts.st_size - off); if (r <= 0) break; off += (size_t)r; }
        if (off != (size_t)sts.st_size) { free(buf); buf = NULL; } else { buf[off] = 0; *n = off; }
    }
    close(fd);
    return buf;
}
void lagfx_bundle_free(lagfx_bundle_t *b) {
    if (!b) return;
    free(b->vs_spv), free(b->fs_spv), free(b->vs_bc), free(b->fs_bc);
    memset(b, 0, sizeof *b);
}
static void errf(char *err, size_t cap, const char *fmt, const char *a) { if (err && cap) snprintf(err, cap, fmt, a); }

bool lagfx_bundle_load(const char *dir, lagfx_bundle_t *b, char *err, size_t err_cap) {
    if (!dir || !b) return false;
    memset(b, 0, sizeof *b);
    size_t sn = 0;
    char *st = (char *)slurp(dir, "state", STATE_MAX, &sn);
    if (!st) { errf(err, err_cap, "cannot read %s/state", dir); return false; }
    if (strncmp(st, "lagfx-pipefail-bundle 1\n", 24) != 0) { errf(err, err_cap, "%s: not a lagfx-pipefail-bundle 1", dir); free(st); return false; }
    for (char *line = st, *nx; line && *line; line = nx) {
        nx = strchr(line, '\n');
        if (nx) *nx++ = 0;
        if (line[0] == '#' || !line[0]) continue;
        char *sp = strchr(line, ' '), *val = sp ? sp + 1 : line + strlen(line);
        if (sp) *sp = 0;
        if (!strcmp(line, "result")) b->result = (int32_t)strtol(val, NULL, 10);
        else if (!strcmp(line, "translator")) snprintf(b->translator, sizeof b->translator, "%s", val);
        else if (!strcmp(line, "origin")) snprintf(b->origin, sizeof b->origin, "%s", val);
        else if (!strcmp(line, "vs_entry")) snprintf(b->vs_entry, sizeof b->vs_entry, "%s", val);
        else if (!strcmp(line, "fs_entry")) snprintf(b->fs_entry, sizeof b->fs_entry, "%s", val);
        else if (!strcmp(line, "vs_fn")) snprintf(b->vs_fn, sizeof b->vs_fn, "%s", val);
        else if (!strcmp(line, "fs_fn")) snprintf(b->fs_fn, sizeof b->fs_fn, "%s", val);
        else if (!strcmp(line, "vs_hash")) b->vs_hash = strtoull(val, NULL, 16);
        else if (!strcmp(line, "fs_hash")) b->fs_hash = strtoull(val, NULL, 16);
        else if (!strcmp(line, "layout.sets")) b->n_sets = (uint32_t)strtoul(val, NULL, 10);
        else if (!strcmp(line, "layout.binding") && b->n_dsl < LAGFX_BUNDLE_MAX_DSL) {
            unsigned bi, ty, ct, sf;
            if (sscanf(val, "%u %u %u %u", &bi, &ty, &ct, &sf) == 4) b->dsl[b->n_dsl++] = (VkDescriptorSetLayoutBinding){ .binding = bi, .descriptorType = (VkDescriptorType)ty, .descriptorCount = ct, .stageFlags = sf };
        } else if (!strcmp(line, "layout.push") && b->n_push < LAGFX_BUNDLE_MAX_PUSH) {
            unsigned sf, off, sz;
            if (sscanf(val, "%u %u %u", &sf, &off, &sz) == 3) b->push[b->n_push++] = (VkPushConstantRange){ .stageFlags = sf, .offset = off, .size = sz };
        } else if (!strncmp(line, "dev.", 4)) {
            b->have_dev = true;
            const char *k = line + 4;
            if (!strcmp(k, "name")) snprintf(b->dev_name, sizeof b->dev_name, "%s", val);
            else if (!strcmp(k, "driver_name")) snprintf(b->dev_driver_name, sizeof b->dev_driver_name, "%s", val);
            else if (!strcmp(k, "driver_info")) snprintf(b->dev_driver_info, sizeof b->dev_driver_info, "%s", val);
            else if (!strcmp(k, "extensions")) snprintf(b->dev_exts, sizeof b->dev_exts, "%s", val);
            else if (!strcmp(k, "features")) snprintf(b->dev_features, sizeof b->dev_features, "%s", val);
            else if (!strcmp(k, "driver_version")) b->dev_driver_version = (uint32_t)strtoul(val, NULL, 10);
            else if (!strcmp(k, "api_version")) b->dev_api_version = (uint32_t)strtoul(val, NULL, 10);
            else if (!strcmp(k, "vendor_id")) b->dev_vendor = (uint32_t)strtoul(val, NULL, 10);
            else if (!strcmp(k, "device_id")) b->dev_device = (uint32_t)strtoul(val, NULL, 10);
            else if (!strcmp(k, "caps_enabled")) b->dev_caps = (uint32_t)strtoul(val, NULL, 10);
        } else if (!strncmp(line, "env.", 4) && b->n_env < 8u) snprintf(b->env[b->n_env++], sizeof b->env[0], "%s=%s", line + 4, val);
        else if (!strcmp(line, "desc.slot_blend")) {
            unsigned v[9];
            if (sscanf(val, "%u %u %u %u %u %u %u %u %u", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8]) == 9 && v[0] < 4) {
                b->desc.slot_blend[v[0]].set = (uint8_t)v[1], b->desc.slot_blend[v[0]].enable = (uint8_t)v[2], b->desc.slot_blend[v[0]].src_rgb = v[3], b->desc.slot_blend[v[0]].dst_rgb = v[4];
                b->desc.slot_blend[v[0]].op_rgb = v[5], b->desc.slot_blend[v[0]].src_a = v[6], b->desc.slot_blend[v[0]].dst_a = v[7], b->desc.slot_blend[v[0]].op_a = v[8];
            }
        } else if (!strncmp(line, "desc.", 5)) {
            for (size_t i = 0; i < NFIELDS; i++) {
                const field_t *f = &k_fields[i];
                if (strcmp(line + 5, f->name)) continue;
                char *c = val;
                for (unsigned k = 0; k < f->count; k++) {
                    char *e = NULL;
                    uint32_t v = (uint32_t)strtoul(c, &e, 10);
                    if (e == c) break;
                    c = e;
                    uint8_t *q = (uint8_t *)&b->desc + f->off + (size_t)k * f->elem;
                    if (f->elem == 1u) *q = (uint8_t)v; else memcpy(q, &v, 4);
                }
            }
        }
    }
    free(st);
    b->vs_spv = slurp(dir, "vs.spv", SPV_MAX, &b->vs_bytes), b->fs_spv = slurp(dir, "fs.spv", SPV_MAX, &b->fs_bytes);
    b->vs_bc = slurp(dir, "vs.bc", BC_MAX, &b->vs_bc_bytes), b->fs_bc = slurp(dir, "fs.bc", BC_MAX, &b->fs_bc_bytes);
    if (!b->vs_spv || !b->fs_spv || (b->vs_bytes & 3u) || (b->fs_bytes & 3u) || !b->vs_bytes || !b->fs_bytes) { errf(err, err_cap, "%s: vs.spv / fs.spv missing or not word aligned", dir); lagfx_bundle_free(b); return false; }
    if (!b->vs_entry[0]) strcpy(b->vs_entry, "main");
    if (!b->fs_entry[0]) strcpy(b->fs_entry, "main");
    return true;
}

#endif /* LAGFX_HAVE_VULKAN */
