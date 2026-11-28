#!/usr/bin/env python3
"""Rebuild SPIR-V modules from "pipeline-build-fail-spv:" lines of a g7_lagfx log.
usage: spv-from-log.py <log> <outdir>   -> <outdir>/<stage>_<hash>.spv (then: spirv-dis / spirv-val / examples/pipeline-repro)"""
import re, struct, sys, os
if len(sys.argv) != 3: sys.exit(__doc__)
pat = re.compile(r"pipeline-build-fail-spv: (vs|fs) hash=([0-9a-f]{16}) bytes=(\d+) line=(\d+)/(\d+) ((?:[0-9a-f]{8} ?)+)")
mods = {}
for ln in open(sys.argv[1], errors="replace"):
    m = pat.search(ln)
    if not m: continue
    st, h, nb, k, tot, words = m.groups()
    d = mods.setdefault((st, h), {"bytes": int(nb), "lines": int(tot), "parts": {}})
    d["parts"][int(k)] = [int(w, 16) for w in words.split()]
os.makedirs(sys.argv[2], exist_ok=True)
for (st, h), d in sorted(mods.items()):
    if sorted(d["parts"]) != list(range(1, d["lines"] + 1)): print(f"{st} {h}: incomplete ({len(d['parts'])}/{d['lines']} lines)"); continue
    w = [x for k in sorted(d["parts"]) for x in d["parts"][k]]
    if len(w) * 4 != d["bytes"]: print(f"{st} {h}: size mismatch {len(w)*4} != {d['bytes']}"); continue
    path = os.path.join(sys.argv[2], f"{st}_{h}.spv")
    open(path, "wb").write(struct.pack(f"<{len(w)}I", *w)); print(path, d["bytes"], "bytes")
