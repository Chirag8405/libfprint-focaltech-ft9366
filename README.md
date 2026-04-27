# libfprint-focaltech-ft9366

Open-source driver work for the FocalTech FT9366 fingerprint sensor (USB 2808:a658), targeting ASUS Vivobook-class devices that currently fail with chipid `0x0` paths.

License: LGPL-2.1-or-later

## Background
See [research/BACKGROUND.md](research/BACKGROUND.md) for project context and [research/PROTOCOL.md](research/PROTOCOL.md) for live reverse-engineering notes.

## Hardware Compatibility

| Model | USB ID | bcdDevice | Status |
|---|---|---|---|
| ASUS Vivobook S14 OLED S5402 | `2808:a658` | `1.12` | In progress (protocol discovery + skeleton) |

## Current Status

- Protocol discovery artifacts are documented.
- Binary symbol/disassembly baseline is documented.
- A clean-room source skeleton exists (`src/`) for USB + crypto + FT9366 state flow.
- Runtime confirmation on 2026-04-27 shows this Arch host is already past `No driver found`: `fprintd` is entering a `focaltech:fw9366` path and repeatedly reading chipid `0x0`.
- Full enroll/verify is not complete until the chipid command tuple, event-status handling, and key schedule are confirmed from captures.
- The repo is still buildable as a standalone module, but its code should now be treated as Path B oriented: ready to drop into `libfprint/drivers/` when protocol behavior is stable.

## fprintd Verify Success Rate

Current measured success rate after installation:
- Device claim/discovery is confirmed, but enroll/verify is still blocked by `chipid 0x0` and abnormal interrupt status handling.

This section will be updated with real numbers after end-to-end testing (`fprintd-enroll`, `fprintd-verify`, suspend/resume).

## Build

### Arch Linux

```bash
sudo pacman -S base-devel meson ninja glib2 libgusb nss libfprint python
meson setup build
meson compile -C build
sudo meson install -C build
```

### Ubuntu / Debian

```bash
sudo apt update
sudo apt install -y build-essential meson ninja-build libglib2.0-dev \
  libgusb-dev libnss3-dev libfprint-2-dev python3
meson setup build
meson compile -C build
sudo meson install -C build
```

### Fedora

```bash
sudo dnf install -y gcc meson ninja-build glib2-devel libgusb-devel \
  nss-devel libfprint-devel python3
meson setup build
meson compile -C build
sudo meson install -C build
```

## Installation Notes

- Shared object installs to `libfprint-2/tod-1` under your `libdir`.
- udev rules are installed to your configured udev rules directory.
- For current host-state confirmation, run `tools/check-runtime-state.sh`.
- You may need to restart fprintd:

```bash
sudo systemctl restart fprintd
```

## Contributing Captures

Contributions from additional laptops and firmware variants are essential.

1. Capture a session:

```bash
sudo python tools/capture.py --bus 3 --output research/captures/session.pcap \
  --restart-fprintd --enroll-user "$USER"
```

2. Analyze and export control tuples:

```bash
python tools/analyze_pcap.py research/captures/session.pcap \
  --markdown research/captures/session_commands.md
```

3. Open a PR with:
- laptop model
- USB IDs (`lsusb` line)
- firmware/release if known
- command tuple markdown export

## Project Layout

```text
src/                 Core FT9366 transport + crypto skeleton
research/            Reverse-engineering notes and protocol logs
tools/               Capture and pcap analysis helpers
udev/                Device access and power rules
packaging/           Distribution packaging (AUR template)
```

## Short-Term Roadmap

1. Capture full enroll + verify USB sessions from native hardware.
2. Confirm chipid command tuple and expected register response shape.
3. Confirm AES key derivation/import sequence around PK11 calls.
4. Map interrupt payloads to state machine transitions.
5. Integrate with libfprint operation lifecycle for enroll/verify/list/delete.
