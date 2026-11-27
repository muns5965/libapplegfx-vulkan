#!/bin/sh
# Replays the PIPELINE CREATION of pipefail bundles (pipefail_<hash>/{state,vs.spv,fs.spv}, written on a device by LAGFX_PIPEFAIL_BUNDLE_DIR / the arm64 dump) on the
# local Vulkan device (lavapipe here) WITH the Khronos validation layer (LAGFX_VK_VALIDATION=1) and, when the layer has it, synchronization validation
# (VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT), through examples/pipeline-repro (the same lagfx_pipeline_build() as production).
# Prints, per bundle, whether the pipeline builds here and every validation VUID (count + first message) that is not on the allow list; exit 1 when a bundle has
# one. A build that succeeds here with validation errors is exactly the case examples/pipeline-repro reports as "does NOT reproduce" without looking at them.
#
# The allow list holds messages that come from the OLD validation layer, not from the pipeline: layer 1.3.275 (Ubuntu 24.04) predates
# VK_KHR_dynamic_rendering_local_read (header 1.3.287) and VK_EXT_rasterization_order_attachment_access under dynamic rendering, so it still demands a render pass for
# InputAttachment shaders (VUID-...-renderPass-06061) and for the rasterization-order flag (VUID-...-flags-06482). Override: PIPEFAIL_VUID_ALLOW="vuid1 vuid2".
# Performance warnings (WARNING-*) are listed but never fail.
#
# What a clean run means: lavapipe + this layer version accept the pipeline. It says NOTHING about the Adreno driver (the 8 bundles of the 20261126-231501 run are
# clean here and failed with -13 on the device).
# Usage: tools/replay-pipefail-validate.sh <build dir> <bundle dir | dir with pipefail_* bundles>...      (no bundle given: LAGFX_PIPEFAIL_CHECK_DIR; neither: exit 77)
# SPDX-License-Identifier: MIT
set -u
B=${1:?build dir}; shift
[ $# -eq 0 ] && [ -n "${LAGFX_PIPEFAIL_CHECK_DIR:-}" ] && set -- "$LAGFX_PIPEFAIL_CHECK_DIR"
[ $# -eq 0 ] && { echo "no bundle dir (argument or LAGFX_PIPEFAIL_CHECK_DIR): skip"; exit 77; }
REPRO="$B/examples/pipeline-repro"
[ -x "$REPRO" ] || { echo "missing $REPRO"; exit 2; }
ALLOW=${PIPEFAIL_VUID_ALLOW:-"VUID-VkGraphicsPipelineCreateInfo-renderPass-06061 VUID-VkGraphicsPipelineCreateInfo-flags-06482"}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
list="$T/list"; : > "$list"
for a in "$@"; do
    if [ -f "$a/state" ]; then echo "$a" >> "$list"; else for b in "$a"/pipefail_*; do [ -f "$b/state" ] && echo "$b" >> "$list"; done; fi
done
n=0; bad=0
while read -r b; do
    n=$((n + 1))
    LAGFX_VK_VALIDATION=1 LAGFX_VK_VALIDATION_PRINT=1 VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT \
        timeout 300 "$REPRO" "$b" > "$T/out.txt" 2> "$T/err.txt"
    res=$(grep -m1 '^RESULT' "$T/out.txt" || echo "RESULT: (none: repro did not run)")
    # one validation message = one line starting "validation:"; its VUID / id is the first "[ ... ]"
    grep '^validation:' "$T/err.txt" | sed -n 's/^validation: [A-Za-z ]*: \[ \([^ ]*\) \].*/\1/p' | sort | uniq -c | sort -rn > "$T/ids.txt"
    : > "$T/unexp.txt"
    while read -r cnt id; do
        case "$id" in WARNING-*|UNASSIGNED-*Perf*) continue ;; esac
        case " $ALLOW " in *" $id "*) continue ;; esac
        echo "$cnt $id" >> "$T/unexp.txt"
    done < "$T/ids.txt"
    echo "$b: $res"
    if [ -s "$T/unexp.txt" ]; then
        bad=$((bad + 1))
        while read -r cnt id; do
            echo "    validation: $cnt x $id"
            grep "^validation:.*\[ $id \]" "$T/err.txt" | head -1 | sed 's/^validation: //; s/Object [0-9]*: handle = [^;]*; //g' | cut -c1-420 | sed 's/^/        /'
        done < "$T/unexp.txt"
    else
        echo "    validation: no message outside the allow list ($(grep -c '^validation:' "$T/err.txt") total incl. allowed / performance)"
    fi
done < "$list"
[ "$n" -eq 0 ] && { echo "no pipefail bundle found: skip"; exit 77; }
echo "$n bundle(s) replayed, $bad with validation errors"
[ "$bad" -eq 0 ]
