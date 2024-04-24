{ stdenv
, pkgs
}:
with pkgs; stdenv.mkDerivation rec {
  name = "conmon";
  src = ./..;
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
  ];
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
}
