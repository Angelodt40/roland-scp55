# The wave engine: what the vendor driver says, and what the card does

Status: open. This file collects only measured facts and corrections, so the next
session does not repeat the dead ends.

## Where the wave code lives

`SCP95.DRV`, segment 7: file range `0x038e0..0x07520`, length 15424.
Extract it with:

    dd if=SCP95.DRV of=seg7.bin bs=1 skip=$((0x038e0)) count=15424

and disassemble 16-bit. `objdump` on this machine has no i8086 target; capstone
in a throwaway venv does the job:

    python3 -m venv /tmp/venv && /tmp/venv/bin/pip install capstone

## The engine kick, as compiled

At `0x1e36` there is a function that ends with the only two `out` writes whose
ports are computed as offsets from the structure field `[si+8]`:

    mov  ax, 3
    mov  dx, [si + 8]      ; 0x33A
    out  dx, al            ; index 3
    ...test two flags...
    inc  dx
    out  dx, al            ; 0x33B <- 0 or 1

So the driver selects register 3 of the `0x33A` register file and writes 0 or 1
to it immediately before starting the wave path.

## Register 3, tested on the card

Replayed from user space (`/dev/port`, no module reload):

| write | `0x33A` reads | `0x33B` reads |
| --- | --- | --- |
| index 3, value 1 | `03` | `02` |
| index 3, value 0 | `03` | `02` |

The index latches, and `0x33B` keeps returning the status byte `0x02`, not the
value written - consistent with `0x33B` being write-data/read-status.

Then a C-E-G chord was played with register 3 = 1 and again with register 3 = 0.
Angelo listened to both: **identical**. So register 3 does not touch the synth
path we hear today. It is a negative result, and a useful one: it keeps the
search away from the MIDI side.

## Not streaming: the copies at the top of the segment

The `rep movsw` at `0x0082`, `0x0094`, `0x00a6` copy 0x391, 0xf9 and 0x6f0 words
from the code segment into a buffer whose segment comes from `[si+0x1e]`. They
run once, at init, and carry driver tables - not audio frames. Reading them as
'streaming into the window' was wrong.

## The window behaves like a port, not like RAM

This is the measurement that reframes everything: writing one word at offset 0
reads back correctly at offset 0, but offsets 2, 4, 6, 0x100, 0x1000 and the
next card page all return the high byte replicated, and writing eight words in a
row leaves only the last one readable. Every write lands in the same place.

That is what a sequential data port looks like from the host side, and it fits a
1996 driver that plays PCM without any DMA channel: the host pushes samples in,
the card consumes them in order.

## Next, concretely

1. Find the playback routine, not the init one: the loop that writes frames
   while the stream is active. Look for the interrupt handler the PIC helper in
   segment 5 masks, and for the structure field that carries the buffer
   pointer.
2. From the same routine, read what selects the sample format: the CS4231 at
   `0x338`/`0x339` has the format register (index 0x08) and the interface
   control (0x09); the `0x33A` file is something else and is still unmapped.
3. Only then write to the window from a test: a short silent frame at a known
   format, then a tone, with the mute ready.

## Two facts that narrow the plan (29/10)

### This socket has no DMA, measured

`/sys/class/pcmcia_socket/pcmcia_socket0/` lists `available_resources_io`,
`available_resources_mem` and `available_resources_setup_done`. There is no
`available_resources_dma`. So on this laptop the CS4231's usual PCM path -
ISA-style DMA - cannot be fed at all, whatever the driver asks for.

That leaves exactly one route to PCM here: the memory window, if the card's
controller really drains it in order. It also means the question 'can this card
play PCM on Linux' is a question about the window, not about DMA.

### The only iret is a vector-chaining stub

Segment 5 holds the single `iret` of the whole driver, and it is not an audio
handler. The code around it is a hook/unhook helper: `int 21h ah=35` to read a
vector, `int 21h ah=25` to install one, and the stub itself does

    push ds / push es / pushad
    mov ax, 0xffff ; mov ds, ax ; sti
    call 0xffff:0xd3a          ; chain to the previous handler
    cli
    mov si, 0x1234
    mov ax, [si+0x41] ; or al, al ; jz skip ; mov dx, [si+0x48] ; out dx, al
    mov al, ah ; mov dx, [si+0x46] ; out dx, al
    popad / pop es / pop ds / iret

So the interrupt path ends by writing two values to two ports taken from a
structure - which looks like an acknowledge to the card, not like feeding it
audio. Finding who installs this hook, and with which vector, is the next step.

### No literal ports anywhere

A scan of segments 3, 6, 7 and 8 finds no `out 0xNN` / `in al,0xNN`: every port
access goes through `dx` loaded from a structure field. That is why the
disassembly shows a lot of `out dx, al` with nothing readable around them, and
why the register map has to be recovered by following the structure, not by
grepping for constants.

## The 64-register file is write-only from the host (29/10, 0.4.1)

`0x33A` latches any index from 0x00 to 0x3f - all sixty-four hold the value
written - and `0x33B` returns `0x02` for every one of them. So it is not a
register file we can read back: index in, data out, status when reading. The
register map can only come from the driver, not from the card.

### Register 3 does not enable the window

With the window reserved (`mem_probe=1`) and register 3 set to 0 and then to 1
from user space, the window behaves identically: the same sixteen zero bytes at
page 0, and the same alias pattern - `1234` at offset 0, `1200` everywhere else,
including page 1. Whatever register 3 selects, it is not the path to the window
and it is not audible on the synth either (measured earlier).

### Driving the research without reloading

`/proc/scp55probe` now runs the experiments on a card that is already up, which
matters because `insmod` re-runs the probe and changes the state we want to
observe:

    insmod scp55.ko mem_probe=1
    echo read  > /proc/scp55probe    # first 16 bytes of page 0
    echo alias > /proc/scp55probe    # one word, read from several offsets
    echo w8    > /proc/scp55probe    # 8-bit write test
    echo w16   > /proc/scp55probe    # 16-bit write test

Results land in dmesg. The next experiments - feeding a stream, trying a
register that matters - can be timed against a note that is already playing,
which was impossible when every test needed a reload.

## Feeding the window does nothing (29/10, 0.4.2)

With the window reserved and a card that was already playing, `/proc/scp55probe`
wrote 8192 words into the aperture - zeros first, then a quiet square wave at two
different amplitudes - once with register 3 at 1 and once at 0. The tones were
written while the synth was idle and nothing was audible at any point: silence,
measured by ear, both ways.

So the window accepts writes and nobody consumes them. The engine is not running,
and neither writing to the window nor selecting register 3 starts it.

That leaves a short list, in order of likelihood:

1. A command the vendor VxD issues before streaming that we have not found yet.
   The 16-bit driver writes the 0x33A file only twice in its whole wave path
   (index 3, value 0/1), so the sequence that starts the engine is in
   `SCP95.VXD`, not in `SCP95.DRV`.
2. A card mode that disables the memory interface until told otherwise - the
   card does behave differently with the window reserved (the signature stops
   reading 0xaa and `base+2` changes), so something in the configuration does
   reach the card.
3. The window is not for streaming at all, and the 1 MB is used by the card for
   its own buffers, never read by the host. In that case PCM would need the
   codec path, which needs DMA, which this socket does not have - and the
   honest conclusion would be that this laptop cannot play PCM through this
   card.

The next step is therefore not on the card: it is reading `SCP95.VXD` for the
sequence that precedes the first sample, and comparing it with what the 16-bit
driver does.

## The VxD transmit loop is MIDI, not PCM (29/10)

The most promising loop in `SCP95.VXD` turned out to be the MIDI transmitter.
At `0x1D01` the code loads a byte index, a count of `0x80`, a port from a data
word, and then:

    inc dx / in al, dx / and al, 0x40 / jnz done
    dec dx / mov al, [esi] / out dx, al / inc si / and si, 0x3ff / loop

A byte at a time with the MPU-401 ready bit, a 1 KB ring, at most 128 bytes per
call. Three things say MIDI and not audio:

- a few instructions earlier the code compares `al` with `0xF7`, the end of a
  SysEx message;
- the queue built at `0x1ccc` holds 128 entries of `(dx, ax)` pairs - status
  and data bytes - not samples;
- the handshake bit is `0x40` on the status port, which is the MPU-401 DSR this
  driver already uses for rawmidi.

So the PCM path is still unfound, and the 16-bit driver has no other port
streaming to offer. Recorded here because it is an easy thing to mistake for
the wave engine, and it cost time once.

## The VxD has no named exports (29/10)

SCP95.VXD is an LE image whose strings are imports and object names, not
services:

    _ICODE__RegOpenKey, _ICODE__RegQueryValue, _ICODE__PageAllocate,
    _ICODE__MapPhysToLinear, _ICODE__SHELL_CallDll, _ICODE__SHELL_GetProcAddress
    objects: ICOD, PDAT, LCOD, and the module name SCP95

There is no WriteWave, no SetFormat, no name of any kind to attach to: the
exports are ordinal-only. So the route of finding the entry point by name ends
here.

What remains is real work, not a probe: extract the LE objects, apply the fixup
records, and follow the device I/O control handler from its dispatch message
code. That is a project of its own, and it should be started knowing that the
answer may be that PCM needs either DMA or an internal buffer the host never
touches - in which case this laptop cannot play PCM through this card whatever
the driver does.

The honest position for the project today: the card is a working MIDI synth
interface with a working volume control, the memory window is mapped and
measured, and PCM is an open research question with no promise attached.

## Inside SCP95.VXD: the object table (29/10)

Parsed from the LE header at 0x80. Page size is 0x200 (512 bytes), 27 pages,
no resident name table and no fixup tables where the usual layout puts them -
which is why a textbook LE parser returns nothing useful.

The object table sits at file offset 0x144, seven entries of 24 bytes, each
ending with a four-character name:

    object   size     flags   page   pages  name
    1        0x119b   0x2045  1      9      LCOD
    2        0x0087   0x2005  0x0a   1      RARE
    3        0x0871   0x2005  0x0b   5      PNP
    4        0x005a   0x2005  0x10   1      _LMS
    5        0x1056   0x2005  0x11   9      PCOD
    6        0x013e   0x2023  0x1a   1      PDAT
    7        0x00ec   0x2015  0x1b   1      ICOD

So there are two code objects of nine pages each (LCOD and PCOD), a small ICOD,
a PDAT data object, and three small ones. Page map at file offset 0x1ec maps
consecutive page numbers - the pages are not interleaved.

Nothing here says which object holds the wave path; that needs the pages
extracted and disassembled with the relocation records applied, and the
relocations are exactly what this header does not describe in the usual place.

## Plan for the machine with Windows 98

The decisive experiment is not more static analysis: it is watching the vendor
driver work.

1. Install Win98 on the CF-30 (or any machine with a 16-bit PCMCIA socket), then
   SCP95.DRV and the mixer from the original disk. PCMCIA support in 98 is good
   enough for this socket.
2. Play a WAV from the control panel, or simply click a system sound, while
   logging port writes.
3. What to log: every write to 0x330-0x33B, with the value and the order, plus
   reads of the status ports. That sequence *is* the answer - it is what we have
   been trying to reconstruct from disassembly for two days.
4. If no logger is available, the fallback is a minimal VxD that hooks the port
   I/O, or a hardware probe on the socket lines. Both are more work than the
   logger, so try the software route first.

Until that happens, the honest statement stays: PCM is not implemented, the
memory window is mapped and measured, and nothing we have tried makes the card
consume a stream.
