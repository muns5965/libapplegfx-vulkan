# g7 blit: fill atomicity (tag 306/319) and generateMipmaps (tag 307)

Branch research/g7-blit-mip. Status words: implemented / host-test-verified (lavapipe) / device-verified. Nothing here is device-verified.

## 1. Fill records nothing when rejected (implemented, host-test-verified)

Files: `src/protocol/arm64_resources.c` (`blit_306`, `blit_319`), test `tests/arm64-exec-blit-unit.c` (`test_fill_reject_records_nothing`).

Problem [V]: both functions recorded the 4-aligned middle `vkCmdFillBuffer` and bumped `wgen`/`gpu_dirty`, then took the unaligned head/tail staging slot (`x->ntmp >= MAX_TMP`, `make_buf`). If that failed the function returned false (counted `blit_rejected`) with the middle already in the command buffer.

Fix [V]: order is now validate range -> reserve the staging buffer (slot check, `make_buf`, pattern bytes, slot registration) -> record every `vkCmd*` -> only then `wgen++` / `gpu_dirty = true`. A rejected fill records nothing and leaves the buffer state alone.

Audit of the other blit paths in the same file [I, by reading]: 300/302 (`buf_tex_copy`), 301, 303 validate everything before `lin_pull_x`/recording; `lin_pull_x` returns false before its own transfer is recorded and `lin_push_x` runs after the command with its result ignored by design (nothing to roll back). No other record-then-fail instance found, so nothing else was changed.

Additional ordering [V by code, not observable in a test]: `wgen` / `gpu_dirty` are private to the mirror; there is no accessor and a probe through the guest-upload path did not distinguish the old order, so that part is [I]. Forced `make_buf` failure: there is no injection hook in `make_buf`, so only the MAX_TMP exhaustion path is tested; the `make_buf` failure path shares the same early return.

Test [V]: one exec issues 64 one-byte unaligned 306 fills (each takes a staging slot, MAX_TMP is 64), then a 306 (`loc 0x2001, len 0x41`: head 3, middle 0x3c, tail 2) and a 319 (`loc 0x2800, len 0x1b`: middle 0x18, tail 3) which need a 65th/66th slot. Asserts 64 accepted + 2 rejected and that the guest-visible destination is byte-identical to the expectation (the rejected ranges still 0x11). A fresh exec then runs the same two fills, which are accepted and write exact head/middle/tail bytes. Without the src change the test fails on the destination comparison (the rejected middle was written); with it, the whole `arm64-exec-blit-unit` passes. No fault-injection hook was needed: the slot limit is reachable from the wire.

## 2. Tag 307 generateMipmaps: protocol wiring BLOCKED (payload unknown); Vulkan-side recorder implemented standalone

What this repo establishes and what it does not:

- [V] `docs/arm64-opcode-comparison.md` lists `0x133 (307) generateMipmaps` as `missing-known` with no payload layout (the row is blank; every implemented tag in the table has its layout written out).
- [V] `src/handlers/compute/blit_inner_ops.c` has `{ 0x133, "GenerateMipmaps", 4, 1, op_ack_stub }`. That table is the x86-era paravirt table, explicitly "not verified for ARM64" in the opcode-comparison doc; the `4` is a body size only.
- [V] `tests/arm64-exec-blit-unit.c` sends tag 307 with a 4-byte dummy body only to check it is counted `blit_unsupported`. No captured trace in `tests/` or `docs/` contains a real 307 record (the only 307 count in a log line is that synthetic test command).
- [I] A 4-byte body is consistent with a single u32 object ref (the texture), but it is not established whether the ARM64 record carries only that ref, whether it has level/slice ranges, or whether the serializer expands it. The upstream reference (`pvg-vulkan-wire ops/blit.rs`) is not vendored in this repo.

Unknown, needed before implementing: exact ARM64 payload size, field order, whether the ref is a texture object ref in the same table as 300..303, whether any level/slice range exists, and what Metal semantics the guest relies on (all levels, all slices). The Vulkan side (format feature checks via `vkGetPhysicalDeviceFormatProperties`, per-level layout transitions, `vkCmdBlitImage` linear box halving, refusal of depth/stencil/MS/compressed with a distinct counter) can be written from the spec once a real capture of a 307 record exists. 307 remains counted `blit_unsupported`; `lagfx_a64_blit_op` is unchanged.

### Standalone recorder (implemented, host-test-verified on lavapipe, NOT wired)

Files: `src/translate/mip_gen.{h,c}` (`lagfx_mipgen_record`, `lagfx_mipgen_stats_get/reset`), added to `src/translate/meson.build`; test `tests/blit-mipgen-unit.c` (meson test 'blit mip-chain generation ...'). Written from the Vulkan spec only.

- Checks (refusal records nothing, each has its own counter): args (null/zero/mip count above the extent's chain length/UNDEFINED entry layout), type (only 2D), multisample, depth/stencil formats, block-compressed formats, usage (TRANSFER_SRC and TRANSFER_DST required), format features via `vkGetPhysicalDeviceFormatProperties` optimal tiling (BLIT_SRC, BLIT_DST, SAMPLED_IMAGE_FILTER_LINEAR; integer formats fail here). One level = NOTHING_TO_DO (not an error, counted).
- Recording: level 0 -> TRANSFER_SRC, levels 1.. -> TRANSFER_DST, then per level `vkCmdBlitImage` i-1 -> i (VK_FILTER_LINEAR, extent max(1, dim>>1), all array layers in one region), a transfer write->read barrier DST->SRC for the level just written, and finally every level back to the entry layout with a general memory read|write access barrier. The entry layout is a parameter; the ARM64 executor keeps images in GENERAL.
- Test [V on lavapipe]: 16x4 RGBA8, 5 levels, 3 layers (height clamps to 1), each level compared to a CPU 2x2 box average of the GPU's own previous level, max difference 1/255 (tolerance +-1); layout restored (re-read in GENERAL); refusals (D32, D24S8, 4x and 2x MS, BC1, ASTC, missing SRC / DST usage, RGBA8_UINT, 3D, too many levels, one level) each under the right counter, and a refused request leaves level 3 unchanged while the accepted one propagates level 0 down to it. Validation-layer message count is asserted unchanged (only meaningful if the layer is installed on the runner [N]).
- Not covered: 3D chains, odd extents (the spec leaves linear-filter arithmetic and edge weighting to the implementation; a 2x2 box reference only holds for the power-of-two halving tested), formats beyond RGBA8, Adreno behaviour [N], anything on device [N].
- Wiring to tag 307 needs the payload (see above) plus the object lookup (`lagfx_a64_res_object`, texture kind/format/mips/layers/usage fields exist on `lagfx_a64_obj_t`) and the linear-texture pull/push; deliberately not done.

## Not verified on device
Everything above ran on lavapipe only. The Adreno behaviour of the fill ordering change is [N].
