# The memory window on the SCP-55

Everything below was measured on a Panasonic CF-30 (kernel 6.1.0-54-686-pae,
yenta_cardbus) with the card in its socket. Numbers are read from dmesg; nothing
here is inferred from another driver's source unless stated.

## What the socket gives you

- The CardBus bridge on this machine exposes exactly 1 MB of memory:
  `[mem 0xe8200000-0xe82fffff]`, and the first 64 KB is excluded by the memory
  probe. The free part is `0xe8210000-0xe82fffff`, 960 KB.
- The card's CIS asks for a 1 MB memory window. It does not fit, so
  `pcmcia_loop_config()` with `CONF_AUTO_SET_IOMEM` fails with -28 (ENOSPC).
  Use I/O only in the normal path and request the window yourself when you want
  to research: that is what `mem_probe` does.

## Three traps that cost a session each

1. In `pcmcia_request_window()` the field `res->end` is the SIZE to reserve, not
   the last address. The core does `res->end += res->start - 1` once the window
   is granted. It must also be a multiple of `s->map_size`: with size-1 (0xFFFF)
   the check `res->end & (s->map_size - 1)` fails and the call returns -EINVAL,
   which is the famous -22.
2. `MAP_ACTIVE` (0x01) must be set in `res->flags`. The core copies into the
   socket window only `res->flags & WIN_FLAGS_MAP` (0x63 = MAP_ATTRIB |
   MAP_16BIT | MAP_ACTIVE | MAP_USE_WAIT). Without it the window is reserved,
   mapped, and never enabled: every read returns 0xff, including attribute
   memory where the CIS obviously lives. Do not confuse `IORESOURCE_MEM_16BIT`
   (0x08) with `MAP_16BIT` (0x02): the former sits in WIN_FLAGS_REQ and means
   'window 1'.
3. `pcmcia_map_mem_page()` takes an address in the card's own memory space, and
   the card pages are 64 KB. Within a page it is the offset in the mapped area
   that moves the read. Mapping the window again without changing the offset
   reads the same bytes forever.

## What is behind the window

With `mem_probe=1` the attribute memory reads a clean CIS: the product string
decodes as "Roland" / "PCMCIA Sound Card" and the CISTPL_CFTABLE_ENTRY (0x1b)
tuples are visible with their I/O and memory descriptors.

The common memory reads 0x00 across all sixteen 64 KB pages, and it is not
addressed RAM:

| test | written | read back |
| --- | --- | --- |
| one byte at offset 0 with 8-bit writes | a5 5a 0f f0 ... | de f0 8e f0 00 f0 ... |
| eight words, 16-bit writes | a55a 0ff0 33cc ... def0 | def0 def0 def0 de00 de00 ... |
| one word at offset 0 | 1234 at 0x0000 | 1234 at 0x0000; 1200 at 0x0002, 0x0004,
  0x0006, 0x0008, 0x0100, 0x1000 and at page 1 |

So writes land in one place regardless of the address, and reads come back
either the last value written or a byte-lane shifted copy of it. That is not
RAM: it looks like a data port towards the wave engine - which fits the 1996
Windows driver, whose wave code streams samples through a mapped segment instead
of using DMA.

## How to reproduce

    insmod scp55.ko                       # normal: I/O only, synth and MIDI
    insmod scp55.ko mem_probe=1           # reserve the windows, dump CIS
    insmod scp55.ko mem_probe=2           # 8-bit write test
    insmod scp55.ko mem_probe=3           # 16-bit write test
    insmod scp55.ko mem_probe=4           # alias test across offsets and pages

Reserving the window changes the card's state: with it reserved, the signature
at base+5 no longer reads 0xaa and base+2 reads 0x80 instead of 0xa0. The synth
still plays, verified by ear. The signature also stops reading 0xaa as soon as
the driver writes the routing mode, which is documented behaviour.

## Still open

- What enables the wave engine, and whether that port needs a control write
  (the I/O pair base+2/base+3 reads 0xa0/0x00 before the window and 0x80/0x00
  after) or a specific pattern that switches the card out of synth-only mode.
- Whether the sixteen pages mean anything once the engine is running, or the
  whole 1 MB is one stream.
