# scp55 — Linux driver for the Roland SCP-55 "Sound Canvas" PC Card

[![build](https://github.com/Angelodt40/roland-scp55/actions/workflows/build.yml/badge.svg)](https://github.com/Angelodt40/roland-scp55/actions/workflows/build.yml)

Copyright (C) 2026 Angelo Di Tomasso. GPL-2.0, see [LICENSE](LICENSE).

Revive a 1995 Roland SCP-55 PCMCIA sound module on a modern Linux kernel.
The module programs the card's socket, un-mutes the codec that sits between
the synth and the line output, and exposes the MPU-401 UART as an ALSA
rawmidi device — so the card shows up as a normal external MIDI synth and can
be driven by anything that speaks ALSA sequencer: `aplaymidi`, Rosegarden,
Qtractor, vmpk, a tracker, a script.

No firmware, no proprietary blobs, no vendor daemon. One small kernel module.

**Status:** working as a MIDI synth interface. Tested on a Panasonic CF-30
(Toughbook) running MX-23.6 Fluxbox i386, kernel `6.1.0-54-686-pae`. This is a
single-hardware report — see [docs/hardware.md](docs/hardware.md) for what is
known and what is not.

## Scope, stated plainly

**What this is.** A kernel module that brings the card up as an external MIDI
synth: it programs the socket, un-mutes the codec, selects the routing mode the
vendor driver uses, and exposes the MPU-401 UART as an ALSA **rawmidi** device.
That is all it does, and it is what the releases were tested on.

**What this is not.** The card is a full sound card under Windows: it also plays
PCM, records, mixes and emulates a Sound Blaster. **None of that is implemented
here**, and the project does not claim it: there is no ALSA PCM device, no mixer
and no recording.

**What is being researched.** The card asks for a 1 MB memory window. It is now
reserved and readable ([docs/memory-window.md](docs/memory-window.md)) and it
does not behave like addressed RAM, while the wave code of the vendor driver is
partly mapped ([docs/wave-engine.md](docs/wave-engine.md)). Those documents
record measurements and negative results alike; none of it is a supported
feature, and none of it is on the path the synth uses today. This machine’s
PCMCIA socket exposes no DMA resources at all, which is worth knowing before
anyone expects PCM on it.

Questions, bug reports and results from other machines: please open an
[issue](https://github.com/Angelodt40/roland-scp55/issues). That is the
preferred channel — the maintainer does not publish an email address.

## What works, what does not

Working: the synth plays anything sent to the ALSA sequencer port, including
`aplaymidi` and Rosegarden, and the card is identified by its own signature.

Not working, with reasons in [docs/hardware.md](docs/hardware.md): MIDI **input**
(the card never transmits, and the vendor VxD imports no VPICD service either),
a native ALSA mixer (the codec exposes an indirect register bank we have not
mapped yet), and reliable resume from suspend on the test machine.

## What the card is

The SCP-55 is a PC Card (16-bit PCMCIA) version of the Roland Sound Canvas
line: a GS/GM synthesiser with an MPU-401 compatible MIDI interface, sold
around 1995. The audio leaves the card through the machine's codec, and the
synth section stays muted until that codec is enabled — which is why a card
that "does nothing" is usually not broken, just silent.

## How it works

1. On load, the driver locates the card's PCMCIA function and programs the
   socket registers so the MPU-401 UART is reachable at I/O base `0x330`.
2. It identifies the card: after power-on the card answers `0xAA` at `base+5`,
   and the driver checks that signature before trusting anything else. This is
   the same test the original Roland enabler performs.
3. It brings up the CS4231A codec the synth's audio runs through: input gains,
   DAC attenuation, and the routing **mode the vendor driver itself selects**
   (control registers `base+4` / `base+5` set to 3 / 1). Releases up to 0.2.1
   used an ADC-to-DAC loopback instead: that was a workaround for this routing,
   not the real path.
   silent even when every status register says it is healthy.
4. It registers an ALSA rawmidi device; the sequencer port appears as
   `Roland SCP-55` in `aplaymidi -l`.
5. It deliberately leaves the MPU-401 alone. The card is already in UART mode
   after power-on; re-initialising it on every open sends `0xFF`, which puts an
   MPU-401 into *intelligent* mode, and if the following `0x3F` is lost the card
   silently discards every note. The order the vendor uses, for the case where
   re-initialisation is really needed, is in
   [docs/vendor-init-sequence.md](docs/vendor-init-sequence.md).
6. Two procfs interfaces give you control without extra tools:
   - `/proc/scp55` — status; `echo 1 > /proc/scp55` sends a test phrase
   - `/proc/scp55mix` — live codec mixer: `w <hex idx> <hex val>`,
     `r <hex idx>`, `i` (unmute defaults), `u` (mute both channels)

## Releases and versions

A tag is not a release. Versions are marked **pre-release** until the `.deb`
has been built and the driver has been tried on real hardware — the card is
old enough that a change which compiles cleanly can still wedge the machine
(0.1.7 did exactly that). A build that has not been heard making sound is not
ready. Flip the release to stable only after that, and write down what you
verified.

## Requirements

- A laptop with a 16-bit PCMCIA slot (CardBus-only slots are not enough) and
  the card itself.
- Kernel headers for the running kernel, `gcc`, `make`.
- `alsa-utils` for `aplaymidi`, and any MIDI player you like.

## Install

Three options, from least work to most control.

### 1. From a release package (Debian, Ubuntu, MX and derivatives)

Download the `.deb` from the [releases page](https://github.com/Angelodt40/roland-scp55/releases)
and install it. It builds itself through DKMS, so kernel updates are handled:

    sudo apt install ./scp55-dkms_0.2.0-1_all.deb

### 2. From the repository, one command

    git clone https://github.com/Angelodt40/roland-scp55.git
    cd roland-scp55
    sudo ./install.sh

The script uses DKMS when it is available and falls back to a direct module
install; it also installs the udev rule and sends the test phrase, so you know
immediately whether the card answers.

There is no manual loading step: with the module installed, the card is picked
up when it is present, including after a reboot.

### 3. DKMS by hand

DKMS rebuilds the module automatically after a kernel update. Copy only what
the build needs — not the whole working tree:

    sudo mkdir -p /usr/src/scp55-0.2.0
    sudo cp -r src dkms.conf /usr/src/scp55-0.2.0/
    sudo dkms add -m scp55 -v 0.2.0
    sudo dkms build -m scp55 -v 0.2.0
    sudo dkms install -m scp55 -v 0.2.0 --force
    sudo depmod -a

### Without DKMS

From the repository root:

    make
    sudo make install

Or by hand:

    make -C src
    sudo cp src/scp55.ko /lib/modules/$(uname -r)/extra/
    sudo depmod -a
    sudo modprobe scp55

Optional: install `udev/99-scp55.rules` so users in the `audio` group can open
the raw MIDI device without root.

## Usage

    aplaymidi -l                      # look for "Roland SCP-55"
    aplaymidi -p 20:0 song.mid        # play a file to it

Or with the helper script, which finds the port itself:

    ./scripts/scp55-play.sh song.mid [more.mid ...]

In a DAW (Rosegarden, Qtractor) just select `Roland SCP-55` as the MIDI output
device; the connection is made by ALSA, nothing to configure.

## MIDI input

The device is duplex: a rawmidi input stream is registered next to the output
one, and a polling workqueue drains whatever the card has to send.

**On the tested hardware it never sends anything.** Tested with the driver and
directly against the ports, bypassing the driver entirely: a GM identity
request, a GS RQ1 dump request, a GS reset and ordinary notes all produced zero
bytes back, with the MPU status register staying at `0x00` — no data-ready flag,
ever. The card answers MPU-401 commands (`0xFF` and `0x3F` are acknowledged with
`0xFE`), so the receive path itself is alive; the card simply does not transmit
on this PCMCIA interface.

The input support is kept because it costs little and should work on any other
MPU-401-compatible device behind the same driver. Treat it as unverified, and if
your card does send data, that is worth reporting.

## Troubleshooting

| Symptom | Cause |
| --- | --- |
| No port in `aplaymidi -l` | module not loaded — check `lsmod \| grep scp55` and `dmesg` |
| Port present, silence | codec muted — `echo i > /proc/scp55mix` |
| Notes hang after Ctrl+C | send All Sound Off — `echo 1 > /proc/scp55`, or replay the file |
| Only the attack is heard, then silence | the client closed the port immediately: with `reset_on_close=1` (the default) closing the rawmidi output sends all-notes-off and all-sound-off. Use a player that keeps the port open (`aplaymidi`), or `echo 0 > /sys/module/scp55/parameters/reset_on_close` |
| Card not detected | slot is CardBus-only, or the PCMCIA bridge needs `yenta_socket` |

## Using it with DOSBox

DOSBox (and DOSBox-X, DOSBox Staging) sends its MIDI output to any ALSA
sequencer port, so MS-DOS games with General MIDI music can play through the
card while the game runs on Linux:

    [midi]
    mididevice=alsa
    midiconfig=20:0      ; the client:port printed by aplaymidi -l

Nothing else to configure: DOSBox connects to the same sequencer port the
driver exposes.

Set your expectations on one point: the SCP-55 is a General MIDI / GS device.
Games written for the Roland MT-32 will play, but with the wrong instruments —
that is a different synthesiser, not a configuration error. For those, use an
MT-32 emulator (Munt).

If instead you want to boot real MS-DOS on the machine and drive the card
natively, the card needs a DOS-side enabler: see Related work below.

## Suspend and resume

The driver implements suspend and resume: on suspend it stops the work queues and
sends all-notes-off, and on resume it re-programmes the codec and puts the
MPU-401 back into UART mode. Both log one line in `dmesg`.

A suspend is a power cycle for the card: it loses the codec state *and* the
routing mode (`0x334 <- 3`, `0x335 <- 1`), so resume reads the signature first
— writing the mode destroys it — then selects the mode again and re-runs the
codec bring-up, and finally puts the MPU-401 back into UART mode.

**What the driver recovers, measured on the CF-30 (2026-10-29):**

    scp55: resume: core enable failed (-13), re-configuring
    scp55: resumed, io 0x330, signature 0xaa, codec ok

The pcmcia part now works: when the core refuses to re-enable the window with
`-EACCES`, the driver drops the stale configuration and rebuilds it as probe
does (`pcmcia_disable_device`, `pcmcia_loop_config`, `pcmcia_enable_device`),
and the socket does not have to be reloaded any more — the hook logs "scheda
tornata da sola" and leaves `yenta_socket` alone.

**What it does not recover:** the MPU, and with it the synth.

    scp55: UART mode not acknowledged, resetting the MPU
    scp55: MPU-401 still silent

The card comes back with its I/O window and its codec alive — registers read and
write correctly — but the MPU answers neither `0x3F` nor `0xFF` until the
machine is really powered down. Programming the control interface first (the
vendor sequence, see `docs/vendor-init-sequence.md`) makes it answer
*sometimes*: it worked twice on a card that had been running, and never on a
freshly resumed one. So on this machine, after S3: the driver puts the socket
back and the codec back, and the card stays mute until a power cycle.

Two operational notes, both learned the hard way: writing `base+4`/`base+5`
on a card that is already running can make it emit broadband noise between
60 and 600 Hz — not notes, noise — and that noise passes through the codec's
DAC, so it can be stopped from software with `echo 'w 6 0x3f' > /proc/scp55mix`
and the same for `0x07`, without touching the power. And `scripts/scp55-vendor-probe.sh`
is the instrument for all of this: it walks the sequence one step at a time and
prints what the card answers, so a failure can be attributed instead of guessed.

It writes what happened to `/tmp/scp55-resume.log`. Without the hook the card is
unavailable until the next reboot; results on other machines may differ, and
reports are welcome.

## Status of testing

Verified on one machine only (details above). What is not verified, and worth
knowing before you spend an evening on it:

- suspend/resume — the socket is programmed at load time and not restored
- kernels other than 6.x — the code uses plain inb/outb and the ALSA rawmidi
  API, so it should port, but nobody has tried at runtime
- two cards in one machine — the sequencer port name is fixed

Automatic loading **is** verified: with the module installed through DKMS and
`depmod` run, a reboot with the card inserted loads `scp55` and creates the
sequencer port without any manual `modprobe`. The module advertises a PCMCIA
modalias (`pcmcia:mC00Cc0001...`), which is what makes this work.

## Kernel compatibility

Compiled in CI on every push — build only, no hardware involved:

| Kernel | Distribution | Result |
| --- | --- | --- |
| 6.1 | Debian bookworm | builds |
| 6.12 | Debian trixie | builds |
| latest | Fedora | builds |

Runtime-tested on 6.1.0-54-686-pae only. The badge at the top reports the
current CI state.

## What is in this repository

    src/scp55.c           the driver
    Makefile              builds from the repository root
    dkms.conf             DKMS build description
    install.sh            one-command installer (DKMS, or direct build)
    packaging/make-deb.sh builds the .deb without debhelper
    udev/99-scp55.rules   permissions for the raw MIDI device
    scripts/              helper to play files
    docs/hardware.md      what is known about the card, and open questions
    docs/windows-driver.md what the original Roland driver disk reveals
    experiments/          earlier variants (play/midi/beep), not maintained
    .github/workflows/    CI: compile matrix, package on tag
    CHANGELOG.md          release notes

The files under `experiments/` are not maintained: they were the steps that led
here, and they are kept because the comments in them document behaviour that is
otherwise undocumented.

## Versioning

Version numbers follow `MAJOR.MINOR.PATCH`. The same string appears in three
places and they must match:

- `dkms.conf` → `PACKAGE_VERSION`
- `src/scp55.c` → `MODULE_VERSION(...)`, visible with `modinfo -F version`
- the git tag, `vX.Y.Z`

Releasing means: bump those three, add an entry to `CHANGELOG.md`, commit, then
`git tag -a vX.Y.Z -m "scp55 X.Y.Z"` and `git push --tags`. CI builds the .deb
on the tag and attaches it to the release.

## The original Windows drivers

The vendor driver disk for this card survives as a self-extracting archive, and
it states things that took experimentation to find here: the card's I/O window is
0x330-0x33B (twelve ports, not two), it declares IRQ 5, and the Windows driver
writes a much larger mixer table than the four registers this driver touches.
What is inside it, and what is still unexplored, is written up in
[docs/windows-driver.md](docs/windows-driver.md).

## Related work and thanks

- **zikolas/scp55-enabler** — a DOS enabler for the same card, C and assembly,
  with three host backends (82365, Card Services, OmniBook ROM Socket
  Services). Written independently, and it reaches the same conclusions about
  the hardware: the card is 8-bit, the MPU status register sits at 0x331, the
  codec data port at 0x339, and the codec must be un-muted before the synth is
  audible. Those facts are stated here with confidence because their write-up
  confirms them.

## In italiano (breve)

Driver per la scheda Roland SCP-55 (Sound Canvas su PC Card, 1995): programma
il socket, toglie il mute al codec CS4231A e presenta la porta MPU-401 come
dispositivo ALSA rawmidi, quindi la scheda diventa un synth MIDI esterno
normale. Installazione con DKMS, poi `aplaymidi -p <porta> file.mid`.

## License

GPL-2.0 (see `LICENSE`). Not affiliated with, endorsed by, or supported by
Roland Corporation. "Roland" and "Sound Canvas" are trademarks of their
respective owners and are used here only to identify the hardware.
