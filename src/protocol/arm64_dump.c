/*
 * libapplegfx-vulkan — ARM64 bounded capture of real guest data. See arm64_dump.h.
 */
#include "arm64_dump.h"
#include "arm64_objects.h"
#include "arm64_pipeline.h"
#include "../common/le.h"
#include "../common/log.h"
#include "../device.h"
#include "../air2spirv/metallib_extract.h"

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SEEN_MAX 1024u
#define LOG_OBJ_MAX 512u
#define INDEX_LINES_MAX 8192u
#define MAXB 32u

static struct {
    uint64_t seen[SEEN_MAX]; uint32_t nseen;
    int dirfd, idxfd; bool dir_tried, announced;
    uint64_t total, max_file, max_total;
    uint32_t idx_lines, logged;
    /* binding maps of the pass being walked */
    uint32_t pipe_ref;
    struct { uint32_t ref; uint64_t off; } vb[MAXB], fb[MAXB];
    uint32_t tex[MAXB], samp[MAXB];
} G = { .dirfd = -1, .idxfd = -1 };

bool lagfx_a64_dump_enabled(void) {
    const char *e = getenv("LAGFX_ARM64_DUMP");
    return e && e[0] == '1' && !e[1];
}

void lagfx_a64_dump_reset(void) {
    if (G.dirfd >= 0) close(G.dirfd);
    if (G.idxfd >= 0) close(G.idxfd);
    memset(&G, 0, sizeof G);
    G.dirfd = G.idxfd = -1;
}

static uint64_t env_u64(const char *name, uint64_t def) {
    const char *e = getenv(name);
    if (!e || !e[0]) return def;
    char *end = NULL;
    unsigned long long v = strtoull(e, &end, 0);
    return end && !*end && v ? v : def;
}

static bool seen_add(uint64_t key) {   /* true = new */
    for (uint32_t i = 0; i < G.nseen; i++) if (G.seen[i] == key) return false;
    if (G.nseen < SEEN_MAX) G.seen[G.nseen++] = key;
    return true;
}

static void hexn(char *dst, size_t cap, const uint8_t *b, uint32_t n) {
    size_t o = 0;
    for (uint32_t i = 0; i < n && o + 3 < cap; i++) o += (size_t)snprintf(dst + o, cap - o, "%02x%s", b[i], (i & 15u) == 15u ? "|" : "");
    dst[o < cap ? o : cap - 1] = 0;
}

static void open_dir(void) {
    if (G.dir_tried) return;
    G.dir_tried = true;
    G.max_file = env_u64("LAGFX_ARM64_DUMP_MAX_FILE", 4ull << 20);
    G.max_total = env_u64("LAGFX_ARM64_DUMP_MAX_TOTAL", 32ull << 20);
    const char *d = getenv("LAGFX_ARM64_DUMP_DIR");
    if (!d || !d[0]) { LAGFX_LOG("arm64-dump: logging only (no LAGFX_ARM64_DUMP_DIR); caps file=%llu total=%llu", (unsigned long long)G.max_file, (unsigned long long)G.max_total); return; }
    G.dirfd = open(d, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (G.dirfd < 0) { LAGFX_WARN("arm64-dump: cannot open dir '%s': errno %d (%s); logging only", d, errno, strerror(errno)); return; }
    G.idxfd = openat(G.dirfd, "index.txt", O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);   /* append: never truncates or overwrites */
    if (G.idxfd < 0) LAGFX_WARN("arm64-dump: cannot open index.txt in '%s': errno %d (%s)", d, errno, strerror(errno));
    LAGFX_LOG("arm64-dump: dir=%s caps file=%llu total=%llu index=%s", d, (unsigned long long)G.max_file, (unsigned long long)G.max_total, G.idxfd >= 0 ? "yes" : "no");
}

static void index_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void index_line(const char *fmt, ...) {
    if (G.idxfd < 0 || G.idx_lines >= INDEX_LINES_MAX) return;
    char l[768];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(l, sizeof l - 1, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n > sizeof l - 2) n = (int)sizeof l - 2;
    l[n++] = '\n';
    if (write(G.idxfd, l, (size_t)n) == (ssize_t)n) G.idx_lines++;
}

/* Writes `data` (saved_len of full_size bytes) as `name`; returns the file name used (or "-") in out_name. Updates counters. */
static void save_blob(lagfx_protocol_t *p, const char *name, const uint8_t *data, uint64_t full_size, uint64_t have, char *out_name, size_t cap, uint64_t *saved_out, int *trunc_out) {
    *saved_out = 0, *trunc_out = 0;
    snprintf(out_name, cap, "-");
    if (G.dirfd < 0) return;
    uint64_t room = G.total >= G.max_total ? 0 : G.max_total - G.total;
    uint64_t want = have < G.max_file ? have : G.max_file;
    if (want > room) want = room;
    if (!want) { p->diag2.exec.pipe.dump_skipped_cap++; return; }
    int trunc = want < full_size;
    char fn[160];
    snprintf(fn, sizeof fn, "%s%s", name, trunc ? ".TRUNCATED" : "");
    int fd = openat(G.dirfd, fn, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { if (errno != EEXIST) LAGFX_WARN("arm64-dump: cannot create %s: errno %d", fn, errno); else snprintf(out_name, cap, "%s", fn); return; }
    uint64_t done = 0;
    while (done < want) {
        ssize_t w = write(fd, data + done, (size_t)(want - done));
        if (w <= 0) break;
        done += (uint64_t)w;
    }
    close(fd);
    G.total += done;
    p->diag2.exec.pipe.dump_bytes += done, p->diag2.exec.pipe.dump_files++;
    if (trunc) p->diag2.exec.pipe.dump_truncated++;
    snprintf(out_name, cap, "%s", fn);
    *saved_out = done, *trunc_out = trunc || done < want;
}

static void dump_function(lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint32_t ref, const uint32_t e[3]) {
    uint8_t h[20];
    if (!lagfx_arm64_task_read(p, task, (uint64_t)e[2] << 32 | e[1], h, sizeof h)) { LAGFX_LOG("arm64-dump fn ref=%u: header unreadable", ref); return; }
    uint64_t va = lagfx_le64(h);
    uint32_t len = lagfx_le32(h + 8);
    uint64_t cap = len < (64ull << 20) ? len : (64ull << 20), keep = cap < G.max_file ? cap : G.max_file;
    uint8_t *b = len ? malloc(keep) : NULL;
    if (!b || !lagfx_arm64_task_read(p, task, va, b, keep)) {
        LAGFX_LOG("arm64-dump fn ref=%u: library va=0x%llx len=%u unreadable", ref, (unsigned long long)va, len);
        index_line("fn task=%u ref=%u library_va=0x%llx full_size=%u saved_size=0 file=- unreadable=1", task->id, ref, (unsigned long long)va, len);
        free(b);
        return;
    }
    uint64_t hash = lagfx_a64_hash64(b, keep);
    char hx[3 * 64 + 8], names[300];
    hexn(hx, sizeof hx, b, 32);
    names[0] = 0;
    lagfx_metallib_function_t fn[8];
    size_t nfn = 0, o = 0;
    if (keep == len && lagfx_metallib_extract_functions(b, keep, fn, 8, &nfn) == LAGFX_OK)
        for (size_t i = 0; i < nfn && i < 8 && o + 80 < sizeof names; i++) o += (size_t)snprintf(names + o, sizeof names - o, "%s%s(stage=%u,bc=%zu)", i ? "," : "", fn[i].name, (unsigned)fn[i].stage, fn[i].bitcode_len);
    LAGFX_LOG("arm64-dump fn task=%u ref=%u lib_va=0x%llx len=%u saved=%llu hash=%016llx magic=%.4s first32=%s functions=[%s]%s", task->id, ref, (unsigned long long)va, len,
              (unsigned long long)keep, (unsigned long long)hash, (const char *)b, hx, names, keep < len ? " TRUNCATED-BY-CAP" : "");
    char name[64], file[200];
    uint64_t saved; int tr;
    /* complete libraries are named by content (identical libraries share one file: the second create fails with EEXIST and only the index line is added); a
     * truncated one is named by its (task, ref) too, because libraries often share their first bytes (the MTLB header) */
    if (keep < len) snprintf(name, sizeof name, "lib_t%u_r%u_%016llx.bin", task->id, ref, (unsigned long long)hash);
    else snprintf(name, sizeof name, "lib_%016llx.bin", (unsigned long long)hash);
    save_blob(p, name, b, len, keep, file, sizeof file, &saved, &tr);
    index_line("fn task=%u ref=%u library_va=0x%llx file=%s full_size=%u saved_size=%llu truncated=%d hash=%016llx functions=[%s]", task->id, ref, (unsigned long long)va, file, len,
               (unsigned long long)saved, tr || keep < len, (unsigned long long)hash, names);
    free(b);
}

static void dump_object(lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint32_t ref, const uint32_t e[3]) {
    uint32_t type = e[0] & 0xffu;
    if (type != LAGFX_A64_OBJ_SERIALIZED && type != LAGFX_A64_OBJ_FUNCTION) return;
    uint32_t k[8] = { task->id, ref, e[0], e[1], e[2], (uint32_t)task->root_page_pfn, 0, 0 };
    if (!seen_add(lagfx_a64_hash64(k, sizeof k))) return;
    if (G.nseen >= SEEN_MAX) p->diag2.exec.pipe.dump_table_full++;
    open_dir();
    if (G.logged >= LOG_OBJ_MAX) { p->diag2.exec.pipe.dump_skipped_cap++; return; }
    G.logged++;
    p->diag2.exec.pipe.dump_objects++;
    if (type == LAGFX_A64_OBJ_FUNCTION) { dump_function(p, task, ref, e); return; }

    uint32_t full = e[0] >> 8;
    uint64_t at = (uint64_t)e[2] << 32 | e[1];
    uint64_t keep = full < G.max_file ? full : G.max_file;
    uint8_t *b = full ? malloc(keep ? keep : 1) : NULL;
    if (!b || !lagfx_arm64_task_read(p, task, at, b, keep)) {
        LAGFX_LOG("arm64-dump obj task=%u ref=%u type=%u size=%u at=0x%llx: unreadable", task->id, ref, type, full, (unsigned long long)at);
        index_line("obj task=%u ref=%u type=%u full_size=%u saved_size=0 file=- unreadable=1", task->id, ref, type, full);
        free(b);
        return;
    }
    uint64_t hash = lagfx_a64_hash64(b, keep);
    char hx[3 * 128 + 16];
    hexn(hx, sizeof hx, b, keep < 128 ? (uint32_t)keep : 128u);
    uint32_t tag = keep >= 4 ? lagfx_le32(b) : 0;
    char name[96], file[200];
    uint64_t saved;
    int tr;
    snprintf(name, sizeof name, "pso_%u_%u_%016llx.bin", task->id, ref, (unsigned long long)hash);
    save_blob(p, name, b, full, keep, file, sizeof file, &saved, &tr);
    LAGFX_LOG("arm64-dump obj task=%u ref=%u type=7 size=%u at=0x%llx hash=%016llx tag=0x%x file=%s first128=%s", task->id, ref, full, (unsigned long long)at, (unsigned long long)hash, tag, file, hx);
    index_line("obj task=%u ref=%u type=7 tag=0x%x file=%s full_size=%u saved_size=%llu truncated=%d hash=%016llx", task->id, ref, tag, file, full, (unsigned long long)saved, tr || keep < full,
               (unsigned long long)hash);
    lagfx_a64_pdesc_t d;
    lagfx_a64_fnctx_t fc = { p, task };
    if (tag == 0xeu && lagfx_a64_pdesc_decode(b, (uint32_t)keep, lagfx_a64_is_function_ref, &fc, &d)) {
        char rs[160], as[200];
        size_t o = 0, q = 0;
        rs[0] = as[0] = 0;
        for (uint32_t i = 0; i < d.nrefs && o + 12 < sizeof rs; i++) o += (size_t)snprintf(rs + o, sizeof rs - o, "%s%u", i ? "," : "", d.refs[i]);
        for (uint32_t i = 0; i < d.nattrs && q + 28 < sizeof as; i++) q += (size_t)snprintf(as + q, sizeof as - q, "%sfmt%u@%u/buf%u", i ? " " : "", d.attrs[i].fmt, d.attrs[i].off, d.attrs[i].bufidx);
        LAGFX_LOG("arm64-dump pso task=%u ref=%u: header size=%u ref=%u length=%u function_refs=[%s] vertex_attrs=[%s] stride=%u colour_formats=not-decoded", task->id, ref, d.size, d.ref, d.length, rs, as, d.stride);
        index_line("pso task=%u ref=%u function_refs=[%s] vertex_attrs=[%s] stride=%u", task->id, ref, rs, as, d.stride);
        for (uint32_t i = 0; i < d.nrefs; i++) {
            uint32_t fe[3];
            if (lagfx_a64_objects_read_entry(p, task, d.refs[i], fe)) {
                uint32_t kk[8] = { task->id, d.refs[i], fe[0], fe[1], fe[2], (uint32_t)task->root_page_pfn, 0, 0 };
                if (seen_add(lagfx_a64_hash64(kk, sizeof kk))) { p->diag2.exec.pipe.dump_objects++; dump_function(p, task, d.refs[i], fe); }
            }
        }
    }
    free(b);
}

void lagfx_a64_dump_touch_object(lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint32_t ref, const uint32_t e[3]) {
    if (!p || !task || !e || !ref || !lagfx_a64_dump_enabled()) return;
    uint32_t type = e[0] & 0xffu;
    if (type == LAGFX_A64_OBJ_SERIALIZED || type == LAGFX_A64_OBJ_FUNCTION) dump_object(p, task, ref, e);
}

/* type 11 (mapped texture) of the resource list: {u64 mapping, u32 tag, u32 size, u32 ref, description}; one index line + the first 128 bytes, once per entry */
static void dump_mapped(lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint32_t ref, const uint32_t e[3]) {
    uint32_t k[8] = { task->id, ref, e[0], e[1], e[2], (uint32_t)task->root_page_pfn, 11, 0 };
    if (!seen_add(lagfx_a64_hash64(k, sizeof k))) return;
    open_dir();
    if (G.logged >= LOG_OBJ_MAX) { p->diag2.exec.pipe.dump_skipped_cap++; return; }
    G.logged++;
    p->diag2.exec.pipe.dump_objects++;
    uint32_t full = e[0] >> 8, n = full < 128u ? full : 128u;
    uint8_t b[128];
    memset(b, 0, sizeof b);
    if (!full || !lagfx_arm64_task_read(p, task, (uint64_t)e[2] << 32 | e[1], b, n)) {
        LAGFX_LOG("arm64-dump mapped task=%u ref=%u size=%u: unreadable", task->id, ref, full);
        index_line("mapped task=%u ref=%u unreadable=1 full_size=%u", task->id, ref, full);
        return;
    }
    uint64_t mapping = n >= 8 ? lagfx_le64(b) : 0;
    uint32_t tag = n >= 12 ? lagfx_le32(b + 8) : 0, dsz = n >= 16 ? lagfx_le32(b + 12) : 0;
    char surf[160], hx[3 * 128 + 16];
    snprintf(surf, sizeof surf, "surface=unknown fourcc=unknown row=unknown first=unknown");
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    uint8_t d[256];
    size_t dl = 0;
    memset(d, 0, sizeof d);
    if (dev && dev->desc.shell.get_surface_desc && mapping <= 0xffffffffull && dev->desc.shell.get_surface_desc(dev->desc.shell.opaque, (uint32_t)mapping, d, sizeof d, &dl) && dl >= 0x64 && dl <= sizeof d)
        snprintf(surf, sizeof surf, "surface=%llux%llu fourcc=%08x row=%llu first=%u", (unsigned long long)lagfx_le64(d + 0x20), (unsigned long long)lagfx_le64(d + 0x28),
                 lagfx_le32(d + 4), (unsigned long long)lagfx_le64(d + 0x30), lagfx_le32(d + 0x60));
    hexn(hx, sizeof hx, b, n);
    LAGFX_LOG("arm64-dump mapped task=%u ref=%u mapping=%llu tag=0x%x desc_size=%u %s first128=%s", task->id, ref, (unsigned long long)mapping, tag, dsz, surf, hx);
    index_line("mapped task=%u ref=%u mapping=%llu tag=0x%x desc_size=%u %s first128=%s", task->id, ref, (unsigned long long)mapping, tag, dsz, surf, hx);
}

void lagfx_a64_dump_resources(lagfx_protocol_t *p, const lagfx_task_entry_t *task, const uint8_t *res, uint32_t nres) {
    if (!p || !task || !res || !lagfx_a64_dump_enabled()) return;
    memset(&G.vb, 0, sizeof G.vb), memset(&G.fb, 0, sizeof G.fb), memset(G.tex, 0, sizeof G.tex), memset(G.samp, 0, sizeof G.samp);
    G.pipe_ref = 0;
    for (uint32_t i = 0; i < nres; i++) {
        uint32_t ref = lagfx_le32(res + 8u * i), e[3];
        if (!ref || !lagfx_a64_objects_read_entry(p, task, ref, e)) continue;
        if ((e[0] & 0xffu) == 11u) dump_mapped(p, task, ref, e); else dump_object(p, task, ref, e);
    }
}

static void print_maps(char *dst, size_t cap, const char *name, const uint32_t *refs, const uint64_t *offs, const uint32_t *refs2) {
    size_t o = strlen(dst);
    o += (size_t)snprintf(dst + o, cap - o, " %s[", name);
    bool first = true;
    for (uint32_t i = 0; i < MAXB && o + 24 < cap; i++) {
        uint32_t r = refs ? refs[i] : refs2[i];
        if (!r) continue;
        o += (size_t)snprintf(dst + o, cap - o, "%s%u:%u", first ? "" : " ", i, r);
        if (offs) o += (size_t)snprintf(dst + o, cap - o, "@%llu", (unsigned long long)offs[i]);
        first = false;
    }
    if (o + 2 < cap) dst[o++] = ']', dst[o] = 0;
}

void lagfx_a64_dump_segment(lagfx_protocol_t *p, const lagfx_task_entry_t *task, const uint8_t *buf, uint32_t avail, const lagfx_exec_seg_t *s) {
    if (!p || !task || !buf || !s || !lagfx_a64_dump_enabled() || s->type != 0u) return;
    uint64_t pos = s->prot_skipped ? 24u : 8u;
    uint64_t have = avail > pos ? avail - pos : 0, lim = s->body_len < have ? s->body_len : have, q = 0;
    uint32_t n_inner = 0;
    while (q + 8u <= lim && n_inner++ < LAGFX_EXEC_MAX_INNER) {
        uint32_t tag = lagfx_le32(buf + pos + q), sz = lagfx_le32(buf + pos + q + 4);
        if (sz < 8u || q + sz > lim) break;
        const uint8_t *r = buf + pos + q + 8u;
        uint32_t n = sz - 8u;
        q += sz;
        switch (tag) {
        case 26: memset(&G.vb, 0, sizeof G.vb), memset(&G.fb, 0, sizeof G.fb), memset(G.tex, 0, sizeof G.tex), memset(G.samp, 0, sizeof G.samp), G.pipe_ref = 0; break;
        case 116: if (n >= 4) G.pipe_ref = lagfx_le32(r); break;
        case 125: case 110: {
            if (n < 8) break;
            uint32_t idx = lagfx_le32(r), cnt = lagfx_le32(r + 4);
            if (cnt > 31u || n < 8u + 12ull * cnt) break;
            for (uint32_t i = 0; i < cnt && idx + i < MAXB; i++) {
                if (tag == 125) G.vb[idx + i].ref = lagfx_le32(r + 8 + i * 12), G.vb[idx + i].off = lagfx_le64(r + 12 + i * 12);
                else G.fb[idx + i].ref = lagfx_le32(r + 8 + i * 12), G.fb[idx + i].off = lagfx_le64(r + 12 + i * 12);
            }
            break;
        }
        case 126: case 111:
            if (n >= 12 && lagfx_le32(r) < MAXB) { if (tag == 126) G.vb[lagfx_le32(r)].off = lagfx_le64(r + 4); else G.fb[lagfx_le32(r)].off = lagfx_le64(r + 4); }
            break;
        case 114: case 112: {
            if (n < 8) break;
            uint32_t idx = lagfx_le32(r), cnt = lagfx_le32(r + 4);
            if (cnt > 128u || n < 8u + 4ull * cnt) break;
            for (uint32_t i = 0; i < cnt && idx + i < MAXB; i++) { if (tag == 114) G.tex[idx + i] = lagfx_le32(r + 8 + i * 4); else G.samp[idx + i] = lagfx_le32(r + 8 + i * 4); }
            break;
        }
        case 0: case 1: case 2: case 3: case 6: case 7: case 8: case 9:
            if (p->diag2.exec.pipe.dump_draws < 16u) {
                char l[700];
                uint32_t prim = tag == 7 || tag == 9 || tag == 6 || tag == 8 ? (n >= 2 ? (uint32_t)(r[0] | r[1] << 8) : 0) : (n >= 4 ? lagfx_le32(r) : 0);
                snprintf(l, sizeof l, "task=%u draw_tag=%u prim=%u pipeline=%u", task->id, tag, prim, G.pipe_ref);
                uint32_t vr[MAXB], fr[MAXB];
                uint64_t vo[MAXB], fo[MAXB];
                for (uint32_t i = 0; i < MAXB; i++) vr[i] = G.vb[i].ref, vo[i] = G.vb[i].off, fr[i] = G.fb[i].ref, fo[i] = G.fb[i].off;
                print_maps(l, sizeof l, "vbuf", vr, vo, NULL);
                print_maps(l, sizeof l, "fbuf", fr, fo, NULL);
                print_maps(l, sizeof l, "ftex", NULL, NULL, G.tex);
                print_maps(l, sizeof l, "fsamp", NULL, NULL, G.samp);
                LAGFX_LOG("arm64-dump draw #%llu %s", (unsigned long long)p->diag2.exec.pipe.dump_draws, l);
                open_dir();
                index_line("draw n=%llu %s", (unsigned long long)p->diag2.exec.pipe.dump_draws, l);
                p->diag2.exec.pipe.dump_draws++;
            }
            break;
        default: break;
        }
    }
}
