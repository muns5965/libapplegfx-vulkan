#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Vertex / fragment function pairs of an arm64 GPU dump (index.txt), the ONE derivation used by check-dump-spirv.sh (pairs file for dump-translate)
and check-stage-interfaces.py.

index.txt order: `pso` line (function_refs=[vs,fs]), its `pso_root` line, then the `fn` lines of the functions that pso scanned (hash of the library the ref
named at that time), then other lines. A guest REUSES function refs (task 2, ref 11 is air.vertexFetchFunction of library fa13d506 for the first pso and of
library 4f70917c for a later one), so a ref has no single library: the fn lines that FOLLOW a pso define its functions, a ref those lines do not mention keeps its
previous definition. A pso is resolved when the first line that is not an `fn` / `pso_root` line after it is reached (or at the end).
The earlier derivations were wrong in two different ways: "last fn line of the ref in the whole file wins" (check-dump-spirv.sh) and "fn lines before the pso
win, seeded with the last one of the file" (check-stage-interfaces.py), so they disagreed about which pairs exist (2 pairs the checker wanted had no translation).

Usage: dump_pairs.py <index.txt>      prints "<vs lib hash> <fs lib hash>" per distinct pair, sorted.
"""
import re
import sys

FN = re.compile(r'fn task=(\d+) ref=(\d+) .* hash=(\w+) functions=\[(\w[\w.]*)\(stage=(\d)')
PSO = re.compile(r'pso task=(\d+) ref=\d+ function_refs=\[(\d+),(\d+)\]')


def pairs(lines):
    """-> ordered list of ((vs hash, vs function, vs stage), (fs hash, fs function, fs stage)), one entry per pso that resolved."""
    cur, pending, out = {}, [], []

    def flush():
        for t, a, b in pending:
            if (t, a) in cur and (t, b) in cur:
                out.append((cur[(t, a)], cur[(t, b)]))
        del pending[:]

    for l in lines:
        m = FN.match(l)
        if m:
            cur[(m[1], m[2])] = (m[3], m[4], m[5])
            continue
        if l.startswith('pso_root'):
            continue
        flush()
        m = PSO.match(l)
        if m:
            pending.append((m[1], m[2], m[3]))
    flush()
    return out


def distinct_hash_pairs(lines):
    return sorted({(v[0], f[0]) for v, f in pairs(lines)})


def main(argv):
    if len(argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    for a, b in distinct_hash_pairs(open(argv[1]).read().splitlines()):
        print(a, b)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
