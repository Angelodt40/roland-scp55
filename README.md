# scp55 — Linux driver for the Roland SCP-55 "Sound Canvas" PC Card

Revive a 1995 Roland SCP-55 PCMCIA sound module on a modern Linux kernel.
The module programs the card's socket, un-mutes the codec that sits between
the synth and the line output, and exposes the MPU-401 UART as an ALSA
rawmidi device — so the card shows up as a normal external MIDI synth and can
be driven by anything that speaks ALSA sequencer: `aplaymidi`, Rosegarden,
Qtractor, vmpk, a tracker, a script.

No firmware, no proprietary blobs, no vendor daemon. One small kernel module.

**Status:** working. Tested on a Panasonic CF-30 (Toughbook) running MX-23.6
Fluxbox i386, kernel `6.1.0-54-686-pae`. This is a single-hardware report —
see [docs/hardware.md](docs/hardware.md) for what is known and what is not.

## What the card is

The SCP-55 is a PC Card (16-bit PCMCIA) version of the Roland Sound Canvas
line: a GS/GM synthesiser with an MPU-401 compatible MIDI interface, sold
around 1995. The audio leaves the card through the machine's codec, and the
synth section stays muted until that codec is enabled — which is why a card
that "does nothing" is usually not broken, just silent.

## How it works

1. On load, the driver locates the card's PCMCIA function and programs the
   socket registers so the MPU-401 UART is reachable at I/O base `0x330`.
2. It un-mutes the CS4231A codec, which the synth's output runs through.
   Without this step the card accepts MIDI and produces nothing audible.
3. It registers an ALSA rawmidi device; the sequencer port appears as
   `Roland SCP-55` in `aplaymidi -l`.
4. Two procfs interfaces give you control without extra tools:
   - `/proc/scp55` — status; `echo 1 > /proc/scp55` sends a test phrase
   - `/proc/scp55mix` — live codec mixer: `w <hex idx> <hex val>`,
     `r <hex idx>`, `i` (unmute defaults), `u` (mute both channels)

## Requirements

- A laptop with a 16-bit PCMCIA slot (CardBus-only slots are not enough) and
  the card itself.
- Kernel headers for the running kernel, `gcc`, `make`.
- `alsa-utils` for `aplaymidi`, and any MIDI player you like.

## Install with DKMS (recommended)

DKMS rebuilds the module automatically after a kernel update.

```sh
sudo cp -r . /usr/src/scp55-0.1.0
sudo dkms add -m scp55 -v 0.1.0
sudo dkms build -m scp55 -v 0.1.0
sudo dkms install -m scp55 -v 0.1.0
sudo depmod -a
```

## Install manually

```sh
make
sudo cp scp55.ko /lib/modules/$(uname -r)/extra/
sudo depmod -a
sudo modprobe scp55
```

Optional: install `udev/99-scp55.rules` so users in the `audio` group can
open the raw MIDI device without root.

## Usage

```sh
aplaymidi -l                      # look for "Roland SCP-55"
aplaymidi -p 24:0 song.mid        # play a file to it
```

Or with the helper script, which finds the port itself:

```sh
./scripts/scp55-play.sh song.mid [more.mid ...]
```

In a DAW (Rosegarden, Qtractor) just select `Roland SCP-55` as the MIDI
output device; the connection is made by ALSA, nothing to configure.

## Troubleshooting

| Symptom | Cause |
| --- | --- |
| No port in `aplaymidi -l` | module not loaded — check `lsmod \| grep scp55` and `dmesg` |
| Port present, silence | codec muted — `echo i > /proc/scp55mix` |
| Notes hang after Ctrl+C | send All Sound Off — `echo 1 > /proc/scp55`, or replay the file |
| Card not detected | slot is CardBus-only, or the PCMCIA bridge needs `yenta_socket` |

## What is in this repository

```
src/scp55.c          the driver
dkms.conf            DKMS build description
udev/99-scp55.rules  permissions for the raw MIDI device
scripts/             helper to play files
docs/hardware.md     what is known about the card, and open questions
experiments/         earlier variants (play/midi/beep) kept for reference
```

The files under `experiments/` are not maintained: they were the steps that
led here, and they are kept because the comments in them document behaviour
that is otherwise undocumented.

## In italiano (breve)

Driver per la scheda Roland SCP-55 (Sound Canvas su PC Card, 1995): programma
il socket, toglie il mute al codec CS4231A e presenta la porta MPU-401 come
dispositivo ALSA rawmidi, quindi la scheda diventa un synth MIDI esterno
normale. Installazione con DKMS, poi `aplaymidi -p <porta> file.mid`.

## License

GPL-2.0 (see `LICENSE`). Not affiliated with, endorsed by, or supported by
Roland Corporation. "Roland" and "Sound Canvas" are trademarks of their
respective owners and are used here only to identify the hardware.
