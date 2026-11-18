#!/usr/bin/env bash

source ./common.sh

requireGit

# Test basic caching: trace should only appear on first evaluation
basicCacheDir="$TEST_ROOT/basic-cache-flake"
createGitRepo "$basicCacheDir" ""
cp "${config_nix}" "$basicCacheDir/"
git -C "$basicCacheDir" add config.nix
git -C "$basicCacheDir" commit -m "config.nix"

cat >"$basicCacheDir/flake.nix" <<EOF
{
  description = "Basic cache test";
  outputs = { self }: let inherit (import ./config.nix) mkDerivation; in {
    # Assert pure-eval is enabled (builtins.currentSystem is unavailable in pure mode)
    packages.$system.default = assert builtins ? currentSystem -> throw "pure-eval not enabled";
      builtins.trace "basic-test-evaluating" (mkDerivation {
        name = "cached-build";
        buildCommand = "echo hello > \\\$out";
      });
  };
}
EOF

git -C "$basicCacheDir" add flake.nix
git -C "$basicCacheDir" commit -m "Init"

clearBinaryCache

# First build should show trace
nix build --no-link "$basicCacheDir" 2>&1 | grepQuiet 'trace: basic-test-evaluating'

# Second build should use cache, no trace output
nix build --no-link "$basicCacheDir" 2>&1 | grepQuietInverse 'trace: basic-test-evaluating'

# Third build should also use cache, no trace output
nix build --no-link "$basicCacheDir" 2>&1 | grepQuietInverse 'trace: basic-test-evaluating'

# Test edge cases with separate flake
flake1Dir="$TEST_ROOT/eval-cache-flake"

createGitRepo "$flake1Dir" ""
cp ../simple.nix ../simple.builder.sh "${config_nix}" "$flake1Dir/"
git -C "$flake1Dir" add simple.nix simple.builder.sh config.nix
git -C "$flake1Dir" commit -m "config.nix"

cat >"$flake1Dir/flake.nix" <<EOF
{
  description = "Fnord";
  outputs = { self }: let inherit (import ./config.nix) mkDerivation; in {
    foo.bar = throw "breaks";
    drv = mkDerivation {
      name = "build";
      buildCommand = ''
        echo true > \$out
      '';
    };
    stack-depth =
      let
        f = x: if x == 0 then true else f (x - 1);
      in
        assert (f 100); self.drv;
    ifd = assert (import self.drv); self.drv;
  };
}
EOF

git -C "$flake1Dir" add flake.nix
git -C "$flake1Dir" commit -m "Init"

expect 1 nix build "$flake1Dir#foo.bar" 2>&1 | grepQuiet 'error: breaks'
expect 1 nix build "$flake1Dir#foo.bar" 2>&1 | grepQuiet 'error: breaks'

# Stack overflow error must not be cached
expect 1 nix build --max-call-depth 50 "$flake1Dir#stack-depth" 2>&1 \
  | grepQuiet 'error: stack overflow; max-call-depth exceeded'
# If the SO is cached, the following invocation will produce a cached failure; we expect it to succeed
nix build --no-link "$flake1Dir#stack-depth"

# Conditional error should not be cached
expect 1 nix build "$flake1Dir#ifd" --option allow-import-from-derivation false 2>&1 \
  | grepQuiet 'error: cannot build .* during evaluation because the option '\''allow-import-from-derivation'\'' is disabled'
nix build --no-link "$flake1Dir#ifd"

# nix eval answers from the cache and prints what a fresh eval prints
evalDir="$TEST_ROOT/eval-cache-eval"
createGitRepo "$evalDir" ""
cp "${config_nix}" "$evalDir/"
cat >"$evalDir/flake.nix" <<EOF2
{
  outputs = { self }: let inherit (import ./config.nix) mkDerivation; in rec {
    drv = builtins.trace "eval-drv" (mkDerivation {
      name = "eval-cache-eval";
      buildCommand = "echo \$(cat \${./data}) > \\\$out";
    });
    str = builtins.trace "eval-str" "hello";
    slash = "/not/a/path";
    path = ./data;
    big = 4294967297;
    yes = true;
    sub = builtins.readFile ./sub/data;
  };
}
EOF2
echo one >"$evalDir/data"
git -C "$evalDir" add config.nix flake.nix data
git -C "$evalDir" commit -m Init

for attr in drv.drvPath str slash path big yes; do
  fresh=$(nix eval --no-eval-cache "$evalDir#$attr")
  [[ $(nix eval "$evalDir#$attr") = "$fresh" ]]
  [[ $(nix eval "$evalDir#$attr") = "$fresh" ]]
  [[ $(nix eval --json "$evalDir#$attr") = $(nix eval --no-eval-cache --json "$evalDir#$attr") ]]
done
NIX_ALLOW_EVAL=0 nix eval "$evalDir#drv.drvPath" 2>&1 | grepQuietInverse 'trace: eval-drv'
NIX_ALLOW_EVAL=0 nix eval "$evalDir#str"
NIX_ALLOW_EVAL=0 nix eval "$evalDir#big"
NIX_ALLOW_EVAL=0 nix eval "$evalDir#yes"
# A string without context may be a cached path, so it is evaluated
expect 1 env NIX_ALLOW_EVAL=0 nix eval "$evalDir#slash"
expect 1 env NIX_ALLOW_EVAL=0 nix eval "$evalDir#path"

# A cached drvPath whose derivation was deleted is evaluated again
drvPath=$(nix eval --raw "$evalDir#drv.drvPath")
nix-store --delete "$drvPath"
[[ ! -e "$drvPath" ]]
[[ $(nix eval --raw "$evalDir#drv.drvPath") = "$drvPath" ]]
[[ -e "$drvPath" ]]
NIX_ALLOW_EVAL=0 nix eval --read-only "$evalDir#str"

# Editing a file in a dirty tree is a miss, also when the size stays
echo two >"$evalDir/data"
expect 1 env NIX_ALLOW_EVAL=0 nix eval "$evalDir#drv.drvPath"
drvTwo=$(nix eval --raw "$evalDir#drv.drvPath")
[[ $drvTwo != "$drvPath" ]]
[[ $(NIX_ALLOW_EVAL=0 nix eval --raw "$evalDir#drv.drvPath") = "$drvTwo" ]]
echo six >"$evalDir/data"
expect 1 env NIX_ALLOW_EVAL=0 nix eval "$evalDir#drv.drvPath"
[[ $(nix eval --raw "$evalDir#drv.drvPath") = $(nix eval --no-eval-cache --raw "$evalDir#drv.drvPath") ]]

