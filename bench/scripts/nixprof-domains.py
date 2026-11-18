#!/usr/bin/env python
# nixprof-domains.py <folded-stacks>: split a Nix-level profile by the part of
# nixpkgs the innermost frame belongs to
#
#   nix eval --eval-profiler flamegraph --eval-profile-file out.folded \
#     --eval-profiler-frequency 999 <installable>
#
# The profiler samples at function calls, so leaf attribution leans towards
# whatever is called after a long primop. The domain split is steadier
import collections
import re
import sys

domains = [
    ("mkDerivation and check-meta", re.compile(r"/pkgs/stdenv/generic/(make-derivation|check-meta)\.nix")),
    ("packages (pkgs/**)", re.compile(r"/pkgs/")),
    ("module system (lib/modules, types, options)", re.compile(r"/lib/(modules|types|options)\.nix")),
    ("NixOS modules (nixos/**)", re.compile(r"»/nixos/")),
    ("home-manager", re.compile(r"home-manager[^»]*»")),
    ("user flake", re.compile(r"^/etc/nixos/")),
    ("flake glue", re.compile(r"flakes-internal|/flake\.nix")),
]
total = 0
by_domain = collections.Counter()
by_file = collections.Counter()
for line in open(sys.argv[1]):
    stack, count = line.rsplit(" ", 1)
    count = int(count)
    total += count
    frames = stack.split(";")
    domain = None
    for frame in reversed(frames):
        domain = next((name for name, rx in domains if rx.search(frame)), None)
        if domain:
            break
    by_domain[domain or "other"] += count
    m = re.search(r"»(/[^:]+):", frames[-1]) or re.search(r"^(/[^:]+):", frames[-1])
    by_file[m.group(1) if m else frames[-1][:40]] += count
print("samples", total)
for name, count in by_domain.most_common():
    print(f"{100 * count / total:6.2f}%  {name}")
print("leaf file")
for name, count in by_file.most_common(15):
    print(f"{100 * count / total:6.2f}%  {name}")
