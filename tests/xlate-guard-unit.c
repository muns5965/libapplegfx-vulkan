/* ARM64 mode must never run the legacy (x86, 4 KiB) task translator chain: compute op 0x37 and root op 0x20 are explicitly rejected (counted, stamp
 * completed once, no guest read at an untranslated address used as a GPA); the legacy walker and its identity fallbacks stay as they were in x86 mode.
 * Fixture: 16 KiB ring pages, fake guest memory that records which addresses were read / written.
 * SPDX-License-Identifier: MIT */
#include "dispatchers/channel_compute_dispatcher.h"
#include "dispatchers/channel_0_dispatcher.h"
#include "dispatchers/ring_common.h"
#include "handlers/handlers.h"
#include "handlers/compute/info_replies.h"
#include "protocol/state.h"
#include "protocol/object_resolver.h"
#include "device.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include)
#  if __has_include("protocol/xlate_guard.h")
#    include "protocol/xlate_guard.h"
#    define HAVE_XG 1
#  endif
#endif
#ifndef HAVE_XG
#  define HAVE_XG 0
#endif

#define MEM_SIZE   (1u << 20)
#define SHARED_PFN 1u      /* gpa 0x4000 */
#define TABLE_PFN  2u
#define RING_PFN0  4u
#define SENT       0x50000u   /* guest-physical sentinel: only an identity (VA used as GPA) access can reach it */
#define SENT_LEN   0x40u

static uint8_t *g_mem;
static unsigned g_sent_reads, g_sent_writes;
static bool in_sent(uint64_t gpa, uint64_t len) { return gpa < SENT + 0x1000u && gpa + len > SENT; }
static bool rd(void *o, uint64_t gpa, uint64_t len, void *dst) { (void)o; if (gpa + len > MEM_SIZE) return false; if (in_sent(gpa, len)) g_sent_reads++; memcpy(dst, g_mem + gpa, len); return true; }
static bool wr(void *o, uint64_t gpa, uint64_t len, const void *src) { (void)o; if (gpa + len > MEM_SIZE) return false; if (in_sent(gpa, len)) g_sent_writes++; memcpy(g_mem + gpa, src, len); return true; }
static void irq(void *o, uint32_t v) { (void)o; (void)v; }
static void put32(uint64_t gpa, uint32_t v) { memcpy(g_mem + gpa, &v, 4); }
static uint32_t g_wp, g_stamp;

static void submit(lagfx_protocol_t *p, uint16_t op, uint16_t arg8, const uint32_t *words, uint32_t nwords) {
    uint32_t len = 12u + nwords * 4u;
    uint64_t ring = (uint64_t)RING_PFN0 << 14;
    uint8_t cmd[256]; memset(cmd, 0, sizeof cmd);
    memcpy(cmd, &op, 2); memcpy(cmd + 2, &arg8, 2); memcpy(cmd + 4, &len, 4);
    uint32_t stamp = g_wp + 1u; g_stamp = stamp; memcpy(cmd + 8, &stamp, 4);
    if (nwords) memcpy(cmd + 12, words, nwords * 4u);
    memcpy(g_mem + ring + (g_wp % 0x10000u), cmd, len);
    g_wp += len;
    put32(((uint64_t)SHARED_PFN << 14) + LAGFX_RING_FIFO_DESC_BASE_OFFSET + 0x00, g_wp);
    p->current_chan_id = 1;
    channel_compute_dispatcher_drain(p, 1);
}

static lagfx_protocol_t *mk(uint32_t mode) {
    memset(g_mem, 0, MEM_SIZE); g_wp = 0; g_sent_reads = g_sent_writes = 0;
    lagfx_device_t *dev = calloc(1, sizeof *dev);
    dev->desc.shell.read_memory = rd; dev->desc.shell.write_memory = wr; dev->desc.shell.raise_interrupt = irq;
    lagfx_protocol_t *p = calloc(1, sizeof *p);
    p->magic = LAGFX_PROTOCOL_MAGIC; p->dev = dev;
    p->ring_page_shift = 14; p->protocol_mode = mode;
    p->ring_shared_page_pfn = SHARED_PFN;
    uint64_t d = ((uint64_t)SHARED_PFN << 14) + LAGFX_RING_FIFO_DESC_BASE_OFFSET;
    put32(d + 0x0c, 1); put32(d + 0x10, TABLE_PFN);
    for (uint32_t i = 0; i < 4; i++) put32(((uint64_t)TABLE_PFN << 14) + 4u * i, RING_PFN0 + i);
    /* a valid-looking encoder segment at the sentinel, so an identity read would be walked as a command buffer */
    uint32_t seg[2] = { SENT_LEN, 2u };
    memcpy(g_mem + SENT, seg, 8);
    return p;
}

/* A task whose root page is garbage: every translation (legacy or arm64) fails. */
static lagfx_task_entry_t *def_task(lagfx_protocol_t *p, uint32_t task, uint32_t root_pfn) {
    uint32_t w[4] = { task * 2u, 0, 0, root_pfn };
    submit(p, 0x38, 0, w, 4);
    lagfx_task_entry_t *t = lagfx_protocol_find_task(p, task);
    assert(t && t->live);
    return t;
}

/* Legacy CmdExecIndirect2 / 0x37 layout: {task, desc_count=0, res_count=1, res{addr u64, len u32, pad u32}} pointing at SENT (as a "VA"). */
static const uint32_t LEG[7] = { 2u, 0u, 1u, SENT, 0u, SENT_LEN, 0u };

int main(void) {
    g_mem = calloc(1, MEM_SIZE);
#if HAVE_XG
    lagfx_xg_stats_t s;
#endif

    /* ---- x86 regression: the legacy walker runs, unmapped task VA falls back to identity (unchanged behaviour) ---- */
    {
        lagfx_protocol_t *x = mk(LAGFX_PROTO_X86);
        def_task(x, 2, 0x1234);
        uint64_t c0 = x->total_cmds_completed;
        submit(x, 0x37, 0, LEG, 7);
        assert(g_sent_reads > 0);                                   /* identity read still happens in x86 mode */
        assert(x->total_cmds_completed == c0 + 2);                  /* handler's own completion + the drain's: x86 behaviour untouched */
        /* root 0x20 in x86 mode reaches the same walker */
        g_sent_reads = 0;
        x->ring_armed = true; x->ring_size = 0x400; x->ring_start_offset = 0; x->ring_base_gpa = (uint64_t)RING_PFN0 << 14;
        uint8_t cmd[40] = {0}; uint16_t op = 0x20; uint32_t len = 40, st = 77;
        memcpy(cmd, &op, 2); memcpy(cmd + 4, &len, 4); memcpy(cmd + 8, &st, 4); memcpy(cmd + 12, LEG, 28);
        memcpy(g_mem + ((uint64_t)RING_PFN0 << 14), cmd, 40);
        x->read_ptr = 0; x->write_ptr = 40;
        assert(channel_0_dispatcher_drain(x) == 1);
        assert(g_sent_reads > 0 && x->last_completed_stamp == 77);
        /* info reply identity write in x86 mode (translate unavailable): unchanged */
        uint8_t body[16] = {0}; uint8_t res[16] = {0};
        uint64_t wa = SENT + 0x100u, rl = 0x100;
        memcpy(res, &wa, 8); memcpy(res + 8, &rl, 8);
        g_sent_writes = 0;
        lagfx_info_dispatch(x, 0x1c2u, body, sizeof body, res, 1, NULL, NULL);
        assert(g_sent_writes > 0);
        /* legacy 4 KiB translate is still the x86 walker */
        lagfx_task_entry_t *t = lagfx_protocol_find_task(x, 2);
        uint64_t gpa;
        assert(!lagfx_task_translate(x, t, 0x1000, &gpa));          /* root 0x1234<<12 is outside memory: false, as before */
#if HAVE_XG
        lagfx_xg_snapshot(&s);
        assert(s.legacy_xlate_in_arm64 == 0 && s.rejected_total == 0);   /* x86: the guard counts nothing */
#endif
    }

#if HAVE_XG
    lagfx_xg_reset();
#endif

    /* ---- arm64: compute 0x37 ---- */
    {
        lagfx_protocol_t *a = mk(LAGFX_PROTO_ARM64_VMAPPLE);
        def_task(a, 2, 0x1234);
        uint64_t c0 = a->total_cmds_completed;
        submit(a, 0x37, 0, LEG, 7);
        assert(g_sent_reads == 0 && g_sent_writes == 0);            /* no identity access to the untranslated address */
        assert(a->total_cmds_completed == c0 + 1);                  /* stamp completed exactly once (by the drain), the guest is not left waiting */
        assert(a->last_completed_stamp == g_stamp);                 /* ... and it is this command's stamp */
        /* with a barrier slot and a short payload: same */
        uint32_t w8[9] = { 0xdead, 0xbeef, 2u, 0u, 1u, SENT, 0u, SENT_LEN, 0u };
        submit(a, 0x37, 1, w8, 9);
        submit(a, 0x37, 0, NULL, 0);
        assert(g_sent_reads == 0);
        assert(a->total_cmds_completed == c0 + 3);
        assert(a->unknown_opcode_count == 0);
#if HAVE_XG
        lagfx_xg_snapshot(&s);
        assert(s.rejected[LAGFX_XG_REJ_COMPUTE_37] == 3 && s.rejected_total == 3);
        assert(s.legacy_xlate_in_arm64 == 0);                       /* the legacy chain was never entered */
#endif
        /* the backstop: calling the legacy walker directly in arm64 mode refuses, reads nothing, completes nothing */
        lagfx_cmd_header_t h; memset(&h, 0, sizeof h);
        uint8_t pl[28]; memcpy(pl, LEG, 28);
        h.opcode = 0x37; h.length = 40; h.stamp = 999; h.payload = pl; h.payload_size = 28;
        a->current_chan_id = 1;
        uint64_t c1 = a->total_cmds_completed;
        assert(lagfx_compute_exec_cmdbuf(a, &h) != LAGFX_HANDLER_OK);
        assert(g_sent_reads == 0 && a->total_cmds_completed == c1);
    }

    /* ---- arm64: root 0x20 ---- */
    {
        lagfx_protocol_t *r = mk(LAGFX_PROTO_ARM64_VMAPPLE);
        r->ring_armed = true; r->ring_size = 0x400; r->ring_start_offset = 0; r->ring_base_gpa = (uint64_t)RING_PFN0 << 14;
        uint8_t cmd[40] = {0}; uint16_t op = 0x20; uint32_t len = 40, st = 78;
        memcpy(cmd, &op, 2); memcpy(cmd + 4, &len, 4); memcpy(cmd + 8, &st, 4); memcpy(cmd + 12, LEG, 28);
        memcpy(g_mem + ((uint64_t)RING_PFN0 << 14), cmd, 40);
        r->read_ptr = 0; r->write_ptr = 40;
        uint64_t c0 = r->total_cmds_completed;
        assert(channel_0_dispatcher_drain(r) == 1);
        assert(g_sent_reads == 0);                                  /* (the ring itself is not at SENT) */
        assert(r->total_cmds_completed == c0 + 1 && r->last_completed_stamp == 78);
        assert(r->read_ptr == 40);                                  /* the command was consumed, the ring is not stuck */
#if HAVE_XG
        lagfx_xg_snapshot(&s);
        assert(s.rejected[LAGFX_XG_REJ_ROOT_20] == 1);
#endif
    }

    /* ---- arm64: the legacy translator API itself ---- */
    {
        lagfx_protocol_t *a = mk(LAGFX_PROTO_ARM64_VMAPPLE);
        /* a mapped 16 KiB task: root page pfn 8 = {table pfn 9, depth 1}; table[0] -> data pfn 10; VA 0x10 -> gpa 0x28010 */
        put32((uint64_t)8 << 14, 9); put32(((uint64_t)8 << 14) + 4, 1);
        put32((uint64_t)9 << 14, 10);
        memcpy(g_mem + ((uint64_t)10 << 14) + 0x10, "ARM64-16K-PAGE!!", 16);
        lagfx_task_entry_t *t = def_task(a, 3, 8);
#if HAVE_XG
        lagfx_xg_reset();
#endif
        uint64_t gpa = 0x1111;
        assert(!lagfx_task_translate(a, t, 0x10, &gpa));            /* legacy translate: refused in arm64 mode, even though the 16 KiB map is valid */
        uint8_t b[16] = {0};
        assert(lagfx_task_read_virtual(a, t, 0x10, 16, b) && memcmp(b, "ARM64-16K-PAGE!!", 16) == 0);   /* read_virtual is served by the arm64 reader */
        assert(!lagfx_task_read_virtual(a, t, SENT, 16, b));       /* unmapped VA: false ... */
        assert(g_sent_reads == 0);                                  /* ... and NO identity read of GPA == VA */
        g_sent_writes = 0;
        uint8_t body[16] = {0}; uint8_t res[16] = {0};
        uint64_t wa = SENT + 0x100u, rl = 0x100;
        memcpy(res, &wa, 8); memcpy(res + 8, &rl, 8);
        lagfx_info_dispatch(a, 0x1c2u, body, sizeof body, res, 1, t, lagfx_task_translate);
        assert(g_sent_writes == 0);                                 /* info reply to an untranslated address: no identity write */
#if HAVE_XG
        lagfx_xg_snapshot(&s);
        assert(s.legacy_xlate_in_arm64 >= 4);
        assert(s.blocked >= 3 && s.redirected == 2);
        bool saw_tr = false, saw_rv = false, saw_ir = false;
        for (uint32_t i = 0; i < s.nsites; i++) {
            if (!strcmp(s.site_name[i], "task_translate")) saw_tr = true;
            if (!strcmp(s.site_name[i], "task_read_virtual")) saw_rv = true;
            if (!strcmp(s.site_name[i], "info_reply_identity")) saw_ir = true;
        }
        assert(saw_tr && saw_rv && saw_ir);
        lagfx_xg_stats_line(a, true);                               /* prints the g7-gpu-xlate line; must not crash */
#endif
    }

    puts("xlate-guard-unit: ok");
    return 0;
}
