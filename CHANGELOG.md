# Changelog

All notable changes to this project. Versions correspond to git tags.

## 0.1.0 — 2026-10-28

First public release.

- Kernel module for the Roland SCP-55 PC Card: programs the PCMCIA socket so
  the MPU-401 UART answers at I/O base 0x330, and un-mutes the CS4231A codec the
  synth output runs through
- ALSA rawmidi output device, exposed as the sequencer port "Roland SCP-55"
- `/proc/scp55` status and test phrase, `/proc/scp55mix` codec register access
- DKMS build description, udev rule, `install.sh`
- Tested on a Panasonic CF-30, MX-23.6, kernel 6.1.0-54-686-pae
