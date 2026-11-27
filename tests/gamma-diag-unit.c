/*
 * libapplegfx-vulkan — gamma descriptor diagnostics: 16 KiB task reads, split counters, 257-entry table layout, address-space probes (links the library, no Vulkan device)
 * tests/gamma-diag-unit.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Pins one proven defect and the read-only diagnostics built around it:
 *   BUG  the gamma diagnostic read (diag.c dd_read_region) called the LEGACY translator lagfx_task_read_virtual (root = pfn << 12, 4 KiB pages, 10 index bits per level) on an
 *        arm64-vmapple device, whose task page tables are Apple's (root = pfn << 14, 16 KiB pages, 12 index bits per level; lagfx_arm64_task_read). Every scenario below that maps
 *        a page in the 16 KiB geometry therefore failed with the legacy reader ("fail-before") and succeeds with lagfx_gd_task_read ("pass-after").
 * Fixtures are synthetic 16 KiB page tables in a fake guest memory (root page {top, depth 2} -> top table -> leaf table -> data pages).
 */
#include "protocol/state.h"
#include "protocol/display_diag.h"
#include "protocol/gamma_diag.h"
#include "protocol/xlate_guard.h"
#include "protocol/diag.h"
#include "protocol/arm64_exec_decode.h"
#include "protocol/object_resolver.h"
#include "handlers/compute/task_translate.h"
#include "device.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MEM_SIZE (24u << 20)
static uint8_t *g_mem;
static bool rd(void *o, uint64_t gpa, uint64_t len, void *dst) { (void)o; if (gpa + len > MEM_SIZE || gpa + len < gpa) return false; memcpy(dst, g_mem + gpa, len); return true; }
static bool wr(void *o, uint64_t gpa, uint64_t len, const void *src) { (void)o; if (gpa + len > MEM_SIZE) return false; memcpy(g_mem + gpa, src, len); return true; }
static void put32(uint64_t gpa, uint32_t v) { memcpy(g_mem + gpa, &v, 4); }
static void w32(uint8_t *b, size_t o, uint32_t v) { memcpy(b + o, &v, 4); }

/* ---- 16 KiB task page tables ---- */
static uint32_t g_next;   /* next free 16 KiB pfn; all fixture pfns are >= 0x40 so that pfn << 12 (legacy) lands in untouched zero memory below 0x100000 */
typedef struct { uint32_t root, top, leaf; } tbl_t;
static uint32_t newpfn(void) { return g_next++; }
static tbl_t tbl_new(void) {
    tbl_t t = { newpfn(), newpfn(), newpfn() };
    put32((uint64_t)t.root << 14, t.top); put32(((uint64_t)t.root << 14) + 4, 2);
    put32((uint64_t)t.top << 14, t.leaf);   /* top index 0 -> leaf table: VAs below 4096 * 16 KiB = 64 MiB */
    return t;
}
static void map_vpn(tbl_t t, uint32_t vpn, uint32_t pfn) { put32(((uint64_t)t.leaf << 14) + 4u * vpn, pfn); }
static uint32_t map_new(tbl_t t, uint32_t vpn) { uint32_t pfn = newpfn(); map_vpn(t, vpn, pfn); return pfn; }

static lagfx_protocol_t *mk(uint32_t mode) {
    memset(g_mem, 0, MEM_SIZE); g_next = 0x40;
    lagfx_device_t *dev = calloc(1, sizeof *dev);
    dev->desc.shell.read_memory = rd; dev->desc.shell.write_memory = wr;
    lagfx_protocol_t *p = calloc(1, sizeof *p);
    p->magic = LAGFX_PROTOCOL_MAGIC; p->dev = dev; p->ring_page_shift = mode ? 14 : 0; p->protocol_mode = mode;
    lagfx_gd_test_reset(); if (ftruncate(2, 0) || lseek(2, 0, SEEK_SET) < 0) abort();   /* fresh counters and a fresh captured log per scenario */
    return p;
}
static void fin(lagfx_protocol_t *p) { free(p->dev); free(p); }
static lagfx_task_entry_t *add_task(lagfx_protocol_t *p, uint32_t id, uint64_t root_pfn) {
    lagfx_task_entry_t *t = lagfx_protocol_alloc_task_slot(p);
    assert(t); t->id = id; t->live = true; t->root_page_pfn = root_pfn;
    return t;
}
/* a byte pattern that depends on the VA (so a wrong page or a wrong order shows) and on a per-task seed */
static uint8_t pat(uint64_t va, uint32_t seed) { return (uint8_t)((va * 131u) ^ (va >> 8) ^ seed); }
static void fill_va(tbl_t t, uint64_t va, uint32_t n, uint32_t seed) {   /* host-side write through the same tables (asserts mapped) */
    for (uint32_t i = 0; i < n; i++, va++) {
        uint32_t pfn; memcpy(&pfn, g_mem + ((uint64_t)t.leaf << 14) + 4u * (uint32_t)(va >> 14), 4);
        assert(pfn);
        g_mem[((uint64_t)(pfn & 0x7fffffffu) << 14) + (va & 0x3fff)] = pat(va, seed);
    }
}
static bool check_va(const uint8_t *b, uint64_t va, uint32_t n, uint32_t seed) { for (uint32_t i = 0; i < n; i++) if (b[i] != pat(va + i, seed)) return false; return true; }

/* ---- the op 0x08 command as the display dispatcher hands it to lagfx_dd_note_cmd ---- */
static void op8(lagfx_protocol_t *p, uint64_t mapping, uint64_t off, uint64_t len, uint32_t count, uint32_t sum) {
    uint8_t c[52]; memset(c, 0, sizeof c);
    w32(c, 0, 0x08); w32(c, 4, 52); w32(c, 8, 0x400);
    w32(c, 12 + 8, (uint32_t)mapping); w32(c, 12 + 12, (uint32_t)(mapping >> 32));
    w32(c, 12 + 16, (uint32_t)off); w32(c, 12 + 20, (uint32_t)(off >> 32));
    w32(c, 12 + 24, (uint32_t)len); w32(c, 12 + 28, (uint32_t)(len >> 32));
    w32(c, 12 + 32, count); w32(c, 12 + 36, sum);
    lagfx_dd_note_cmd(p, 1, 4, c, 52);
}
static uint8_t sd[0x64];
static bool getdesc(void *o, uint32_t id, uint8_t *out, size_t cap, size_t *len) { (void)o; if (id != 1u || cap < sizeof sd) return false; memcpy(out, sd, sizeof sd); *len = sizeof sd; return true; }
static uint32_t g_owner_task; static uint64_t g_owner_map; static bool g_owner_on;
static bool owner_fn(lagfx_protocol_t *p, uint64_t m, uint32_t *t) { (void)p; if (!g_owner_on || m != g_owner_map) return false; *t = g_owner_task; return true; }
/* the BEFORE state: dd_read_region called the legacy reader unconditionally and nothing classified failures */
/* The legacy 4 KiB walk as it ran BEFORE g7-addrfix: lagfx_task_read_virtual now redirects to the arm64 reader whenever protocol_mode is arm64, so the old geometry
 * is exercised by presenting the same tables in x86 mode (integ6: this test's fail-before premise was invalidated by the addrfix redirect). */
static bool legacy_geom_read(lagfx_protocol_t *p, lagfx_task_entry_t *te, uint64_t va, uint32_t len, uint8_t *out) {
    uint32_t m = p->protocol_mode; p->protocol_mode = 0;
    bool ok = lagfx_task_read_virtual(p, te, va, len, out);
    p->protocol_mode = m;
    return ok;
}
static bool legacy_bool(lagfx_protocol_t *p, uint32_t task, uint64_t va, uint32_t len, uint8_t *out) {
    lagfx_task_entry_t *t = lagfx_protocol_find_task(p, task);
    return t && legacy_geom_read(p, t, va, len, out);
}
static void use_fixed(void) { lagfx_dd_set_reader(lagfx_gd_task_read_bool); lagfx_gd_set_reader(lagfx_gd_task_read); lagfx_dd_set_owner_lookup(owner_fn); }
static void use_legacy(void) { lagfx_dd_set_reader(legacy_bool); lagfx_gd_set_reader(NULL); lagfx_dd_set_owner_lookup(owner_fn); }

/* identity table: 3 planes x 257 float32, entry i = i / 256 (the layout read from the older host logs, see gamma_diag.h) */
static void ident_table(uint8_t *b, size_t len) {
    memset(b, 0, len);
    for (int c = 0; c < 3; c++) for (uint32_t i = 0; i < 257; i++) { float f = (float)i / 256.0f; memcpy(b + 4u * (c * 257u + i), &f, 4); }
}
static uint32_t wsum(const uint8_t *b, uint32_t nwords) { uint32_t s = 0; for (uint32_t i = 0; i < nwords; i++) { uint32_t w; memcpy(&w, b + 4u * i, 4); s += w; } return s; }
static void put_table(tbl_t t, uint64_t va, uint8_t *tab, size_t len) { for (size_t i = 0; i < len; i++) { uint64_t v = va + i; uint32_t pfn; memcpy(&pfn, g_mem + ((uint64_t)t.leaf << 14) + 4u * (uint32_t)(v >> 14), 4); g_mem[((uint64_t)(pfn & 0x7fffffffu) << 14) + (v & 0x3fff)] = tab[i]; } }

static void stats_ok(const lagfx_gd_stats_t *s) {   /* the invariants every scenario must keep */
    assert(s->read_attempts == s->read_done + s->unmapped + s->phys_read_fail);
    assert(s->translate_ok == s->read_done + s->phys_read_fail);
    assert(s->applied_ok == 0);
}
static unsigned long long field(const char *line, const char *key) { const char *k = strstr(line, key); assert(k); return strtoull(k + strlen(key), NULL, 10); }

static char *g_log; static size_t g_log_n;
static void log_load(void) { int fd = 2; off_t e = lseek(fd, 0, SEEK_END); assert(e >= 0); free(g_log); g_log = malloc((size_t)e + 1); assert(pread(fd, g_log, (size_t)e, 0) == e); g_log[e] = 0; g_log_n = (size_t)e; }
static unsigned count_str(const char *s) { unsigned n = 0; for (const char *k = g_log; (k = strstr(k, s)); k += strlen(s)) n++; return n; }

int main(void) {
    char tmpl[] = "/tmp/gamma-diag-unit-XXXXXX";
    int lf = mkstemp(tmpl); assert(lf >= 0); unlink(tmpl); dup2(lf, 2);   /* the diagnostics print to stderr: capture it */
    g_mem = calloc(1, MEM_SIZE); assert(g_mem);
    uint8_t buf[0x10000];

    /* 1. normal mapping: VA 0x118000 mapped in the 16 KiB geometry. fail-before: the legacy reader cannot read it; pass-after: the arm64 reader can. */
    {
        lagfx_protocol_t *p = mk(1);
        tbl_t t = tbl_new(); add_task(p, 2, t.root);
        map_new(t, 0x118000u >> 14); fill_va(t, 0x118000, 0x4000, 7);
        lagfx_task_entry_t *te = lagfx_protocol_find_task(p, 2);
        assert(!legacy_geom_read(p, te, 0x118000, 0x4000, buf));                       /* FAIL BEFORE: legacy geometry on a 16 KiB table */
        memset(buf, 0, sizeof buf);
        assert(lagfx_gd_task_read(p, 2, 0x118000, 0x4000, buf) == LAGFX_GD_RD_OK);       /* PASS AFTER */
        assert(check_va(buf, 0x118000, 0x4000, 7));
        { lagfx_xg_stats_t xs; lagfx_xg_reset(); memset(buf, 0, sizeof buf);   /* integ6: the gamma read goes straight to the arm64 reader, it does not touch the legacy-xlate guard counters */
          assert(lagfx_gd_task_read(p, 2, 0x118000, 0x4000, buf) == LAGFX_GD_RD_OK); lagfx_xg_snapshot(&xs);
          assert(xs.legacy_xlate_in_arm64 == 0 && xs.blocked == 0 && xs.redirected == 0);
          assert(lagfx_task_read_virtual(p, te, 0x118000, 0x4000, buf)); lagfx_xg_snapshot(&xs);   /* any OTHER caller of the legacy API: one redirected entry */
          assert(xs.legacy_xlate_in_arm64 == 1 && xs.redirected == 1 && xs.blocked == 0); lagfx_xg_reset(); }
        uint64_t g1; assert(lagfx_arm64_task_translate(p, te, 0x118000, &g1) && (g1 & 0x3fff) == 0);
        assert(lagfx_gd_task_read(p, 99, 0x118000, 16, buf) == LAGFX_GD_RD_NO_TASK);
        fin(p);
    }

    /* 2. unmapped page: the status says unmapped, never a partial success */
    {
        lagfx_protocol_t *p = mk(1);
        tbl_t t = tbl_new(); add_task(p, 2, t.root);
        map_new(t, 0x118000u >> 14);
        assert(lagfx_gd_task_read(p, 2, 0x11c000, 0x4000, buf) == LAGFX_GD_RD_UNMAPPED);
        assert(lagfx_gd_task_read(p, 2, 0x118000, 0x8000, buf) == LAGFX_GD_RD_UNMAPPED);   /* second page of the range is the unmapped one */
        assert(lagfx_gd_task_read(p, 2, ~0ull - 8, 64, buf) == LAGFX_GD_RD_UNMAPPED);     /* va + len wraps */
        fin(p);
    }

    /* 3. a read crossing a page boundary: contiguous and non-contiguous physical pages; the bytes follow the VA, not the GPA order */
    {
        lagfx_protocol_t *p = mk(1);
        tbl_t t = tbl_new(); add_task(p, 2, t.root);
        uint32_t a = newpfn(), b2 = newpfn();               /* contiguous: b2 == a + 1 */
        assert(b2 == a + 1);
        map_vpn(t, 0x44, a); map_vpn(t, 0x45, b2);
        uint32_t far_a = newpfn(); newpfn(); newpfn(); uint32_t far_b = newpfn();   /* non-contiguous: 3 pages apart */
        map_vpn(t, 0x50, far_b); map_vpn(t, 0x51, far_a);   /* reversed physical order */
        fill_va(t, 0x44 * 0x4000ull, 0x8000, 3); fill_va(t, 0x50 * 0x4000ull, 0x8000, 5);
        uint64_t va = 0x45 * 0x4000ull - 0x80;               /* 0x80 bytes in page 0x44, the rest in 0x45 */
        assert(lagfx_gd_task_read(p, 2, va, 0x100, buf) == LAGFX_GD_RD_OK && check_va(buf, va, 0x100, 3));
        va = 0x51 * 0x4000ull - 0x40;
        assert(lagfx_gd_task_read(p, 2, va, 0x80, buf) == LAGFX_GD_RD_OK && check_va(buf, va, 0x80, 5));
        va = 0x44 * 0x4000ull + 0x3ff0;                       /* 16 bytes ending exactly at the boundary, then a read starting exactly on it */
        assert(lagfx_gd_task_read(p, 2, va, 0x10, buf) == LAGFX_GD_RD_OK && check_va(buf, va, 0x10, 3));
        assert(lagfx_gd_task_read(p, 2, 0x45 * 0x4000ull, 0x10, buf) == LAGFX_GD_RD_OK && check_va(buf, 0x45 * 0x4000ull, 0x10, 3));
        lagfx_task_entry_t *te = lagfx_protocol_find_task(p, 2);
        assert(!legacy_geom_read(p, te, 0x45 * 0x4000ull - 0x80, 0x100, buf));   /* fail-before (legacy geometry) */
        assert(lagfx_task_read_virtual(p, te, 0x45 * 0x4000ull - 0x80, 0x100, buf) && check_va(buf, 0x45 * 0x4000ull - 0x80, 0x100, 3));   /* g7-addrfix: arm64 mode redirects the legacy API to the 16 KiB reader */
        fin(p);
    }

    /* 4. the same VA in two tasks: the owner of the mapping is asked first and alone; the checksum tells the right table from a readable wrong one */
    {
        lagfx_protocol_t *p = mk(1); use_fixed();
        tbl_t ta = tbl_new(), tb = tbl_new();
        add_task(p, 2, ta.root); add_task(p, 5, tb.root);
        map_new(ta, 0x118000u >> 14); map_new(tb, 0x118000u >> 14);
        fill_va(ta, 0x118000, 0x4000, 11);                    /* task 2: junk that reads fine (as the shader-library text did in the older host logs) */
        uint8_t tab[0x4000]; ident_table(tab, sizeof tab); put_table(tb, 0x118000, tab, sizeof tab);   /* task 5: the table */
        const uint32_t sum = wsum(tab, 771); assert(sum == 0x48400000u);
        p->diag2.last44_global_valid = 1; p->diag2.last44_global_task = 2;
        g_owner_on = true; g_owner_map = 9; g_owner_task = 5;
        op8(p, 9, 0x118000, 0x4000, 257, sum);
        lagfx_gd_stats_t s; lagfx_gd_stats_get(&s); stats_ok(&s);
        assert(s.read_attempts == 1 && s.read_done == 1 && s.sum_match == 1 && s.sum_mismatch == 0);   /* owner 5 read first: the table */
        log_load();
        assert(strstr(g_log, "region task=5 via=owner"));
        assert(strstr(g_log, "lut-probe swap#1 task=5") && strstr(g_log, "task=5 va=0x118000 len=0x4000 read=ok sum=0x48400000 expect=0x48400000 match=1"));
        assert(strstr(g_log, "lut-probe swap#1 task=2 va=0x118000 len=0x4000 read=ok") && strstr(g_log, "match=0"));   /* readable in task 2 too, but NOT the table */
        char line[2000]; lagfx_dd_stats_line(line, sizeof line);
        assert(field(line, "probe_sum_match=") == 1);
        /* the other owner: task 2's junk is read first, the checksum says so */
        lagfx_gd_test_reset(); g_owner_task = 2;
        op8(p, 9, 0x118000, 0x4000, 257, sum);
        lagfx_gd_stats_get(&s); stats_ok(&s);
        /* g7-gamma-select: owner 2 reads fine but its checksum mismatches, so the other live task (5) is asked in the same attempt and selected (before: stopped at the mismatch) */
        assert(s.read_done == 1 && s.sum_match == 1 && s.sum_mismatch == 0 && s.sel_accepted == 1 && s.sel_rejected_mismatch == 1);
        log_load(); assert(strstr(g_log, "region task=5 via=last44(owner not accepted)") || strstr(g_log, "region task=5 via=scan("));
        assert(strstr(g_log, "gamma-select attempt=1 result=ACCEPTED task=5"));
        fin(p);
    }

    /* 5. a mapping created between two reads: unmapped first (counted), readable at the retry */
    {
        lagfx_protocol_t *p = mk(1); use_fixed(); g_owner_on = true; g_owner_map = 1; g_owner_task = 2;
        tbl_t t = tbl_new(); add_task(p, 2, t.root);
        p->diag2.last44_global_valid = 1; p->diag2.last44_global_task = 2;
        uint8_t tab[0x4000]; ident_table(tab, sizeof tab); const uint32_t sum = wsum(tab, 771);
        op8(p, 1, 0x118000, 0x4000, 257, sum);
        lagfx_gd_stats_t s; lagfx_gd_stats_get(&s); stats_ok(&s);
        assert(s.read_attempts == 1 && s.read_done == 0 && s.unmapped == 1 && s.translate_ok == 0 && s.phys_read_fail == 0);
        map_new(t, 0x118000u >> 14); put_table(t, 0x118000, tab, sizeof tab);   /* the guest maps the page after the first swap */
        op8(p, 1, 0x118000, 0x4000, 257, sum);
        lagfx_gd_stats_get(&s); stats_ok(&s);
        assert(s.read_attempts == 2 && s.read_done == 1 && s.unmapped == 1 && s.translate_ok == 1 && s.sum_match == 1);
        op8(p, 1, 0x118000, 0x4000, 257, sum);                    /* done: no further region read (bounded) */
        lagfx_gd_stats_get(&s); assert(s.read_attempts == 2);
        /* the same sequence with the legacy reader (the pre-fix behaviour): never readable */
        lagfx_gd_test_reset(); use_legacy();
        op8(p, 1, 0x118000, 0x4000, 257, sum); op8(p, 1, 0x118000, 0x4000, 257, sum);
        lagfx_gd_stats_get(&s); assert(s.read_done == 0 && s.read_attempts == 2);   /* FAIL BEFORE */
        fin(p);
    }

    /* 6. invalid PTEs / invalid descriptors: safe errors, classified, no crash */
    {
        lagfx_protocol_t *p = mk(1); use_fixed(); g_owner_on = true; g_owner_map = 1; g_owner_task = 2;
        tbl_t t = tbl_new(); add_task(p, 2, t.root);
        p->diag2.last44_global_valid = 1; p->diag2.last44_global_task = 2;
        map_vpn(t, 0x118000u >> 14, 0x7ffffu);                      /* translates (pfn in range of the 32-bit entry) but the physical page is beyond the guest memory */
        assert(lagfx_gd_task_read(p, 2, 0x118000, 64, buf) == LAGFX_GD_RD_PHYS_FAIL);
        map_vpn(t, 0x11c000u >> 14, 0x80000000u | 0x70u);           /* bit 31 set: a flag, stripped by the walk (same as the real reader) */
        assert(lagfx_gd_task_read(p, 2, 0x11c000, 64, buf) == LAGFX_GD_RD_OK);
        put32((uint64_t)t.root << 14, 0x7ffffu);                    /* top table pointer outside RAM */
        assert(lagfx_gd_task_read(p, 2, 0x11c000, 64, buf) == LAGFX_GD_RD_UNMAPPED);
        put32((uint64_t)t.root << 14, t.top); put32(((uint64_t)t.root << 14) + 4, 9);   /* depth 9 is invalid */
        assert(lagfx_gd_task_read(p, 2, 0x11c000, 64, buf) == LAGFX_GD_RD_UNMAPPED);
        put32(((uint64_t)t.root << 14) + 4, 2);
        op8(p, 1, 0x118000, 0x4000, 257, 0x48400000u);           /* physical read fails: translate_ok, phys_read_fail */
        lagfx_gd_stats_t s; lagfx_gd_stats_get(&s); stats_ok(&s);
        assert(s.read_attempts == 1 && s.phys_read_fail == 1 && s.translate_ok == 1 && s.unmapped == 0 && s.read_done == 0);
        log_load(); assert(strstr(g_log, "reason=phys_read_fail"));
        /* descriptors: short payload, len too large, off without len, count that does not fit */
        uint8_t c[52]; memset(c, 0, sizeof c); w32(c, 0, 8); w32(c, 4, 40); lagfx_dd_note_cmd(p, 1, 4, c, 40);                    /* 28-byte payload */
        op8(p, 1, 0x118000, 0x20000, 1, 0);
        op8(p, 1, 0x118000, 0, 1, 0);
        op8(p, 1, 0, 0x4000, 1, 0);
        op8(p, 1, 0x118000, 0x4000, 0x1000, 0);
        op8(p, 1, ~0ull - 0x10, 0x4000, 1, 0);                       /* off + len wraps */
        lagfx_gd_stats_get(&s); stats_ok(&s);
        assert(s.descriptor_parse_fail == 6 && s.read_attempts == 1);   /* none of them started a read */
        assert(lagfx_gd_desc_parse(NULL, 0, 0, &(lagfx_gd_desc_t){0}) == LAGFX_GD_DESC_SHORT);
        fin(p);
    }

    /* 7. the legacy 4 KiB path (x86 protocol mode) is unchanged: root = pfn << 12, 4 KiB pages, 10 index bits */
    {
        lagfx_protocol_t *p = mk(0); use_fixed();
        uint32_t rootp = 0x800, l1 = 0x801, leaf = 0x802, data0 = 0x900, data1 = 0x910;   /* 4 KiB pfns (GPA = pfn << 12), above everything the 16 KiB fixtures use */
        put32((uint64_t)rootp << 12, l1); put32(((uint64_t)rootp << 12) + 4, 2);            /* {first table, levels 2} */
        put32((uint64_t)l1 << 12, leaf);
        uint64_t va = 0x118000;                                                              /* page index 0x118: leaf slot 0x118 */
        put32(((uint64_t)leaf << 12) + 4u * 0x118, data0); put32(((uint64_t)leaf << 12) + 4u * 0x119, data1);   /* two non-contiguous 4 KiB pages */
        for (uint32_t i = 0; i < 0x1000; i++) { g_mem[((uint64_t)data0 << 12) + i] = pat(va + i, 1); g_mem[((uint64_t)data1 << 12) + i] = pat(va + 0x1000 + i, 1); }
        add_task(p, 4, rootp);
        lagfx_task_entry_t *te = lagfx_protocol_find_task(p, 4);
        memset(buf, 0, sizeof buf);
        assert(lagfx_task_read_virtual(p, te, va + 0xf80, 0x100, buf) && check_va(buf, va + 0xf80, 0x100, 1));   /* the legacy reader as before */
        memset(buf, 0, sizeof buf);
        assert(lagfx_gd_task_read(p, 4, va + 0xf80, 0x100, buf) == LAGFX_GD_RD_OK && check_va(buf, va + 0xf80, 0x100, 1));   /* the mode switch picks it */
        assert(!lagfx_arm64_task_read(p, te, va, buf, 16));                                  /* and the arm64 geometry cannot read this table */
        assert(lagfx_gd_task_read(p, 4, 0x200000, 16, buf) == LAGFX_GD_RD_UNMAPPED);
        fin(p);
    }

    /* 8. the 257-entry (0x101) descriptor with len 0x4000: layout, checksum, type */
    {
        /* [V arithmetic] 0x4000 / 257 = 63.75 bytes: not an integer, so 0x4000 is the allocation (one 16 KiB page), not 257 equal records. [I] 3 planes x 257 float32 = 771 words =
         * 3084 bytes (0xc0c) at the start of the page, plane c at word c * 257, the rest zero. Its wrapping u32 word sum is exactly the descriptor's 0x48400000. */
        uint8_t tab[0x4000]; ident_table(tab, sizeof tab);
        assert(wsum(tab, 771) == 0x48400000u);
        float f1; memcpy(&f1, tab + 4 * 257, 4); assert(f1 == 0.0f);                      /* plane 1 entry 0 */
        uint32_t w; memcpy(&w, tab + 4 * 258, 4); assert(w == 0x3b800000u);              /* plane 1 entry 1 = 1/256 */
        memcpy(&w, tab + 4 * 384, 4); assert(w == 0x3efe0000u);                          /* word 384 = plane 1 entry 127 = 127/256: the "task 0@off+600" window of the host logs */
        memcpy(&w, tab + 4 * 764, 4); assert(w == 0x3f7a0000u);                          /* word 764 = plane 2 entry 250: the "+bf0" window */
        memcpy(&w, tab + 4 * 770, 4); assert(w == 0x3f800000u);                          /* last entry = 1.0f */
        memcpy(&w, tab + 4 * 771, 4); assert(w == 0);                                    /* zeros follow */
        /* the first 24 words the host printed (bytes in memory order) are our plane 0 */
        static const char *host = "00000000 0000803b 0000003c 0000403c 0000803c 0000a03c 0000c03c 0000e03c 0000003d 0000103d 0000203d 0000303d 0000403d 0000503d 0000603d 0000703d 0000803d 0000883d 0000903d 0000983d 0000a03d 0000a83d 0000b03d 0000b83d";
        char mine[400]; size_t k = 0; for (int i = 0; i < 24 * 4; i++) k += (size_t)sprintf(mine + k, "%02x%s", tab[i], (i % 4 == 3 && i < 95) ? " " : "");
        assert(strcmp(mine, host) == 0);
        lagfx_gd_lut_t l;
        assert(lagfx_gd_lut_decode(tab, sizeof tab, 257, 0x48400000u, &l) == 0);
        assert(l.fits && l.nwords == 771 && l.sum == 0x48400000u && l.sum_match && l.tail_zero && l.finite);
        assert(l.monotonic[0] && l.monotonic[1] && l.monotonic[2] && l.maxdev < 1e-7f && l.first[2] == 0.0f && l.last[1] == 1.0f);
        tab[4 * 500] ^= 1; assert(lagfx_gd_lut_decode(tab, sizeof tab, 257, 0x48400000u, &l) == 0 && !l.sum_match);   /* one flipped bit: checksum mismatch */
        ident_table(tab, sizeof tab); tab[4 * 900] = 1; assert(lagfx_gd_lut_decode(tab, sizeof tab, 257, 0x48400000u, &l) == 0 && !l.tail_zero);
        assert(lagfx_gd_lut_decode(tab, 1000, 257, 0, &l) == 0 && !l.fits);              /* too short for 3 * 257 words */
        assert(lagfx_gd_lut_decode(NULL, 8, 257, 0, &l) == -1);
        lagfx_gd_desc_t d; uint8_t pl[40]; memset(pl, 0, sizeof pl);
        w32(pl, 8, 1); w32(pl, 16, 0x118000); w32(pl, 24, 0x4000); w32(pl, 32, 0x101); w32(pl, 36, 0x48400000u);
        assert(lagfx_gd_desc_parse(pl, sizeof pl, 0, &d) == LAGFX_GD_DESC_OK && d.mapping == 1 && d.off == 0x118000 && d.len == 0x4000 && d.count == 257 && d.sum == 0x48400000u);
        assert(lagfx_gd_desc_parse(pl, sizeof pl - 4, 0, &d) == LAGFX_GD_DESC_SHORT);
        /* the descriptor read end to end, 16 KiB geometry, whole page */
        lagfx_protocol_t *p = mk(1); use_fixed(); g_owner_on = true; g_owner_map = 1; g_owner_task = 0;
        tbl_t t = tbl_new(); add_task(p, 0, t.root); map_new(t, 0x118000u >> 14);
        ident_table(tab, sizeof tab); put_table(t, 0x118000, tab, sizeof tab);
        op8(p, 1, 0x118000, 0x4000, 0x101, 0x48400000u);
        lagfx_gd_stats_t s; lagfx_gd_stats_get(&s); stats_ok(&s);
        assert(s.read_done == 1 && s.sum_match == 1 && s.applied_ok == 0);
        log_load(); assert(strstr(g_log, "lut_decode[fits=1 sum=0x48400000 expect=0x48400000 match=1 ramp_maxdev=0.000000]"));
        fin(p);
    }

    /* 9. diagnostics: every descriptor logged on change (no 3-op cap), bounded walk / probe output, walk agrees with the translator, ram / mapping probes */
    {
        lagfx_protocol_t *p = mk(1); use_fixed(); g_owner_on = false;
        tbl_t t0 = tbl_new(), t2 = tbl_new(); add_task(p, 0, t0.root); add_task(p, 2, t2.root);
        map_new(t0, 0x114000u >> 14); map_new(t2, 0x114000u >> 14); map_new(t2, 0x118000u >> 14);   /* task 0: 0x118000 unmapped; task 2: mapped */
        lagfx_task_entry_t *te = lagfx_protocol_find_task(p, 2); uint64_t gpa114; assert(lagfx_arm64_task_translate(p, te, 0x114000, &gpa114));
        setenv("LAGFX_GD_RAM_BASE", "0x300000", 1);
        /* RAM probe: base (env override) 0x300000 + 0x118000 = 0x418000 */
        for (int i = 0; i < 64; i++) g_mem[0x418000 + i] = (uint8_t)(0xa0 + i);
        /* a fake mapping: page list at 0x500000, 1013 entries, entry 0x46 -> page at 0x600000 holding a pattern */
        lagfx_device_t *dev = (lagfx_device_t *)p->dev;
        w32(sd, 0, (0x500000u >> 12) | 1u); w32(sd, 4, 0x42475241u); w32(sd, 0x18, 1013u * 0x4000u); w32(sd, 0x60, 0);
        dev->desc.shell.get_surface_desc = getdesc;
        for (uint32_t i = 0; i < 1013; i++) put32(0x500000 + 4u * i, ((0x600000u + i * 0x4000u) >> 12) | 1u);
        for (int i = 0; i < 64; i++) g_mem[0x600000 + 0x46u * 0x4000u + i] = (uint8_t)(0x10 + i);
        /* 200 swaps, descriptor changing on swaps 1..10 (the old code looked at the first 3 only) then constant */
        for (uint32_t n = 1; n <= 200; n++) op8(p, 1, n <= 10 ? 0x118000u + 0x4000u * (n % 3) : 0, n <= 10 ? 0x4000u : 0, n <= 10 ? 257u : 0, n <= 10 ? 0x48400000u + n : 0);
        log_load();
        assert(count_str("g7-pvg-gamma: op8 desc #") >= 11 && count_str("g7-pvg-gamma: op8 desc #") <= 12);   /* 10 changes + the change to the zero descriptor (swap 11) */
        assert(strstr(g_log, "op8 desc #7 ch=4 mapping=1"));                                                  /* beyond the old cap of 3 */
        assert(count_str("g7-pvg-gamma: walk swap#") <= 64 && count_str("g7-pvg-gamma: walk swap#1 task=0") == 1 && count_str("g7-pvg-gamma: walk swap#1 task=2") == 1);
        assert(count_str("g7-pvg-gamma: walk swap#64 task=") >= 1 && count_str("g7-pvg-gamma: walk swap#100 task=") == 0);   /* sampled at 1 2 3 4 8 16 32 64 */
        char want[200]; snprintf(want, sizeof want, "task=2 root_pfn=0x%x", t2.root); assert(strstr(g_log, want));
        assert(strstr(g_log, "control va=0x114000: L1[0x0]=0x") && strstr(g_log, "va=0x118000: L1[0x0]=0x") && strstr(g_log, "L0[0x46]=0x0 UNMAPPED"));
        snprintf(want, sizeof want, "->gpa=0x%llx", (unsigned long long)gpa114); assert(strstr(g_log, want));   /* the walk ends where lagfx_arm64_task_translate does */
        assert(strstr(g_log, "va=0x134000:"));
        assert(strstr(g_log, "ram-probe swap#1 gpa=0x418000") && strstr(g_log, "a0a1a2a3 a4a5a6a7"));
        assert(strstr(g_log, "map-probe swap#1 mapping=1 npages=1013 page 70 gpa=0x") && strstr(g_log, "10111213 14151617"));
        assert(count_str("g7-pvg-gamma: ram-probe") <= 12 && count_str("g7-pvg-gamma: map-probe") <= 12 && count_str("g7-pvg-gamma: lut-probe") <= 64);
        lagfx_gd_stats_t s; lagfx_gd_stats_get(&s); stats_ok(&s);
        assert(s.descs_seen == 200 && s.desc_changes >= 11);
        char line[2000]; size_t n = lagfx_dd_stats_line(line, sizeof line);
        assert(n < sizeof line - 1 && strstr(line, "gamma[raw_lines=") && strstr(line, " read_attempts=") && strstr(line, " read_done=") && strstr(line, " applied_ok=0"));
        assert(strstr(line, " translate_ok=") && strstr(line, " unmapped=") && strstr(line, " phys_read_fail=") && strstr(line, " descriptor_parse_fail="));
        assert(strstr(line, "cursor[") && strstr(line, "vbl[") && strstr(line, "swaps="));        /* the existing fields are still there */
        if (getenv("GAMMA_DIAG_UNIT_DUMP")) { ssize_t w = write(1, g_log, g_log_n); (void)w; }   /* shows the log lines of this scenario (expected next-run shapes) */
        unsetenv("LAGFX_GD_RAM_BASE");
        fin(p);
    }

    /* 10. through lagfx_diag_cmd, i.e. diag.c's own registration (the real hook) */
    {
        lagfx_protocol_t *p = mk(1);
        tbl_t t = tbl_new(); add_task(p, 6, t.root); map_new(t, 0x118000u >> 14);
        uint8_t tab[0x4000]; ident_table(tab, sizeof tab); put_table(t, 0x118000, tab, sizeof tab);
        p->diag2.last44_global_valid = 1; p->diag2.last44_global_task = 6;
        uint8_t c[52]; memset(c, 0, sizeof c); w32(c, 0, 8); w32(c, 4, 52); w32(c, 8, 0x400); w32(c, 12 + 8, 1); w32(c, 12 + 16, 0x118000); w32(c, 12 + 24, 0x4000); w32(c, 12 + 32, 257); w32(c, 12 + 36, 0x48400000u);
        lagfx_diag_cmd(p, LAGFX_DIAG_COMPUTE, 4, 0, c, 52);
        lagfx_gd_stats_t s; lagfx_gd_stats_get(&s); stats_ok(&s);
        assert(s.read_done == 1 && s.sum_match == 1);   /* diag.c's dd_read_region picked the 16 KiB reader by the mode switch */
        fin(p);
    }
    printf("gamma-diag-unit OK\n");
    return 0;
}
