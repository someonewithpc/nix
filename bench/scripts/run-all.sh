#!/usr/bin/env bash
# run-all.sh <base-tree> <corpus-list> [work-dir]: build the branch and the
# commit it is based on, compare their dumps, rerun the comparison under
# ASan and UBSan, and build both with O3 and LTO for timing
#
# Logs one line per step; the details stay in the work dir
set -u
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../.." && pwd)
base=$(cd "$1" && pwd)
list=$(realpath "$2")
w=${3:-$tree/bench/run}
mkdir -p "$w"
step() { echo "$(date +%T) $*"; }
b() { local log=$w/build-$(basename "$1")-$2.log; step "build $1 $2"; TREE=$1 "$here/build.sh" "$2" > "$log" 2>&1 || { step "FAILED, see $log"; exit 1; }; }

b "$base" dev
b "$tree" dev
"$here/build-bench.sh" "$base/outputs/out" "$w/bench-old" || exit 1
"$here/build-bench.sh" "$tree/outputs/out" "$w/bench-new" || exit 1
step "check dev"
"$here/check.sh" "$w/bench-old" "$w/bench-new" "$list" "$w/check-dev"

b "$tree" asan
"$here/build-bench.sh" "$tree/outputs/asan" "$w/bench-asan" -fsanitize=address,undefined || exit 1
step "check asan"
ASAN_OPTIONS=detect_leaks=0 "$here/check.sh" "$w/bench-old" "$w/bench-asan" "$list" "$w/check-asan"
grep -l -E 'ERROR: AddressSanitizer|runtime error' "$w"/check-asan/err.txt 2>/dev/null && step "ASan or UBSan reported errors"

b "$base" o3
b "$tree" o3
"$here/build-bench.sh" "$base/outputs/o3" "$w/bench-old-o3" -O3 || exit 1
"$here/build-bench.sh" "$tree/outputs/o3" "$w/bench-new-o3" -O3 || exit 1
step "done"
