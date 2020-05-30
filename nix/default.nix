{ system ? builtins.currentSystem }:
let
  pkgs = (import ./nixpkgs.nix {
    overlays = [ (final: prev: {
      pcre = prev.pcre.overrideAttrs (x: {
        configureFlags = x.configureFlags ++ [ "--enable-static" ];
      });
    })];
    config = {
      packageOverrides = pkg: {
        glib = pkg.glib.overrideAttrs(x: {
          outputs = [ "bin" "out" "dev" ];
          mesonFlags = [
            "-Ddefault_library=static"
            "-Ddevbindir=${placeholder ''dev''}/bin"
            "-Dgtk_doc=false"
            "-Dnls=disabled"
          ];
        });
        systemd = pkg.systemd.overrideAttrs(x: {
          mesonFlags = x.mesonFlags ++ [ "-Dstatic-libsystemd=true" ];
          postFixup = ''
            ${x.postFixup}
            sed -ri "s;$out/(.*);$nukedRef/\1;g" $lib/lib/libsystemd.a
          '';
        });
      };
    };
  });

  self = with pkgs; stdenv.mkDerivation rec {
    name = "conmon";
    src = ./..;
    doCheck = false;
    enableParallelBuilding = true;
    nativeBuildInputs = [ git pkg-config ];
    buildInputs = [ glib glibc glibc.static pcre systemd ];
    prePatch = ''
      export LDFLAGS='-static-libgcc -static -s -w'
    '';
    installPhase = ''
      install -Dm755 bin/conmon $out/bin/conmon
    '';
  };
in self
