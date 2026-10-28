#!/bin/sh
# scp55 installer — builds and installs the Roland SCP-55 driver.
# DKMS is used when available so the module survives kernel updates,
# otherwise the module is built and installed directly for the running kernel.
#
# Usage: sudo ./install.sh
set -eu

MOD=scp55
VER=0.1.0
SRC=$(cd "$(dirname "$0")" && pwd)
KVER=$(uname -r)

if [ "$(id -u)" -ne 0 ]; then
  echo "Run this as root:  sudo $0" >&2
  exit 1
fi

if [ ! -d "/lib/modules/$KVER/build" ]; then
  echo "Kernel headers for $KVER are missing." >&2
  echo "Debian/Ubuntu:  sudo apt install linux-headers-$KVER" >&2
  echo "Fedora:         sudo dnf install kernel-devel-$KVER" >&2
  exit 1
fi

if command -v dkms >/dev/null 2>&1; then
  echo "==> Installing through DKMS ($MOD $VER)"
  rm -rf "/usr/src/$MOD-$VER"
  mkdir -p "/usr/src/$MOD-$VER"
  cp -r "$SRC/src" "$SRC/dkms.conf" "/usr/src/$MOD-$VER/"
  [ -f "$SRC/LICENSE" ] && cp "$SRC/LICENSE" "/usr/src/$MOD-$VER/"
  dkms add -m "$MOD" -v "$VER" 2>/dev/null || true
  dkms build -m "$MOD" -v "$VER"
  dkms install -m "$MOD" -v "$VER" --force
else
  echo "==> DKMS not found, building directly for $KVER"
  make -C "$SRC/src"
  install -Dm644 "$SRC/src/$MOD.ko" "/lib/modules/$KVER/extra/$MOD.ko"
  depmod -a
fi

if [ -f "$SRC/udev/99-scp55.rules" ]; then
  install -Dm644 "$SRC/udev/99-scp55.rules" /etc/udev/rules.d/99-scp55.rules
  udevadm control --reload 2>/dev/null || true
fi

modprobe "$MOD" 2>/dev/null || true

if [ -e /proc/scp55 ]; then
  echo "==> Module loaded. Sending the test phrase — you should hear a short riff."
  echo 1 > /proc/scp55 2>/dev/null || true
  echo
  echo "Port:"
  aplaymidi -l 2>/dev/null | grep -i "SCP-55" || echo "  (aplaymidi not installed: apt install alsa-utils)"
  echo
  echo "Play a file:  aplaymidi -p <port> file.mid"
  echo "If it is silent:  echo i > /proc/scp55mix"
else
  echo "!! /proc/scp55 is missing: the module did not load. Check: dmesg | tail -20" >&2
  exit 1
fi
