# Changelog

All notable changes to this project. Versions correspond to git tags.

## 0.2.4 — non ancora pubblicata

Verificata sulla CF-30 solo in parte: il resume è stato letto nel log del kernel,
non ancora riascoltato dopo il risveglio. Non va taggata prima di quella prova.

- **il driver prova a ricostruirsi la configurazione da solo dopo S3**: il core
  pcmcia rifiuta `pcmcia_enable_device` al resume con `-EACCES`, anche se la
  scheda è ancora sul bus con il driver legato. Se l'enable del core fallisce,
  `.resume` ora fa quello che fa il probe: `pcmcia_disable_device`,
  `pcmcia_loop_config`, `pcmcia_enable_device`, e solo dopo ripristina il modo
  della scheda e il codec
- **il resume ripristina il modo della scheda**: una sospensione è un ciclo di
  alimentazione per la scheda, che perde sia lo stato del codec sia il modo di
  routing (`0x334 <- 3`, `0x335 <- 1`). La firma va riletta *prima* di
  riscrivere il modo, perché il modo la cancella
- nota d'uso documentata nel README: con `reset_on_close=1` (default) chiudere
  la porta rawmidi manda all-notes-off e all-sound-off, quindi un client che
  apre, scrive una nota e chiude subito (come `amidi -S`) fa sentire solo
  l'attacco

## 0.2.2 — 2026-10-28

- **packaging fix only**: 0.2.1 shipped a `.deb` named `scp55-dkms_0.2.0-1_all.deb`
  because the packaging script takes the version from `dkms.conf`, which had not
  been bumped with the source. The module itself was correct and verified; the
  package label was not. No code changes in this release

## 0.2.1 — 2026-10-28

- **the codec loopback was a workaround, and it is gone**: the vendor wave path
  selects the card's audio routing by writing `3` to the control register at
  `base+4` and `1` to its neighbour (recovered from `SCP95.DRV`, segment 7).
  With that mode the synth reaches the codec's DAC directly — verified on
  hardware with the loopback register read back as `0x00`: the test phrase and
  a full MIDI file are audible. The loopback is now `use_loopback=0` by
  default and kept only as a switch
- **the card is identified before it is configured**: selecting the mode
  overwrites the `0xAA` signature, so `/proc/scp55` reported the card as
  unexpected until the read was moved ahead of the mode write
- `card_mode=1` by default; both parameters documented in
  [docs/hardware.md](docs/hardware.md)

## 0.2.0 — 2026-10-28

- **card identification by signature**: after power-on the card answers `0xAA`
  at `base+5`. Both vendor programs rely on it (`SCPENA.EXE` 0x29ae,
  `SCP95.VXD` 0x29b5) after selecting the register with a write to `base+4`;
  at probe nothing has touched those ports yet, so the driver only *reads* it.
  A read cannot put the card in a wrong mode, a write demonstrably can: applying
  the full vendor sequence to an already running card silenced it until reboot
- `/proc/scp55` now reports the signature and says whether the card is
  confirmed
- reverse engineering written up in `docs/vendor-init-sequence.md`: how to
  unpack and disassemble the original disk, the full vendor init sequence, the
  control register pair at `base+4`/`base+5`, and the indirect codec bank that
  the Windows VxD uses

## 0.1.9 — 2026-10-28

- **the MPU is no longer reset on every open**: `mpu_init()` sent `0xFF` (which
  puts an MPU-401 into *intelligent* mode) and then `0x3F` without waiting for
  the card to be ready. When the `0x3F` was lost the card stayed in intelligent
  mode and silently discarded every note — `aplaymidi` reported success, the
  byte counters grew, and the card was mute. The driver now leaves the MPU alone
  on open (`mpu_init_on_open`, off by default): after power-on the card is
  already in UART mode, as the probe beep proves
- **`reset_on_open` off by default**: the GS reset on open is harmless on its
  own, but clients that open and close the port repeatedly turned it into a
  stream of resets that wiped playback mid-file. Both parameters are documented
  and can be re-enabled
- **`mpu_init()` rewritten** for the case where it *is* needed: it waits for DSR
  before each command byte, retries `0x3F` up to five times and only falls back
  to a full `0xFF` reset when nothing is acknowledged

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
