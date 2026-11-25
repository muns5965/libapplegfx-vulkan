/* compute 0x38 / 0x44 task registration (args follow the barrier slots), per devices/pvg_lite.c. SPDX-License-Identifier: MIT */
#include "dispatchers/channel_0_dispatcher.h"
#include "protocol/state.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    lagfx_protocol_t *p = calloc(1, sizeof *p);
    p->magic = LAGFX_PROTOCOL_MAGIC;
    uint32_t w[8] = {0};
    lagfx_cmd_header_t h; memset(&h, 0, sizeof h);
    h.opcode = 0x38; h.stamp = 1;

    /* 0x44 before the task exists: skipped */
    uint32_t l[8] = {7, 0x11111111, 0x22, 0x33333333, 0x44, 0x1000, 0x2000, 0};
    h.opcode = 0x44; h.payload = (const uint8_t *)l; h.payload_size = 28;
    assert(!lagfx_define_task_lists(p, &h));

    /* 0x38 no barriers: slot 14 -> task 7, pfn 0x1f6f2 */
    w[0] = 14; w[3] = 0x1f6f2;
    h.opcode = 0x38; h.arg_count_8b = 0; h.payload = (const uint8_t *)w; h.payload_size = 16;
    assert(lagfx_define_host_task(p, &h));
    lagfx_task_entry_t *t = lagfx_protocol_find_task(p, 7);
    assert(t && t->root_page_pfn == 0x1f6f2);

    /* too short is refused */
    h.payload_size = 12; assert(!lagfx_define_host_task(p, &h));

    /* with 1 barrier slot (8 bytes) the args move by 8 */
    uint32_t b[8] = {0xdead, 0xbeef, 18, 0, 0, 0xabc, 0, 0};
    h.arg_count_8b = 1; h.payload = (const uint8_t *)b; h.payload_size = 24;
    assert(lagfx_define_host_task(p, &h));
    t = lagfx_protocol_find_task(p, 9);
    assert(t && t->root_page_pfn == 0xabc);
    h.payload_size = 16; assert(!lagfx_define_host_task(p, &h));   /* skip makes it too short */

    /* 0x44 now registers */
    h.opcode = 0x44; h.arg_count_8b = 0; h.payload = (const uint8_t *)l; h.payload_size = 28;
    assert(lagfx_define_task_lists(p, &h));
    t = lagfx_protocol_find_task(p, 7);
    assert(t->lists_set && t->objects_addr == 0x2211111111ull && t->placement_addr == 0x4433333333ull);
    assert(t->objects_len == 0x1000 && t->placement_len == 0x2000);
    h.payload_size = 24; assert(!lagfx_define_task_lists(p, &h));

    free(p);
    puts("task-define-unit: ok");
    return 0;
}
