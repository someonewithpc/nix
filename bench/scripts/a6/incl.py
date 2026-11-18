import sys, re, collections
# incl.py <perf-script-output> <M per sample> [symbol regex]: inclusive and self instructions per symbol for comm nix
per=float(sys.argv[2]); incl=collections.Counter(); selfc=collections.Counter(); tot=0
frames=[]; cur=None
def flush():
    global tot
    if cur=='nix' and frames:
        tot+=1
        for f in set(frames): incl[f]+=1
        selfc[frames[0]]+=1
for line in open(sys.argv[1]):
    if not line.strip(): flush(); frames=[]; cur=None; continue
    if not line.startswith((' ','\t')): cur=line.split()[0]; continue
    s=line.strip().split(' ',1); sym=s[1] if len(s)>1 else '?'
    sym=re.sub(r'\+0x[0-9a-f]+','',sym); sym=re.sub(r' \(.*\)$','',sym)
    sym=re.sub(r'\(.*','',sym)
    frames.append(sym[:110])
flush()
pat=sys.argv[3] if len(sys.argv)>3 else 'nix::'
print(f"total {tot*per:.1f}M")
for k,v in incl.most_common(400):
    if re.search(pat,k): print(f"{v*per:7.1f}M incl {selfc[k]*per:6.1f}M self  {k}")
