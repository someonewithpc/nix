# nix eval --raw --impure --expr "import ./nixos.nix { nixpkgs = <path>; }": a minimal
# NixOS system, which imports a few thousand modules
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
