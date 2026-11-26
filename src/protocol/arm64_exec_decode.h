/*
 * libapplegfx-vulkan — ARM64 GPU exec (compute ops 0x43 / 0x45): decode only, nothing is executed or rendered.
 * src/protocol/arm64_exec_decode.h
 *
 * Reference: Apple's devices/pvg_metal.m exec (~1847-1984), devices/pvg_lite.c task_read / task_ptr / task_objects.
 *
 * Outer command (after arg_count_8b*8 barrier bytes): {u32 task, u32 nres, u32 nbuf}, nres * {u32 ref, u32 flags},
 * nbuf * {u64 off, u64 len}. Each buffer is one segment in the task's GPU address space:
 *   {u32 seglen, u32 info}  info: type = b0, cont = b1, more = b2, (b3 kept as hdr_top, unused by Apple)
 *   type 5 (protection options): 8 bytes of options follow, then the real {seglen, info}
 *   body (seglen bytes, clamped to the buffer): commands {u32 tag, u32 size (incl. this 8-byte header)}...
 * Types: 0 render, 1 compute, 2 blit, 4 information; other types are walked and counted too (Apple skips them).
 * Resource flags (Apple): 0x01 guest changed, 0x02 GPU changes (guest copy stale), 0x04 sync back after GPU work.
 */
#ifndef LAGFX_PROTOCOL_ARM64_EXEC_DECODE_H
#define LAGFX_PROTOCOL_ARM64_EXEC_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "state.h"
#include "opcodes.h"

#define LAGFX_EXEC_MAX_RES        4096u          /* sanity limit on nres */
#define LAGFX_EXEC_MAX_BUFS       64u            /* sanity limit on nbuf */
#define LAGFX_EXEC_MAX_BUF_BYTES  (64u << 20)    /* Apple skips a buffer larger than this (or shorter than 8) */
#define LAGFX_EXEC_READ_CAP_BUF   (1u << 20)     /* bytes actually read per buffer */
#define LAGFX_EXEC_READ_CAP_TOTAL (4u << 20)     /* bytes actually read per exec */
#define LAGFX_EXEC_MAX_INNER      65536u         /* inner commands walked per segment */

/* Task address space read, mirroring Apple's task_read: root page (16 KiB page number from op 0x38, always shift 14 whatever the ring page
 * shift) holds {u32 table page, u32 depth 1..3}; each level is 4096 u32 page numbers (12 bits of the 16 KiB page number), bit 31 is a flag.
 * Like Apple's, a task address above the depth's range wraps (high bits ignored). Reads may span pages. Returns false if any page is unmapped
 * or unreadable (nothing partial is promised in dst). */
bool lagfx_arm64_task_read(lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint64_t va, void *dst, uint64_t n);

/* One byte of task address space -> guest physical address (the page walk of task_read, for a single address). */
bool lagfx_arm64_task_translate(lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint64_t va, uint64_t *gpa_out);
/* Same walk, write direction (shell.write_memory); may span pages; false if any page is unmapped (earlier pages are then already written). */
bool lagfx_arm64_task_write(lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint64_t va, const void *src, uint64_t n);

typedef void (*lagfx_exec_tag_fn)(void *user, uint32_t tag, uint32_t size);

typedef struct {
    uint32_t type, cont, more, hdr_top;   /* from the (real) segment header */
    uint32_t seglen;                      /* as stated */
    uint32_t body_len;                    /* after clamping to the buffer */
    uint32_t ninner;                      /* inner commands walked */
    uint32_t tail;                        /* bytes of the buffer after the segment (Apple ignores them) */
    uint32_t slack;                       /* bytes inside the body after the last whole command (< 8) */
    bool prot_skipped;                    /* a type 5 header was skipped */
    bool clamped;                         /* seglen exceeded the buffer */
    bool truncated;                       /* the buffer was read only partly (read cap): the walk stopped there, not an error */
    bool bad_inner;                       /* a command had size < 8 or ran past the body: the walk stopped there */
} lagfx_exec_seg_t;

enum { LAGFX_SEG_OK = 0, LAGFX_SEG_SHORT = 1 /* no room for the header(s) */ };

/* Walks one buffer. buf holds `avail` bytes of the `len`-byte buffer (avail <= len; avail < len only under the read cap). Never reads
 * outside buf[0..avail). Calls fn (may be NULL) for every inner command. */
int lagfx_exec_walk_segment(const uint8_t *buf, uint32_t avail, uint64_t len, lagfx_exec_seg_t *seg, lagfx_exec_tag_fn fn, void *user);

/* The whole command: decodes, counts (p->diag2.exec), logs the first 32 and every 256th. Never changes guest-visible state. */
void lagfx_arm64_exec_decode(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);

#endif
