/* Diagnostics only: command sequence/counters, last-0x44 bookkeeping, raw logging bounds, and that none of it changes task state. SPDX-License-Identifier: MIT */
#include "dispatchers/channel_0_dispatcher.h"
#include "protocol/diag.h"
#include "protocol/state.h"
#include "device.h"
#include "doorbell.h"
#include "protocol/arm64_completion.h"
#include "libapplegfx-vulkan.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

static long file_lines(const char *path, const char *needle) {
    FILE *f = fopen(path, "r"); if (!f) return -1;
    char line[2048]; long n = 0;
    while (fgets(line, sizeof line, f)) if (strstr(line, needle)) n++;
    fclose(f); return n;
}

#include <time.h>
static uint64_t now_ns_for_test(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }

static void *tick_hammer(void *arg) {
    lagfx_protocol_t *p = arg;
    for (int i = 0; i < 20000; i++) lagfx_diag_tick(p);
    return NULL;
}

int main(void) {
    char path[] = "/tmp/lagfx-diag-unit-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    setenv("LAGFX_LOG_FILE", path, 1); setenv("LAGFX_LOG_LEVEL", "info", 1);

    lagfx_protocol_t *p = calloc(1, sizeof *p);
    p->magic = LAGFX_PROTOCOL_MAGIC;

    /* a 0x25 on compute channel 2 with 1 barrier slot: header 12 bytes + 8 barrier + 8 payload = 28 */
    uint8_t cmd[28] = {0x25, 0, 1, 0, 28, 0, 0, 0, 0x00, 0x08, 0, 0};
    lagfx_diag_note_0x44(p, 2, 5);
    assert(p->diag2.last44_valid[2] && p->diag2.last44_task[2] == 5 && p->diag2.last44_global_task == 5);

    for (int i = 0; i < 200; i++) lagfx_diag_cmd(p, LAGFX_DIAG_COMPUTE, 2, 0x100u + (unsigned)i, cmd, sizeof cmd);
    assert(p->diag2.seq == 200 && p->diag2.src_cmds[LAGFX_DIAG_COMPUTE] == 200 && p->diag2.op_count[LAGFX_DIAG_COMPUTE][0x25] == 200);
    /* bounded: first 64, then every 64th (128, 192) = 66 lines; counters never stop */
    assert(file_lines(path, "diag cmd #") == 66);
    assert(file_lines(path, "arg8=1 (barrier bytes 8)") == 66);

    /* root commands: only the first 16 are logged */
    uint8_t root[16] = {0x3a, 0, 0, 0, 16, 0, 0, 0, 0, 0xf, 0, 0};
    for (int i = 0; i < 40; i++) lagfx_diag_cmd(p, LAGFX_DIAG_ROOT, 0, 0, root, sizeof root);
    assert(p->diag2.src_cmds[LAGFX_DIAG_ROOT] == 40 && file_lines(path, "src=root") == 16);

    /* too short or null input is ignored and counts nothing */
    uint64_t before = p->diag2.seq;
    lagfx_diag_cmd(p, LAGFX_DIAG_COMPUTE, 2, 0, cmd, 8); lagfx_diag_cmd(p, 7, 2, 0, cmd, 28); lagfx_diag_cmd(NULL, 0, 0, 0, cmd, 28); lagfx_diag_cmd(p, 0, 0, 0, NULL, 28);
    assert(p->diag2.seq == before);

    /* MMIO counters; the statistics line is only due after 10 s, so none yet */
    for (int i = 0; i < 1000; i++) lagfx_diag_mmio(p, i & 1);
    assert(p->diag2.mmio_reads == 500 && p->diag2.mmio_writes == 500 && file_lines(path, "g7-gpu:") == 0);
    p->diag2.last_stats_ns -= 11ull * 1000000000ull;   /* pretend 11 s passed */
    for (int i = 0; i < 256; i++) lagfx_diag_mmio(p, false);
    assert(file_lines(path, "g7-gpu: t=+") == 1 && file_lines(path, "mmio_reads=") == 1 && file_lines(path, "log_write_errors=0") == 1);

    /* the diagnostics do not touch task state: 0x38 then redefine logs the old root and keeps the lists */
    uint32_t w[4] = {4, 0, 0, 0x1000};
    lagfx_cmd_header_t h; memset(&h, 0, sizeof h);
    h.opcode = 0x38; h.payload = (const uint8_t *)w; h.payload_size = 16;
    assert(lagfx_define_host_task(p, &h));
    uint32_t l[7] = {2, 0x11, 0, 0x22, 0, 0x100, 0x200};
    h.opcode = 0x44; h.payload = (const uint8_t *)l; h.payload_size = 28;
    assert(lagfx_define_task_lists(p, &h));
    w[3] = 0x2000; h.opcode = 0x38; h.payload = (const uint8_t *)w; h.payload_size = 16;
    assert(lagfx_define_host_task(p, &h));
    lagfx_task_entry_t *t = lagfx_protocol_find_task(p, 2);
    assert(t && t->root_page_pfn == 0x2000 && t->lists_set && t->objects_addr == 0x11);
    assert(file_lines(path, "REDEFINED old_root_pfn=0x1000 new_root_pfn=0x2000") == 1);

    /* idle guest: no MMIO at all, only the timer tick -> the 10 s statistics line still appears; the final one is printed at teardown */
    {
        long nb = file_lines(path, "g7-gpu: t=");
        lagfx_diag_tick(p);   /* not due yet */
        assert(file_lines(path, "g7-gpu: t=") == nb);
        p->diag2.last_stats_ns -= 11ull * 1000000000ull;
        lagfx_diag_tick(p);
        assert(file_lines(path, "g7-gpu: t=") == nb + 1);
        lagfx_diag_tick(p);   /* once per interval, however many callers */
        assert(file_lines(path, "g7-gpu: t=") == nb + 1);
        /* concurrent callers (two vCPU threads + the shell timer + more): one line per interval, and a stamp stored by another thread never makes t - last underflow */
        p->diag2.last_stats_ns -= 11ull * 1000000000ull;
        pthread_t th[8];
        for (int i = 0; i < 8; i++) assert(pthread_create(&th[i], NULL, tick_hammer, p) == 0);
        for (int i = 0; i < 8; i++) pthread_join(th[i], NULL);
        assert(file_lines(path, "g7-gpu: t=") == nb + 2);
        p->diag2.last_stats_ns = p->diag2.last_stats_ns + 5ull * 1000000000ull;   /* a stamp NEWER than now: no print, no underflow */
        lagfx_diag_tick(p);
        assert(file_lines(path, "g7-gpu: t=") == nb + 2);
        lagfx_diag_final(p);
        assert(file_lines(path, "g7-gpu: t=") == nb + 3 && file_lines(path, "g7-gpu-final:") == 1);
    }

    {   /* display doorbell counters: reads of 0x1014 and signals, library side; they must reach the g7-gpu-display line without any display op */
        lagfx_device_t *dev = calloc(1, sizeof *dev);
        dev->magic = LAGFX_DEVICE_MAGIC; dev->protocol_state = p;
        assert(doorbell_handle_read(p, 0x1014) == 0);
        lagfx_device_signal_display(dev, 0x1); lagfx_device_signal_display(dev, 0x5); lagfx_device_signal_display(dev, 0);
        assert(doorbell_handle_read(p, 0x1014) == 0x5);   /* 0x1 | 0x5, cleared by the read */
        assert(doorbell_handle_read(p, 0x1014) == 0);
        lagfx_device_signal_display(dev, 0x8);
        assert(doorbell_handle_read(p, 0x1014) == 0x8);
        assert(p->db_reads == 4 && p->db_nonzero == 2 && p->db_bit[0] == 1 && p->db_bit[1] == 0 && p->db_bit[2] == 1 && p->db_bit[3] == 1);
        assert(p->sig_total == 3 && p->sig_bit[0] == 2 && p->sig_bit[1] == 0 && p->sig_bit[2] == 1 && p->sig_bit[3] == 1);
        long nd = file_lines(path, "g7-gpu-display:");
        lagfx_diag_stats(p, now_ns_for_test(), true);
        assert(file_lines(path, "g7-gpu-display:") == nd + 1);
        assert(file_lines(path, "op_setpage=0 op_ack=0 op_swap=0") >= 1);
        assert(file_lines(path, "db_reads=4 db_nonzero=2 db_bit0=1 db_bit1=0 db_bit2=1 db_bit3=1 sig_total=3 sig_bit0=2 sig_bit1=0 sig_bit2=1 sig_bit3=1") == 1);
        free(dev);
    }

    {   /* idle triage line: absent without the completion queue; with it, the last poll time is recorded and the counters reach the line */
        long ni = file_lines(path, "g7-gpu-idle:");
        lagfx_diag_stats(p, now_ns_for_test(), true);
        assert(file_lines(path, "g7-gpu-idle:") == ni);
        p->protocol_mode = 1u;
        assert(lagfx_arm64_cq_enable(p));
        assert(p->diag2.last_poll_ns == 0);
        lagfx_arm64_poll_completions(p);
        assert(p->diag2.last_poll_ns != 0);
        atomic_store(&p->pending_stamps_bitmask, 0x8u);
        lagfx_diag_stats(p, now_ns_for_test(), true);
        assert(file_lines(path, "g7-gpu-idle:") == ni + 1);
        assert(file_lines(path, "held_chan_mask=0x0 held_exec_mask=0x0 drain_depth=0") == 1 && file_lines(path, "stamp_pending=0x8") == 1);
        lagfx_arm64_cq_shutdown(p, 10);
        p->protocol_mode = 0u;
    }

    free(p); remove(path); close(fd);
    puts("diag-unit OK");
    return 0;
}
