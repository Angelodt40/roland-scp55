# The vendor init sequence, recovered from `SCPENA.EXE`

Notes from disassembling the DOS enabler on the 1995 driver disk
(`SCP55 Enabler Ver.1.02 95/10/03 (c)Roland Corp.`). This is the program that
brought the card to life before any driver ran, so it is the closest thing to a
datasheet for the parts of the card the Linux driver never touched.

## How it was read

```
unzip -o scp-55.exe -d scp55win
python3 - <<'EOF'   # MZ header: header paragraphs * 16 = image offset
import struct
d=open('SCPENA.EXE','rb').read()
_,last,pg,rel,hdr,_,_,ss,sp,_,ip,cs,_,_=struct.unpack('<2s13H',d[:28])
print(hdr*16, 'entry', hdr*16+cs*16+ip)   # 512, entry 0x0ee3
EOF
dd if=SCPENA.EXE of=scpena.bin bs=512 skip=1 status=none
objdump -D -b binary -m i8086 -M intel scpena.bin > scpena.asm
```

The image is one 16-bit segment: file offsets in `scpena.bin` are the addresses
quoted below.

## It uses Card Services, not direct socket programming

The strings give the flow away: `Card Service Registration OK`,
`Request I/O Failed`, `IRQ Request Failed`, `Card Configuration Succeeded`,
`Card is Inserted to #`. There is one `int 0x1A` and no PIC programming: the
enabler asks Card Services to map the window and reserve an IRQ, then talks to
the card.

The candidate windows are a 7-byte table at **0x264**, one record per choice:

    'A' 30 03 0c 01 02 01   -> base 0x0330, 0x0C = 12 ports
    'B' 70 03 0c 01 02 01   -> base 0x0370
    'C' b0 03 ...           -> base 0x03b0
    'D' f0 03 ...           -> base 0x03f0
    'E' 30 13 ...           -> base 0x1330
    ...  up to 'I'

Twelve ports, exactly as `Scp95a.inf` says (`IOConfig=330-33B`).

## The init routine (0x6d4)

`ds:0x166` holds the I/O base. In order:

| where | what | meaning |
|---|---|---|
| 0x6dd | `base+4 <- 0x02` | control register selector |
| 0x6e1 | `base+5 <- 0x01` | value 1 |
| 0x6e5 | loop: `base+4 <- 0x02`, read `base+5` until 0 | wait for the control interface |
| 0x704 | `base+10 <- 0x00` | 0x33A, the other untouched port |
| 0x712 | `base+1 <- 0x3F` then read `base+0` | **enter UART mode first** |
| 0x71e | `base+4 <- 0x04`, `base+5 <- 0x02` | switch the control interface |
| 0x72d | poll `base+1` bit 0x80, read `base+0` until `0xFE` | wait for the ack |
| 0x751 | `base+1 <- 0xFF` then read `base+0` | MPU reset **after** the ack |
| 0x75d | `base+4 <- 0x04`, `base+5 <- 0x01` | control back to idle |
| 0x773 | `rep outs` 13 bytes from **0x7ef** | RQ1 request (see below) |
| 0x77f | `rep outs` 11 bytes from **0x7e4** | GS Reset |
| 0x796 | codec `0x0C <- 0xC0` | |
| 0x7a4 | codec `0x12 <- 0x08` | |
| 0x7b2 | codec `0x13 <- 0x08` | line in, unmuted, 0 dB |
| 0x7c0 | codec `0x02 <- 0x08` | auxiliary 1 left |
| 0x7ce | codec `0x03 <- 0x08` | auxiliary 1 right |

The two sysex tables:

    0x7e4 (11): F0 41 10 42 12 40 00 7F 00 41 F7   GS Reset
    0x7ef (13): F0 41 10 42 11 4F 40 00 00 00 01 70 F7   RQ1, 1 byte @ 4F 40 00

## What this changes for our driver

1. **`0x334`/`0x335` are a control interface on the card, not the codec.**
   Write a selector to `base+4`, then a value to `base+5`; reading `base+5`
   returns a status that goes to zero when the operation lands. Our earlier port
   dump showed `0x334 = 0x00`, `0x335 = 0xAA`, i.e. a card that was never put
   through this sequence. Everything the driver does today works *around* that.
2. **The vendor never enables the codec loopback (`0x0E`).** That is the most
   interesting consequence: with the control interface programmed properly the
   synth's audio probably reaches the codec's DAC by the intended path, and our
   loopback is a workaround for a card left in the wrong mode. Worth testing
   before the next release: run the vendor sequence, then disable the loopback
   and see whether the card still plays.
3. **Order matters at the MPU**: `0x3F` first, wait for the `0xFE`
   acknowledgement, *then* `0xFF`. 0.1.9 avoided the problem by not touching the
   MPU at all on open; the vendor shows the correct order for the case where it
   does need re-initialising.
4. **`0x33A` gets a zero write.** Probably a mode or interrupt-mask latch, still
   unknown, but it is part of the sequence.

## Next

- `SCP95.VXD` (`SCP-55 VxD`, string `SCP-55 VxDRolandA0`) to find the IRQ 5
  handler and the MIDI receive path: the enabler only *reserves* the IRQ, the
  handler must be in the VxD.
- `SCP55CNT.EXE` / `SCPMIXER.DLL` for the full mixer map behind the
  `SOUND.INI` values.


## SCP95.VXD (Windows 95)

Strings identify it as a real VxD (`_ICODE__MapPhysToLinear`, `_PageAllocate`,
`_RegQueryValueEx`, `SHELL_CallAppyTime`, module name `SCP95`/`scp95`). The
object table is not where a standard LE header puts it, so the whole file was
disassembled as flat 32-bit code
(`objdump -D -b binary -m i386 -M intel SCP95.VXD`): 94 port accesses, in four
clusters that map neatly onto the routines below. The `mov dx, WORD PTR ds:0x0`
instructions are relocations — the disassembly shows the offset as zero because
the fixups are applied at load time; the real value is the I/O base.

### Card identification: the `0xAA` signature (0x29ae)

```
xor  al,al ; out dx,al        ; write 0 to base+4
inc  dx    ; in  al,dx
cmp  al,0xaa ; jne fail       ; base+5 must read 0xAA
mov  al,1  ; out to base+4
loop: al=cl ; out base+5 ; in al ; cmp al,cl ; jne fail   ; read/write test
```

That is the answer to the port dump question: `base+4`/`base+5` is a
read/write register pair, and **0xAA is the card's signature**. Our dump showed
`0x334 = 0x00`, `0x335 = 0xAA` — the card sitting in exactly the state this
test leaves behind. The driver can identify the card with four port accesses,
without relying on the CIS alone.

### MPU reset with a real handshake (0x2940)

```
mov dx, base+1
loop until (in al,dx & 0x40) == 0     ; DSR clear: card ready
mov al,0xff ; out dx,al               ; reset
loop until in al,dx & 0x80            ; DRR set: something to read
in al,dx ; cmp al,0xfe ; jne fail     ; must be the acknowledgement
```

Both the enabler and the VxD do it this way, with the same timeout budget
(0x6000 iterations). Our 0.1.9 avoids the need by never touching the MPU, but
this is the correct routine for the case where it does need re-initialising —
and it is the one that was missing when 0.1.7 wedged the machine.

### Codec access, including the second register bank (0x29de)

The accessor waits for bit 7 of the codec port to clear, then writes index in
`AH` and data in `AL`. Right after the MPU reset it does:

```
ah=0x0C al=0xC0   ; write, then verify (in al,dx & 0xC0) == 0xC0
ah=0x49 al=0xC8
ah=0x10 al=0xC0
```

Registers `0x49` and `0x10` do not exist on a plain CS4231A, which is why
guessing the mixer from the four registers we knew was never going to be
complete: the card's codec is driven through an **indirect bank**
(`0x10` is the door), and `0x49` is a register inside it. This is the missing
half of the mixer story, and the reason `SOUND.INI` lists controls we could not
map.

### Full init sequence (0x2a4f), identical in structure to the enabler

```
base+4 <- 2 ; base+5 <- 1
loop { base+4 <- 2 ; read base+5 } until 0
read base+5 ; base+1 <- 0x3F ; read base+0
base+4 <- 4 ; base+5 <- 2
loop { wait DRR ; read base+0 } until 0xFE
base+1 <- 0xFF ; read base+0
base+4 <- 4 ; base+5 <- 1
rep outs base+0 <- 13 bytes   ; RQ1  4F 40 00
rep outs base+0 <- 11 bytes   ; GS Reset
```

Two independent binaries — a DOS enabler from 1995 and a Windows 95 VxD from
1996 — perform the same sequence in the same order, which is as close to a
manufacturer's blessing as this card will ever get.

## What is still open

- **The IRQ 5 handler**: the enabler only *reserves* the IRQ through Card
  Services; the handler must be in the VxD. Finding it is the route to MIDI
  input, which we currently document as impossible.
- **The indirect codec bank**: enumerate which indices the VxD writes through
  `0x10`, and the mixer map becomes explicit instead of inferred from
  `SOUND.INI`.
- **Why the loopback is needed at all**: our driver needs `0x0E = 0x02`; neither
  vendor program sets it. Either the control-port sequence puts the card in a
  mode where the synth reaches the DAC directly, or the vendor relied on a codec
  state we still have not reproduced. Testable: run the vendor sequence on the
  real card, with the loopback off, and listen.


## SCP95.DRV — the Windows driver, and the part I got wrong

`SCP95.DRV` is a 16-bit NE executable (nine segments, one code segment).
Strings settle a question that this repository previously answered badly:

- the INF installs it as `wave*=scp95.drv` with
  `SubClasses="wave,midi,aux,mixer"` and class "Sound, video and game
  controllers";
- the driver contains `Single Mode DMA`, `Sound Blaster Emulation`,
  `WaveOutVol`, `WaveOutMute`, `WaveADCSource`, `VU for Wave Input`;
- `SCP55.DRV` (Windows 3.1) talks about `Waveform Input`/`Waveform Output`.

So the SCP-55 is **not** a MIDI-only card: on Windows it plays PCM, records,
mixes and emulates a Sound Blaster. Saying otherwise was a mistake — what this
project implements today is the MIDI half, and the driver's own documentation
had already said "wave/midi driver".

### The register pair at base+2/base+3

The wave/mixer code accesses a pair that no Linux driver touches. In the
disassembly (function at 0x12ea in the code segment) the idiom is:

```
mov dx,[si+6] ; inc dx ; inc dx ; out dx,al   ; write to base+2
mov dx,[si+6] ; add dx,3 ; in al,dx           ; read from base+3
```

with a shadow byte at `[si+0x17]` holding what looks like the CS4231
interface-control register: the driver sets bit `0x40` (Mode Change Enable,
the same bit the CS4231 keeps in register 0x09), writes it out, then clears it
and re-writes with bits `0x09`, polling bit `0x20` of `base+3` for
completion.

A port dump of a card that had just been probed by our driver reads
`0x332 = 0x80`, `0x333 = 0x00` — plausible for exactly this pair. This is a
candidate, not a conclusion: it has not been written to on purpose, because the
neighbouring pair `0x334`/`0x335` is a mode switch that silences the card when
touched at runtime.

### PCM and the DMA problem

The CS4231A is the codec of the Sound Blaster Pro; its PCM path is driven by
**ISA-style DMA**, and the "Single Mode DMA" string says the Windows driver
relied on it. On the test machine the PCMCIA socket provides no DMA resources
at all (`/sys/class/pcmcia_socket/pcmcia_socket0/available_resources_dma` does
not exist), so `pcmcia_request_dma()` would have nothing to hand out.

That makes PCM playback on this particular laptop unlikely, but it is not
proven yet. The decisive test is a one-line probe in the driver: ask the socket
for a DMA channel at probe time and log the result. If that fails, the honest
position is "the card can do PCM, this machine's bridge cannot feed it", and
the next thing to try is a machine whose PC Card controller still implements
DMA.


### The vendor driver does not use DMA

Three independent sources agree:

| evidence | says |
|---|---|
| `Scp95a.inf`: `DMAConfig=` (empty) | the card requests no DMA channel |
| `SCP95.VXD` imports | no `VDMAD`, `VSD` or `DMA` service at all |
| `SCP95.DRV` strings | `DMA` appears only in `Single Mode DMA`, a mixer option label |

So the Windows driver played PCM **without ISA DMA**, which is the opposite of
what a CS4231A normally needs. The plausible architecture — a hypothesis, not a
measurement — is that the card carries its own audio controller:

- the CIS asks for a **1 MB memory window** (the socket probe logs
  `memory probe 0xe8210000-0xe82fffff` at load), which is the size one would
  expect for an audio ring buffer;
- the wave/mixer code addresses a register pair at `base+2`/`base+3` with a
  shadow byte that mirrors the CS4231 interface control register, i.e. a
  command/status interface of its own;
- the code uses far pointers and `ES` segment loads (`les bx,[bp+4]`,
  `mov es,dx`), consistent with streaming samples through a mapped segment
  rather than through port I/O.

If that holds, PCM playback on Linux is possible **without any DMA channel**:
the driver would map the window, write samples into it, and kick the card
through `base+2`/`base+3`. That would turn this from a MIDI interface into a
real sound card.

### Next concrete steps

1. Decode the CIS config entries by hand (the modern `pcmciautils` no longer
   ships `dump_cis`) to see the memory window descriptors: attribute or common
   memory, 8- or 16-bit, size.
2. Read the wave entry points of `SCP95.DRV` and turn them into a register map
   for `base+2`/`base+3`: what selects format, what carries the buffer
   pointer, what starts and stops playback.
3. Before touching the kernel module, prove it from user space: map the window,
   write a few frames, and see whether the card makes a noise.


## Port map, measured on the card

Twelve ports, all probed read-only unless stated. Values are from a card freshly
powered up and then running the driver's probe.

| port | behaviour | what it probably is |
|---|---|---|
| `0x330` | data, a stale `0x42` sat here for hours | MPU-401 data |
| `0x331` | `0x80` = DRR set, `0xA0` after traffic | MPU-401 status |
| `0x332` | moves in lockstep with `0x331` (`0x80` → `0xA0`) | mirror of the MPU status |
| `0x333` | always read `00`, ignored the values written | unknown, not an index latch |
| `0x334` | writable | mode selector |
| `0x335` | `0xAA` cold = signature; `00`, `01` after the vendor sequence | signature / status |
| `0x338` | codec index | CS4231A index |
| `0x339` | codec data (`0x0C` → `0x8A`) | CS4231A data |
| `0x33A` | writable register, keeps the value written | card control |
| `0x33B` | `0x02` at rest, `0x00` after MIDI activity | card status |

Experiments that produced this table (all reversible, each followed by a test
phrase to confirm the card still played):

- writing `0x00` to `0x332`: no observable change, so it is not a register
  selector;
- writing `0x0C` to `0x332` and reading `0x333`: returns `0x00`, while the
  same index through `0x338`/`0x339` returns `0x8A` — so `0x332`/`0x333` is
  **not** a second view of the codec;
- writing `0x0C` and then `0x02` to `0x33A`: the value stays in `0x33A`
  (`0x02`), which proves it is a plain register and not an index latch;
- `0x33B` fell from `0x02` to `0x00` after the test phrase, twice.

One earlier reading of the disassembly was wrong and is corrected here: the
`base+2`/`base+3` accesses in `SCP95.DRV` are **not** relative to the I/O base
— the structure field they use points elsewhere, most likely to `base+8`, which
would make them `0x33A`/`0x33B`. That is consistent with the enabler writing
`0x00` to `0x33A` during its init sequence.
