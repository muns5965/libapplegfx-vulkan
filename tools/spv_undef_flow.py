#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""OpUndef reachability over a translated SPIR-V module (binary walker, no spirv-dis needed).

usage: spv_undef_flow.py <module.spv>   -> one line per OpUndef: its sinks. API: analyze(path, lane_sens) -> (Mod, sinks, shuffle pseudo-origins).
Calls are analysed context-sensitively (the callee is walked once per call chain), memory per root variable and constant access path, vectors / structs per lane.
Taint = per-lane set of origin ids. Origins: every OpUndef result id, plus one
pseudo origin per OpVectorShuffle literal 0xFFFFFFFF lane ('shuf<resid>:<lane>').
Memory: per root variable, path-keyed (constant access-chain indices, '*' = dynamic).
Sinks: output (store to Output var), buffer (store to StorageBuffer/Uniform/Workgroup/
PhysicalStorageBuffer var, ImageWrite, atomics), branch (BranchConditional/Switch selector),
select (OpSelect condition), address (access-chain index/base, load/store pointer, image
coordinate/handle operands).
"""
import struct, sys
from collections import defaultdict

NORES = {0, 62, 63, 64, 99, 224, 225, 228, 245 - 1000, 246, 247, 249, 250, 251, 252, 253, 254, 255, 56, 8, 317, 71, 72, 5, 6, 4, 3, 2, 14, 15, 16, 17, 10}
# opcodes with result id but no result type
NOTYPE = {248, 11, 7} | set(range(19, 40)) | {33}
TYPE_OPS = set(range(19, 40))
OP_UNDEF, OP_EXTINST, OP_VECTOR, OP_STRUCT, OP_PTR = 1, 12, 23, 30, 32
STORAGE = {0: 'UniformConstant', 1: 'Input', 2: 'Uniform', 3: 'Output', 4: 'Workgroup', 5: 'CrossWorkgroup', 6: 'Private', 7: 'Function', 8: 'Generic', 9: 'PushConstant', 10: 'AtomicCounter', 11: 'Image', 12: 'StorageBuffer', 5349: 'PhysicalStorageBuffer'}
BUF_SC = {2, 4, 5, 11, 12, 5349}
NONCW = {142, 143, 144, 145, 146, 147, 148, 154, 155, 84, 86, 100}


def parse(path):
    d = open(path, 'rb').read()
    w = struct.unpack('<%dI' % (len(d) // 4), d)
    assert w[0] == 0x07230203
    i, ins = 5, []
    while i < len(w):
        n = w[i] >> 16
        ins.append((w[i] & 0xFFFF, w[i + 1:i + n]))
        i += n
    return ins


class Mod:
    def __init__(self, path):
        self.ins = parse(path)
        self.ty = {}      # id -> (kind, ...)
        self.const = {}   # id -> int value (scalar int consts)
        self.deco = defaultdict(list)
        self.var = {}     # id -> (storage, ptr type)
        self.def_ = {}    # id -> (op, words)
        self.funcs = []   # list of (fid, params, instrs)
        self.names = {}
        cur = None
        for op, ws in self.ins:
            if op == 71:
                self.deco[ws[0]].append(ws[1:])
            elif op == 5:
                self.names[ws[0]] = bytes(b for x in ws[1:] for b in struct.pack('<I', x)).split(b'\0')[0].decode(errors='replace')
            elif op in TYPE_OPS:
                self.ty[ws[0]] = (op,) + tuple(ws[1:])
            elif op == 43:
                self.const[ws[1]] = ws[2]
                self.def_[ws[1]] = (op, ws)
            elif op == 46:
                self.const[ws[1]] = 0
                self.def_[ws[1]] = (op, ws)
            elif op == 54:
                cur = [ws[1], [], []]
                self.funcs.append(cur)
                self.def_[ws[1]] = (op, ws)
            elif op == 56:
                cur = None
            elif cur is not None:
                cur[2].append((op, ws))
                if op == 55:
                    cur[1].append(ws[1])
            if op == 59:
                self.var[ws[1]] = (ws[2], ws[0])
            if op not in TYPE_OPS and op not in (71, 5, 59 - 1000) and len(ws) >= 2 and op not in NORES:
                if op in NOTYPE:
                    self.def_.setdefault(ws[0], (op, ws))
                else:
                    self.def_.setdefault(ws[1], (op, ws))
        self.undefs = [ws[1] for op, ws in self.ins if op == OP_UNDEF]
        self.undef_ty = {ws[1]: ws[0] for op, ws in self.ins if op == OP_UNDEF}

    def width(self, t):
        k = self.ty.get(t)
        if not k:
            return 1
        if k[0] == OP_VECTOR:
            return k[2]
        if k[0] == OP_STRUCT:
            return len(k) - 1
        return 1

    def tyname(self, t):
        k = self.ty.get(t)
        if not k:
            return '?'
        o = k[0]
        if o == 19: return 'void'
        if o == 20: return 'bool'
        if o == 21: return ('i%d' if k[2] == 1 else 'u%d') % k[1] if False else 'i%d' % k[1]
        if o == 22: return 'f%d' % k[1]
        if o == OP_VECTOR: return 'v%d%s' % (k[2], self.tyname(k[1]))
        if o == OP_STRUCT: return 'struct'
        if o == OP_PTR: return 'ptr'
        return 'T%d' % o

    def out_desc(self, v):
        for d in self.deco.get(v, []):
            if d[0] == 11: return 'BuiltIn%d' % d[1]
            if d[0] == 30: return 'Location%d' % d[1]
        return self.names.get(v, 'var%d' % v)


def analyze(path, lane_sens=True):
    M = Mod(path)
    T = {}              # (ctx,id) -> tuple(frozenset origins) per lane
    mem = defaultdict(dict)   # root -> {path: frozenset}
    ptr = {}            # (ctx,id) -> (root, path)
    sinks = defaultdict(lambda: defaultdict(set))   # origin -> kind -> set(desc)
    E = frozenset()
    LOCAL = set()
    for fid, params, body in M.funcs:
        LOCAL.update(params)
        for op, ws in body:
            if op in NORES or op in NOTYPE or len(ws) < 2 or op == 248:
                continue
            LOCAL.add(ws[1])
    FUN = {f[0]: f for f in M.funcs}
    entry = [ws[1] for op, ws in M.ins if op == 15][0]
    ctxs = {(): 0}

    def width(rt):
        return M.width(rt) if lane_sens else 1

    changed = [True]

    def K(ctx, i):
        return (ctx, i) if i in LOCAL else (0, i)

    for u in M.undefs:
        t = M.undef_ty[u]
        T[(0, u)] = tuple(frozenset([u]) for _ in range(width(t)))
    for v in M.var:
        if v not in LOCAL:
            ptr[(0, v)] = (v, ())
    shuf_orig = {}
    why = {}
    itof = {}
    it_ = [0]
    rwhy = {}
    cur = [None]
    ret = {}

    def setT(k, v):
        if not lane_sens:
            v = (frozenset().union(*v) if v else E,)
        old = T.get(k)
        if old is not None:
            if len(old) == len(v):
                v = tuple(a | b for a, b in zip(old, v))
            if v == old:
                return
        T[k] = v
        if k not in why: why[k] = cur[0]; itof[k] = it_[0]
        changed[0] = True

    def sink(origs, kind, desc):
        for o in origs:
            if desc not in sinks[o][kind]:
                sinks[o][kind].add(desc)
                changed[0] = True

    mwhy = {}
    def memstore(root, p, origs):
        d = mem[root]
        old = d.get(p, E)
        if not origs <= old:
            d[p] = old | origs
            changed[0] = True
            for o in origs - old:
                mwhy.setdefault((root, o), cur[0])

    def compat(q, p):
        n = min(len(q), len(p))
        return all(a == b or a == '*' or b == '*' for a, b in zip(q[:n], p[:n]))

    def memload(root, p, w):
        res = [E] * w
        for q, o in mem.get(root, {}).items():
            if not compat(q, p):
                continue
            if len(q) > len(p) and w > 1 and lane_sens and isinstance(q[len(p)], int) and q[len(p)] < w:
                res[q[len(p)]] = res[q[len(p)]] | o
            else:
                res = [r | o for r in res]
        return tuple(res)

    def run(fid, ctx, depth=0):
        _, params, body = FUN[fid]
        def tt(i): return T.get(K(ctx, i))
        def anyt(i):
            t = T.get(K(ctx, i))
            return frozenset().union(*t) if t else E
        def wt(x):
            d = M.def_.get(x)
            if d is None: return 1
            if d[0] == 55: return M.width(d[1][0])
            if d[0] in NOTYPE: return 1
            return M.width(d[1][0])
        for op, ws in body:
            cur[0] = (op, tuple(ws[:8]), ctx)
            if op == 59:
                ptr[K(ctx, ws[1])] = (K(ctx, ws[1]), ())
                continue
            if op in (65, 66, 67):
                o3 = 1 if op == 67 else 0
                base = ws[2 + o3]; idxs = ws[3 + o3:]
                if anyt(base): sink(anyt(base), 'address', 'accesschain-base')
                b = ptr.get(K(ctx, base)) or (('anon', ctx, base), ())
                path = list(b[1])
                for x in idxs:
                    path.append(M.const[x] if x in M.const else '*')
                    if anyt(x): sink(anyt(x), 'address', 'accesschain-index')
                ptr[K(ctx, ws[1])] = (b[0], tuple(path))
                continue
            if op == 61:
                rt, rid, p = ws[0], ws[1], ws[2]
                if anyt(p): sink(anyt(p), 'address', 'load-ptr')
                pi = ptr.get(K(ctx, p))
                if pi: setT(K(ctx, rid), memload(pi[0], pi[1], width(rt)))
                continue
            if op == 62:
                p, v = ws[0], ws[1]
                if anyt(p): sink(anyt(p), 'address', 'store-ptr')
                pi = ptr.get(K(ctx, p)); tv = tt(v)
                if pi is None or not tv or not any(tv): continue
                root, path = pi
                sc = M.var.get(root if not isinstance(root, tuple) else root[1], (7,))[0]
                allo = frozenset().union(*tv)
                if len(tv) == 1 or not lane_sens: memstore(root, path, allo)
                else:
                    for l, o in enumerate(tv):
                        if o: memstore(root, path + (l,), o)
                if sc == 3: sink(allo, 'output', M.out_desc(root if not isinstance(root, tuple) else root[1]))
                elif sc in BUF_SC: sink(allo, 'buffer', STORAGE.get(sc))
                continue
            if op == 63:
                pt, ps = ptr.get(K(ctx, ws[0])), ptr.get(K(ctx, ws[1]))
                if pt and ps:
                    m = memload(ps[0], ps[1], 1)[0]
                    if m:
                        memstore(pt[0], pt[1], m)
                        sc = M.var.get(pt[0] if not isinstance(pt[0], tuple) else pt[0][1], (7,))[0]
                        if sc == 3: sink(m, 'output', 'copymemory')
                        elif sc in BUF_SC: sink(m, 'buffer', 'copymemory')
                continue
            if op == 55: continue
            if op == 57:
                rt, rid, fn, args = ws[0], ws[1], ws[2], ws[3:]
                cctx = ctxs.setdefault((ctx, rid), len(ctxs))
                cctx = (ctx, rid)
                for pa, a in zip(FUN[fn][1], args):
                    if K(ctx, a) in ptr: ptr[K(cctx, pa)] = ptr[K(ctx, a)]
                    t = T.get(K(ctx, a))
                    if t:
                        cur[0] = ('arg', K(ctx, a), None); setT(K(cctx, pa), t)
                run(fn, cctx, depth + 1)
                if cctx in ret:
                    cur[0] = ('ret', cctx, None)
                    r = ret[cctx]
                    setT(K(ctx, rid), r if len(r) == width(rt) else (frozenset().union(*r),) * width(rt))
                continue
            if op == 254:
                t = tt(ws[0])
                if t:
                    o = ret.get(ctx)
                    n = t if o is None or len(o) != len(t) else tuple(a | b for a, b in zip(o, t))
                    if n != o:
                        ret[ctx] = n; changed[0] = True; rwhy.setdefault(ctx, set()).add((ctx, ws[0]))
                continue
            if op == 250:
                if anyt(ws[0]): sink(anyt(ws[0]), 'branch', 'branch-cond')
                continue
            if op == 251:
                if anyt(ws[0]): sink(anyt(ws[0]), 'branch', 'switch-selector')
                continue
            if op == 99:
                for k, nm in ((0, 'image'), (1, 'coord')):
                    if anyt(ws[k]): sink(anyt(ws[k]), 'address', 'imagewrite-' + nm)
                if anyt(ws[2]): sink(anyt(ws[2]), 'buffer', 'imagewrite-texel')
                continue
            if 227 <= op <= 242:
                for x in (ws[3:4] if op == 228 else ws[4:]):
                    if anyt(x): sink(anyt(x), 'buffer', 'atomic')
                continue
            if op in NORES or op in NOTYPE or len(ws) < 2 or op == OP_UNDEF: continue
            rt, rid, opnds = ws[0], ws[1], ws[2:]
            w = width(rt); rk = K(ctx, rid)
            def fin(res):
                setT(rk, tuple(res) if lane_sens else (frozenset().union(*res),))
            if op == 79:
                a, b, lanes = opnds[0], opnds[1], opnds[2:]
                ta, tb = tt(a), tt(b); wa = wt(a)
                res = []
                for li, l in enumerate(lanes):
                    if l == 0xFFFFFFFF:
                        res.append(frozenset([shuf_orig.setdefault((rid, li, ctx), 'shuf%d:%d' % (rid, li))]))
                    elif l < wa: res.append(ta[l] if ta and l < len(ta) else E)
                    else: res.append(tb[l - wa] if tb and (l - wa) < len(tb) else E)
                fin(res); continue
            if op == 81:
                tc = tt(opnds[0])
                if tc:
                    i0 = opnds[1]
                    if lane_sens: setT(rk, (tc[i0] if i0 < len(tc) else E,) * w)
                    else: setT(rk, tc)
                continue
            if op == 80:
                res = []
                rvec = M.ty.get(rt, (0,))[0] == OP_VECTOR
                for x in opnds:
                    xt = tt(x); d = M.def_.get(x)
                    xvec = d is not None and d[0] not in NOTYPE and M.ty.get(d[1][0], (0,))[0] == OP_VECTOR
                    if rvec and xvec: res.extend(xt if xt else [E] * M.width(d[1][0]))
                    else: res.append(frozenset().union(*xt) if xt else E)
                if len(res) != w:
                    u = frozenset().union(*res) if res else E
                    res = [u] * w
                fin(res); continue
            if op in (77, 78):
                vec = opnds[0]
                idx = opnds[-1]
                tv = tt(vec); ti = anyt(idx)
                if idx in M.const and lane_sens:
                    i0 = M.const[idx]
                    if op == 77:
                        setT(rk, ((tv[i0] if tv and i0 < len(tv) else E),) * w)
                    else:
                        res = list(tv) if tv else [E] * w
                        tc = tt(opnds[1])
                        if i0 < len(res): res[i0] = frozenset().union(*tc) if tc else E
                        fin(res)
                    continue
                allo = ti
                for x in opnds[:-1]:
                    allo = allo | anyt(x)
                if ti: sink(ti, 'address', 'dyn-index')
                if allo: fin([allo] * w)
                continue
            if op == 82:
                obj, comp, idx = opnds[0], opnds[1], opnds[2:]
                tc = list(tt(comp) or (E,) * w); to = tt(obj)
                ou = frozenset().union(*to) if to else E
                if lane_sens and idx[0] < len(tc): tc[idx[0]] = ou
                else: tc = [t | ou for t in tc]
                fin(tc); continue
            if op == 83:
                if tt(opnds[0]): setT(rk, tt(opnds[0]))
                if K(ctx, opnds[0]) in ptr: ptr[rk] = ptr[K(ctx, opnds[0])]
                continue
            if op == 169:
                c_, a, b = opnds[:3]
                if anyt(c_): sink(anyt(c_), 'select', 'select-cond')
                ta, tb = tt(a), tt(b); cu = anyt(c_)
                res = []
                for l in range(w):
                    r = cu
                    if ta: r = r | (ta[l] if l < len(ta) else ta[0])
                    if tb: r = r | (tb[l] if l < len(tb) else tb[0])
                    res.append(r)
                fin(res); continue
            if op == 245:
                res = [E] * w
                for k in range(0, len(opnds), 2):
                    t = tt(opnds[k])
                    if t: res = [r | (t[l] if l < len(t) else t[0]) for l, r in enumerate(res)]
                fin(res); continue
            if 87 <= op <= 107 or op == 86:
                allo = E
                for k, x in enumerate(opnds):
                    if anyt(x):
                        sink(anyt(x), 'address', 'image-operand%d(op%d)' % (k, op)); allo = allo | anyt(x)
                if allo: fin([allo] * w)
                continue
            lst = opnds[2:] if op == OP_EXTINST else opnds
            tl = [tt(x) for x in lst]
            if not any(tl): continue
            cw = (op == OP_EXTINST) or (109 <= op <= 205 and op not in NONCW) or op in (207, 208, 209)
            res = [E] * w
            for t in tl:
                if not t: continue
                if cw and lane_sens and len(t) == w and w > 1: res = [r | t[l] for l, r in enumerate(res)]
                else:
                    o = frozenset().union(*t); res = [r | o for r in res]
            fin(res)

    it = 0
    while changed[0] and it < 100:
        changed[0] = False; it += 1; it_[0] = it
        run(entry, 0)
    analyze.itof = itof; analyze.ptr = ptr; analyze.local = LOCAL; analyze.mwhy = mwhy; analyze.rwhy = rwhy; analyze.why = why; analyze.T = T; analyze.mem = mem; analyze.iters = it
    return M, sinks, shuf_orig


if __name__ == '__main__':
    M, s, so = analyze(sys.argv[1])
    for u in M.undefs:
        print(u, {k: sorted(v) for k, v in s.get(u, {}).items()})
