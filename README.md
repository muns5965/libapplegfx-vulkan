# libapplegfx-vulkan — Android / ARM64 research fork

[English](README.md) · [한국어](README.ko.md) · [Compatibility matrix](docs/metal-vulkan-compatibility.md) · [Original README](docs/archive/README-upstream.md) · [Upstream](https://github.com/MattJackson/libapplegfx-vulkan) · [MIT License](LICENSE)

## Project overview

This is an experimental fork of Matthew Jackson's [libapplegfx-vulkan](https://github.com/MattJackson/libapplegfx-vulkan), a clean-room, host-side reimplementation of Apple's ParavirtualizedGraphics (PVG) framework with a Vulkan backend. The library decodes the PVG wire protocol, walks guest page tables, translates Metal command streams and Apple AIR shaders (AIR → SPIR-V) to Vulkan, and renders through any Vulkan driver.

The fork has two goals: run the library on **Android ARM64**, and add an **ARM64 vmapple** protocol mode alongside the original x86 behaviour. It documents what works, what is only recognised at the protocol level, and what is missing. The Metal-to-Vulkan translation itself is **upstream work**; this fork did not write it.

**Branch:** `research/android-pfn-shift`. **Documented source revision:** `6a29b1d` (2026-11-26). **Upstream baseline:** `7574eac`.

Reading guide: *Implemented* in this document means code exists. Only *Implemented & Verified* means a test passed for this fork. Recognising an opcode, logging it, or returning a stamp is **not** Metal support; the [compatibility matrix](docs/metal-vulkan-compatibility.md) separates the two.

## Upstream versus fork

| Origin | Meaning | Examples |
| --- | --- | --- |
| Upstream implementation | Already in `7574eac` | AIR → SPIR-V translator, render/draw path, blit helpers, display, cursor, IOSurface registry, stamps, ring decoder |
| Fork implementation | Added by this fork | ARM64 vmapple protocol mode, 16 KiB ring/PFN selection, arm64 device-info table, compute-channel `0x38`/`0x44`, diagnostics |
| Fork modification | Upstream feature changed by this fork | Vulkan memory-correctness hardening, cursor glyph memory handling, `LAGFX_NO_PROCESS` |
| Planned | Not implemented | See *Roadmap* |

Per-feature origin is listed in the [compatibility matrix](docs/metal-vulkan-compatibility.md).

## Fork-specific changes

Changes after upstream `7574eac` (23 commits; the first three change the README only):

- **ARM64 vmapple protocol mode** — `lagfx_device_descriptor_t.protocol_mode` (`LAGFX_PROTO_X86` = 0 default, `LAGFX_PROTO_ARM64_VMAPPLE` = 1; any other value makes `lagfx_device_new` fail). Follows the semantics of Apple Henge `pvg_lite.c` / `pvg_metal.m`.
- **16 KiB ring pages / PFN** — `lagfx_device_descriptor_t.ring_page_shift` (0 or 12 = 4 KiB default, 14 = 16 KiB; other values fail). Applies to the root, shared and channel PFNs, ring page-array indexing, in-page offsets and stamp cells. A one-time diagnostic logs what the ring start holds if the PFN is read as 4 KiB or 16 KiB.
- **MMIO and device info** — MMIO `0x122c` returns 1 in arm64 mode (9 in x86). arm64 mode uses Apple Henge's `CmdGetDeviceInfo` table verbatim.
- **Compute-channel opcodes** — `0x38` (define host task) and `0x44` (object/placement lists) handled on compute channels; in arm64 mode `0x25` removes a `{task, ref}` object and `0x20` drops the task's object cache and never executes a command buffer.
- **Diagnostics** — command sequence, timing, raw bytes of `0x20`/`0x22`/`0x25`, last `0x44` per channel, display-submit reasons, log byte counters. No behaviour depends on them.
- **Android Vulkan memory correctness** — host-visible and coherent memory requirements for upload buffers, fallback uniform buffer and cursor glyph memory; map and flush failures propagate; cursor and display fence errors are handled before resources are released.
- **`LAGFX_NO_PROCESS`** — removes the four `system()`/`popen()` calls (debug dumps, `llc` lookup) for runners that forbid process creation.
- **Tests and docs** — new unit tests (`pfn-shift`, `protocol-mode`, `task-define`, `diag`, `android-memory-mock`), this documentation, and the [ARM64 correspondence document](docs/arm64-vmapple-correspondence.md).

## Metal-to-Vulkan support (summary)

Details, per-opcode status and evidence: [docs/metal-vulkan-compatibility.md](docs/metal-vulkan-compatibility.md).

| Area | Status | Notes |
| --- | --- | --- |
| Shader (AIR → SPIR-V) | Implemented & Verified | Android: 11 modules / 66 functions translate and validate. GPU execution verified only for `ColorFill` and `TextureCopy`. |
| Render pipeline | Partially Implemented | Pipeline build, blending, viewport and scissor implemented; most fixed-function state setters are log-only. |
| Compute pipeline | Not Implemented | No compute pipeline or dispatch. |
| Buffer | Implemented, Untested | Vertex/fragment buffer binding; buffer-to-buffer copies are stubs. |
| Texture | Partially Implemented | Fragment textures implemented; vertex textures and sampler binding are log-only. |
| Render pass | Implemented, Untested | Colour attachments; depth/stencil and MSAA unverified. |
| Blit | Partially Implemented | 2 of 24 opcodes act (`CopyFromTextureToTexture`, `FillTextureWithColor`); 22 acknowledge only. |
| Draw | Partially Implemented | 5 draw forms reach `vkCmdDraw`/`vkCmdDrawIndexed`; instanced-base, patch and indirect draws are log-only. |
| IOSurface | Partially Implemented | Registry and on-demand `VkImage` surfaces; the opcode handlers do not allocate `VkImage`. |
| Synchronization | Stub / Placeholder | Stamps/IRQ work at protocol level only. Metal fences and barriers are log-only; `CmdSynchronizeResources` returns not-implemented. |
| Display | Implemented, Untested | Swap mapping, scanout, readback. Mode commit (`0x11`) is log-ack. |
| Cursor | Implemented, Untested | Glyph upload and overlay. MMIO `0x1220` cursor move not implemented. |

Info replies (device info, compute info, pipeline info) report what the guest may use. They are protocol answers, **not** evidence that Vulkan can perform the feature.

## ARM64 vmapple support

**Status: Experimental.** The arm64 behaviour is unit-tested only (the unit tests run in a build without the Vulkan loader, so Vulkan-dependent code is not exercised by them). It has not been run against a live ARM64 guest, and no APK contains it.

| Item | x86 mode (default) | arm64 vmapple mode |
| --- | --- | --- |
| Selection | `protocol_mode = 0` | `protocol_mode = 1` |
| Ring page size | 4 KiB | independent: `ring_page_shift` 12 or 14 |
| MMIO `0x122c` | 9 | 1 |
| `CmdGetDeviceInfo` | x86 table | Apple Henge table (verbatim) |
| Compute `0x20` | command-buffer walker (`CmdExecIndirect2`) | drops task object cache; never executes |
| Compute `0x25` | registers an object on the first live task (fallback) | removes `{task, ref}` |
| Compute `0x37` | command-buffer walker | command-buffer walker (unchanged) |
| Compute `0x38`, `0x44` | handled | handled |

The `0x44` object and placement lists are only recorded; nothing consumes them yet. Metal command execution in arm64 mode is reachable only through `0x37`; how an ARM64 guest submits Metal command buffers is not yet mapped in this library.

### Compatibility and limits: x86

- With a default descriptor (`protocol_mode = 0`, `ring_page_shift = 0`) the x86 behaviour is intended to be unchanged, and `protocol-mode-unit` checks the x86 side of `0x122c` and `0x25`. The x86 `0x20` execution path is not covered by that test.
- Two changes apply to **both** modes and are not verified against an x86 guest: `CmdDefineHostTask` (`0x38`) now reads its arguments after the `arg_count_8b × 8` barrier-slot bytes and rejects task ids ≥ 512; the compute dispatcher now also handles `0x38` and `0x44`.
- Diagnostic hooks run in both modes.

### Compatibility and limits: ARM64

- Known mismatches against Apple Henge, recorded and **not fixed** (see the [correspondence document](docs/arm64-vmapple-correspondence.md)): MMIO `0x100c` read (Henge runs the root ring on read and returns the cumulative read pointer), `0x1014` and `0x1018` semantics, `0x1220` cursor move, storage of registers `0x1000–0x1fff` (only `0x1000–0x103f` kept), and root read-pointer arithmetic (cumulative `u32` in Henge, wrapped here).
- In arm64 mode the device-info table matches Henge's, but 26 of its 39 keys differ from the x86 table; the library has not been run against a guest that consumes it.
- The correct ring page shift for a real guest is still under investigation (the one-time PFN diagnostic exists for that reason).

## Android support

- **Status as of 2026-11-25:** Android ARM64 translation and selected GPU rendering verified; the full Metal API and live PVG compatibility are not established.
- Tested on Samsung Galaxy S26 Ultra, Android 17, Adreno 840, NDK r27c, arm64-v8a, API 30+. Results below are developer-reported; they are standalone GPU tests, **not** a live PVG session.
- Android build glue and the test harness live in the [Henge](https://github.com/muns5965/Henge/tree/research/android-port) repository, not in this fork. Henge's own development status is out of scope here.
- Fork changes for Android: memory-correctness hardening, `LAGFX_NO_PROCESS`, 16 KiB PFN support. APK `.bc` assets must be renamed (`.airdata`) to install on the tested device.

## Compatibility matrix

Full per-feature table with origin, kind, status and evidence: [docs/metal-vulkan-compatibility.md](docs/metal-vulkan-compatibility.md) ([한국어](docs/metal-vulkan-compatibility.ko.md)).

## Known limitations

- No compute shaders or dispatch; no tessellation, indirect or nested command buffers.
- 22 of 24 blit opcodes, all Metal fences and barriers, most fixed-function state setters, vertex textures and sampler binding are log-only.
- `CmdSynchronizeResources` returns not-implemented; stamps are signalled after parsing, not after GPU completion.
- `CmdDisplaySetProperties` (mode commit) is log-ack only.
- Pixel-format coverage, depth/stencil attachments and MSAA are unverified; patterned-texture and UV accuracy are untested.
- Login-screen shaders other than `ColorFill` and `TextureCopy` have not been executed on a GPU; the real login vertex+fragment pair is untested.
- ARM64 mode: Experimental; open MMIO mismatches listed above; Metal command execution path in arm64 mode not mapped.
- No live PVG or macOS desktop rendering on Android.

## Testing status

| Layer | Status | Detail |
| --- | --- | --- |
| Unit tests | Partially verified | 2026-11-26, x86-64 Linux, gcc 13.3, **no Vulkan loader**: 74 tests, 72 passed, 0 failed, 2 skipped. Vulkan-gated code was not compiled or run. Fork-added `pfn-shift`, `protocol-mode`, `task-define` and `diag` unit tests passed. `android-memory-mock` needs Vulkan headers and was not run. |
| CI | Not verified | Workflow exists (Ubuntu debug/release with lavapipe, Alpine, macOS, sanitizers) but triggers only on pushes to `main` and `research/android-memory-correctness`, pull requests to `main`, and manual runs. GitHub shows **0 workflow runs** for this fork as of 2026-11-26. |
| APK build | Verified for APK 4.4 | Henge [CI run 45575251001](https://github.com/muns5965/Henge/actions/runs/45575251001) succeeded against library `7574eac`. That run is the stand-alone GPU test APK (library `7574eac`), not the VM runner. The VM runner APK was built against library `c53dcb5`, which already contains the 16 KiB PFN support (confirmed on a device in the clone-001 run). The arm64-mode, device-info, task-lookup and 0x20/0x25 changes are in no APK yet. |
| Real device | Partially verified | Galaxy S26 Ultra, standalone GPU tests (below). Live PVG traffic with a macOS guest (16 KiB PFN rings, command intake) was observed in VM clone runs; arm64 mode, real PVG rendering and desktop output: not verified. |

## Roadmap

Derived from open items in the source and documents. It is not a schedule.

- Fix or decide the open ARM64 MMIO mismatches (`0x100c`, `0x1014`/`0x1018`, `0x1220`, register storage, root read pointer) and map ARM64 command-buffer submission.
- Run arm64 mode and the 16 KiB ring against a real ARM64 guest; settle the ring page shift.
- Implement the remaining blit opcodes, Metal fences/barriers and `CmdSynchronizeResources`; bind sampler states and vertex textures; wire the fixed-function state setters.
- Compute pipeline and dispatch.
- GPU execution tests for the remaining login-screen shaders, patterned textures and UV accuracy.
- Run CI on this branch (add it to the workflow triggers) and build an APK that contains the arm64-mode changes.
- Keep this README and the compatibility matrix updated in the same commit or PR as any feature or status change.

## Android validation results

### Test environment and reproducibility

- Samsung Galaxy S26 Ultra, Android 17, Qualcomm Adreno 840.
- Tested library revision: [7574eac](https://github.com/muns5965/libapplegfx-vulkan/tree/7574eac0d7396a9f49366f007c7fbeb44245826c).
- Android NDK r27c, arm64-v8a, API 30 minimum; Vulkan 1.2 SPIR-V host validation.
- Standalone Henge GPU test APK 4.4; separate from the VM. Android build glue and test harness currently live in Henge, not in this fork.
- [Test source](https://github.com/muns5965/Henge/tree/fe6064e5ad415efd400572489216731855bfe0cc/tests/gpu-android-gpt) · [Successful CI build](https://github.com/muns5965/Henge/actions/runs/45575251001) · [Download APK 4.4](https://github.com/muns5965/Henge/actions/runs/45575251001/artifacts/15004136515).
- Install and run the six-stage GPU test, the 11-module corpus translation test, then the ColorFill / TextureCopy GPU output test. Artifacts may expire or require sign-in.

### Android compatibility results

PASS = observed success (including expected negative-control behavior); FAIL = unexpected test failure; FIXED = previously observed issue resolved by a documented change; NOT TESTED = no completed verification. Results are developer-reported physical-device tests, not universal guarantees. Expected negative-control failures demonstrate that the draw is required and are not functional regressions.

| Test | Result | Evidence |
| --- | --- | --- |
| ARM64 NDK build and native execution | ✅ PASS | Android 17 |
| Vulkan dynamic dispatch | ✅ PASS | Adreno 840 |
| 1. AIR triangle | ✅ PASS | 3,724/3,724 |
| 2. Texture sampling (GLSL) | ✅ PASS | 4,096/4,096 |
| 3. Alpha blending (GLSL) | ✅ PASS | 4,096/4,096 |
| 4. FP16 render and conversion (GLSL) | ✅ PASS | 4,096 raw + 4,096 converted |
| 5. Fresh on-device AIR translation and draw | ✅ PASS | SHA-256 match; 3,724/3,724 |
| 6. Library render target clear/readback | ✅ PASS | 4,096/4,096 |
| 7. Real Metal ColorFill | ✅ PASS | 4,096/4,096; 00FF00FF |
| 8. Real Metal TextureCopy | ✅ PASS | 4,096/4,096; 4080FFFF |
| Android APK AIR asset naming (.bc → .airdata) | ✅ FIXED | APK installation rejected with .bc assets (4.2 / control B); identical AIR bytes renamed to .airdata install and execute successfully (4.3/4.4). Packaging limitation, not an AIR translator or GPU failure. |
| Neutral .airdata APK assets (4.3/4.4) | ✅ PASS | Installed / 설치 성공 |
| ColorFill negative control (draw omitted) | ✅ PASS | Expected 4,096/4,096 mismatches and exit 1 observed |
| TextureCopy negative control (draw omitted) | ✅ PASS | Expected 4,096/4,096 mismatches and exit 1 observed |
| Real login vertex + fragment pair | ⬜ NOT TESTED | GLSL reference vertex |
| Patterned Metal texture / UV accuracy | ⬜ NOT TESTED | Uniform texture only |
| UberComposite, Vfx, Xgc GPU rendering | ⬜ NOT TESTED | Translation only |
| Live Henge PVG / desktop integration | ⬜ NOT TESTED | Standalone APK |

### Real macOS login AIR shader corpus

11 modules and 66 defined-function translations reported successful. Emitted SPIR-V matched the Linux host SHA-256 and passed host `spirv-val --target-env vulkan1.2`. Xgc's size is its main function; the other 55 reported functions were translated separately, not independently GPU-executed.

| Function/module | Stage | Translation | Host hash / validation | Main SPIR-V bytes | GPU render |
| --- | --- | --- | --- | ---: | --- |
| ViewportToNDC | Vertex | ✅ 1/1 PASS | ✅ PASS | 2,056 | ⬜ NOT TESTED |
| UberCompositeVertex | Vertex | ✅ 1/1 PASS | ✅ PASS | 1,952 | ⬜ NOT TESTED |
| Vfx | Vertex | ✅ 1/1 PASS | ✅ PASS | 2,136 | ⬜ NOT TESTED |
| ColorFill | Fragment | ✅ 1/1 PASS | ✅ PASS | 900 | ✅ PASS |
| TextureCopy | Fragment | ✅ 1/1 PASS | ✅ PASS | 1,136 | ✅ PASS |
| UberCompositeFragment A | Fragment | ✅ 1/1 PASS | ✅ PASS | 1,544 | ⬜ NOT TESTED |
| UberCompositeFragment B | Fragment | ✅ 1/1 PASS | ✅ PASS | 1,524 | ⬜ NOT TESTED |
| VfxU11 | Vertex | ✅ 1/1 PASS | ✅ PASS | 2,940 | ⬜ NOT TESTED |
| TvcmXc_Isrc | Fragment | ✅ 1/1 PASS | ✅ PASS | 6,740 | ⬜ NOT TESTED |
| VfxXgb | Vertex | ✅ 1/1 PASS | ✅ PASS | 10,208 | ⬜ NOT TESTED |
| Xgc | Fragment | ✅ 56/56 PASS | ✅ PASS | 337,892 | ⬜ NOT TESTED |

### Actual GPU rendering

ColorFill reads a StorageBuffer; TextureCopy samples an image with a separate sampler. Both use freshly translated Metal fragment shaders with a GLSL reference vertex. Each produced 4,096/4,096 matching pixels, totaling 8,192/8,192. Omitting the draw produced 8,192/8,192 mismatches and exit code 1 in both controls.

The uniform-color TextureCopy test does not establish general UV accuracy. Successful translation and SPIR-V validation do not prove full Metal semantics. Real login vertex pairing, complex composition, live PVG commands, and macOS desktop rendering remain unverified.

### Build and upstream information

The upstream Linux build uses C11, Meson, Ninja and Vulkan headers/loader:

```sh
meson setup builddir
meson compile -C builddir
meson test -C builddir --print-errorlogs
```

For the Android cross-build, use the pinned [Henge workflow](https://github.com/muns5965/Henge/blob/fe6064e5ad415efd400572489216731855bfe0cc/.github/workflows/gpt-gpu-test.yml). It builds the actual Vulkan-enabled static library and packages the native test executables. AIR fixtures use a neutral `.airdata` suffix. APKs containing the original `.bc`-suffixed assets were rejected on the tested Android device; renaming the assets without changing their bytes resolved installation. Android's legacy RenderScript `.bc` detection is the likely explanation, but the exact package-manager rejection code was not captured. This is an APK asset filename-extension / packaging issue, not a demonstrated filesystem path or AIR conversion defect. The same AIR contents passed translation and selected GPU rendering tests after renaming; `.bc` packaging itself remains unverified as installable.

For the original Linux/QEMU architecture, API, dependencies, milestones, contributing rules, and upstream test descriptions, see the [archived original README](docs/archive/README-upstream.md). Upstream Linux results are not Android test results.

## Credits and license

Original library by Matthew Jackson ([MattJackson](https://github.com/MattJackson)); copyright notices in the source files and the [MIT License](LICENSE) are unchanged. This fork adds the changes listed above. Android test integration is developed in the Henge fork. Third-party Apple shader fixtures are test inputs; no separate license grant for Apple's material is asserted. See the upstream [contribution guidelines](CONTRIBUTING.md) and [security policy](SECURITY.md).
