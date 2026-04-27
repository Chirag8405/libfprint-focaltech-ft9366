# FT9366 Protocol Notes (USB 2808:a658)

Status: in progress
Last updated: 2026-04-26

## Scope
This document tracks concrete protocol findings for the FocalTech FT9366 sensor behind Realtek bridge (USB 2808:a658), with emphasis on the chipid handshake and encrypted command channel.

## Method A: usbmon capture

### Attempted in this environment
- Device is present: `ID 2808:a658 Realtek USB2.0 Finger Print Bridge FocalTech Fingerprint Device`.
- Capture was blocked in this environment due to:
  - `tcpdump` missing
  - non-interactive sudo not available (`sudo -n modprobe usbmon` failed)
  - `/sys/kernel/debug/usb/usbmon` not visible

### Required on target host (interactive)
Run on the hardware owner machine with sudo and tcpdump:

```bash
sudo pacman -S tcpdump wireshark-qt
sudo modprobe usbmon
sudo tcpdump -i usbmon3 -w focaltech_session.pcap &
sudo systemctl restart fprintd
fprintd-enroll -f right-index-finger "$USER"
kill %1
```

Wireshark filter:

```text
usb.idVendor == 0x2808
```

## Method B/C: binary and symbol analysis (completed)

Binary analyzed:
- `~/focaltech-ft9366-arch-shim/libfprint-2.so.2.0.0`

### Key dynamic imports confirmed
- USB transport:
  - `g_usb_device_control_transfer`
  - `g_usb_device_bulk_transfer`
  - `g_usb_device_interrupt_transfer`
- NSS/PK11 crypto:
  - `NSS_NoDB_Init`
  - `PK11_ImportSymKey`
  - `PK11_CreateContextBySymKey`
  - `PK11_ParamFromIV`
  - `PK11_CipherOp`

### High-value symbols and addresses
From `readelf -Ws` and `nm -a`:

| Symbol | Address | Size | Note |
|---|---:|---:|---|
| `fw9366_query_event_status` | `0x151822` | `3641` | Interrupt/event handling core |
| `fw9366_probe_id` | `0x152975` | (local) | Early device probing path |
| `_Z17fw9366_chipid_getv` | `0x154955` | `47` | Chipid getter wrapper |
| `_Z32ft_feature_devinit_JudgeByChipIdPh` | `0x1535ad` | `100` | Device-init chipid gate |

### Disassembly findings
`_Z17fw9366_chipid_getv` at `0x154955`:
- Loads constant register address `0x1a8b`
- Calls `_Z16fw9366_sram_readt` at `0x1663d0`
- Returns the value read from SRAM/register path

Implication:
- Chipid retrieval is not a simple constant check. It depends on a lower-level register/SRAM read path that eventually relies on valid USB transaction state.

`fw9366_query_event_status` at `0x151822`:
- Calls local helper chain, including `_Z26fw9366_check_communicationh`
- No direct calls to `g_usb_device_control_transfer` inside this wrapper
- Event polling likely layered above lower-level USB helpers

### USB control transfer location in binary
Observed `g_usb_device_control_transfer@plt` call site in libfprint helper path:
- `fpi_usb_transfer_submit_sync+0x160` contains call to control transfer
- Address of call site: `0x562e8`

This supports the model that fw9366 logic uses shared transfer wrappers rather than direct libgusb calls in every function.

## Strings evidence (selected)
Searches required by PRD produced very large result sets. High-value findings include:
- `FW9366_REG`
- `fw9366_context_t`
- `fpi_device_focaltech_init`
- `got device id fw9366`
- `rts5811_method_init`
- command/transport diagnostics such as:
  - `alloc_send_cmd*_transfer`
  - `usb_control_transfer`
  - `usb_bulk_*_transfer`
  - response validation errors (`expected response`, `CMD response too short`)

## Additional upstream reality check
A current upstream `libfprint` checkout contains `drivers/focaltech_moc/`.
- VID `0x2808` support exists for several PIDs.
- PID `0xa658` is not present in that driver id table.
- `0x2808:0xa658` appears in `fprint-list-udev-hwdb.c` only.

Practical implication:
- There is likely reusable FocalTech MoC architecture upstream, but this specific device still needs protocol work and id-table support.

## Command/response table template (to fill after usbmon)

```text
CMD_READ_CHIPID:
  bmRequestType: 0xC0
  bRequest: 0x??
  wValue: 0x????
  wIndex: 0x????
  wLength: 4
  Response (expected): [93 66 00 00] or equivalent endian form
  Response (broken shim): [00 00 00 00]
```

## Next concrete actions
1. Capture a full enroll session on target host with usbmon and export control transfer tuples.
2. Map the transfer tuple used before `_Z17fw9366_chipid_getv` success path.
3. Locate key material/derivation around `PK11_ImportSymKey` in decompiler (Ghidra/r2) once tools are available.
4. Build driver state machine around: open -> chipid -> crypto init -> event loop -> enroll/verify.
