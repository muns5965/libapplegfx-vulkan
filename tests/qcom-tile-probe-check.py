#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Checks tools/qcom-tile-probe: CPU selftest, SPIR-V generation + spirv-val of the validatable modules, and a full run.

usage: qcom-tile-probe-check.py <probe> [spirv-val]
A device that lists VK_QCOM_tile_shading (the phone) is only checked for the line format; any other device (lavapipe) must report
E1..E5 UNSUPPORTED, E6 UNSUPPORTED/PASS with the plain part PASS, E5's split reference PASS, and exit 0. Exit 77 = no Vulkan ICD (skip).
"""
import glob, os, re, subprocess, sys, tempfile

probe = sys.argv[1]
spirv_val = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] else None
fails = []

def run(args, timeout=120):
    return subprocess.run([probe] + args, capture_output=True, text=True, timeout=timeout)

r = run(["--selftest"])
if r.returncode != 0 or "qcomprobe: selftest status=PASS" not in r.stdout:
    fails.append("selftest: %r" % r.stdout[-300:])

with tempfile.TemporaryDirectory() as d:
    r = run(["--dump-spirv", d])
    mods = glob.glob(os.path.join(d, "*.spv"))
    if r.returncode != 0 or len(mods) < 30:
        fails.append("dump-spirv: rc=%d modules=%d" % (r.returncode, len(mods)))
    for m in mods:
        data = open(m, "rb").read()
        words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
        if words[0] != 0x07230203:
            fails.append("bad magic " + m)
        n = os.path.basename(m)
        # real tile modules must carry the QCOM tokens (OpCapability 4495 / OpVariable storage class 4491), twins and plain ones must not
        has_cap = any((words[i] & 0xffff) == 17 and words[i + 1] == 4495 for i in range(5, len(words) - 1))
        want = (re.search(r"tile|dump", n) is not None) and ".twin" not in n and not n.startswith("gfx_")
        if has_cap != want:
            fails.append("%s: capability 4495 present=%s expected=%s" % (n, has_cap, want))
        if spirv_val and (".twin" in n or "plain" in n or n.startswith("gfx_")):
            v = subprocess.run([spirv_val, "--target-env", "vulkan1.3", m], capture_output=True, text=True)
            if v.returncode != 0:
                fails.append("spirv-val %s: %s" % (n, v.stderr.strip()[:200]))

r = run([])
if r.returncode == 2 and "setup status=FAIL" in r.stdout:
    print("no usable Vulkan device: skip")
    sys.exit(77)
if r.returncode != 0:
    fails.append("full run exit %d" % r.returncode)
lines = [l for l in r.stdout.splitlines() if l.startswith("qcomprobe:")]
st = {}
for l in lines:
    m = re.match(r"qcomprobe: E([1-6]) status=(PASS|FAIL|UNSUPPORTED)(?: |$)", l)
    if m:
        st[int(m.group(1))] = (m.group(2), l)
for i in range(1, 7):
    if i not in st:
        fails.append("no status line for E%d" % i)
if not lines or not lines[-1].startswith("qcomprobe: summary "):
    fails.append("last line is not the summary")
tile_dev = any("ext[tile_shading=1" in l for l in lines)
if not tile_dev and not fails:
    for i in range(1, 6):
        if st[i][0] != "UNSUPPORTED":
            fails.append("E%d on a device without tile shading: %s" % (i, st[i][1]))
    if st[6][0] not in ("UNSUPPORTED", "PASS") or "plain=PASS" not in st[6][1]:
        fails.append("E6: " + st[6][1])
    if "split_reference=PASS" not in st[5][1] or "split_matches_model=1" not in st[5][1]:
        fails.append("E5 split reference: " + st[5][1])
if not tile_dev and not fails:
    # analysis self-test (plain kernels in place of the tile ones): the pattern / RMW / SIMD models and the per-format rows must hold on a plain device too
    m = run(["--mock-tile", "--no-risky"]).stdout
    for want in ("qcomprobe: E3 status=PASS", "qcomprobe: E4 status=PASS"):
        if not any(l.startswith(want) and l.endswith("mock=1") for l in m.splitlines()):
            fails.append("mock run lacks '%s ... mock=1'" % want)
    if not any(l.startswith("qcomprobe: E6 ") and "plain=PASS tile=PASS" in l for l in m.splitlines()):
        fails.append("mock run: E6 plain/tile parts")
if fails:
    print("\n".join(fails)); print(r.stdout[-2000:]); sys.exit(1)
print("ok: tile_shading_device=%s " % tile_dev + " ".join("E%d=%s" % (i, st[i][0]) for i in range(1, 7)))
