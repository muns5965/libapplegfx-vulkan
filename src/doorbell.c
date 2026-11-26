/*
 * libapplegfx-vulkan — Doorbell routing (BAR0 write/read entry point)
 * src/doorbell.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Single entry point for ALL inbound MMIO above the MSI-X range. See
 * the long comment in doorbell.h for the architectural rationale.
 *
 * Layout (BAR0):
 *   0x0000..0x0FFF   MSI-X vector table — shell owns; never reaches us
 *   0x1000..0x1FFF   registers / doorbells — owned here
 *
 * Within 0x1000..0x1FFF, every offset is either a state register
 * (write updates protocol state + register shadow) or a doorbell
 * (write triggers a dispatcher via g_doorbell_doors[]). Unknown
 * offsets are logged at WARN level so gaps are visible.
 */

#include "doorbell.h"
#include "dispatchers/channel_0_dispatcher.h"
#include "dispatchers/channel_door_dispatcher.h"
#include "dispatchers/primary_ring_door_dispatcher.h"
#include "protocol/state.h"
#include "protocol/arm64_completion.h"
#include "common/log.h"

#include "device.h"
#include "common/le.h"

#include <string.h>

/* === Door Registry ================================================
 * Maps real BAR offsets to the dispatcher function for that doorbell.
 * Only the two actual doorbells live here; everything else is plain
 * register state handled inline in doorbell_handle_write.
 */
const doorbell_door_descriptor_t g_doorbell_doors[] = {
    /* Root channel write pointer (BAR0+0x1008) - primary ring FIFO doorbell */
    { .bar_offset = 0x1008u, .id = DOOR_PRIMARY_RING, .dispatch_fn = (void(*)(void*, uint32_t))primary_ring_door_dispatcher_dispatch },

    /* Channel ID selector (BAR0+0x1020) - routes to ch 0, compute (1-4), display (5+) */
    { .bar_offset = 0x1020u, .id = DOOR_CHANNEL, .dispatch_fn = (void(*)(void*, uint32_t))channel_door_dispatcher_dispatch },
};

const size_t g_doorbell_door_count = sizeof(g_doorbell_doors) / sizeof(doorbell_door_descriptor_t);

/* === Register shadow ==============================================
 * The shadow covers BAR0+0x1000..0x1FFF mapped to idx 0..15 via
 * doorbell_reg_index. Reads of state registers (anything that isn't
 * a special-case offset like 0x1014 / 0x1018 / 0x122c) return the
 * shadow; writes update the shadow as a side effect of
 * doorbell_handle_write so the subsequent read sees what the guest
 * just wrote.
 *
 * State lives on \`lagfx_protocol_t.reg[16]\` (declared in state.h).
 * A previous version of this module owned a file-static array
 * \`g_reg_shadow\`; that has been removed in favour of per-device
 * state so multiple devices (test fixtures, future multi-device
 * builds) don't share a single shadow.
 */
#define LAGFX_MSIX_RANGE_END 0x1000u


/* Read-only comparison, logged once at the first primary-ring doorbell: what is at the ring start (and at the shared page) if the PFN is read as a
   4 KiB page number and as a 16 KiB page number. Only the memory callback's read is used; no ring pointer moves and no command is parsed. The
   address the device actually uses is the one marked "active". */
static void pfn_candidates_diag(lagfx_protocol_t *p) {
    if (!p || p->pfn_diag_done) return;
    p->pfn_diag_done = 1;
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    if (!dev || !dev->desc.shell.read_memory) return;
    static const uint32_t shifts[2] = {12u, 14u};
    for (int i = 0; i < 2; i++) {
        uint32_t sh = shifts[i];
        const char *mark = (sh == lagfx_ring_page_shift(p)) ? " active" : "";
        for (int w = 0; w < 2; w++) {
            uint64_t pfn = w == 0 ? p->ring_base_pfn : p->ring_shared_page_pfn;
            uint64_t base = (sh >= 64u || (pfn >> (64u - sh)) != 0u) ? ~(uint64_t)0 : (pfn << sh);
            uint64_t gpa = base + (w == 0 ? p->ring_start_offset : 0u);
            uint8_t b[32];
            memset(b, 0, sizeof b);
            bool ok = base != ~(uint64_t)0 && dev->desc.shell.read_memory(dev->desc.shell.opaque, gpa, sizeof b, b);
            LAGFX_LOG("pfn-diag: %s pfn=0x%llx shift=%u gpa=0x%llx read=%s%s first32=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x | header opcode=0x%04x length=0x%x",
                      w == 0 ? "ring-start" : "shared-page", (unsigned long long)pfn, sh, (unsigned long long)gpa, ok ? "ok" : "failed", mark,
                      b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15],
                      b[16],b[17],b[18],b[19],b[20],b[21],b[22],b[23],b[24],b[25],b[26],b[27],b[28],b[29],b[30],b[31],
                      (unsigned)lagfx_le16(b), (unsigned)lagfx_le32(b + 4));
        }
    }
}

static inline int doorbell_reg_index(uint64_t offset) {
    if (offset < LAGFX_MSIX_RANGE_END || offset >= 0x2000) return -1;
    int idx = ((int)(offset & 0xFFFu)) >> 2;
    return (idx >= 0 && idx < 16) ? idx : -1;
}

/* === Registry Lookup ============================================== */

static const doorbell_door_descriptor_t* doorbell_lookup_by_offset_internal(uint64_t offset) {
    for (size_t i = 0; i < g_doorbell_door_count; i++) {
        if (g_doorbell_doors[i].bar_offset == offset) {
            return &g_doorbell_doors[i];
        }
    }
    return NULL;
}

/* Public lookup function */
const doorbell_door_descriptor_t* doorbell_lookup_by_offset(uint64_t offset) {
    return doorbell_lookup_by_offset_internal(offset);
}

/* === Dispatch Entry Point =========================================
 *
 * Called from doorbell_handle_write for the two actual doorbell
 * offsets (0x1008, 0x1020), and directly from test fixtures that want
 * to drive a dispatcher without going through MMIO. Anything that
 * isn't a registered doorbell offset is a programmer error and we
 * log/return.
 */
void doorbell_dispatch(void *protocol_state, uint64_t bar_offset, uint32_t data) {
    const doorbell_door_descriptor_t* door = doorbell_lookup_by_offset_internal(bar_offset);

    if (!door || !door->dispatch_fn) {
        LAGFX_WARN("doorbell_dispatch: no handler for BAR0+0x%llx, data=0x%x",
                   (unsigned long long)bar_offset, data);
        return;
    }

    /* Call the dispatcher's routing function with protocol state */
    door->dispatch_fn(protocol_state, data);
}

/* === One-shot init ================================================
 * Owns any startup register values. Called from lagfx_device_new
 * right after lagfx_protocol_new. STATUS_CONTROL=1 is the "FIFO
 * armed/enabled" default that older test fixtures (lifecycle-smoke
 * Phase 1.A) rely on.
 *
 * The protocol struct's reg[] array is already zeroed by calloc in
 * lagfx_protocol_new; we just stamp STATUS_CONTROL here. (Note:
 * lagfx_protocol_new also seeds REG_STATUS_CONTROL with 0x3 — that
 * encodes both "present" and "ready" bits. doorbell_init resetting
 * it to 1 is the legacy lifecycle-smoke shape; honour it.)
 */
void doorbell_init(void *protocol_state) {
    lagfx_protocol_t *p = (lagfx_protocol_t *)protocol_state;
    if (!p) return;
    memset(p->reg, 0, sizeof(p->reg));
    p->reg[0] = 1u;  /* STATUS_CONTROL @ 0x1000: decoder live */
    LAGFX_LOG("doorbell_init: shadow reset, STATUS_CONTROL=1");
}

/* === Unified write entry point ====================================
 *
 * Every BAR0 write above the MSI-X region routes here. We:
 *   1. Update the register shadow so reads of state registers see
 *      the value the guest just wrote.
 *   2. Apply side effects on protocol state for known registers.
 *   3. Forward the two actual doorbells (0x1008, 0x1020) to
 *      doorbell_dispatch → registry → dispatcher.
 *   4. Log unknown offsets at WARN so gaps are visible in
 *      /tmp/lagfx.log rather than silently dropped.
 */
void doorbell_handle_write(void *protocol_state, uint64_t offset, uint32_t value) {
    lagfx_protocol_t *p = (lagfx_protocol_t *)protocol_state;

    /* Shadow first so a subsequent read sees what the guest wrote.
     * The shadow update is intentionally unconditional on idx — even
     * if the offset has no state-register meaning, mirroring it lets
     * us spot odd writes by reading them back. */
    int idx = doorbell_reg_index(offset);
    if (idx >= 0 && p) {
        p->reg[idx] = value;
    }

    switch (offset) {
        case 0x1000u: /* STATUS_CONTROL */
            if (p) p->ring_armed = (value != 0u);
            LAGFX_LOG("doorbell: write off=0x1000 STATUS_CONTROL val=0x%08x ring_armed=%d",
                      value, p ? (int)p->ring_armed : -1);
            return;

        case 0x1004u: /* ring_size */
            if (p) p->ring_size = value ? value : 0x10000u;
            LAGFX_LOG("doorbell: write off=0x1004 ring_size val=0x%08x → 0x%x",
                      value, p ? p->ring_size : 0u);
            return;

        case 0x1008u: /* primary ring doorbell */
            pfn_candidates_diag(p);
            LAGFX_LOG("doorbell: write off=0x1008 PRIMARY_RING wp=0x%x → dispatch", value);
            doorbell_dispatch(protocol_state, 0x1008u, value);
            return;

        case 0x1010u: /* ring_start_offset */
            if (p) {
                p->ring_start_offset = value;
                p->page_size = 0x1000u;
                p->ring_base_gpa = lagfx_pfn_to_gpa(p, p->ring_base_pfn) + p->ring_start_offset;
            }
            LAGFX_LOG("doorbell: write off=0x1010 ring_start_offset val=0x%08x ring_base_gpa=0x%llx",
                      value, p ? (unsigned long long)p->ring_base_gpa : 0ull);
            return;

        case 0x101cu: /* ring_shared_page_pfn */
            if (p) p->ring_shared_page_pfn = value;
            LAGFX_LOG("doorbell: write off=0x101c ring_shared_page_pfn val=0x%08x", value);
            return;

        case 0x1020u: /* channel doorbell */
            if (p) p->current_chan_id = (uint8_t)value;
            LAGFX_LOG("doorbell: write off=0x1020 CHANNEL chan_id=%u → dispatch", value);
            doorbell_dispatch(protocol_state, 0x1020u, value);
            return;

        case 0x1024u: /* RE: PROTOCOL.md §2.0 — per-device 0-arg ObjC trampoline (value discarded). */
            LAGFX_TRACE("doorbell: write off=0x1024 trampoline_A (val=0x%x discarded)", value);
            return;

        case 0x1028u: /* RE: PROTOCOL.md §2.0 — per-device 0-arg ObjC trampoline (value discarded). */
            LAGFX_TRACE("doorbell: write off=0x1028 trampoline_B (val=0x%x discarded)", value);
            return;

        case 0x1034u: /* RE: PROTOCOL.md §2.0 — _binaryVersion (spec says R, kext sometimes writes; we just log+shadow). */
            LAGFX_LOG("doorbell: write off=0x1034 _binaryVersion val=0x%08x (spec-divergent — kext write)", value);
            return;

        case 0x1030u: /* ring_base_pfn */
            if (p) {
                p->ring_base_pfn = value;
                p->ring_base_gpa = lagfx_pfn_to_gpa(p, value) + p->ring_start_offset;
                if (p->ring_size == 0u) {
                    p->ring_size = 0x10000u;
                }
            }
            LAGFX_LOG("doorbell: write off=0x1030 ring_base_pfn val=0x%08x ring_base_gpa=0x%llx ring_size=0x%x",
                      value,
                      p ? (unsigned long long)p->ring_base_gpa : 0ull,
                      p ? p->ring_size : 0u);
            return;

        /* === Secondary capability bank (PROTOCOL.md §2.1) =====
         * The kext writes to these as part of EFI mode publish /
         * capability negotiation. Shadow store (already done above)
         * is the entire side effect — read-back returns the shadow.
         * Documented offsets get explicit cases per
         * reference_lagfx_mmio_handler.md rule 1; do not let them
         * fall to default-warn even when the body is just a log
         * line.
         */
        case 0x1200u: /* RE: PROTOCOL.md §2.1 — _efi boot-state */
            LAGFX_LOG("sec_cap: 0x1200 efi_boot_state val=0x%08x", value);
            return;

        case 0x1210u: /* RE: PROTOCOL.md §2.1 — queue-control capability */
            LAGFX_LOG("sec_cap: 0x1210 queue_control val=0x%08x", value);
            return;

        case 0x1214u: /* RE: PROTOCOL.md §2.1 — capability bits */
            LAGFX_LOG("sec_cap: 0x1214 capability_bits val=0x%08x", value);
            return;

        case 0x1218u: /* RE: PROTOCOL.md §2.1 / 2.2 — setEFIFramebufferMode mode_class */
            LAGFX_LOG("sec_cap: 0x1218 setEFIFramebufferMode mode_class=0x%08x", value);
            return;

        case 0x121cu: { /* RE: PROTOCOL.md §2.1 / 2.2 — setEFIModeSelect (H<<16)|W */
            uint32_t height = value >> 16;
            uint32_t width  = value & 0xFFFFu;
            LAGFX_LOG("sec_cap: 0x121c setEFIModeSelect %ux%u (raw=0x%08x)",
                      width, height, value);
            return;
        }

        case 0x1228u: /* RE: PROTOCOL.md §2.1 — capability bits (u32 at +0x5a0) */
            LAGFX_LOG("sec_cap: 0x1228 capability_bits_2 val=0x%08x", value);
            return;

        default:
            LAGFX_WARN("doorbell: unhandled MMIO write offset=0x%llx value=0x%08x",
                       (unsigned long long)offset, value);
            return;
    }
}

/* === Unified read entry point =====================================
 *
 * Mirror of doorbell_handle_write. Special-case offsets (0x1014,
 * 0x1018, 0x122c) implement their bespoke semantics; everything else
 * returns the register shadow. Unknown offsets are TRACE (noisy on
 * the read side — macOS polls a lot).
 */
uint32_t doorbell_handle_read(void *protocol_state, uint64_t offset) {
    lagfx_protocol_t *p = (lagfx_protocol_t *)protocol_state;

    /* Capability gate — modern paravirt path indicator */
    if (offset == 0x122cu) {
        /* arm64 vmapple: Apple Henge pvg_lite.c answers "number of displays" = 1. */
        uint32_t v = lagfx_proto_arm64(p) ? 1u : 9u;
        LAGFX_TRACE("doorbell: read off=0x122c cap_gate → %u", v);
        return v;
    }

    /* Display bitmask — atomic xchg-and-clear with acquire ordering.
     * See state.h's pending_displays_bitmask doc-block for the
     * cross-thread contract. */
    if (offset == 0x1014u) {
        uint32_t mask = p
            ? atomic_exchange_explicit(&p->pending_displays_bitmask,
                                       0u, memory_order_acquire)
            : 0u;
        if (p) {
            atomic_fetch_add_explicit(&p->db_reads, 1u, memory_order_relaxed);
            if (mask) {
                atomic_fetch_add_explicit(&p->db_nonzero, 1u, memory_order_relaxed);
                for (int b = 0; b < 4; b++)
                    if (mask & (1u << b)) atomic_fetch_add_explicit(&p->db_bit[b], 1u, memory_order_relaxed);
            }
        }
        LAGFX_TRACE("doorbell: read off=0x1014 display_bitmask → 0x%x (cleared)", mask);
        return mask;
    }

    /* Stamp bitmask — atomic xchg-and-clear with acquire ordering. */
    if (offset == 0x1018u) {
        if (p && p->a64cq) lagfx_arm64_poll_completions(p);   /* arm64 deferred completion: retire finished GPU work first (no-op when off) */
        uint32_t mask = p
            ? atomic_exchange_explicit(&p->pending_stamps_bitmask,
                                       0u, memory_order_acquire)
            : 0u;
        LAGFX_TRACE("doorbell: read off=0x1018 stamp_bitmask → 0x%x (cleared)", mask);
        return mask;
    }

    /* arm64 vmapple (Apple Henge gfx_read): reading the root read pointer first runs the root ring (the driver spins on this waiting for
     * space), then returns the cumulative pointer. */
    if (offset == 0x100cu && p && lagfx_proto_arm64(p)) {
        if (p->a64cq) lagfx_arm64_poll_completions(p);
        channel_0_dispatcher_drain(p);
        LAGFX_TRACE("doorbell: read off=0x100c root_rp → 0x%x", p->read_ptr);
        return p->read_ptr;
    }

    int idx = doorbell_reg_index(offset);
    if (idx < 0 || !p) {
        LAGFX_TRACE("doorbell: read off=0x%llx unmapped → 0",
                    (unsigned long long)offset);
        return 0u;
    }

    uint32_t value = p->reg[idx];
    LAGFX_TRACE("doorbell: read off=0x%llx idx=%d → 0x%08x",
                (unsigned long long)offset, idx, value);
    return value;
}
