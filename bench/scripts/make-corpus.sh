#!/usr/bin/env bash
# make-corpus.sh <dir> <list>: list every .nix file under one source tree,
# such as a nixpkgs checkout or its store path
find "$1" -name '*.nix' -type f | sort > "$2"
wc -l < "$2"
