#!/usr/bin/env bash
# ab.sh <list> [reps] [rounds]: time bench/run/bench-dev against the
# libraries saved in bench/run/lib-prev (by `ab.sh save` before a rebuild)
# and against the current outputs/out, alternating, on one thread
cd "$(dirname "$0")/../.."
if [ "$1" = save ]; then
  rm -rf bench/run/lib-prev && cp -a outputs/out/lib bench/run/lib-prev
  exit
fi
for i in $(seq "${3:-3}"); do
  for side in prev cur; do
    lib=$PWD/outputs/out/lib
    [ $side = prev ] && lib=$PWD/bench/run/lib-prev
    echo "$side $(LD_LIBRARY_PATH=$lib NIX_LEXER_THREADS=1 NIX_PARSER_THREADS=1 bench/run/bench-dev "$1" "${2:-5}" 2>/dev/null | grep -E '^(lex|bind)' | tr -s ' ' | tr '\n' ' ')"
  done
done
