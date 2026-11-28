# Blit (encType 4) coverage in ARM64 mode: per-operation classification

Source of truth: `src/handlers/compute/blit_inner_ops.c` (dispatch table), `src/protocol/arm64_resources.c` (`lagfx_a64_blit_op`, `lagfx_a64_res_blit_segment`), `docs/g7-blit-mip.md`, `docs/arm64-opcode-comparison.md`.
Levels: **executed** = recorded into the Vulkan command buffer and checked against CPU bytes on lavapipe (`tests/arm64-exec-blit-unit.c`); **device-verified: nothing**.

| wire tag (opcode) | Metal operation | ARM64 state | class |
|---|---|---|---|
| 300 (0x12c) | copy buffer -> texture | `buf_tex_copy`, validated (bounds, row/image pitch, level/slice, depth/stencil and MS refused) | executed, host-verified |
| 301 (0x12d) | copy buffer -> buffer | `blit_301` (overlap within one buffer refused) | executed, host-verified |
| 302 (0x12e) | copy texture -> buffer | `buf_tex_copy` (host barrier, linear texture pull) | executed, host-verified |
| 303 (0x12f) | copy texture -> texture | `blit_303` (same format and type required; overlap within one image refused) | executed, host-verified |
| 306 (0x132) | fill buffer (byte) | `blit_306` (unaligned edges through staging copy; rejected fill records nothing) | executed, host-verified |
| 319 (0x13f) | fill buffer (32-bit pattern) | `blit_319` | executed, host-verified |
| 308-311 (0x134-0x137) | optimize for CPU/GPU access | counted no-op (hint) | no-op by design |
| 314, 315 (0x13a, 0x13b) | synchronize resource / texture image | counted no-op | no-op (managed storage not mirrored) [I] |
| 316, 317 (0x13c, 0x13d) | blit fence update / wait | counted no-op (one in-order command buffer) | no-op by design |
| 322, 323 (0x142, 0x143) | invalidate compressed texture | counted no-op (compressed textures not mirrored) | no-op by design |
| 307 (0x133) | generate mipmaps | **not executed**. Recorder `lagfx_mipgen_record` exists (2D, uncompressed, host-verified) but is NOT wired: the ARM64 payload is unknown | implemented-not-wired, blocked on payload |
| 321 (0x141) | fill texture with colour | **not executed in ARM64**. An x86-era handler and `lagfx_translate_blit_fill_texture_color` exist (host-verified) but the ARM64 layout is unknown | implemented-not-wired, blocked on payload |
| 320 (0x140) | fill texture with bytes | not executed (no layout anywhere) | unimplemented, payload unknown |
| 304 (0x130) | copy texture -> texture with options | not executed | unimplemented, payload unknown |
| 318 (0x13e) | copy texture -> texture with slice/level counts | not executed | unimplemented, payload unknown |
| 305 (0x131), 312, 313 (0x138, 0x139) | indirect command buffer copy / optimize / reset | not executed | unimplemented (indirect command buffers) |

Two different gaps, kept apart:
- **Wired but missing a payload layout** (307, 321): the Vulkan side exists; connecting it would require guessing byte layouts, so it is not done.
- **No implementation at all** (320, 304, 318, 305/312/313).

## What this change adds (census only, NOT a feature)
- Per-tag counters `blit_unsup_tag[]` (commands not executed for want of a layout) and `blit_rej_tag[]` (validation refused), tags 300..331.
- The first 4 payloads of every unexecuted tag are logged raw (size and up to 16 words) at warn level. A real guest run that issues 307 / 321 / 320 / 304 / 318 will show their layouts, which unblocks wiring the recorders that already exist. No guest has been observed issuing them in this repository's captures.
- This is diagnostics: it does not raise any operation's coverage.
