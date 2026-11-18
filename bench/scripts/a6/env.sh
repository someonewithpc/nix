# env.sh: shared variables for the a6 probes; S holds the chroot store in $S/st
S=${S:-/tmp/claude-1000/-home-hugo-projects-nix/b2cb8e62-361f-4d78-84ec-c4fa14dd3c56/scratchpad/fe-a6-buildpath}
W=~/projects/worktrees/nix/fe-a6-buildpath
NIX=${NIX:-$W/outputs/out/bin/nix}
PERF=/nix/store/hln3gq5s0pajpqlwcffqadzxglqfhx29-perf-linux-7.3.4/bin/perf
ST="local?root=$S/st"
XE=(--extra-experimental-features nix-command --option post-build-hook "" --option substituters "")
XO=("${XE[@]}" --no-link)
LOCK=/tmp/claude-1000/fe-locks/heavy.lock
