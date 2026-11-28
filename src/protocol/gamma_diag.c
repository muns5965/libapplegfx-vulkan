/*
 * libapplegfx-vulkan — gamma-descriptor diagnostics, stateful part. See gamma_diag.h. Logging and counters, and the accepted table; applying it is vulkan/gamma_apply.c (opt-in).
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 */
#include "gamma_diag.h"
#include "state.h"
#include "device.h"
#include "common/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define INC(x) __atomic_add_fetch(&(x), 1u, __ATOMIC_RELAXED)
#define GET(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)

#define GD_MAX_REREADS 16u   /* changed-table re-validations after the first acceptance */
#define GD_MAX_REGION_ATTEMPTS 8u   /* region read attempts while none has succeeded (was 3 before the descriptor check lost its cap) */
#define GD_MAX_DESC_LINES      64u
#define GD_MAX_PARSE_LINES     8u
#define GD_MAX_WALK_LINES      64u
#define GD_MAX_PROBE_LINES     64u
#define GD_MAX_RAM_LINES       12u
#define GD_MAX_MAP_LINES       12u
#define GD_MAX_TASKS_PER_EVENT 16u
#define GD_RAM_BASE_DEFAULT    0x70000000ull   /* the guest RAM base the cboot log of the integ4 run prints (ram_base=0x70000000); LAGFX_GD_RAM_BASE overrides */
#define GD_PROBE_OFF           0x118000ull     /* the offset whose address space is the open question */

static lagfx_gd_reader_t s_ex;
void lagfx_gd_set_reader(lagfx_gd_reader_t r) { s_ex = r; }

static lagfx_gd_stats_t s_st;
static uint64_t s_desc_lines, s_parse_lines, s_walk_lines, s_probe_lines, s_ram_lines, s_map_lines, s_probe_reads, s_probe_match;
static bool s_have_last, s_have_nonzero; static lagfx_gd_desc_t s_last, s_nonzero;
static int s_busy;
/* selected-LUT record (see lagfx_gd_select_t); guarded by a tiny spin flag so the getter is safe against the op 0x08 thread */
static lagfx_gd_select_t s_sel; static uint8_t s_sel_data[3u * 4096u * 4u], s_pend_data[3u * 4096u * 4u]; static int s_sel_lock; static uint32_t s_sel_gen; static uint32_t s_rereads;   /* s_pend_data: the candidate table, committed with s_sel under ONE lock (no torn table / state); only the op 0x08 thread writes it */
static void sel_lock(void) { while (__atomic_test_and_set(&s_sel_lock, __ATOMIC_ACQUIRE)) { } }
static void sel_unlock(void) { __atomic_clear(&s_sel_lock, __ATOMIC_RELEASE); }

void lagfx_gd_test_reset(void) {
    memset(&s_st, 0, sizeof s_st);
    s_desc_lines = s_parse_lines = s_walk_lines = s_probe_lines = s_ram_lines = s_map_lines = s_probe_reads = s_probe_match = 0;
    s_have_last = s_have_nonzero = false;
    memset(&s_last, 0, sizeof s_last); memset(&s_nonzero, 0, sizeof s_nonzero);
    s_rereads = 0;
    sel_lock(); memset(&s_sel, 0, sizeof s_sel); sel_unlock();   /* s_sel_gen is NOT reset: a consumer's cache keyed by gen must never see a reused number */
    __atomic_clear(&s_busy, __ATOMIC_RELEASE);
}

void lagfx_gd_reset(void) { lagfx_gd_test_reset(); }

void lagfx_gd_stats_get(lagfx_gd_stats_t *o) {
    if (!o) return;
    o->rereads = GET(s_st.rereads); o->read_attempts = GET(s_st.read_attempts); o->read_done = GET(s_st.read_done); o->translate_ok = GET(s_st.translate_ok);
    o->unmapped = GET(s_st.unmapped); o->phys_read_fail = GET(s_st.phys_read_fail); o->descriptor_parse_fail = GET(s_st.descriptor_parse_fail);
    o->applied_ok = 0;   /* nothing applies a gamma table: this is a constant, not a counter */
    o->sum_match = GET(s_st.sum_match); o->sum_mismatch = GET(s_st.sum_mismatch);
    o->descs_seen = GET(s_st.descs_seen); o->desc_changes = GET(s_st.desc_changes);
    o->sel_accepted = GET(s_st.sel_accepted); o->sel_ambiguous = GET(s_st.sel_ambiguous); o->sel_no_match = GET(s_st.sel_no_match);
    o->sel_rejected_mismatch = GET(s_st.sel_rejected_mismatch); o->sel_rejected_nofit = GET(s_st.sel_rejected_nofit); o->sel_rejected_unreadable = GET(s_st.sel_rejected_unreadable);
}

size_t lagfx_gd_stats_suffix(char *out, size_t cap) {
    lagfx_gd_stats_t s; lagfx_gd_stats_get(&s);
    int n = snprintf(out, cap, " translate_ok=%llu unmapped=%llu phys_read_fail=%llu descriptor_parse_fail=%llu applied_ok=%llu sum_match=%llu sum_mismatch=%llu descs=%llu desc_changes=%llu probe_reads=%llu probe_sum_match=%llu",
                     (unsigned long long)s.translate_ok, (unsigned long long)s.unmapped, (unsigned long long)s.phys_read_fail, (unsigned long long)s.descriptor_parse_fail,
                     (unsigned long long)s.applied_ok, (unsigned long long)s.sum_match, (unsigned long long)s.sum_mismatch, (unsigned long long)s.descs_seen,
                     (unsigned long long)s.desc_changes, (unsigned long long)GET(s_probe_reads), (unsigned long long)GET(s_probe_match));
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

uint32_t lagfx_gd_select_gen(void) { sel_lock(); const uint32_t g = s_sel.state == LAGFX_GD_SEL_ACCEPTED ? s_sel.gen : 0u; sel_unlock(); return g; }

size_t lagfx_gd_select_get(lagfx_gd_select_t *o, uint8_t *copy, size_t cap) {
    size_t n = 0;
    sel_lock();
    if (o) *o = s_sel;
    if (s_sel.state == LAGFX_GD_SEL_ACCEPTED && copy) { n = (size_t)s_sel.nwords * 4u; if (n > cap) n = cap; memcpy(copy, s_sel_data, n); }
    sel_unlock();
    return n;
}

/* lagfx_gd_task_read / lagfx_gd_task_read_bool live in diag.c: they need the arm64 / legacy translators, and this module must not pull the protocol layers into every static-library consumer. */

/* ---- logging ---- */
static void gd_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void gd_say(const char *fmt, ...) {
    char line[2400];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    fprintf(stderr, "%s\n", line);
    LAGFX_LOG("%s", line);
}
static size_t hexs(char *o, size_t cap, const uint8_t *b, size_t n) {
    size_t k = 0;
    for (size_t i = 0; i < n && k + 4 < cap; i++) k += (size_t)snprintf(o + k, cap - k, "%02x%s", b[i], (i + 1 < n && (i & 3u) == 3u) ? " " : "");
    return k;
}
static const char *rd_name(int r) { return r == LAGFX_GD_RD_OK ? "ok" : r == LAGFX_GD_RD_UNMAPPED ? "unmapped" : r == LAGFX_GD_RD_PHYS_FAIL ? "phys_read_fail" : "no_task"; }

static int rd_any(lagfx_protocol_t *p, lagfx_dd_reader_t reader, uint32_t task, uint64_t va, uint32_t len, uint8_t *buf) {
    if (s_ex) return s_ex(p, task, va, len, buf);
    return reader && reader(p, task, va, len, buf) ? LAGFX_GD_RD_OK : LAGFX_GD_RD_UNMAPPED;   /* a bool reader cannot say why: counted as unmapped */
}

/* ---- level-by-level 16 KiB walk (arm64 geometry; read-only; mirrors lagfx_arm64_task_translate, which the unit tests compare it against) ---- */
static bool gd_rd_mem(lagfx_protocol_t *p, uint64_t gpa, uint64_t n, void *dst) {
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    return dev && dev->desc.shell.read_memory && dev->desc.shell.read_memory(dev->desc.shell.opaque, gpa, n, dst);
}
static uint32_t le32p(const uint8_t *b) { return lagfx_dd_rd32(b); }

/* Appends the walk of one VA to o. Returns true when it ends in a data page. */
static bool gd_walk_va(lagfx_protocol_t *p, uint32_t top, uint32_t depth, uint64_t va, char *o, size_t cap) {
    size_t k = (size_t)snprintf(o, cap, "va=0x%llx:", (unsigned long long)va);
    uint64_t vpn = va >> 14; uint32_t page = top;
    for (int level = (int)depth - 1; level >= 0 && k + 80 < cap; level--) {
        uint32_t idx = (uint32_t)((vpn >> (12 * level)) & 0xfffu);
        uint8_t w[4];
        if (!page) { snprintf(o + k, cap - k, " UNMAPPED@L%d(pointer 0)", level); return false; }
        uint64_t egpa = ((uint64_t)page << 14) + 4u * idx;
        if (!gd_rd_mem(p, egpa, 4, w)) { snprintf(o + k, cap - k, " NOMEM@L%d(gpa=0x%llx)", level, (unsigned long long)egpa); return false; }
        uint32_t pte = le32p(w);
        k += (size_t)snprintf(o + k, cap - k, " L%d[0x%x]=0x%x", level, idx, pte);
        page = pte & 0x7fffffffu;
    }
    if (!page) { snprintf(o + k, cap - k, " UNMAPPED"); return false; }
    snprintf(o + k, cap - k, " ->gpa=0x%llx", ((unsigned long long)page << 14) + (va & 0x3fffu));
    return true;
}

static void gd_walk_task(lagfx_protocol_t *p, uint64_t n, const lagfx_task_entry_t *t) {
    if (__atomic_fetch_add(&s_walk_lines, 1u, __ATOMIC_RELAXED) >= GD_MAX_WALK_LINES) return;
    char line[2200]; size_t k;
    if (!lagfx_proto_arm64(p)) {
        gd_say("g7-pvg-gamma: walk swap#%llu task=%u skipped: protocol_mode is x86 (legacy 4 KiB geometry), the 16 KiB walk does not apply", (unsigned long long)n, t->id);
        return;
    }
    uint8_t hdr[8];
    k = (size_t)snprintf(line, sizeof line, "g7-pvg-gamma: walk swap#%llu task=%u root_pfn=0x%llx root_gpa=0x%llx", (unsigned long long)n, t->id,
                         (unsigned long long)t->root_page_pfn, (unsigned long long)(t->root_page_pfn << 14));
    if (!t->root_page_pfn || !gd_rd_mem(p, t->root_page_pfn << 14, 8, hdr)) { gd_say("%s root page unreadable (geometry: root_pfn<<14, 16 KiB pages, 12 index bits per level)", line); return; }
    uint32_t top = le32p(hdr), depth = le32p(hdr + 4);
    k += (size_t)snprintf(line + k, sizeof line - k, " {top=0x%x depth=%u}", top, depth);
    if (depth < 1u || depth > 3u) { gd_say("%s BAD DEPTH (the walk accepts 1..3)", line); return; }
    static const uint64_t vas[] = { 0x114000, 0x118000, 0x11c000, 0x120000, 0x124000, 0x128000, 0x12c000, 0x130000, 0x134000 };
    for (size_t i = 0; i < sizeof vas / sizeof vas[0] && k + 200 < sizeof line; i++) {
        k += (size_t)snprintf(line + k, sizeof line - k, " | %s", i == 0 ? "control " : "");
        char one[300]; gd_walk_va(p, top, depth, vas[i], one, sizeof one);
        k += (size_t)snprintf(line + k, sizeof line - k, "%s", one);
    }
    gd_say("%s", line);
}

/* ---- read-only probes of the open "which address space" question ---- */
static void gd_ram_probe(lagfx_protocol_t *p, uint64_t n) {
    if (__atomic_fetch_add(&s_ram_lines, 1u, __ATOMIC_RELAXED) >= GD_MAX_RAM_LINES) return;
    const char *e = getenv("LAGFX_GD_RAM_BASE");
    uint64_t base = e && *e ? strtoull(e, NULL, 0) : GD_RAM_BASE_DEFAULT, gpa = base + GD_PROBE_OFF;
    uint8_t b[64]; char hx[160];
    if (gd_rd_mem(p, gpa, sizeof b, b)) { hexs(hx, sizeof hx, b, sizeof b); gd_say("g7-pvg-gamma: ram-probe swap#%llu gpa=0x%llx (RAM base 0x%llx [I] + 0x118000) first64: %s (no interpretation; AArch64 code words would decode as NOP/ldr/stur...)", (unsigned long long)n, (unsigned long long)gpa, (unsigned long long)base, hx); }
    else gd_say("g7-pvg-gamma: ram-probe swap#%llu gpa=0x%llx (RAM base 0x%llx [I] + 0x118000) read FAILED (no RAM there)", (unsigned long long)n, (unsigned long long)gpa, (unsigned long long)base);
}

/* offset 0x118000 inside the swap's mapping: descriptor layout per vulkan/surface_writeback.h (+0 page list, +0x18 alloc, +0x60 first-page offset; entry bit 0 = present;
 * the surface is the 16 KiB pages back to back [I]: surface byte offset o is in entry o >> 14 at o & 0x3fff). Parsed here so this module needs no Vulkan header. */
static void gd_map_probe(lagfx_protocol_t *p, uint64_t n, uint64_t mapping) {
    if (__atomic_fetch_add(&s_map_lines, 1u, __ATOMIC_RELAXED) >= GD_MAX_MAP_LINES) return;
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    uint8_t d[256]; size_t dl = 0;
    memset(d, 0, sizeof d);
    if (!dev || !dev->desc.shell.get_surface_desc || mapping > 0xffffffffull || !dev->desc.shell.get_surface_desc(dev->desc.shell.opaque, (uint32_t)mapping, d, sizeof d, &dl) || dl < 0x64u || dl > sizeof d) {
        gd_say("g7-pvg-gamma: map-probe swap#%llu mapping=%llu no surface descriptor from the shell", (unsigned long long)n, (unsigned long long)mapping);
        return;
    }
    uint32_t list = le32p(d); uint64_t alloc = le32p(d + 0x18) | (uint64_t)le32p(d + 0x1c) << 32; uint32_t first = le32p(d + 0x60);
    uint64_t npages = (alloc + first + 0x3fffu) >> 14;
    if (!(list & 1u) || npages == 0 || npages > 4096u) { gd_say("g7-pvg-gamma: map-probe swap#%llu mapping=%llu descriptor not usable (list=0x%x alloc=0x%llx first=0x%x npages=%llu)", (unsigned long long)n, (unsigned long long)mapping, list, (unsigned long long)alloc, first, (unsigned long long)npages); return; }
    uint64_t pi = GD_PROBE_OFF >> 14;
    if (pi >= npages) { gd_say("g7-pvg-gamma: map-probe swap#%llu mapping=%llu too small for offset 0x118000: npages=%llu (0x%llx bytes of pages) alloc=0x%llx", (unsigned long long)n, (unsigned long long)mapping, (unsigned long long)npages, (unsigned long long)(npages << 14), (unsigned long long)alloc); return; }
    uint8_t e[4];
    if (!gd_rd_mem(p, ((uint64_t)(list & ~1u) << 12) + 4u * pi, 4, e)) { gd_say("g7-pvg-gamma: map-probe swap#%llu mapping=%llu page list unreadable", (unsigned long long)n, (unsigned long long)mapping); return; }
    uint32_t ent = le32p(e);
    if (!(ent & 1u)) { gd_say("g7-pvg-gamma: map-probe swap#%llu mapping=%llu page %llu of %llu not present (entry 0x%x)", (unsigned long long)n, (unsigned long long)mapping, (unsigned long long)pi, (unsigned long long)npages, ent); return; }
    uint64_t gpa = (uint64_t)(ent & ~1u) << 12;
    uint8_t b[64]; char hx[160];
    if (!gd_rd_mem(p, gpa, sizeof b, b)) { gd_say("g7-pvg-gamma: map-probe swap#%llu mapping=%llu page gpa=0x%llx read FAILED", (unsigned long long)n, (unsigned long long)mapping, (unsigned long long)gpa); return; }
    hexs(hx, sizeof hx, b, sizeof b);
    gd_say("g7-pvg-gamma: map-probe swap#%llu mapping=%llu npages=%llu page %llu gpa=0x%llx first64: %s (surface pixels are RGhA, so floats here would be 16-bit halves; no interpretation)", (unsigned long long)n, (unsigned long long)mapping, (unsigned long long)npages, (unsigned long long)pi, (unsigned long long)gpa, hx);
}

/* every live task: read the descriptor's region and decode it (checksum / ramp). The task whose checksum matches is the owner of the table [I]. */
static void gd_lut_probe(lagfx_protocol_t *p, uint64_t n, const lagfx_gd_desc_t *d, lagfx_dd_reader_t reader) {
    uint32_t done = 0;
    uint8_t *buf = (uint8_t *)malloc(d->len);
    if (!buf) return;
    for (uint32_t i = 0; i < LAGFX_MAX_TASKS && done < GD_MAX_TASKS_PER_EVENT; i++) {
        if (!p->tasks[i].live) continue;
        done++;
        if (__atomic_fetch_add(&s_probe_lines, 1u, __ATOMIC_RELAXED) >= GD_MAX_PROBE_LINES) break;
        uint32_t id = p->tasks[i].id;
        int r = rd_any(p, reader, id, d->off, (uint32_t)d->len, buf);
        if (r != LAGFX_GD_RD_OK) { gd_say("g7-pvg-gamma: lut-probe swap#%llu task=%u va=0x%llx len=0x%llx read=%s", (unsigned long long)n, id, (unsigned long long)d->off, (unsigned long long)d->len, rd_name(r)); continue; }
        INC(s_probe_reads);
        lagfx_gd_lut_t l; lagfx_gd_lut_decode(buf, (size_t)d->len, d->count, d->sum, &l);
        if (l.sum_match) INC(s_probe_match);
        char hx[100]; hexs(hx, sizeof hx, buf, 16);
        gd_say("g7-pvg-gamma: lut-probe swap#%llu task=%u va=0x%llx len=0x%llx read=ok sum=0x%08x expect=0x%08x match=%d ramp_maxdev=%.6f monotonic=%d%d%d tail_zero=%d finite=%d first=%g,%g,%g last=%g,%g,%g head16: %s (INFER: 3 planes x %u float32; DIAGNOSTIC decode, never applied)",
               (unsigned long long)n, id, (unsigned long long)d->off, (unsigned long long)d->len, l.sum, d->sum, (int)l.sum_match, (double)l.maxdev, (int)l.monotonic[0], (int)l.monotonic[1], (int)l.monotonic[2],
               (int)l.tail_zero, (int)l.finite, (double)l.first[0], (double)l.first[1], (double)l.first[2], (double)l.last[0], (double)l.last[1], (double)l.last[2], hx, d->count);
    }
    free(buf);
}

static bool is_sample(uint64_t n) { return n <= 4u || (n <= 64u && (n & (n - 1u)) == 0u); }   /* 1 2 3 4 8 16 32 64 */

static bool gd_sel_accepted(void) { return GET(s_st.sel_accepted) != 0u; }

/* ---- the region read + address-space selection ----
 * A read only counts as ACCEPTED when the region decodes (3 * count words fit) AND its wrapping u32 word sum equals the descriptor's checksum. The preferred task (owner of the
 * mapping, else the last 0x44) is asked first; if it reads but is not accepted (or does not read), the last 0x44 and every other live task are asked in the same attempt. Exactly
 * one matching task is selected; two or more matching fallback tasks are reported as ambiguous and nothing is selected. A match by the preferred task ends the attempt (the
 * other tasks are not read: "owner asked first and alone" is kept). Reads per attempt <= LAGFX_MAX_TASKS + 1; attempts <= GD_MAX_REGION_ATTEMPTS. */
#define GD_SEL_MAX_CAND  (LAGFX_MAX_TASKS + 1u)
#define GD_SEL_LIST_MAX  16u
typedef struct { uint32_t task; const char *via; } gd_cand_t;
typedef struct { uint32_t task; const char *via; char line[420]; lagfx_gd_lut_t l; } gd_rec_t;

static void gd_region_read(lagfx_protocol_t *p, const lagfx_gd_desc_t *d, lagfx_dd_reader_t reader, lagfx_dd_owner_t owner_fn) {
    const uint64_t att = INC(s_st.read_attempts);
    /* The task that CREATED the display mapping (named by this very op 0x08) owns the region: ask it first. The last 0x44 is only a guess (vm2: last 0x44 = task 6, the
     * surface was created by task 2) and the same VA can be mapped in several tasks, so the scan over every other live task is a DIAGNOSTIC FALLBACK, reported as such. */
    uint32_t owner = 0; bool have_owner = owner_fn && owner_fn(p, d->mapping, &owner);
    const bool have_task = p->diag2.last44_global_valid != 0;
    if ((!have_task && !have_owner) || (!s_ex && !reader)) {
        INC(s_st.unmapped);   /* no task to ask: nothing could translate */
        gd_say("g7-pvg-gamma: region not read: %s (valid=%d task=%u owner=%s)", (s_ex || reader) ? "no live task for the last 0x44 and no owner task of the mapping" : "no reader registered", (int)p->diag2.last44_global_valid, p->diag2.last44_global_task, have_owner ? "known" : "unknown");
        return;
    }
    uint8_t *buf = (uint8_t *)malloc(d->len);
    if (!buf) return;
    /* candidate order: preferred, last 0x44 (if different), every other live task */
    gd_cand_t cand[GD_SEL_MAX_CAND]; uint32_t nc = 0;
    const uint32_t pref = have_owner ? owner : p->diag2.last44_global_task;
    cand[nc].task = pref; cand[nc++].via = have_owner ? "owner" : "last44";
    if (have_owner && have_task && p->diag2.last44_global_task != owner) { cand[nc].task = p->diag2.last44_global_task; cand[nc++].via = "last44(owner not accepted)"; }
    for (uint32_t i = 0; i < LAGFX_MAX_TASKS && nc < GD_SEL_MAX_CAND; i++) {
        if (!p->tasks[i].live || (have_owner && p->tasks[i].id == owner) || (have_task && p->tasks[i].id == p->diag2.last44_global_task)) continue;
        cand[nc].task = p->tasks[i].id; cand[nc++].via = "scan(DIAGNOSTIC FALLBACK: neither the owner nor the last 0x44 was accepted; the same VA may exist in several tasks)";
    }
    gd_rec_t first, acc; bool have_first = false, have_acc = false;
    uint32_t tried = 0, nmatch = 0, mtasks[4] = {0}, nread = 0, nmis = 0, nfit = 0, nunr = 0;
    bool any_translated = false, ok = false, pref_unreadable = false;
    char rej[300]; size_t rk = 0; rej[0] = 0;
    for (uint32_t i = 0; i < nc; i++) {
        tried++;
        int r = rd_any(p, reader, cand[i].task, d->off, (uint32_t)d->len, buf);
        any_translated |= r == LAGFX_GD_RD_OK || r == LAGFX_GD_RD_PHYS_FAIL;
        if (r != LAGFX_GD_RD_OK) {
            nunr++; INC(s_st.sel_rejected_unreadable); if (i == 0) pref_unreadable = true;
            if (rk + 40 < sizeof rej) rk += (size_t)snprintf(rej + rk, sizeof rej - rk, " task=%u:%s", cand[i].task, rd_name(r));
            continue;
        }
        ok = true; nread++;
        lagfx_gd_lut_t l; lagfx_gd_lut_decode(buf, (size_t)d->len, d->count, d->sum, &l);
        gd_rec_t *rec = NULL;
        if (!have_first) { rec = &first; have_first = true; }
        const bool match = l.fits && l.sum_match;
        if (match) {
            if (nmatch < 4u) mtasks[nmatch] = cand[i].task;
            nmatch++;
            if (nmatch == 1u) {
                rec = &acc; have_acc = true;
                memcpy(s_pend_data, buf, (size_t)l.nwords * 4u);   /* the validated words (committed below together with the state); the rest of the page is not kept */
            }
        } else if (l.fits) {
            nmis++; INC(s_st.sel_rejected_mismatch);
            if (rk + 48 < sizeof rej) rk += (size_t)snprintf(rej + rk, sizeof rej - rk, " task=%u:sum=0x%08x!=0x%08x", cand[i].task, l.sum, d->sum);
        } else {
            nfit++; INC(s_st.sel_rejected_nofit);
            if (rk + 40 < sizeof rej) rk += (size_t)snprintf(rej + rk, sizeof rej - rk, " task=%u:nofit", cand[i].task);
        }
        if (rec) {
            lagfx_dd_gamma_t g; lagfx_dd_gamma_summarize(buf, d->len, &g); lagfx_dd_gamma_format(rec->line, sizeof rec->line, &g);
            rec->task = cand[i].task; rec->l = l;
            /* via text keeps the older strings: "last44(owner failed)" when the owner could not even read */
            rec->via = (i == 1u && have_owner && pref_unreadable) ? "last44(owner failed)" : cand[i].via;
        }
        if (match && i == 0) break;   /* the preferred task matched: stop, the others are not asked */
    }
    const gd_rec_t *shown = have_acc ? &acc : have_first ? &first : NULL;
    if (ok) {
        INC(s_st.read_done); INC(s_st.translate_ok);
        if (nmatch == 1u) { INC(s_st.sel_accepted); INC(s_st.sum_match); }
        else { INC(s_st.sum_mismatch); if (nmatch >= 2u) INC(s_st.sel_ambiguous); else INC(s_st.sel_no_match); }
        gd_say("g7-pvg-gamma: region task=%u via=%s (owner=%d last 0x44 task=%u, %u task(s) tried) va=0x%llx len=0x%llx %s lut_decode[fits=%d sum=0x%08x expect=0x%08x match=%d ramp_maxdev=%.6f] (INFER: the descriptor's offset is a task virtual address; the checksum is the wrapping u32 sum of 3*count words, see gamma_diag.h; a read that succeeds in the wrong task does NOT match; applied only with LAGFX_GAMMA_APPLY=1)",
               shown->task, shown->via, have_owner ? (int)owner : -1, p->diag2.last44_global_task, tried, (unsigned long long)d->off, (unsigned long long)d->len, shown->line, (int)shown->l.fits, shown->l.sum, d->sum, (int)shown->l.sum_match, (double)shown->l.maxdev);
        /* the one selection line per attempt (<= GD_MAX_REGION_ATTEMPTS lines in total) */
        char selfld[40]; selfld[0] = 0;
        if (nmatch) snprintf(selfld, sizeof selfld, " %s=%u", nmatch == 1u ? "task" : "first_match", mtasks[0]);
        gd_say("g7-pvg-gamma: gamma-select attempt=%llu result=%s%s (owner=%d preferred=%u) tried=%u readable=%u rejected[mismatch=%u nofit=%u unreadable=%u]:%s (selection; the table is applied to the scanout bytes only with LAGFX_GAMMA_APPLY=1, see vulkan/gamma_apply.h)",
               (unsigned long long)att, nmatch == 1u ? "ACCEPTED" : nmatch >= 2u ? "AMBIGUOUS" : "NO_MATCH", selfld, have_owner ? (int)owner : -1, pref,
               tried, nread, nmis, nfit, nunr, rk ? rej : " none");
        if (nmatch >= 2u) { gd_say("g7-pvg-gamma: gamma-select AMBIGUOUS: %u tasks match the checksum (first %u, %u): not choosing", nmatch, mtasks[0], mtasks[1]); }
        sel_lock();
        if (nmatch == 0u) memset(&s_sel, 0, sizeof s_sel);   /* no stale AMBIGUOUS from an earlier attempt */
        if (nmatch == 1u) {
            memset(&s_sel, 0, sizeof s_sel);
            s_sel.state = LAGFX_GD_SEL_ACCEPTED; s_sel.task = mtasks[0]; s_sel.preferred = mtasks[0] == pref; s_sel.count = d->count; s_sel.sum = d->sum;
            s_sel.va = d->off; s_sel.len = d->len; s_sel.nwords = acc.l.nwords; s_sel.attempt = (uint32_t)att; s_sel.gen = ++s_sel_gen;
            memcpy(s_sel_data, s_pend_data, (size_t)acc.l.nwords * 4u);
        } else if (nmatch >= 2u) {
            memset(&s_sel, 0, sizeof s_sel);
            s_sel.state = LAGFX_GD_SEL_AMBIGUOUS; s_sel.ntasks = nmatch; for (uint32_t k = 0; k < 4u && k < nmatch; k++) s_sel.tasks[k] = mtasks[k];
            s_sel.count = d->count; s_sel.sum = d->sum; s_sel.va = d->off; s_sel.len = d->len; s_sel.attempt = (uint32_t)att;
        }
        sel_unlock();
    } else {
        if (any_translated) { INC(s_st.translate_ok); INC(s_st.phys_read_fail); } else INC(s_st.unmapped);
        gd_say("g7-pvg-gamma: region read FAILED task=%u owner=%d (%u task(s) tried) va=0x%llx len=0x%llx (INFER: offset is not a plain task VA, or the page is unmapped) reason=%s attempt=%llu",
               pref, have_owner ? (int)owner : -1, tried, (unsigned long long)d->off, (unsigned long long)d->len, any_translated ? "phys_read_fail" : "unmapped", (unsigned long long)att);
    }
    free(buf);
}

void lagfx_gd_note_op8(lagfx_protocol_t *p, uint32_t chan, const uint8_t *pl, uint32_t plen, uint32_t skip, lagfx_dd_reader_t reader, lagfx_dd_owner_t owner) {
    if (!p || !pl) return;
    if (__atomic_test_and_set(&s_busy, __ATOMIC_ACQUIRE)) return;   /* ch4 is drained serially; a second thread here would only duplicate diagnostics */
    lagfx_gd_desc_t d;
    int pr = lagfx_gd_desc_parse(pl, plen, skip, &d);
    const uint64_t n = INC(s_st.descs_seen);
    if (pr != LAGFX_GD_DESC_OK) {
        INC(s_st.descriptor_parse_fail);
        if (__atomic_fetch_add(&s_parse_lines, 1u, __ATOMIC_RELAXED) < GD_MAX_PARSE_LINES)
            gd_say("g7-pvg-gamma: op8 desc #%llu ch=%u PARSE FAIL: %s (payload_len=%u skip=%u off=0x%llx len=0x%llx count=%u) no read attempted", (unsigned long long)n, chan, pr == LAGFX_GD_DESC_SHORT ? "payload shorter than 10 words after the barrier slots" : "inconsistent off/len/count",
                   plen, skip, (unsigned long long)d.off, (unsigned long long)d.len, d.count);
        __atomic_clear(&s_busy, __ATOMIC_RELEASE);
        return;
    }
    if (!s_have_last || memcmp(&d, &s_last, sizeof d) != 0) {
        INC(s_st.desc_changes);
        if (__atomic_fetch_add(&s_desc_lines, 1u, __ATOMIC_RELAXED) < GD_MAX_DESC_LINES)
            gd_say("g7-pvg-gamma: op8 desc #%llu ch=%u mapping=%llu w1=%u off=0x%llx len=0x%llx count=%u sum=0x%08x%s (logged on change; every channel-4 op 0x08 is checked, no cap)", (unsigned long long)n, chan, (unsigned long long)d.mapping, d.w1,
                   (unsigned long long)d.off, (unsigned long long)d.len, d.count, d.sum, d.off == 0 ? " [zero descriptor: no table]" : "");
        s_last = d; s_have_last = true;
    }
    if (d.off != 0) { s_nonzero = d; s_have_nonzero = true; }
    if (d.off != 0 && !gd_sel_accepted() && GET(s_st.read_attempts) < GD_MAX_REGION_ATTEMPTS) gd_region_read(p, &d, reader, owner);
    else if (d.off != 0 && gd_sel_accepted() && s_rereads < GD_MAX_REREADS) {   /* a NEW table (another count / checksum) after one was accepted: validate it again. A zero descriptor never clears the selection (the live captures send the table once, zeros afterwards). A re-read that matches replaces the table; one that does not DROPS it (nothing is applied rather than a stale table). */
        lagfx_gd_select_t cur; lagfx_gd_select_get(&cur, NULL, 0);
        if (cur.count != d.count || cur.sum != d.sum) { s_rereads++; INC(s_st.rereads); gd_region_read(p, &d, reader, owner); }
    }
    if (is_sample(n)) {
        uint32_t k = 0;
        for (uint32_t i = 0; i < LAGFX_MAX_TASKS && k < GD_MAX_TASKS_PER_EVENT; i++) if (p->tasks[i].live) { k++; gd_walk_task(p, n, &p->tasks[i]); }
        gd_ram_probe(p, n);
        gd_map_probe(p, n, d.mapping);
        if (s_ex && s_have_nonzero && s_nonzero.len) gd_lut_probe(p, n, &s_nonzero, reader);   /* needs the status reader (the legacy bool test hook is not asked extra questions) */
    }
    __atomic_clear(&s_busy, __ATOMIC_RELEASE);
}
