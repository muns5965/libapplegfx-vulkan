# integ6: integration of ten g7 branches on top of integ5

Branch `research/g7-integ6`, base `origin/research/g7-integ5` (3ca2128). Ten `--no-ff` merges, in this order, the FULL meson suite (release build, one build dir reused) run after each
merge. The per-step numbers below are the whole suite at that step, not a sum of per-branch numbers.

## Merged branches and per-step results

| # | branch | head SHA | conflicts | OK / Fail / Skip | tests |
|---|--------|----------|-----------|------------------|-------|
| 1 | g7-audit-fix2 (contains g7-audit-fix + g7-spec-audit) | 4870710 | none | 149 / 0 / 1 | 150 |
| 2 | g7-texview | 48045e1 | none | 151 / 0 / 1 | 152 |
| 3 | g7-f2-readback | 5c6f705 | none | 152 / 0 / 1 | 153 |
| 4 | g7-gapdiag | aff4271 | tests/meson.build | 153 / 0 / 1 | 154 |
| 5 | g7-gamma | c5a2526 | src/protocol/diag.c, src/protocol/meson.build | 154 / 0 / 1 | 155 |
| 6 | g7-addrfix | d765fe0 | src/protocol/meson.build; semantic: gamma-diag-unit premise | 155 / 0 / 1 (154 / 1 / 1 before the test update) | 156 |
| 7 | g7-msaa | da772ee | arm64_exec_run.c, arm64_resources.c, tests/meson.build; build: SRCDIR | 157 / 0 / 1 | 158 |
| 8 | g7-qcom-tile | dacc194 | none | 158 / 0 / 1 | 159 |
| 9 | g7-qcom-probe | 95c6967 | tests/meson.build | 159 / 0 / 1 | 160 |
| 10 | g7-ssa | 067b7b3 | spv_reduce.c, spv_reduce.h, tests/meson.build; semantic: LRD bit collision | 160 / 0 / 1 | 161 |

The one Skip is `pipefail bundles` (needs `LAGFX_PIPEFAIL_CHECK_DIR`). The final tip (with the two UBSan guards below) was re-run: see the end of this file for its numbers.

## Conflict decisions

- **tests/meson.build** (steps 4, 7, 9, 10): every branch appends a block at the end of the file and the blocks share the final `endif`. Resolution: keep all test blocks, close the earlier block with its own `endif`.
- **src/protocol/meson.build** (steps 5, 6): both sides add a source line. Kept all: `gap_diag.c`, `gamma_diag.c`, `xlate_guard.c`.
- **src/protocol/diag.c** (step 5, gapdiag vs gamma): includes only; kept `gap_diag.h`, `gamma_diag.h`, `arm64_exec_decode.h`, `task_translate.h`. Gamma's `lagfx_gd_task_read` / `dd_read_region` stays the gamma-region reader; the gapdiag hooks are untouched. The addrfix merge (step 6) auto-merged in diag.c with no change.
- **gamma read vs addrfix redirect (step 6)**. `lagfx_task_read_virtual` in arm64 mode now redirects to `lagfx_arm64_task_read` and counts one `redirected` entry in the xlate guard. The gamma reader `lagfx_gd_task_read` already chooses `lagfx_arm64_task_read` itself in arm64 mode (and the legacy walk only in x86 mode), so after the merge the gamma read does NOT enter the legacy API at all: `dd_read_region` is read once and counted neither as `legacy_xlate_in_arm64` nor as `redirected`. Any other caller of `lagfx_task_read_virtual` in arm64 mode counts as `redirected` (not `blocked`). Both facts are asserted in `tests/gamma-diag-unit.c`. The comment in `object_resolver.c` that called gamma the "only known arm64 caller" was corrected.
- **tests/gamma-diag-unit.c premise (step 6)**: its "fail before" assertions called `lagfx_task_read_virtual` on an arm64 protocol and expected the legacy 4 KiB walk to fail. addrfix makes that call succeed (redirect), so the premise was invalid. The old geometry is now exercised through `legacy_geom_read` (same tables presented with `protocol_mode = 0` for the call), and a new assertion covers the redirect and the counters. No production code changed for this.
- **arm64_exec_run.c (step 7, gapdiag vs msaa)**: both lines kept (`lagfx_gap_note_pass(...)` loop, then the `ms_passes++` counter).
- **arm64_resources.c (step 7, texview vs msaa)**: texview split `lagfx_a64_res_sample_view` into `base_sample_view` + mip-chain view + `lagfx_a64_res_sample_view_for`; msaa added "a multisampled image is not a sampled 2D texture" at the top of the old function. Kept texview's structure and put the msaa `samples > 1` refusal at the top of `base_sample_view` (which every non-3D path of the new code goes through) and added `o->samples <= 1u` to the 2D-array view condition, so no view of a multisampled image can be created by any texview path.
- **tests/meson.build, msaa-guest (step 7)**: the msaa guest test includes the render-compat harness, which (from spec-audit) reads `tests/fixtures` via `SRCDIR`; added `-DSRCDIR` to that executable.
- **LRD bit collision (step 10, ssa vs audit-fix2)**: g7-ssa takes `1<<28..31` for its four SSA flags, g7-audit-fix2 had taken `1<<28` for `LRD_NANMAX`. All 32 bits were in use. Resolution: `LRD_NANMAX` keeps bit 28 (the recipe table and docs refer to it); the SSA flags move to bits 29..32 (macros, `1ull<<n`); `lagfx_spv_reduce_t.bits`, the `B` locals of spv_reduce.c, `post_prune`'s argument and the SSA recipe table are `uint64_t`. Stats-line additions of both sides (`nanmax` counter and the SSA info string) and the "changes nothing" test (`LRD_POST_MASK | LRD_SSA_MASK`) are kept. Behaviour of every pre-existing row is unchanged (all their bits are below 32).
- instance.c / devinfo.c (qcom-tile, texview cubic, audit demote) and arm64_pipeline.c (ssa hook next to the audit/texview changes) merged without conflict.

Fixes found by the ASan/UBSan run (both are pre-existing UB in branch code, `memcpy(dst, NULL, 0)`, fixed with an `if (n)` guard):
- `src/vulkan/spv_ssa.c` cfg_build: `memcpy(sl + tot, tmp, 0)` with `sl == NULL` (made the reduction ladder abort under UBSan before it reached its known crash row).
- `tools/qcom-tile-probe/qp_spv.h` qp_finish: empty section with `w == NULL` (made the qcom-tile-probe E5/E6 children abort under UBSan).

## Flags and defaults

Nothing below is on unless stated. With no environment flags a lavapipe run prints `g7-gpu-qcomtile: flag=0 ... decision=flag_off`, no `g7-gpu-cubic` and no `g7-gpu-readback` line, and no SSA / NaN-max rewriting.

| flag | branch | default | effect |
|------|--------|---------|--------|
| `LAGFX_QCOM_TILE` (`=1`) | qcom-tile | OFF | VK_QCOM_tile_shading use; without it only the read-only `g7-gpu-qcomtile` caps/decision line (`flag=0`) |
| `LAGFX_QCOM_TILE_INJECT` | qcom-tile | OFF | test-only capability injection |
| `LAGFX_CUBIC` (`=1`) | texview | OFF | bicubic via VK_EXT_filter_cubic; `g7-gpu-cubic` line printed only when requested |
| `LAGFX_GPU_READBACK` (`=1`), `LAGFX_GPU_READBACK_EVERY`, `LAGFX_WB_VERIFY` | f2-readback | OFF | independent GPU readback verification of sampled swaps; `LAGFX_WB_VERIFY` alone does not enable it |
| `LAGFX_SPV_SSA` (`1..4`), `LAGFX_SPV_SSA_MAXPHI` | ssa | OFF | Function variables -> SSA after translation; unset = translator output byte for byte |
| `LAGFX_SPV_NANMAX` (`=1`) | audit-fix2 | OFF | FMin/FMax/FClamp -> NMin/NMax/NClamp |
| `LAGFX_GAPDIAG` (`=0` disables), `LAGFX_GAPDIAG_GAP_MS` | gapdiag | ON (light, bounded: 20 s gap threshold, capped line counts) | idle-gap diagnostics (`g7-gap*`, `g7-op09`) |
| `LAGFX_GD_RAM_BASE` | gamma | probe off unless set; gamma diag lines are bounded and read-only | gamma descriptor diagnostics (`g7-pvg-gamma`) |
| (none) xlate guard | addrfix | ON in arm64 mode only (x86 mode: no-op) | legacy 4 KiB translators refused / redirected; root 0x20 and compute 0x37 counted and rejected; `g7-gpu-xlate` line |
| (none) audit fixes | audit-fix2 | ON | srem, vec3 layout, instance id, demote etc. (docs/spec-audit.md) |
| (none) msaa | msaa | follows the guest's texture sample count (no env flag); 1x fallback is counted | guest-visible MSAA on the ARM64 exec path |

## Device verification

Nothing new. Every item above was verified on lavapipe / offline only; nothing in this integration was run on a device (Adreno or otherwise). The qcom-tile, cubic, readback and SSA paths are default-off and unproven on hardware.

## Final checks (integ6 tip)

- Full meson, release: see the table at the end of this file.
- `tools/check-dump-spirv.sh`: /tmp/run3/z/dump 39 shaders, 0 failed / 0 interface mismatch; x231501 40 shaders OK; vm2 40 shaders OK (x230109: no dump input available, only spv outputs).
- `examples/air-pair-translate` on /tmp/ana-bf/input.{vs,fs}.bc: fs sha256 aa5ef4ce7adf..., vs sha256 d625b8ccc7d5... (unchanged, equal to the manifest).
- ASan + UBSan build of the whole suite: all pass except the known SMPN crash row of `pipeline reduction ladder` (SEGV, signal 11, exit status 1), the only failure after the two guards above.
- Android NDK compile: not done (no NDK on the build machine).

## Open items

- `pipeline reduction ladder`: known SMPN crash row under ASan (lavapipe), unchanged.
- Android NDK compile of the integrated tree still to be done on a machine with an NDK.
- No device verification of any of: QCOM tile shading, cubic filtering, GPU readback, SSA, NaN-max, MSAA, addrfix/xlate guard behaviour on a real guest, gapdiag/gamma output on a real run.
- `LRD_*` mask is now 64 bit; any out-of-tree consumer of `lagfx_spv_reduce_t.bits` as `uint32_t` must be widened.
- The xlate-guard statistics are a separate `g7-gpu-xlate` line (not part of the diag.c stats line).

## Final tip, full meson (release)

| step | OK / Fail / Skip | tests |
|------|------------------|-------|
| tip after the UBSan guards and this document | 160 / 0 / 1 | 161 |
