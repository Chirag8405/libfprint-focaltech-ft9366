# Path B Integration Notes

This repository started as a standalone FocalTech FT9366 module, but the long-term shape should match `libfprint`'s in-tree driver layout.

## Confirmed Runtime State

On this Arch host, the runtime milestone has already moved past driver discovery:

- `pacman -Qi libfprint` reports `1.94.10-1`
- `/usr/lib/libfprint-2.so.2.0.0` contains built-in `focaltech_moc` strings
- recent `journalctl -u fprintd` output shows active `focaltech:fw9366` logs and repeated `Read chipid: 0x0`
- the blocker is no longer `No driver found for USB device 2808:A658`

Practical implication: Path A is not required to prove claim/discovery on this machine. The active blocker is protocol parity around chip ID and event/status handling.

## Target In-Tree Layout

Copy these files into an upstream `libfprint` checkout:

- `src/focaltech-ft9366.c` -> `libfprint/drivers/focaltech-ft9366.c`
- `src/focaltech-ft9366.h` -> `libfprint/drivers/focaltech-ft9366.h`
- `src/focaltech-usb.c` -> `libfprint/drivers/focaltech-usb.c`
- `src/focaltech-usb.h` -> `libfprint/drivers/focaltech-usb.h`
- `src/focaltech-crypto.c` -> `libfprint/drivers/focaltech-crypto.c`
- `src/focaltech-crypto.h` -> `libfprint/drivers/focaltech-crypto.h`

## Required Upstream File Edits

In `libfprint/drivers/meson.build`, add:

```meson
  'focaltech-ft9366.c',
  'focaltech-usb.c',
  'focaltech-crypto.c',
```

In `libfprint/drivers/all-drivers.h`, add:

```c
extern const FpDeviceClass fpi_device_focaltech_ft9366;
```

In `libfprint/drivers/driver-list.inc`, add:

```c
&fpi_device_focaltech_ft9366,
```

## Local Driver Notes

The current `src/focaltech-ft9366.c` has already been nudged toward in-tree use:

- it prefers `fpi_device_get_usb_device()` when that internal helper is available, while keeping a standalone fallback
- `open` and `close` can report completion back to libfprint when the internal callbacks are available
- stub `enroll` and `verify` paths fail cleanly with `FP_DEVICE_ERROR_NOT_SUPPORTED`

That keeps the standalone skeleton and the future in-tree port closer to the same code.
