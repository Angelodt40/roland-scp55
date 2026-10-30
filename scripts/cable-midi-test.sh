#!/bin/sh
# scp55-cable-test.sh - test a home-made MIDI cable on the Roland SCP-55
#
# What it does: plays a tone on the card (so you know it is alive), then sends
# a few notes while dumping the card's rawmidi input. Anything that comes back
# was echoed by the card itself, which only happens if the loop through your
# cable works and the card forwards incoming MIDI.
#
#   ./cable-midi-test.sh      loopback: card MIDI OUT -> card MIDI IN
#
# Run as root, on the machine with the card in the socket.

set -u

RAW=/dev/snd/$(ls /dev/snd 2>/dev/null | grep -m1 '^midiC')
PORT=$(amidi -l 2>/dev/null | awk '/^IO|^I /{print $2; exit}')
OUT=/tmp/scp55-cable-in.hex

echo '== card =='
if ! lsmod | grep -q '^scp55'; then
	echo 'scp55 module not loaded - load it first, then run this again.'
	exit 1
fi
modinfo -F version scp55 2>/dev/null | sed 's/^/module version: /'
[ -n "$RAW" ] && echo "rawmidi device: $RAW" || { echo 'no rawmidi device found'; exit 1; }
[ -n "$PORT" ] && echo "amidi port:     $PORT" || { echo 'no amidi port found'; exit 1; }

# closing the port would send all-notes-off in the middle of the test
[ -w /sys/module/scp55/parameters/reset_on_close ] && echo 0 > /sys/module/scp55/parameters/reset_on_close

echo
echo '== is the card alive? =='
if [ -w /proc/scp55probe ]; then
	echo 'tone' > /proc/scp55probe
	echo 'A tone should have played. If you heard nothing, stop here: the problem'
	echo 'is not the cable, the card is mute and needs a real power cycle.'
else
	echo '/proc/scp55probe missing: skip the tone, trust only what you hear next.'
fi
printf 'Continue with the loopback test? [y/N] '
read -r ans
case "$ans" in y|Y) ;; *) exit 0 ;; esac

echo
echo '== loopback: sending notes, listening on the card own input =='
: > "$OUT"
amidi -p "$PORT" -d > "$OUT" 2>/dev/null &
DUMP=$!
sleep 0.3

# C4 E4 G4 then off - channel 1
printf '\220\100\144\220\104\144\220\107\144\200\100\000\200\104\000\200\107\000' > "$RAW"
sleep 1.2

kill "$DUMP" 2>/dev/null
wait "$DUMP" 2>/dev/null

BYTES=$(grep -c . "$OUT" 2>/dev/null)
BYTES=${BYTES:-0}

echo
if [ "$BYTES" -gt 0 ]; then
	echo "RESULT: $BYTES rows came back - the loop works."
	echo 'The card OUT carried host data and its IN fed it back: cable, pinout'
	echo 'and MIDI forwarding are all confirmed, in both directions.'
	echo 'First bytes:'
	head -3 "$OUT" | sed 's/^/  /'
else
	echo 'RESULT: nothing came back (0 bytes).'
	echo 'This is not proof the cable is wrong. It means at least one of these:'
	echo '  - the card does not echo host data out its MIDI OUT socket;'
	echo '  - the card MIDI OUT lacks the 220 ohm current source (a loopback'
	echo '    needs the source on the transmitting side);'
	echo '  - the wiring or the polarity is wrong;'
	echo '  - the card MIDI IN is dead.'
	echo
	echo 'Next test, and the one that matters: keyboard MIDI OUT into the card'
	echo 'MIDI IN, channel 1, and see if the SCP-55 makes a sound.'
fi
