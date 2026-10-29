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
