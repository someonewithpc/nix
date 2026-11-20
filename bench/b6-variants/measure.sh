#!/usr/bin/env bash
# measure.sh <rounds> "<workloads>" label=prefix...: interleaved instruction counts
set -e
rounds=$1; wls=$2; shift 2
P=$(realpath "$(dirname "$0")/../b3-variants/perf.py")
cd "${OUT:-.}"
for r in $(seq "$rounds"); do
  for wl in $wls; do
    for lp in "$@"; do
      flock /tmp/claude-1000/fe-locks/heavy.lock python "$P" "${lp%%=*}" "${lp#*=}" "$wl"
    done
  done
done
