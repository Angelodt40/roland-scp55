# Hardware notes — Roland SCP-55

Everything here was established by testing on one card and one machine. If you
have different results, please open an issue: this document is the most useful
part of the repository for the next person.

## Identity

- Type: 16-bit PCMCIA (PC Card), not CardBus.
- Function: GS/GM synthesiser with an MPU-401 compatible MIDI interface.
- Output path: the synth does not drive a DAC with its own line out; audio
  goes through the host's CS4231A codec, which starts muted.

## What the driver programs

- The PCMCIA socket registers, so that the MPU-401 UART answers at I/O base
  `0x330`. The exact attribute-memory and configuration-register sequence is
  in `src/scp55.c`, next to the comments that explain each step.
- The CS4231A codec, to un-mute the two channels the synth feeds. The register
  indices and the values that worked are reachable at runtime through
  `/proc/scp55mix`, so they can be changed without rebuilding.

## Procfs interfaces

- `/proc/scp55` — one-line status; writing `1` sends a short test phrase.
- `/proc/scp55mix` — `w <hex idx> <hex val>` write a codec register,
  `r <hex idx>` read it, `i` apply the unmute defaults, `u` mute both
  channels.

## Known limitations / open questions

- No module alias for autoload: the module is loaded by hand (`modprobe scp55`)
  or from a startup script. Detecting the card through its PCMCIA CIS and
  emitting a `MODULE_ALIAS` would remove that step.
- Suspend/resume is untested. The socket programming is done at init only.
- The sequencer port name is fixed; if you have two cards, they will look
  identical.
- Only the MPU-401 UART mode is implemented. The card's "intelligent" mode is
  not documented publicly and is not used.
- Tested with kernel 6.1; the code uses plain `inb`/`outb` and the ALSA
  rawmidi API, so it should port to nearby versions, but this is unverified.

## Notes

The card is silent, not broken, when the codec is muted. Several hours were
spent looking for a fault that did not exist: if you hear nothing, check
`/proc/scp55` first, then `/proc/scp55mix`.
