/* SPDX-License-Identifier: MIT */
/* libapplegfx-vulkan — the fragment-module reduction ladder (see pipeline_reduce.h). DIAGNOSTIC only, opt-in, never changes the normal build. */
#include "pipeline_reduce.h"

#ifdef LAGFX_HAVE_VULKAN

#include "common/log.h"
#include "descriptor_layout.h"
#include "instance.h"
#include "pipeline_bundle.h"
#include "spv_iface.h"
#include "spv_variants.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------- sha-256 (public domain algorithm, own implementation) */
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) { uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10); w[i] = w[i - 16] + s0 + w[i - 7] + s1; }
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25), ch = (e & f) ^ (~e & g), t1 = hh + S1 + ch + K256[i] + w[i], S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj;
        hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}
void lagfx_reduce_sha16(const void *data, size_t bytes, char out[17]) {
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const uint8_t *p = data;
    size_t n = bytes;
    while (n >= 64) { sha_block(h, p); p += 64, n -= 64; }
    uint8_t t[128] = { 0 };
    memcpy(t, p, n);
    t[n] = 0x80;
    size_t tl = n < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)bytes * 8;
    for (int i = 0; i < 8; i++) t[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha_block(h, t);
    if (tl == 128) sha_block(h, t + 64);
    snprintf(out, 17, "%08x%08x", h[0], h[1]);
}

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6; }

/* ---------------------------------------------------------------- building one candidate */
static const char *vkname(int r) {
    switch (r) {
    case 0: return "OK";
    case -13: return "VK_ERROR_UNKNOWN";
    case -1: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case -2: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case -3: return "VK_ERROR_INITIALIZATION_FAILED";
    case LRS_VK_NODEVICE: return "NO_DEVICE_IN_CHILD";
    case 1000297000: return "VK_PIPELINE_COMPILE_REQUIRED";
    default: return "VK_ERROR_(other)";
    }
}

typedef struct { const uint32_t *vs, *fs; size_t vsn, fsn; uint32_t flags; } cand_t;
enum { CF_NOFRAG = 1, CF_OPT_OFF = 2, CF_FAILCR = 4 };

static VkResult build_cand(VkDevice dev, const lagfx_pipeline_desc_t *dsrc, VkPipelineLayout layout, const cand_t *c, const char *fe) {
    lagfx_pipeline_desc_t d = *dsrc;
    VkShaderModule vm = VK_NULL_HANDLE, fm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = c->vsn, .pCode = c->vs };
    VkResult r = vkCreateShaderModule(dev, &ci, NULL, &vm);
    if (r == VK_SUCCESS && !(c->flags & CF_NOFRAG)) { ci.codeSize = c->fsn, ci.pCode = c->fs; r = vkCreateShaderModule(dev, &ci, NULL, &fm); }
    if (r == VK_SUCCESS) {
        d.vertex_shader = vm, d.fragment_shader = fm, d.layout = layout, d.vertex_entry_point = "main", d.fragment_entry_point = fe ? fe : "main";
        d.vertex_spv = c->vs, d.vertex_spv_bytes = c->vsn, d.fragment_spv = c->fs, d.fragment_spv_bytes = c->fsn, d.diag = NULL, d.diag_gpci_pnext = NULL, d.diag_render_pass = 0;
        d.diag_no_fragment = (c->flags & CF_NOFRAG) ? 1 : 0;
        d.diag_create_flags = (c->flags & CF_OPT_OFF) ? 1u : (c->flags & CF_FAILCR) ? 0x100u : 0u;
        VkPipeline pl = VK_NULL_HANDLE;
        lagfx_pipeline_bundle_suppress(1);
        lagfx_pipeline_build_quiet(1);
        lagfx_status_t st = lagfx_pipeline_build(dev, &d, &pl);
        lagfx_pipeline_build_quiet(0);
        lagfx_pipeline_bundle_suppress(0);
        r = st == LAGFX_OK ? VK_SUCCESS : (VkResult)lagfx_pipeline_build_last_result();
        if (st != LAGFX_OK && r == VK_SUCCESS) r = VK_ERROR_UNKNOWN;
        if (pl) vkDestroyPipeline(dev, pl, NULL);
    }
    if (vm) vkDestroyShaderModule(dev, vm, NULL);
    if (fm) vkDestroyShaderModule(dev, fm, NULL);
    return r;
}

#ifndef LAGFX_NO_PROCESS /* Henge/Android runner: no process creation inside the library (fork); the app starts one process per row instead (--only) */
/* the isolated child: its OWN instance / device / layout (fork happens before the parent creates any) */
typedef struct { int32_t vk; double ms; } child_msg_t;
static void child_run(const lagfx_reduce_target_t *t, const cand_t *c, const char *row, int fd) {
    child_msg_t m = { LRS_VK_NODEVICE, 0 };
    const char *crash = getenv("LAGFX_PIPELINE_REDUCE_CRASH_ROW"), *hang = getenv("LAGFX_PIPELINE_REDUCE_HANG_ROW");
    if (crash && !strcmp(crash, row)) raise(SIGSEGV);
    if (hang && !strcmp(hang, row)) for (;;) pause();
    struct lagfx_vk_state *vk = NULL;
    lagfx_device_descriptor_t dd;
    memset(&dd, 0, sizeof dd);
    if (lagfx_vk_init(&vk, &dd) == LAGFX_OK && vk && vk->initialized) {
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        VkPipelineLayout pl = vk->empty_layout;
        VkResult lr = VK_SUCCESS;
        if (t->nbind) {
            VkDescriptorSetLayoutCreateInfo dl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = t->nbind, .pBindings = t->bind };
            lr = vkCreateDescriptorSetLayout(vk->device, &dl, NULL, &dsl);
            if (lr == VK_SUCCESS) {
                VkPipelineLayoutCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = t->npush, .pPushConstantRanges = t->push };
                lr = vkCreatePipelineLayout(vk->device, &pi, NULL, &pl);
            }
        }
        double t0 = now_ms();
        m.vk = lr == VK_SUCCESS ? (int32_t)build_cand(vk->device, &t->d0, pl, c, t->fs_entry) : (int32_t)lr;
        m.ms = now_ms() - t0;
        if (pl && pl != vk->empty_layout) vkDestroyPipelineLayout(vk->device, pl, NULL);
        if (dsl) vkDestroyDescriptorSetLayout(vk->device, dsl, NULL);
        lagfx_vk_shutdown(vk);
    }
    if (write(fd, &m, sizeof m) != (ssize_t)sizeof m) _exit(3);
    _exit(0);
}

/* evaluates a candidate; returns LRS_BUILT / FAIL / CRASH / TIMEOUT; *vk = VkResult (or the signal for CRASH) */
static int eval_isolated(const lagfx_reduce_cfg_t *cfg, const char *row, const cand_t *c, int32_t *vk, double *ms) {
    int fds[2];
    if (pipe(fds)) { *vk = errno; return LRS_CRASH; }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); *vk = errno; return LRS_CRASH; }
    if (pid == 0) { close(fds[0]); child_run(cfg->target, c, row, fds[1]); _exit(4); }
    close(fds[1]);
    const double deadline = now_ms() + (cfg->timeout_ms ? cfg->timeout_ms : 20000u);
    child_msg_t m;
    size_t got = 0;
    int timed_out = 0;
    for (;;) {
        struct pollfd pf = { fds[0], POLLIN, 0 };
        double left = deadline - now_ms();
        if (left <= 0) { timed_out = 1; break; }
        int pr = poll(&pf, 1, left > 200 ? 200 : (int)left + 1);
        if (pr > 0) { ssize_t n = read(fds[0], (char *)&m + got, sizeof m - got); if (n <= 0) break; got += (size_t)n; if (got == sizeof m) break; }
        else if (pr < 0 && errno != EINTR) break;
    }
    if (timed_out) kill(pid, SIGKILL);
    close(fds[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (timed_out) { *vk = 0; *ms = (double)(cfg->timeout_ms ? cfg->timeout_ms : 20000u); return LRS_TIMEOUT; }
    if (WIFSIGNALED(st)) { *vk = WTERMSIG(st); return LRS_CRASH; }
    if (got != sizeof m) { *vk = WIFEXITED(st) ? -WEXITSTATUS(st) : 0; return LRS_CRASH; }
    *vk = m.vk, *ms = m.ms;
    return m.vk == 0 ? LRS_BUILT : LRS_FAIL;
}
#else
static int eval_isolated(const lagfx_reduce_cfg_t *cfg, const char *row, const cand_t *c, int32_t *vk, double *ms) {
    (void)cfg; (void)row; (void)c; (void)ms;
    *vk = -1;
    return LRS_CRASH;   /* unreachable: pipeline_repro clears the ISOLATE flag in this build */
}
#endif

/* ---------------------------------------------------------------- the ladder */
#define NF 11
typedef struct {
    const lagfx_reduce_cfg_t *cfg;
    lagfx_red_info_t *info;
    uint64_t fh;
    char sha_orig[17];
    uint32_t rows, built, fail, crash, timeout, invalid, na, lines;
    char ids[6][1100];
    uint32_t smallest, only_n;
    char smallest_id[64];
    int r0, from_seen, adaptive_left, truncated;
    int fd_log;
    char only[1200];
    struct { char fs[17], vs[17], id[48]; } seen[512];   /* fragment + vertex module of every row built so far in this run; rows of the second-round group (x) that repeat one are reported n/a, not run */
    uint32_t nseen;
    int dedupe;
} L_t;

static void add_id(char *list, size_t cap, const char *id) {
    size_t n = strlen(list);
    if (n + strlen(id) + 2 >= cap) { if (n + 4 < cap) strcat(list, ",.."); return; }
    if (n) list[n++] = ',';
    strcpy(list + n, id);
}
static void emit_line(L_t *l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void emit_line(L_t *l, const char *fmt, ...) {
    char buf[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    l->lines++;
    if (l->fd_log >= 0) { size_t n = strlen(buf); if (write(l->fd_log, buf, n) < 0 || write(l->fd_log, "\n", 1) < 0) {} fsync(l->fd_log); }
    if (l->cfg->emit) l->cfg->emit(l->cfg->user, buf);
}

static int id_in_list(const char *list, const char *id) {
    size_t n = strlen(id);
    for (const char *p = list; *p;) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !strncmp(p, id, n)) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}
/* does the row run? (--only / --from filters) */
static int selected(L_t *l, const char *id) {
    if (l->cfg->only && l->cfg->only[0]) return id_in_list(l->cfg->only, id);
    if (l->cfg->from && l->cfg->from[0]) { if (!l->from_seen && !strcmp(l->cfg->from, id)) l->from_seen = 1; return l->from_seen; }
    return 1;
}
static int group_on(L_t *l, char g) { const char *s = l->cfg->groups; return !s || !s[0] || strchr(s, g) != NULL; }

static void sanitize(const char *id, char *o, size_t cap) {
    size_t n = 0;
    for (; *id && n + 1 < cap; id++) o[n++] = (char)(((*id >= '0' && *id <= '9') || (*id >= 'a' && *id <= 'z') || (*id >= 'A' && *id <= 'Z') || *id == '-') ? *id : '_');
    o[n] = 0;
}

/* One row. Returns LRS_*. fs/fsn = the candidate module (NULL = not applicable, `why` says why). vs: NULL = stub. */
static int do_row(L_t *l, const char *id, const char *what, const char *meaning, const uint32_t *vs, size_t vsn, uint32_t flags, const uint32_t *fs, size_t fsn, const char *why, int announce_only) {
    const lagfx_reduce_cfg_t *cfg = l->cfg;
    char sha[17] = "-";
    char selfwhy[160] = "";
    if (cfg->list_only) { emit_line(l, "pipeline-reduce: id %s", id); return LRS_NA; }
    if (!selected(l, id)) return LRS_NA;
    if (l->rows >= (cfg->max_rows ? cfg->max_rows : 400u)) { if (!l->truncated) { l->truncated = 1; emit_line(l, "pipeline-reduce: row limit reached (LAGFX_PIPELINE_REDUCE_MAX=%u): the remaining rows are NOT run", cfg->max_rows ? cfg->max_rows : 400u); } return LRS_NA; }
    (void)announce_only;
    l->rows++;
    if (!fs) {
        l->na++;
        add_id(l->ids[4], sizeof l->ids[0], id);
        emit_line(l, "pipeline-reduce: row=%s result=n/a vk=- ms=0 bytes=- inst=- sha=- orig=%s val=- [%s] %s: %s", id, l->sha_orig, meaning, what, why ? why : "not applicable");
        return LRS_NA;
    }
    lagfx_reduce_sha16(fs, fsn, sha);
    char vsha[17] = "stub";
    if (vs) lagfx_reduce_sha16(vs, vsn, vsha);
    if (l->dedupe) for (uint32_t q = 0; q < l->nseen; q++) if (!strcmp(l->seen[q].fs, sha) && !strcmp(l->seen[q].vs, vsha) && !(flags & (CF_NOFRAG | CF_OPT_OFF | CF_FAILCR))) {
        l->na++;
        add_id(l->ids[4], sizeof l->ids[0], id);
        emit_line(l, "pipeline-reduce: row=%s result=n/a vk=- ms=0 bytes=%zu inst=%u sha=%s orig=%s val=- [%s] %s: identical fragment module and vertex stage (%s) to row %s, not run again", id, fsn, lagfx_red_count_inst(fs, fsn / 4), sha, l->sha_orig, meaning, what, vs ? "real/generated VS" : "stub VS", l->seen[q].id);
        return LRS_NA;
    }
    if (l->nseen < 512 && !(flags & (CF_NOFRAG | CF_OPT_OFF | CF_FAILCR))) { snprintf(l->seen[l->nseen].fs, 17, "%s", sha), snprintf(l->seen[l->nseen].vs, 17, "%s", vsha), snprintf(l->seen[l->nseen].id, 48, "%s", id), l->nseen++; }
    uint32_t ninst = lagfx_red_count_inst(fs, fsn / 4);
    int status;
    int32_t vk = 0;
    double ms = 0;
    if (!(flags & CF_NOFRAG) && !lagfx_red_selfcheck(fs, fsn / 4, selfwhy, sizeof selfwhy)) {
        status = LRS_INVALID, vk = 0;
        l->invalid++;
        add_id(l->ids[5], sizeof l->ids[0], id);
        emit_line(l, "pipeline-reduce: row=%s result=INVALID vk=- ms=0 bytes=%zu inst=%u sha=%s orig=%s val=selfcheck-failed [%s] %s: %s (not built)", id, fsn, ninst, sha, l->sha_orig, meaning, what, selfwhy);
        return status;
    }
    if (cfg->dump_dir && cfg->dump_dir[0]) {
        char san[96], path[1200];
        sanitize(id, san, sizeof san);
        snprintf(path, sizeof path, "%s/%016llx.%s.fs.spv", cfg->dump_dir, (unsigned long long)l->fh, san);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(fs, 1, fsn, f); fclose(f); }
        snprintf(path, sizeof path, "%s/manifest.tsv", cfg->dump_dir);
        f = fopen(path, "ab");
        if (f) { fprintf(f, "%016llx\t%s\t%s\t%zu\t%u\t%s\t%s\t%s\n", (unsigned long long)l->fh, id, sha, fsn, ninst, meaning, what, flags ? "stage-flags" : "-"); fclose(f); }
    }
    cand_t c = { .vs = vs ? vs : lagfx_stub_vs_spv, .vsn = vs ? vsn : sizeof lagfx_stub_vs_spv, .fs = fs, .fsn = fsn, .flags = flags };
    if (cfg->dry) { status = LRS_FAIL, vk = -13, ms = 0; }
    else if (cfg->isolate) status = eval_isolated(cfg, id, &c, &vk, &ms);
    else {
        double t0 = now_ms();
        VkResult r = build_cand(cfg->dev, &cfg->target->d0, cfg->layout, &c, cfg->target->fs_entry);
        ms = now_ms() - t0;
        vk = (int32_t)r, status = r == VK_SUCCESS ? LRS_BUILT : LRS_FAIL;
    }
    if (cfg->test_override && (status == LRS_BUILT || status == LRS_FAIL)) {
        VkResult r = cfg->test_override(cfg->user, id, fs, fsn, (VkResult)vk);
        vk = (int32_t)r, status = r == VK_SUCCESS ? LRS_BUILT : LRS_FAIL;
    }
    const char *sn = status == LRS_BUILT ? "BUILT" : status == LRS_FAIL ? "FAIL" : status == LRS_CRASH ? "CRASH" : "TIMEOUT";
    char vkb[96];
    if (status == LRS_CRASH) snprintf(vkb, sizeof vkb, "signal=%d", vk);
    else if (status == LRS_TIMEOUT) snprintf(vkb, sizeof vkb, "timeout");
    else snprintf(vkb, sizeof vkb, "%d(%s)", vk, vkname(vk));
    l->built += status == LRS_BUILT, l->fail += status == LRS_FAIL, l->crash += status == LRS_CRASH, l->timeout += status == LRS_TIMEOUT;
    add_id(l->ids[status == LRS_BUILT ? 0 : status == LRS_FAIL ? 1 : status == LRS_CRASH ? 2 : 3], sizeof l->ids[0], id);
    if (status == LRS_FAIL && (!l->smallest || fsn < l->smallest)) l->smallest = (uint32_t)fsn, snprintf(l->smallest_id, sizeof l->smallest_id, "%s", id);
    emit_line(l, "pipeline-reduce: row=%s result=%s vk=%s ms=%.1f bytes=%zu inst=%u sha=%s orig=%s val=offline [%s] %s", id, sn, vkb, ms, fsn, ninst, sha, l->sha_orig, meaning, what);
    return status;
}

void lagfx_pipeline_reduce_cfg_env(lagfx_reduce_cfg_t *cfg) {
    const char *e;
    if (!cfg->only && (e = getenv("LAGFX_PIPELINE_REDUCE_ONLY")) && e[0]) cfg->only = e;
    if (!cfg->from && (e = getenv("LAGFX_PIPELINE_REDUCE_FROM")) && e[0]) cfg->from = e;
    if (!cfg->groups && (e = getenv("LAGFX_PIPELINE_REDUCE_GROUPS")) && e[0]) cfg->groups = e;
    if (!cfg->max_rows && (e = getenv("LAGFX_PIPELINE_REDUCE_MAX")) && e[0]) cfg->max_rows = (uint32_t)atoi(e);
    if (!cfg->timeout_ms && (e = getenv("LAGFX_PIPELINE_REDUCE_TIMEOUT_MS")) && e[0]) cfg->timeout_ms = (uint32_t)atoi(e);
}

static const char *MEAN_P = "meaning-preserving";
static const char *MEAN_D = "diagnostic: meaning changed";
static const char *MEAN_O = "optimizer output (spirv-opt claims semantics-preserving; not re-checked)";

/* recipe -> row */
static int row_recipe(L_t *l, const char *id, const char *what, const lagfx_red_recipe_t *r, const uint32_t *vs, size_t vsn, uint32_t flags) {
    uint32_t *o = NULL;
    size_t on = 0;
    char info[480] = "";
    int preserving = !(r->bits & ~(LRD_DYN_CONST | LRD_DYN_SELECT | LRD_POST_MASK));
    for (uint32_t k = 0; k < LAGFX_RED_MAXFN; k++) if (r->stub[k]) preserving = 0;
    if (r->cut_blocks) preserving = 0;
    if (r->ssbo_len) preserving = 0;
    if (l->cfg->list_only || !selected(l, id)) { if (l->cfg->list_only) do_row(l, id, what, "", NULL, 0, 0, NULL, 0, NULL, 0); return LRS_NA; }
    int rc = lagfx_red_apply(l->cfg->fs, l->cfg->fs_bytes / 4, r, &o, &on, info, sizeof info);
    int st;
    if (rc == 1) { char what2[900]; snprintf(what2, sizeof what2, "%s | %s", what, info); st = do_row(l, id, what2, preserving ? MEAN_P : MEAN_D, vs, vsn, flags, o, on * 4, NULL, 0); }
    else st = do_row(l, id, what, preserving ? MEAN_P : MEAN_D, vs, vsn, flags, NULL, 0, rc == 0 ? info : "rewrite failed (out of memory / malformed)", 0);
    free(o);
    return st;
}

/* the cumulative-chain features */
static const char *FNAME[NF] = { "undef->null", "switch->default", "loops one-trip", "dyn-vector-access->select", "derivatives->null", "samples->null", "imageread->null+IA dropped", "helpers stubbed", "entry cut", "SSBO arrays->16", "FS input loads->null" };
static void recipe_from_mask(const L_t *l, uint32_t mask, lagfx_red_recipe_t *r) {
    memset(r, 0, sizeof *r);
    const lagfx_red_info_t *in = l->info;
    if (mask & 1u) r->undef_class = LRD_UC_NUM | LRD_UC_STRUCT;
    if (mask & 2u) r->bits |= LRD_SW_E | LRD_SW_H;
    if (mask & 4u) r->bits |= LRD_LP_E | LRD_LP_H;
    if (mask & 8u) r->bits |= LRD_DYN_SELECT;
    if (mask & 16u) r->bits |= LRD_DERIV_NULL;
    if (mask & 32u) r->bits |= LRD_SMP_NULL_E | LRD_SMP_NULL_H;
    if (mask & 64u) r->bits |= LRD_READ_NULL | LRD_IA_DROP;
    if (mask & 128u) for (uint32_t k = 0; k < in->nfn; k++) if (k != in->entry) r->stub[k] = 1;
    if (mask & 256u) r->bits |= LRD_CUT, r->cut_blocks = in->entry_blocks / 10 ? in->entry_blocks / 10 : 1;
    if (mask & 512u) r->bits |= LRD_SSBO_SHRINK;
    if (mask & 1024u) r->bits |= LRD_INPUT_CONST, r->input_sel = 0xffffffffu;
}
static void mask_text(uint32_t mask, char *o, size_t cap) {
    o[0] = 0;
    for (uint32_t k = 0; k < NF; k++) if (mask & (1u << k)) { size_t n = strlen(o); snprintf(o + n, cap - n, "%s%s", n ? "+" : "", FNAME[k]); }
    if (!o[0]) snprintf(o, cap, "(nothing)");
}
static int row_mask(L_t *l, const char *id, uint32_t mask) {
    lagfx_red_recipe_t r;
    char txt[400];
    recipe_from_mask(l, mask, &r);
    mask_text(mask, txt, sizeof txt);
    return row_recipe(l, id, txt, &r, NULL, 0, 0);
}

static int fails(int st) { return st == LRS_FAIL; }

static uint32_t parse_hex(const char *s) { return (uint32_t)strtoul(s, NULL, 16); }

static void f16txt(const lagfx_red_info_t *in, char *o, size_t cap) {
    static const char *nm[8] = { "f16", "f32", "f64", "i8", "i16", "i32", "i64", "other" };
    size_t n = 0;
    o[0] = 0;
    for (int i = 0; i < 8; i++) if (in->k[i].unique) n += (size_t)snprintf(o + n, cap - n, "%s=%u/%u/%u ", nm[i], in->k[i].unique, in->k[i].distinct_values, in->k[i].uses);
}

static void stats(L_t *l) {
    const lagfx_red_info_t *in = l->info;
    char kt[300];
    f16txt(in, kt, sizeof kt);
    emit_line(l, "pipeline-reduce: stats fs=%016llx bytes=%zu inst=%u bound=%u spirv=%u.%u functions=%u uncalled=%u entry_blocks=%u labels=%u fn_variables=%u variables_total=%u loads=%u stores=%u phi=%u coopmat_caps=%u switch=%u loop=%u selection_merge=%u branch=%u fconvert=%u ptr_access_chain=%u kill=%u",
              (unsigned long long)l->fh, l->cfg->fs_bytes, in->ninst, in->bound, (in->version >> 16) & 0xff, (in->version >> 8) & 0xff, in->nfn, in->nfn_uncalled, in->entry_blocks, in->nlabel, in->nfnvar, in->nvar_total, in->nload, in->nstore, in->nphi, in->ncoopmat_cap,
              in->nswitch, in->nloop, in->nselmerge, in->nbranch, in->nfconvert, in->nptrchain, in->nkill);
    emit_line(l, "pipeline-reduce: stats constants (unique/distinct-values/uses-in-functions by type) %s| bool=%u composite=%u spec=%u null=%u global-undef=%u uses_total=%u", kt, in->kbool, in->kcomposite, in->kspec, in->knull, in->kundef_global, in->kuses_total);
    char dv[300];
    size_t n = 0;
    dv[0] = 0;
    for (int q = 0; q < 8; q++) if (in->dyn_vec[q].n) n += (size_t)snprintf(dv + n, sizeof dv - n, "v%u%s%u=%u ", in->dyn_vec[q].ncomp, in->dyn_vec[q].kind ? "i" : "f", in->dyn_vec[q].width, in->dyn_vec[q].n);
    char top[200];
    n = 0, top[0] = 0;
    for (uint32_t k = 0; k < in->nfn; k++) if (in->dyn_fn[k] >= 20 && n < 150) n += (size_t)snprintf(top + n, sizeof top - n, "fn%u(id %u)=%u ", k, in->fn[k].id, in->dyn_fn[k]);
    emit_line(l, "pipeline-reduce: stats dynamic vector access (D1): total=%u index-folds-to-constant=%u truly-dynamic=%u index-source[const=%u load=%u other=%u] by type: %s| functions with >=20: %s", in->dyn_total, in->dyn_const, in->dyn_truly, in->dyn_idx_const, in->dyn_idx_load, in->dyn_idx_other, dv, top);
    char i64[160];
    n = 0, i64[0] = 0;
    for (int q = 0; q < 10; q++) if (in->i64_op[q].n) n += (size_t)snprintf(i64 + n, sizeof i64 - n, "op%u=%u ", in->i64_op[q].op, in->i64_op[q].n);
    emit_line(l, "pipeline-reduce: stats i64 result instructions by opcode (D5): %s| bitwise 8/16-bit=%u of %u bitwise | whole-struct load=%u store=%u copymem=%u | private vector arrays=%u (max length %u; Vita3K #4141 shape needs >=19) | ssbo big array: length=%u ArrayStride=%u member offsets=%u,%u",
              i64[0] ? i64 : "none ", in->nbit_small, in->nbit_total, in->nstruct_load, in->nstruct_store, in->ncopymem, in->nprivate_vecarr, in->nprivate_arr_max, in->ssbo_arr_len, in->ssbo_arr_stride, in->ssbo_offsets[0], in->ssbo_offsets[1]);
    char ud[400];
    n = 0, ud[0] = 0;
    for (uint32_t u = 0; u < in->nundef && n < 330; u++) n += (size_t)snprintf(ud + n, sizeof ud - n, "#%u(id %u,%s,uses %u) ", u, in->undef[u].id, in->undef[u].cls == 1 ? "numeric" : in->undef[u].cls == 2 ? "struct" : in->undef[u].cls == 4 ? "pointer" : "composite", in->undef[u].nuse);
    emit_line(l, "pipeline-reduce: stats OpUndef (global, declaration order): %s", ud);
    {
        static const char *kn[6] = { "?", "sampler", "image", "input-attachment", "buffer", "other" };
        char cb[300];
        size_t cn = 0;
        cb[0] = 0;
        for (uint32_t q = 0; q < in->ncollide && q < 4; q++) cn += (size_t)snprintf(cb + cn, sizeof cb - cn, "set %u binding %u: %s vs %s; ", in->collide[q].set, in->collide[q].binding, kn[in->collide[q].ka % 6], kn[in->collide[q].kb % 6]);
        emit_line(l, "pipeline-reduce: stats descriptor-binding collisions (variables of DIFFERENT kinds on one set+binding; the shared binding table (air2spv/binding_alloc.h) refuses such a module: no layout is derived from it): %u %s%s", in->ncollide, cb, in->ncxs ? "| the module has translator constexpr samplers (OpName lagfx.cxs.*)" : "");
    }
}

/* FS-input / VS-output audit (report only) */
static void audit(L_t *l, const lagfx_reduce_cfg_t *cfg) {
    lagfx_spv_iface_t *f = calloc(1, sizeof *f), *v = calloc(1, sizeof *v), *s = calloc(1, sizeof *s);
    if (f && v && s && lagfx_spv_iface_reflect(cfg->fs, cfg->fs_bytes / 4, 4, f)) {
        int rv = cfg->vs && lagfx_spv_iface_reflect(cfg->vs, cfg->vs_bytes / 4, 0, v);
        int rs = lagfx_spv_iface_reflect(lagfx_stub_vs_spv, sizeof lagfx_stub_vs_spv / 4, 0, s);
        uint32_t miss_r = 0, miss_s = 0, mis = 0;
        for (uint32_t i = 0; i < f->n_in; i++) {
            const lagfx_spv_io_t *fi = &f->in[i];
            if (!fi->has_loc) continue;
            char vr[80] = "no-real-VS", vs[80] = "MISSING";
            if (rv) {
                snprintf(vr, sizeof vr, "MISSING");
                for (uint32_t j = 0; j < v->n_out; j++) {
                    const lagfx_spv_io_t *vo = &v->out[j];
                    if (!vo->has_loc || vo->loc != fi->loc) continue;
                    if (vo->kind != fi->kind || vo->width != fi->width || vo->ncomp < fi->ncomp || vo->comp != fi->comp || vo->flat != fi->flat) { snprintf(vr, sizeof vr, "MISMATCH(vs kind%u w%u x%u comp%u flat%d)", vo->kind, vo->width, vo->ncomp, vo->comp, vo->flat); mis++; }
                    else snprintf(vr, sizeof vr, "match");
                    break;
                }
                if (!strcmp(vr, "MISSING")) miss_r++;
            }
            miss_s += rs;
            emit_line(l, "pipeline-reduce: audit fs-input loc=%u comp=%u kind=%u width=%u x%u flat=%d  real-VS=%s  stub-VS=%s", fi->loc, fi->comp, fi->kind, fi->width, fi->ncomp, fi->flat, vr, vs);
        }
        emit_line(l, "pipeline-reduce: audit summary fs-inputs-with-location=%u missing-in-real-VS=%u mismatched-in-real-VS=%u missing-in-stub-VS=%u (report only; the d2 / reduce rows use the stub VS unless marked real-VS)", f->n_in, miss_r, mis, miss_s);
    }
    free(f), free(v), free(s);
}

static const char *FILE_NAMES[] = { "ssa", "local-single-block", "local-single-store", "local-multi-store", "scalar", "inline", "deadfn", "dce", "merge", "deadbr", "unify", "strip", "O", "Os", "locals", "reenc", "asm", "hlsl", "ccp", "simplify", "redundancy", "local-redundancy", "u2", "u3", "u5" };
#define FILE_FIRST_U 22   /* index of "u2": the U-chain files are diagnostic (switch rewritten), the ones before are spirv-opt passes of the original */
static const char *FILE_WHAT[] = { "spirv-opt --ssa-rewrite (Function variables -> SSA, adds OpPhi)", "spirv-opt --eliminate-local-single-block", "spirv-opt --eliminate-local-single-store", "spirv-opt --eliminate-local-multi-store",
    "spirv-opt --scalar-replacement=100", "spirv-opt --inline-entry-points-exhaustive", "spirv-opt --eliminate-dead-functions", "spirv-opt --eliminate-dead-code-aggressive", "spirv-opt --merge-blocks", "spirv-opt --eliminate-dead-branches",
    "spirv-opt --unify-const (U4: fewer unique constants)", "spirv-opt --strip-debug", "spirv-opt -O", "spirv-opt -Os", "spirv-opt --scalar-replacement=100 --ssa-rewrite --eliminate-dead-code-aggressive (fewer locals)",
    "spirv-opt with no pass (producer re-encode)", "spirv-dis -> spirv-as round trip", "spirv-opt --legalize-hlsl", "spirv-opt --ccp (constant propagation)", "spirv-opt --simplify-instructions", "spirv-opt --redundancy-elimination", "spirv-opt --local-redundancy-elimination", "U2: U1 (switch -> most frequent case) then --eliminate-dead-branches --eliminate-dead-code-aggressive",
    "U3: U2 then --eliminate-dead-functions", "U5: U1 then --unify-const" };

static uint32_t *slurp(const char *path, size_t *bytes) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *w = n > 0 && n % 4 == 0 && n <= (16l << 20) ? malloc((size_t)n) : NULL;
    if (!w || fread(w, 1, (size_t)n, f) != (size_t)n) { free(w); w = NULL; }
    fclose(f);
    if (w) *bytes = (size_t)n;
    return w;
}

void lagfx_pipeline_reduce_run(const lagfx_reduce_cfg_t *cfg, lagfx_reduce_result_t *res) {
    lagfx_reduce_result_t rs;
    memset(&rs, 0, sizeof rs);
    if (res) *res = rs;
    if (!cfg || !cfg->fs || !cfg->fs_bytes || (cfg->fs_bytes & 3u)) return;
    L_t *l = calloc(1, sizeof *l);
    lagfx_red_info_t *info = calloc(1, sizeof *info);
    if (!l || !info) { free(l); free(info); return; }
    l->cfg = cfg, l->info = info, l->fd_log = -1;
    l->fh = lagfx_pipeline_spv_hash(cfg->fs, cfg->fs_bytes);
    lagfx_reduce_sha16(cfg->fs, cfg->fs_bytes, l->sha_orig);
    const char *lp = getenv("LAGFX_PIPELINE_REDUCE_LOG");
    if (lp && lp[0] && !cfg->list_only) l->fd_log = open(lp, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (!lagfx_red_analyze(cfg->fs, cfg->fs_bytes / 4, info)) {
        emit_line(l, "pipeline-reduce: fs=%016llx cannot be analysed (malformed or more than %d functions): ladder not run", (unsigned long long)l->fh, LAGFX_RED_MAXFN);
        goto out;
    }
    emit_line(l, "pipeline-reduce: begin fs=%016llx sha=%s bytes=%zu vs=%s isolate=%d dry=%d groups=%s only=%s from=%s. BUILT = pipeline creation returned VK_SUCCESS on the device that wrote this log (says nothing about rendering correctness; a lavapipe BUILT says nothing about Adreno). val=offline: look the sha up in manifest.tsv",
              (unsigned long long)l->fh, l->sha_orig, cfg->fs_bytes, cfg->vs ? "real+stub" : "stub", cfg->isolate, cfg->dry, cfg->groups && cfg->groups[0] ? cfg->groups : "all", cfg->only && cfg->only[0] ? cfg->only : "-", cfg->from && cfg->from[0] ? cfg->from : "-");
    if (!cfg->list_only) { stats(l); audit(l, cfg); }
    l->adaptive_left = (int)(cfg->max_adaptive ? cfg->max_adaptive : 24u);
    const uint32_t nfn = info->nfn, ent = info->entry;
    const uint32_t *rvs = cfg->vs;
    const size_t rvsn = cfg->vs_bytes;

    /* R0 */
    {
        int st = do_row(l, "R0", "original fragment module, stub vertex stage (= the d2 row)", MEAN_P, NULL, 0, 0, cfg->fs, cfg->fs_bytes, NULL, 0);
        l->r0 = st == LRS_NA ? LRS_FAIL : st;   /* a filtered-out R0 is assumed to FAIL (the bundle recorded -13) */
        if (st == LRS_BUILT && !cfg->list_only) emit_line(l, "pipeline-reduce: note R0 BUILT here: the original module does not fail in this process; the rows below are informative only and the adaptive searches are skipped");
    }
    if (rvs) do_row(l, "R0V", "ORIGINAL pair: original fragment module + REAL vertex stage (the pair that was recorded as failing; R0 uses a stub VS and can fail on interface linkage instead)", MEAN_P, rvs, rvsn, 0, cfg->fs, cfg->fs_bytes, NULL, 0);
    const int r0fail = l->r0 == LRS_FAIL || l->r0 == LRS_CRASH;
    lagfx_red_recipe_t r;
    char what[500];

    if (group_on(l, 'h')) {
        memset(&r, 0, sizeof r);
        for (uint32_t k = 0; k < nfn; k++) if (k != ent) r.stub[k] = 1;
        int hall = row_recipe(l, "HALL", "every helper function body -> return OpConstantNull (entry untouched)", &r, NULL, 0, 0);
        uint32_t nh = nfn - 1;
        memset(&r, 0, sizeof r);
        for (uint32_t k = 0, c = 0; k < nfn; k++) if (k != ent) { if (c++ < nh / 2) r.stub[k] = 1; }
        row_recipe(l, "HA", "first half of the helpers stubbed", &r, NULL, 0, 0);
        memset(&r, 0, sizeof r);
        for (uint32_t k = 0, c = 0; k < nfn; k++) if (k != ent) { if (c++ >= nh / 2) r.stub[k] = 1; }
        row_recipe(l, "HB", "second half of the helpers stubbed", &r, NULL, 0, 0);
        memset(&r, 0, sizeof r);
        for (uint32_t k = 0; k < nfn; k++) if (k != ent && info->fn[k].nsamp) r.stub[k] = 1;
        row_recipe(l, "HSMP", "helpers that sample a texture stubbed", &r, NULL, 0, 0);
        memset(&r, 0, sizeof r);
        for (uint32_t k = 0; k < nfn; k++) if (k != ent && (info->fn[k].nloop || info->fn[k].nswitch)) r.stub[k] = 1;
        row_recipe(l, "HCF", "helpers with a loop or switch stubbed", &r, NULL, 0, 0);
        memset(&r, 0, sizeof r);
        { uint32_t big = ent == 0 ? 1 : 0; for (uint32_t k = 0; k < nfn; k++) if (k != ent && info->fn[k].ninst > info->fn[big].ninst) big = k; r.stub[big] = 1; snprintf(what, sizeof what, "the largest helper (fn %u id %u, %u instructions) stubbed", big, info->fn[big].id, info->fn[big].ninst); }
        row_recipe(l, "HBIG", what, &r, NULL, 0, 0);
        memset(&r, 0, sizeof r);
        for (uint32_t k = 0; k < nfn; k++) if (k != ent && info->fn[k].ninst <= 60) r.stub[k] = 1;
        row_recipe(l, "HSMALL", "helpers of <= 60 instructions stubbed", &r, NULL, 0, 0);
        /* adaptive 1-minimal search (ddmin) over the helpers that must stay for the failure: only when R0 fails and HALL builds */
        if (r0fail && fails(hall) == 0 && hall == LRS_BUILT && !cfg->list_only && !(cfg->only && cfg->only[0])) {
            uint32_t cur[LAGFX_RED_MAXFN], ncur = 0;
            for (uint32_t k = 0; k < nfn; k++) if (k != ent) cur[ncur++] = k;
            uint32_t gran = 2;
            while (ncur >= 2 && l->adaptive_left > 0) {
                uint32_t chunk = (ncur + gran - 1) / gran;
                int reduced = 0;
                for (uint32_t c0 = 0; c0 < ncur && !reduced && l->adaptive_left > 0; c0 += chunk) {   /* try each chunk alone */
                    uint32_t c1 = c0 + chunk < ncur ? c0 + chunk : ncur, keep[LAGFX_RED_MAXFN], nk = 0;
                    for (uint32_t i = c0; i < c1; i++) keep[nk++] = cur[i];
                    memset(&r, 0, sizeof r);
                    for (uint32_t k = 0; k < nfn; k++) if (k != ent) r.stub[k] = 1;
                    for (uint32_t i = 0; i < nk; i++) r.stub[keep[i]] = 0;
                    char id[80];
                    uint32_t bits[4] = { 0 };
                    for (uint32_t i = 0; i < nk; i++) bits[keep[i] / 32] |= 1u << (keep[i] % 32);
                    snprintf(id, sizeof id, "HK:%x.%x.%x.%x", bits[3], bits[2], bits[1], bits[0]);
                    snprintf(what, sizeof what, "ddmin: only %u helper(s) kept (indices %u..), the rest stubbed", nk, keep[0]);
                    l->adaptive_left--;
                    if (fails(row_recipe(l, id, what, &r, NULL, 0, 0))) { memcpy(cur, keep, nk * 4); ncur = nk; gran = 2; reduced = 1; }
                }
                if (!reduced && gran > 2 && l->adaptive_left > 0) for (uint32_t c0 = 0; c0 < ncur && !reduced && l->adaptive_left > 0; c0 += chunk) {   /* complements */
                    uint32_t c1 = c0 + chunk < ncur ? c0 + chunk : ncur, keep[LAGFX_RED_MAXFN], nk = 0;
                    for (uint32_t i = 0; i < ncur; i++) if (i < c0 || i >= c1) keep[nk++] = cur[i];
                    memset(&r, 0, sizeof r);
                    for (uint32_t k = 0; k < nfn; k++) if (k != ent) r.stub[k] = 1;
                    for (uint32_t i = 0; i < nk; i++) r.stub[keep[i]] = 0;
                    char id[80];
                    uint32_t bits[4] = { 0 };
                    for (uint32_t i = 0; i < nk; i++) bits[keep[i] / 32] |= 1u << (keep[i] % 32);
                    snprintf(id, sizeof id, "HK:%x.%x.%x.%x", bits[3], bits[2], bits[1], bits[0]);
                    snprintf(what, sizeof what, "ddmin: %u helper(s) kept (complement of a chunk)", nk);
                    l->adaptive_left--;
                    if (fails(row_recipe(l, id, what, &r, NULL, 0, 0))) { memcpy(cur, keep, nk * 4); ncur = nk; if (gran > 2) gran--; reduced = 1; }
                }
                if (!reduced) { if (gran >= ncur) break; gran = gran * 2 > ncur ? ncur : gran * 2; }
            }
            char list[300];
            size_t n = 0;
            list[0] = 0;
            for (uint32_t i = 0; i < ncur && n < 250; i++) n += (size_t)snprintf(list + n, sizeof list - n, "%u(id %u) ", cur[i], info->fn[cur[i]].id);
            emit_line(l, "pipeline-reduce: ddmin result: helpers that must stay for the failure (everything else stubbed): %zu: %s| %s", (size_t)ncur, list, l->adaptive_left > 0 ? "1-minimal w.r.t. the rows tried" : "row budget exhausted: NOT proven minimal");
        } else if (r0fail && hall == LRS_FAIL) emit_line(l, "pipeline-reduce: note HALL FAILs: the helper bodies are not needed for the failure (entry function + declarations are enough)");
    }
    if (group_on(l, 's')) {
        memset(&r, 0, sizeof r);
        r.bits = LRD_SMP_NULL_E | LRD_SMP_NULL_H;
        row_recipe(l, "SMPN", "every OpImageSample/Fetch/Gather -> OpConstantNull", &r, NULL, 0, 0);
        r.bits = LRD_SMP_NULL_E;
        row_recipe(l, "SMPN-E", "samples in the entry function -> null", &r, NULL, 0, 0);
        r.bits = LRD_SMP_NULL_H;
        row_recipe(l, "SMPN-H", "samples in helper functions -> null", &r, NULL, 0, 0);
        r.bits = LRD_SMP_LOD0;
        row_recipe(l, "LOD0", "every implicit-lod sample -> ExplicitLod with Lod 0.0 (Bias/MinLod dropped)", &r, NULL, 0, 0);
        r.bits = LRD_DERIV_NULL;
        row_recipe(l, "DERIV", "OpDPdx/OpDPdy/OpFwidth (+Fine/Coarse) -> null", &r, NULL, 0, 0);
        r.bits = LRD_READ_NULL;
        row_recipe(l, "READ", "the framebuffer-fetch OpImageRead -> null (input attachment variables kept)", &r, NULL, 0, 0);
        r.bits = LRD_IA_DROP;
        row_recipe(l, "IADROP", "OpImageRead -> null AND the input-attachment variables + loads removed (bindings 40/41 vanish from the module)", &r, NULL, 0, 0);
    }
    if (group_on(l, 'c')) {
        memset(&r, 0, sizeof r);
        r.bits = LRD_SW_E | LRD_SW_H;
        row_recipe(l, "SW", "every OpSwitch -> OpBranch default (its OpSelectionMerge dropped)", &r, NULL, 0, 0);
        r.bits = LRD_SW_E;
        row_recipe(l, "SW-E", "switch in the entry function -> default", &r, NULL, 0, 0);
        r.bits = LRD_SW_H;
        row_recipe(l, "SW-H", "switches in helpers -> default", &r, NULL, 0, 0);
        r.bits = LRD_SWC_E | LRD_SWC_H;
        row_recipe(l, "U1", "every OpSwitch -> its statically most frequent case target (no run-time frequency data offline)", &r, NULL, 0, 0);
        r.bits = LRD_LP_E | LRD_LP_H;
        row_recipe(l, "LP", "every loop runs ONE trip (back-edge -> OpBranchConditional false)", &r, NULL, 0, 0);
        r.bits = LRD_LP_E;
        row_recipe(l, "LP-E", "loops of the entry function one trip", &r, NULL, 0, 0);
        r.bits = LRD_LP_H;
        row_recipe(l, "LP-H", "loops of helpers one trip", &r, NULL, 0, 0);
        static const uint32_t div[6] = { 64, 16, 8, 4, 2, 1 };   /* entry blocks / div * (1..) : 1/64 .. 3/4 */
        static const uint32_t mul[6] = { 1, 1, 1, 1, 1, 3 }, dv[6] = { 64, 16, 8, 4, 2, 4 };
        (void)div;
        uint32_t prevnb = 0;
        for (int i = 0; i < 6; i++) {
            uint32_t nb = info->entry_blocks * mul[i] / dv[i];
            char id[24];
            if ((nb ? nb : 1) == prevnb) continue;
            prevnb = nb ? nb : 1;
            memset(&r, 0, sizeof r);
            r.bits = LRD_CUT, r.cut_blocks = nb ? nb : 1;
            snprintf(id, sizeof id, "CUT%u", r.cut_blocks);
            snprintf(what, sizeof what, "entry function cut at its first block >= %u (of %u); open constructs keep one-instruction stubs; unreferenced helpers removed", r.cut_blocks, info->entry_blocks);
            row_recipe(l, id, what, &r, NULL, 0, 0);
        }
    }
    if (group_on(l, 'u')) {
        memset(&r, 0, sizeof r);
        r.undef_class = LRD_UC_NUM;
        row_recipe(l, "UN", "numeric (scalar/vector) OpUndef -> OpConstantNull (not struct, not pointer)", &r, NULL, 0, 0);
        r.undef_class = LRD_UC_STRUCT;
        row_recipe(l, "US", "struct OpUndef -> OpConstantNull", &r, NULL, 0, 0);
        r.undef_class = LRD_UC_PTR;
        row_recipe(l, "UP", "pointer OpUndef -> null (n/a: a Function-storage pointer has no valid null)", &r, NULL, 0, 0);
        for (uint32_t u = 0; u < info->nundef; u++) {
            char id[24];
            memset(&r, 0, sizeof r);
            r.undef_sel = 1u << u;
            snprintf(id, sizeof id, "UD%u", u);
            snprintf(what, sizeof what, "ONLY OpUndef #%u (id %u, %s, %u uses) -> OpConstantNull", u, info->undef[u].id, info->undef[u].cls == 1 ? "numeric" : info->undef[u].cls == 2 ? "struct" : "pointer", info->undef[u].nuse);
            row_recipe(l, id, what, &r, NULL, 0, 0);
        }
        static const struct { const char *id, *what; uint32_t cls; } uu[] = {
            { "UU-SHUF", "undef OPERANDS of OpVectorShuffle -> null of the same type (the undef itself stays)", LRD_UU_SHUF }, { "UU-INSDYN", "undef operands of OpVectorInsertDynamic -> null", LRD_UU_INSDYN },
            { "UU-CCOMP", "undef constituents of OpConstantComposite -> null", LRD_UU_CCOMP }, { "UU-CINS", "undef operands of OpCompositeInsert -> null", LRD_UU_CINS },
            { "UU-STORE", "undef values stored by OpStore -> null", LRD_UU_STORE }, { "UU-CALL", "undef function-call arguments -> null", LRD_UU_CALLARG } };
        for (size_t i = 0; i < sizeof uu / sizeof uu[0]; i++) { memset(&r, 0, sizeof r); r.use_class = uu[i].cls; row_recipe(l, uu[i].id, uu[i].what, &r, NULL, 0, 0); }
    }
    if (group_on(l, 'd')) {
        memset(&r, 0, sizeof r);
        r.bits = LRD_DYN_CONST;
        row_recipe(l, "DYN-C", "D2: dynamic vector access whose index folds to a constant -> OpCompositeExtract/Insert (value-preserving)", &r, NULL, 0, 0);
        r.bits = LRD_DYN_SELECT;
        row_recipe(l, "DYN-S", "D2+D3: also the truly dynamic accesses -> OpSelect chains (value-preserving for in-range indices)", &r, NULL, 0, 0);
        r.dyn_top = 1;
        row_recipe(l, "DYN-S1", "D4: the function with the most dynamic accesses only", &r, NULL, 0, 0);
        r.dyn_top = 4;
        row_recipe(l, "DYN-S4", "D4: the 4 functions with the most dynamic accesses only", &r, NULL, 0, 0);
    }
    if (group_on(l, 'b')) {
        static const struct { const char *id; uint32_t len; } sb[] = { { "S1", 16384 }, { "S2", 4096 }, { "S3", 1024 }, { "S4", 256 }, { "S0", 16 } };
        for (int i = 0; i < 5; i++) {
            memset(&r, 0, sizeof r);
            r.bits = LRD_SSBO_SHRINK, r.ssbo_len = sb[i].len;
            snprintf(what, sizeof what, "every array of >= 1024 elements -> %u elements (ArrayStride/Offsets untouched; creation-only: dynamic indices may exceed the bound)", sb[i].len);
            row_recipe(l, sb[i].id, what, &r, NULL, 0, 0);
        }
        memset(&r, 0, sizeof r);
        r.bits = LRD_SSBO_RT;
        row_recipe(l, "S6", "a big array that is only ever the LAST member of a StorageBuffer struct -> runtime array (only one per struct; others shrunk to 16)", &r, NULL, 0, 0);
        if (!cfg->list_only && selected(l, "S5")) emit_line(l, "pipeline-reduce: row=S5/S7/S8/S9 result=n/a: S5 (split into two SSBO bindings) needs a layout change the reduce hook cannot carry; S7 (half->float storage) changes every access type; S8/S9 (no accesses) need access removal across thousands of chains");
    }
    if (group_on(l, 'i')) {
        do_row(l, "IFA", "NO fragment stage + rasterizer discard, STUB vertex stage", MEAN_D, NULL, 0, CF_NOFRAG, cfg->fs, cfg->fs_bytes, NULL, 0);
        if (rvs) do_row(l, "IFB", "NO fragment stage + rasterizer discard, REAL vertex stage", MEAN_D, rvs, rvsn, CF_NOFRAG, cfg->fs, cfg->fs_bytes, NULL, 0);
        else do_row(l, "IFB", "NO fragment stage, REAL vertex stage", MEAN_D, NULL, 0, 0, NULL, 0, "no real vertex module given", 0);
        memset(&r, 0, sizeof r);
        r.bits = LRD_MIN_ENTRY;
        row_recipe(l, "IFC", "FS keeps every declaration/interface/input attachment, entry body = write null to the outputs; REAL VS", &r, rvs, rvsn, 0);
        row_recipe(l, "IFD", "same minimal FS, STUB VS", &r, NULL, 0, 0);
        r.bits = LRD_MIN_ENTRY | LRD_IA_DROP;
        row_recipe(l, "IFE", "minimal FS without the input attachments (interface otherwise kept); REAL VS", &r, rvs, rvsn, 0);
        {
            uint32_t *gv = NULL;
            size_t gn = 0;
            char gi[160] = "";
            int g = lagfx_red_make_vs_for_fs(cfg->fs, cfg->fs_bytes / 4, &gv, &gn, gi, sizeof gi);
            if (g == 1) do_row(l, "IFF", "ORIGINAL FS + a generated VS that writes every Location the FS reads", MEAN_D, gv, gn * 4, 0, cfg->fs, cfg->fs_bytes, NULL, 0);
            else do_row(l, "IFF", "ORIGINAL FS + generated mirror VS", MEAN_D, NULL, 0, 0, NULL, 0, gi[0] ? gi : "cannot mirror the inputs", 0);
            free(gv);
        }
        memset(&r, 0, sizeof r);
        r.bits = LRD_INPUT_CONST, r.input_sel = 0xffffffffu;
        row_recipe(l, "FI-ALL", "loads of every Location FS input -> OpConstantNull (the unmatched-input shape of Cemu #2029)", &r, NULL, 0, 0);
        for (uint32_t k = 0; k < info->nin && k < 12; k++) {
            char id[16];
            r.input_sel = 1u << k;
            snprintf(id, sizeof id, "FI%u", k);
            snprintf(what, sizeof what, "loads of FS input Location %u (kind %u x%u) -> null", info->in[k].loc, info->in[k].kind, info->in[k].ncomp);
            row_recipe(l, id, what, &r, NULL, 0, 0);
        }
        if (info->nin > 1) {
            r.input_sel = 0;
            for (uint32_t k = 0; k < info->nin / 2; k++) r.input_sel |= 1u << k;
            row_recipe(l, "FI-HALF1", "first half of the Location inputs -> null", &r, NULL, 0, 0);
            r.input_sel = 0;
            for (uint32_t k = info->nin / 2; k < info->nin; k++) r.input_sel |= 1u << k;
            row_recipe(l, "FI-HALF2", "second half of the Location inputs -> null", &r, NULL, 0, 0);
        }
    }
    if (group_on(l, 'f')) {
        do_row(l, "FL-NOOPT", "pipeline create flag DISABLE_OPTIMIZATION (0x1), original FS", MEAN_P, NULL, 0, CF_OPT_OFF, cfg->fs, cfg->fs_bytes, NULL, 0);
    }
    if (group_on(l, 'o')) {
        char missing[400] = "";
        uint32_t nmiss = 0;
        for (size_t i = 0; i < sizeof FILE_NAMES / sizeof FILE_NAMES[0]; i++) {
            char id[48], path[1200];
            size_t fb = 0;
            uint32_t *fw = NULL;
            snprintf(id, sizeof id, "F:%s", FILE_NAMES[i]);
            if (cfg->list_only) { do_row(l, id, "", "", NULL, 0, 0, NULL, 0, NULL, 0); continue; }
            if (!selected(l, id)) continue;
            const char *dirs[2] = { cfg->file_dir, cfg->file_dir2 };
            for (int d = 0; d < 2 && !fw; d++) if (dirs[d] && dirs[d][0]) { snprintf(path, sizeof path, "%s/%016llx.red.%s.fs.spv", dirs[d], (unsigned long long)l->fh, FILE_NAMES[i]); fw = slurp(path, &fb); }
            if (!fw) {   /* no file: a real n/a row (NOT silence: the Android runner runs one process per id and would otherwise report a row that printed nothing as a crash) */
                nmiss++;
                if (strlen(missing) < 330) { strcat(missing, FILE_NAMES[i]); strcat(missing, " "); }
                char why2[200];
                snprintf(why2, sizeof why2, "offline file %016llx.red.%s.fs.spv not found (no-op pass or not shipped): the row was NOT run", (unsigned long long)l->fh, FILE_NAMES[i]);
                do_row(l, id, FILE_WHAT[i], (i >= FILE_FIRST_U) ? MEAN_D : MEAN_O, NULL, 0, 0, NULL, 0, why2, 0);
                continue;
            }
            do_row(l, id, FILE_WHAT[i], (i >= FILE_FIRST_U) ? MEAN_D : MEAN_O, NULL, 0, 0, fw, fb, NULL, 0);
            free(fw);
        }
        if (nmiss && !cfg->list_only) emit_line(l, "pipeline-reduce: %u offline file row(s) absent (no %016llx.red.<name>.fs.spv in LAGFX_PIPELINE_MATRIX_DIR / bundle dir): %s(a pass whose output equals the input has no file by design: it is a no-op)", nmiss, (unsigned long long)l->fh, missing);
    }
    if (group_on(l, 'k')) {
        const uint32_t all = (1u << NF) - 1u;
        char id[48];
        if (!(cfg->only && cfg->only[0])) {
            uint32_t m = 0;
            for (uint32_t k = 0; k < NF; k++) { m |= 1u << k; snprintf(id, sizeof id, "CH%u:%x", k + 1, m); row_mask(l, id, m); }
            for (uint32_t k = 0; k < NF; k++) { snprintf(id, sizeof id, "RI%u:%x", k, all & ~(1u << k)); row_mask(l, id, all & ~(1u << k)); }
            if (r0fail && !cfg->list_only) {   /* greedy: keep a change only if the pipeline still FAILs */
                uint32_t g = 0;
                for (uint32_t k = 0; k < NF; k++) {
                    snprintf(id, sizeof id, "GR%u:%x", k, g | (1u << k));
                    int st = row_mask(l, id, g | (1u << k));
                    if (fails(st)) g |= 1u << k;
                }
                char txt[400];
                mask_text(g, txt, sizeof txt);
                emit_line(l, "pipeline-reduce: greedy chain result mask=%x: changes that still FAIL when applied cumulatively: %s", g, txt);
                for (uint32_t k = 0; k < NF; k++) if (g & (1u << k)) { snprintf(id, sizeof id, "RG%u:%x", k, g & ~(1u << k)); row_mask(l, id, g & ~(1u << k)); }
            }
        } else {   /* --only: the parameterised ids carry their mask */
            for (const char *p = cfg->only; *p;) {
                const char *e = strchr(p, ','), *c = strchr(p, ':');
                size_t len = e ? (size_t)(e - p) : strlen(p);
                if (c && c < p + len && (p[0] == 'C' || p[0] == 'R' || p[0] == 'G')) { char idb[48]; snprintf(idb, sizeof idb, "%.*s", (int)(len < 47 ? len : 47), p); row_mask(l, idb, parse_hex(c + 1)); }
                if (!e) break;
                p = e + 1;
            }
        }
    }
    if (group_on(l, 'x')) {   /* second round: rows that separate "size / structure" from "live input loads" (see docs/ in the Henge repo: reduce-round2). Offline-designed; all diagnostic. */
        static const uint32_t CH10 = 0x3ffu, CH11 = 0x7ffu;   /* the cumulative-chain masks of CH10 / CH11 */
        l->dedupe = 1;
        char id[64], loc[96];
        const uint32_t nin = info->nin < 6 ? info->nin : 6, nblt = info->nblt < 8 ? info->nblt : 8, fullin = nin ? (1u << nin) - 1u : 0, fullblt = nblt ? (1u << nblt) - 1u : 0;
#define LOCTXT(sel) do { size_t n_ = 0; loc[0] = 0; for (uint32_t k_ = 0; k_ < nin; k_++) if ((sel) >> k_ & 1u) n_ += (size_t)snprintf(loc + n_, sizeof loc - n_, "%s%u", n_ ? "," : "", info->in[k_].loc); } while (0)
        /* (a) per-input nulling on CH10: every proper subset of the Location inputs nulled (the full set is CH11, the empty set CH10) */
        if (nin > 1) for (uint32_t sel = 1; sel < fullin; sel++) {
            recipe_from_mask(l, CH10, &r);
            r.bits |= LRD_INPUT_CONST, r.input_sel = sel;
            LOCTXT(sel);
            snprintf(id, sizeof id, "XN%02x", sel);
            snprintf(what, sizeof what, "CH10 (all reductions, inputs live) + loads of the Location inputs {%s} -> null, the other inputs stay live", loc);
            row_recipe(l, id, what, &r, NULL, 0, 0);
        }
        /* (b) entry-cut position x inputs live / null, on the CH10 reductions without their own cut (L = 0: no cut; none+null is RI8, not repeated) */
        static const uint32_t lv[] = { 1, 2, 4, 7, 15, 29, 59, 118, 236, 354, 0 };
        for (size_t q = 0; q < sizeof lv / sizeof lv[0]; q++) for (int nul = 0; nul < 2; nul++) {
            if (!lv[q] && nul) continue;
            if (lv[q] && lv[q] >= info->entry_blocks) continue;
            recipe_from_mask(l, CH10 & ~256u, &r);
            if (lv[q]) r.bits |= LRD_CUT, r.cut_blocks = lv[q];
            if (nul) r.bits |= LRD_INPUT_CONST, r.input_sel = 0xffffffffu;
            if (lv[q]) snprintf(id, sizeof id, "XG%u.%s", lv[q], nul ? "null" : "live"); else snprintf(id, sizeof id, "XGnone.live");
            snprintf(what, sizeof what, "CH10 reductions without the 1/10 cut, cut at first block >= %u (0 = no cut), Location inputs %s", lv[q], nul ? "NULLED" : "LIVE");
            row_recipe(l, id, what, &r, NULL, 0, 0);
        }
        /* the plain CUT<L> rows (inputs live, nothing else reduced) with the inputs nulled */
        for (int i = 0; i < 6; i++) {
            static const uint32_t cm[6] = { 1, 1, 1, 1, 1, 3 }, cd[6] = { 64, 16, 8, 4, 2, 4 };
            uint32_t nb = info->entry_blocks * cm[i] / cd[i];
            memset(&r, 0, sizeof r);
            r.bits = LRD_CUT | LRD_INPUT_CONST, r.cut_blocks = nb ? nb : 1, r.input_sel = 0xffffffffu;
            snprintf(id, sizeof id, "XU%u.null", r.cut_blocks);
            snprintf(what, sizeof what, "the plain CUT%u row (nothing else reduced) + loads of every Location input -> null", r.cut_blocks);
            row_recipe(l, id, what, &r, NULL, 0, 0);
        }
        /* (c) prune unreachable blocks / unused locals / unneeded memory accesses, each measured on its own and combined, on four bases */
        static const struct { const char *id, *what; uint32_t bits; } po[] = {
            { "PB", "unreachable blocks deleted", LRD_PRUNE_BLK }, { "PV", "unused Function variables deleted", LRD_PRUNE_VAR },
            { "PW", "write-only Function variables deleted with their stores and access chains", LRD_PRUNE_WOVAR }, { "PBW", "unreachable blocks + write-only locals deleted", LRD_PRUNE_BLK | LRD_PRUNE_WOVAR },
            { "PS", "stores into write-only locals deleted (variables stay)", LRD_MEM_STORE }, { "PL", "unused loads (not of Input variables) and unused access chains deleted", LRD_MEM_LOAD },
            { "PLI", "unused loads incl. the Input-variable loads and unused access chains deleted", LRD_MEM_LOAD | LRD_MEM_LOAD_IN }, { "PM", "unneeded memory accesses: stores into write-only locals + unused loads (not Input) + chains", LRD_MEM_STORE | LRD_MEM_LOAD },
            { "PALL", "blocks + write-only locals + unneeded memory accesses (Input loads kept)", LRD_PRUNE_BLK | LRD_PRUNE_WOVAR | LRD_MEM_STORE | LRD_MEM_LOAD },
            { "PALLI", "blocks + write-only locals + unneeded memory accesses incl. unused Input loads", LRD_PRUNE_BLK | LRD_PRUNE_WOVAR | LRD_MEM_STORE | LRD_MEM_LOAD | LRD_MEM_LOAD_IN } };
        static const char *bn[4] = { "CH10", "CH11", "CUT7", "ORIG" };
        for (int b = 0; b < 4; b++) for (size_t q = 0; q < sizeof po / sizeof po[0]; q++) {
            memset(&r, 0, sizeof r);
            if (b < 2) recipe_from_mask(l, b ? CH11 : CH10, &r);
            else if (b == 2) { uint32_t nb = info->entry_blocks / 64; r.bits = LRD_CUT, r.cut_blocks = nb ? nb : 1; }
            r.bits |= po[q].bits;
            snprintf(id, sizeof id, "%s.%s", po[q].id, bn[b]);
            snprintf(what, sizeof what, "%s: %s%s", bn[b], po[q].what, b == 0 ? " (inputs live)" : b == 1 ? " (inputs nulled)" : b == 2 ? " (plain CUT, inputs live)" : " (the original module)");
            row_recipe(l, id, what, &r, NULL, 0, 0);
        }
        /* (d) minimal fragment stage (every declaration + interface kept) that loads the inputs and wires them straight to the outputs: tests the INTERFACE itself, without the 1944 locals / 473 blocks */
        if (nin) {
            static const struct { const char *id; uint32_t bits; int real_vs; int mirror; } iw[] = { { "IW0.all", LRD_MIN_ENTRY | LRD_MIN_LOADS, 1, 0 }, { "IW1.all", LRD_MIN_ENTRY | LRD_MIN_WIRE, 1, 0 },
                { "IW1.all.stub", LRD_MIN_ENTRY | LRD_MIN_WIRE, 0, 0 } };
            for (size_t q = 0; q < sizeof iw / sizeof iw[0]; q++) {
                memset(&r, 0, sizeof r);
                r.bits = iw[q].bits, r.input_sel = fullin;
                snprintf(what, sizeof what, "minimal FS (IFC shape) that %s all %u Location input(s)%s; %s", iw[q].bits & LRD_MIN_WIRE ? "loads and WIRES to the outputs" : "loads (results unused)", nin, "", iw[q].real_vs ? "REAL VS" : "STUB VS");
                row_recipe(l, iw[q].id, what, &r, iw[q].real_vs ? rvs : NULL, iw[q].real_vs ? rvsn : 0, 0);
            }
            {   /* the same wiring against the generated VS that writes every Location the ORIGINAL FS reads */
                uint32_t *gv = NULL;
                size_t gn = 0;
                char gi[160] = "";
                int g = lagfx_red_make_vs_for_fs(cfg->fs, cfg->fs_bytes / 4, &gv, &gn, gi, sizeof gi);
                memset(&r, 0, sizeof r);
                r.bits = LRD_MIN_ENTRY | LRD_MIN_WIRE, r.input_sel = fullin;
                if (g == 1) row_recipe(l, "IW1.all.mirror", "minimal FS wiring all Location inputs to the outputs; GENERATED mirror VS (writes every Location the original FS reads)", &r, gv, gn * 4, 0);
                else do_row(l, "IW1.all.mirror", "minimal wiring FS + generated mirror VS", MEAN_D, NULL, 0, 0, NULL, 0, gi[0] ? gi : "cannot mirror the inputs", 0);
                free(gv);
            }
            for (uint32_t k = 0; k < nin; k++) {
                memset(&r, 0, sizeof r);
                r.bits = LRD_MIN_ENTRY | LRD_MIN_WIRE, r.input_sel = 1u << k;
                snprintf(id, sizeof id, "IW1.loc%u", info->in[k].loc);
                snprintf(what, sizeof what, "minimal FS that loads and wires ONLY Location %u (kind %u x%u) to the outputs; REAL VS", info->in[k].loc, info->in[k].kind, info->in[k].ncomp);
                row_recipe(l, id, what, &r, rvs, rvsn, 0);
            }
            if (nblt) {
                static const struct { const char *id; uint32_t bits; } ib[] = { { "IWB0", LRD_MIN_ENTRY | LRD_MIN_LOADS }, { "IWB1", LRD_MIN_ENTRY | LRD_MIN_WIRE } };
                for (size_t q = 0; q < 2; q++) {
                    memset(&r, 0, sizeof r);
                    r.bits = ib[q].bits, r.blt_sel = fullblt;
                    row_recipe(l, ib[q].id, ib[q].bits & LRD_MIN_WIRE ? "minimal FS that loads and wires only the BuiltIn inputs (FragCoord / PointCoord) to the outputs; REAL VS" : "minimal FS that loads only the BuiltIn inputs (results unused); REAL VS", &r, rvs, rvsn, 0);
                }
                static const struct { const char *id; uint32_t bits; } ia[] = { { "IWA0", LRD_MIN_ENTRY | LRD_MIN_LOADS }, { "IWA1", LRD_MIN_ENTRY | LRD_MIN_WIRE } };
                for (size_t q = 0; q < 2; q++) {
                    memset(&r, 0, sizeof r);
                    r.bits = ia[q].bits, r.input_sel = fullin, r.blt_sel = fullblt;
                    row_recipe(l, ia[q].id, ia[q].bits & LRD_MIN_WIRE ? "minimal FS that loads and wires EVERY input read by the original (Location + BuiltIn); REAL VS" : "minimal FS that loads every input read by the original (Location + BuiltIn), results unused; REAL VS", &r, rvs, rvsn, 0);
                }
            }
        }
        /* (e) the translator's constexpr samplers (OpName lagfx.cxs.*) folded onto the older sampler binding: removes the extra sampler bindings (39/40 of the 348,036 B module; 40 collides with an input attachment) */
        memset(&r, 0, sizeof r);
        r.bits = LRD_CXS_FOLD;
        row_recipe(l, "CXF", "constexpr-sampler variables removed, their loads read the highest-binding non-constexpr sampler (original module otherwise unchanged)", &r, NULL, 0, 0);
        recipe_from_mask(l, CH10, &r);
        r.bits |= LRD_CXS_FOLD;
        row_recipe(l, "CXF.CH10", "CH10 + constexpr samplers folded", &r, NULL, 0, 0);
        recipe_from_mask(l, CH11, &r);
        r.bits |= LRD_CXS_FOLD;
        row_recipe(l, "CXF.CH11", "CH11 + constexpr samplers folded", &r, NULL, 0, 0);
#undef LOCTXT
    }
    if (!cfg->list_only) {
        emit_line(l, "pipeline-reduce: summary fs=%016llx R0=%s BUILT=[%s] FAIL=[%s] CRASH=[%s] TIMEOUT=[%s] N/A=[%s] INVALID=[%s] rows=%u built=%u fail=%u crash=%u timeout=%u invalid=%u na=%u (BUILT = pipeline creation only, nothing about rendering)",
                  (unsigned long long)l->fh, l->r0 == LRS_BUILT ? "BUILT" : l->r0 == LRS_FAIL ? "FAIL" : "CRASH/TIMEOUT", l->ids[0], l->ids[1], l->ids[2], l->ids[3], l->ids[4], l->ids[5], l->rows, l->built, l->fail, l->crash, l->timeout, l->invalid, l->na);
        if (l->smallest) emit_line(l, "pipeline-reduce: smallest-failing row=%s bytes=%u (original %zu bytes, %.1f%%)", l->smallest_id, l->smallest, cfg->fs_bytes, 100.0 * l->smallest / (double)cfg->fs_bytes);
        else emit_line(l, "pipeline-reduce: smallest-failing none (no row FAILed)");
    }
    rs.rows = l->rows, rs.built = l->built, rs.fail = l->fail, rs.crash = l->crash, rs.timeout = l->timeout, rs.invalid = l->invalid, rs.na = l->na, rs.r0 = l->r0;
    snprintf(rs.smallest_fail, sizeof rs.smallest_fail, "%s", l->smallest_id), rs.smallest_fail_bytes = l->smallest;
    if (res) *res = rs;
out:
    if (l->fd_log >= 0) close(l->fd_log);
    free(info);
    free(l);
}

/* ---------------------------------------------------------------- production hook */
static struct { bool lr, ro; } g_dev;
void lagfx_pipeline_reduce_set_device(bool lr, bool ro) { g_dev.lr = lr, g_dev.ro = ro; }
static void emit_warn(void *u, const char *l) { (void)u; LAGFX_WARN("%s", l); }

void lagfx_pipeline_reduce_on_failure(VkDevice dev, const lagfx_pipeline_desc_t *failed, VkResult vr) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static int limit = -1;
    static uint64_t seen[8];
    static uint32_t nseen;
    if (limit == 0) return;
    if (limit < 0) {
        const char *e = getenv("LAGFX_PIPELINE_REDUCE");
        int v = e && e[0] ? atoi(e) : 0;
        limit = v <= 0 ? 0 : v > 4 ? 4 : v;
        if (!limit) return;
        LAGFX_WARN("pipeline-reduce: ENABLED (LAGFX_PIPELINE_REDUCE=%s): the first %d distinct failing fragment modules get the reduction ladder; rows are `pipeline-reduce: row=<id> result=BUILT|FAIL|n/a ...` (in-process: a compiler crash in a row ends the process; use pipeline-repro --reduce for crash isolation)", e, limit);
    }
    if (!dev || !failed || !failed->fragment_spv || !failed->fragment_spv_bytes || (failed->fragment_spv_bytes & 3u)) return;
    uint64_t fh = lagfx_pipeline_spv_hash(failed->fragment_spv, failed->fragment_spv_bytes);
    pthread_mutex_lock(&mu);
    for (uint32_t i = 0; i < nseen; i++) if (seen[i] == fh) { pthread_mutex_unlock(&mu); return; }
    if (nseen >= (uint32_t)limit) { pthread_mutex_unlock(&mu); return; }
    seen[nseen++] = fh;
    lagfx_reduce_target_t t;
    memset(&t, 0, sizeof t);
    t.d0 = *failed, t.fs_entry = failed->fragment_entry_point;
    lagfx_reduce_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    const char *md = getenv("LAGFX_PIPELINE_MATRIX_DIR");
    cfg.fs = failed->fragment_spv, cfg.fs_bytes = failed->fragment_spv_bytes, cfg.vs = failed->vertex_spv, cfg.vs_bytes = failed->vertex_spv_bytes;
    cfg.file_dir = md && md[0] ? md : NULL;
    lagfx_pipeline_reduce_cfg_env(&cfg);
    cfg.emit = emit_warn, cfg.dev = dev, cfg.layout = failed->layout, cfg.target = &t;
    LAGFX_WARN("pipeline-reduce: failing fs=%016llx vk=%d: running the ladder", (unsigned long long)fh, (int)vr);
    lagfx_pipeline_reduce_run(&cfg, NULL);
    pthread_mutex_unlock(&mu);
}

#endif /* LAGFX_HAVE_VULKAN */
