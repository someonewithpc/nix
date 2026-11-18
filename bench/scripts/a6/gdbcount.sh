#!/usr/bin/env bash
# gdbcount.sh <out> <fns...> -- <cmd...>: count calls to fns in the parent process
out=$1; shift; args=()
while [ "$1" != -- ]; do args+=(-ex "break $1" -ex "ignore \$bpnum 100000000"); shift; done; shift
timeout 900 /nix/store/000b7g0nqi237818s99w3x0by5m3iakf-gdb-17.2/bin/gdb -q -batch -ex "set pagination off" -ex "set breakpoint pending on" -ex "set detach-on-fork on" "${args[@]}" -ex run -ex "info breakpoints" --args "$@" > $out 2>&1
grep -E "already hit|^[0-9]+ +breakpoint" $out | sed -E 's/\(.*//; s/.* in //; s/.*already hit//' | paste - - 
