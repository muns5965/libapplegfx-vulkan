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
| 314, 315 (0x13a, 0x13b) | synchronize resource / texture image | counted no-op | **not a hint**: see "Synchronisation" below; guest visibility comes from the exec's write-back flags, not from this tag [I] |
| 316, 317 (0x13c, 0x13d) | blit fence update / wait | counted no-op | **covered by the execution structure** [V code], see "Synchronisation" |
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

## Synchronisation: intentionally not executed vs guaranteed elsewhere
Fences (316/317) and synchronize (314/315) are NOT hints. They are skipped only because the surrounding structure already provides the ordering [V by reading code]:
- every executed blit is followed by `barrier(cb)` (ALL_COMMANDS write -> read|write memory barrier), so a later command in the same command buffer sees the copy;
- one exec records into one command buffer; a new exec is not started while the previous one is pending (`begin` waits via `pending_wait`, or is refused and counted `busy_skipped`), so two execs never overlap on the GPU;
- GPU -> host visibility of copy results uses `host_barrier` + the fence of the exec, and guest write-back is driven by the exec's resource flags.
Not established: that a guest which relies on `SynchronizeResource` for CPU visibility of a *managed* resource gets it without the write-back flags being set (needs a capture), and what a refused/skipped exec (`busy_skipped`) does to a pending fence value. Neither is device-verified.

## 307 and 321: what exists vs what is missing
| | 307 generate mipmaps | 321 fill texture with colour |
|---|---|---|
| Vulkan lowering | `lagfx_mipgen_record` (2D, uncompressed, host-verified) | `lagfx_translate_blit_fill_texture_color` (region/level/slice/layout, host-verified) |
| ARM64 payload layout | unknown (a 4-byte body is only consistent with one texture ref) | unknown (the x86-era layout has 32-bit fields; ARM64 blits use 64-bit fields) |
| object lookup | `lagfx_a64_res_object` + `lagfx_a64_obj_t` mips/layers/usage exist | the lowering takes an IOSurface, the ARM64 mirror uses `lagfx_a64_obj_t`: an adapter is also needed |
| to do after a capture | wire the ref to the recorder (linear texture pull/push) | decode the layout, write the adapter, wire it |

## 303 review (the reported sibling bug is NOT reproduced)
Checked by reading and covered by tests: format and image type equal, level / slice / box bounds, depth-stencil and multisample refused, overlap inside one image refused, linear pull before / push after, refusals counted per tag. Not tested: copies between different-but-compatible formats (refused today), whole-slice-range copies (that is tag 318), 3D to 3D, array layers other than slice 0 on device, compressed formats, behaviour on Adreno.

## What this change adds (census only, NOT a feature)
- Per-tag counters `blit_unsup_tag[]` (commands not executed for want of a layout) and `blit_rej_tag[]` (validation refused), tags 300..331.
- The first 4 payloads of every unexecuted tag are logged raw (size and up to 16 words) at warn level. A real guest run that issues 307 / 321 / 320 / 304 / 318 will show their layouts, which unblocks wiring the recorders that already exist. No guest has been observed issuing them in this repository's captures.
- The same counters are printed in the periodic / final stats as a `g7-gpu-blit:` line, which goes to the lagfx log (LAGFX_LOG_FILE) that the Android export bundles (`lagfx_log=` in the dump info); verified by a unit test that captures the diag output, not on Android.
- This is diagnostics: it does not raise any operation's coverage.
