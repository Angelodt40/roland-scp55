# scp55 — Linux driver for the Roland SCP-55 "Sound Canvas" PC Card

![build](https://github.com/Angelodt40/roland-scp55/actions/workflows/build.yml/badge.svg)

Copyright (C) 2026 Angelo Di Tomasso <angelo.dt40@gmail.com>

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

## Install

Three ways, from laziest to most explicit.

**From a release package** (Debian, Ubuntu, MX and derivatives): download the
`.deb` from the [releases page](https://github.com/Angelodt40/roland-scp55/releases)
and install it. It builds itself through DKMS, so kernel updates are handled:

    sudo apt install ./scp55-dkms_0.1.0-1_all.deb

**One command, from the repository** (needs the kernel headers):

    git clone https://github.com/Angelodt40/roland-scp55.git
    cd roland-scp55
    sudo ./install.sh

The script uses DKMS when it is available and falls back to a direct module
install; it also installs the udev rule and sends the test phrase, so you
know immediately whether the card answers. There is no manual loading step:
with the module installed, the card is picked up when it is present, including
after a reboot.

## Install with DKMS (recommended for kernels that update)

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

## Using it with DOSBox

DOSBox (and DOSBox-X, DOSBox Staging) sends its MIDI output to any ALSA
sequencer port, so MS-DOS games with General MIDI music can play through the
card while the game runs on Linux:

    [midi]
    mididevice=alsa
    midiconfig=24:0      # the client:port printed by aplaymidi -l

Nothing else to configure: DOSBox connects to the same sequencer port the
driver exposes.

Set your expectations on one point: the SCP-55 is a General MIDI / GS device.
Games written for the Roland MT-32 will play, but with the wrong instruments —
that is a different synthesiser, not a configuration error. For those, use an
MT-32 emulator (Munt).

If instead you want to boot real MS-DOS on the machine and drive the card
natively, the card needs a DOS-side enabler: see Related work below.

## Status of testing

Verified on one machine only (details above). Not verified, and worth knowing
before you spend an evening on it:

- suspend/resume — the socket is programmed at load time and not restored
- kernels other than 6.x — the code uses plain inb/outb and the ALSA rawmidi
  API, so it should port, but nobody has tried
- automatic loading — **verified**: with the module installed through DKMS and
  `depmod` run, a reboot with the card inserted loads `scp55` and creates the
  sequencer port without any manual `modprobe` (Panasonic CF-30, MX-23.6,
  kernel 6.1.0-54-686-pae). The module advertises a PCMCIA modalias
  (`pcmcia:mC00Cc0001...`), which is what makes this work
- two cards in one machine — the sequencer port name is fixed

Reports from other hardware are the most useful thing you can contribute.

## Versioning

Version numbers follow `MAJOR.MINOR.PATCH`. The same string appears in three
places and they must match:

- `dkms.conf` → `PACKAGE_VERSION`
- `src/scp55.c` → `MODULE_VERSION(...)`, visible with `modinfo -F version`
- the git tag, `vX.Y.Z`

Releasing means: bump those three, add an entry to `CHANGELOG.md`, commit, then
`git tag -a vX.Y.Z -m "scp55 X.Y.Z"` and `git push --tags`.

## Kernel compatibility

Compiled in CI on every push — build only, no hardware involved:

| Kernel | Distribution | Result |
| --- | --- | --- |
| 6.1 | Debian bookworm | builds |
| 6.12 | Debian trixie | builds |
| latest | Fedora | builds |

Runtime-tested on 6.1.0-54-686-pae only (Panasonic CF-30). The badge at the top
reports the current CI state.

## Related work and thanks

- **zikolas/scp55-enabler** — a DOS enabler for the same card, C and assembly,
  with three host backends (82365, Card Services, OmniBook ROM Socket
  Services). Written independently, and it reaches the same conclusions about
  the hardware: the card is 8-bit, the MPU status register sits at 0x331, the
  codec data port at 0x339, and the codec must be un-muted before the synth is
  audible. Those facts are stated here with confidence because their write-up
  confirms them.

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
