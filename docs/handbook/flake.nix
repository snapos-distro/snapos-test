# /etc/nixos/flake.nix: where your system comes from.
# nixpkgs gives every program; snapos gives the SnapOS packs.
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    snapos.url = "github:snapos-distro/SnapOS";
    snapos.inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs = { nixpkgs, snapos, ... }: {
    # "mypc" is the name you use with nixos-install and nixos-rebuild
    nixosConfigurations.mypc = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";            # "aarch64-linux" on an ARM64 computer
      modules = [ snapos.nixosModules.packs ./configuration.nix ];
    };
  };
}
