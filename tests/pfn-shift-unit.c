/*
 * libapplegfx-vulkan — PFN shift unit test
 * tests/pfn-shift-unit.c
 *
 * SPDX-License-Identifier: MIT
 *
 * The guest's page-frame numbers are turned into addresses with one shift. It is 12 (4 KiB pages) unless the device was created with
 * ring_page_shift=14 (16 KiB page numbers). This pins: the default and explicit 12 give the old addresses, 14 gives the 16 KiB addresses, a
 * PFN that cannot fit gives an address no memory callback accepts, and the ring PFN-array walk uses the same shift for the array page and
 * for the data page while the in-page offset stays 12 bits wide (only the PFN encoding changes).
 */

#include "dispatchers/ring_common.h"
#include "protocol/state.h"
#include "device.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint64_t g_last_read_gpa;
static uint32_t g_table_entries = 3;   /* non-zero entries in the PFN-array page */

static bool fake_read(void *opaque, uint64_t gpa, uint64_t len, void *dst) {
    (void)opaque;
    g_last_read_gpa = gpa;
    uint8_t *out = (uint8_t *)dst;
    memset(out, 0, len);
    for (uint64_t i = 0; i + 4 <= len; i += 4) {
        uint32_t idx = (uint32_t)(((gpa & 0x3fffu) + i) / 4u);   /* entry number inside the array page */
        uint32_t pfn = idx < g_table_entries ? 0x1234u + idx : 0u;
        memcpy(out + i, &pfn, 4);
    }
    return true;
}

int main(void) {
    lagfx_protocol_t p;
    memset(&p, 0, sizeof p);

    /* default (0) and explicit 12 */
    assert(lagfx_ring_page_shift(&p) == 12u);
    assert(lagfx_pfn_to_gpa(&p, 0x1e296u) == 0x1e296000ull);
    p.ring_page_shift = 12u;
    assert(lagfx_pfn_to_gpa(&p, 0x1e296u) == 0x1e296000ull);
    assert(lagfx_ring_page_shift(NULL) == 12u);
    assert(lagfx_pfn_to_gpa(NULL, 0x10u) == 0x10000ull);

    /* 16 KiB page numbers: the values seen in the first Android lagfx log */
    p.ring_page_shift = 14u;
    assert(lagfx_pfn_to_gpa(&p, 0x1e296u) == 0x78a58000ull);
    assert(lagfx_pfn_to_gpa(&p, 0x1e719u) == 0x79c64000ull);
    assert(lagfx_pfn_to_gpa(&p, 0x1ea47u) == 0x7a91c000ull);
    assert(lagfx_pfn_to_gpa(&p, 0x1ff13u) == 0x7fc4c000ull);

    /* a PFN that cannot fit in 64 bits after the shift */
    assert(lagfx_pfn_to_gpa(&p, 0xffffffffffffffffull) == ~(uint64_t)0);
    p.ring_page_shift = 12u;
    assert(lagfx_pfn_to_gpa(&p, 0x0010000000000000ull) == ~(uint64_t)0);
    assert(lagfx_pfn_to_gpa(&p, 0x000fffffffffffffull) == 0xfffffffffffff000ull);

    /* ring PFN-array walk with 16 KiB ring pages: array page and data page use the shift, the page index is offset >> 14 and the in-page offset
       is 14 bits wide */
    lagfx_device_t dev;
    memset(&dev, 0, sizeof dev);
    dev.desc.shell.read_memory = fake_read;
    p.dev = &dev;
    p.ring_page_shift = 14u;
    assert(lagfx_ring_page_size(&p) == 0x4000u);
    uint64_t page0 = lagfx_pfn_to_gpa(&p, 0x100u);          /* 0x400000 */
    uint64_t data = 0;
    assert(lagfx_ring_resolve_data_gpa(&p, page0, 0x8000u, 0x4010u, &data));
    assert(g_last_read_gpa == page0 + 4u);                   /* ring page 1 -> second array entry */
    assert(data == (0x1235ull << 14) + 0x10u);
    assert(lagfx_ring_resolve_data_gpa(&p, page0, 0xc000u, 0x3ff0u, &data));   /* still ring page 0 (0x1000 is not a page boundary any more) */
    assert(g_last_read_gpa == page0);
    assert(data == (0x1234ull << 14) + 0x3ff0u);

    /* ring size from the PFN array: pages * page size at 14, the fixed size at 12 */
    assert(lagfx_ring_size_from_table(&p, page0, 0x10000u) == (3u << 14));
    g_table_entries = 0;
    assert(lagfx_ring_size_from_table(&p, page0, 0x10000u) == 0x10000u);   /* empty array: fixed */
    g_table_entries = 3;
    p.ring_page_shift = 12u;
    assert(lagfx_ring_size_from_table(&p, page0, 0x10000u) == 0x10000u);

    /* 4 KiB ring pages (the default 0 and an explicit 12): the old walk */
    p.ring_page_shift = 0u;
    assert(lagfx_ring_resolve_data_gpa(&p, 0x100000ull, 0x2000u, 0x1010u, &data));
    assert(data == (0x1235ull << 12) + 0x10u);
    p.ring_page_shift = 12u;
    assert(lagfx_ring_resolve_data_gpa(&p, 0x100000ull, 0x2000u, 0x1010u, &data));
    assert(data == (0x1235ull << 12) + 0x10u);

    printf("pfn-shift-unit: OK\n");
    return 0;
}
