#!/usr/bin/env bash
# build-bench.sh <prefix> <output> [compiler flags]: build bench/parse-bench.cc
# against an installed prefix, such as outputs/out of this checkout or of a
# master checkout for the Bison baseline
set -e
cd "$(dirname "$0")/../.."
prefix=$(realpath "$1"); out=$2; shift 2
nix develop ".#${NIX_DEVSHELL:-native-ccacheStdenv}" -c bash -c "
export PKG_CONFIG_PATH=$prefix/lib/pkgconfig:\$PKG_CONFIG_PATH
g++ -std=c++23 -O2 -g -fno-omit-frame-pointer $* bench/parse-bench.cc -o $out \$(pkg-config --cflags --libs nix-expr nix-store nix-fetchers nix-util) -Wl,-rpath,$prefix/lib
"
