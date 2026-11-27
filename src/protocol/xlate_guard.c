/*
 * libapplegfx-vulkan — guard for the legacy task translator chain in ARM64 mode. See xlate_guard.h.
 */
#include "xlate_guard.h"
#include "../common/log.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static lagfx_xg_stats_t g_st;
static uint32_t g_lock;                  /* tiny spin lock for the site table only */
static uint64_t g_t0_ns, g_last_ns, g_last_sum;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void lock(void)   { while (__atomic_exchange_n(&g_lock, 1u, __ATOMIC_ACQUIRE)) { } }
static void unlock(void) { __atomic_store_n(&g_lock, 0u, __ATOMIC_RELEASE); }
#define INC(x) __atomic_add_fetch(&(x), 1u, __ATOMIC_RELAXED)
#define LD(x)  __atomic_load_n(&(x), __ATOMIC_RELAXED)

static bool is_arm64(const lagfx_protocol_t *p) { return lagfx_proto_arm64(p); }

/* Returns the site's new count; *first = true when this call created the site entry. */
static uint64_t site_hit(const char *site, bool *first) {
    uint64_t n = 0;
    *first = false;
    lock();
    uint32_t i = 0;
    for (; i < g_st.nsites; i++) if (strncmp(g_st.site_name[i], site, sizeof g_st.site_name[i] - 1u) == 0) break;
    if (i == g_st.nsites && g_st.nsites < LAGFX_XG_MAX_SITES) {
        snprintf(g_st.site_name[i], sizeof g_st.site_name[i], "%s", site);
        g_st.nsites++;
        *first = true;
    }
    if (i < g_st.nsites) n = ++g_st.site_count[i];
    unlock();
    return n;
}

bool lagfx_xg_legacy_entered(const lagfx_protocol_t *p, const char *site, const void *ret) {
    if (!is_arm64(p)) return false;
    bool first;
    INC(g_st.legacy_xlate_in_arm64);
    INC(g_st.blocked);
    site_hit(site ? site : "?", &first);
    if (first)
        LAGFX_WARN("g7-gpu-xlate: legacy translator used in arm64 mode: site=%s caller=%p (blocked: no translation, no identity access; "
                   "later entries of this site are only counted)", site ? site : "?", ret);
    return true;
}

void lagfx_xg_note_redirect(const lagfx_protocol_t *p, const char *site, const void *ret) {
    if (!is_arm64(p)) return;
    bool first;
    INC(g_st.legacy_xlate_in_arm64);
    INC(g_st.redirected);
    site_hit(site ? site : "?", &first);
    if (first)
        LAGFX_WARN("g7-gpu-xlate: legacy translator API entered in arm64 mode: site=%s caller=%p (served by the arm64 16 KiB reader; "
                   "later entries of this site are only counted)", site ? site : "?", ret);
}

void lagfx_xg_reject(const lagfx_protocol_t *p, lagfx_xg_reject_kind_t kind, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr || (unsigned)kind >= LAGFX_XG_REJ_KINDS) return;
    static const char *const names[LAGFX_XG_REJ_KINDS] = { "compute op 0x37", "root op 0x20", "exec entry" };
    uint64_t n = __atomic_add_fetch(&g_st.rejected[kind], 1u, __ATOMIC_RELAXED);
    INC(g_st.rejected_total);
    if (n <= 8u || (n % 256u) == 0u) {
        char hx[3 * 32 + 1];
        uint32_t m = 0, dn = hdr->payload && hdr->payload_size ? (hdr->payload_size < 32u ? hdr->payload_size : 32u) : 0u;
        hx[0] = 0;
        for (uint32_t i = 0; i < dn; i++) m += (uint32_t)snprintf(hx + m, sizeof hx - m, "%02x%s", hdr->payload[i], (i & 3u) == 3u ? " " : "");
        LAGFX_WARN("g7-gpu-xlate: arm64 %s rejected #%llu ch=%u op=0x%02x stamp=0x%08x arg8=%u payload=%u first%u=[%s] "
                   "(not executed: no arm64 layout known; stamp is completed by the drain, legacy 4 KiB translator not run)",
                   names[kind], (unsigned long long)n, (unsigned)p->current_chan_id, (unsigned)hdr->opcode, hdr->stamp,
                   (unsigned)hdr->arg_count_8b, (unsigned)hdr->payload_size, dn, hx);
    }
}

void lagfx_xg_snapshot(lagfx_xg_stats_t *out) {
    if (!out) return;
    lock();
    *out = g_st;
    unlock();
}

void lagfx_xg_reset(void) {
    lock();
    memset(&g_st, 0, sizeof g_st);
    g_t0_ns = g_last_ns = g_last_sum = 0;
    unlock();
}

void lagfx_xg_stats_line(const lagfx_protocol_t *p, bool final) {
    (void)p;
    lagfx_xg_stats_t s;
    lagfx_xg_snapshot(&s);
    char sites[LAGFX_XG_MAX_SITES * 40u + 8u];
    uint32_t m = 0;
    sites[0] = 0;
    for (uint32_t i = 0; i < s.nsites; i++) m += (uint32_t)snprintf(sites + m, sizeof sites - m, "%s%s=%llu", i ? " " : "", s.site_name[i], (unsigned long long)s.site_count[i]);
    uint64_t t = g_t0_ns ? (now_ns() - g_t0_ns) / 1000000000ull : 0;
    LAGFX_LOG("g7-gpu-xlate: t=+%llus%s legacy_xlate_in_arm64=%llu[blocked=%llu redirected=%llu] sites[%s] rejected=%llu[op37=%llu root20=%llu exec_entry=%llu]",
              (unsigned long long)t, final ? " final" : "", (unsigned long long)s.legacy_xlate_in_arm64, (unsigned long long)s.blocked,
              (unsigned long long)s.redirected, sites, (unsigned long long)s.rejected_total, (unsigned long long)s.rejected[LAGFX_XG_REJ_COMPUTE_37],
              (unsigned long long)s.rejected[LAGFX_XG_REJ_ROOT_20], (unsigned long long)s.rejected[LAGFX_XG_REJ_EXEC_ENTRY]);
}

void lagfx_xg_tick(const lagfx_protocol_t *p) {
    if (!is_arm64(p)) return;
    uint64_t t = now_ns();
    uint64_t sum = LD(g_st.legacy_xlate_in_arm64) + LD(g_st.rejected_total);
    if (!g_t0_ns) { g_t0_ns = g_last_ns = t; g_last_sum = sum; lagfx_xg_stats_line(p, false); return; }   /* first tick: all-zero line is visible */
    if (t - g_last_ns < 10ull * 1000000000ull || sum == g_last_sum) return;
    g_last_ns = t; g_last_sum = sum;
    lagfx_xg_stats_line(p, false);
}
