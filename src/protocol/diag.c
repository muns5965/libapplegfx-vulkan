/*
 * libapplegfx-vulkan — command diagnostics. See diag.h. Logging and counters only.
 */
#include "diag.h"
#include "draw_diag.h"
#include "state.h"
#include "arm64_completion.h"
#include "arm64_resources.h"
#include "../vulkan/command.h"
#include "../vulkan/caps.h"
#include "../vulkan/sampler_cache.h"
#include "display_diag.h"
#include "object_resolver.h"
#include "../common/log.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static const char *src_name(int src) { return src == LAGFX_DIAG_ROOT ? "root" : src == LAGFX_DIAG_COMPUTE ? "compute" : "display"; }

void lagfx_diag_note_0x44(lagfx_protocol_t *p, uint32_t chan, uint32_t task) {
    if (!p) return;
    if (chan < LAGFX_MAX_CHANNELS) { p->diag2.last44_task[chan] = task; p->diag2.last44_valid[chan] = 1; }
    p->diag2.last44_global_task = task; p->diag2.last44_global_seq = p->diag2.seq; p->diag2.last44_global_valid = 1;
}

/* First observer starts the clocks (several threads may race here): last_stats is stored BEFORE t0 is published, and only one thread's t0 wins.
 * Returns true if this call started them. */
static bool diag_init_clock(lagfx_protocol_t *p, uint64_t t) {
    if (__atomic_load_n(&p->diag2.t0_ns, __ATOMIC_ACQUIRE)) return false;
    uint64_t zero = 0;
    __atomic_store_n(&p->diag2.last_stats_ns, t, __ATOMIC_RELAXED);
    return __atomic_compare_exchange_n(&p->diag2.t0_ns, &zero, t, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

static bool dd_read_region(lagfx_protocol_t *p, uint32_t task_id, uint64_t va, uint32_t len, uint8_t *out) {
    lagfx_task_entry_t *t = lagfx_protocol_find_task(p, task_id);
    return t && lagfx_task_read_virtual(p, t, va, len, out);
}

static bool dd_owner(lagfx_protocol_t *p, uint64_t mapping, uint32_t *task) { return lagfx_a64_res_display_owner(p, mapping, task); }

void lagfx_diag_cmd(lagfx_protocol_t *p, int src, uint32_t chan, uint32_t rp, const uint8_t *raw, uint32_t length) {
    if (!p || !raw || length < 12u || src < 0 || src > 2) return;
    uint64_t t = now_ns();
    diag_init_clock(p, t);
    p->diag2.last_cmd_ns = t;
    uint64_t seq = ++p->diag2.seq;
    uint16_t op = (uint16_t)(raw[0] | raw[1] << 8);
    uint16_t arg8 = (uint16_t)(raw[2] | raw[3] << 8);
    uint32_t stamp = (uint32_t)raw[8] | (uint32_t)raw[9] << 8 | (uint32_t)raw[10] << 16 | (uint32_t)raw[11] << 24;
    p->diag2.src_cmds[src]++;
    uint64_t n = ++p->diag2.op_count[src][op & 0xff];
    if (src == LAGFX_DIAG_COMPUTE && op == 0x20) p->diag2.exec_20++;
    if (op == 0x43 || op == 0x45) p->diag2.exec_43_45++;
    lagfx_dd_set_reader(dd_read_region);
    lagfx_dd_set_owner_lookup(dd_owner);
    lagfx_dd_note_cmd(p, src, chan, raw, length);   /* display diagnostics: counts / read-only captures, no effect on dispatch */
    /* Bounded: the first 64 of each (source, opcode) and every 64th after that; root commands only the first 16. Counters above never stop. */
    uint64_t cap = src == LAGFX_DIAG_ROOT ? 16u : 64u;
    if (n > cap && (n % 64u) != 0u) return;
    char hex[3 * 64 + 8]; uint32_t shown = length < 64u ? length : 64u, o = 0;
    for (uint32_t i = 0; i < shown; i++) o += (uint32_t)snprintf(hex + o, sizeof hex - o, "%02x ", raw[i]);
    if (shown < length) snprintf(hex + o, sizeof hex - o, "...");
    LAGFX_LOG("diag cmd #%llu t=+%llums src=%s ch=%u rp=0x%x op=0x%04x n=%llu len=%u arg8=%u (barrier bytes %u) stamp=0x%08x raw: %s",
              (unsigned long long)seq, (unsigned long long)((t - p->diag2.t0_ns) / 1000000ull), src_name(src), chan, rp, op,
              (unsigned long long)n, length, arg8, (unsigned)arg8 * 8u, stamp, hex);
}

void lagfx_diag_stats(lagfx_protocol_t *p, uint64_t t, bool final);

void lagfx_diag_mmio(lagfx_protocol_t *p, bool is_write) {
    if (!p) return;
    /* two vCPU threads can arrive here: relaxed atomics keep the counts honest */
    if (is_write) __atomic_fetch_add(&p->diag2.mmio_writes, 1, __ATOMIC_RELAXED); else __atomic_fetch_add(&p->diag2.mmio_reads, 1, __ATOMIC_RELAXED);
    /* The clock is looked at only every 256th access. */
    if (((p->diag2.mmio_reads + p->diag2.mmio_writes) & 0xffu) != 0u) return;
    uint64_t t = now_ns();
    p->diag2.last_mmio_ns = t;
    if (diag_init_clock(p, t)) return;
    lagfx_diag_stats(p, t, false);
}

void lagfx_diag_tick(lagfx_protocol_t *p) {
    if (!p || !p->diag2.t0_ns) return;   /* nothing seen yet */
    lagfx_diag_stats(p, now_ns(), false);
}

void lagfx_diag_final(lagfx_protocol_t *p) {
    if (!p || !p->diag2.t0_ns) return;
    lagfx_diag_stats(p, now_ns(), true);
}

/* Statistics block. Timer/teardown callers are why this is not tied to guest MMIO any more: an idle guest (WFI) makes no MMIO accesses. */
void lagfx_diag_stats(lagfx_protocol_t *p, uint64_t t, bool final) {
    uint64_t last = __atomic_load_n(&p->diag2.last_stats_ns, __ATOMIC_RELAXED);
    if (!final) {
        if (t <= last || t - last < 10ull * 1000000000ull) return;   /* t <= last: another thread already stored a newer stamp — never underflow */
        /* only one of several callers (two vCPU threads + timer) prints for a given interval */
        if (!__atomic_compare_exchange_n(&p->diag2.last_stats_ns, &last, t, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    } else {
        __atomic_store_n(&p->diag2.last_stats_ns, t, __ATOMIC_RELAXED);
        LAGFX_LOG("g7-gpu-final: cumulative statistics at VM end follow (log_bytes=%llu log_write_errors=%llu; the log has no size limit of its own)",
                  (unsigned long long)lagfx_log_bytes_written(), (unsigned long long)lagfx_log_write_errors());
    }
    char line[2400], tags[400];
    const __typeof__(p->diag2.exec) *X = &p->diag2.exec;
    {   /* up to 24 busiest inner tags as type:tag=count (cumulative) */
        static const char tn[4] = {'0', '1', '2', '4'};
        size_t o = 0; bool used[4][512] = {{false}};
        tags[0] = 0;
        for (int n = 0; n < 24; n++) {
            int br = -1, bt = -1; uint64_t bc = 0;
            for (int r = 0; r < 4; r++) for (int g = 0; g < 512; g++) if (!used[r][g] && X->tag_hist[r][g] > bc) bc = X->tag_hist[r][g], br = r, bt = g;
            if (br < 0 || o + 24 >= sizeof tags) break;
            used[br][bt] = true;
            o += (size_t)snprintf(tags + o, sizeof tags - o, "%st%c:%d=%llu", n ? " " : "", tn[br], bt, (unsigned long long)bc);
        }
    }
    uint64_t age_cmd = p->diag2.last_cmd_ns ? (t - p->diag2.last_cmd_ns) / 1000000ull : 0;
    snprintf(line, sizeof line,
             "g7-gpu: t=+%llus mmio_reads=%llu mmio_writes=%llu root_commands=%llu compute_commands=%llu display_commands=%llu unknown_commands=%llu "
             "exec20=%llu exec43_45=%llu display_submits=%llu display_submit_no_object=%llu display_submit_index_ignored=%llu display_submit_rendered=%llu "
             "irq_raised=%llu last_command_age_ms=%llu log_bytes=%llu log_write_errors=%llu "
             "ignored_known=%llu exec_received=%llu exec_decoded=%llu exec_decode_failed=%llu exec_bad_size=%llu exec_read_failed=%llu exec_bad_seg=%llu exec_no_task=%llu "
             "exec_bufs=%llu exec_bytes=%llu exec_capped=%llu exec_clamped=%llu exec_tail_bytes=%llu seg_t0=%llu seg_t1=%llu seg_t2=%llu seg_t4=%llu seg_t5=%llu seg_other=%llu "
             "seg_cont=%llu seg_more=%llu exec_res=%llu res_f1=%llu res_f2=%llu res_f4=%llu res_fhi=%llu res_noobj=%llu exec_inner=%llu exec_inner_bad=%llu tags[%s]",
             (unsigned long long)((t - p->diag2.t0_ns) / 1000000000ull), (unsigned long long)p->diag2.mmio_reads, (unsigned long long)p->diag2.mmio_writes,
             (unsigned long long)p->diag2.src_cmds[0], (unsigned long long)p->diag2.src_cmds[1], (unsigned long long)p->diag2.src_cmds[2],
             (unsigned long long)p->unknown_opcode_count, (unsigned long long)p->diag2.exec_20, (unsigned long long)p->diag2.exec_43_45,
             (unsigned long long)p->diag2.display_submits, (unsigned long long)p->diag2.display_submit_skipped_null,
             (unsigned long long)p->diag2.display_submit_ignored_index, (unsigned long long)p->diag2.display_submit_rendered,
             (unsigned long long)p->diag2.irq_raised, (unsigned long long)age_cmd,
             (unsigned long long)lagfx_log_bytes_written(), (unsigned long long)lagfx_log_write_errors(),
             (unsigned long long)p->diag2.ignored_known, (unsigned long long)X->received, (unsigned long long)X->decoded, (unsigned long long)X->failed,
             (unsigned long long)X->bad_size, (unsigned long long)X->read_failed, (unsigned long long)X->bad_seg, (unsigned long long)X->no_task,
             (unsigned long long)X->bufs, (unsigned long long)X->bytes_read, (unsigned long long)X->bytes_capped, (unsigned long long)X->seg_clamped,
             (unsigned long long)X->tail_bytes, (unsigned long long)X->seg_t[0], (unsigned long long)X->seg_t[1], (unsigned long long)X->seg_t[2],
             (unsigned long long)X->seg_t[4], (unsigned long long)X->seg_t[5], (unsigned long long)X->seg_other, (unsigned long long)X->seg_cont,
             (unsigned long long)X->seg_more, (unsigned long long)X->res, (unsigned long long)X->res_flag[0], (unsigned long long)X->res_flag[1],
             (unsigned long long)X->res_flag[2], (unsigned long long)X->res_flag_hi, (unsigned long long)X->obj_missing, (unsigned long long)X->inner,
             (unsigned long long)X->inner_bad, tags);
    fprintf(stderr, "%s\n", line);
    LAGFX_LOG("%s", line);
    #define DBC(f) ((unsigned long long)atomic_load_explicit(&p->f, memory_order_relaxed))
    if (p->diag2.disp.op_setpage || p->diag2.disp.op_ack || p->diag2.disp.op_swap || DBC(db_reads) || DBC(sig_total)) {   /* also when the guest only polls the doorbell */   /* arm64 display ops, stage by stage (display_submit_rendered is the legacy vchan path only) */
        const __typeof__(p->diag2.disp) *D = &p->diag2.disp;
        snprintf(line, sizeof line, "g7-gpu-display: op_setpage=%llu op_ack=%llu op_swap=%llu swap_short=%llu surface_resolved=%llu surface_unresolved=%llu gpu_content=%llu written_back=%llu gpu_unwritten=%llu handed_to_shell=%llu not_handed=%llu swap_events=%llu db_reads=%llu db_nonzero=%llu db_bit0=%llu db_bit1=%llu db_bit2=%llu db_bit3=%llu sig_total=%llu sig_bit0=%llu sig_bit1=%llu sig_bit2=%llu sig_bit3=%llu",
                 (unsigned long long)D->op_setpage, (unsigned long long)D->op_ack, (unsigned long long)D->op_swap, (unsigned long long)D->swap_short, (unsigned long long)D->surface_resolved,
                 (unsigned long long)D->surface_unresolved, (unsigned long long)D->gpu_content, (unsigned long long)D->written_back, (unsigned long long)D->gpu_unwritten,
                 (unsigned long long)D->handed_to_shell, (unsigned long long)D->not_handed, (unsigned long long)X->run.swap_events,
                 DBC(db_reads), DBC(db_nonzero), DBC(db_bit[0]), DBC(db_bit[1]), DBC(db_bit[2]), DBC(db_bit[3]),
                 DBC(sig_total), DBC(sig_bit[0]), DBC(sig_bit[1]), DBC(sig_bit[2]), DBC(sig_bit[3]));
        fprintf(stderr, "%s\n", line);
        LAGFX_LOG("%s", line);
    }
    {   /* arm64 deferred completion + GPU wait failures (separate line: the line above keeps its format) */
        uint64_t wt = 0, wf = 0, sf = 0;
        lagfx_arm64_cq_stats_t q;
        lagfx_arm64_cq_stats(p, &q);
        lagfx_vk_gpu_fail_stats(&wt, &wf, &sf);
        if (p->a64cq || wt || wf || sf || X->run.mapped_written_back) {
            snprintf(line, sizeof line, "g7-gpu-sync: cq_ok=%llu cq_failed_pre=%llu cq_fault_completed=%llu cq_fault_rejected=%llu cq_slow=%llu cq_hard_timeouts=%llu barrier_holds=%llu barrier_drains=%llu barrier_timeouts=%llu fault=%d cq_pending=%llu vk_wait_timeouts=%llu vk_wait_failed=%llu vk_submit_failed=%llu mapped_written_back=%llu mapped_bytes_down=%llu",
                     (unsigned long long)q.ok, (unsigned long long)q.failed_pre, (unsigned long long)q.fault_completed, (unsigned long long)q.fault_rejected,
                     (unsigned long long)q.slow, (unsigned long long)q.hard_timeouts, (unsigned long long)q.barrier_holds, (unsigned long long)q.barrier_drains, (unsigned long long)q.barrier_timeouts, (int)q.fault, (unsigned long long)lagfx_arm64_cq_pending(p),
                     (unsigned long long)wt, (unsigned long long)wf, (unsigned long long)sf, (unsigned long long)X->run.mapped_written_back, (unsigned long long)X->run.mapped_bytes_down);
            fprintf(stderr, "%s\n", line);
            LAGFX_LOG("%s", line);
        }
    }
    if (p->a64cq) {   /* stall triage, lock-free: completions == commands plus an old last poll means an idle guest, not a stuck queue */
        uint64_t lp = __atomic_load_n(&p->diag2.last_poll_ns, __ATOMIC_RELAXED);
        snprintf(line, sizeof line, "g7-gpu-idle: last_poll_age_ms=%llu last_cmd_age_ms=%llu held_chan_mask=0x%x held_exec_mask=0x%x drain_depth=%u display_cause_pending=0x%x stamp_pending=0x%x",
                 (unsigned long long)(lp && t > lp ? (t - lp) / 1000000ull : 0), (unsigned long long)age_cmd,
                 (unsigned)lagfx_arm64_held_channels(p), (unsigned)p->a64_held_chan_mask, (unsigned)p->a64_drain_depth,
                 (unsigned)atomic_load_explicit(&p->pending_displays_bitmask, memory_order_relaxed), (unsigned)atomic_load_explicit(&p->pending_stamps_bitmask, memory_order_relaxed));
        fprintf(stderr, "%s\n", line);
        LAGFX_LOG("%s", line);
    }
    if (X->run.execs) {   /* ARM64 render exec extensions (draw variants, sampler fidelity, misaligned buffer binds, no-op tags) */
        const __typeof__(X->run) *R = &X->run;
        char el[1600];
        snprintf(el, sizeof el,
                 "g7-gpu-ext: draw[instanced=%llu base=%llu wide=%llu indirect=%llu indirect_bad=%llu zero=%llu] noop[hint=%llu fence=%llu barrier_partial=%llu] "
                 "sampler[compare_ignored=%llu unnorm_applied=%llu mirror_clamp_approx=%llu lod_binds=%llu lod_dropped=%llu] buf[gathered=%llu gather_failed=%llu range_clamped=%llu writable_skipped=%llu] storage_img_unsupported=%llu vs_tex_binds=%llu unnorm_lod0=%llu unnorm_skipped=%llu gather_why[nomap=%llu range=%llu cache=%llu scratch=%llu budget=%llu] gather[bytes=%llu reused=%llu] no_pipeline_why[none=%llu unresolved=%llu build=%llu bind=%llu] linear_stale=%llu tex_prepared=%llu cxs_binds=%llu tex3d_binds=%llu tex_dim_mismatch=%llu",
                 (unsigned long long)R->ext_draw_instanced, (unsigned long long)R->ext_draw_base, (unsigned long long)R->ext_draw_wide, (unsigned long long)R->ext_draw_indirect,
                 (unsigned long long)R->ext_draw_indirect_bad, (unsigned long long)R->ext_draw_zero, (unsigned long long)R->ext_noop_hint, (unsigned long long)R->ext_noop_fence,
                 (unsigned long long)R->ext_barrier, (unsigned long long)R->ext_samp_compare, (unsigned long long)R->ext_samp_unnorm, (unsigned long long)R->ext_samp_mirror_clamp,
                 (unsigned long long)R->ext_samp_lod_binds, (unsigned long long)R->ext_samp_lod_dropped, (unsigned long long)R->ext_gathered, (unsigned long long)R->ext_gather_failed,
                 (unsigned long long)R->ext_range_clamped, (unsigned long long)R->ext_wsb_skipped, (unsigned long long)R->ext_storage_img_unsupported,
                 (unsigned long long)R->ext_vs_tex_binds, (unsigned long long)R->ext_unnorm_lod0, (unsigned long long)R->ext_unnorm_skipped,
                 (unsigned long long)R->ext_gather_why[0], (unsigned long long)R->ext_gather_why[1], (unsigned long long)R->ext_gather_why[2], (unsigned long long)R->ext_gather_why[3], (unsigned long long)R->ext_gather_why[4], (unsigned long long)R->ext_gather_bytes, (unsigned long long)R->ext_gather_reused,
                 (unsigned long long)R->no_pipeline_why[0], (unsigned long long)R->no_pipeline_why[1], (unsigned long long)R->no_pipeline_why[2], (unsigned long long)R->no_pipeline_why[3], (unsigned long long)X->pipe.desc_linear_stale, (unsigned long long)R->ext_tex_prepared,
                 (unsigned long long)R->ext_cxs_binds, (unsigned long long)R->ext_tex3d_binds, (unsigned long long)R->ext_tex_dim_mismatch);
        fprintf(stderr, "%s\n", el);
        LAGFX_LOG("%s", el);
        lagfx_sampler_diag_t sd;
        lagfx_sampler_diag_get(&sd);
        snprintf(el, sizeof el,
                 "g7-gpu-cxs: cxs_state_builds[bicubic_as_linear=%llu clamp_zero_as_border=%llu addr_outside_table=%llu filter_outside_table=%llu mip_outside_table=%llu compare_field_not8=%llu] cxs_testpath_state_builds[unnorm_illegal_addr=%llu] cxs_log[distinct_words_logged=%llu words_log_suppressed=%llu] rt_sampler_objs[addr_out_of_range=%llu mip_out_of_range=%llu]",
                 (unsigned long long)sd.cxs_state_builds_bicubic_as_linear, (unsigned long long)sd.cxs_state_builds_clamp_zero_as_border, (unsigned long long)sd.cxs_state_builds_addr_outside_table, (unsigned long long)sd.cxs_state_builds_filter_outside_table,
                 (unsigned long long)sd.cxs_state_builds_mip_outside_table, (unsigned long long)sd.cxs_state_builds_compare_field_not8, (unsigned long long)sd.cxs_testpath_state_builds_unnorm_illegal_addr, (unsigned long long)sd.cxs_distinct_words_logged, (unsigned long long)sd.cxs_words_log_suppressed,
                 (unsigned long long)R->ext_samp_addr_unknown, (unsigned long long)R->ext_samp_mip_unknown);
        fprintf(stderr, "%s\n", el);
        LAGFX_LOG("%s", el);
    }
    if (X->run.execs) {   /* ARM64 render execution (LAGFX_ARM64_EXEC=1): totals, then the per-tag outcome of every tag seen */
        const __typeof__(X->run) *R = &X->run;
        char rl[2400], tg[900];
        size_t o = 0;
        tg[0] = 0;
        for (int g = 0; g < 512 && o + 40 < sizeof tg; g++)
            if (R->tag_done[g] || R->tag_partial[g] || R->tag_unsup[g])
                o += (size_t)snprintf(tg + o, sizeof tg - o, "%s%d=%llu/%llu/%llu", o ? " " : "", g, (unsigned long long)R->tag_done[g],
                                      (unsigned long long)R->tag_partial[g], (unsigned long long)R->tag_unsup[g]);
        snprintf(rl, sizeof rl,
                 "g7-gpu-run: execs=%llu passes=%llu draws=%llu draws_skipped=%llu submits=%llu submit_failed=%llu submit_pending=%llu seg_render=%llu seg_unsupported=%llu no_vulkan=%llu "
                 "bad_rec=%llu no_pass=%llu no_target=%llu no_pipeline=%llu no_vertex_buf=%llu no_index_buf=%llu prim_unsupported=%llu extra_attach_ignored=%llu multi_attach=%llu indep_mismatch=%llu "
                 "load_uninit_cleared=%llu pipeline_unresolved=%llu obj_unsupported=%llu tag_over=%llu "
                 "mapped_created=%llu mapped_shared=%llu mapped_uploaded=%llu mapped_written_back=%llu mapped_unsupported=%llu mapped_no_desc=%llu mapped_bytes_up=%llu mapped_bytes_down=%llu mapped_pages_skipped=%llu swap_events=%llu swap_flushed=%llu swap_deferred=%llu swap_failed=%llu swap_dropped=%llu mapped_remapped=%llu swap_partial=%llu "
                 "tags(done/partial/unsupported)[%s]",
                 (unsigned long long)R->execs, (unsigned long long)R->passes, (unsigned long long)R->draws, (unsigned long long)R->draws_skipped,
                 (unsigned long long)R->submits, (unsigned long long)R->submit_failed, (unsigned long long)R->submit_pending, (unsigned long long)R->seg_render, (unsigned long long)R->seg_unsupported,
                 (unsigned long long)R->no_vulkan, (unsigned long long)R->bad_rec, (unsigned long long)R->no_pass, (unsigned long long)R->no_target,
                 (unsigned long long)R->no_pipeline, (unsigned long long)R->no_vertex_buf, (unsigned long long)R->no_index_buf,
                 (unsigned long long)R->prim_unsupported, (unsigned long long)R->extra_attach_ignored, (unsigned long long)R->multi_attach, (unsigned long long)R->blend_indep_mismatch, (unsigned long long)R->load_uninit_cleared,
                 (unsigned long long)R->pipeline_unresolved, (unsigned long long)R->obj_unsupported, (unsigned long long)R->tag_over,
                 (unsigned long long)R->mapped_created, (unsigned long long)R->mapped_shared, (unsigned long long)R->mapped_uploaded, (unsigned long long)R->mapped_written_back,
                 (unsigned long long)R->mapped_unsupported, (unsigned long long)R->mapped_no_desc, (unsigned long long)R->mapped_bytes_up, (unsigned long long)R->mapped_bytes_down,
                 (unsigned long long)R->mapped_pages_skipped, (unsigned long long)R->swap_events, (unsigned long long)R->swap_flushed, (unsigned long long)R->swap_deferred, (unsigned long long)R->swap_failed, (unsigned long long)R->swap_dropped, (unsigned long long)R->mapped_remapped, (unsigned long long)R->swap_partial, tg);
        fprintf(stderr, "%s\n", rl);
        LAGFX_LOG("%s", rl);
        {   /* mapped write-back host-sync counters (read-only): HOST_READ barriers recorded / failed-exec clears / finishes skipped / unreadable descriptors */
            char wl[300];
            snprintf(wl, sizeof wl, "g7-wb-sync: wb_host_barriers_recorded=%llu wb_down_pending_cleared_on_fail=%llu wb_down_skipped_not_recorded=%llu wb_finish_desc_unreadable_owed=%llu",
                     (unsigned long long)R->wb_host_barriers_recorded, (unsigned long long)R->wb_down_pending_cleared_on_fail, (unsigned long long)R->wb_down_skipped_not_recorded, (unsigned long long)R->wb_finish_desc_unreadable_owed);
            fprintf(stderr, "%s\n", wl);
            LAGFX_LOG("%s", wl);
        }
        if (R->dss_decoded || R->dss_bad || R->ds_passes || R->cull_set || R->winding_set || R->bias_set || R->stencil_ref_set || R->viewport_depth_set) {
            char dl[700];
            snprintf(dl, sizeof dl,
                     "g7-gpu-ds: dss[decoded=%llu bad=%llu bound=%llu unresolved=%llu] passes[ds=%llu depth_only=%llu attach_unsupported=%llu split_ignored=%llu no_device_support=%llu] draws_ds=%llu "
                     "stencil_ref=%llu cull=%llu winding=%llu bias=%llu bias_clamp_dropped=%llu viewport_depth=%llu",
                     (unsigned long long)R->dss_decoded, (unsigned long long)R->dss_bad, (unsigned long long)R->dss_bound, (unsigned long long)R->dss_unresolved,
                     (unsigned long long)R->ds_passes, (unsigned long long)R->ds_depth_only_passes, (unsigned long long)R->ds_attach_unsupported, (unsigned long long)R->ds_split_ignored,
                     (unsigned long long)R->ds_no_device_support, (unsigned long long)R->ds_draws, (unsigned long long)R->stencil_ref_set, (unsigned long long)R->cull_set,
                     (unsigned long long)R->winding_set, (unsigned long long)R->bias_set, (unsigned long long)R->bias_clamp_dropped, (unsigned long long)R->viewport_depth_set);
            fprintf(stderr, "%s\n", dl);
            LAGFX_LOG("%s", dl);
        }
    }
    if (X->pipe.resolved || X->pipe.resolve_failed || X->pipe.obj_created || X->pipe.dump_objects || X->pipe.cap_draws || X->pipe.cap_failed || X->pipe.rf_missing_cap) {   /* ARM64 pipeline resolver / descriptor binding / capture */
        const __typeof__(X->pipe) *Q = &X->pipe;
#define U(f) (unsigned long long)Q->f
        char pl[4800];
        snprintf(pl, sizeof pl,
                 "g7-gpu-pipe: resolved=%llu failed=%llu[no_blob=%llu bad_tag=%llu no_refs=%llu no_lib=%llu no_stage=%llu module=%llu translate=%llu layout=%llu multi_stream=%llu vertex_res=%llu bindings=%llu no_vk=%llu root[malformed=%llu missing=%llu same=%llu bad_type=%llu wrong_stage=%llu]] "
                 "xlate[air2spv=%llu] stage_link[mismatch_fixed=%llu missing_zeroed=%llu unlinked=%llu] blend[decoded=%llu off=%llu color1=%llu] fbfetch[pipes=%llu draws=%llu stub=%llu(vertex=%llu no_local_read=%llu attachment_index=%llu too_many=%llu)] stubs[blend=%llu depth_stencil=%llu color_fmt_from_pass=%llu raster=%llu vertex_tex=%llu] built=%llu build_failed=%llu replaced=%llu topo[%llu %llu %llu %llu %llu] "
                 "desc[sets=%llu unbound=%llu(buf=%llu input=%llu simg=%llu tex=%llu texview=%llu samp=%llu) unaligned=%llu feedback=%llu failed=%llu] point_no_size=%llu samplers=%llu/%llu dump[objs=%llu bytes=%llu files=%llu dropped=%llu truncated=%llu draws=%llu table_full=%llu] cap[draws=%llu passes=%llu snaps=%llu bytes=%llu truncated=%llu skipped_filter=%llu skipped_cap=%llu failed=%llu dedup=%llu]",
                 U(resolved), U(resolve_failed), U(rf_no_blob), U(rf_bad_tag), U(rf_no_refs), U(rf_no_lib), U(rf_no_stage), U(rf_module), U(rf_translate), U(rf_layout), U(rf_multi_stream), U(rf_vertex_res),
                 U(rf_bindings), U(rf_no_vulkan), U(rf_root[2]), U(rf_root[3]), U(rf_root[4]), U(rf_root[5]), U(rf_root_stage), U(xlate_air2spv), U(link_mismatch_fixed), U(link_missing_zeroed), U(link_unlinked), U(blend_decoded), U(blend_off), U(color1_seen), U(fbfetch_pipes), U(fbfetch_draws), U(stub_fbfetch), U(stub_fbfetch_why[1]), U(stub_fbfetch_why[2]), U(stub_fbfetch_why[3]), U(stub_fbfetch_why[4]), U(stub_blend), U(stub_depth_stencil), U(stub_color_format_from_pass), U(stub_raster_state), U(stub_vertex_textures),
                 U(pipes_built), U(pipe_build_failed), U(pipe_replaced), U(topo[0]), U(topo[1]), U(topo[2]), U(topo[3]), U(topo[4]),
                 U(desc_sets), U(desc_unbound), U(desc_unbound_why[0]), U(desc_unbound_why[1]), U(desc_unbound_why[2]), U(desc_unbound_why[3]), U(desc_unbound_why[4]), U(desc_unbound_why[5]), U(desc_unaligned), U(desc_feedback), U(desc_failed), U(point_no_size), U(samplers), U(sampler_failed),
                 U(dump_objects), U(dump_bytes), U(dump_files), U(dump_skipped_cap), U(dump_truncated), U(dump_draws), U(dump_table_full),
                 U(cap_draws), U(cap_passes), U(cap_snaps), U(cap_bytes), U(cap_truncated), U(cap_skipped_filter), U(cap_skipped_cap), U(cap_failed), U(cap_blobs_dedup));
        {
            size_t po = strlen(pl);
            for (unsigned c = 0; c < LAGFX_VK_CAP_COUNT && po + 40 < sizeof pl; c++)
                if (Q->stub_missing_cap[c]) po += (size_t)snprintf(pl + po, sizeof pl - po, " stub_missing_cap_%s=%llu", lagfx_vk_cap_name(c), U(stub_missing_cap[c]));
        }
        {   /* mirror object table, cumulative per run (arm64_resources.c); live <= 4096, full = creations refused because all slots were live */
            size_t po = strlen(pl);
            if (po + 200 < sizeof pl)
                snprintf(pl + po, sizeof pl - po, " obj_table[created=%llu destroyed=%llu reused=%llu live=%llu peak_live=%llu high_water=%llu full=%llu lookup_miss=%llu deferred=%llu]",
                         U(obj_created), U(obj_destroyed), U(obj_reused), U(obj_live), U(obj_peak_live), U(obj_high_water), U(obj_full), U(obj_lookup_miss), U(obj_deferred));
        }
        {   /* desc[... tex=N ...] by reason (only the non-zero ones); the first 8 distinct textures of each are logged as g7-gpu-tex-unbound lines when they happen */
            size_t po = strlen(pl);
            bool first = true;
            for (unsigned c = 0; c < LAGFX_A64_TEXWHY_COUNT && po + 40 < sizeof pl; c++)
                if (Q->desc_tex_why[c]) po += (size_t)snprintf(pl + po, sizeof pl - po, "%s%s=%llu", first ? " texwhy[" : " ", lagfx_a64_texwhy_name(c), U(desc_tex_why[c])), first = false;
            if (!first && po + 2 < sizeof pl) pl[po++] = ']', pl[po] = 0;
        }
        {   /* desc[... buf=N ...] by cause (non-zero only): the first 8 distinct are logged as g7-gpu-buf-unbound lines */
            size_t po = strlen(pl);
            static const char *const bn[7] = { "no_metal_slot", "slot_range", "never_bound", "unresolved", "not_buffer", "?", "offset" };
            bool first = true;
            for (unsigned c = 0; c < 7u && po + 40 < sizeof pl; c++)
                if (Q->desc_buf_why[c]) po += (size_t)snprintf(pl + po, sizeof pl - po, "%s%s=%llu", first ? " bufwhy[" : " ", bn[c], U(desc_buf_why[c])), first = false;
            if (!first && po + 2 < sizeof pl) pl[po++] = ']', pl[po] = 0;
        }
#undef U
        fprintf(stderr, "%s\n", pl);
        LAGFX_LOG("%s", pl);
    }
    if (X->tile.pipes || X->tile.pipe_failed || X->tile.dispatches || X->tile.binds_buf || X->tile.binds_tex || X->tile.draws_ignored) {   /* ARM64 tile shaders (arm64_tile.c) */
        const __typeof__(X->tile) *T = &X->tile;
#define U(f) (unsigned long long)T->f
        char tl[900];
        snprintf(tl, sizeof tl,
                 "g7-gpu-tile: pipes=%llu failed=%llu[no_kernel=%llu missing_cap=%llu bindings=%llu unsupported_simd=%llu] binds[buf=%llu tex=%llu ignored=%llu] dispatches=%llu groups=%llu whole=%llu region=%llu(unaligned=%llu empty=%llu) "
                 "sg[pinned32=%llu override=%llu emulated=%llu] rt_index_ignored=%llu draws_ignored=%llu skipped[no_pass=%llu no_pipe=%llu bad_rec=%llu local=%llu no_sg32=%llu rt=%llu unbound=%llu unaligned=%llu feedback=%llu desc=%llu pipe_build=%llu] resume_failed=%llu",
                 U(pipes), U(pipe_failed), U(pipe_no_kernel), U(pipe_missing_cap), U(pipe_bindings), U(pipe_unsupported_simd), U(binds_buf), U(binds_tex), U(binds_ignored), U(dispatches), U(groups), U(whole), U(region), U(region_unaligned), U(region_empty),
                 U(sg_pinned), U(sg_override), U(sg_emulated), U(rt_index_ignored), U(draws_ignored), U(skip_no_pass), U(skip_no_pipe), U(skip_bad_rec), U(skip_local), U(skip_no_sg32), U(skip_rt), U(skip_unbound), U(skip_unaligned), U(skip_feedback),
                 U(skip_desc), U(skip_pipe_build), U(resume_failed));
#undef U
        fprintf(stderr, "%s\n", tl);
        LAGFX_LOG("%s", tl);
        char gl[900]; int gn = snprintf(gl, sizeof gl, "g7-gpu-tile-detail: gathered=%llu gather_failed=%llu unaligned_written=%llu geom[", (unsigned long long)T->gathered, (unsigned long long)T->gather_failed, (unsigned long long)T->unaligned_written);
        for (uint32_t i = 0; i < T->n_geom && gn > 0 && (size_t)gn < sizeof gl; i++)
            gn += snprintf(gl + gn, sizeof gl - (size_t)gn, "%s{%s o=%u,%u r=%ux%u tg=%ux%ux%u tgt=%ux%u}", i ? " " : "", T->geom[i].region ? "region" : "whole", T->geom[i].ox, T->geom[i].oy, T->geom[i].rw, T->geom[i].rh, T->geom[i].tw, T->geom[i].th, T->geom[i].td, T->geom[i].tgt_w, T->geom[i].tgt_h);
        if (gn > 0 && (size_t)gn < sizeof gl) gn += snprintf(gl + gn, sizeof gl - (size_t)gn, "] unaligned_binds[");
        for (uint32_t i = 0; i < T->n_unal && gn > 0 && (size_t)gn < sizeof gl; i++)
            gn += snprintf(gl + gn, sizeof gl - (size_t)gn, "%s{buffer(%u) off=%llu len=%llu align=%llu %s}", i ? " " : "", T->unal[i].metal_index, (unsigned long long)T->unal[i].off, (unsigned long long)T->unal[i].len, (unsigned long long)T->unal[i].align, T->unal[i].written ? "WRITTEN" : "read-only");
        if (gn > 0 && (size_t)gn < sizeof gl - 1u) strcat(gl, "]");
        fprintf(stderr, "%s\n", gl);
        LAGFX_LOG("%s", gl);
    }
    lagfx_dd_log_stats(final);   /* g7-pvg-ops: display-path counters (capped) */
    lagfx_draw_diag_flush(p, final);   /* per-draw tuples seen since the last statistics (bounded; formats and logs outside the draw path) */
}
