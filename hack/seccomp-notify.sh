#! /usr/bin/env bash
if $(printf '#include <linux/seccomp.h>\nvoid main(){struct seccomp_notif_sizes s;}' | cc -x c - -o /dev/null 2> /dev/null && pkg-config --atleast-version 2.5.0 libseccomp); then
        echo "0"
fi
