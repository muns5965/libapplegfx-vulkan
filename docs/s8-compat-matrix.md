# S8 (Adreno 730, Vulkan 1.1.128) compatibility matrix

Status words: **native** (device does it as is) · **fallback done** (substitute path exists in source) · **partial** · **missing** · **host-verified** (lavapipe tests only) · **device-verified** (S8 log/pixel evidence) · **device-pending**.
Nothing below is "done" because init or submit succeeded. Source of each row: file named in the Evidence column; device numbers are from the S8 logs of 2026-11-28 (run 192949-542 and the 900 s OFF control).
Companion: `docs/metal-vulkan-compatibility.md` (API-level mapping). This file is the S8 view and is updated when a row changes status.

| Area | Item | Status | Evidence / blocker |
|---|---|---|---|
| A Shader/Pipeline | AIR→SPIR-V general | partial, host-verified | `src/air2spv/`; device: 3 shaders refused at translation (no_pipeline=10508 draws) |
| A | constexpr sampler / i64 words | missing (hypothesis only) | refused i64 constants match the sampler state word layout (`constexpr_sampler.h`); needs captured AIR (xlatereject_*.air.bc) |
| A | Int64 on a device without shaderInt64 | fallback done (narrowing, refuses real 64-bit data), host-verified | `translate_function.c` `t_narrow64` / `t_narrow_unsafe` |
| A | Atomic64 | gated: capability gate refuses without device support, no emulation | `caps.c` `lagfx_vk_atomic_caps`; strict grammar `LAGFX_ATOMIC_R_*` |
| A | Pipeline creation -13 | missing | 8 `pipefail_*` bundles on device, not yet uploaded; replay + validation layer blocked (denied build commands) |
| A | 1D array texture (offset / pixel sampling) | missing | 2D-array path warns at `translate_function.c` ~6165; needs AIR operand order |
| B Compute | Tile shader as compute in a render pass | fallback done, host-verified, device-pending | `arm64_tile.c`; device before fix: 0 dispatches, no_sg32=1220 |
| B | Subgroup 32 on 64..128 range | fallback done (logical 32-lane emulation, pinned 64), host-verified, device-pending | `lagfx_a64_tile_simd_emul_width`, `tile-simd-plan-unit` |
| B | Independent compute segment (type 1) | missing | 0/153 run; record format unknown → census (`seg_census.c`) in the pending APK |
| B | Information segment (type 4) | not analysed | 153 seen, not executed; semantics unknown |
| B | Threadgroup-memory *arguments* `[[threadgroup(n)]]` | translator done (host-verified), dispatcher not wired | Workgroup T[N], N = SpecConstant (SpecId 3+k, elements); i32/i64/float elements, indexed access only; a bare dereference refuses the kernel. The metadata key `air.threadgroup_buffer` is unverified against a captured module; no guest compute path calls it yet |
| C Resource | Usage trimming per format | fallback done, device-verified (counted) | `format_caps.c`, `fmt.dropped[]` (storage usage removed 333) |
| C | Storage image write | partial | 18 refusals; cause split (linear / format unsupported / usage not requested / view) in the pending APK; no substitute path yet |
| C | Linear (buffer-backed) texture as storage image | missing | `t->linear` returns no view |
| C | Mapped resource | partial | 120 refusals; by-site counters in the pending APK |
| C | Texture view | partial | whole-image view only (`tex_view`) |
| D Render | Legacy render pass (no dynamic rendering) | fallback done, device-verified | log: legacy_renderpass active; images non-black |
| D | Framebuffer fetch (input attachment) | partial | `legacy_fetch`; multisampled fetch refused below Vulkan 1.4 property |
| D | MSAA sampled texture read | missing | `texture2d_ms` spelling unverified: draw skipped, counted |
| D | Depth/stencil resolve | implemented (backend + guest path), host-verified, device-pending | `render_pass.c` ~224-254 (`vkCreateRenderPass2` + `VkSubpassDescriptionDepthStencilResolve`), `arm64_exec_run.c` ~1249 → `lagfx_a64_msaa_ds_resolve`; the `render_pass.h` header comment saying otherwise is stale (found by review, previously mis-listed) |
| D | Blend / MRT correctness | device-pending | `have_independent_blend`; no pixel comparison on device |
| E Display/VM | Swap / present | partial, device-verified | 757 swaps then stalled (OFF control) |
| E | VBL / display events | experiment | ON 900 s run pending |
| E | Cursor glyph | partial | guest-drawn cursor visible; capture empty |
| E | Touch / keyboard to guest | device-pending | delivery unconfirmed |
| – | Turnip comparison, Validation Layer | missing | build commands denied; not retried |

## What blocks what
- Needs external data: A (AIR bundles), B-independent compute (census dump from the next device run), C-storage (format list from the next device run).
- Can proceed from source now (corrected after review; the earlier "none" was wrong): Pipeline-creation localisation rows in the on-device matrix (stage / vertex-input / capability), the common Vulkan compute execution base (pipeline + descriptors + dispatch + barrier, separable from the guest record parsing), threadgroup-memory arguments, general Int64 lowering, texture-view mip/layer. Only the guest compute *record format* and the real -13 root cause need device data; those are not guessed.
- S26 path: every change in this branch is gated by capability (subgroup range, usage trimming) or counters only; the S26 native paths are unchanged.
