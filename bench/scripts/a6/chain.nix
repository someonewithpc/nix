{ n ? "0", len ? 13 }:
let
  bb = "${builtins.storePath /nix/store/n8dj8jab4w99bi3yh19mg8xzdgqgvw1f-busybox-1.37.0}/bin/busybox";
  mk = i: prev: derivation {
    name = "a6-chain-${n}-${toString i}"; system = "x86_64-linux"; builder = bb;
    args = [ "sh" "-c" "echo ${n} ${prev} > $out" ];
    preferLocalBuild = true; allowSubstitutes = false;
  };
  go = i: prev: if i == len then prev else go (i + 1) (mk i prev);
in go 0 ""
