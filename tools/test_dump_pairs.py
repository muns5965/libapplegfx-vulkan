#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Unit test of tools/dump_pairs.py on a synthetic index with REUSED function refs (the shape of a real dump)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dump_pairs  # noqa: E402

IDX = """\
pso task=2 ref=12 function_refs=[11,10] vertex_attrs=[] stride=48
pso_root task=2 ref=12 status=ok tag1=11 tag2=10 scan=[11,10,16]
fn task=2 ref=11 library_va=0x1 file=lib_aaaa.bin full_size=1 saved_size=1 truncated=0 hash=aaaa000000000001 functions=[air.vertexFetchFunction(stage=1,bc=1)]
fn task=2 ref=10 library_va=0x2 file=lib_bbbb.bin full_size=1 saved_size=1 truncated=0 hash=bbbb000000000002 functions=[FragOne(stage=2,bc=1)]
draw n=0 task=2 draw_tag=7 prim=3 pipeline=12
pso task=2 ref=27 function_refs=[11,9] vertex_attrs=[] stride=48
pso_root task=2 ref=27 status=ok tag1=11 tag2=9 scan=[9]
fn task=2 ref=9 library_va=0x3 file=lib_cccc.bin full_size=1 saved_size=1 truncated=0 hash=cccc000000000003 functions=[FragTwo(stage=2,bc=1)]
obj task=2 ref=5 type=7
pso task=2 ref=13 function_refs=[11,10] vertex_attrs=[] stride=32
pso_root task=2 ref=13 status=ok tag1=11 tag2=10 scan=[11,10]
fn task=2 ref=11 library_va=0x4 file=lib_dddd.bin full_size=1 saved_size=0 truncated=0 hash=dddd000000000004 functions=[air.vertexFetchFunction(stage=1,bc=1)]
fn task=2 ref=10 library_va=0x2 file=lib_eeee.bin full_size=1 saved_size=0 truncated=0 hash=eeee000000000005 functions=[FragThree(stage=2,bc=1)]
res task=2 ref=16 type=1
pso task=3 ref=1 function_refs=[11,10] vertex_attrs=[] stride=8
"""


class T(unittest.TestCase):
    def test_reused_refs(self):
        got = dump_pairs.distinct_hash_pairs(IDX.splitlines())
        self.assertEqual(got, [('aaaa000000000001', 'bbbb000000000002'),     # first pso: the fn lines AFTER it
                               ('aaaa000000000001', 'cccc000000000003'),     # second pso: ref 11 keeps its previous library, ref 9 is defined by the fn line after it
                               ('dddd000000000004', 'eeee000000000005')])    # third pso: the refs are redefined right after it
        # the old "last fn line of the ref wins" derivation would have given (dddd, eeee) for every pso and never the first two
        last = {}
        for l in IDX.splitlines():
            m = dump_pairs.FN.match(l)
            if m:
                last[m[2]] = m[3]
        self.assertEqual({last['11'], last['10']}, {'dddd000000000004', 'eeee000000000005'})

    def test_other_task_is_not_mixed(self):
        # a pso of task 3 names refs that task 3 never defined: unresolved, not guessed from task 2
        self.assertFalse([p for p in dump_pairs.pairs(IDX.splitlines()) if p[0][0] == 'dddd000000000004'][1:])


def fn(task, ref, h, name=None, stage=1):
    return 'fn task=%d ref=%d library_va=0x1 file=lib_%s.bin full_size=1 saved_size=1 truncated=0 hash=%s functions=[%s(stage=%d,bc=1)]' % (task, ref, h, h, name or 'f' + h, stage)


def pso(task, ref, v, f):
    return 'pso task=%d ref=%d function_refs=[%d,%d] vertex_attrs=[] stride=8' % (task, ref, v, f)


def root(task, ref, v, f):
    return 'pso_root task=%d ref=%d status=ok tag1=%d tag2=%d scan=[%d,%d]' % (task, ref, v, f, v, f)


def old_last_wins(lines):
    """The first (wrong) derivation, check-dump-spirv.sh: the LAST fn line of a ref in the whole file defines it."""
    last = {}
    for l in lines:
        m = dump_pairs.FN.match(l)
        if m:
            last[(m[1], m[2])] = m[3]
    out = []
    for l in lines:
        m = dump_pairs.PSO.match(l)
        if m and (m[1], m[2]) in last and (m[1], m[3]) in last:
            out.append((last[(m[1], m[2])], last[(m[1], m[3])]))
    return out


def old_before_seeded(lines):
    """The second (wrong) derivation, check-stage-interfaces.py: fn lines BEFORE the pso win, seeded with the last one of the file."""
    cur = {}
    for l in lines:
        m = dump_pairs.FN.match(l)
        if m:
            cur[(m[1], m[2])] = m[3]
    out = []
    seen = {}
    for l in lines:
        m = dump_pairs.FN.match(l)
        if m:
            seen[(m[1], m[2])] = m[3]
            continue
        m = dump_pairs.PSO.match(l)
        if m:
            a, b = seen.get((m[1], m[2]), cur.get((m[1], m[2]))), seen.get((m[1], m[3]), cur.get((m[1], m[3])))
            if a and b:
                out.append((a, b))
    return out


def new_pairs(lines):
    return [(v[0], f[0]) for v, f in dump_pairs.pairs(lines)]


class Robust(unittest.TestCase):
    def test_ref_reuse_across_pso_lines(self):
        # pso 1 defines refs 1 and 2 right after itself; pso 2 redefines ONLY ref 1: ref 2 keeps its previous library
        L = [pso(2, 20, 1, 2), fn(2, 1, 'a1', 'VtxA'), fn(2, 2, 'b1', 'FragB'), 'draw n=0 task=2',
             pso(2, 21, 1, 2), fn(2, 1, 'c1', 'VtxC'), 'draw n=1 task=2']
        self.assertEqual(new_pairs(L), [('a1', 'b1'), ('c1', 'b1')])
        self.assertEqual(old_last_wins(L), [('c1', 'b1'), ('c1', 'b1')])          # mispairs pso 1 (and never produces (a1, b1))
        self.assertEqual(old_before_seeded(L), [('c1', 'b1'), ('a1', 'b1')])      # pso 1 has no fn line before it: seeded with the file's last (c1); pso 2 sees the stale a1: both wrong
        self.assertNotEqual(new_pairs(L), old_last_wins(L))
        self.assertNotEqual(new_pairs(L), old_before_seeded(L))

    def test_old_before_logic_mispairs_a_late_definition(self):
        # the fn lines come AFTER their pso; a "fn before the pso wins" derivation picks up the PREVIOUS pso's definition for a redefined ref
        L = [pso(2, 20, 1, 2), fn(2, 1, 'a1'), fn(2, 2, 'b1'), 'draw n=0 task=2',
             pso(2, 21, 1, 2), fn(2, 1, 'a2'), fn(2, 2, 'b2'), 'draw n=1 task=2']
        self.assertEqual(new_pairs(L), [('a1', 'b1'), ('a2', 'b2')])
        self.assertEqual(old_before_seeded(L), [('a2', 'b2'), ('a1', 'b1')])      # seeded with the last / lagging by one pso: both pairs wrong (reversed)
        self.assertEqual(old_last_wins(L), [('a2', 'b2'), ('a2', 'b2')])

    def test_several_psos_share_a_function(self):
        L = [pso(2, 20, 1, 2), fn(2, 1, 'v1'), fn(2, 2, 'f1'), 'draw n=0 task=2',
             pso(2, 21, 1, 3), fn(2, 3, 'f2'), 'draw n=1 task=2',                # vertex function 1 shared with the first pso
             pso(2, 22, 4, 2), fn(2, 4, 'v2'), 'draw n=2 task=2',                # fragment function 2 shared with the first pso
             pso(2, 23, 1, 2), 'draw n=3 task=2']                                # all four refs defined already: a pso without any fn line after it
        self.assertEqual(new_pairs(L), [('v1', 'f1'), ('v1', 'f2'), ('v2', 'f1'), ('v1', 'f1')])
        self.assertEqual(dump_pairs.distinct_hash_pairs(L), [('v1', 'f1'), ('v1', 'f2'), ('v2', 'f1')])   # the duplicate pair is reported once
        # function names / stages travel with the pair (check-stage-interfaces.py needs them)
        v, f = dump_pairs.pairs(L)[1]
        self.assertEqual((v[1], v[2], f[1], f[2]), ('fv1', '1', 'ff2', '1'))

    def test_missing_refs(self):
        L = [pso(2, 20, 1, 2), fn(2, 1, 'v1'), 'draw n=0 task=2',               # ref 2 never defined (yet): no pair
             pso(2, 21, 1, 9), 'draw n=1 task=2',                               # ref 9 never defined at all
             fn(2, 2, 'f1'), fn(2, 9, 'f9'), 'res task=2',                      # defined AFTER both psos were resolved: not applied retroactively
             pso(2, 22, 1, 2), 'draw n=2 task=2']                               # now ref 2 is known
        self.assertEqual(new_pairs(L), [('v1', 'f1')])
        self.assertEqual(old_last_wins(L), [('v1', 'f1'), ('v1', 'f9'), ('v1', 'f1')])   # the old derivation invented a pair for each unresolved pso
        self.assertEqual(new_pairs([pso(2, 1, 5, 6)]), [])                      # pso only, no fn line anywhere
        self.assertEqual(new_pairs([]), [])
        self.assertEqual(new_pairs(['nonsense', 'pso task=x']), [])

    def test_root_lines_do_not_resolve_a_pso_early(self):
        # `pso_root` sits between a pso and ITS fn lines: it must not resolve the pso (before the fn lines), and a line of any other kind must
        L = [pso(2, 20, 1, 2), root(2, 20, 1, 2), fn(2, 1, 'v1'), fn(2, 2, 'f1'), root(2, 20, 1, 2), 'draw n=0 task=2']
        self.assertEqual(new_pairs(L), [('v1', 'f1')])
        # the same pso with an unrelated line between it and its fn lines resolves BEFORE them: unresolved (no earlier definition)
        L2 = [pso(2, 20, 1, 2), 'obj task=2 ref=5 type=7', fn(2, 1, 'v1'), fn(2, 2, 'f1')]
        self.assertEqual(new_pairs(L2), [])
        # two root lines and a pso whose root says status != ok still pair by function_refs
        L3 = [pso(2, 20, 1, 2), root(2, 20, 1, 2).replace('status=ok', 'status=fail'), root(2, 20, 1, 2), fn(2, 1, 'v1'), fn(2, 2, 'f1')]
        self.assertEqual(new_pairs(L3), [('v1', 'f1')])

    def test_consecutive_pso_lines(self):
        # a pso directly followed by another pso is resolved with the definitions known so far (its own fn lines would come after the second pso)
        L = [pso(2, 20, 1, 2), fn(2, 1, 'v1'), fn(2, 2, 'f1'), 'draw n=0 task=2',
             pso(2, 21, 1, 2), pso(2, 22, 1, 2), fn(2, 1, 'v2'), 'draw n=1 task=2']
        self.assertEqual(new_pairs(L), [('v1', 'f1'), ('v1', 'f1'), ('v2', 'f1')])

    def test_tasks_do_not_share_refs(self):
        L = [pso(2, 20, 11, 10), fn(2, 11, 'v2'), fn(2, 10, 'f2'), 'draw n=0 task=2',
             pso(3, 20, 11, 10), fn(3, 11, 'v3'), fn(3, 10, 'f3'), 'draw n=0 task=3',
             pso(4, 20, 11, 10), 'draw n=0 task=4']                              # task 4 never defined ref 11 / 10
        self.assertEqual(new_pairs(L), [('v2', 'f2'), ('v3', 'f3')])
        self.assertEqual(old_last_wins(L), [('v2', 'f2'), ('v3', 'f3')])   # old logic keys by (task, ref) too: this case is NOT one it mispaired

    def test_cli_prints_sorted_distinct_pairs(self):
        import io
        import tempfile
        from contextlib import redirect_stdout
        L = [pso(2, 20, 1, 2), fn(2, 1, 'v2'), fn(2, 2, 'f1'), 'draw', pso(2, 21, 3, 2), fn(2, 3, 'v1'), 'draw', pso(2, 22, 1, 2), 'draw']
        with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as t:
            t.write('\n'.join(L) + '\n')
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                rc = dump_pairs.main(['dump_pairs.py', t.name])
            self.assertEqual(rc, 0)
            self.assertEqual(buf.getvalue().splitlines(), ['v1 f1', 'v2 f1'])
        finally:
            os.unlink(t.name)
        self.assertEqual(dump_pairs.main(['dump_pairs.py']), 2)


if __name__ == '__main__':
    unittest.main()
