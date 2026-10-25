# nix eval --raw --impure --expr 'import ./nixos.nix { nixpkgs = "<path>"; }': a
# minimal NixOS system, which imports a few thousand modules. Pass nixpkgs as
# a string: a path would be copied to the store by the interpolation below,
# and hashing all of nixpkgs takes longer than the rest of the eval
{ nixpkgs }:
(import "${nixpkgs}/nixos/lib/eval-config.nix" {
  system = "x86_64-linux";
  modules = [
    {
      fileSystems."/" = {
        device = "/dev/sda1";
        fsType = "ext4";
      };
      boot.loader.grub.enable = false;
      system.stateVersion = "25.05";
    }
  ];
}).config.system.build.toplevel.drvPath
