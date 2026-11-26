/*
 * libapplegfx-vulkan — ARM64 GPU exec (0x43 / 0x45), decode only. See arm64_exec_decode.h.
 */
#include "arm64_exec_decode.h"
#include "../device.h"
#include "../common/le.h"
#include "../common/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- task address space (mirrors Apple pvg_lite.c task_ptr / task_read) ---- */

static bool rd_mem(lagfx_protocol_t *p, uint64_t gpa, uint32_t n, void *dst) {
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    return dev && dev->desc.shell.read_memory && dev->desc.shell.read_memory(dev->desc.shell.opaque, gpa, n, dst);
}

static bool pfn_gpa(uint64_t pfn, uint64_t *gpa) {
    if (pfn >> 50) return false;
    *gpa = pfn << 14;
    return true;
}

bool lagfx_arm64_task_read(lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint64_t va, void *dst, uint64_t n) {
    if (!p || !task || !dst || !task->root_page_pfn) return false;
    uint64_t rgpa;
    uint8_t hdr[8];
    if (!pfn_gpa(task->root_page_pfn, &rgpa) || !rd_mem(p, rgpa, 8, hdr)) return false;
    uint32_t top = lagfx_le32(hdr), depth = lagfx_le32(hdr + 4);
    if (depth < 1u || depth > 3u) return false;
    uint8_t *d = dst;
    while (n) {
        uint64_t vpn = va >> 14;
        uint32_t page = top;
        for (int level = (int)depth - 1; level >= 0; level--) {
            uint64_t tgpa;
            uint8_t w[4];
            if (!page || !pfn_gpa(page, &tgpa) || !rd_mem(p, tgpa + (((vpn >> (12 * level)) & 0xfffu) * 4u), 4, w)) return false;
            page = lagfx_le32(w) & 0x7fffffffu;
        }
        uint64_t pgpa;
        if (!page || !pfn_gpa(page, &pgpa)) return false;
        uint32_t in = (uint32_t)(va & 0x3fffu), avail = 0x4000u - in;
        uint32_t c = n < avail ? (uint32_t)n : avail;
        if (!rd_mem(p, pgpa + in, c, d)) return false;
        d += c, va += c, n -= c;
    }
    return true;
}

/* ---- segment walker (mirrors the Apple loop, with bounds) ---- */

int lagfx_exec_walk_segment(const uint8_t *buf, uint32_t avail, uint64_t len, lagfx_exec_seg_t *s, lagfx_exec_tag_fn fn, void *user) {
    memset(s, 0, sizeof *s);
    if (!buf || avail > len || len < 8u || avail < 8u) return LAGFX_SEG_SHORT;
    uint64_t pos = 8;
    uint32_t seglen = lagfx_le32(buf), info = lagfx_le32(buf + 4);
    if ((info & 0xffu) == 5u) {                      /* protection options: 8 bytes, then the real header */
        pos += 8;
        if (pos + 8 > len) return LAGFX_SEG_SHORT;   /* Apple: p + 8 > end -> skip the buffer */
        if (pos + 8 > avail) { s->truncated = true; return LAGFX_SEG_SHORT; }
        seglen = lagfx_le32(buf + pos), info = lagfx_le32(buf + pos + 4);
        pos += 8;
        s->prot_skipped = true;
    }
    s->type = info & 0xffu, s->cont = (info >> 8) & 0xffu, s->more = (info >> 16) & 0xffu, s->hdr_top = info >> 24;
    s->seglen = seglen;
    uint64_t room = len - pos;                       /* bytes after the header(s) */
    uint64_t body = seglen;
    if (body > room) body = room, s->clamped = true; /* Apple: `if (p + seglen < end) end = p + seglen` -> clamps silently */
    s->body_len = (uint32_t)body;
    s->tail = (uint32_t)(room - body);
    uint64_t have = avail > pos ? avail - pos : 0;   /* bytes of the body actually in buf */
    uint64_t lim = body < have ? body : have;
    if (lim < body) s->truncated = true;
    uint64_t q = 0;
    while (q + 8 <= lim && s->ninner < LAGFX_EXEC_MAX_INNER) {
        uint32_t tag = lagfx_le32(buf + pos + q), sz = lagfx_le32(buf + pos + q + 4);
        if (sz < 8u || q + sz > body) { s->bad_inner = true; break; }
        if (q + sz > lim) break;                     /* the command goes on past what was read: not an error */
        if (fn) fn(user, tag, sz);
        s->ninner++;
        q += sz;
    }
    if (!s->bad_inner && !s->truncated && s->ninner < LAGFX_EXEC_MAX_INNER) s->slack = (uint32_t)(body - q);
    return LAGFX_SEG_OK;
}

/* ---- the command ---- */

typedef struct { uint32_t type, tag, n; } tagn_t;
typedef struct {
    lagfx_protocol_t *p;
    uint32_t type;
    tagn_t d[24];
    uint32_t nd;
    uint64_t newtags;
} tagctx_t;

static int hist_row(uint32_t type) { return type == 0 ? 0 : type == 1 ? 1 : type == 2 ? 2 : type == 4 ? 3 : -1; }

static void on_tag(void *u, uint32_t tag, uint32_t size) {
    (void)size;
    tagctx_t *c = u;
    lagfx_protocol_t *p = c->p;
    p->diag2.exec.inner++;
    int row = hist_row(c->type);
    if (row >= 0) {
        if (tag < 512u) {
            if (p->diag2.exec.tag_hist[row][tag]++ == 0 && p->diag2.exec.tags_logged < 96u) {
                p->diag2.exec.tags_logged++;
                LAGFX_LOG("compute: arm64 exec new inner tag: seg_type=%u tag=%u size=%u (first seen; %u logged of max 96)", c->type, tag, size, p->diag2.exec.tags_logged);
            }
        } else p->diag2.exec.tag_over++;
    }
    for (uint32_t i = 0; i < c->nd; i++)
        if (c->d[i].type == c->type && c->d[i].tag == tag) { c->d[i].n++; return; }
    if (c->nd < 24u) c->d[c->nd].type = c->type, c->d[c->nd].tag = tag, c->d[c->nd].n = 1, c->nd++;
}

typedef struct { char *b; size_t n, cap; } sb_t;
static void sb_add(sb_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_add(sb_t *s, const char *fmt, ...) {
    if (s->n + 1 >= s->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(s->b + s->n, s->cap - s->n, fmt, ap);
    va_end(ap);
    if (r > 0) s->n = (size_t)r < s->cap - s->n ? s->n + (size_t)r : s->cap - 1;
}

void lagfx_arm64_exec_decode(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) return;
    __typeof__(p->diag2.exec) *E = &p->diag2.exec;
    uint64_t k = E->received++;
    bool logit = k < 32u || (k % 256u) == 0u;
    char line[1400];
    sb_t sb = { line, 0, sizeof line };
    uint32_t skip = (uint32_t)hdr->arg_count_8b * 8u;
    bool failed = false;
    if (!hdr->payload || hdr->payload_size < skip + 12u) {
        E->bad_size++, E->failed++;
        if (logit) LAGFX_WARN("compute: 0x%02x (arm64) exec #%llu payload too short (%u, skip %u)", hdr->opcode, (unsigned long long)(k + 1u), (unsigned)hdr->payload_size, skip);
        return;
    }
    const uint8_t *a = hdr->payload + skip;
    uint64_t avail_args = hdr->payload_size - skip;
    uint32_t task = lagfx_le32(a), nres = lagfx_le32(a + 4), nbuf = lagfx_le32(a + 8);
    if (nres > LAGFX_EXEC_MAX_RES || nbuf > LAGFX_EXEC_MAX_BUFS || 12ull + 8ull * nres + 16ull * nbuf > avail_args) {
        E->bad_size++, E->failed++;
        if (logit) LAGFX_WARN("compute: 0x%02x (arm64) exec #%llu ch=%u task=%u BAD-SIZE nres=%u nbuf=%u args=%llu", hdr->opcode, (unsigned long long)(k + 1u),
                              (unsigned)p->current_chan_id, task, nres, nbuf, (unsigned long long)avail_args);
        return;
    }
    const uint8_t *res = a + 12, *bufs = res + 8ull * nres;
    lagfx_task_entry_t *t = lagfx_protocol_find_task(p, task);
    if (!t || !t->root_page_pfn) E->no_task++, failed = true;
    sb_add(&sb, "compute: 0x%02x (arm64) exec #%llu ch=%u task=%u live=%d nres=%u nbuf=%u (decode only, not executed)", hdr->opcode, (unsigned long long)(k + 1u),
           (unsigned)p->current_chan_id, task, t != NULL, nres, nbuf);

    /* resources: ref, flags (Apple reads flags & 1, 2, 4; the rest is kept as seen), and the object entry {type | size << 8, addr lo, addr hi} */
    sb_add(&sb, " res[");
    for (uint32_t i = 0; i < nres; i++) {
        uint32_t ref = lagfx_le32(res + 8u * i), fl = lagfx_le32(res + 8u * i + 4);
        E->res++;
        for (int b = 0; b < 3; b++) if (fl & (1u << b)) E->res_flag[b]++;
        if (fl & ~7u) E->res_flag_hi++;
        uint32_t e[3] = {0, 0, 0};
        bool have = false;
        if (ref && t && t->lists_set && t->objects_addr && (!t->objects_len || ((uint64_t)ref + 1u) * 12u <= t->objects_len)) {
            uint8_t eb[12];
            if (lagfx_arm64_task_read(p, t, t->objects_addr + (uint64_t)ref * 12u, eb, 12)) {
                e[0] = lagfx_le32(eb), e[1] = lagfx_le32(eb + 4), e[2] = lagfx_le32(eb + 8);
                have = true;
            }
        }
        if (have) E->obj_kind[(e[0] & 0xffu) < 31u ? (e[0] & 0xffu) : 31u]++;
        else if (ref) E->obj_missing++;
        if (logit && i < 12u) {
            if (have) sb_add(&sb, " %u:%02x(k%u/%u)", ref, fl, e[0] & 0xffu, e[0] >> 8);
            else sb_add(&sb, " %u:%02x(?)", ref, fl);
        }
    }
    if (logit && nres > 12u) sb_add(&sb, " +%u", nres - 12u);
    sb_add(&sb, " ]");

    /* buffers */
    uint32_t budget = LAGFX_EXEC_READ_CAP_TOTAL;
    uint8_t *scratch = NULL;
    uint32_t scratch_cap = 0;
    tagctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.p = p;
    for (uint32_t b = 0; b < nbuf; b++) {
        uint64_t off = lagfx_le64(bufs + 16u * b), blen = lagfx_le64(bufs + 16u * b + 8);
        E->bufs++;
        if (blen > LAGFX_EXEC_MAX_BUF_BYTES || blen < 8u) {   /* Apple skips these without a word */
            E->bad_size++, failed = true;
            if (logit) sb_add(&sb, " b%u[off=0x%llx len=%llu BAD-SIZE]", b, (unsigned long long)off, (unsigned long long)blen);
            continue;
        }
        uint32_t want = blen > LAGFX_EXEC_READ_CAP_BUF ? LAGFX_EXEC_READ_CAP_BUF : (uint32_t)blen;
        if (want > budget) want = budget;
        if (want < blen) E->bytes_capped += blen - want;
        if (want < 8u) { E->truncated++; if (logit) sb_add(&sb, " b%u[off=0x%llx len=%llu CAPPED]", b, (unsigned long long)off, (unsigned long long)blen); continue; }
        if (want > scratch_cap) {
            uint8_t *n2 = realloc(scratch, want);
            if (!n2) { E->read_failed++, failed = true; continue; }
            scratch = n2, scratch_cap = want;
        }
        if (!t || !lagfx_arm64_task_read(p, t, off, scratch, want)) {
            E->read_failed++, failed = true;
            if (logit) sb_add(&sb, " b%u[off=0x%llx len=%llu UNREADABLE]", b, (unsigned long long)off, (unsigned long long)blen);
            continue;
        }
        budget -= want;
        E->bytes_read += want;
        {   /* bounded raw dump of the first buffers, so the real inner commands can be read from the log */
            static unsigned dumped;
            if (dumped < 12u) {
                char hx[3 * 96 + 1]; uint32_t dn = want < 96u ? want : 96u, k = 0;
                for (uint32_t i = 0; i < dn; i++) k += (uint32_t)snprintf(hx + k, sizeof hx - k, "%02x%s", scratch[i], (i & 15u) == 15u ? "|" : " ");
                LAGFX_LOG("exec-buf-dump #%u b%u off=0x%llx len=%llu first%u: %s", dumped, b, (unsigned long long)off, (unsigned long long)blen, dn, hx);
                dumped++;
            }
        }
        lagfx_exec_seg_t s;
        uint64_t inner_before = E->inner;
        /* the walker reports a segment's type only after the header, so tag counting needs it first: peek at it */
        uint32_t pt = lagfx_le32(scratch + 4) & 0xffu;
        if (pt == 5u && want >= 24u) pt = lagfx_le32(scratch + 20) & 0xffu;
        ctx.type = pt;
        int r = lagfx_exec_walk_segment(scratch, want, blen, &s, on_tag, &ctx);
        if (r != LAGFX_SEG_OK) {
            if (s.truncated) E->truncated++;
            else E->bad_seg++, failed = true;
            if (logit) sb_add(&sb, " b%u[off=0x%llx len=%llu SHORT-HEADER]", b, (unsigned long long)off, (unsigned long long)blen);
            continue;
        }
        if (s.prot_skipped) E->seg_t[5]++;
        if (s.type == 0 || s.type == 1 || s.type == 2 || s.type == 4) E->seg_t[s.type]++; else E->seg_other++;
        if (s.cont) E->seg_cont++;
        if (s.more) E->seg_more++;
        if (s.clamped) E->seg_clamped++;
        if (s.truncated) E->truncated++;
        E->tail_bytes += s.tail;
        if (s.bad_inner) E->inner_bad++, E->bad_seg++, failed = true;
        if (logit) {
            sb_add(&sb, " b%u[off=0x%llx len=%llu%s t%u c%u m%u top%u seglen=%u n=%u", b, (unsigned long long)off, (unsigned long long)blen, s.prot_skipped ? " prot" : "",
                   s.type, s.cont, s.more, s.hdr_top, s.seglen, s.ninner);
            if (s.clamped) sb_add(&sb, " CLAMPED");
            if (s.tail) sb_add(&sb, " tail=%u", s.tail);
            if (s.bad_inner) sb_add(&sb, " BAD-INNER");
            sb_add(&sb, "]");
        }
        (void)inner_before;
    }
    free(scratch);
    if (logit) {
        sb_add(&sb, " tags[");
        for (uint32_t i = 0; i < ctx.nd; i++) sb_add(&sb, "%st%u:%u x%u", i ? " " : "", ctx.d[i].type, ctx.d[i].tag, ctx.d[i].n);
        sb_add(&sb, "]");
    }
    if (failed) E->failed++; else E->decoded++;
    if (logit) LAGFX_LOG("%s %s", line, failed ? "FAILED" : "ok");
}
