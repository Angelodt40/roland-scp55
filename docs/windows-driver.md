# Notes on the original Windows drivers

The official Roland driver disk for the SCP-55 survives as a self-extracting
archive, and it is the closest thing to documentation this card has. This note
records what is inside it, because it answers questions the Linux driver had to
discover by experiment.

## The archive

`scp-55.exe`, 117638 bytes, is a **PKLITE self-extracting ZIP** (MZ stub,
"PKLITE Copr. 1990-92 PKWARE"). It can be unpacked without running it:

    unzip -o scp-55.exe -d scp55win

Contents (1995-1997):

    INSTALL.EXE      installer
    OEMSETUP.INF     Windows 3.1 setup
    Scp95a.inf       Windows 95 setup
    SOUND.INI        mixer initialisation values
    README.wri       original documentation (Windows Write format)
    SCP95.DRV        Windows 95 wave/midi driver
    SCP95.VXD        Windows 95 VxD
    SCP55.DRV        Windows 3.1 driver
    SCP55CNT.EXE     Windows 3.1 control panel
    SCPENA.EXE       Windows 3.1 enabler
    SCPMIXER.DLL     mixer

The README identifies the package as the **SCP-55B Windows 95 driver, version
4.02, dated 9 October 1996**, and notes that Windows 95 recognises the card when
it is inserted while running, without Card Services or socket services — the
same behaviour the Linux driver relies on through its PCMCIA modalias.

## What the .inf says about the hardware

`Scp95a.inf` is the authoritative statement of what the card needs:

    [SCP55_Device.FactDef]
    ConfigPriority=NORMAL
    IOConfig=330-33B
    IOConfig=201
    IRQConfig=5
    DMAConfig=

That is worth reading carefully:

- the I/O window is **0x330-0x33B, twelve ports**, not the two the Linux driver
  assumes. Our MPU-401 lives at 0x330/0x331, the CS4231A codec at 0x338/0x339;
  **0x33A and 0x33B are never touched by any Linux driver and their purpose is
  unknown.** Reading them with `dd if=/dev/port` on the test machine returns
  0x01 and 0x02
- a second I/O range at **0x201**, the classic gameport address
- the card declares **IRQ 5**. The Linux driver uses no interrupt at all: it
  polls the status register. If the MPU-401 receive path only latches data when
  interrupts are enabled, this is the missing piece for MIDI input, which has
  never delivered a byte
- no DMA channel, consistent with the codec being driven through I/O only

The device ID used for matching is `PCMCIA\ROLAND-PCMCIA_SOUND_CARD-D11B`.

## Mixer initialisation

`SOUND.INI` carries the values the Windows driver writes into the mixer,
grouped as `Mixer_Control_DataXXXX` where the first two digits look like a
control index and the last two the channel:

    Mixer_Control_Data0200=44800   (0xAF00)
    Mixer_Control_Data0300=61440   (0xF000)
    Mixer_Control_Data0400=48896   (0xBF00)
    Mixer_Control_Data0600=65535   (0xFFFF)
    Mixer_Control_Data0700=3840    (0x0F00)

The Linux driver knows only four codec registers (0x02-0x05, the input gains and
their mute bits) plus 0x06/0x07. This table is presumably the complete
initialisation, and decoding it is the most direct route to full mixer control
instead of the current unmute-only support.

## Reading the binaries

The port numbers do **not** appear as immediates: no `mov dx, 0x330` exists in
any of the four binaries, because Windows hands the driver its I/O base at run
time. Verified by scanning for the byte patterns `BA 30 03`, `BA 38 03`,
`BA 39 03` and friends in `SCP95.VXD`, `SCP55.DRV`, `SCPENA.EXE` and
`SCP55CNT.EXE`: zero hits, except one reference to 0x201 in `SCP55.DRV` at
offset 0x2af7.

So recovering the register sequences means tracing where `DX` is loaded from,
not grepping for constants. Practical route: disassemble with
`objdump -D -b binary -m i386 -M intel`, find the `out dx, al` (0xEE) and
`in al, dx` (0xEC) instructions, and work backwards through the code that
sets up `DX` — the enabler `SCPENA.EXE` is the smallest and most likely
place to start, since it only has to bring the card to life.

## Why this matters

Two claims in our README are now questionable and worth re-testing with this
information in hand:

- "MIDI input is not possible" — it may simply need interrupts, which the card
  declares and we never enable
- "the codec needs only an unmute" — the Windows driver writes a much larger
  table

Neither conclusion should be rewritten until it is tested; this note exists so
the next session starts from the vendor's own statements rather than from
guesses.
