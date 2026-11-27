#!/bin/sh
# Re-translates every metallib of an arm64 GPU dump with the CURRENT translator,
# validates each result with spirv-val (vulkan1.3), and checks VS/FS interface
# compatibility per pipeline. Usage: tools/check-dump-spirv.sh <build dir> <dump dir> [expected-failures file]
# expected-failures: one "<lib hash> <function>" per line that is allowed to fail
# translation. Exit 0 only if nothing else fails.
set -u
B=${1:?build dir}; D=${2:?dump dir}; X=${3:-/dev/null}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
python3 -I "$(dirname "$0")/dump_pairs.py" "$D/index.txt" > "$T/pairs.txt" || { echo "dump_pairs.py failed"; exit 1; }
mkdir "$T/spv"
"$B/examples/dump-translate" "$D" "$T/spv" "$T/pairs.txt" 2> "$T/err.txt"; rc=$?
fail=0
# translation failures not on the expected list
grep "translate failed\|module_open failed" "$T/err.txt" | while read -r lib fn rest; do
    key="${lib%.bin} ${fn%:}"; key=${key#lib_}
    grep -qxF "$key" "$X" || echo "UNEXPECTED translation failure: $key"
done > "$T/unexp.txt"
[ -s "$T/unexp.txt" ] && { cat "$T/unexp.txt"; fail=1; }
for f in "$T"/spv/*.spv; do
    spirv-val --target-env vulkan1.3 "$f" >/dev/null 2>"$T/val.txt" || { echo "spirv-val FAILED: $(basename "$f"): $(head -1 "$T/val.txt")"; fail=1; }
done
python3 "$(dirname "$0")/check-stage-interfaces.py" "$T/spv" "$D/index.txt" >/dev/null || { echo "stage-interface type mismatch"; fail=1; }
[ $fail = 0 ] && echo "dump check OK ($(ls "$T/spv" | wc -l) shaders)"
exit $fail
