/* ARM64 render exec on lavapipe (LAGFX_ARM64_EXEC=1): vertex fetch and index handling against a CPU model. Every draw of a configuration is rasterised twice, by
 * the Vulkan device through the executor and by a small CPU triangle model (pixel centres, edge distances; pixels within 0.03 px of an edge are not compared), and the
 * whole 16 x 16 image must match. Covered:
 *   - indexed draws, 16 and 32 bit indices, index buffer offsets that are no multiple of any large alignment (2, 6, 10, 18, 30 bytes ...), compact / wide records,
 *     first index and base vertex (tags 0x07 / 0x0b / 0x06 / 0x0a / 0x11), a vertex buffer bound at an offset, a vertex stride larger than the attribute with the attribute at an
 *     offset (the PSO layout path of the pipeline builder);
 *   - vertex PULLING (no stage-in; the shader reads a storage buffer, as air.vertexFetchFunction-less / buffer-argument vertex shaders do) at 4-aligned offsets that the
 *     device cannot bind directly (the gather copy), with several strides and attribute offsets, non-indexed with a first vertex and indexed with a base vertex;
 *   - the 4-attribute compositor layout (Float4 @0, Float2 @16, Float2 @24, Half4 @32, stride 48, buffer 1) decoded from a REAL device descriptor: colour must come out.
 * Metal conventions assumed by the model (NOT verified on Apple hardware): +y up in NDC, framebuffer y down. SPDX-License-Identifier: MIT */
#define main run_unit_main
#include "arm64-exec-run-unit.c"
#undef main

#ifdef LAGFX_HAVE_VULKAN
#include "arm64-vertex-index-shaders.h"
#include "arm64-pipe-psos.h"
#include "protocol/arm64_pipeline.h"
static float len2f(float x, float y) { float s = x * x + y * y, r = s > 1.f ? s : 1.f; for (int i = 0; i < 30; i++) r = 0.5f * (r + s / r); return r; }   /* no libm */
static float absf(float v) { return v < 0 ? -v : v; }

#define BLUE 0xff0000ffu
#define NVERT 24u
enum { M_ATTR = 1, M_PULL, M_ATTR4 };
static struct { int mode; uint32_t stride_w, posoff_w; VkShaderModule m[8]; VkDescriptorSetLayout dsl; VkPipelineLayout pl; lagfx_a64_pdesc_t pd; } g_v;

static void p16(uint8_t *b, uint32_t o, uint16_t v) { memcpy(b + o, &v, 2); }
static void p32(uint8_t *b, uint32_t o, uint32_t v) { memcpy(b + o, &v, 4); }
static void p64(uint8_t *b, uint32_t o, uint64_t v) { memcpy(b + o, &v, 8); }

static VkShaderModule mod(struct lagfx_vk_state *vk, int slot, const uint32_t *code, size_t bytes) {
    if (!g_v.m[slot]) {
        VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code };
        assert(vkCreateShaderModule(vk->device, &ci, NULL, &g_v.m[slot]) == VK_SUCCESS);
    }
    return g_v.m[slot];
}
static bool resolver_v(void *c, lagfx_protocol_t *p, const lagfx_task_entry_t *t, uint32_t ref, const uint32_t e[3], lagfx_a64_pipeline_src_t *out) {
    (void)c; (void)t; (void)ref; (void)e;
    struct lagfx_vk_state *vk = ((lagfx_device_t *)p->dev)->vk;
    memset(out, 0, sizeof *out);
    out->desc.fragment_shader = mod(vk, 1, k_fs_spv, sizeof k_fs_spv);
    out->desc.layout = vk->empty_layout;
    if (g_v.mode == M_ATTR) {   /* one Float2 attribute at offset 8 of a 24 byte vertex, through the PSO layout fields of the builder */
        out->desc.vertex_shader = mod(vk, 0, k_vs_spv, sizeof k_vs_spv);
        out->desc.n_vtx_inputs = 1, out->desc.vtx_in_loc[0] = 0, out->desc.vtx_in_comp[0] = 2, out->desc.vtx_stride = 24;
        out->desc.pso_attrs_serialized = 1, out->desc.n_pso_attrs = 1, out->desc.pso_attr_fmt[0] = 29, out->desc.pso_attr_off[0] = 8;
        out->vertex_buffer_index = 0;
    } else if (g_v.mode == M_PULL) {
        out->desc.vertex_shader = mod(vk, 2, k_vs_pull_spv, sizeof k_vs_pull_spv);
        if (!g_v.pl) {
            VkDescriptorSetLayoutBinding b = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT };
            VkDescriptorSetLayoutCreateInfo dl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b };
            assert(vkCreateDescriptorSetLayout(vk->device, &dl, NULL, &g_v.dsl) == VK_SUCCESS);
            VkPipelineLayoutCreateInfo pl = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_v.dsl };
            assert(vkCreatePipelineLayout(vk->device, &pl, NULL, &g_v.pl) == VK_SUCCESS);
        }
        out->desc.layout = g_v.pl, out->dsl = g_v.dsl;
        out->n_bind = 1, out->bind[0].binding = 0, out->bind[0].kind = LAGFX_A64_BIND_BUFFER, out->bind[0].stage = 0, out->bind[0].metal_index = 0;
    } else {   /* M_ATTR4: filled from the decoded descriptor exactly as arm64_pipeline.c does */
        const lagfx_a64_pdesc_t *d = &g_v.pd;
        out->desc.vertex_shader = mod(vk, 3, k_vs_attr4_spv, sizeof k_vs_attr4_spv), out->desc.fragment_shader = mod(vk, 4, k_fs_col_spv, sizeof k_fs_col_spv);
        static const uint8_t comp[4] = { 4, 2, 2, 4 };
        out->desc.n_vtx_inputs = 4;
        for (uint32_t i = 0; i < 4; i++) out->desc.vtx_in_loc[i] = (uint8_t)i, out->desc.vtx_in_comp[i] = comp[i];
        assert(d->nattrs);
        out->vertex_buffer_index = d->attrs[0].bufidx;
        out->desc.pso_attrs_serialized = 1;
        out->desc.n_pso_attrs = (uint8_t)d->nattrs;
        for (uint32_t a = 0; a < d->nattrs; a++) out->desc.pso_attr_fmt[a] = d->attrs[a].fmt, out->desc.pso_attr_off[a] = d->attrs[a].off;
        out->desc.vtx_stride = d->stride;
    }
    return true;
}

/* ---- the CPU model ---- */
typedef struct { float x, y; } v2;
static v2 g_pos[NVERT];   /* pixel-space position of vertex n of the table */
static void make_table(void) {
    /* four triangles, one per 8 x 8 quadrant; the corner coordinates keep pixel centres off every edge of their own triangle */
    static const float L[4][3][2] = {
        { { 0.25f, 0.25f }, { 7.25f, 0.25f }, { 0.25f, 7.25f } },
        { { 7.75f, 0.30f }, { 0.25f, 7.80f }, { 7.75f, 7.80f } },
        { { 3.75f, 0.25f }, { 7.25f, 7.75f }, { 0.25f, 7.75f } },
        { { 0.25f, 0.25f }, { 7.75f, 3.25f }, { 0.25f, 7.75f } },
    };
    for (uint32_t v = 0; v < NVERT; v++) {
        uint32_t t = v < 12u ? v / 3u : (v / 3u + 1u) % 4u;   /* vertices 12..23: the same set rotated by one triangle (a base vertex of 12 must change the picture) */
        g_pos[v].x = (float)((t & 1u) * 8u) + L[t][v % 3u][0];
        g_pos[v].y = (float)((t >> 1) * 8u) + L[t][v % 3u][1];
    }
}
static v2 ndc(v2 p) { return (v2){ p.x / 8.0f - 1.0f, 1.0f - p.y / 8.0f }; }

#define NIDX 64u
static uint32_t g_idx[NIDX];   /* vertex numbers 3..11: triangles 1..3 only, so that a negative base vertex of 3 is still in range and reaches triangle 0 */
static void make_indices(void) { for (uint32_t k = 0; k < NIDX / 3u + 1u; k++) for (uint32_t i = 0; i < 3u && 3u * k + i < NIDX; i++) g_idx[3u * k + i] = 3u * (1u + k % 3u) + (i + k) % 3u; }

/* 0 = clear colour, 1 = covered, 2 = not compared (within 0.03 px of an edge, or a degenerate triangle) */
static void raster(uint8_t *m, v2 a, v2 b, v2 c) {
    float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (absf(area) < 1e-4f) return;
    const v2 P[3] = { a, b, c };
    for (uint32_t y = 0; y < RT_H; y++) for (uint32_t x = 0; x < RT_W; x++) {
        float cx = (float)x + 0.5f, cy = (float)y + 0.5f; int pos = 0, neg = 0, near = 0;
        for (int i = 0; i < 3; i++) {
            v2 s = P[i], e = P[(i + 1) % 3];
            float len = len2f(e.x - s.x, e.y - s.y), d = ((e.x - s.x) * (cy - s.y) - (e.y - s.y) * (cx - s.x)) / len * (area > 0 ? 1.f : -1.f);
            if (d > 0.03f) pos++; else if (d < -0.03f) neg++; else near++;
        }
        uint8_t *q = &m[y * RT_W + x];
        if (pos == 3) *q = 1;
        else if (pos && neg) continue;
        else if (*q != 1) *q = 2;
    }
}
static uint32_t g_ndistinct, g_seen[64];
static uint32_t compare(const uint8_t *img, const uint8_t *m, uint32_t cfg) {
    uint32_t cmp = 0, red = 0, h = 2166136261u;
    for (uint32_t y = 0; y < RT_H; y++) for (uint32_t x = 0; x < RT_W; x++) {
        uint8_t e = m[y * RT_W + x]; h = (h ^ e) * 16777619u;
        if (e == 2) continue;
        cmp++; red += e == 1;
        uint32_t got = px(img, x, y), want = e ? RED : BLUE;
        if (got != want) { fprintf(stderr, "config %u: pixel (%u,%u) got %08x want %08x\n", cfg, x, y, got, want); assert(!"pixel differs from the CPU model"); }
    }
    assert(cmp >= 120 && red >= 8);   /* the comparison is meaningful: enough pixels, something drawn */
    bool seen = false;
    for (uint32_t i = 0; i < g_ndistinct; i++) seen |= g_seen[i] == h;
    if (!seen && g_ndistinct < 64) g_seen[g_ndistinct++] = h;
    return cmp;
}

/* ---- guest data ---- */
#define IB16_OFF 0u
#define IB32_OFF 0x800u
#define IB16_FAR 0x2000u
static void write_indices(void) {
    uint16_t a[NIDX]; uint32_t b[NIDX];
    for (uint32_t i = 0; i < NIDX; i++) a[i] = (uint16_t)g_idx[i], b[i] = g_idx[i];
    va_write(((uint64_t)VPN_IB << 14) + IB16_OFF, a, sizeof a);
    va_write(((uint64_t)VPN_IB << 14) + IB32_OFF, b, sizeof b);
    va_write(((uint64_t)VPN_IB << 14) + IB16_FAR, a, sizeof a);
}
static uint32_t g_junk = 0x7fc00001u;   /* a NaN: a stride / offset slip that reads padding gives a visibly wrong picture */
static void write_vb_attr(uint32_t vb_off) {   /* 24 byte vertices, the position (Float2) at +8 */
    uint8_t buf[NVERT * 24u];
    for (uint32_t i = 0; i < sizeof buf; i += 4) memcpy(buf + i, &g_junk, 4);
    for (uint32_t v = 0; v < NVERT; v++) { v2 n = ndc(g_pos[v]); memcpy(buf + v * 24u + 8u, &n, 8); }
    va_write(((uint64_t)VPN_VB << 14) + vb_off, buf, sizeof buf);
}
static void write_vb_pull(uint32_t vb_off, uint32_t stride_w, uint32_t posoff_w) {   /* {stride, posoff} then the vertices, 4-aligned */
    uint32_t w[2 + NVERT * 8u];
    for (uint32_t i = 0; i < 2 + NVERT * stride_w; i++) w[i] = g_junk;
    w[0] = stride_w, w[1] = posoff_w;
    for (uint32_t v = 0; v < NVERT; v++) { v2 n = ndc(g_pos[v]); memcpy(&w[2 + v * stride_w + posoff_w], &n, 8); }
    va_write(((uint64_t)VPN_VB << 14) + vb_off, w, (2 + NVERT * stride_w) * 4u);
}

/* ---- one draw description, its record, and its CPU model ---- */
typedef struct {
    int rec;                 /* 1 draw 0x01, 2 indexed 0x07 (bv 0) / 0x0b, 3 indexed wide 0x06 / 0x0a, 4 indexed indirect 0x11 */
    uint32_t first;          /* rec 1: first vertex */
    uint32_t type;           /* indexed: 0 = u16, 1 = u32 */
    uint32_t ioff;           /* indexed: byte offset into the index buffer */
    uint32_t istart;         /* rec 4: first index (elements) from the argument block */
    uint32_t count;
    int32_t  bv;
} draw_t;
static uint32_t g_args[5];
#define REF_IND 6u
#define VPN_IND 14u
static draw_t g_draw;
static uint32_t g_vb_off;
static void k_draw(void) {
    const draw_t *d = &g_draw;
    uint8_t r[48]; memset(r, 0, sizeof r);
    switch (d->rec) {
    case 1: p32(r, 0, 3), p16(r, 4, (uint16_t)d->first), p16(r, 6, (uint16_t)d->count); cb_cmd(1, r, 8); break;
    case 2:
        if (!d->bv) { p16(r, 0, 3), p16(r, 2, (uint16_t)d->type), p32(r, 4, REF_IB), p16(r, 8, (uint16_t)d->count), p16(r, 10, (uint16_t)d->ioff); cb_cmd(7, r, 12); }
        else { p16(r, 0, 3), p16(r, 2, (uint16_t)d->type), p32(r, 4, REF_IB), p16(r, 8, (uint16_t)d->ioff), p16(r, 10, (uint16_t)d->count), p16(r, 12, 1), p16(r, 14, (uint16_t)(int16_t)d->bv), p16(r, 16, 0); cb_cmd(11, r, 20); }
        break;
    case 3:
        if (!d->bv) { p16(r, 0, 3), p16(r, 2, (uint16_t)d->type), p32(r, 4, REF_IB), p64(r, 8, d->count), p64(r, 16, d->ioff); cb_cmd(6, r, 24); }
        else { p16(r, 0, 3), p16(r, 2, (uint16_t)d->type), p32(r, 4, REF_IB), p64(r, 8, d->ioff), p64(r, 16, d->count), p64(r, 24, 1), p64(r, 32, (uint64_t)(int64_t)d->bv), p64(r, 40, 0); cb_cmd(10, r, 48); }
        break;
    case 4: p16(r, 0, 3), p16(r, 2, (uint16_t)d->type), p32(r, 4, REF_IB), p32(r, 8, REF_IND), p64(r, 12, d->ioff), p64(r, 20, 0); cb_cmd(17, r, 28); break;
    }
}
/* the vertices the draw fetches, in order */
static uint32_t vertices_of(const draw_t *d, v2 *out) {
    uint32_t n = 0;
    for (uint32_t k = 0; k < d->count; k++) {
        int64_t vid;
        if (d->rec == 1) vid = (int64_t)d->first + k;
        else {
            uint32_t rel = d->type ? d->ioff - IB32_OFF : d->ioff >= IB16_FAR ? d->ioff - IB16_FAR : d->ioff;   /* from the array this offset lies in */
            uint32_t e = rel / (d->type ? 4u : 2u) + (d->rec == 4 ? d->istart : 0u) + k;
            assert(e < NIDX);
            vid = (int64_t)g_idx[e] + d->bv;
        }
        assert(vid >= 0 && vid < NVERT);
        out[n++] = g_pos[vid];
    }
    return n;
}
static void model_of(const draw_t *d, uint8_t *m) {
    v2 vs[64]; memset(m, 0, RT_W * RT_H);
    uint32_t n = vertices_of(d, vs);
    for (uint32_t i = 0; i + 2 < n + 0u && i + 3 <= n; i += 3) raster(m, vs[i], vs[i + 1], vs[i + 2]);
}

static lagfx_protocol_t *vrun(int mode, const draw_t *d, uint32_t vb_off, uint8_t *img) {
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    g_v.mode = mode; g_draw = *d; g_vb_off = vb_off;
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    lagfx_task_entry_t *t = def_task(p, 1);
    build_objects();
    write_indices();
    if (mode == M_ATTR) write_vb_attr(vb_off); else write_vb_pull(vb_off, g_v.stride_w, g_v.posoff_w);
    if (d->rec == 4) {
        memset(g_args, 0, sizeof g_args); g_args[0] = d->count, g_args[1] = 1, g_args[2] = d->istart, g_args[3] = (uint32_t)d->bv, g_args[4] = 0;
        va_write((uint64_t)VPN_IND << 14, g_args, sizeof g_args);
        put_buffer_object(REF_IND, 0x240, 0x4000, VPN_IND);
    }
    lagfx_a64_exec_set_pipeline_resolver(p, resolver_v, NULL);
    static const double blue[4] = { 0, 0, 1, 1 };
    cb_reset(); cb_pass(REF_TEX, 2, blue); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE); cb_vbuf(REF_VB, vb_off);
    k_draw();
    exec(p, 1, cb_finish(), true);
    assert(p->diag2.exec.run.submit_failed == 0);
    assert(lagfx_a64_exec_readback_texture(p, t, REF_TEX, img, RT_W * RT_H * 4, NULL, NULL));
    return p;
}
static void done(lagfx_protocol_t *p) { lagfx_a64_exec_drop_task(p, 1); lagfx_a64_exec_free(p); free(p); }

static uint32_t g_cfg;
static void check(int mode, draw_t d, uint32_t vb_off) {
    uint8_t img[RT_W * RT_H * 4], m[RT_W * RT_H];
    model_of(&d, m);
    lagfx_protocol_t *p = vrun(mode, &d, vb_off, img);
    assert(p->diag2.exec.run.draws == 1 && p->diag2.exec.run.draws_skipped == 0);
    compare(img, m, g_cfg++);
    done(p);
}

static void test_indexed_attr(void) {
    make_table(); make_indices();
    static const uint32_t offs[] = { 0, 2, 6, 10, 18, 30 };
    for (uint32_t i = 0; i < sizeof offs / sizeof *offs; i++)   /* 16 bit, compact record, different vertex buffer offsets (4-aligned) */
        check(M_ATTR, (draw_t){ .rec = 2, .type = 0, .ioff = offs[i], .count = 3u * (1u + i % 3u) }, 4u * (1u + 3u * i));
    check(M_ATTR, (draw_t){ .rec = 2, .type = 0, .ioff = IB16_FAR + 10, .count = 6 }, 8);   /* an offset deep in the page */
    static const int bvs[] = { 3, -3, 12 };
    for (uint32_t i = 0; i < 3; i++) {   /* base vertex (0x0b, i16) with an odd (2 byte) index offset */
        check(M_ATTR, (draw_t){ .rec = 2, .type = 0, .ioff = 2u * (2u * i + 1u), .count = 6, .bv = bvs[i] }, 16);
        check(M_ATTR, (draw_t){ .rec = 3, .type = 0, .ioff = 2u * (3u * i + 2u), .count = 6, .bv = bvs[i] }, 16);   /* wide forms: 0x06 / 0x0a */
    }
    check(M_ATTR, (draw_t){ .rec = 3, .type = 0, .ioff = 12, .count = 9 }, 0);
    for (uint32_t i = 0; i < 3; i++)   /* 32 bit indices: offsets are multiples of 4 */
        check(M_ATTR, (draw_t){ .rec = 2, .type = 1, .ioff = IB32_OFF + 4u * (1u + 2u * i), .count = 3u * (i + 1u) }, 12);
    check(M_ATTR, (draw_t){ .rec = 3, .type = 1, .ioff = IB32_OFF + 20, .count = 6, .bv = 12 }, 12);
    /* indirect: index start (elements) from the argument block, the byte offset from the record; the two add up */
    check(M_ATTR, (draw_t){ .rec = 4, .type = 0, .ioff = 2, .istart = 3, .count = 6 }, 4);
    check(M_ATTR, (draw_t){ .rec = 4, .type = 0, .ioff = 6, .istart = 5, .count = 6, .bv = -3 }, 4);
    check(M_ATTR, (draw_t){ .rec = 4, .type = 1, .ioff = IB32_OFF + 8, .istart = 2, .count = 3, .bv = 3 }, 4);
    /* plain draws through the attribute layout: first vertex, 24 byte stride */
    check(M_ATTR, (draw_t){ .rec = 1, .first = 5, .count = 6 }, 20);
    check(M_ATTR, (draw_t){ .rec = 1, .first = 9, .count = 12 }, 0);
    puts("PASS: indexed draws (16 / 32 bit, odd offsets, compact / wide / indirect, base vertex) and attribute stride / offset match the CPU model on lavapipe");
}

static void test_pulling(void) {
    VkPhysicalDeviceProperties pr;
    vkGetPhysicalDeviceProperties(g_dev->vk->phys_device, &pr);
    uint32_t al = (uint32_t)pr.limits.minStorageBufferOffsetAlignment;
    static const struct { uint32_t off, stride, posoff; } L[] = {
        { 4, 2, 0 }, { 12, 3, 1 }, { 20, 5, 3 }, { 36, 7, 4 }, { 100, 4, 2 }, { 260, 6, 0 }, { 0, 3, 0 }, { 64, 8, 5 }, { 68, 5, 1 },
    };
    uint32_t gathered_expected = 0;
    for (uint32_t i = 0; i < sizeof L / sizeof *L; i++) {
        g_v.stride_w = L[i].stride, g_v.posoff_w = L[i].posoff;
        uint8_t img[RT_W * RT_H * 4], m[RT_W * RT_H];
        draw_t d[3] = { { .rec = 1, .first = 2u * i % 7u, .count = 3u * (1u + i % 4u) }, { .rec = 2, .type = 0, .ioff = 2u * (i + 1u), .count = 6, .bv = (i & 1u) ? 12 : 0 }, { .rec = 4, .type = 0, .ioff = 4, .istart = i, .count = 6, .bv = (i % 3u == 2u) ? -3 : 3 } };
        for (int k = 0; k < 3; k++) {
            model_of(&d[k], m);
            lagfx_protocol_t *p = vrun(M_PULL, &d[k], L[i].off, img);
            __typeof__(p->diag2.exec.run) *R = &p->diag2.exec.run;
            assert(R->draws == 1 && R->draws_skipped == 0);
            if (L[i].off % al) assert(R->ext_gathered == 1 && p->diag2.exec.pipe.desc_unaligned == 0), gathered_expected++;   /* the aligned private copy served this bind */
            else assert(R->ext_gathered == 0);
            compare(img, m, g_cfg++);
            done(p);
        }
    }
    printf("PASS: vertex pulling from a storage buffer at 4-aligned offsets (minStorageBufferOffsetAlignment %u, %u binds gathered), strides 2..8 words, attribute offsets, first vertex / index offset / base vertex match the CPU model\n", al, gathered_expected);
}

/* indexed draws the Metal rules forbid or the executor cannot serve are skipped and counted, never drawn from a wrong place */
static void test_index_refusals(void) {
    uint8_t img[RT_W * RT_H * 4];
    draw_t odd32 = { .rec = 2, .type = 1, .ioff = IB32_OFF + 2, .count = 3 };   /* a 32 bit index buffer offset of 2: not a multiple of the index size */
    lagfx_protocol_t *p = vrun(M_ATTR, &odd32, 0, img);
    assert(p->diag2.exec.run.draws == 0 && p->diag2.exec.run.draws_skipped == 1 && px(img, 3, 2) == BLUE && px(img, 5, 8) == BLUE);
    done(p);
    draw_t past = { .rec = 2, .type = 0, .ioff = 0x4000 - 4, .count = 6 };      /* runs past the 16 KiB index buffer */
    p = vrun(M_ATTR, &past, 0, img);
    assert(p->diag2.exec.run.draws == 0 && p->diag2.exec.run.draws_skipped == 1);
    done(p);
    puts("PASS: misaligned / out of range index buffer ranges are skipped and counted");
}

/* the compositor's 4-attribute vertex from a REAL descriptor (pso 19): positions, and the Half4 colour at +32 of a 48 byte vertex, must arrive */
static void test_real_layout(void) {
    make_table();
    assert(lagfx_a64_pdesc_decode(k_pso19, sizeof k_pso19, NULL, NULL, &g_v.pd) && g_v.pd.nattrs == 4 && g_v.pd.stride == 48 && g_v.pd.attrs[0].bufidx == 1);
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    g_v.mode = M_ATTR4;
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    lagfx_task_entry_t *t = def_task(p, 1);
    build_objects();
    uint8_t vb[12 * 48], junk[4] = { 0xde, 0xad, 0xbe, 0xef };
    for (uint32_t i = 0; i < sizeof vb; i++) vb[i] = junk[i & 3];
    for (uint32_t v = 0; v < 12; v++) {
        v2 n = ndc(g_pos[v]); float pos[4] = { n.x, n.y, 0.f, 1.f }, uv[2] = { 0.1f, 0.2f }; uint16_t h[4] = { 0x3c00, 0x3800, 0x3400, 0x3c00 };   /* colour (1, 0.5, 0.25, 1) */
        memcpy(vb + v * 48u, pos, 16), memcpy(vb + v * 48u + 16, uv, 8), memcpy(vb + v * 48u + 24, uv, 8), memcpy(vb + v * 48u + 32, h, 8);
    }
    va_write(((uint64_t)VPN_VB << 14) + 48, vb, sizeof vb);   /* bound at offset 48 */
    put_buffer_object(REF_VB, 0x200, 0x4000, VPN_VB);
    lagfx_a64_exec_set_pipeline_resolver(p, resolver_v, NULL);
    static const double blue[4] = { 0, 0, 1, 1 };
    cb_reset(); cb_pass(REF_TEX, 2, blue); cb_viewport(RT_W, RT_H); cb_pipeline(REF_PIPE);
    { uint8_t r[20]; uint32_t idx = 1, cnt = 1, ref = REF_VB; uint64_t off = 48; memcpy(r, &idx, 4); memcpy(r + 4, &cnt, 4); memcpy(r + 8, &ref, 4); memcpy(r + 12, &off, 8); cb_cmd(125, r, 20); }   /* buffer slot 1 */
    { uint8_t r[8]; p32(r, 0, 3), p16(r, 4, 0), p16(r, 6, 6); cb_cmd(1, r, 8); }   /* triangles 0 and 1 */
    exec(p, 1, cb_finish(), true);
    uint8_t img[RT_W * RT_H * 4], m[RT_W * RT_H];
    assert(p->diag2.exec.run.submit_failed == 0 && p->diag2.exec.run.draws == 1 && lagfx_a64_exec_readback_texture(p, t, REF_TEX, img, sizeof img, NULL, NULL));
    draw_t d = { .rec = 1, .first = 0, .count = 6 };
    model_of(&d, m);
    uint32_t covered = 0;
    for (uint32_t y = 0; y < RT_H; y++) for (uint32_t x = 0; x < RT_W; x++) {
        uint32_t got = px(img, x, y), e = m[y * RT_W + x];
        if (e == 2) continue;
        if (!e) { assert(got == BLUE); continue; }
        covered++;   /* B8G8R8A8 word: A R G B; (1, 0.5, 0.25, 1) -> R 255, G 127 / 128, B 63 / 64 */
        uint32_t r = got >> 16 & 255, g = got >> 8 & 255, b = got & 255;
        if (!(got >> 24 == 255 && r == 255 && (g == 127 || g == 128) && (b == 63 || b == 64))) { fprintf(stderr, "pixel (%u,%u) = %08x\n", x, y, got); assert(!"colour of the Half4 attribute at +32 did not arrive"); }
    }
    assert(covered >= 40);
    done(p);
    puts("PASS: the 4-attribute 48 byte compositor vertex of a real descriptor (Float4 @0, Half4 colour @32, buffer slot 1 at an offset) renders its colour on lavapipe");
}

int main(void) {
    g_mem = calloc(1, MEM_SIZE);
    g_dev = make_device();
    assert(g_dev);
    if (!(g_dev->vk && g_dev->vk->initialized)) { puts("SKIP: no Vulkan device"); lagfx_device_free(g_dev); free(g_mem); return 77; }
    test_indexed_attr();
    test_pulling();
    test_index_refusals();
    test_real_layout();
    assert(g_ndistinct >= 20);   /* the configurations produce many different pictures: a wrong index / offset would not go unnoticed */
    printf("%u distinct expected pictures over %u configurations\n", g_ndistinct, g_cfg);
    lagfx_device_free(g_dev); free(g_mem);
    return 0;
}
#else
int main(void) { puts("SKIP: built without Vulkan"); return 77; }
#endif
