#!/bin/sh
# scp55-vendor-probe.sh - exercise the SCP-55 control interface and the MPU
# by hand, one step at a time, printing what the card answers after each one.
#
# Written 2026-10-28 after the finding that the MPU only acknowledges 0x3F
# once the control interface has been programmed (selector 2, value 1, wait
# for status zero). Without that handshake 0x3F is never acknowledged, the
# driver falls back to 0xFF and the synth stays deaf until a power cycle.
# The same sequence did not work twice in a row, so this tool exists to find
# out exactly which step decides it.
#
# Needs root (it writes to /dev/port). Usage:
#     sudo ./scp55-vendor-probe.sh [base]        # base defaults to 0x330
#
# Nothing here is destructive in a way a power cycle cannot undo, but read the
# output before trusting it: the card's mode registers are write-only in
# practice, and writing the wrong selector leaves it silent.
set -u

BASE=${1:-0x330}
BASE=$((BASE))

w() { printf "\\$(printf '%03o' "$2")" | dd of=/dev/port bs=1 seek=$(($1)) conv=notrunc 2>/dev/null; }
r() { dd if=/dev/port bs=1 skip=$(($1)) count=1 2>/dev/null | od -An -tu1 | tr -d ' '; }
show() { printf '  %-22s %s\n' "$1" "$(r $2)"; }

echo "base = $(printf '0x%03x' "$BASE")"
echo
echo '1. what the card answers before we touch anything'
show 'base+4 (select)' $((BASE + 4))
show 'base+5 (value/signature)' $((BASE + 5))
show 'base+1 (MPU status)' $((BASE + 1))
echo
echo '2. control handshake: selector 2, value 1'
w $((BASE + 4)) 0x02
w $((BASE + 5)) 0x01
i=0
while [ $i -lt 200 ]; do
  w $((BASE + 4)) 0x02
  v=$(r $((BASE + 5)))
  [ "$v" = "0" ] && break
  i=$((i + 1))
done
echo "   status went to zero after $i loops (last value $v)"
echo '3. base+10 <- 0 (the vendor zeroes 0x33A here)'
w $((BASE + 10)) 0x00
echo
echo '3. enter UART mode: base+1 <- 0x3F, then read base+0'
w $((BASE + 1)) 0x3f
sleep 1
show 'base+1 (status)' $((BASE + 1))
show 'base+0 (data)' $((BASE + 0))
echo '   ^ 254 = 0xFE, the UART acknowledgement. Anything else: try step 3 again.'
echo
echo '4. reset the MPU after the ack: base+1 <- 0xFF'
w $((BASE + 1)) 0xff
sleep 1
show 'base+1 (status)' $((BASE + 1))
show 'base+0 (data)' $((BASE + 0))
echo
echo '5. back to UART mode: base+1 <- 0x3F, wait for 0xFE'
w $((BASE + 1)) 0x3f
sleep 1
show 'base+0 (data)' $((BASE + 0))
echo
echo '6. select the audio routing mode: base+4 <- 3, base+5 <- 1'
w $((BASE + 4)) 0x03
w $((BASE + 5)) 0x01
show 'base+5' $((BASE + 5))
echo
echo 'Done. Play something on the sequencer port to see whether it reaches the DAC.'
