#!/bin/sh
# midi-thru.sh - the "application" the SCP-55 needs to hear its own MIDI IN
#
# The MCB-3 manual states it plainly: the SCP-55 has no MIDI THRU, so a keyboard
# on its MIDI IN will not make the synth sound unless an application routes the
# input back to the output. This is that application, done by the ALSA sequencer:
# the card's own input port becomes the source, its own output the destination.
#
#   ./midi-thru.sh            connect (idempotent)
#   ./midi-thru.sh off        disconnect
#   ./midi-thru.sh status     show what aconnect sees
set -u

find_client() {
	aconnect -l 2>/dev/null | awk '/Roland SCP-55/{c=$2; sub(":","",c); print c; exit}'
}

C=$(find_client)
[ -z "$C" ] && { echo 'SCP-55 client not found - is the module loaded?'; exit 1; }

case "${1:-on}" in
off)
	aconnect -d "$C:0" "$C:0" 2>/dev/null && echo "disconnected $C:0 from itself" || echo 'nothing to disconnect'
	;;
status)
	aconnect -l | sed -n "/Roland SCP-55/,+2p"
	cat /proc/asound/card*/midi0 2>/dev/null | sed -n '1,8p'
	;;
*)
	if aconnect "$C:0" "$C:0" 2>/dev/null; then
		echo "connected $C:0 -> $C:0 (software MIDI thru active)"
	else
		echo 'already connected, or refused'; aconnect -l | sed -n "/Roland SCP-55/,+2p"
	fi
	;;
esac
