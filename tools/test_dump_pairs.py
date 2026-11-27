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


if __name__ == '__main__':
    unittest.main()
