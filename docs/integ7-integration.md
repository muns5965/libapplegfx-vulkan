# integ7: integration of seven g7 branches on top of integ6

Branch `research/g7-integ7`, base `origin/research/g7-integ6` (697c548, frozen, not modified). Seven `--no-ff` merges in this order; after each one the FULL meson suite (release build, clean build dir, deleted afterwards) was run. Numbers are the whole suite at that step.

## Merge order and per-step results

| # | branch | head SHA | conflicts | OK / Fail / Skip | tests |
|---|--------|----------|-----------|------------------|-------|
| 0 | integ6 (base) | 697c548 | - | 160 / 0 / 1 | 161 |
| 1 | g7-audit-fix3 | 92e12fb | none | 161 / 0 / 1 | 162 |
| 2 | g7-msaa2 | 8e0f245 | none (auto-merged over integ6's msaa) | 161 / 0 / 1 | 162 |
| 3 | g7-msaa3 (based on msaa2) | e1cfdf4 | none | 162 / 0 / 1 | 163 |
| 4 | g7-perf1 | e08ecde | none | 163 / 0 / 1 | 164 |
| 5 | g7-boot1 | ade81b4 | tests/meson.build | 164 / 0 / 1 | 165 |
| 6 | g7-blitfill (based on 92e12fb) | 625dfc8 | none | 165 / 0 / 1 | 166 |
| 7 | g7-atomic (based on 92e12fb) | 98d5c3a | docs/spec-audit.md | 166 / 0 / 1 | 167 |

The one Skip is `pipefail bundles` (needs `LAGFX_PIPEFAIL_CHECK_DIR`). Step 2 adds no new test entry: msaa2 extends `arm64-msaa-guest-unit`, `vulkan-msaa-coverage-unit` and the render-compat test inside their existing entries (tests/meson.build is unchanged by that merge), so the count stays 161 although the other steps are +1.

## Conflicts and decisions

- **tests/meson.build (step 5, boot1 vs integ6's gamma-diag block)**: both sides appended a block at the same place sharing the final `endif`. Kept both: the `gamma-diag-unit` block, then the `display-diag-clock-unit` block, each with its own `endif`.
- **docs/spec-audit.md (step 7, atomic vs blitfill)**: both appended a section after "Pass 3 totals". Kept both, "Blit fill pass" first, then "Atomics", separated by a blank line.
- **msaa2 over integ6's msaa (da772ee)**: no textual conflict. The semantic change is real and is listed under flags: exact sample counts are now the default; integ6's clamp / 1x fallback behaviour is only available with `LAGFX_MSAA_COMPAT=1`. Counters `ms_images_clamped` and `ms_fallback_1x` were replaced by `ms_compat_clamped` and `ms_compat_1x`; the integ6 tests that asserted the old behaviour were rewritten inside msaa2 itself (not by this integration).
- **LRD bits**: no branch touched the `LRD_*` masks; integ6's 64-bit decisions are untouched.
- Auto-merged without conflict but touched by several branches: arm64_exec_run.c (perf1 timing wrappers + msaa3 strict-pass gate), arm64_resources.c, arm64_msaa.*, display_diag.c (boot1, one line), instance.c, caps.[ch] (atomic: new `atomic64` cap), docs/metal-vulkan-compatibility*.md (msaa, blitfill, atomic rows, both kept).
- No merged test failed; no test was adjusted by this integration.

## perf1 (based on integ5): did anything of integ6 disappear?

perf1's merge base with integ6 is integ5 (3ca2128). Check done: (a) for the 9 files perf1 changed, the lines integ6 added relative to integ5 vs the integ7 head; (b) for every file integ6 added or changed relative to integ5 (src, tests, include, tools, examples, docs), every added line that is absent from the same file at the integ7 head.

- perf1 files: channel_compute_dispatcher.c (xlate_guard hooks, 13 integ6 lines): none removed. arm64_completion.c, arm64_hold.*, arm64-hold-unit.c, arm64-mapped-unit.c: integ6 did not touch them. protocol/meson.build and tests/meson.build (88 integ6 lines): nothing removed. arm64_exec_run.c: 13 integ6 lines are gone, all replaced on purpose, none lost: perf1 wraps the resolver call and `lagfx_pipeline_build` in hold timers (3 lines); msaa3 replaces the attachment loop with `look.*` lookups plus strict-mode rejects (10 lines). The non-strict path of every replaced line is still present (same counters `extra_attach_ignored`, `no_target`, `obj_unsupported_linear`, `ds_attach_unsupported`).
- (b) The remaining absent lines are only in files that the msaa branches, blitfill, atomic or audit-fix3 rewrote on purpose: arm64_msaa.c/.h, msaa_stats.h, arm64-msaa-guest-unit.c (old clamp / 1x asserts and the old stats line), arm64_resources.c (`MKFAIL` now distinguishes `TEXWHY_C_MSAA`), ops_misc.c (device-info warning text), blit_encoder.c (fill rewritten), caps.c (`k_names` gained `atomic64`), arm64-render-compat-unit.c (validation-baseline helper replaced by audit-fix3's analysis version), docs (spec-audit E1 row, msaa section: updated text).
- Still present at head: xlate_guard (dispatchers, object_resolver.c, exec_cmdbuf.c, info_replies.c, draw_resources.c, channel_0_dispatcher.c); `lagfx_gd_task_read` choosing `lagfx_arm64_task_read` in arm64 mode (diag.c) and the object_resolver.c comment about it; no integ6 line is missing in any other file (address translation, image lifetime, synchronization, texview, ssa, addrfix files included).
- Limit of the method: it compares added lines textually; a line moved or reformatted would show as absent (none such was found beyond the intentional ones above).

## New environment flags and defaults (this integration)

| flag | branch | default | effect |
|------|--------|---------|--------|
| `LAGFX_MSAA_COMPAT` (`=1`) | msaa2 | OFF (read at every call) | OFF = EXACT mode (new default): a guest sample count the device does not offer exactly makes the multisample texture refused, and a pass naming it is rejected as a whole. `=1` = compat: clamp down / 1x fallback (integ6 behaviour), NOT exact MSAA |
| `LAGFX_SLOW_MS` | perf1 | 20 (ms); `0` = off; capped at 3600000 | threshold of the `g7-gpu-slow` warning (max 100 lines); the hold ledger itself has no flag |

No other env flag is added. Behaviour that is ON without a flag:

- msaa2 EXACT mode (a default-behaviour change relative to integ6) and the device-info MSAASamples lowered to the largest count exact mode grants for the common colour + depth formats.
- msaa3 whole-pass rejection in exact mode (only for passes naming a multisampled attachment).
- perf1 hold ledger: `g7-gpu-hold` INFO line at most once per 10 s, only when something was recorded; idle = no lines.
- boot1: the `g7-pvg-ops` clock is anchored at the first command (diagnostic timing only).
- blitfill: 0x141 FillTextureWithColor honours region / level / slice / real layout; out-of-range is refused, not clipped. 0x140 is only counted.
- atomic: `air.atomic.*` and LLVM atomic instructions are translated (32-bit integer); an untranslatable atomic fails the translation (counted). 64-bit and f32 add are gated by device caps (`atomic64`, `atomic_f32_add`).
- audit-fix3: robustness / discard fixes (docs/spec-audit.md, pass 3).
- msaa3 host-side items (texel fetch, SampleId/SampleMask, alpha-to-coverage, sample shading, depth resolve modes) are UNWIRED: reachable only through the test injection hook `lagfx_msaa_inject`; no env flag; nothing in production uses them.

## Counters and log lines introduced

- msaa (`g7-gpu-msaa` line): `ms_compat_clamped`, `ms_compat_1x` (replace `ms_images_clamped`, `ms_fallback_1x`), `ms_exact_refused`, `ms_pass_refused`, `ms_fetch_views`, `ms_fetch_refused`, `a2c_pipes`, `sample_shading_pipes`, `sample_shading_refused`, `ds_resolve_refused`, `devinfo_gap`; the line names the mode. New warnings: `g7-gpu-msaa: mode=exact refused ...` (first 8), `mode=exact whole pass REJECTED` (first 16), `mode=compat ... granted` (first 8).
- hold ledger (perf1): phases `DRAIN, POLL, EXEC_END, RESOLVE, PIPE_BUILD, PIPE_FAIL` (nested, not additive); lines `g7-gpu-hold`, `g7-gpu-slow`.
- blitfill: `fill_full, fill_partial, fill_empty, fill_oob, fill_bad_subres, fill_format, fill_failed` (`lagfx_translate_blit_fill_stats_get`).
- atomic: `modules, translated, refused, modules_rejected` (`lagfx_air2spv_atomic_totals`).

## Final checks (integ7 head, before this document)

- Full meson, release, clean build dir: 166 OK / 0 fail / 1 skip (167 tests).
- A2Xghfc pair (`examples/air-pair-translate` on /tmp/ana-bf/input.{vs,fs}.bc): fs sha256 aa5ef4ce7adf1338..., vs sha256 d625b8ccc7d56c49... unchanged; `collisions: 0`.
- `tools/check-dump-spirv.sh`: run3/z 39 shaders OK, x231501 40 OK, vm2 40 OK (0 failed).
- ASan + UBSan (release, `-g0`, `detect_leaks=0`) full suite: 165 OK / 1 fail / 1 skip; the one failure is `pipeline reduction ladder` (ASan SEGV, the known pre-existing SMPN row). No other test failed under ASan/UBSan. A first ASan attempt ran out of disk while linking two examples and was redone with `-g0`.
- Android NDK compile: NOT done and not possible on this machine (no NDK). Nothing here claims an NDK build.

## Status table

| item | implemented | build-verified (host gcc, lavapipe) | device-verified |
|------|-------------|-------------------------------------|-----------------|
| audit-fix3 robustness fixes | yes | yes | NO |
| MSAA exact-by-default policy, compat opt-in, whole-pass rejection, advertised count | yes | yes | NO |
| MSAA host side (fetch, SampleId/Mask, a2c, sample shading, depth resolve) | yes, unwired | yes (lavapipe unit) | NO |
| hold ledger / `g7-gpu-slow` | yes | yes | NO |
| boot1 pvg-ops clock anchor | yes | yes | NO |
| blit fill 0x141 region/level/layout (0x140 only counted) | yes | yes | NO |
| atomics translation (32-bit int; 64-bit / f32 gated) | yes | yes (lavapipe, hand-written AIR) | NO |
| Android NDK build | - | NO (no NDK here) | NO |

## What the NDK CI build must still confirm

- Compile of the new sources with the NDK toolchain: `src/protocol/arm64_hold.c` (`clock_gettime`, `_Atomic`, `getenv`), `src/vulkan/msaa_host.c`, the atomic section of `src/air2spv/translate_function.c`, `src/vulkan/caps.c` (`atomic64`), and the changed `arm64_exec_run.c`, `blit_encoder.c`.
- The meson source lists pick the new files up under the Android cross file.
- No warning-as-error from `uint64_t` printf formats in the new log lines on 32-bit targets (cannot be checked here).
- A first device run is needed for everything in the table above.

## integ7b appendix

Branch `research/g7-integ7b`, base integ7 (a21e9be, not modified). Step 1: `--no-ff` merge of `origin/research/g7-atomic2` (acba1cd, based on 98d5c3a which integ7 already contains): no conflicts, no manual decisions. Full meson suite (release, clean build dir) after the merge: 170 OK / 0 fail / 1 skip (171 tests; integ7 baseline 166 / 1, +4 from atomic2). A2Xghfc pair unchanged: fs sha256 `aa5ef4ce7adf1338...`, vs `d625b8ccc7d56c49...`.

Step 2 (see `docs/spec-audit.md`, "Unlowered calls (integ7b)"):
- commit 1: unlowered-call ledger, default named refusal of non-void unlowered `llvm.*` calls (no reference-stub fallback), `llvm.umax/umin/smax/smin`, `llvm.fma/fmuladd`; new unit test `air2spv unlowered calls (unit)`, 5 + 2 fixtures; the lavapipe integer test gains the min/max/fma cases with negative controls.
- commit 2 (separately revertable): `llvm.abs/ctlz/cttz/ctpop/bitreverse/bswap/fshl/fshr` (none occurs in any scanned module) and 32-bit-only `OpBitCount` / `OpBitReverse` (also for the existing `air.popcount` / `air.reverse_bits`; `air2spv-int-intrinsics-unit` expectation 4 -> 5 for the 64-bit halves).
- `shaders` (video-encode kernel) translates without `LAGFX_ALLOW_UNDEF_CONTROL=1`, spirv-val vulkan1.2 OK, not executed.
- A2Xghfc output byte-identical after both commits (same sha256 as above); dump checks run3/z 39, x231501 40, vm2 40 OK.

New flags (all read at every translation): `LAGFX_LOG_UNLOWERED=1` (log each unlowered call site, default off), `LAGFX_ALLOW_UNLOWERED_LLVM=1` (restore typed undef for unlowered non-void `llvm.*`; default off = refuse by name), `LAGFX_STRICT_INTRINSICS=1` (also refuse non-void `air.*`; default off). New counters / API: `lagfx_air2spv_unlowered_calls`, `_unlowered_reset`, `_last_unlowered`, `_last_unlowered_refused`, `_unlowered_refused`. Default-behaviour change: only the refusal of non-void unlowered `llvm.*` calls.

Not verifiable here: Android NDK build (no NDK), any run on a device or Adreno; fused single rounding of `Fma`; the semantics of the scanned third-party modules.
