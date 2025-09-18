{ stdenv
, pkgs
, enableSystemd ? false
}:
with pkgs; stdenv.mkDerivation rec {
  name = "conmon";
  # Use Pure to avoid exuding the .git directory
  src = nix-gitignore.gitignoreSourcePure [ ../.gitignore ] ./..;
  vendorHash = null;
  doCheck = false;
  enableParallelBuilding = true;
  outputs = [ "out" ];
  nativeBuildInputs = with buildPackages; [
    gitMinimal
    pkg-config
  ];
  buildInputs = lib.optionals (!stdenv.hostPlatform.isMusl) [
    glibc.static
  ] ++ [
    pkgsStatic.glib
    libseccomp
    json_c
  ] ++ lib.optionals enableSystemd [
    # Only include systemd for dynamic builds, not static builds
    # Static builds will use PKG_CONFIG_PATH approach instead
  ];
  prePatch = ''
    export CFLAGS='-static -pthread'
    export LDFLAGS='-s -w -static-libgcc -static'
    export EXTRA_LDFLAGS='-s -w -linkmode external -extldflags "-static -lm"'
    ${lib.optionalString (!enableSystemd) "export DISABLE_SYSTEMD=1"}
  '';
  buildPhase = ''
    patchShebangs .
    make
  '';
  installPhase = ''
    install -Dm755 bin/conmon $out/bin/conmon
  '';
}
