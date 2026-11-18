{ n ? "0", width ? 1 }:
let
  bb = "${builtins.storePath /nix/store/n8dj8jab4w99bi3yh19mg8xzdgqgvw1f-busybox-1.37.0}/bin/busybox";
  mk = i: derivation {
    name = "a6-subst-${n}-${toString i}"; system = "x86_64-linux"; builder = bb;
    args = [ "sh" "-c" "echo ${n} ${toString i} > $out" ];
  };
in derivation {
  name = "a6-subst-top-${n}"; system = "x86_64-linux"; builder = bb;
  args = [ "sh" "-c" "echo ${n} > $out" ]; deps = builtins.genList mk width;
}
