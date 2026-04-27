#!/usr/bin/env bash

set -euo pipefail

libfprint_so="/usr/lib/libfprint-2.so.2.0.0"

if [[ ! -r "$libfprint_so" ]]; then
  echo "Missing $libfprint_so" >&2
  exit 1
fi

echo "== libfprint package =="
pacman -Qi libfprint | sed -n '1,20p'
echo

echo "== libfprint strings (driver/TOD markers) =="
strings "$libfprint_so" | grep -i -E 'focaltech_moc|tod|plugin|dlopen|No driver found' | head -n 40 || true
echo

echo "== installed focaltech modules =="
find /usr/lib/libfprint-2 -maxdepth 3 \( -name 'libfprint-focaltech-ft9366.so*' -o -type d -name 'tod-1' -o -type d -name 'drivers' \) 2>/dev/null | sort
echo

echo "== recent fprintd journal evidence =="
journalctl -u fprintd --since '2026-04-26 00:00:00' --no-pager \
  | grep -E 'No driver found|focaltech:fw9366|Read chipid|abnormality interrup flag' \
  | tail -n 120 || true
echo

echo "== summary =="
echo "Look for focaltech:fw9366 / Read chipid lines to confirm the device is claimed."
echo "If No driver found is absent and chipid reads 0x0, discovery succeeded and protocol bring-up is the remaining blocker."
