# Tools

Standalone helpers for lagfx development.

## air_intrinsic_inventory.py

Extracts `@air.*` intrinsic inventory from a .metallib file. Pipes
LLVM bitcode through `llvm-dis` and emits a CSV.

**Dependencies:** Python 3.10+, `llvm-dis` on PATH (install with
`brew install llvm` on macOS, `apt install llvm` on Linux).

**Usage:**
```sh
python3 tools/air_intrinsic_inventory.py path/to/file.metallib --out inventory.csv
```

**Stage 50.5 gate** per `mos/memory/M5_progress_scale_5percent.md`:
the CSV must list every distinct `@air.*` intrinsic with name,
signature, callsite count, first-seen function. Expected inventory
size: ≥ 5, ≤ ~200 per metallib.

**MVP scope (this iteration):** treats the entire bitcode blob as
a single module. Per-function splitting is Stage 50.5 part 3 (needs
a `metallib_dump` C helper that calls `lagfx_metallib_get_bitcode`
per function).

## undef-census.py / spv_undef_flow.py

OpUndef census of the translated shaders of an arm64 GPU dump (census item X03).
`python3 tools/undef-census.py <build dir> <dump dir> [work dir]` re-translates the dump with
`examples/dump-translate` (as shipped, and with `LAGFX_UNDEF_NOCACHE=1` so that every OpUndef has one
creation site, attributed by `LAGFX_UNDEF_SITES=1`), walks each module with `spv_undef_flow.py`
(def-use graph over the SPIR-V binary: per-lane taint, memory per variable and constant access path,
calls analysed per call chain) and prints, per origin class and per module, how many OpUndef values
are unused / dead / lane-dead / reach an address, a branch-or-select condition, or an Output / buffer /
image store. Needs `stdbuf` (coreutils). `python3 tools/spv_undef_flow.py module.spv` prints the sinks of
each OpUndef of one module.
