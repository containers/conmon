{ system ? builtins.currentSystem }:
let
  pkgs = (import ./nixpkgs.nix {
    overlays = [ (final: prev: {
      pcre = prev.pcre.overrideAttrs (x: {
        configureFlags = x.configureFlags ++ [ "--enable-static" ];
      });
      p11-kit = prev.p11-kit.overrideAttrs(x: { doCheck = false; });
      e2fsprogs = prev.e2fsprogs.overrideAttrs(x: { doCheck = false; });
      gnutls = prev.gnutls.overrideAttrs(x: { doCheck = false; });
    })];
    config = {
      packageOverrides = pkg: {
        systemd = pkg.systemd.overrideAttrs(x: {
          mesonFlags = x.mesonFlags ++ [ "-Dstatic-libsystemd=true" ];
          postFixup = ''
            ${x.postFixup}
            sed -ri "s;$out/(.*);$nukedRef/\1;g" $lib/lib/libsystemd.a
          '';
        });
        glib = pkg.glib.overrideAttrs(x: {
          outputs = [ "bin" "out" "dev" ];
          mesonFlags = [
            "-Ddefault_library=static"
            "-Ddevbindir=${placeholder ''dev''}/bin"
            "-Dgtk_doc=false"
            "-Dnls=disabled"
          ];
          doCheck = false;
        });
      };
    };
  });

  self = with pkgs; {
    conmon-static = conmon.overrideAttrs(x: {
      name = "conmon-static";
      src = ./..;
      buildInputs = x.buildInputs ++ [ pcre ];
      LDFLAGS="-static-libgcc -static";
    });
  };
in self
