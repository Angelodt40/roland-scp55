# The mixer: what the vendor files say

Measured from the vendor disk (`scp-55.exe`, extracted with `unzip`), not from
guesswork. Nothing here is implemented in the driver yet: this file is the map
for whoever writes the ALSA mixer.

## `SCPMIXER.DLL` exports, complete list

    OPENSCP_MIXER / CLOSESCP_MIXER
    GETGSREVERB   SETGSREVERB
    GETGSCHORUS   SETGSCHORUS
    GETGSVOLUME   SETGSVOLUME
    GETGSPAN      SETGSPAN
    GETSCPDAATTN    SETSCPDAATTN      (DAC attenuation)
    GETSCPADGAIN    SETSCPADGAIN      (ADC / capture gain)
    GETSCPLINEMIXGAIN  SETSCPLINEMIXGAIN  (line mix)
    GETSCPGSMIXGAIN    SETSCPGSMIXGAIN    (synth mix)
    GETSCPMICMIXGAIN   SETSCPMICMIXGAIN   (mic mix)
    GETSCPINPUTSELECTOR SETSCPINPUTSELECTOR
    GETSCP_SW     SETSCP_SW

There are two separate worlds in that list, and they must not be confused:

- **GS parameters** - reverb, chorus, volume, pan - belong to the synthesiser
  and are almost certainly sent as GS SysEx over the MPU-401, not written to a
  register. They would work on any Sound Canvas.
- **The `SCP*` group** is the CS4231A codec around the synth: DAC attenuation,
  capture gain, line mix, synth mix, mic mix, input selector, and a switch.
  These are the ones an ALSA mixer should expose, and they map onto the CS4231
  mixer registers the codec answers at `0x338`/`0x339` (ID `0x8A` at index
  `0x0C`, already verified on the card).

## `SOUND.INI`, the stored defaults

    [SCP95.DRV]
    Wait=10
    Mixer_Control_Data0000=2          Mixer_Control_Data0001=0
    Mixer_Control_Data0100=0          Mixer_Control_Data0101=0
    Mixer_Control_Data0200=44800      Mixer_Control_Data0201=44800
    Mixer_Control_Data0300=61440      Mixer_Control_Data0301=61440
    Mixer_Control_Data0400=48896      Mixer_Control_Data0401=48896
    Mixer_Control_Data0500=0          Mixer_Control_Data0501=0
    Mixer_Control_Data0600=65535      Mixer_Control_Data0601=65535
    Mixer_Control_Data0700=3840       Mixer_Control_Data0701=3840
    Mixer_Control_Data0800=3840       Mixer_Control_Data0801=3840
    Mixer_Control_Data0900=3840       Mixer_Control_Data0901=3840
    Mixer_Control_Data1000=65535      Mixer_Control_Data1001=65535
    Mixer_Control_Data1100=0          Mixer_Control_Data1101=0
    Mixer_Control_Data1200=65535      Mixer_Control_Data1201=0
    Mixer_Control_Data1300=0 .. 2401=0
    Mixer_Control_Data1600=1          Mixer_Control_Data1601=0
    Mixer_Control_Data1900=1          Mixer_Control_Data1901=0

Twenty-five controls, each with a left and a right value, 16-bit. The values
that are not 0 or 65535 sit in the high byte - 3840 is 0x0F00, 61440 is 0xF000,
48896 is 0xBF00, 44800 is 0xAF00 - which is the shape of a CS4231 mixer value
where bits 7..4 carry the attenuation.

**What is not proven:** which control id is which name. The numbering of
`Mixer_Control_DataNN` is the mixer applet's, and matching it to the DLL
exports requires either disassembling `SCPMIXER.DLL`'s WEP table or trying one
control at a time against the codec registers. Treat the mapping above as a
hypothesis with a value table attached, not as a register list.

## What this means for the driver

An ALSA mixer is reachable without touching the wave engine: the codec already
answers, the register set is the CS4231's own, and the vendor's control names
tell us which functions a user expects (output volume, capture gain, input
selector, mic and synth mix levels). It is a separate piece of work from PCM
and it does not depend on the memory window.

## Dead ends recorded

- `SCP55.DRV` (the Windows 3.1 driver, 6 segments) contains **no direct port
  access at all**: it is a WEP and delegates to the system. It is not a source
  for the register map.
- The sixteen `mov [di+..]` in segment 8 of `SCP95.DRV` copy a parameter block
  into a structure; they are not frame writes.
- The three `rep movsw` at the top of segment 7 copy driver tables at init.

## The mixer writes real registers, but not the ones you hear (29/10)

The ALSA mixer in 0.3.0/0.3.1 exposes Master/Aux1/Aux2 playback, AD input,
line and mic capture, and a master mute switch, all on the CS4231 registers at
`0x338`/`0x339`. Verified in reading: `amixer 0` writes attenuation 3, `amixer`
3 writes 0, the switch sets and clears bit 7 on both DAC registers.

The scale is four steps, not eight. Writing each attenuation value to `0x06`
and reading it back:

| written | read back |
| --- | --- |
| `00` | `00` |
| `10` | `10` |
| `20` | `20` |
| `30` | `30` |
| `40` | `00` |
| `50` | `10` |
| `70` | `30` |

The register keeps two bits; the value wraps above `0x30`. So the honest range
is 0..3, which is what the driver now uses.

**And then the part that matters:** with `Master Playback Switch` off (DAC
registers at `0x88`/`0x80`) a chord sounded exactly as loud as with the switch
on. The mute does not silence the synth. So these registers are real and
writable, but the audible synth path does not pass through them - which also
means the four attenuation steps cannot be heard.

That is not a broken control, it is a control on the wrong path. What a Sound
Canvas actually uses for output level is the GS master volume, a SysEx message
(`F0 41 10 42 12 40 00 04 vv sum F7`), not a codec register.

### A test-hygiene lesson

`beep_on_probe` used to default to 1, so every `insmod` played a test phrase.
That silent beep sat under several A/B listening tests and made them ambiguous.
It is now opt-in (default 0) and writable at runtime.
