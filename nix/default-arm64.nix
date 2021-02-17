let
  pkgs = (import ./nixpkgs.nix {
    crossSystem = {
      config = "aarch64-unknown-linux-gnu";
    };
    overlays = [
      (final: pkg: {
        pcre = (static pkg.pcre).overrideAttrs (x: {
          configureFlags = x.configureFlags ++ [
            "--enable-static"
          ];
        });
      })
    ];
    config = {
      packageOverrides = pkg: {
        autogen = (static pkg.autogen);
        e2fsprogs = (static pkg.e2fsprogs);
        libuv = (static pkg.libuv);
        glib = (static pkg.glib).overrideAttrs (x: {
          outputs = [ "bin" "out" "dev" ];
          mesonFlags = [
            "-Ddefault_library=static"
            "-Ddevbindir=${placeholder ''dev''}/bin"
            "-Dgtk_doc=false"
            "-Dnls=disabled"
          ];
          postInstall = ''
            moveToOutput "share/glib-2.0" "$dev"
            substituteInPlace "$dev/bin/gdbus-codegen" --replace "$out" "$dev"
            sed -i "$dev/bin/glib-gettextize" -e "s|^gettext_dir=.*|gettext_dir=$dev/share/glib-2.0/gettext|"
            sed '1i#line 1 "${x.pname}-${x.version}/include/glib-2.0/gobject/gobjectnotifyqueue.c"' \
              -i "$dev"/include/glib-2.0/gobject/gobjectnotifyqueue.c
          '';
        });
        gnutls = (static pkg.gnutls).overrideAttrs (x: {
          configureFlags = (x.configureFlags or [ ]) ++ [
            "--disable-non-suiteb-curves"
            "--disable-openssl-compatibility"
            "--disable-rpath"
            "--enable-local-libopts"
            "--without-p11-kit"
          ];
        });
        systemd = (static pkg.systemd).overrideAttrs (x: {
          outputs = [ "out" "dev" ];
          mesonFlags = x.mesonFlags ++ [
            "-Dstatic-libsystemd=true"
          ];
        });
      };
    };
  });

  static = pkg: pkg.overrideAttrs (x: {
    doCheck = false;
    configureFlags = (x.configureFlags or [ ]) ++ [
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
    nativeBuildInputs = with buildPackages; [
      bash
      gitMinimal
      pcre
      pkg-config
      which
    ];
    buildInputs = [ glibc glibc.static glib ];
    prePatch = ''
      export CFLAGS='-static -pthread'
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
in
self
