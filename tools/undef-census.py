#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""OpUndef census of the translated shaders of an arm64 GPU dump (docs: census item X03).

usage: tools/undef-census.py <build dir> <dump dir> [work dir]

Re-translates every function of the dump twice with examples/dump-translate:
  default   the modules as shipped (OpUndef values are shared per type);
  nocache   LAGFX_UNDEF_NOCACHE=1: one OpUndef per creation site, so every OpUndef has exactly one origin (LAGFX_UNDEF_SITES attributes it to its emit site).
Then walks every module with spv_undef_flow.py (def-use graph, lane precision, call-context sensitive) and prints, per origin class and per module,
how many OpUndef values are unused / dead (used, reach nothing) / lane-dead (only through lanes that are never read) / reach an address / a branch-or-select
condition / an Output or buffer or image store. Needs stdbuf (coreutils) so that the stdout module markers interleave with the stderr site lines.
"""
import collections, glob, os, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spv_undef_flow as U


def pairs(index, out):
    cur, o = {}, set()
    L = open(index).read().splitlines()
    for l in L:
        m = re.match(r'fn task=\d+ ref=(\d+) .* hash=(\w+) ', l)
        if m: cur[m[1]] = m[2]
    for l in L:
        m = re.match(r'pso task=\d+ ref=\d+ function_refs=\[(\d+),(\d+)\]', l)
        if m and m[1] in cur and m[2] in cur: o.add((cur[m[1]], cur[m[2]]))
    open(out, 'w').write(''.join('%s %s\n' % p for p in sorted(o)))


def translate(build, dump, work, mode):
    out = os.path.join(work, mode); os.makedirs(out, exist_ok=True)
    env = dict(os.environ, DUMP_UNDEF='1', LAGFX_UNDEF_SITES='1')
    if mode == 'nocache': env['LAGFX_UNDEF_NOCACHE'] = '1'
    log = os.path.join(work, mode + '.log')
    with open(log, 'w') as f:
        subprocess.run(['stdbuf', '-oL', os.path.join(build, 'examples/dump-translate'), dump, out, os.path.join(work, 'pairs.txt')], env=env, stdout=f, stderr=subprocess.STDOUT, check=False)
    mods, cur = [], []
    for l in open(log, errors='replace'):
        m = re.match(r'UNDEFSITE id=(\d+) ty=(\d+) fn=(\S+) line=(\d+) vid=(-?\d+) cached=(\d)', l)
        if m: cur.append((int(m[1]), m[3], int(m[4]), int(m[6]))); continue
        m = re.match(r'undef (\w+) (\S+): defs=', l)
        if m: mods.append((m[1], m[2], cur)); cur = []
    files = sorted(glob.glob(out + '/*.spv')); ids = {f: set(U.Mod(f).undefs) for f in files}; used = set(); res = []
    for lib, fn, sites in mods:
        want = {s[0] for s in sites if s[3] == 0}
        c = [f for f in files if os.path.basename(f).startswith(lib + '_') and f not in used and ids[f] == want]
        if c: used.add(c[0]); res.append((c[0], lib, fn, sites))
    return res


def origin(site):
    fn, line = site[1], site[2]
    if fn.startswith('const_undef'): return 'AIR undef (CST_CODE_UNDEF)'
    if fn == 'const_unknown(code26)': return 'AIR poison (CST_CODE_POISON)'
    if fn == 'const_unknown(code11)': return 'CE_CAST not resolved (translator gap)'
    if fn.startswith('const_unknown'): return 'AIR constant ' + fn[14:-1]
    if fn.startswith('ce_cast'): return 'cast of undef/poison (LangRef: undef)'
    if fn == 'emit_inst_shufflevec': return 'shufflevector fallback placeholder'
    if fn == 'emit_inst_call': return 'call placeholder (sample_texture i8 status)'
    if fn == 'translate_body_dispatch': return 'dispatch placeholder (state-machine iteration-cap bail return)'
    if fn == 'emit_module_vars_and_function': return 'prologue pre-warm'
    return 'other %s:%d' % (fn, line)


def uses_of(M):
    cnt = collections.Counter(); und = set(M.undefs)
    for op, ws in M.ins:
        if op in U.TYPE_OPS or op in (43, 71, 72, 5, 6, 1): continue
        ids = ws[:2] if op == 251 else ws[2:4] if op == 79 else ws[2:3] if op == 81 else ws[2:4] if op == 82 else ws if op in U.NORES else ws[1:] if op in U.NOTYPE else ws[2:]
        for x in ids:
            if x in und: cnt[x] += 1
    return cnt


def classify(path):
    M, S, _ = U.analyze(path, True); _, S2, _ = U.analyze(path, False); uses = uses_of(M); rows = {}
    for u in M.undefs:
        k = set(S.get(u, {}))
        out = bool(k & {'output', 'buffer'}); ctl = bool(k & {'branch', 'select'}); adr = 'address' in k
        cls = 'reach' if (out or ctl or adr) else 'unused' if not uses[u] else 'lane-dead' if S2.get(u) else 'dead'
        rows[u] = (cls, out, ctl, adr)
    shuf = sum(1 for fid, p, b in M.funcs for op, ws in b if op == 79 for l in ws[4:] if l == 0xFFFFFFFF)
    return rows, shuf


def main():
    build, dump = sys.argv[1], sys.argv[2]
    work = sys.argv[3] if len(sys.argv) > 3 else tempfile.mkdtemp(prefix='undef-census-')
    os.makedirs(work, exist_ok=True)
    pairs(os.path.join(dump, 'index.txt'), os.path.join(work, 'pairs.txt'))
    print('work dir:', work)
    # per origin class (nocache: one origin per OpUndef)
    t = collections.defaultdict(collections.Counter); shuf_total = 0
    for path, lib, fn, sites in translate(build, dump, work, 'nocache'):
        rows, sh = classify(path); shuf_total += sh
        site = {s[0]: s for s in sites if s[3] == 0}
        for u, (cls, o, c, a) in rows.items():
            k = origin(site[u]); t[k]['n'] += 1; t[k][cls] += 1; t[k]['out'] += o; t[k]['ctl'] += c; t[k]['adr'] += a
    print('\nper origin (one OpUndef per creation site)')
    print('%-62s %5s %6s %5s %9s | %6s %7s %7s' % ('origin', 'n', 'unused', 'dead', 'lane-dead', 'output', 'control', 'address'))
    tot = collections.Counter()
    for k, v in sorted(t.items(), key=lambda kv: -kv[1]['n']):
        print('%-62s %5d %6d %5d %9d | %6d %7d %7d' % (k, v['n'], v['unused'], v['dead'], v['lane-dead'], v['out'], v['ctl'], v['adr'])); tot.update(v)
    print('%-62s %5d %6d %5d %9d | %6d %7d %7d' % ('TOTAL', tot['n'], tot['unused'], tot['dead'], tot['lane-dead'], tot['out'], tot['ctl'], tot['adr']))
    print('OpVectorShuffle literal-0xFFFFFFFF (undefined mask) lanes in the modules: %d (not OpUndef values)' % shuf_total)
    # per module, as shipped
    print('\nper module, as shipped (OpUndef shared per type)')
    print('%-9s %-40s %5s %6s %5s %9s | %6s %7s %7s' % ('lib', 'function', 'defs', 'unused', 'dead', 'lane-dead', 'output', 'control', 'address'))
    tot = collections.Counter()
    for path, lib, fn, sites in translate(build, dump, work, 'default'):
        rows, _ = classify(path); c = collections.Counter(r[0] for r in rows.values())
        v = dict(n=len(rows), unused=c['unused'], dead=c['dead'], ld=c['lane-dead'], out=sum(r[1] for r in rows.values()), ctl=sum(r[2] for r in rows.values()), adr=sum(r[3] for r in rows.values()))
        print('%-9s %-40s %5d %6d %5d %9d | %6d %7d %7d' % (lib[:8], fn[:40], v['n'], v['unused'], v['dead'], v['ld'], v['out'], v['ctl'], v['adr']))
        for k in v: tot[k] += v[k]
    print('%-9s %-40s %5d %6d %5d %9d | %6d %7d %7d' % ('TOTAL', '', tot['n'], tot['unused'], tot['dead'], tot['ld'], tot['out'], tot['ctl'], tot['adr']))


if __name__ == '__main__':
    main()
