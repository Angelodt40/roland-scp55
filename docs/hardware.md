# Hardware notes — Roland SCP-55

Everything below was established by testing on one card (a 1995 SCP-55) in one
machine (Panasonic CF-30) and by disassembling the original Roland driver disk.
Where a statement is a hypothesis rather than a measurement it is labelled as
such — this file is more useful honest than confident.

If your card behaves differently, please open an issue. Negative results are as
welcome as positive ones.

## Identity

- 16-bit PCMCIA (PC Card), not CardBus. Type II.
- Roland SCP-55 "Sound Canvas": GS/GM synthesiser plus an MPU-401 compatible
  MIDI interface, on the card.
- The CIS declares a **twelve port** I/O window (`IOConfig=330-33B`), a
  gameport at `0x201` and IRQ 5, with no DMA channel.

## Port map

| ports | what | status |
|---|---|---|
| `base+0` / `base+1` | MPU-401 data / status | proven |
| `base+4` / `base+5` | control and signature pair | partly proven |
| `base+8` / `base+9` | CS4231A codec index / data | proven |
| `base+10` | written `0x00` by both vendor programs | unknown purpose |

## The signature: 0xAA

After power-on `base+5` reads **0xAA**. Both vendor programs verify it
(`SCPENA.EXE` at 0x29ae, `SCP95.VXD` at 0x29b5); they first select the
register with a write of `0x00` to `base+4`.

The driver only *reads* it, at probe, before anything else has touched those
ports. That is deliberate: writing to this pair is a **mode switch**, and doing
it on a card that is already running silences it until the next power cycle —
observed, not theorised. The full sequence the vendor runs, and what it does to
the card, is in [vendor-init-sequence.md](vendor-init-sequence.md).

## Output path

The synth does not drive a DAC with its own line out: its audio enters the
CS4231A codec, and the codec drives the card's analogue output. **How** it
reaches the codec depends on a mode the card selects through the control pair
at `base+4`/`base+5`.

- The vendor wave path writes `0x03` to `base+4` and `0x01` to `base+5`
  before playing (`SCP95.DRV`, segment 7). With that mode the audio is routed
  as the card was designed to route it.
- **Verified on hardware:** with that mode selected and the codec's loopback
  register read back as `0x00`, both the test phrase and a full MIDI file are
  audible. The driver selects the mode at probe (`card_mode=1`).
- The codec's ADC→DAC loopback (`0x0E` bit 1) is what version 0.2.0 relied on.
  It works, but it was a workaround for not knowing about the mode: unmuting the
  input gains alone is not enough, and without either the loopback or the mode
  the card is completely silent while every status register looks healthy. The
  loopback is still available as `use_loopback=1` for cards where the mode
  write does not take.

## The MPU-401

- In UART mode, status bit 0x80 (DRR) means a byte is waiting, bit 0x40 (DSR)
  means the card is not ready for another one.
- The card answers `0xFE` to the `0x3F` (enter UART) and `0xFF` (reset)
  commands — but not always, and not quickly. Measured behaviour: replies can
  arrive hundreds of milliseconds late, and a read that happened earlier can
  leave a stale `0xFE` in the buffer.
- **Do not reset the MPU when opening the device.** `0xFF` puts an MPU-401 into
  *intelligent* mode, where bytes that look like MIDI are interpreted as
  commands; if the following `0x3F` is lost the card stays there and silently
  discards every note. This cost a whole debugging session: the byte counters
  kept growing while the card was mute.
- If re-initialisation is ever needed, the vendor order is: wait for DSR,
  `0x3F`, wait for the `0xFE` acknowledgement, then `0xFF`.

## MIDI input

Documented as unavailable, and the reasoning is worth recording:

- the card never raises DRR on the read side: identity requests, RQ1, GS reset
  and note traffic all produce zero bytes;
- it does acknowledge MPU *commands*, so the card→host path exists but carries
  no MIDI;
- **the MIDI IN side was never exercised.** It needs the breakout cable with the
  DIN connectors, which we do not have. So "the card does not send" is measured;
  "the card does not forward incoming MIDI" is **not** measured, and until the
  cable is here this stays untested;
- an earlier revision of this file argued that the Windows 95 VxD imports no
  VPICD service, therefore the vendor had no interrupt-driven input either. That
  argument does not hold: the 16-bit driver enables and masks IRQ lines directly
  on the 8259 (segment 5, ports `0x21` and `0xA1`), so the vendor *did* use
  interrupts. What was never traced is whether a handler ever read MIDI data.

The 13-byte RQ1 both vendor programs send (`F0 41 10 42 11 4F 40 00 00 00 01 70
F7`) is a request that this card, as far as we can tell, never answers.

## Codec register bank

The Windows VxD writes, beyond `0x02`–`0x07`:

    ah=0x0C al=0xC0   (then verifies (in al,dx & 0xC0) == 0xC0)
    ah=0x49 al=0xC8
    ah=0x10 al=0xC0

`0x49` does not exist in a plain CS4231A register map, which suggests the card's
codec exposes an **indirect bank** (`0x10` acting as the door). This is the
missing half of the mixer map and the reason the `SOUND.INI` controls could not
be matched one to one. Not yet explored.

## Procfs interfaces

- `/proc/scp55` — status (I/O base, codec state, signature, chip id); writing
  `1` sends a short test phrase to the synth.
- `/proc/scp55mix` — `w <hex idx> <hex val>` write a codec register,
  `r <hex idx>` read it, `i` apply the unmute defaults, `u` mute both
  channels.

## Module parameters

| parameter | default | what it does |
|---|---|---|
| `beep_on_probe` | 1 | play the test phrase when the card is probed |
| `reset_on_open` | 0 | send a GS reset and all-notes-off on open; off because clients that open and close the port repeatedly turn it into a stream of resets that wipes playback |
| `mpu_init_on_open` | 0 | re-initialise the MPU-401 on open; off because the card is already in UART mode after power-on, and a lost command leaves it in intelligent mode |

## Open questions

1. ~~Does the card mode make the loopback unnecessary?~~ **Answered:** yes.
   `0x334 <- 3`, `0x335 <- 1` is the routing the vendor wave path selects, and
   with it the card plays with the loopback register read back as `0x00`. The
   driver now does this at probe. Applying the *full* vendor sequence with `4`
   instead of `3` still silences a running card — that part remains a warning.
2. What is the indirect codec bank, and does it expose a real mixer we can
   model as ALSA controls instead of the current procfs escape hatch?
3. What is `base+10` for?
4. Suspend/resume: on the test machine the PCMCIA bridge times out after
   resume and the card disappears from the bus. Reloading `yenta_socket`
   brings it back, but audio does not return without a real power cycle.

## Reproducing the analysis

The driver disk can be unpacked without running it:

    unzip -o scp-55.exe -d scp55win

`SCPENA.EXE` is a DOS MZ image; `SCP95.VXD` is a 32-bit LE VxD. The method
used to read both, and the sequences recovered from them, are in
[vendor-init-sequence.md](vendor-init-sequence.md).
