# Contributing

This driver exists because a 1995 card still works. Anything that keeps it
working on machines I do not own is welcome.

## Before you open a pull request

1. **Build it for the kernel you run.** `make` in the repository root; the DKMS
   path is what users get, so check it too if you touch `dkms.conf`.
2. **Try it on the hardware.** The card is old enough that a change which
   compiles cleanly can still wedge a machine — version 0.1.7 did exactly that
   with an unbounded register poll inside a workqueue. A change nobody has heard
   making sound is not ready.
3. **Say how you verified it** in the commit message and in the changelog:
   which machine, which kernel, what you listened to.

## Style

- Kernel C, tabs, 80 columns, comments that explain *why* a value is what it is.
  If you discovered a magic number by experiment, say so next to it.
- One idea per commit. If a commit needs the word "and" twice, split it.
- No new module parameters unless the behaviour is genuinely ambiguous; a
  parameter that exists because we are unsure is a bug we have not fixed yet.

## Releases

Versions are marked pre-release until the `.deb` is built and the driver has
been heard on real hardware. When you flip one to stable, write down what you
tested.

## Reporting results from other machines

Open an issue with: laptop model, kernel version, what `/proc/scp55` says, and
whether you heard the test phrase. Negative results are as useful as positive
ones — the documentation is built out of them.
