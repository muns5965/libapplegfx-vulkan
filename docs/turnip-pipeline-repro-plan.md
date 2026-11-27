# Plan: loading Mesa Turnip in the Pipeline Repro app only

Status: design only. Nothing in this repository loads a replacement GPU driver, and this document does not change that.
Tags: [V] verified (file or command output read in this session), [I] inferred, [N] not checked.

## 1. Purpose

One fragment shader (the translated A2Xghfc module, 348,036 B of SPIR-V) is rejected by the vendor driver at `vkCreateGraphicsPipelines` with `VK_ERROR_INITIALIZATION_FAILED` (-13) on a real Adreno 8xx device. Pipeline-state variants and the interface audit were clean. The open question is whether the failure belongs to the vendor shader compiler or to the module. A second, independent compiler on the same GPU answers that. Mesa Turnip is the only open Vulkan driver for Adreno, and Mesa 26.0 carries experimental A8xx support [I: from release notes read earlier, re-check the exact release before use].

Scope: the Pipeline Repro executable only (the diagnostic that builds the module with the reduction ladder). The renderer, the library and the default code path stay on the system driver.

## 2. How an app can load a different Vulkan driver

- [V] libadrenotools (github.com/bylaws/libadrenotools) is a library for rootless Adreno driver replacement. Its README states that loading custom drivers such as Turnip is a supported use, that it needs Android 9+ and arm64, and that each app must opt in itself.
- [V] The entry point is `adrenotools_open_libvulkan(dlopenMode, featureFlags, tmpLibDir, hookLibDir, customDriverDir, customDriverName, fileRedirectDir, userMappingHandle)` in `include/adrenotools/driver.h`. It returns a `dlopen` handle for a libvulkan that resolves to the custom driver. Flags select the custom driver, file redirection (for shader dumps and driver config) and a GPU mapping import.
- [V] It depends on liblinkernsbypass as a git submodule (`lib/linkernsbypass`). The bypass lets an app `dlopen` vendor libraries that the linker namespace would otherwise hide.
- Flow for the Repro: stage `libvulkan_freedreno.so` (renamed to a soname the app chooses) and its ICD data in the app's private files directory, call `adrenotools_open_libvulkan` before any Vulkan use, take `vkGetInstanceProcAddr` from the returned handle, and run the same `lagfx_pipeline_reduce_run` ladder against it. [I]
- Everything the repro needs afterwards (`vkCreateShaderModule`, `vkCreateGraphicsPipelines`) is core Vulkan, so no library code needs to know which driver is underneath. The repro would need an instance-proc-addr injection point; the library presently links the system loader [N: not inspected for this plan].

## 3. Licensing

| Component | License | Use here |
|---|---|---|
| libadrenotools | BSD 2-Clause, Copyright (c) 2021 Billy Laws [V: LICENSE read from the repository] | allowed as a dependency |
| liblinkernsbypass | BSD 2-Clause, same copyright holder [V] | allowed as a dependency |
| Mesa (Turnip) | MIT [I: Mesa's own license for freedreno/turnip; verify the license file of the exact release you ship] | allowed as a prebuilt binary or built from source |
| This library | MIT | unchanged |

Obligations: BSD-2 requires keeping the copyright notice and license text with source and with binary redistribution, so ship a NOTICE entry inside the repro APK. Mesa's MIT notice goes in the same file. Neither license is copyleft. This plan does not use any GPL or LGPL code as an implementation basis. Check the LICENSE of every transitive submodule at the pinned commit before the first build [N].

## 4. Risks

1. A8xx support is experimental. A crash, hang or `VK_ERROR_DEVICE_LOST` inside Turnip says nothing about the shader. Treat it as case 3 below.
2. Kernel interface. Turnip talks to the KGSL node on Android. The app must be allowed to open it. Stock vendor drivers do that from an untrusted app, so it should work, but SELinux policy on a given build is [N].
3. Build. Turnip has to be cross-built with the NDK against bionic (Mesa documents an Android NDK build). The binary must match the device ABI and the Vulkan loader expectations (`hwvulkan`/ICD layout). A mismatch gives case 4.
4. Driver and firmware pairing. A newer userspace driver on an older firmware or kernel can fail at device creation, which again is case 4.
5. Shader features. The module uses VariablePointersStorageBuffer, 64-bit integers and 16-bit floats [I]. Turnip may lack a capability and reject the module at `vkCreateShaderModule` for a reason unrelated to the vendor failure. Record the exact error string.
6. Two different compilers can disagree on a legal module, so a pass on Turnip proves the module is likely valid, not that it is optimal.
7. Distribution. The repro APK is a diagnostic. Shipping a replacement driver inside a store release is a policy and support decision outside this plan.
8. Provenance and privacy. Use an upstream Mesa release or a build whose commit is recorded. Do not upload device identifiers with the results.

## 5. Interpreting the result

The reference row is the unmodified A2Xghfc module on the system driver: -13 (28 of 811 draws skipped in the existing run).

| Case | Turnip result on the same module and state | Conclusion | Next step |
|---|---|---|---|
| 1 | Pipeline builds | The module is accepted by an independent compiler on the same GPU family. The -13 is specific to the vendor compiler or its limits. Strong evidence against a translator bug. | Run the SSA and reduce2 rows on the vendor driver and file the smallest failing module. |
| 2 | Clean rejection (a Vulkan error or a compiler message) | The module hits something both compilers dislike (size, features, a real invalidity). | Read the message, map it to the module, check `spirv-val` and the SSA rows. |
| 3 | Crash, hang or device lost inside Turnip | Inconclusive. Experimental A8xx code is not a reliable oracle. | Retry with the SSA-promoted module and with the smaller ladder rows. If those build, size or structure drives the failure. |
| 4 | Driver does not load, device not created, or capability missing | No information about the shader. | Fix loading (ABI, KGSL access, version pairing) before reading anything into it. |

Rows 1 and 2 are the only ones that move the investigation. Cases 3 and 4 must never be reported as "Turnip also fails".

## 6. Work items (not started)

- Add a Repro-only setting that selects the driver. Default stays the system loader.
- Vendored third-party notices.
- A Turnip build recipe pinned to one Mesa release and one NDK.
- A result line in the repro log that names the driver and its version string, so rows from different drivers cannot be mixed.
