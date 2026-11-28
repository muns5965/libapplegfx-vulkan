/* AIR atomics: the translation MATRIX (address space x operation x signedness spelling x element type), one tiny module per cell. No Vulkan device needed.
 * SPDX-License-Identifier: MIT
 *
 * Purpose: classify every spelling the grammar of air.atomic.<global|local>.<op>[.<s|u>].<i32|i64|f32> can express as
 *     SUPPORTED_V  translates, and the spelling was seen in a real Apple module     (only the six [V] spellings of docs/spec-audit.md "Atomics")
 *     SUPPORTED_N  translates by grammar extrapolation; the spelling was never observed [N]
 *     REFUSED(why) refused with a named LAGFX_ATOMIC_R_* reason in the default (strict) mode
 * The expectation of each cell is written here as an independent oracle (expect()), NOT read back from the translator, so a change of the handler that moves a cell fails this test.
 * The table is printed in full (and written to $LAGFX_ATOMIC_MATRIX_OUT when set, for docs/evidence).
 *
 * Operand shapes are the observed ones only (order 0, scope 2, rmw extra 0, final i1 true); i64 read-modify-write and store are declared void, i64 load / rmw declared non-void
 * are probed separately (they are the strict-mode I64_RETURN refusals). Compare-exchange uses the one implemented synthetic form (expected by pointer in function memory, i1 result).
 * Modules are assembled from text with llvm-as: when llvm-as is missing the test exits 77 (SKIP) and CI treats a skip as a failure.
 */
#include "air2spv/translate.h"
#include "air2spv/translate_function.h"
#include "air/bitcode_reader.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { OP_LOAD, OP_STORE, OP_XCHG, OP_ADD, OP_SUB, OP_AND, OP_OR, OP_XOR, OP_MIN, OP_MAX, OP_CAS, OP_N };
static const char *const opname[OP_N] = { "load", "store", "xchg", "add", "sub", "and", "or", "xor", "min", "max", "cmpxchg" };
enum { SG_NONE, SG_S, SG_U, SG_WEAK, SG_STRONG, SG_N };
static const char *const sgname[SG_N] = { "", "s", "u", "weak", "strong" };
enum { T_I32, T_I64, T_F32, T_N };
static const char *const tyname[T_N] = { "i32", "i64", "f32" };
static const char *const tyll[T_N] = { "i32", "i64", "float" };
static const unsigned tybytes[T_N] = { 4u, 8u, 4u };

typedef struct { int space /* 0 global, 1 local */, op, sg, ty; } cell_t;
#define OK LAGFX_ATOMIC_R_NONE

static int is_cas_sg(int sg) { return sg == SG_NONE || sg == SG_WEAK || sg == SG_STRONG; }

/* the only spellings seen in real Apple modules */
static int verified(const cell_t *c) {
    if (c->space != 0 || c->ty != T_I32) return 0;
    if (c->op == OP_LOAD || c->op == OP_STORE || c->op == OP_XCHG) return c->sg == SG_NONE;
    if (c->op == OP_ADD || c->op == OP_AND || c->op == OP_OR) return c->sg == SG_U;
    return 0;
}

/* Is this cell expressible at all (a sane spelling)? sg: rmw sign letters only for rmw ops; cas words only for cmpxchg. */
static int spelled(const cell_t *c) {
    if (c->op == OP_CAS) return is_cas_sg(c->sg);
    return c->sg == SG_NONE || c->sg == SG_S || c->sg == SG_U;
}

/* Independent oracle: expected refusal reason (OK = translates) in the default mode. */
static unsigned expect(const cell_t *c) {
    if (c->ty == T_F32) {
        if (c->op == OP_CAS) return LAGFX_ATOMIC_R_SPELLING;
        if (c->op != OP_ADD) return LAGFX_ATOMIC_R_SPELLING;          /* float load / store / xchg / sub / and / or / xor / min / max: never emulated */
        return LAGFX_ATOMIC_R_FLOAT;                                  /* float add: spelling never observed, refused in strict mode */
    }
    const int signed_spelling = c->op == OP_ADD || c->op == OP_SUB || c->op == OP_MIN || c->op == OP_MAX;
    if (c->op != OP_CAS && signed_spelling && c->sg == SG_NONE) return LAGFX_ATOMIC_R_SPELLING;   /* signedness is part of the spelling */
    if (c->ty == T_I64) {
        if (c->op == OP_LOAD) return LAGFX_ATOMIC_R_I64_RETURN;       /* a 64-bit load always has a result; its real AIR return type is unknown */
        if (c->op == OP_CAS) return OK;
        return OK;                                                    /* void-declared store / rmw */
    }
    return OK;
}

static const char *classname(unsigned want, const cell_t *c, char *buf, size_t n) {
    if (want == OK) return verified(c) ? "SUPPORTED_V" : "SUPPORTED_N";
    snprintf(buf, n, "REFUSED(%s)", lagfx_air2spv_atomic_reason_name(want));
    return buf;
}

/* ---- module text ---- */
static void emit_ll(FILE *f, const cell_t *c, const char *fname) {
    const char *T = tyll[c->ty];
    char sym[96];
    if (c->op == OP_CAS) snprintf(sym, sizeof sym, "air.atomic.%s.cmpxchg%s%s%s.%s", c->space ? "local" : "global", c->sg == SG_NONE ? "" : ".", sgname[c->sg], "", tyname[c->ty]);
    else snprintf(sym, sizeof sym, "air.atomic.%s.%s%s%s.%s", c->space ? "local" : "global", opname[c->op], c->sg == SG_NONE ? "" : ".", sgname[c->sg], tyname[c->ty]);
    const char *AS = c->space ? "addrspace(3)" : "addrspace(1)";
    const int is64 = c->ty == T_I64;
    fputs("target datalayout = \"e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32\"\n"
          "target triple = \"air64-apple-macosx15.6.0\"\n", f);
    if (c->space) fprintf(f, "@tg = internal unnamed_addr addrspace(3) global [4 x %s] undef, align %u\n", T, tybytes[c->ty]);
    /* declaration */
    switch (c->op) {
    case OP_LOAD:  fprintf(f, "declare %s @%s(ptr %s, i32, i32, i1)\n", T, sym, AS); break;
    case OP_STORE: fprintf(f, "declare void @%s(ptr %s, %s, i32, i32, i1)\n", sym, AS, T); break;
    case OP_CAS:   fprintf(f, "declare i1 @%s(ptr %s, ptr, %s, i32, i32, i32, i1)\n", sym, AS, T); break;
    default:       fprintf(f, "declare %s @%s(ptr %s, %s, i32, i32, i32, i1)\n", is64 ? "void" : T, sym, AS, T); break;
    }
    fprintf(f, "define void @%s(ptr addrspace(1) %%c, i32 %%tid) {\nentry:\n", fname);
    if (c->space) fprintf(f, "  %%p = getelementptr inbounds [4 x %s], ptr addrspace(3) @tg, i64 0, i64 0\n", T);
    else          fprintf(f, "  %%p = getelementptr %s, ptr addrspace(1) %%c, i64 0\n", T);
    const char *V = c->ty == T_F32 ? "1.0" : "1";
    switch (c->op) {
    case OP_LOAD:  fprintf(f, "  %%r = call %s @%s(ptr %s %%p, i32 0, i32 2, i1 true)\n", T, sym, AS); break;
    case OP_STORE: fprintf(f, "  call void @%s(ptr %s %%p, %s %s, i32 0, i32 2, i1 true)\n", sym, AS, T, V); break;
    case OP_CAS:
        fprintf(f, "  %%exp = alloca %s, align %u\n  store %s %s, ptr %%exp, align %u\n", T, tybytes[c->ty], T, c->ty == T_F32 ? "0.0" : "0", tybytes[c->ty]);
        fprintf(f, "  %%ok = call i1 @%s(ptr %s %%p, ptr %%exp, %s %s, i32 0, i32 0, i32 2, i1 true)\n", sym, AS, T, V);
        break;
    default:
        if (is64) fprintf(f, "  call void @%s(ptr %s %%p, i64 %s, i32 0, i32 2, i32 0, i1 true)\n", sym, AS, V);
        else      fprintf(f, "  %%r = call %s @%s(ptr %s %%p, %s %s, i32 0, i32 2, i32 0, i1 true)\n", T, sym, AS, T, V);
        break;
    }
    fprintf(f, "  ret void\n}\n"
        "!llvm.module.flags = !{!0}\n!air.version = !{!1}\n!air.language_version = !{!2}\n"
        "!0 = !{i32 1, !\"wchar_size\", i32 4}\n!1 = !{i32 2, i32 7, i32 0}\n!2 = !{!\"Metal\", i32 3, i32 2, i32 0}\n!3 = !{}\n"
        "!air.kernel = !{!20}\n!20 = !{ptr @%s, !3, !19}\n"
        "!10 = !{i32 0, !\"air.buffer\", !\"air.location_index\", i32 0, i32 1, !\"air.read_write\", !\"air.address_space\", i32 1, !\"air.arg_type_size\", i32 4, !\"air.arg_type_align_size\", i32 4, !\"air.arg_type_name\", !\"atomic_uint\", !\"air.arg_name\", !\"c\"}\n"
        "!12 = !{i32 1, !\"air.thread_position_in_grid\", !\"air.arg_type_name\", !\"uint\", !\"air.arg_name\", !\"tid\"}\n"
        "!19 = !{!10, !12}\n", fname);
}

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)l + 1u);
    if (b && fread(b, 1, (size_t)l, f) != (size_t)l) { free(b); b = NULL; }
    fclose(f); if (b) *n = (size_t)l; return b;
}

int main(void) {
    if (system("llvm-as --version >/dev/null 2>&1") != 0) { printf("SKIP: llvm-as not found\n"); return 77; }
    if (getenv("LAGFX_ATOMIC_LENIENT") && !strcmp(getenv("LAGFX_ATOMIC_LENIENT"), "1")) { printf("FAIL: run without LAGFX_ATOMIC_LENIENT=1\n"); return 1; }
    char dir[] = "/tmp/lagfx-atomic-matrixXXXXXX";
    if (!mkdtemp(dir)) { printf("FAIL: mkdtemp\n"); return 1; }
    const char *outp = getenv("LAGFX_ATOMIC_MATRIX_OUT");
    FILE *out = outp ? fopen(outp, "w") : NULL;
    unsigned cells = 0, bad = 0, nv = 0, nn = 0, nr = 0;
    if (out) fprintf(out, "space\top\tsignedness\ttype\tclass\n");
    for (int sp = 0; sp < 2; sp++)
    for (int op = 0; op < OP_N; op++)
    for (int sg = 0; sg < SG_N; sg++)
    for (int ty = 0; ty < T_N; ty++) {
        cell_t c = { sp, op, sg, ty };
        if (!spelled(&c)) continue;
        if (op == OP_CAS && sg == SG_NONE && ty == T_F32) { /* still a cell: refused as spelling */ }
        char ll[256], bc[256], cmd[640];
        snprintf(ll, sizeof ll, "%s/cell.ll", dir); snprintf(bc, sizeof bc, "%s/cell.air.bc", dir);
        FILE *f = fopen(ll, "w"); if (!f) { printf("FAIL: open %s\n", ll); return 1; }
        emit_ll(f, &c, "cell"); fclose(f);
        snprintf(cmd, sizeof cmd, "llvm-as %s -o %s 2>%s/err.txt", ll, bc, dir);
        if (system(cmd) != 0) { printf("FAIL: llvm-as rejected the cell %s %s %s %s (see %s/cell.ll)\n", sp ? "local" : "global", opname[op], sgname[sg], tyname[ty], dir); return 1; }
        size_t an = 0; uint8_t *air = slurp(bc, &an);
        lagfx_air_module_t *m = NULL;
        if (!air || lagfx_air_module_open(air, an, &m) != LAGFX_OK || !m) { printf("FAIL: module_open %s\n", ll); return 1; }
        uint8_t *spv = NULL; size_t sn = 0;
        lagfx_air2spv_set_atomic_lenient(0);
        lagfx_status_t st = lagfx_air2spv_translate_module(m, &spv, &sn);
        lagfx_air2spv_set_atomic_lenient(-1);
        const unsigned got = st == LAGFX_OK ? OK : lagfx_air2spv_last_atomic_reason();
        const unsigned want = expect(&c);
        char b1[64], b2[64];
        cells++;
        if (want == OK) { if (verified(&c)) nv++; else nn++; } else nr++;
        if (st != LAGFX_OK && got == OK) { printf("FAIL %s %s%s%s %s: refused but no atomic reason (status %d)\n", sp ? "local" : "global", opname[op], sg ? "." : "", sgname[sg], tyname[ty], (int)st); bad++; }
        else if (got != want) {
            printf("FAIL %s %s%s%s %s: expected %s, got %s\n", sp ? "local" : "global", opname[op], sg ? "." : "", sgname[sg], tyname[ty],
                   classname(want, &c, b1, sizeof b1), got == OK ? "translates" : (snprintf(b2, sizeof b2, "REFUSED(%s)", lagfx_air2spv_atomic_reason_name(got)), b2));
            bad++;
        }
        if (out) fprintf(out, "%s\t%s\t%s\t%s\t%s\n", sp ? "local" : "global", opname[op], sg ? sgname[sg] : "-", tyname[ty], classname(want, &c, b1, sizeof b1));
        free(spv); lagfx_air_module_free(m); free(air);
    }
    if (out) fclose(out);
    printf("atomic matrix: %u cells: %u SUPPORTED_V, %u SUPPORTED_N (extrapolated spelling), %u REFUSED; %u mismatch(es)\n", cells, nv, nn, nr, bad);
    char rm[300]; snprintf(rm, sizeof rm, "rm -rf %s", dir); (void)system(rm);
    return bad ? 1 : 0;
}
