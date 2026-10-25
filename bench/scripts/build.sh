#!/usr/bin/env bash
# build.sh <kind> [jobs]: build this checkout into build-<kind> and install
# into outputs/<kind>
#
#   dev   debugoptimized, frame pointers (outputs/out for kind dev)
#   asan  ASan and UBSan, GC off
#   tsan  TSan, GC off
#   o3    release, -O3, LTO
#
# TREE builds another checkout, such as master for the Bison baseline.
# NIX_DEVSHELL picks the devshell of the flake in that checkout
# (native-ccacheStdenv by default, native where there is no ccache)
set -e
kind=$1
jobs=${2:-$(( $(nproc) * 3 / 4 ))}
cd "${TREE:-$(dirname "$0")/../..}"
shell=${NIX_DEVSHELL:-native-ccacheStdenv}
case $kind in
  dev) bdir=build; bprefix=$PWD/outputs/out
       opts=(-Dbuildtype=debugoptimized "-Dcpp_args=-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer" -Dc_args=-fno-omit-frame-pointer) ;;
  asan) bdir=build-asan; bprefix=$PWD/outputs/asan
       opts=(-Dbuildtype=debugoptimized -Db_sanitize=address,undefined -Db_lundef=false "-Dcpp_args=-fno-omit-frame-pointer -fno-sanitize-recover=undefined" -Dlibexpr:gc=disabled) ;;
  tsan) bdir=build-tsan; bprefix=$PWD/outputs/tsan
       opts=(-Dbuildtype=debugoptimized -Db_sanitize=thread -Db_lundef=false -Dcpp_args=-fno-omit-frame-pointer -Dlibexpr:gc=disabled) ;;
  o3) bdir=build-o3; bprefix=$PWD/outputs/o3
       opts=(-Dbuildtype=release -Doptimization=3 -Db_lto=true -Db_lto_threads=$jobs) ;;
  *) echo "unknown kind $kind" >&2; exit 1 ;;
esac
export bdir bprefix jobs
nix develop ".#$shell" -c bash -c '
set -e
if [ ! -f "$bdir/build.ninja" ]; then
  rm -rf "$bdir"
  meson setup "$bdir" $mesonFlags --prefix="$bprefix" "$@" > "$bdir.setup.log" 2>&1 || { grep ERROR "$bdir.setup.log"; rm -rf "$bdir"; exit 1; }
fi
ninja -C "$bdir" -j"$jobs" 2>&1 | grep -E "error|FAILED" | head -40 || true
ninja -C "$bdir" install >/dev/null
echo "installed $bprefix"
' bash "${opts[@]}"
