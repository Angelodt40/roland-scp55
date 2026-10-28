#!/bin/bash
# make-deb.sh — builds a DKMS .deb without debhelper.
# Usage: bash packaging/make-deb.sh [output-dir]
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/dist}"
VER="$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$ROOT/dkms.conf")"
PKG="scp55-dkms"
ARCH="all"
STAGE="$(mktemp -d)"

[ -n "$VER" ] || { echo "cannot read PACKAGE_VERSION from dkms.conf" >&2; exit 1; }

echo "==> $PKG $VER"
mkdir -p "$STAGE/DEBIAN" \
         "$STAGE/usr/src/scp55-$VER" \
         "$STAGE/usr/share/doc/$PKG" \
         "$STAGE/usr/lib/udev/rules.d"

cp -r "$ROOT/src" "$ROOT/dkms.conf" "$STAGE/usr/src/scp55-$VER/"
rm -f "$STAGE/usr/src/scp55-$VER"/src/*.o \
      "$STAGE/usr/src/scp55-$VER"/src/*.ko \
      "$STAGE/usr/src/scp55-$VER"/src/*.mod \
      "$STAGE/usr/src/scp55-$VER"/src/*.mod.c \
      "$STAGE/usr/src/scp55-$VER"/src/.*.cmd \
      "$STAGE/usr/src/scp55-$VER"/src/Module.symvers \
      "$STAGE/usr/src/scp55-$VER"/src/modules.order 2>/dev/null || true
cp "$ROOT/README.md" "$ROOT/LICENSE" "$STAGE/usr/share/doc/$PKG/"
cp "$ROOT/udev/99-scp55.rules" "$STAGE/usr/lib/udev/rules.d/99-scp55.rules"

cat > "$STAGE/DEBIAN/control" <<EOF
Package: $PKG
Version: $VER-1
Section: kernel
Priority: optional
Architecture: $ARCH
Depends: dkms (>= 2.1.0), make | build-essential, gcc
Maintainer: Angelo Di Tomasso <angelo.dt40@gmail.com>
Homepage: https://github.com/Angelodt40/roland-scp55
Description: Linux driver for the Roland SCP-55 Sound Canvas PC Card
 Programs the PCMCIA socket so the MPU-401 UART answers at I/O base 0x330,
 un-mutes the CS4231A codec the synth output runs through, and exposes the
 card as an ALSA rawmidi device, so it appears as an external MIDI synth.
 .
 Built through DKMS, so the module is rebuilt automatically on kernel updates.
EOF

cat > "$STAGE/DEBIAN/postinst" <<EOF
#!/bin/sh
set -e
export PATH=/usr/sbin:/sbin:\$PATH
if ! command -v dkms >/dev/null 2>&1; then
  echo "scp55: dkms not found. Install it (apt install dkms) and run:" >&2
  echo "  dpkg --configure -a" >&2
  exit 1
fi
dkms add -m scp55 -v $VER 2>/dev/null || true
dkms build -m scp55 -v $VER
dkms install -m scp55 -v $VER --force
depmod -a
modprobe scp55 2>/dev/null || true
if [ -e /proc/scp55 ] && [ "\$1" = "configure" ]; then
  echo "scp55 loaded. Test phrase:  echo 1 > /proc/scp55"
fi
exit 0
EOF

cat > "$STAGE/DEBIAN/prerm" <<EOF
#!/bin/sh
set -e
if command -v dkms >/dev/null 2>&1; then
  dkms remove -m scp55 -v $VER --all 2>/dev/null || true
fi
exit 0
EOF

chmod 0755 "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/prerm"
mkdir -p "$OUT"
dpkg-deb --root-owner-group --build "$STAGE" "$OUT/${PKG}_${VER}-1_${ARCH}.deb" >/dev/null
rm -rf "$STAGE"
ls -l "$OUT/${PKG}_${VER}-1_${ARCH}.deb"
