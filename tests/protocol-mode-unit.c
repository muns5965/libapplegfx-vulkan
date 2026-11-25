/* x86 vs arm64-vmapple compute command semantics, driven through a virtual ring (16 KiB PFNs) and a fake guest memory.
 * SPDX-License-Identifier: MIT */
#include "dispatchers/channel_compute_dispatcher.h"
#include "dispatchers/ring_common.h"
#include "protocol/state.h"
#include "device.h"
#include "doorbell.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_SIZE   (1u << 20)
#define SHARED_PFN 1u      /* gpa 0x4000 */
#define TABLE_PFN  2u      /* ring page table */
#define RING_PFN0  4u      /* ring pages 4,5,6,7 (contiguous here) */

static uint8_t *g_mem;
static bool rd(void *o, uint64_t gpa, uint64_t len, void *dst) { (void)o; if (gpa + len > MEM_SIZE) return false; memcpy(dst, g_mem + gpa, len); return true; }
static bool wr(void *o, uint64_t gpa, uint64_t len, const void *src) { (void)o; if (gpa + len > MEM_SIZE) return false; memcpy(g_mem + gpa, src, len); return true; }
static void irq(void *o, uint32_t v) { (void)o; (void)v; }

static void put32(uint64_t gpa, uint32_t v) { memcpy(g_mem + gpa, &v, 4); }

static uint32_t g_wp;   /* absolute ring write pointer */

static void submit(lagfx_protocol_t *p, uint16_t op, uint16_t arg8, const uint32_t *words, uint32_t nwords) {
    uint32_t len = 12u + nwords * 4u;
    uint64_t ring = (uint64_t)RING_PFN0 << 14;
    uint8_t cmd[256]; memset(cmd, 0, sizeof cmd);
    memcpy(cmd, &op, 2); memcpy(cmd + 2, &arg8, 2); memcpy(cmd + 4, &len, 4);
    uint32_t stamp = g_wp + 1u; memcpy(cmd + 8, &stamp, 4);
    if (nwords) memcpy(cmd + 12, words, nwords * 4u);
    memcpy(g_mem + ring + (g_wp % 0x10000u), cmd, len);
    g_wp += len;
    put32(((uint64_t)SHARED_PFN << 14) + LAGFX_RING_FIFO_DESC_BASE_OFFSET + 0x00, g_wp);
    channel_compute_dispatcher_drain(p, 1);
}

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
    return p;
}

static lagfx_task_entry_t *def_task(lagfx_protocol_t *p, uint32_t task) {
    uint32_t w[4] = { task * 2u, 0, 0, 0x1234 };
    submit(p, 0x38, 0, w, 4);
    lagfx_task_entry_t *t = lagfx_protocol_find_task(p, task);
    assert(t && t->live);
    return t;
}

int main(void) {
    g_mem = calloc(1, MEM_SIZE);

    /* 0x122c */
    lagfx_protocol_t *x = mk(LAGFX_PROTO_X86), *a = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    assert(doorbell_handle_read(x, 0x122c) == 9u);
    assert(doorbell_handle_read(a, 0x122c) == 1u);

    /* ARM64 0x25 {task, ref} removes ref from that task only (even with another task live first) */
    a = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    lagfx_task_entry_t *t2 = def_task(a, 2), *t3 = def_task(a, 3);
    t2->active_objects.object_ids[0] = 0x55; t2->active_objects.count = 1;
    t3->active_objects.object_ids[0] = 0x55; t3->active_objects.object_ids[1] = 0x66; t3->active_objects.count = 2;
    uint32_t r[2] = { 3, 0x55 };
    submit(a, 0x25, 0, r, 2);
    assert(t3->active_objects.count == 1 && t3->active_objects.object_ids[0] == 0x66);
    assert(t2->active_objects.count == 1);                 /* the first live task is NOT touched */
    /* with one barrier slot the args move by 8 bytes */
    uint32_t rb[4] = { 0xdead, 0xbeef, 3, 0x66 };
    submit(a, 0x25, 1, rb, 4);
    assert(t3->active_objects.count == 0);

    /* ARM64 0x20: slot>>1 names the task, cache dropped, never an exec; a short payload is ignored */
    t2->active_objects.count = 1; t3->active_objects.count = 2;
    uint32_t s = 4;                                         /* task 2 */
    submit(a, 0x20, 0, &s, 1);
    assert(t2->active_objects.count == 0 && t3->active_objects.count == 2);
    submit(a, 0x20, 0, NULL, 0);                            /* too short: ignored, no crash, no exec */
    assert(t3->active_objects.count == 2);

    /* x86 0x25 {count, objectId} registers into the first live task (old behaviour kept) */
    x = mk(LAGFX_PROTO_X86);
    lagfx_task_entry_t *x2 = def_task(x, 2);
    uint32_t xr[2] = { 1, 0x77 };
    submit(x, 0x25, 0, xr, 2);
    assert(x2->active_objects.count == 1 && x2->active_objects.object_ids[0] == 0x77);

    /* Task ids and slots are independent: id 65 then id 0 (slot 0 holds id 65); find by id gives the right one, and
     * inner ops (which now use find_task) must not see id 65 when asked for id 0. */
    x = mk(LAGFX_PROTO_X86);
    lagfx_task_entry_t *x65 = def_task(x, 65), *x0 = def_task(x, 0);
    assert(x65 != x0 && x65->id == 65 && x0->id == 0);
    assert(lagfx_protocol_find_task(x, 65) == x65 && lagfx_protocol_find_task(x, 0) == x0);
    assert(lagfx_protocol_find_task(x, 7) == NULL);
    x65->active_objects.object_ids[0] = 0x99; x65->active_objects.count = 1;
    /* arm64 0x25/0x20 on id 0 must not touch id 65 */
    lagfx_protocol_t *y = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    lagfx_task_entry_t *y65 = def_task(y, 65), *y0 = def_task(y, 0);
    y65->active_objects.object_ids[0] = 0x99; y65->active_objects.count = 1;
    y0->active_objects.object_ids[0] = 0x99; y0->active_objects.count = 1;
    uint32_t r0[2] = { 0, 0x99 };
    submit(y, 0x25, 0, r0, 2);
    assert(y0->active_objects.count == 0 && y65->active_objects.count == 1);
    /* redefine the same id: same entry, state kept */
    uint32_t w5[4] = { 130, 0, 0, 0x4321 };
    submit(y, 0x38, 0, w5, 4);
    assert(lagfx_protocol_find_task(y, 65) == y65 && y65->root_page_pfn == 0x4321 && y65->active_objects.count == 1);


    /* Root ring, arm64: cumulative u32 pointers, wrapping the 0x100-byte ring several times; 0x100c returns the cumulative rp. */
    {
        lagfx_protocol_t *r = mk(LAGFX_PROTO_ARM64_VMAPPLE);
        r->ring_armed = true; r->ring_size = 0x100; r->ring_start_offset = 0;
        r->ring_base_gpa = (uint64_t)RING_PFN0 << 14;
        uint32_t wp = 0;
        for (uint32_t n = 0; n < 40; n++) {          /* 40 x 28 bytes = 0x460 > 4 rings */
            uint8_t cmd[28] = {0};
            uint16_t op = 0x38; uint32_t len = 28, stamp = n + 1, slot = (n % 8) * 2, pfn = 0x100 + n;
            memcpy(cmd, &op, 2); memcpy(cmd + 4, &len, 4); memcpy(cmd + 8, &stamp, 4);
            memcpy(cmd + 12, &slot, 4); memcpy(cmd + 24, &pfn, 4);
            for (uint32_t i = 0; i < len; i++) g_mem[((uint64_t)RING_PFN0 << 14) + ((wp + i) % 0x100)] = cmd[i];
            wp += len;
            r->write_ptr = wp;
            uint32_t got = doorbell_handle_read(r, 0x100c);   /* runs the ring */
            assert(got == wp && r->read_ptr == wp);
        }
        assert(wp == 40u * 28u);
        for (uint32_t t = 0; t < 8; t++) {
            lagfx_task_entry_t *e = lagfx_protocol_find_task(r, t);
            assert(e && e->root_page_pfn == 0x100u + 32u + t);   /* the last command naming task t */
        }
        assert(doorbell_handle_read(r, 0x100c) == wp);        /* repeated reads: nothing re-runs */
        /* absurd pending (> ring): dropped and resynced, not replayed */
        r->write_ptr = r->read_ptr + 0x1000;
        assert(doorbell_handle_read(r, 0x100c) == r->write_ptr);
        /* x86: 0x100c is the plain shadow register, no ring run */
        lagfx_protocol_t *rx = mk(LAGFX_PROTO_X86);
        rx->ring_armed = true; rx->ring_size = 0x100; rx->write_ptr = 28;
        assert(doorbell_handle_read(rx, 0x100c) == 0u && rx->read_ptr == 0u);
    }

    puts("protocol-mode-unit: ok");
    return 0;
}
