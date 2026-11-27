/* AIR constant-expression casts (CST_CODE_CE_CAST) -> census X03 (OpUndef origin analysis: "CE_CAST unresolved code11 = translator gap").
 *
 * Part 1: lagfx_ce_fold (src/air2spv/ce_cast.c) against LLVM LangRef semantics, value by value. LLVM folds every cast of a plain constant while it builds IR, so a fold of
 *         a scalar constant cannot be reached through a .ll fixture: the folding rules are pinned here directly. No Apple GPU semantics are involved.
 * Part 2: tests/fixtures/ce_cast_fragment.air.bc (assembled by llvm-as from ce_cast_fragment.ll: HAND-WRITTEN IR, NOT Apple output) holds the two casts LLVM cannot fold - an
 *         addrspacecast and a ptrtoint of the address of a module global. The translator must (a) emit no OpUndef pointer placeholder for the first (before this change: one
 *         OpUndef of a pointer type that nothing consumed) and (b) keep exactly one OpUndef i64 for the second (an address SPIR-V cannot express), and the module must pass
 *         spirv-val. Exit 77 for part 2 when spirv-val is not installed.
 * SPDX-License-Identifier: MIT */
#include "air2spv/ce_cast.h"
#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int bad;
#define EXPECT(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); bad++; } } while (0)

static uint32_t fbits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static lagfx_ce_val_t I(uint32_t w, uint64_t v) { lagfx_ce_val_t s = { .ok = true, .width = w, .bits = v & lagfx_ce_mask(w) }; return s; }
static lagfx_ce_val_t F(float f) { lagfx_ce_val_t s = { .ok = true, .is_float = true, .width = 32, .bits = fbits(f) }; return s; }
static lagfx_ce_val_t H(uint32_t bits) { lagfx_ce_val_t s = { .ok = true, .is_float = true, .width = 16, .bits = bits }; return s; }
static lagfx_ce_val_t PN(void) { lagfx_ce_val_t s = { .ok = true, .width = 64, .is_ptr_null = true }; return s; }
static lagfx_ce_val_t UD(uint32_t w) { lagfx_ce_val_t s = { .ok = true, .width = w, .undef = true }; return s; }

/* expect a folded value (bits) */
static void val(const char *what, uint32_t opc, lagfx_ce_val_t s, bool df, bool dp, uint32_t dw, uint64_t want) {
    lagfx_ce_val_t o;
    bool ok = lagfx_ce_fold(opc, &s, df, dp, dw, &o);
    EXPECT(ok && o.ok && !o.undef && o.bits == want, "%s: ok=%d undef=%d bits=0x%llx want 0x%llx", what, ok, o.undef, (unsigned long long)o.bits, (unsigned long long)want);
}
static void poison(const char *what, uint32_t opc, lagfx_ce_val_t s, bool df, bool dp, uint32_t dw) {
    lagfx_ce_val_t o;
    bool ok = lagfx_ce_fold(opc, &s, df, dp, dw, &o);
    EXPECT(ok && o.undef, "%s: expected undef/poison (ok=%d undef=%d)", what, ok, o.undef);
}
static void nofold(const char *what, uint32_t opc, lagfx_ce_val_t s, bool df, bool dp, uint32_t dw) {
    lagfx_ce_val_t o;
    EXPECT(!lagfx_ce_fold(opc, &s, df, dp, dw, &o), "%s: must stay unresolved", what);
}

static void part1(void) {
    /* trunc / zext / sext */
    val("trunc i32 0x1FF to i8", 0, I(32, 0x1FF), false, false, 8, 0xFF);
    val("trunc i32 2 to i1", 0, I(32, 2), false, false, 1, 0);
    val("trunc i64 0x123456789 to i32", 0, I(64, 0x123456789ull), false, false, 32, 0x23456789u);
    val("zext i8 255 to i32", 1, I(8, 0xFF), false, false, 32, 0xFF);
    val("zext i1 1 to i8", 1, I(1, 1), false, false, 8, 1);
    val("sext i8 255 to i32", 2, I(8, 0xFF), false, false, 32, 0xFFFFFFFFu);
    val("sext i8 127 to i32", 2, I(8, 0x7F), false, false, 32, 0x7F);
    val("sext i16 0x8000 to i64", 2, I(16, 0x8000), false, false, 64, 0xFFFFFFFFFFFF8000ull);
    val("sext i1 1 to i8", 2, I(1, 1), false, false, 8, 0xFF);
    nofold("trunc to a wider type is not a trunc", 0, I(8, 1), false, false, 32);
    nofold("zext to a narrower type", 1, I(32, 1), false, false, 8);
    /* float -> int: truncation toward zero; out of range / NaN is poison */
    val("fptoui 3.9f to i32", 3, F(3.9f), false, false, 32, 3);
    val("fptosi -3.9f to i32", 4, F(-3.9f), false, false, 32, 0xFFFFFFFDu);
    val("fptosi 127.5f to i8", 4, F(127.5f), false, false, 8, 127);
    val("fptoui 2.5h to i32", 3, H(0x4100), false, false, 32, 2);   /* 0x4100 = 2.5 */
    poison("fptoui -1.0f to i32", 3, F(-1.0f), false, false, 32);
    poison("fptoui 256.0f to i8", 3, F(256.0f), false, false, 8);
    poison("fptosi 128.0f to i8", 4, F(128.0f), false, false, 8);
    poison("fptosi NaN to i32", 4, F(NAN), false, false, 32);
    /* int -> float: round to nearest even */
    val("uitofp i8 255 to float", 5, I(8, 255), true, false, 32, fbits(255.0f));
    val("sitofp i8 -1 to float", 6, I(8, 0xFF), true, false, 32, fbits(-1.0f));
    val("uitofp i32 16777217 to float (tie -> even)", 5, I(32, 16777217u), true, false, 32, fbits(16777216.0f));
    val("sitofp i32 -2147483648 to float", 6, I(32, 0x80000000u), true, false, 32, fbits(-2147483648.0f));
    val("uitofp i32 2049 to half (tie -> 2048)", 5, I(32, 2049), true, false, 16, 0x6800);
    val("uitofp i32 2051 to half (tie -> 2052)", 5, I(32, 2051), true, false, 16, 0x6802);
    val("uitofp i32 70000 to half = +inf", 5, I(32, 70000), true, false, 16, 0x7C00);
    val("sitofp i32 -1 to half", 6, I(32, 0xFFFFFFFFu), true, false, 16, 0xBC00);
    /* fptrunc / fpext between half and float */
    val("fptrunc 1.0f to half", 7, F(1.0f), true, false, 16, 0x3C00);
    val("fptrunc 65520.0f to half = +inf", 7, F(65520.0f), true, false, 16, 0x7C00);
    val("fptrunc 65504.0f to half = max", 7, F(65504.0f), true, false, 16, 0x7BFF);
    val("fptrunc 2^-24 to half = smallest subnormal", 7, F(ldexpf(1.0f, -24)), true, false, 16, 0x0001);
    val("fptrunc 2^-25 to half = 0 (tie -> even)", 7, F(ldexpf(1.0f, -25)), true, false, 16, 0x0000);
    val("fptrunc -0.0f to half", 7, F(-0.0f), true, false, 16, 0x8000);
    val("fpext 1.0h to float", 8, H(0x3C00), true, false, 32, fbits(1.0f));
    val("fpext +inf half", 8, H(0x7C00), true, false, 32, fbits(INFINITY));
    val("fpext smallest half subnormal", 8, H(0x0001), true, false, 32, fbits(ldexpf(1.0f, -24)));
    val("fpext -2.5h", 8, H(0xC100), true, false, 32, fbits(-2.5f));
    nofold("fptrunc float -> float", 7, F(1.0f), true, false, 32);
    /* bitcast: the bit pattern is the value */
    val("bitcast i32 0x3F800000 to float", 11, I(32, 0x3F800000u), true, false, 32, 0x3F800000u);
    val("bitcast float 1.0 to i32", 11, F(1.0f), false, false, 32, 0x3F800000u);
    val("bitcast half 0x3C00 to i16", 11, H(0x3C00), false, false, 16, 0x3C00);
    nofold("bitcast i16 to float (widths differ)", 11, I(16, 1), true, false, 32);
    /* pointers: only the null pointer has a known address */
    val("ptrtoint null to i64", 9, PN(), false, false, 64, 0);
    { lagfx_ce_val_t o; EXPECT(lagfx_ce_fold(10, &(lagfx_ce_val_t){ .ok = true, .width = 64, .bits = 0 }, false, true, 64, &o) && o.is_ptr_null, "inttoptr 0 = null"); }
    nofold("inttoptr 5 (address not representable)", 10, I(64, 5), false, true, 64);
    { lagfx_ce_val_t s = PN(), o; EXPECT(lagfx_ce_fold(12, &s, false, true, 64, &o) && o.is_ptr_null, "addrspacecast null = null");
      EXPECT(lagfx_ce_fold(11, &s, false, true, 64, &o) && o.is_ptr_null, "bitcast null ptr = null"); }
    /* cast of undef / poison is undef / poison */
    poison("zext undef", 1, UD(8), false, false, 32);
    poison("uitofp undef", 5, UD(32), true, false, 32);
    poison("bitcast undef", 11, UD(32), true, false, 32);
}

/* ---- part 2 --------------------------------------------------------------------------------------------------------------------------------------------------- */
static uint8_t *slurp(const char *name, size_t *n) {
    char path[512];
    const char *dirs[] = { SRCDIR "/fixtures/", "tests/fixtures/", "../tests/fixtures/" };
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
        snprintf(path, sizeof path, "%s%s", dirs[d], name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *b = malloc((size_t)sz);
        if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
        fclose(f); *n = (size_t)sz; return b;
    }
    return NULL;
}

/* 0 = valid, 1 = rejected, -1 = spirv-val not runnable */
static int spirv_val(const uint8_t *spv, size_t sz) {
    char tmpl[] = "/tmp/lagfx-cec-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    if (write(fd, spv, sz) != (ssize_t)sz) { close(fd); unlink(tmpl); return -1; }
    close(fd);
    pid_t pid = fork();
    if (pid == 0) { execlp("spirv-val", "spirv-val", "--target-env", "vulkan1.3", tmpl, (char *)NULL); _exit(127); }
    int st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) < 0) { unlink(tmpl); return -1; }
    if (!WIFEXITED(st) || WEXITSTATUS(st) == 127) { unlink(tmpl); return -1; }
    unlink(tmpl);
    return WEXITSTATUS(st) ? 1 : 0;
}

/* the type ids: OpTypePointer (op 32) and OpTypeInt width 64 (op 21), then the OpUndef (op 1) results by type */
static void undef_census(const uint32_t *w, size_t nw, int *ptr_undefs, int *i64_undefs) {
    uint32_t ptr_ty[64], n_ptr = 0, i64_ty = 0;
    *ptr_undefs = *i64_undefs = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
        if (!wc) break;
        if (op == 32u && n_ptr < 64u) ptr_ty[n_ptr++] = w[i + 1];
        if (op == 21u && w[i + 2] == 64u) i64_ty = w[i + 1];
        if (op == 1u) { for (uint32_t k = 0; k < n_ptr; k++) if (w[i + 1] == ptr_ty[k]) (*ptr_undefs)++; if (i64_ty && w[i + 1] == i64_ty) (*i64_undefs)++; }
        i += wc;
    }
}

static int part2(void) {
    size_t bn = 0; uint8_t *bc = slurp("ce_cast_fragment.air.bc", &bn);
    EXPECT(bc, "fixture ce_cast_fragment.air.bc not found");
    if (!bc) return 0;
    lagfx_air_module_t *m = NULL; uint8_t *spv = NULL; size_t sz = 0;
    EXPECT(lagfx_air_module_open(bc, bn, &m) == LAGFX_OK && m, "module_open");
    if (m && lagfx_air2spv_translate_module(m, &spv, &sz) != LAGFX_OK) spv = NULL;
    EXPECT(spv, "translate");
    int skipped = 0;
    if (spv) {
        int pu, iu;
        undef_census((const uint32_t *)(const void *)spv, sz / 4u, &pu, &iu);
        EXPECT(pu == 0, "%d OpUndef of a pointer type: the addrspacecast of a module global must not leave a placeholder", pu);
        EXPECT(iu == 1, "%d OpUndef of i64 (want 1: ptrtoint of a global address stays unresolved)", iu);
        int v = spirv_val(spv, sz);
        if (v < 0) skipped = 1; else EXPECT(v == 0, "spirv-val rejected the module");
    }
    free(spv); free(bc); lagfx_air_module_free(m);
    return skipped;
}

int main(void) {
    part1();
    int skipped = part2();
    if (bad) { printf("air2spv-ce-cast-unit: %d FAILED\n", bad); return 1; }
    printf("air2spv-ce-cast-unit: ok%s\n", skipped ? " (spirv-val not installed: part 2 not validated)" : "");
    return bad ? 1 : (skipped ? 77 : 0);
}
