{ system ? builtins.currentSystem }:
let
  static = import ./static.nix;
  pkgs = (import ./nixpkgs.nix {
    overlays = [
      (final: pkg: {
        pcre = (static pkg.pcre).overrideAttrs (x: {
          configureFlags = x.configureFlags ++ [ "--enable-static" ];
        });
      })
    ];
    config = {
      packageOverrides = pkg: {
        libseccomp = (static pkg.libseccomp);
        glib = (static pkg.glib).overrideAttrs (x: {
          outputs = [ "bin" "out" "dev" ];
          mesonFlags = [
            "-Ddefault_library=static"
            "-Ddevbindir=${placeholder ''dev''}/bin"
            "-Dgtk_doc=false"
            "-Dnls=disabled"
          ];
        });
      };
    };
  });
  self = import ./derivation.nix { inherit pkgs; };
in
self
