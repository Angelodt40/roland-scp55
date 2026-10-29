#!/bin/sh
# usage: unbind-loaded.sh down|up
#
# "down" is called by the package postinst before dkms replaces the module:
# a loaded module cannot be swapped, and removing it is not enough on its own
# because the registered ALSA card holds a reference. So every device of this
# driver is unbound first, then the module is removed - with a few retries,
# because snd_card_free() drops the reference a moment after the unbind.
#
# "up" is called after modprobe: after an unbind the pcmcia bus does not
# re-attach the device by itself when the driver is loaded again, so the card
# would stay dead until a reboot.

set -e
action=${1:-down}
drv=/sys/bus/pcmcia/drivers/scp55

if [ "$action" = "down" ]; then
    if [ -d "$drv" ]; then
        for d in "$drv"/*; do
            [ -e "$d" ] || continue
            case "$d" in
                */bind|*/unbind|*/uevent|*/module|*/new_id|*/remove_id) continue ;;
            esac
            base=${d##*/}
            echo -n "$base" > "$drv/unbind" 2>/dev/null || true
        done
    fi
    i=0
    while [ -d /sys/module/scp55 ] && [ $i -lt 20 ]; do
        modprobe -r scp55 2>/dev/null || true
        [ -d /sys/module/scp55 ] || break
        sleep 0.5
        i=$((i + 1))
    done
else
    if [ -d "$drv" ]; then
        for d in /sys/bus/pcmcia/devices/*; do
            [ -e "$d" ] || continue
            base=${d##*/}
            [ -e "$drv/$base" ] && continue
            echo -n "$base" > "$drv/bind" 2>/dev/null || true
        done
    fi
fi
exit 0
