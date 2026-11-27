#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tier-1 reduction rows (henge docs/research/reduce-ch10-ch11-analysis.md section 6) as an INDEPENDENT runner:
no Android app, no Java harness. Wraps `pipeline-repro <bundle> --reduce --only <ids>` (isolated child per row), maps the Tier-1 names
(GRID / Q1-Q4 / P2-P6 / N2-N6) onto the ladder row ids, prints one table and the hypothesis A / B reading from the analysis.

usage: tier1-runner.py <bundle dir> [--tool path/to/pipeline-repro] [--nin 5] [--timeout-ms 20000] [--log file] [--from-log file]
  --from-log  do not run anything: evaluate an existing device log (lines 'pipeline-reduce: row=<id> result=...')
Only a row line counts. A child that exited 0 without a row line is NOT RUN (never CRASH: analysis section 5); a row id the ladder did
not generate for this module is 'absent'. BUILT on lavapipe says nothing about Adreno: run it on the device that fails.
Exit: 0 = ran and evaluated, 2 = usage / tool error."""
import argparse, re, subprocess, sys

def rows(nin):
    full = (1 << nin) - 1
    t = []                                               # (name, ladder id, what)
    for lv in (7, 59, 236):
        for s in ("live", "null"):
            t.append((f"GRID L={lv} {s}", f"XG{lv}.{s}", "CH10 reductions, cut level, inputs " + s))
    t += [("Q1", "IW1.all", "IFC + loads wired to outputs"), ("Q2", "IW0.all", "IFC + loads, unused"),
          ("Q3", "PB.CH10", "CH10, unreachable blocks removed"),
          ("Q4~", "PBW.CH10", "CH10, unreachable blocks + write-only locals removed (approximates 'unused vars')")]
    for k in range(nin):
        t.append((f"P{k+2}", f"XN{full & ~(1 << k):02x}", f"only input #{k} live (CH11 + restore #{k})"))
    for k in range(nin):
        t.append((f"N{k+2}", f"XN{1 << k:02x}", f"only input #{k} nulled (CH10 with #{k} null)"))
    t += [("ref CH10", "CH10", "live reference"), ("ref CH11", "CH11", "nulled reference"), ("ref RI8", "RI8", "null column, full size")]
    return t

ROW = re.compile(r"pipeline-reduce: row=(\S+) result=(\S+)")

def parse(text):
    res = {}
    for m in ROW.finditer(text): res[m.group(1)] = m.group(2)
    return res

def verdict(r, nin):
    g = lambda n: r.get(n, "absent")
    out = []
    q1, q2, q3, q4 = g("Q1"), g("Q2"), g("Q3"), g("Q4~")
    if "FAIL" in (q1, q2) or q4 == "FAIL": out.append("A refuted (tiny + live loads fails, or size-pruned Q4 fails)")
    elif q1 == "BUILT" and q2 == "BUILT" and (q3 == "FAIL" or q4 == "BUILT"): out.append("A consistent (tiny+live BUILT; static dead code matters)")
    else: out.append("A undecided")
    P = [g(f"P{k+2}") for k in range(nin)]; N = [g(f"N{k+2}") for k in range(nin)]
    if all(p == "BUILT" for p in P) and all(n == "FAIL" for n in N): out.append("B refuted (no single input matters)")
    else:
        flips = [k + 2 for k in range(nin) if N[k] == "BUILT"]
        fails = [k + 2 for k in range(nin) if P[k] == "FAIL"]
        if len(flips) == 1 and fails == flips: out.append(f"B consistent: location {flips[0]} decides")
        else: out.append("B undecided / partial (N flips %s, P fails %s)" % (flips, fails))
    return out

def main():
    a = argparse.ArgumentParser()
    a.add_argument("bundle", nargs="?"); a.add_argument("--tool", default="builddir/examples/pipeline-repro")
    a.add_argument("--nin", type=int, default=5); a.add_argument("--timeout-ms", type=int, default=20000)
    a.add_argument("--log"); a.add_argument("--from-log")
    a = a.parse_args()
    tab = rows(a.nin)
    if a.from_log:
        text = open(a.from_log, errors="replace").read(); notrun = False
    else:
        if not a.bundle: print("bundle dir required", file=sys.stderr); return 2
        ids = ",".join(i for _, i, _ in tab)
        try: p = subprocess.run([a.tool, a.bundle, "--reduce", "--only", ids, "--timeout", str(a.timeout_ms)], capture_output=True, text=True)
        except OSError as e: print("cannot run tool:", e, file=sys.stderr); return 2
        text = p.stdout + p.stderr; notrun = p.returncode not in (0, 1)
        if a.log: open(a.log, "w").write(text)
        if notrun: print(f"tool exit {p.returncode} (not a repro verdict); rows below may be missing", file=sys.stderr)
    res = parse(text)
    byname = {}
    print(f"{'row':<14}{'ladder id':<14}{'result':<9}what")
    for name, i, what in tab:
        r = res.get(i, "absent"); byname[name] = r
        print(f"{name:<14}{i:<14}{r:<9}{what}")
    if not res: print("no row lines at all: NOT RUN (harness output has no 'pipeline-reduce: row=' lines)")
    print()
    for line in verdict(byname, a.nin): print(line)
    print("note: BUILT only means vkCreateGraphicsPipelines succeeded on the device that wrote the log; the VS-stub control rows (CH10 / CH11 with stub VS) have no ladder row yet.")
    return 0

if __name__ == "__main__": sys.exit(main())
