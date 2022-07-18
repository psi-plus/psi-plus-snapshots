# Nix configuration for Psi+ to make test builds in CI

let
  nixpkgsPin = fetchTarball {
    # Branch: release-22.05
    # Date: 2022-07-18
    url = "https://github.com/NixOS/nixpkgs/archive/0c46150c9cfd66a4738bf62fc08b137e8a0015f7.tar.gz";
    sha256 = "sha256:18wjd2mhbs4ihyva27gzc10rkl9qaf3zn6f46bggi0z29lasnq4m";
  };
in

{ pkgs ? import nixpkgsPin {}
, src ? pkgs.nix-gitignore.gitignoreRecursiveSource [ ../.gitignore ] ../.
, version ? "git-unknown"
, chatType ? "basic" # "webkit" "webengine"
}:

let
  psi-plus =
    (pkgs.psi-plus.override {
      enablePsiMedia = true;
      inherit chatType;
    }).overrideAttrs (srcAttrs: {
      inherit src;
      version = assert builtins.isString version; version;
      buildInputs = srcAttrs.buildInputs ++ [ pkgs.libsForQt5.qtimageformats ];
    });
in

psi-plus
