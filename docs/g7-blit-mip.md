# g7 blit: fill atomicity (tag 306/319) and generateMipmaps (tag 307)

Branch research/g7-blit-mip. Status words: implemented / host-test-verified (lavapipe) / device-verified. Nothing here is device-verified.

## 1. Fill records nothing when rejected (implemented, host-test-verified)

Files: `src/protocol/arm64_resources.c` (`blit_306`, `blit_319`), test `tests/arm64-exec-blit-unit.c` (`test_fill_reject_records_nothing`).

Problem [V]: both functions recorded the 4-aligned middle `vkCmdFillBuffer` and bumped `wgen`/`gpu_dirty`, then took the unaligned head/tail staging slot (`x->ntmp >= MAX_TMP`, `make_buf`). If that failed the function returned false (counted `blit_rejected`) with the middle already in the command buffer.

Fix [V]: order is now validate range -> reserve the staging buffer (slot check, `make_buf`, pattern bytes, slot registration) -> record every `vkCmd*` -> only then `wgen++` / `gpu_dirty = true`. A rejected fill records nothing and leaves the buffer state alone.

Audit of the other blit paths in the same file [I, by reading]: 300/302 (`buf_tex_copy`), 301, 303 validate everything before `lin_pull_x`/recording; `lin_pull_x` returns false before its own transfer is recorded and `lin_push_x` runs after the command with its result ignored by design (nothing to roll back). No other record-then-fail instance found, so nothing else was changed.

Test [V]: one exec issues 64 one-byte unaligned 306 fills (each takes a staging slot, MAX_TMP is 64), then a 306 (`loc 0x2001, len 0x41`: head 3, middle 0x3c, tail 2) and a 319 (`loc 0x2800, len 0x1b`: middle 0x18, tail 3) which need a 65th/66th slot. Asserts 64 accepted + 2 rejected and that the guest-visible destination is byte-identical to the expectation (the rejected ranges still 0x11). A fresh exec then runs the same two fills, which are accepted and write exact head/middle/tail bytes. Without the src change the test fails on the destination comparison (the rejected middle was written); with it, the whole `arm64-exec-blit-unit` passes. No fault-injection hook was needed: the slot limit is reachable from the wire.

## 2. Tag 307 generateMipmaps: NOT implemented, payload unknown

Per the task, stopped before implementing. What this repo establishes and what it does not:

- [V] `docs/arm64-opcode-comparison.md` lists `0x133 (307) generateMipmaps` as `missing-known` with no payload layout (the row is blank; every implemented tag in the table has its layout written out).
- [V] `src/handlers/compute/blit_inner_ops.c` has `{ 0x133, "GenerateMipmaps", 4, 1, op_ack_stub }`. That table is the x86-era paravirt table, explicitly "not verified for ARM64" in the opcode-comparison doc; the `4` is a body size only.
- [V] `tests/arm64-exec-blit-unit.c` sends tag 307 with a 4-byte dummy body only to check it is counted `blit_unsupported`. No captured trace in `tests/` or `docs/` contains a real 307 record (the only 307 count in a log line is that synthetic test command).
- [I] A 4-byte body is consistent with a single u32 object ref (the texture), but it is not established whether the ARM64 record carries only that ref, whether it has level/slice ranges, or whether the serializer expands it. The upstream reference (`pvg-vulkan-wire ops/blit.rs`) is not vendored in this repo.

Unknown, needed before implementing: exact ARM64 payload size, field order, whether the ref is a texture object ref in the same table as 300..303, whether any level/slice range exists, and what Metal semantics the guest relies on (all levels, all slices). The Vulkan side (format feature checks via `vkGetPhysicalDeviceFormatProperties`, per-level layout transitions, `vkCmdBlitImage` linear box halving, refusal of depth/stencil/MS/compressed with a distinct counter) can be written from the spec once a real capture of a 307 record exists. 307 remains counted `blit_unsupported`.

## Not verified on device
Everything above ran on lavapipe only. The Adreno behaviour of the fill ordering change is [N].
