{ pkgs }:
with pkgs; stdenv.mkDerivation rec {
  name = "conmon";
  src = ./..;
  vendorSha256 = null;
  doCheck = false;
  enableParallelBuilding = true;
  outputs = [ "out" ];
  nativeBuildInputs = with buildPackages; [
    bash
    gitMinimal
    pkg-config
    which
  ];
  buildInputs = [
    glib
    glibc
    glibc.static
    libseccomp
    pcre
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
