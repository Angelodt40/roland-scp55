#!/bin/sh
# Called by the package postinst before dkms replaces the module.
#
# A loaded module cannot be swapped, and removing it is not enough either: the
# registered ALSA card holds a reference, so the pcmcia device has to be
# unbound first. Without this the postinst aborts with set -e and the package
# stays half-configured, which is the normal case when upgrading.

set -e

if [ -d /sys/bus/pcmcia/drivers/scp55 ]; then
    for d in /sys/bus/pcmcia/drivers/scp55/*; do
        [ -e "$d" ] || continue
        case "$d" in
            */bind|*/unbind|*/uevent|*/module|*/new_id|*/remove_id) continue ;;
        esac
        base=${d##*/}
        echo -n "$base" > /sys/bus/pcmcia/drivers/scp55/unbind 2>/dev/null || true
    done
fi

modprobe -r scp55 2>/dev/null || true
exit 0
