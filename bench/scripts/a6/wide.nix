{ n ? "0", width ? 800 }:
let
  bb = "${builtins.storePath /nix/store/n8dj8jab4w99bi3yh19mg8xzdgqgvw1f-busybox-1.37.0}/bin/busybox";
  leaf = i: derivation {
    name = "a6-leaf-${toString i}"; system = "x86_64-linux"; builder = bb;
    args = [ "sh" "-c" "echo ${toString i} > $out" ];
    preferLocalBuild = true; allowSubstitutes = false;
  };
  leaves = builtins.genList leaf width;
in {
  leaves = derivation {
    name = "a6-leaves-${toString width}"; system = "x86_64-linux"; builder = bb;
    args = [ "sh" "-c" "echo > $out" ]; deps = leaves;
    preferLocalBuild = true; allowSubstitutes = false;
  };
  top = derivation {
    name = "a6-top-${n}"; system = "x86_64-linux"; builder = bb;
    args = [ "sh" "-c" "echo ${n} > $out" ]; deps = leaves;
    preferLocalBuild = true; allowSubstitutes = false;
  };
}
