#!/bin/bash
# scp55-play.sh — suona uno o piu' file MIDI sulla Roland SCP-55
# Uso: scp55-play.sh file.mid [altro.mid ...]
set -u

if [ $# -lt 1 ]; then
  echo "Uso: $0 file.mid [altro.mid ...]"
  exit 1
fi

PORT="$(aplaymidi -l 2>/dev/null | awk '/Roland SCP-55/ {print $1; exit}')"
if [ -z "$PORT" ]; then
  echo "Porta MIDI della SCP-55 non trovata."
  echo "Controlla che il modulo sia caricato:  lsmod | grep scp55"
  exit 1
fi

[ -w /proc/scp55mix ] && echo i > /proc/scp55mix

for f in "$@"; do
  if [ ! -f "$f" ]; then
    echo "!! file non trovato: $f"
    continue
  fi
  echo ">> $f   (porta $PORT)"
  aplaymidi -p "$PORT" "$f"
  rc=$?
  [ $rc -ne 0 ] && echo "!! errore (rc=$rc) su $f"
done
echo "Fine."
