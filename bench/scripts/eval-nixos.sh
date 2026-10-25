#!/bin/sh
# eval-nixos.sh <lib-dir> [nix]: evaluate the system of nixos.nix with the
# nix binary of this checkout (or the one given) loading <lib-dir>
cd "$(dirname "$0")"
np=${NIXPKGS:-/nix/store/mf1n715j8c2pvz6cwzajhhfbmjqanmi7-source}
LD_LIBRARY_PATH=$1 exec "${2:-../../outputs/out/bin/nix}" --extra-experimental-features nix-command \
  eval --read-only --impure --raw --expr "import ./nixos.nix { nixpkgs = \"$np\"; }"
