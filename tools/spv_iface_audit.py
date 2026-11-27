#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline VS-output / FS-input interface audit of a SPIR-V vertex+fragment pair.

Reads the two binary modules directly (stdlib only, no spirv-dis), prints one table per
stage (every Input/Output variable: Location, Component, type, width, interpolation
decorations, builtin, static loads/stores) and one pair table (what the driver links),
then a list of findings:

  VIOLATION  a rule of the Vulkan "Shader Interfaces" chapter (or a Flat / capability VUID) is broken
  NOTE       legal, but unusual or a known driver-risk pattern (see comments next to each check)

Usage:  spv_iface_audit.py <vs.spv> <fs.spv> [--topology list|point|line|tri] [--quiet]
Exit:   1 when any VIOLATION is reported, 0 otherwise, 2 on a usage / parse error.

The audit says what the SPIR-V declares. It does NOT predict what any driver compiler accepts:
a clean audit is a statement about the Vulkan interface rules only.
"""
import struct
import sys

# ---- SPIR-V numbers (public registry) -------------------------------------------------------------
OP = dict(Undef=1, Name=5, MemberName=6, EntryPoint=15, Capability=17, TypeVoid=19, TypeBool=20, TypeInt=21,
          TypeFloat=22, TypeVector=23, TypeMatrix=24, TypeImage=25, TypeSampler=26, TypeSampledImage=27,
          TypeArray=28, TypeRuntimeArray=29, TypeStruct=30, TypePointer=32, Constant=43, Function=54,
          FunctionEnd=56, FunctionCall=57, Variable=59, Load=61, Store=62, CopyMemory=63, AccessChain=65,
          InBoundsAccessChain=66, Decorate=71, MemberDecorate=72)
SC = {0: 'UniformConstant', 1: 'Input', 2: 'Uniform', 3: 'Output', 4: 'Workgroup', 5: 'CrossWorkgroup',
      6: 'Private', 7: 'Function', 9: 'PushConstant', 10: 'AtomicCounter', 11: 'Image', 12: 'StorageBuffer'}
DEC = dict(RelaxedPrecision=0, Block=2, BuiltIn=11, NoPerspective=13, Flat=14, Patch=15, Centroid=16, Sample=17,
           Invariant=18, Location=30, Component=31, Index=32, PerVertexKHR=5285)
BUILTIN = {0: 'Position', 1: 'PointSize', 3: 'ClipDistance', 4: 'CullDistance', 5: 'VertexId', 6: 'InstanceId',
           7: 'PrimitiveId', 8: 'InvocationId', 9: 'Layer', 10: 'ViewportIndex', 15: 'FragCoord', 16: 'PointCoord',
           17: 'FrontFacing', 18: 'SampleId', 19: 'SamplePosition', 20: 'SampleMask', 22: 'FragDepth',
           23: 'HelperInvocation', 42: 'VertexIndex', 43: 'InstanceIndex', 4424: 'BaseVertex', 4425: 'BaseInstance',
           4426: 'DrawIndex', 4440: 'ViewIndex', 5286: 'BaryCoordKHR'}
MODEL = {0: 'Vertex', 1: 'TessCtl', 2: 'TessEval', 3: 'Geometry', 4: 'Fragment', 5: 'GLCompute'}
CAP_SIO16 = 4087   # StorageInputOutput16


class Mod:
    def __init__(self, blob):
        if len(blob) < 20 or len(blob) % 4:
            raise ValueError('not a SPIR-V binary (size)')
        w = struct.unpack('<%dI' % (len(blob) // 4), blob)
        if w[0] != 0x07230203:
            raise ValueError('bad SPIR-V magic %#x' % w[0])
        self.version = (w[1] >> 16 & 0xff, w[1] >> 8 & 0xff)
        self.types = {}      # id -> tuple
        self.consts = {}     # id -> int
        self.decs = {}       # id -> {dec: [args]}
        self.mdecs = {}      # (id, member) -> {dec: [args]}
        self.vars = {}       # id -> (ptr type id, storage class)
        self.names = {}
        self.caps = set()
        self.entries = []    # (model, fid, name, [iface ids])
        self.loads = {}      # var id -> static OpLoad count (via access chains too)
        self.stores = {}
        self.refs = {}       # any reference inside a function body
        self.n_inst = 0
        self.body = []       # (op, operand words) of every function-body instruction, in order
        self.label_at = []   # body index of each OpLabel (basic block starts)
        self.live = {}       # input var id -> [(load idx, block no, first use idx, last use idx, n uses)] (approximate, see _live)
        self._parse(w)
        self._live()

    @staticmethod
    def _str(ws):
        b = b''.join(struct.pack('<I', x) for x in ws)
        return b.split(b'\0', 1)[0].decode('utf-8', 'replace')

    def _parse(self, w):
        i = 5
        in_fn = False
        alias = {}           # access-chain result -> root var
        while i < len(w):
            wc, op = w[i] >> 16, w[i] & 0xffff
            if wc == 0:
                raise ValueError('zero word count at word %d' % i)
            o = w[i + 1:i + wc]
            i += wc
            if op == OP['Capability']: self.caps.add(o[0])
            elif op == OP['Name']: self.names[o[0]] = self._str(o[1:])
            elif op == OP['EntryPoint']:
                nm = self._str(o[2:]); nw = len(nm.encode()) // 4 + 1
                self.entries.append((o[0], o[1], nm, list(o[2 + nw:])))
            elif op == OP['TypeBool']: self.types[o[0]] = ('bool',)
            elif op == OP['TypeInt']: self.types[o[0]] = ('int', o[1], o[2])
            elif op == OP['TypeFloat']: self.types[o[0]] = ('float', o[1])
            elif op == OP['TypeVector']: self.types[o[0]] = ('vec', o[1], o[2])
            elif op == OP['TypeMatrix']: self.types[o[0]] = ('mat', o[1], o[2])
            elif op == OP['TypeArray']: self.types[o[0]] = ('arr', o[1], o[2])
            elif op == OP['TypeRuntimeArray']: self.types[o[0]] = ('rarr', o[1])
            elif op == OP['TypeStruct']: self.types[o[0]] = ('struct', tuple(o[1:]))
            elif op == OP['TypePointer']: self.types[o[0]] = ('ptr', o[1], o[2])
            elif op in (OP['TypeVoid'], OP['TypeImage'], OP['TypeSampler'], OP['TypeSampledImage']):
                self.types[o[0]] = ('opaque', op)
            elif op == OP['Constant']:
                self.consts[o[1]] = o[2]
            elif op == OP['Decorate']: self.decs.setdefault(o[0], {})[o[1]] = list(o[2:])
            elif op == OP['MemberDecorate']: self.mdecs.setdefault((o[0], o[1]), {})[o[2]] = list(o[3:])
            elif op == OP['Variable'] and not in_fn: self.vars[o[1]] = (o[0], o[2])
            elif op == OP['Function']: in_fn = True
            elif op == OP['FunctionEnd']: in_fn = False
            if in_fn:
                self.n_inst += 1
                if op == 248: self.label_at.append(len(self.body))
                self.body.append((op, o))
                if op in (OP['AccessChain'], OP['InBoundsAccessChain']):
                    alias[o[1]] = alias.get(o[2], o[2])
                    self.refs[alias[o[1]]] = self.refs.get(alias[o[1]], 0) + 1
                elif op == OP['Load']:
                    r = alias.get(o[2], o[2]); self.loads[r] = self.loads.get(r, 0) + 1
                    self.refs[r] = self.refs.get(r, 0) + 1
                elif op == OP['Store']:
                    r = alias.get(o[0], o[0]); self.stores[r] = self.stores.get(r, 0) + 1
                    self.refs[r] = self.refs.get(r, 0) + 1
                elif op == OP['CopyMemory']:
                    for k, d in ((0, self.stores), (1, self.loads)):
                        r = alias.get(o[k], o[k]); d[r] = d.get(r, 0) + 1
                        self.refs[r] = self.refs.get(r, 0) + 1
                elif op == OP['FunctionCall']:
                    for a in o[3:]:
                        r = alias.get(a, a)
                        if r in self.vars:
                            self.refs[r] = self.refs.get(r, 0) + 1
                            self.loads[r] = self.loads.get(r, 0) + 1   # unknown direction: count as both
                            self.stores[r] = self.stores.get(r, 0) + 1

    def _live(self):
        """Where each direct OpLoad of a variable sits and how far its result is used (approximate: a use is any later operand word equal
        to the result id, so a literal that happens to equal a big id can add a false use; SSA ids of loads are normally far above literals)."""
        import bisect
        want = {}
        for n, (op, o) in enumerate(self.body):
            if op == OP['Load'] and o[2] in self.vars:
                want[o[1]] = (o[2], n)
        first = {}; last = {}; cnt = {}
        for n, (op, o) in enumerate(self.body):
            for x in o[2:] if op != 248 else ():
                if x in want and n > want[x][1]:
                    first.setdefault(x, n); last[x] = n; cnt[x] = cnt.get(x, 0) + 1
        for rid, (var, n) in want.items():
            blk = bisect.bisect_right(self.label_at, n) - 1
            self.live.setdefault(var, []).append((n, blk, first.get(rid), last.get(rid), cnt.get(rid, 0)))

    # ---- type helpers ---------------------------------------------------------------------------
    def pointee(self, var):
        t = self.types[self.vars[var][0]]
        return t[2]

    def tstr(self, t):
        ty = self.types.get(t)
        if ty is None: return '?'
        k = ty[0]
        if k == 'bool': return 'bool'
        if k == 'int': return ('i' if ty[2] else 'u') + str(ty[1])
        if k == 'float': return 'f' + str(ty[1])
        if k == 'vec': return 'v%d%s' % (ty[2], self.tstr(ty[1]))
        if k == 'mat': return 'm%dx%s' % (ty[2], self.tstr(ty[1]))
        if k == 'arr': return '%s[%s]' % (self.tstr(ty[1]), self.consts.get(ty[2], '?'))
        if k == 'rarr': return self.tstr(ty[1]) + '[]'
        if k == 'struct': return 'struct{%s}' % ','.join(self.tstr(m) for m in ty[1])
        return k

    def scalar(self, t):
        """(base kind 'f'|'i'|'u'|'b', bit width) of the innermost scalar, plus lane count of the first vector level."""
        ty = self.types.get(t)
        while ty and ty[0] in ('arr', 'rarr', 'mat'):
            ty = self.types.get(ty[1])
        lanes = 1
        if ty and ty[0] == 'vec':
            lanes = ty[2]; ty = self.types.get(ty[1])
        if not ty: return ('?', 0, lanes)
        if ty[0] == 'float': return ('f', ty[1], lanes)
        if ty[0] == 'int': return ('i' if ty[2] else 'u', ty[1], lanes)
        if ty[0] == 'bool': return ('b', 32, lanes)
        return ('?', 0, lanes)

    def n_locations(self, t):
        """Locations consumed by a type (Vulkan 'Location Assignment')."""
        ty = self.types.get(t)
        if ty is None: return 1
        k = ty[0]
        if k in ('float', 'int', 'bool'): return 1
        if k == 'vec':
            sc = self.scalar(t)
            return 2 if (sc[1] == 64 and ty[2] >= 3) else 1
        if k == 'mat': return ty[2] * self.n_locations(ty[1])
        if k == 'arr': return int(self.consts.get(ty[2], 1)) * self.n_locations(ty[1])
        if k == 'struct': return sum(self.n_locations(m) for m in ty[1])
        return 1


def interface_vars(m, entry):
    """Rows for one entry point: dicts with dir, loc, comp, type id, builtin (or None), decorations, loads, stores."""
    rows = []
    for v in entry[3]:
        if v not in m.vars: continue
        sc = SC.get(m.vars[v][1])
        if sc not in ('Input', 'Output'): continue
        d = m.decs.get(v, {})
        t = m.pointee(v)
        base = m.types.get(t)
        if base and base[0] == 'arr' and 'Patch' not in d:      # per-vertex arrays (not used by VS/FS) are unwrapped only for the type string
            pass
        r = dict(var=v, dir=sc, type=t, tstr=m.tstr(t), name=m.names.get(v, ''),
                 loc=d.get(DEC['Location'], [None])[0], comp=d.get(DEC['Component'], [0])[0],
                 builtin=BUILTIN.get(d[DEC['BuiltIn']][0], 'Builtin#%d' % d[DEC['BuiltIn']][0]) if DEC['BuiltIn'] in d else None,
                 flat=DEC['Flat'] in d, nopersp=DEC['NoPerspective'] in d, centroid=DEC['Centroid'] in d,
                 sample=DEC['Sample'] in d, relaxed=DEC['RelaxedPrecision'] in d, patch=DEC['Patch'] in d,
                 loads=m.loads.get(v, 0), stores=m.stores.get(v, 0), refs=m.refs.get(v, 0), live=m.live.get(v, []))
        # a Block of builtins (gl_PerVertex): list the members
        if base and base[0] == 'struct' and any((t, i) in m.mdecs and DEC['BuiltIn'] in m.mdecs[(t, i)] for i in range(len(base[1]))):
            r['builtin'] = '+'.join(BUILTIN.get(m.mdecs[(t, i)][DEC['BuiltIn']][0], '?') for i in range(len(base[1]))
                                    if (t, i) in m.mdecs and DEC['BuiltIn'] in m.mdecs[(t, i)])
        sc3 = m.scalar(t)
        r['kind'], r['bits'], r['lanes'] = sc3
        r['nloc'] = m.n_locations(t) if r['builtin'] is None else 0
        rows.append(r)
    return rows


def interp(r):
    s = [n for n, f in (('Flat', r['flat']), ('NoPersp', r['nopersp']), ('Centroid', r['centroid']), ('Sample', r['sample'])) if f]
    return ','.join(s) or '-'


def load_stage(path, model):
    with open(path, 'rb') as fh:
        m = Mod(fh.read())
    ep = [e for e in m.entries if e[0] == model]
    if not ep:
        raise ValueError('%s: no %s entry point (found %s)' % (path, MODEL[model], [MODEL.get(e[0]) for e in m.entries]))
    return m, ep[0], interface_vars(m, ep[0])


def stage_table(label, m, ep, rows):
    out = ['%s  %s  SPIR-V %d.%d  entry "%s"  fn-instrs %d  caps: %s' % (
        label, MODEL[ep[0]], m.version[0], m.version[1], ep[2], m.n_inst,
        ','.join(c for c in ('Float16', 'Int16', 'Int8', 'Int64', 'StorageInputOutput16') if
                 {'Float16': 9, 'Int16': 22, 'Int8': 39, 'Int64': 11, 'StorageInputOutput16': CAP_SIO16}[c] in m.caps) or '-')]
    out.append('  dir    loc comp nloc type            bits interp   relaxed builtin              loads stores')
    key = lambda r: (r['dir'], 1 if r['builtin'] else 0, r['loc'] if r['loc'] is not None else 1 << 30, r['comp'])
    for r in sorted(rows, key=key):
        out.append('  %-6s %3s %4d %4d %-15s %4d %-9s %-7s %-20s %5d %6d' % (
            r['dir'], '-' if r['loc'] is None else r['loc'], r['comp'], r['nloc'], r['tstr'][:15], r['bits'],
            interp(r), 'yes' if r['relaxed'] else '-', r['builtin'] or '-', r['loads'], r['stores']))
    return out


def live_table(m, rows):
    out = []
    for r in rows:
        if r['dir'] != 'Input' or not r['live']: continue
        for (n, blk, f, l, c) in r['live']:
            out.append('  %-3s %-22s load@instr %6d (block %5d)  first use %s  last use %s  uses %d  span %s instrs of %d' % (
                'loc' if r['loc'] is not None else 'bi', ('%d %s' % (r['loc'], r['tstr'])) if r['loc'] is not None else r['builtin'],
                n, blk, '-' if f is None else f, '-' if l is None else l, c, '-' if l is None else l - n, len(m.body)))
    return out


def slots(r):
    """(location, component) -> set of bit widths for every 32-bit component slot the variable occupies."""
    s = {}
    if r['loc'] is None or r['builtin']: return s
    lanes = r['lanes']; wide = r['bits'] == 64
    per_loc = lanes * (2 if wide else 1)
    nloc = max(1, r['nloc'])
    for l in range(nloc):
        n = min(4, per_loc) if nloc == 1 or not wide else 4
        for c in range(r['comp'], r['comp'] + max(1, n)):
            s[(r['loc'] + l, c)] = r['bits']
    return s


def audit(vs, fs, topology=None):
    """-> (pair rows, findings[(sev, msg)])"""
    (vm, vep, vrows), (fm, fep, frows) = vs, fs
    F = []
    add = lambda sev, msg: F.append((sev, msg))
    vout = [r for r in vrows if r['dir'] == 'Output']
    fin = [r for r in frows if r['dir'] == 'Input']
    # -- per-stage rules
    for who, rows, m in (('VS', vrows, vm), ('FS', frows, fm)):
        for d in ('Input', 'Output'):
            seen = {}
            for r in (x for x in rows if x['dir'] == d):
                for k, bits in slots(r).items():
                    if k in seen and seen[k] is not r:
                        add('VIOLATION', '%s %s: Location %d Component %d is assigned to two variables (%%%d and %%%d) '
                            '[Vulkan: "Any two inputs/outputs listed as operands on the same OpEntryPoint must not be assigned the same Location and Component"]'
                            % (who, d, k[0], k[1], seen[k]['var'], r['var']))
                    seen[k] = r
        for r in rows:
            if r['loc'] is None and not r['builtin']:
                add('VIOLATION', '%s %s %%%d has neither Location nor BuiltIn' % (who, r['dir'], r['var']))
            if r['bits'] == 16 and CAP_SIO16 not in m.caps and r['loc'] is not None:
                add('VIOLATION', '%s %s loc %d is a 16-bit type (%s) but the module lacks capability StorageInputOutput16 '
                    '[Vulkan: 16-bit components need the storageInputOutput16 feature]' % (who, r['dir'], r['loc'], r['tstr']))
    for r in fin:
        if r['kind'] in ('i', 'u') and r['bits'] and not r['flat'] and r['loc'] is not None:
            add('VIOLATION', 'FS Input loc %d %s is an integer type without Flat [VUID-StandaloneSpirv-Flat-04744]' % (r['loc'], r['tstr']))
        if r['bits'] == 64 and not r['flat'] and r['loc'] is not None:
            add('VIOLATION', 'FS Input loc %d %s is 64-bit without Flat [VUID-StandaloneSpirv-Flat-04744]' % (r['loc'], r['tstr']))
    # -- pair rows over (location, component)
    vmap = {}; fmap = {}
    for r in vout:
        for k in slots(r): vmap.setdefault(k, r)
    for r in fin:
        for k in slots(r): fmap.setdefault(k, r)
    pair = []
    done = set()
    for r in sorted([x for x in vout + fin if x['loc'] is not None and not x['builtin']], key=lambda x: (x['loc'], x['comp'])):
        key = (r['loc'], r['comp'])
        if key in done: continue
        done.add(key)
        o = vmap.get(key); i = fmap.get(key)
        # an output/input pair is reported once, keyed on the input's first slot when it exists, else the output's
        if i is not None and key != min(slots(i)): continue
        if i is None and o is not None and key != min(slots(o)): continue
        st = 'OK'
        if i is not None and o is None:
            st = 'IN-NO-OUT'
            sev = 'VIOLATION' if (i['loads'] or i['refs']) else 'NOTE'
            add(sev, 'FS Input loc %d comp %d (%s, %d static loads) has no VS output at that Location/Component '
                '[Vulkan Interface Matching: "All input variables and blocks must have an interface match in the preceding shader stage" '
                '(except FS built-ins); the value is undefined]. Public Adreno precedent: Cemu #2029 (unmatched FS input -> -13 on Adreno X1-85).'
                % (i['loc'], i['comp'], i['tstr'], i['loads']))
        elif o is not None and i is None:
            st = 'OUT-UNUSED' + ('' if o['stores'] else '/UNWRITTEN')
            if not o['stores']:
                add('NOTE', 'VS Output loc %d (%s) is never stored (undefined value); no FS input reads it' % (o['loc'], o['tstr']))
        else:
            probs = []
            if o['tstr'] != i['tstr']:
                probs.append('TYPE %s != %s' % (o['tstr'], i['tstr']))
                add('VIOLATION', 'Location %d: VS Output %s vs FS Input %s types differ '
                    '[Vulkan Interface Matching: types must be equivalent (a wider output vector is allowed only with maintenance4)]'
                    % (i['loc'], o['tstr'], i['tstr']))
            if o['comp'] != i['comp']:
                probs.append('COMPONENT %d != %d' % (o['comp'], i['comp']))
                add('VIOLATION', 'Location %d: Component decoration differs (VS %d, FS %d)' % (i['loc'], o['comp'], i['comp']))
            if o['relaxed'] != i['relaxed']:
                add('NOTE', 'Location %d: RelaxedPrecision differs (VS %s, FS %s) - explicitly exempt from matching' % (i['loc'], o['relaxed'], i['relaxed']))
            if (o['flat'], o['nopersp'], o['centroid'], o['sample']) != (i['flat'], i['nopersp'], i['centroid'], i['sample']):
                probs.append('INTERP %s != %s' % (interp(o), interp(i)))
                add('NOTE', 'Location %d: interpolation decorations differ (VS %s, FS %s); legal in a monolithic pipeline (only library pipelines '
                    'without graphicsPipelineLibraryIndependentInterpolationDecoration require equality)' % (i['loc'], interp(o), interp(i)))
            if not o['stores']:
                probs.append('VS-UNWRITTEN')
                add('NOTE', 'Location %d: FS reads an output the VS never stores (undefined value, legal)' % i['loc'])
            if o['bits'] != i['bits']:
                add('VIOLATION' if o['tstr'] != i['tstr'] else 'NOTE', 'Location %d: width %d (VS) vs %d (FS)' % (i['loc'], o['bits'], i['bits']))
            st = ' '.join(probs) or 'OK'
        pair.append(dict(loc=r['loc'], comp=r['comp'], o=o, i=i, status=st))
    # -- builtins and other legal-but-unusual patterns
    for r in frows:
        if r['dir'] == 'Input' and r['builtin'] == 'PointCoord':
            tn = {None: 'topology unknown', 'tri': 'triangle', 'list': 'triangle', 'line': 'line', 'point': 'point'}.get(topology, topology)
            if topology in (None, 'tri', 'list', 'line'):
                add('NOTE', 'FS reads gl_PointCoord (%d static loads); %s: Vulkan Built-In Variables - PointCoord holds an undefined value when the '
                    'primitive is not a point. Legal. The driver must still link a point-sprite varying.' % (r['loads'], tn))
        if r['dir'] == 'Input' and r['builtin'] in ('FrontFacing', 'SampleId', 'SamplePosition', 'PrimitiveId', 'Layer', 'ViewportIndex') \
                and r['kind'] in ('i', 'u') and not r['flat']:
            add('VIOLATION', 'FS builtin %s lacks Flat' % r['builtin'])
    if any(r['builtin'] and 'PointSize' in r['builtin'] for r in vrows if r['dir'] == 'Output') is False and topology == 'point':
        add('NOTE', 'point topology but the VS does not write PointSize (undefined point size)')
    vpos = [r for r in vout if r['builtin'] and 'Position' in r['builtin']]
    if not vpos:
        add('VIOLATION', 'VS does not declare a Position output (VUID: the last pre-rasterization stage must write Position)')
    elif not any(r['stores'] for r in vpos):
        add('NOTE', 'VS declares Position but never stores it')
    # VS outputs that no FS input consumes: legal (spec: "Shaders can declare and write to output variables that are not declared or read")
    for p in pair:
        if p['status'].startswith('OUT-UNUSED') and p['o']['stores']:
            add('NOTE', 'VS Output loc %d (%s) is written but the FS declares no input there (legal; the driver may or may not drop it)' % (p['loc'], p['o']['tstr']))
    # FS inputs declared and never loaded: static non-use
    for r in fin:
        if r['loc'] is not None and r['refs'] == 0:
            add('NOTE', 'FS Input loc %d %s is declared in the entry-point interface but never loaded (not statically used)' % (r['loc'], r['tstr']))
    return pair, F


def pair_table(pair):
    out = ['PAIR (VS Output -> FS Input), keyed by Location/Component',
           '  loc comp  VS type          VS st  VS interp  | FS type          FS ld  FS interp  | status']
    for p in pair:
        o, i = p['o'], p['i']
        out.append('  %3d %4d  %-15s %5s  %-9s  | %-15s %5s  %-9s  | %s' % (
            p['loc'], p['comp'],
            o['tstr'][:15] if o else '-', o['stores'] if o else '-', interp(o) if o else '-',
            i['tstr'][:15] if i else '-', i['loads'] if i else '-', interp(i) if i else '-', p['status']))
    return out


def main(argv):
    topo = None; quiet = False; paths = []
    k = 1
    while k < len(argv):
        a = argv[k]
        if a == '--topology' and k + 1 < len(argv): topo = argv[k + 1]; k += 1
        elif a == '--quiet': quiet = True
        elif a.startswith('-'): print(__doc__); return 2
        else: paths.append(a)
        k += 1
    if len(paths) != 2:
        print(__doc__); return 2
    try:
        vs = load_stage(paths[0], 0); fs = load_stage(paths[1], 4)
    except (OSError, ValueError, struct.error) as e:
        print('error:', e, file=sys.stderr); return 2
    pair, F = audit(vs, fs, topo)
    if not quiet:
        for lab, (m, ep, rows) in (('VS', vs), ('FS', fs)):
            print('\n'.join(stage_table(lab, m, ep, rows))); print()
            lt = live_table(m, rows)
            if lt: print('%s input load positions (approximate live range of each loaded value)' % lab); print('\n'.join(lt)); print()
        print('\n'.join(pair_table(pair))); print()
    nv = 0
    for sev, msg in F:
        nv += sev == 'VIOLATION'
        print('%-9s %s' % (sev, msg))
    print('%d violation(s), %d note(s)' % (nv, len(F) - nv))
    return 1 if nv else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
