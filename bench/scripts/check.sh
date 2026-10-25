#!/usr/bin/env bash
# check.sh <old-bench> <new-bench> <list> [work-dir]: compare the AST and doc
# comment dumps of two parse-bench builds, serially and on 8 threads, with
# the default sizes and with sizes small enough that every file goes through
# the parallel lexer and parser
w=${4:-$(mktemp -d)}
mkdir -p "$w"
"$1" "$3" 1 --dump "$w/old.txt" >/dev/null 2>&1 || { echo "old bench failed"; exit 1; }
ref=$(sha256sum < "$w/old.txt")
run() {
  rm -f "$w/new.txt"
  env "$@" "$NEW" "$LIST" 1 $MODE --dump "$w/new.txt" >/dev/null 2>>"$w/err.txt"
  [ -s "$w/new.txt" ] && [ "$(sha256sum < "$w/new.txt")" = "$ref" ] && echo "same  $MODE $*" || echo "DIFF  $MODE $* (see $w)"
}
NEW=$2 LIST=$3
for MODE in "" "--threads 8"; do
  run NIX_LEXER_THREADS=1 NIX_PARSER_THREADS=1
  run X=default
  run NIX_LEXER_MIN_CHUNK=4096 NIX_LEXER_THREADS=8 NIX_PARSER_MIN_PIECE=64 NIX_PARSER_THREADS=8
  run NIX_LEXER_MIN_CHUNK=64 NIX_LEXER_THREADS=3 NIX_PARSER_MIN_PIECE=8 NIX_PARSER_THREADS=3
done
