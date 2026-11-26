/* ARM64 0x43/0x45 decode-only: task address space reads (Apple task_read layout: 16 KiB pages, 12-bit levels), the segment walker, and the counters,
 * driven through a virtual ring and a fake guest memory with a virtual task page table.
 * SPDX-License-Identifier: MIT */
#include "dispatchers/channel_compute_dispatcher.h"
#include "dispatchers/ring_common.h"
#include "protocol/state.h"
#include "protocol/arm64_exec_decode.h"
#include "device.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_SIZE   (2u << 20)
#define SHARED_PFN 1u
#define TABLE_PFN  2u
#define RING_PFN0  4u
#define ROOT_PFN   16u   /* task root page {top table, depth} */
#define TOP_PFN    17u   /* depth 2: top table -> leaf table 18 */
#define LEAF_PFN   18u
#define DATA_PFN0  32u   /* task vpn i -> PFN 32 + i for the mapped ones */
#define NVPN       24u

static uint8_t *g_mem;
static bool rd(void *o, uint64_t gpa, uint64_t len, void *dst) { (void)o; if (gpa + len > MEM_SIZE) return false; memcpy(dst, g_mem + gpa, len); return true; }
static bool wr(void *o, uint64_t gpa, uint64_t len, const void *src) { (void)o; if (gpa + len > MEM_SIZE) return false; memcpy(g_mem + gpa, src, len); return true; }
static void irq(void *o, uint32_t v) { (void)o; (void)v; }
static void put32(uint64_t gpa, uint32_t v) { memcpy(g_mem + gpa, &v, 4); }
static uint32_t g_wp;

static void submit(lagfx_protocol_t *p, uint16_t op, uint16_t arg8, const uint32_t *words, uint32_t nwords) {
    uint32_t len = 12u + nwords * 4u;
    uint64_t ring = (uint64_t)RING_PFN0 << 14;
    uint8_t cmd[2048]; memset(cmd, 0, sizeof cmd);
    assert(len <= sizeof cmd);
    memcpy(cmd, &op, 2); memcpy(cmd + 2, &arg8, 2); memcpy(cmd + 4, &len, 4);
    uint32_t stamp = g_wp + 1u; memcpy(cmd + 8, &stamp, 4);
    if (nwords) memcpy(cmd + 12, words, nwords * 4u);
    assert(g_wp % 0x10000u + len <= 0x10000u);   /* these tests never wrap the ring */
    memcpy(g_mem + ring + (g_wp % 0x10000u), cmd, len);
    g_wp += len;
    put32(((uint64_t)SHARED_PFN << 14) + LAGFX_RING_FIFO_DESC_BASE_OFFSET + 0x00, g_wp);
    p->current_chan_id = 1;
    channel_compute_dispatcher_drain(p, 1);
}

/* guest memory with a task page table: vpn 0..NVPN-1 -> data pages, except vpn 3 (unmapped) and vpn 5 (mapped with bit 31 set) */
static lagfx_protocol_t *mk(uint32_t mode) {
    memset(g_mem, 0, MEM_SIZE); g_wp = 0;
    lagfx_device_t *dev = calloc(1, sizeof *dev);
    dev->desc.shell.read_memory = rd; dev->desc.shell.write_memory = wr; dev->desc.shell.raise_interrupt = irq;
    lagfx_protocol_t *p = calloc(1, sizeof *p);
    p->magic = LAGFX_PROTOCOL_MAGIC; p->dev = dev;
    p->ring_page_shift = 14; p->protocol_mode = mode;
    p->ring_shared_page_pfn = SHARED_PFN;
    uint64_t d = ((uint64_t)SHARED_PFN << 14) + LAGFX_RING_FIFO_DESC_BASE_OFFSET;
    put32(d + 0x0c, 1); put32(d + 0x10, TABLE_PFN);
    for (uint32_t i = 0; i < 4; i++) put32(((uint64_t)TABLE_PFN << 14) + 4u * i, RING_PFN0 + i);
    put32((uint64_t)ROOT_PFN << 14, TOP_PFN); put32(((uint64_t)ROOT_PFN << 14) + 4, 2);
    put32((uint64_t)TOP_PFN << 14, LEAF_PFN);                       /* top index 0 -> leaf table */
    for (uint32_t v = 0; v < NVPN; v++) {
        if (v == 3) continue;
        put32(((uint64_t)LEAF_PFN << 14) + 4u * v, (DATA_PFN0 + v) | (v == 5 ? 0x80000000u : 0));
    }
    return p;
}

/* host-side write into task space (through the same table; asserts the page is mapped) */
static void va_write(uint64_t va, const void *src, uint32_t n) {
    const uint8_t *s = src;
    while (n) {
        uint32_t vpn = (uint32_t)(va >> 14), in = (uint32_t)(va & 0x3fff), c = 0x4000 - in < n ? 0x4000 - in : n;
        assert(vpn < NVPN && vpn != 3);
        memcpy(g_mem + ((uint64_t)(DATA_PFN0 + vpn) << 14) + in, s, c);
        va += c; s += c; n -= c;
    }
}

static lagfx_task_entry_t *def_task(lagfx_protocol_t *p, uint32_t task) {
    uint32_t w[4] = { task * 2u, 0, 0, ROOT_PFN };
    submit(p, 0x38, 0, w, 4);
    lagfx_task_entry_t *t = lagfx_protocol_find_task(p, task);
    assert(t && t->live && t->root_page_pfn == ROOT_PFN);
    return t;
}

/* ---- segment builders ---- */
static uint8_t *w32(uint8_t *b, uint32_t v) { memcpy(b, &v, 4); return b + 4; }
static uint32_t info(uint32_t type, uint32_t cont, uint32_t more) { return type | cont << 8 | more << 16; }
/* a command: {tag, size} + (size-8) filler bytes */
static uint8_t *cmd(uint8_t *b, uint32_t tag, uint32_t size) { b = w32(b, tag); b = w32(b, size); for (uint32_t i = 8; i < size; i++) *b++ = (uint8_t)i; return b; }
/* segment {seglen, info} + body; returns total bytes */
static uint32_t seg(uint8_t *out, uint32_t type, uint32_t cont, uint32_t more, uint32_t ntags, const uint32_t *tags, const uint32_t *sizes) {
    uint8_t *b = out + 8, *body = b;
    for (uint32_t i = 0; i < ntags; i++) b = cmd(b, tags[i], sizes[i]);
    w32(out, (uint32_t)(b - body)); w32(out + 4, info(type, cont, more));
    return (uint32_t)(b - out);
}

static uint32_t g_tags[64], g_ntags;
static void collect(void *u, uint32_t tag, uint32_t size) { (void)u; (void)size; if (g_ntags < 64) g_tags[g_ntags++] = tag; }

static void test_task_read(void) {
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    lagfx_task_entry_t *t = def_task(p, 1);
    uint8_t pat[0x9000], got[0x9000];
    for (uint32_t i = 0; i < sizeof pat; i++) pat[i] = (uint8_t)(i * 7 + 1);
    va_write(0x4000 * 1 + 0x3ff0, pat, 0x20);                       /* across vpn 1 -> 2 */
    memset(got, 0, 0x20);
    assert(lagfx_arm64_task_read(p, t, 0x4000 * 1 + 0x3ff0, got, 0x20) && !memcmp(got, pat, 0x20));
    va_write(0x4000 * 6, pat, 0x4000 * 2);                          /* two whole pages */
    assert(lagfx_arm64_task_read(p, t, 0x4000 * 6, got, 0x8000) && !memcmp(got, pat, 0x8000));
    /* bit 31 of the page entry is a flag, not part of the page number (vpn 5) */
    va_write(0x4000 * 5 + 8, pat, 16);
    assert(lagfx_arm64_task_read(p, t, 0x4000 * 5 + 8, got, 16) && !memcmp(got, pat, 16));
    /* unmapped page, also when only the tail of the read reaches it (vpn 2 -> 3) */
    assert(!lagfx_arm64_task_read(p, t, 0x4000 * 3, got, 4));
    assert(!lagfx_arm64_task_read(p, t, 0x4000 * 3 - 4, got, 8));
    assert(!lagfx_arm64_task_read(p, t, 0x4000 * NVPN, got, 4));    /* beyond the mapped range: leaf entry 0 */
    assert(lagfx_arm64_task_read(p, t, 0, got, 0));                 /* nothing to read */
    /* depth 1: the root's first word is the leaf table itself */
    put32(((uint64_t)ROOT_PFN << 14), LEAF_PFN); put32(((uint64_t)ROOT_PFN << 14) + 4, 1);
    va_write(0x4000 * 7 + 3, pat, 5);
    assert(lagfx_arm64_task_read(p, t, 0x4000 * 7 + 3, got, 5) && !memcmp(got, pat, 5));
    /* bad depth / no root / root outside memory */
    put32(((uint64_t)ROOT_PFN << 14) + 4, 0); assert(!lagfx_arm64_task_read(p, t, 0, got, 4));
    put32(((uint64_t)ROOT_PFN << 14) + 4, 4); assert(!lagfx_arm64_task_read(p, t, 0, got, 4));
    put32(((uint64_t)ROOT_PFN << 14) + 4, 1);
    lagfx_task_entry_t z = *t; z.root_page_pfn = 0; assert(!lagfx_arm64_task_read(p, &z, 0, got, 4));
    z.root_page_pfn = 0x7fffffffu; assert(!lagfx_arm64_task_read(p, &z, 0, got, 4));
    z.root_page_pfn = ~0ull; assert(!lagfx_arm64_task_read(p, &z, 0, got, 4));
    assert(!lagfx_arm64_task_read(p, NULL, 0, got, 4));
}

static void test_walker(void) {
    uint8_t b[512], *x;
    lagfx_exec_seg_t s;
    uint32_t tg[4] = {26, 30, 31, 7}, sz[4] = {8, 12, 16, 24};
    /* plain render segment */
    uint32_t n = seg(b, 0, 0, 1, 4, tg, sz);
    x = malloc(n); memcpy(x, b, n);                                 /* exact-size heap block: any read past n is an ASAN/valgrind hit */
    g_ntags = 0;
    assert(lagfx_exec_walk_segment(x, n, n, &s, collect, NULL) == LAGFX_SEG_OK);
    assert(s.type == 0 && s.cont == 0 && s.more == 1 && s.ninner == 4 && s.body_len == 60 && !s.clamped && !s.bad_inner && !s.truncated && s.tail == 0 && s.slack == 0);
    assert(g_ntags == 4 && g_tags[0] == 26 && g_tags[3] == 7);
    free(x);
    /* type 5 protection header skipped, then the real one (8 option bytes in between, whatever they hold) */
    uint8_t pb[512]; uint32_t in = seg(b, 1, 1, 0, 2, tg + 1, sz + 1);
    w32(pb, 0x1234); w32(pb + 4, info(5, 0, 0)); memset(pb + 8, 0xee, 8); memcpy(pb + 16, b, in);
    x = malloc(16 + in); memcpy(x, pb, 16 + in);
    g_ntags = 0;
    assert(lagfx_exec_walk_segment(x, 16 + in, 16 + in, &s, collect, NULL) == LAGFX_SEG_OK);
    assert(s.prot_skipped && s.type == 1 && s.cont == 1 && s.more == 0 && s.ninner == 2 && g_ntags == 2 && g_tags[0] == 30);
    free(x);
    /* type 5 with no room for the real header: short */
    x = malloc(23); memcpy(x, pb, 23); assert(lagfx_exec_walk_segment(x, 23, 23, &s, NULL, NULL) == LAGFX_SEG_SHORT); free(x);
    x = malloc(24); memcpy(x, pb, 16); memset(x + 16, 0, 8);        /* an empty real segment: fine */
    assert(lagfx_exec_walk_segment(x, 24, 24, &s, NULL, NULL) == LAGFX_SEG_OK && s.prot_skipped && s.ninner == 0 && s.type == 0); free(x);
    /* shorter than a header */
    x = malloc(7); memset(x, 0, 7); assert(lagfx_exec_walk_segment(x, 7, 7, &s, NULL, NULL) == LAGFX_SEG_SHORT); free(x);
    /* seglen larger than the buffer: clamped (Apple does the same), walked up to the buffer's end */
    n = seg(b, 2, 0, 0, 4, tg, sz); w32(b, 0x10000);
    x = malloc(n); memcpy(x, b, n); assert(lagfx_exec_walk_segment(x, n, n, &s, NULL, NULL) == LAGFX_SEG_OK);
    assert(s.clamped && s.type == 2 && s.ninner == 4 && s.body_len == n - 8 && s.seglen == 0x10000); free(x);
    /* seglen smaller than the buffer: the bytes after it are tail, not walked */
    n = seg(b, 1, 0, 0, 4, tg, sz); w32(b, 20);                     /* the first two commands only (8 + 12) */
    x = malloc(n); memcpy(x, b, n); assert(lagfx_exec_walk_segment(x, n, n, &s, NULL, NULL) == LAGFX_SEG_OK);
    assert(s.ninner == 2 && s.tail == n - 8 - 20 && !s.clamped); free(x);
    /* a command of size < 8, or running past the body: stop, flagged */
    uint32_t bad1[1] = {9}, bad1s[1] = {4};
    n = seg(b, 1, 0, 0, 1, tg, sz); memcpy(b + 8, &(uint32_t){9}, 4); memcpy(b + 12, &bad1s[0], 4); (void)bad1; w32(b, 8);
    x = malloc(16); memcpy(x, b, 16); assert(lagfx_exec_walk_segment(x, 16, 16, &s, NULL, NULL) == LAGFX_SEG_OK && s.bad_inner && s.ninner == 0); free(x);
    n = seg(b, 1, 0, 0, 2, tg, sz); w32(b + 8 + 8 + 4, 0x7fffffff);   /* 2nd command claims 2 GiB */
    x = malloc(n); memcpy(x, b, n); assert(lagfx_exec_walk_segment(x, n, n, &s, NULL, NULL) == LAGFX_SEG_OK && s.bad_inner && s.ninner == 1); free(x);
    /* fewer than 8 bytes left inside the body: slack, not an error */
    n = seg(b, 1, 0, 0, 1, tg, sz); w32(b, 8 + 5); memset(b + 16, 0, 5); n += 5;
    x = malloc(n); memcpy(x, b, n); assert(lagfx_exec_walk_segment(x, n, n, &s, NULL, NULL) == LAGFX_SEG_OK && s.ninner == 1 && s.slack == 5 && !s.bad_inner); free(x);
    /* read cap: only the first `avail` bytes exist; a command crossing it is neither walked nor an error; nothing past avail is touched */
    n = seg(b, 0, 0, 0, 4, tg, sz);
    x = malloc(8 + 8 + 12 + 5); memcpy(x, b, 8 + 8 + 12 + 5);
    assert(lagfx_exec_walk_segment(x, 8 + 8 + 12 + 5, n, &s, NULL, NULL) == LAGFX_SEG_OK && s.truncated && !s.bad_inner && s.ninner == 2); free(x);
    /* the inner-command walk is capped */
    uint8_t *big = malloc(8 + 8 * (LAGFX_EXEC_MAX_INNER + 10)); w32(big, 8 * (LAGFX_EXEC_MAX_INNER + 10)); w32(big + 4, info(1, 0, 0));
    for (uint32_t i = 0; i < LAGFX_EXEC_MAX_INNER + 10; i++) { w32(big + 8 + 8 * i, 1); w32(big + 12 + 8 * i, 8); }
    assert(lagfx_exec_walk_segment(big, 8 + 8 * (LAGFX_EXEC_MAX_INNER + 10), 8 + 8 * (LAGFX_EXEC_MAX_INNER + 10), &s, NULL, NULL) == LAGFX_SEG_OK && s.ninner == LAGFX_EXEC_MAX_INNER);
    free(big);
}

/* an outer command: args = {task, nres, nbuf}, nres * {ref, flags}, nbuf * {off, len}; arg8 barrier slots (8 bytes each) in front */
static uint32_t outer(uint32_t *w, uint32_t arg8, uint32_t task, uint32_t nres, const uint32_t *rf, uint32_t nbuf, const uint64_t *ol) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < arg8 * 2; i++) w[n++] = 0xb0000000u + i;
    w[n++] = task, w[n++] = nres, w[n++] = nbuf;
    for (uint32_t i = 0; i < nres * 2; i++) w[n++] = rf[i];
    for (uint32_t i = 0; i < nbuf; i++) { w[n++] = (uint32_t)ol[2 * i]; w[n++] = (uint32_t)(ol[2 * i] >> 32); w[n++] = (uint32_t)ol[2 * i + 1]; w[n++] = (uint32_t)(ol[2 * i + 1] >> 32); }
    return n;
}

#define X(p) ((p)->diag2.exec)

static void test_exec(void) {
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    lagfx_task_entry_t *t = def_task(p, 3);
    /* object list at task VA 0x20000 (vpn 8), len 0x3000: ref 5 = type 0xb size 0x40, ref 6 = type 1 */
    uint32_t l[7] = {3, 0x20000, 0, 0x30000, 0, 0x3000, 0x1000}; submit(p, 0x44, 0, l, 7);
    assert(t->lists_set && t->objects_addr == 0x20000);
    uint32_t e5[3] = {0xb | 0x40u << 8, 0x1000, 0}, e6[3] = {1 | 0x200u << 8, 0x2000, 0};
    va_write(0x20000 + 5 * 12, e5, 12); va_write(0x20000 + 6 * 12, e6, 12);

    /* three buffers: render (more) -> render (cont) -> info with a protection header; one exec, barrier slots arg8=2 */
    uint8_t b[512]; uint32_t n;
    uint32_t t0[3] = {26, 30, 34}, s0[3] = {40, 12, 12}, t1[2] = {7, 8}, s1[2] = {16, 8}, t2[2] = {450, 451}, s2[2] = {24, 16};
    n = seg(b, 0, 0, 1, 3, t0, s0); va_write(0x4000 * 9, b, n); uint32_t n0 = n;
    n = seg(b, 0, 1, 0, 2, t1, s1); va_write(0x4000 * 10 + 0x3ff8, b, n); uint32_t n1 = n;   /* straddles vpn 10 -> 11 */
    uint8_t ib[512]; n = seg(ib, 4, 0, 0, 2, t2, s2);
    w32(b, 0); w32(b + 4, info(5, 0, 0)); memset(b + 8, 0xaa, 8); memcpy(b + 16, ib, n); n += 16; va_write(0x4000 * 12, b, n); uint32_t n2 = n;
    uint32_t rf[6] = {5, 0xe1, 6, 0x6, 0x1000, 0x0};      /* ref 5 flag 1 (+high bits), ref 6 flags 2|4, ref 0x1000 beyond the 0x3000-byte list */
    uint64_t ol[6] = {0x4000ull * 9, n0, 0x4000ull * 10 + 0x3ff8, n1, 0x4000ull * 12, n2};
    uint32_t w[300], nw = outer(w, 2, 3, 3, rf, 3, ol);
    uint64_t un = p->unknown_opcode_count;
    submit(p, 0x43, 2, w, nw);
    assert(X(p).received == 1 && X(p).decoded == 1 && X(p).failed == 0);
    assert(X(p).bufs == 3 && X(p).seg_t[0] == 2 && X(p).seg_t[4] == 1 && X(p).seg_t[5] == 1 && X(p).seg_t[1] == 0 && X(p).seg_t[2] == 0 && X(p).seg_other == 0);
    assert(X(p).seg_cont == 1 && X(p).seg_more == 1 && X(p).inner == 7 && X(p).inner_bad == 0 && X(p).bytes_read == (uint64_t)n0 + n1 + n2);
    assert(X(p).tag_hist[0][26] == 1 && X(p).tag_hist[0][30] == 1 && X(p).tag_hist[0][34] == 1 && X(p).tag_hist[0][7] == 1 && X(p).tag_hist[3][450] == 1 && X(p).tag_hist[3][451] == 1);
    assert(X(p).res == 3 && X(p).res_flag[0] == 1 && X(p).res_flag[1] == 1 && X(p).res_flag[2] == 1 && X(p).res_flag_hi == 1);
    assert(X(p).obj_kind[0xb] == 1 && X(p).obj_kind[1] == 1 && X(p).obj_missing == 1);
    assert(p->unknown_opcode_count == un && p->last_completed_stamp != 0);
    uint32_t st = p->last_completed_stamp;

    /* the same through opcode 0x45 and without barrier slots */
    nw = outer(w, 0, 3, 0, NULL, 1, ol); submit(p, 0x45, 0, w, nw);
    assert(X(p).received == 2 && X(p).decoded == 2 && p->last_completed_stamp != st);

    /* unreadable: the buffer sits on the unmapped vpn 3 / beyond the table; the exec is counted failed, others in it still decoded */
    uint64_t ol2[4] = {0x4000ull * 3, 64, 0x4000ull * 9, n0};
    nw = outer(w, 0, 3, 0, NULL, 2, ol2); submit(p, 0x43, 0, w, nw);
    assert(X(p).received == 3 && X(p).failed == 1 && X(p).read_failed == 1 && X(p).decoded == 2 && X(p).seg_t[0] == 4);
    /* a task that was never defined: counted, nothing read */
    nw = outer(w, 0, 9, 0, NULL, 1, ol); submit(p, 0x43, 0, w, nw);
    assert(X(p).received == 4 && X(p).failed == 2 && X(p).no_task == 1 && X(p).read_failed == 2);

    /* sizes: counts that do not fit the payload, too many buffers, buffer len < 8 / > 64 MiB, payload shorter than the counts */
    uint64_t before_bytes = X(p).bytes_read;
    w[0] = 3, w[1] = 1000, w[2] = 1; submit(p, 0x43, 0, w, 3);                      /* nres 1000 does not fit */
    w[0] = 3, w[1] = 0, w[2] = 65; submit(p, 0x43, 0, w, 3 + 65 * 4);               /* nbuf > 64 */
    w[0] = 3, w[1] = 0xffffffffu, w[2] = 0xffffffffu; submit(p, 0x43, 0, w, 3);     /* 32-bit overflow attempt */
    submit(p, 0x43, 0, w, 2);                                                       /* shorter than the three counts */
    submit(p, 0x43, 3, w, 1);                                                       /* barrier slots eat the payload */
    assert(X(p).received == 9 && X(p).failed == 7 && X(p).bad_size == 5);
    uint64_t ol3[6] = {0x4000ull * 9, 7, 0x4000ull * 9, (65ull << 20), 0x4000ull * 9, ~0ull};
    nw = outer(w, 0, 3, 0, NULL, 3, ol3); submit(p, 0x43, 0, w, nw);
    assert(X(p).received == 10 && X(p).failed == 8 && X(p).bad_size == 5 + 3 && X(p).bytes_read == before_bytes);

    /* a segment whose seglen runs past its buffer is clamped (counted, not failed); a bad inner size fails the exec but not the others */
    n = seg(b, 1, 0, 0, 2, t1, s1); w32(b, 0x100000); va_write(0x4000 * 14, b, n);
    n = seg(b, 1, 0, 0, 2, t1, s1); w32(b + 12, 3); va_write(0x4000 * 15, b, n);    /* first command size 3 */
    uint64_t ol4[4] = {0x4000ull * 14, n, 0x4000ull * 15, n};
    uint64_t cl = X(p).seg_clamped; nw = outer(w, 0, 3, 0, NULL, 1, ol4); submit(p, 0x43, 0, w, nw);
    assert(X(p).seg_clamped == cl + 1 && X(p).failed == 8 && X(p).decoded == 3 && X(p).seg_t[1] == 1);
    nw = outer(w, 0, 3, 0, NULL, 1, ol4 + 2); submit(p, 0x43, 0, w, nw);
    assert(X(p).inner_bad == 1 && X(p).bad_seg == 1 && X(p).failed == 9 && X(p).decoded == 3);
    assert(X(p).decoded + X(p).failed == X(p).received);

    /* a header-only buffer (8 bytes: no segment body at all) decodes; 0x0a / 0x0c are ignored-known, not unknown */
    uint8_t hb[8]; w32(hb, 0); w32(hb + 4, info(2, 0, 0)); va_write(0x4000 * 16, hb, 8);
    uint64_t ol5[2] = {0x4000ull * 16, 8}; nw = outer(w, 0, 3, 0, NULL, 1, ol5); submit(p, 0x43, 0, w, nw);
    assert(X(p).decoded == 4 && X(p).seg_t[2] == 1);
    un = p->unknown_opcode_count; uint32_t z[2] = {1, 2};
    submit(p, 0x0a, 0, z, 2); submit(p, 0x0c, 0, z, 2);
    assert(p->diag2.ignored_known == 2 && p->unknown_opcode_count == un);

    /* x86 mode: none of this applies; 0x43 stays unknown, 0x0c stays unknown, no ignored-known */
    lagfx_protocol_t *x = mk(LAGFX_PROTO_X86); uint64_t xu = x->unknown_opcode_count;
    nw = outer(w, 0, 3, 0, NULL, 1, ol5); submit(x, 0x43, 0, w, nw); submit(x, 0x0c, 0, z, 2);
    assert(X(x).received == 0 && x->diag2.ignored_known == 0 && x->unknown_opcode_count == xu + 2);
}

/* Real data: the first 64 bytes of two 0x43 commands from a device run (g7_lagfx log, 'diag cmd #22' len=72 and #21 len=88). The first 12 bytes are the
 * command header; the rest is {task, nres, nbuf}, resource {ref, flags} pairs and the first buffer's offset. The log cut the raw bytes at 64, so the
 * buffer length and the pairs/segments beyond are completed here synthetically, and the buffer contents are synthetic (never captured). */
static uint32_t hexw(const char *h, uint32_t *w, uint32_t maxw) {
    uint8_t by[128]; uint32_t n = 0;
    while (h[0] && h[1] && n < sizeof by) { by[n++] = (uint8_t)strtoul((char[3]){h[0], h[1], 0}, NULL, 16); h += h[2] ? 3 : 2; }
    assert(n % 4 == 0 && n / 4 <= maxw); memcpy(w, by, n); return n / 4;
}

static void test_real(void) {
    static const char c22[] = "43 00 00 00 48 00 00 00 00 02 00 00 02 00 00 00 04 00 00 00 01 00 00 00 19 00 00 00 02 00 00 00 0d 00 00 00 00 00 00 00 18 00 00 00 00 00 00 00 1d 00 00 00 01 00 00 00 00 40 21 00 00 00 00 00";
    uint32_t r[32], nr = hexw(c22, r, 32);
    assert(nr == 16 && r[0] == 0x43 && r[1] == 72 && r[3] == 2 && r[4] == 4 && r[5] == 1);
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    def_task(p, 2);
    uint8_t b[256]; uint32_t tg[3] = {26, 30, 36}, sz[3] = {40, 12, 12}; uint32_t n = seg(b, 0, 0, 0, 3, tg, sz);
    /* real offset 0x214000 -> vpn 133: our table only maps vpn < NVPN, so alias it: map vpn 133 to a data page */
    put32(((uint64_t)LEAF_PFN << 14) + 4u * 133u, DATA_PFN0 + 20u);
    memcpy(g_mem + ((uint64_t)(DATA_PFN0 + 20u) << 14), b, n);
    uint32_t w[32], k = 0;
    for (uint32_t i = 3; i < 16; i++) w[k++] = r[i];                 /* task .. real buffer offset (low, high words) */
    w[k++] = n; w[k++] = 0;                                         /* buffer length: synthetic */
    assert(k == 15 && (12 + 15 * 4) == 72);
    submit(p, 0x43, 0, w, k);
    assert(X(p).received == 1 && X(p).decoded == 1 && X(p).failed == 0 && X(p).bufs == 1 && X(p).seg_t[0] == 1 && X(p).inner == 3);
    assert(X(p).res == 4 && X(p).res_flag[0] == 1 && X(p).res_flag[1] == 1 && X(p).res_flag[2] == 0 && X(p).obj_missing == 4);   /* no 0x44 seen: no object lookups */
    /* #21: flags carry bits above 7 (0xe2, 0xe0, 0xe1): recorded as seen, only 1/2/4 mean something */
    static const char c21[] = "43 00 00 00 58 00 00 00 00 01 00 00 02 00 00 00 06 00 00 00 01 00 00 00 08 00 00 00 e2 00 00 00 0d 00 00 00 e0 00 00 00 0e 00 00 00 e1 00 00 00 14 00 00 00 e1 00 00 00 0f 00 00 00 e0 00 00 00";
    nr = hexw(c21, r, 32);
    assert(nr == 16 && r[4] == 6 && r[5] == 1 && r[7] == 0xe2 && r[9] == 0xe0 && r[11] == 0xe1);
    uint32_t rf[12] = {r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15], 0x10, 0x4};   /* 5 real pairs + 1 synthetic */
    uint64_t ol[2] = {0x214000, n};
    uint64_t f1 = X(p).res_flag[0], f2 = X(p).res_flag[1], hi = X(p).res_flag_hi;
    uint32_t nw = outer(w, 0, 2, 6, rf, 1, ol); submit(p, 0x43, 0, w, nw);
    assert(X(p).received == 2 && X(p).decoded == 2 && X(p).res == 10);
    assert(X(p).res_flag[0] == f1 + 2 && X(p).res_flag[1] == f2 + 1 && X(p).res_flag[2] == 1 && X(p).res_flag_hi == hi + 5);
}

int main(void) {
    g_mem = calloc(1, MEM_SIZE);
    test_task_read();
    test_walker();
    test_exec();
    test_real();
    puts("arm64-exec-decode-unit: ok");
    return 0;
}
