# Changelog

All notable changes to this project. Versions correspond to git tags.

## 0.1.8 — 2026-10-28

- **bounded drain in mpu_init()**: 0.1.7 drained the MPU read register with an
  unbounded loop. On this card DRR can stay high with a byte that never pops, so
  the transmit workqueue never returned and every client that opened the port
  hung: rosegarden froze on its splash screen and aplaymidi sent zero bytes.
  The loop now reads at most eight times. The kernel warned with
  `EIP: mpu_init ... Workqueue: events tx_work_fn`

## 0.1.7 — 2026-10-28

- **the codec loopback was the missing piece**: the synth does not feed the
  CS4231A as a digital stream, it enters the codec as an analogue input, so the
  ADC -> DAC loopback (register `0x0E` bit 1) has to be closed or the card stays
  silent while every status register reports a healthy codec. This is why the
  card played when inserted into a running machine and went mute after a cold
  power-up: 0.1.5 unmuted the input gains but never opened the loopback
- **codec_init()**: one function now brings the whole codec up — Mode Change
  Enable, playback format, ADC/aux/line/mic gains, DAC attenuation and the
  loopback — and it is called from probe and from resume instead of a partial
  unmute in two places
- **MPU-401 acknowledgements**: the driver drains any byte the card queued before
  it arrived and waits up to 1 s for the `0xFE` ack instead of 100 ms. The card
  on the tested CF-30 answers late after a cold boot and the old timeout logged
  "MPU reset not acknowledged" for a card that was in fact answering

## 0.1.6 — 2026-10-28

- **suspend/resume callbacks**: suspend stops the work queues and sends
  all-notes-off; resume re-programmes the codec and re-initialises the MPU-401 in
  UART mode. Both log a line in dmesg
- **MPU initialisation moved to first use**: at probe the card is not ready yet
  and the commands went unacknowledged, which produced misleading warnings in
  the log. The reset and UART-mode commands now run just before the first note
- **packaging/99-scp55-socket**: a pm-utils hook for machines whose PCMCIA bridge
  times out after resume and loses the card, as on the tested CF-30. It reloads
  the bridge and the driver, with retries, and logs to /tmp/scp55-resume.log

## 0.1.5 — 2026-10-28

- **MPU-401 initialisation**: the driver now sends the reset command (`0xFF`) and the
  UART-mode command (`0x3F`) to the command port at probe, checking the `0xFE`
  acknowledgement and logging when the card does not answer. Without this the
  card kept a stale byte in its read path: the receive side read the same value
  forever, which is how the bug was found
- **MIDI input**: a duplex rawmidi stream with a polling workqueue. Implemented
  and compiled, but unverified: on the test hardware the card never transmitted
  anything, including in response to GM identity and GS RQ1 dump requests. The
  receive line appears not to be wired on this interface
- transmission re-verified after the initialisation change: notes still play and
  the byte counters advance

## 0.1.4 — 2026-10-28

- **no more stuck notes**: a GS reset plus all-notes-off on all 16 channels is
  sent when the output opens, and all-sound-off plus all-notes-off when it
  closes, so interrupting playback no longer leaves notes ringing. Controlled by
  the module parameters `reset_on_open` and `reset_on_close` (both default 1)
- **locking**: a mutex serialises every register access (deferred work, procfs,
  test phrase) and a spinlock protects the substream pointer and counters, which
  are touched from atomic context. Teardown order fixed: proc entries, then
  work, then `io_base` under the mutex, then the card
- `/proc/scp55` now reports how many resets were sent and how many payload
  bytes went out, which is what made the behaviour testable
- validated on hardware: 15 playbacks with concurrent procfs reads and writes,
  no kernel warnings, counters consistent

## 0.1.3 — 2026-10-28

Documentation and packaging hygiene, no driver change.

- no email address in the repository: contact happens through GitHub issues
- `Makefile` in the repository root, so `make` works from a fresh clone
- the DKMS instructions copy only `src/` and `dkms.conf`, not the whole tree
- complete file tree in the README; badge links to the CI runs, not the image
- install options numbered 1-3, no more "three ways" followed by four sections
- default branch renamed to `main`

## 0.1.2 — 2026-10-28

Documentation only, no code change.

- autoload on card insertion is now documented as verified rather than expected
- release artifacts are built by CI on the tag and attached automatically

## 0.1.1 — 2026-10-28

- Fix the DKMS build: the module is produced in `src/`, so `dkms.conf` now sets
  `BUILT_MODULE_LOCATION[0]="src"`. With 0.1.0 the compile succeeded but DKMS
  could not find `scp55.ko` and refused to install it
- `install.sh` and the package `postinst` no longer hide DKMS failures behind
  `|| true`; a failed build now fails loudly instead of leaving a machine with
  no module and no explanation

## 0.1.0 — 2026-10-28

First public release.

- Kernel module for the Roland SCP-55 PC Card: programs the PCMCIA socket so
  the MPU-401 UART answers at I/O base 0x330, and un-mutes the CS4231A codec the
  synth output runs through
- ALSA rawmidi output device, exposed as the sequencer port "Roland SCP-55"
- `/proc/scp55` status and test phrase, `/proc/scp55mix` codec register access
- DKMS build description, udev rule, `install.sh`
- Tested on a Panasonic CF-30, MX-23.6, kernel 6.1.0-54-686-pae
