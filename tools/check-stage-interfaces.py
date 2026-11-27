#!/usr/bin/env python3
"""Checks the VS output / FS input interface of every vertex+fragment pair in an
arm64 GPU dump (index.txt) against the SPIR-V that dump-translate produced:
every FS Location input must have a VS output of the same type at that Location.
Pairs come from tools/dump_pairs.py (the same list check-dump-spirv.sh hands to dump-translate).
Usage: check-stage-interfaces.py <spv dir> <dump index.txt>   (exit 1 on a type
mismatch; an FS input with no VS output is reported but only counted, since Metal
leaves it undefined — point_coord, unused inputs)."""
import re,subprocess,sys,glob,os
rt=sys.argv[1]; idx=sys.argv[2]
def iface(path):
    pr=subprocess.run(['spirv-dis',path],capture_output=True,text=True)
    if pr.returncode: print('spirv-dis FAILED',path); sys.exit(1)
    t=pr.stdout
    loc={};bi={};types={};ptr={};vars_=[]
    for l in t.splitlines():
        m=re.match(r'\s*OpDecorate (%\S+) Location (\d+)',l)
        if m: loc[m[1]]=int(m[2])
        m=re.match(r'\s*OpDecorate (%\S+) BuiltIn (\S+)',l)
        if m: bi[m[1]]=m[2]
        m=re.match(r'\s*(%\S+) = OpTypePointer (Input|Output) (%\S+)',l)
        if m: ptr[m[1]]=(m[2],m[3])
        m=re.match(r'\s*(%\S+) = OpVariable (%\S+) (Input|Output)',l)
        if m: vars_.append((m[1],m[2],m[3]))
    r={'Input':{},'Output':{}}
    for v,p,sc in vars_:
        ty=ptr[p][1]
        if v in bi: r[sc]['B:'+bi[v]]=ty
        else: r[sc][loc.get(v,-1)]=ty
    return r
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
import dump_pairs   # the one pair derivation (tools/dump_pairs.py): refs are reused by the guest, the fn lines AFTER a pso define its functions
pairs=dump_pairs.pairs(open(idx).read().splitlines())
seen=set();bad=0;n=0;hard=[]
for vs,fs in pairs:
    k=(vs[0],fs[0])
    if k in seen: continue
    seen.add(k)
    vp=glob.glob(f'{rt}/{vs[0]}_*_vs.spv'); fp=glob.glob(f'{rt}/{fs[0]}_*_fs__vs{vs[0][:8]}.spv') or glob.glob(f'{rt}/{fs[0]}_*_fs.spv')   # pair-specific translation first
    if not vp or not fp:
        print('MISSING translated shader for pair',k); hard.append((k,['missing spv'])); continue
    v=iface(vp[0]); f=iface(fp[0]); n+=1
    vo=v['Output']; fi=f['Input']
    probs=[]
    for l,t in fi.items():
        if isinstance(l,str): continue
        if l not in vo: probs.append(f'FS in loc{l} {t} has no VS out')
        elif vo[l]!=t: probs.append(f'loc{l} VS {vo[l]} != FS {t}')
    mism=[x for x in probs if 'has no VS out' not in x]
    if mism: hard.append((k,mism))
    print(vs[0][:8],fs[0][:8],fs[1],'VSout',{k:v for k,v in vo.items() if not isinstance(k,str)},'FSin',{k:v for k,v in fi.items() if not isinstance(k,str)},'|',probs or 'OK')
    bad+=bool(probs)
print(n,'pairs',bad,'with problems;',len(hard),'type mismatches')
sys.exit(1 if hard else 0)
