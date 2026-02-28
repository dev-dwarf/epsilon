
{
  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-unstable";
    lf.url = "github:dev-dwarf/lf?ref=main";
  };

  outputs = { self, nixpkgs, lf, ... }: let
    system = "x86_64-linux";
    pkgs = import nixpkgs {
      inherit system;
      overlays = [lf.overlay];
    };
  in rec {
    packages.${system} = pkgs // rec {
      test = pkgs.mkSTU rec {
        name = "test";
        src = ./.;
      };
    };
  };
}
