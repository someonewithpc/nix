# b.nix is parsed first and interns its names first, however soon a.nix is
# read, so deepSeq walks a in b's order
let
  a = import ./symbol-order/a.nix;
  b = import ./symbol-order/b.nix;
in
builtins.seq b (builtins.deepSeq a (builtins.length (builtins.attrNames a)))
