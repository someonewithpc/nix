#!/usr/bin/env bash
# runchains.sh <tag> <count> <hook>: build <count> fresh 13-chains from instantiated drvs
source "$(dirname "$0")/env.sh"
for i in $(seq $2); do cat drvs-$1-$i.txt; echo; done | while read d; do
  $NIX build --store "$ST" "${XO[@]}" --option post-build-hook "$3" "$d^*" 2>/dev/null || echo FAIL
done
