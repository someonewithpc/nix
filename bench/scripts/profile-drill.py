#!/usr/bin/env python
# profile-drill.py <leaf-regex> [stop-regex] [depth] < perf-script-output:
# for samples with a frame matching leaf-regex among the innermost <depth>
# frames (6 by default), count the first frame above it that matches
# stop-regex (primops, Expr::eval and EvalState methods by default). Shows who
# pays for a generic cost such as path checks or the symbol table
import sys, re, collections
leaf = re.compile(sys.argv[1]); stop = re.compile(sys.argv[2] if len(sys.argv) > 2 else r'nix::prim_\w+|nix::Expr\w+::eval|nix::EvalState::\w+|nix::derivationStrictInternal|ImportPrefetcher::\w+|GC_mark_thread|nix::flake::\w+|nix::fetchers::\w+')
depth = int(sys.argv[3]) if len(sys.argv) > 3 else 6
n = 0; hit = 0; c = collections.Counter(); c2 = collections.Counter(); frames = []
def flush():
    global n, hit, frames
    if not frames: return
    n += 1
    idx = next((i for i, f in enumerate(frames[:depth]) if leaf.search(f)), None)
    if idx is not None:
        hit += 1
        c2[frames[idx].split('(')[0][-90:]] += 1
        for f in frames[idx:]:
            m = stop.search(f)
            if m and not leaf.search(f): c[m.group(0)] += 1; break
        else: c["(none)"] += 1
    frames = []
for line in sys.stdin:
    if not line.strip(): flush(); continue
    if not line.startswith((' ', '\t')): flush(); continue
    p = line.strip().split(None, 1); frames.append(p[1] if len(p) > 1 else p[0])
flush()
print(f"matched {hit}/{n} = {100*hit/n:.2f}%")
for k, v in c.most_common(18): print(f"  {100*v/n:5.2f}%  {k}")
print(" leaf frames:")
for k, v in c2.most_common(12): print(f"  {100*v/n:5.2f}%  {k}")
