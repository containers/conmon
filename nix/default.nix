{ system ? builtins.currentSystem }:
let
  pkgs = (import ./nixpkgs.nix {
    overlays = [(final: pkg: {
      pcre = (static pkg.pcre).overrideAttrs(x: {
        configureFlags = x.configureFlags ++ [
          "--enable-static"
        ];
      });
    })];
    config = {
      packageOverrides = pkg: {
        autogen = (static pkg.autogen);
        glib = (static pkg.glib).overrideAttrs(x: {
          outputs = [ "bin" "out" "dev" ];
          mesonFlags = [
            "-Ddefault_library=static"
            "-Ddevbindir=${placeholder ''dev''}/bin"
            "-Dgtk_doc=false"
            "-Dnls=disabled"
          ];
        });
        gnutls = (static pkg.gnutls).overrideAttrs(x: {
          configureFlags = (x.configureFlags or []) ++ [
            "--disable-non-suiteb-curves"
            "--disable-openssl-compatibility"
            "--disable-rpath"
            "--enable-local-libopts"
            "--without-p11-kit"
          ];
        });
        systemd = (static pkg.systemd).overrideAttrs(x: {
          mesonFlags = x.mesonFlags ++ [
            "-Dstatic-libsystemd=true"
          ];
          postFixup = ''
            ${x.postFixup}
            sed -ri "s;$out/(.*);$nukedRef/\1;g" $lib/lib/libsystemd.a
          '';
        });
        e2fsprogs = (static pkg.e2fsprogs).overrideAttrs(x: {
          postPatch = x.postPatch + ''
            rm -rf tests/d_fallocate*
          '';
        });
      };
    };
  });

  static = pkg: pkg.overrideAttrs(x: {
    doCheck = false;
    configureFlags = (x.configureFlags or []) ++ [
      "--without-shared"
      "--disable-shared"
    ];
    dontDisableStatic = true;
    enableSharedExecutables = false;
    enableStatic = true;
  });

  self = with pkgs; stdenv.mkDerivation rec {
    name = "conmon";
    src = ./..;
    vendorSha256 = null;
    doCheck = false;
    enableParallelBuilding = true;
    outputs = [ "out" ];
    nativeBuildInputs = [ bash git pcre pkg-config which ];
    buildInputs = [ glibc glibc.static glib ];
    prePatch = ''
      export CFLAGS='-static'
      export LDFLAGS='-s -w -static-libgcc -static'
      export EXTRA_LDFLAGS='-s -w -linkmode external -extldflags "-static -lm"'
    '';
    buildPhase = ''
      patchShebangs .
      make
    '';
    installPhase = ''
      install -Dm755 bin/conmon $out/bin/conmon
    '';
  };
in self
