# Post-translation SSA promotion (opt-in)

`LAGFX_SPV_SSA` selects an optional pass over translated SPIR-V. The default is off, so the translator output is byte-identical to before. The pass lives in `src/vulkan/spv_ssa.c`.

## Modes

| `LAGFX_SPV_SSA` | Effect |
|---|---|
| unset or 0 | nothing; module untouched |
| 1 | promote Function-storage scalars and vectors to SSA values with OpPhi |
| 2 | mode 1 plus dead-code removal of pure instructions with no uses |
| 3 | mode 2 plus removal of unused Location input loads (changes which inputs the module reads, but not any output) |
| 4 | mode 2 plus aggregate (array and struct) promotion |

`LAGFX_SPV_SSA_MAXPHI=n` keeps a variable in memory when it would need more than n phis in one block, which bounds module growth on very wide merges. Counters are available through `lagfx_spv_ssa_counters`. If the result fails the internal self-check, the original module is used and the failure is counted.

## Why spirv-opt passes changed nothing

The earlier run used ssa-rewrite, scalar-replacement, eliminate-local-multi-store, combine-access-chains and inline-entry-points-exhaustive. The module declares `VariablePointersStorageBuffer` and passes pointers to functions. spirv-opt's analyses refuse to treat such pointers as simple, so those passes skip the affected variables and functions (ADCE bails out entirely when that capability is present). The module was valid in every case, so the cause is the capability, not an option mistake. The evidence is in the measurements below; see also the repository notes from the ladder runs.

## Measurements on the A2Xghfc fragment shader (reference module 348,036 B)

Counts from spirv-dis text of each output; every output passes `spirv-val --target-env vulkan1.2`.

| Variant | bytes | inst | locals | blocks | calls | ptr params | OpPtrAccessChain | phi |
|---|---|---|---|---|---|---|---|---|
| original | 348036 | 20239 | 3135 | 723 | 162 | 32 | 70 | 0 |
| SSA (mode 1) | 952484 | 12129 | 72 | 723 | 162 | 32 | 70 | 694 |
| SSAD (mode 2) | 952176 | 12114 | 72 | 723 | 162 | 32 | 61 | 694 |
| SSADA (mode 4) | 952228 | 12068 | 48 | 723 | 162 | 32 | 61 | 696 |
| SSAW8 (max phi 8) | 266128 | 14372 | 389 | 723 | 162 | 32 | 61 | 60 |
| SSAW64 (max phi 64) | 266776 | 14321 | 377 | 723 | 162 | 32 | 61 | 84 |

The byte count of the unbounded rows is larger because the phis carry many operands; the instruction count is what falls (the byte growth is probably phi operand lists and per-predecessor ids [I]). Function calls, pointer parameters and blocks are not changed by this pass; only locals, loads, stores and (in mode 2 and up) dead access chains.

## Ladder rows

The pipeline-repro ladder gains group (f): SSA, SSAD, SSADI, SSADA, SSAW8, SSAW64, SSAP, SSAP.CH10, SSAP.CH11. They are meaning-preserving, so a BUILT result is a candidate fix. The two CH rows null input loads and are diagnostic only. Generate them offline with `examples/reduce-rows`.

## Validation

`tests/spv-ssa-lavapipe.c` runs each promoted module and the original on lavapipe over three input seeds and compares the colour or buffer output bit for bit. It also mutates one OpFAdd to OpFSub in a promoted module as a negative control, which must change the output. Set `LAGFX_SSA_TEST_BIG=1` to include the two large shaders (slow JIT). `examples/spv-ssa in out [mode [maxphi]]` runs the pass on a file.
