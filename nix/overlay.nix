self: super:
{
  libseccomp = super.libseccomp.overrideAttrs (x: {
    doCheck = false;
    dontDisableStatic = true;
  });
  pcre2 = super.pcre2.overrideAttrs (x: {
    configureFlags = x.configureFlags ++ [ "--enable-static" ];
  });
  glib = super.glib.overrideAttrs (x: {
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
}
