#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for spv_iface_audit.py. Fixtures are assembled in-process (no spirv-as needed), so they run offline.

Run with:  python3 tools/test_spv_iface_audit.py
The real-bundle test runs only when the failure bundles named in BUNDLES exist on this machine.
"""
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOL_DIR))
import spv_iface_audit as A   # noqa: E402

BUNDLES = [('/tmp/x231501/dump/pipefail_b7d3e8365670fd6c', 346780), ('/tmp/vm2/dump/pipefail_18d04777d220e041', 348036)]


def ins(op, *ops):
    ws = []
    for x in ops:
        ws.extend(x if isinstance(x, list) else [x])
    return [(len(ws) + 1) << 16 | op] + ws


def lit_str(s):
    b = s.encode() + b'\0'
    b += b'\0' * (-len(b) % 4)
    return list(struct.unpack('<%dI' % (len(b) // 4), b))


D = dict(Flat=14, NoPersp=13, Centroid=16, Sample=17, Relaxed=0)


def build(model, vars_, sio16=False, with_position=None):
    """vars_: list of dict(dir, loc, comp=0, kind 'f'|'i'|'u', bits=32, lanes=1, deco=(), use='load'|'store'|None, builtin=None)."""
    nid = [0]
    def new():
        nid[0] += 1
        return nid[0]
    tys = {}; out = []; decs = []; types = []; gvars = []; body = []; iface = []
    def ty(key, make):
        if key not in tys:
            tys[key] = new(); types.append(make(tys[key]))
        return tys[key]
    void = ty('void', lambda i: ins(A.OP['TypeVoid'], i))
    fnty = ty('fn', lambda i: ins(33, i, void))
    def scalar(kind, bits):
        if kind == 'f': return ty(('f', bits), lambda i: ins(A.OP['TypeFloat'], i, bits))
        return ty((kind, bits), lambda i: ins(A.OP['TypeInt'], i, bits, 1 if kind == 'i' else 0))
    def vec(kind, bits, lanes):
        s = scalar(kind, bits)
        return s if lanes == 1 else ty(('v', kind, bits, lanes), lambda i: ins(A.OP['TypeVector'], i, s, lanes))
    hasp = False
    for v in vars_:
        t = vec(v.get('kind', 'f'), v.get('bits', 32), v.get('lanes', 1))
        sc = 1 if v['dir'] == 'Input' else 3
        p = ty(('p', sc, t), lambda i: ins(A.OP['TypePointer'], i, sc, t))
        vid = new(); gvars.append(ins(A.OP['Variable'], p, vid, sc)); iface.append(vid)
        if v.get('builtin') is not None:
            decs.append(ins(A.OP['Decorate'], vid, A.DEC['BuiltIn'], v['builtin']))
            hasp |= v['builtin'] == 0
        else:
            decs.append(ins(A.OP['Decorate'], vid, A.DEC['Location'], v['loc']))
            if v.get('comp'): decs.append(ins(A.OP['Decorate'], vid, A.DEC['Component'], v['comp']))
        for d in v.get('deco', ()):
            decs.append(ins(A.OP['Decorate'], vid, D[d]))
        if v.get('use') == 'load':
            r = new(); body.append(ins(A.OP['Load'], t, r, vid))
        elif v.get('use') == 'store':
            c = ty(('null', t), lambda i: ins(44, t, i))   # OpConstantNull
            body.append(ins(A.OP['Store'], vid, c))
    caps = [ins(A.OP['Capability'], 1)] + ([ins(A.OP['Capability'], 9), ins(A.OP['Capability'], A.CAP_SIO16)] if sio16 else [])
    fid = new(); lab = new()
    words = []
    words += caps
    words += [ins(14, 0, 1)]
    words += [ins(A.OP['EntryPoint'], model, fid, lit_str('main'), iface)]
    if model == 4: words += [ins(16, fid, 7)]
    words += decs + types + gvars
    words += [ins(A.OP['Function'], void, fid, 0, fnty), ins(248, lab)] + body + [ins(253), ins(A.OP['FunctionEnd'])]
    flat = [w for i in words for w in i]
    return struct.pack('<%dI' % (5 + len(flat)), 0x07230203, 0x00010400, 0, nid[0] + 1, 0, *flat)


POS = dict(dir='Output', builtin=0, lanes=4, use='store')


def vso(loc, lanes=2, **kw): return dict(dir='Output', loc=loc, lanes=lanes, use='store', **kw)
def fsi(loc, lanes=2, **kw): return dict(dir='Input', loc=loc, lanes=lanes, use='load', **kw)


def audit_bytes(vs, fs, topo=None):
    def ld(b, model):
        with tempfile.NamedTemporaryFile(suffix='.spv', delete=False) as f:
            f.write(b)
        try:
            return A.load_stage(f.name, model)
        finally:
            os.unlink(f.name)
    return A.audit(ld(vs, 0), ld(fs, 4), topo)


def sev(findings, s, needle=''):
    return [m for k, m in findings if k == s and needle in m]


class Fixtures(unittest.TestCase):
    def test_matching_pair_is_clean(self):
        _, f = audit_bytes(build(0, [POS, vso(0, 4), vso(1, 2)]), build(4, [fsi(0, 4), fsi(1, 2)]))
        self.assertEqual(sev(f, 'VIOLATION'), [])

    def test_type_mismatch_is_violation(self):
        _, f = audit_bytes(build(0, [POS, vso(0, 4)]), build(4, [fsi(0, 2)]))
        self.assertTrue(sev(f, 'VIOLATION', 'types differ'))

    def test_half_vs_float_is_violation(self):
        _, f = audit_bytes(build(0, [POS, vso(0, 2, bits=16)], sio16=True), build(4, [fsi(0, 2)]))
        self.assertTrue(sev(f, 'VIOLATION', 'types differ'))

    def test_input_without_output_is_violation(self):   # the Cemu #2029 shape
        _, f = audit_bytes(build(0, [POS, vso(0, 4)]), build(4, [fsi(0, 4), fsi(3, 2)]))
        m = sev(f, 'VIOLATION', 'no VS output')
        self.assertTrue(m and 'Location 3' not in m[0] and 'loc 3' in m[0])

    def test_unused_input_without_output_is_only_a_note(self):
        fs = build(4, [fsi(0, 4), dict(dir='Input', loc=3, lanes=2)])
        _, f = audit_bytes(build(0, [POS, vso(0, 4)]), fs)
        self.assertEqual(sev(f, 'VIOLATION', 'no VS output'), [])
        self.assertTrue(sev(f, 'NOTE', 'never loaded'))

    def test_unconsumed_vs_output_is_note_only(self):
        p, f = audit_bytes(build(0, [POS, vso(0, 4), vso(1, 1)]), build(4, [fsi(0, 4)]))
        self.assertEqual(sev(f, 'VIOLATION'), [])
        self.assertTrue(any(x['status'].startswith('OUT-UNUSED') for x in p))

    def test_integer_input_needs_flat(self):
        vs = build(0, [POS, vso(0, 2, kind='u')])
        _, bad = audit_bytes(vs, build(4, [fsi(0, 2, kind='u')]))
        self.assertTrue(sev(bad, 'VIOLATION', 'Flat'))
        _, ok = audit_bytes(vs, build(4, [fsi(0, 2, kind='u', deco=('Flat',))]))
        self.assertEqual(sev(ok, 'VIOLATION'), [])

    def test_interpolation_difference_is_a_note(self):
        _, f = audit_bytes(build(0, [POS, vso(0, 4)]), build(4, [fsi(0, 4, deco=('NoPersp',))]))
        self.assertEqual(sev(f, 'VIOLATION'), [])
        self.assertTrue(sev(f, 'NOTE', 'interpolation'))

    def test_relaxed_precision_difference_is_exempt(self):
        _, f = audit_bytes(build(0, [POS, vso(0, 4, deco=('Relaxed',))]), build(4, [fsi(0, 4)]))
        self.assertEqual(sev(f, 'VIOLATION'), [])
        self.assertTrue(sev(f, 'NOTE', 'RelaxedPrecision'))

    def test_16bit_io_requires_capability(self):
        _, f = audit_bytes(build(0, [POS, vso(0, 2, bits=16)]), build(4, [fsi(0, 2, bits=16)]))
        self.assertTrue(sev(f, 'VIOLATION', 'StorageInputOutput16'))
        _, g = audit_bytes(build(0, [POS, vso(0, 2, bits=16)], sio16=True), build(4, [fsi(0, 2, bits=16)], sio16=True))
        self.assertEqual(sev(g, 'VIOLATION'), [])

    def test_two_variables_on_one_location_component(self):
        _, f = audit_bytes(build(0, [POS, vso(0, 4), vso(0, 2)]), build(4, [fsi(0, 4)]))
        self.assertTrue(sev(f, 'VIOLATION', 'two variables'))

    def test_component_packing_does_not_collide(self):
        vs = build(0, [POS, vso(0, 2, comp=0), vso(0, 2, comp=2)])
        _, f = audit_bytes(vs, build(4, [fsi(0, 2, comp=0), fsi(0, 2, comp=2)]))
        self.assertEqual(sev(f, 'VIOLATION'), [])

    def test_unwritten_vs_output_read_by_fs_is_a_note(self):
        vs = build(0, [POS, dict(dir='Output', loc=0, lanes=4)])
        _, f = audit_bytes(vs, build(4, [fsi(0, 4)]))
        self.assertEqual(sev(f, 'VIOLATION'), [])
        self.assertTrue(sev(f, 'NOTE', 'never stores'))

    def test_pointcoord_on_triangles_is_noted(self):
        fs = build(4, [fsi(0, 4), dict(dir='Input', builtin=16, lanes=2, use='load')])
        _, f = audit_bytes(build(0, [POS, vso(0, 4)]), fs, 'list')
        self.assertTrue(sev(f, 'NOTE', 'PointCoord'))

    def test_missing_position_is_violation(self):
        _, f = audit_bytes(build(0, [vso(0, 4)]), build(4, [fsi(0, 4)]))
        self.assertTrue(sev(f, 'VIOLATION', 'Position'))

    def test_cli_exit_codes(self):
        with tempfile.TemporaryDirectory() as d:
            for n, b in (('vs', build(0, [POS, vso(0, 4)])), ('ok', build(4, [fsi(0, 4)])), ('bad', build(4, [fsi(0, 2)]))):
                Path(d, n + '.spv').write_bytes(b)
            run = lambda fs: subprocess.run([sys.executable, '-I', str(TOOL_DIR / 'spv_iface_audit.py'), d + '/vs.spv', d + '/%s.spv' % fs],
                                            capture_output=True, text=True)
            r = run('ok'); self.assertEqual(r.returncode, 0, r.stdout + r.stderr); self.assertIn('PAIR', r.stdout)
            r = run('bad'); self.assertEqual(r.returncode, 1); self.assertIn('VIOLATION', r.stdout)
            Path(d, 'junk.spv').write_bytes(b'xxxxxxxxxxxxxxxxxxxxxxxx')
            r = run('junk'); self.assertEqual(r.returncode, 2)


@unittest.skipUnless(all(os.path.exists(b + '/fs.spv') for b, _ in BUNDLES), 'Adreno failure bundles not present')
class RealPair(unittest.TestCase):
    def test_a2xghfc_pair_interface(self):
        """The Adreno 840 -13 pair (VS VfxXgh / FS A2Xghfc): the audit's facts, pinned."""
        for b, nbytes in BUNDLES:
            self.assertEqual(os.path.getsize(b + '/fs.spv'), nbytes)
            vs = A.load_stage(b + '/vs.spv', 0); fs = A.load_stage(b + '/fs.spv', 4)
            pair, f = A.audit(vs, fs, 'list')
            self.assertEqual(sev(f, 'VIOLATION'), [])
            got = {p['loc']: (p['o']['tstr'], p['i']['tstr'] if p['i'] else None, p['status']) for p in pair}
            self.assertEqual(got[0], ('v4f32', None, 'OUT-UNUSED'))
            self.assertEqual(got[1], ('f32', None, 'OUT-UNUSED'))
            for l in (2, 3, 4, 5): self.assertEqual(got[l], ('v2f32', 'v2f32', 'OK'))
            self.assertEqual(got[6], ('v4f32', 'v4f32', 'OK'))
            builtins = sorted(r['builtin'] for r in fs[2] if r['builtin'] and r['dir'] == 'Input')
            self.assertEqual(builtins, ['FragCoord', 'PointCoord'])
            self.assertTrue(all(not (r['flat'] or r['nopersp'] or r['centroid'] or r['sample'] or r['relaxed']) for r in vs[2] + fs[2]))


if __name__ == '__main__':
    unittest.main()
