# FT9366 Protocol Notes (USB 2808:a658)

Status: in progress
Last updated: 2026-04-27

## Scope
This document tracks concrete protocol findings for the FocalTech FT9366 sensor behind Realtek bridge (USB 2808:a658), with emphasis on the chipid handshake and encrypted command channel.

## Method A: usbmon capture

### Attempted in this environment
- Device is present: `ID 2808:a658 Realtek USB2.0 Finger Print Bridge FocalTech Fingerprint Device`.
- usbmon capture is now working on the target host.
- Capture files and detailed notes are recorded in:
  - `research/captures/2026-04-27-usbmon-notes.md`
  - `/home/chirag/captures/session_1616.pcap`
  - `/home/chirag/captures/session_timeout_162208.pcap`
  - `/home/chirag/captures/session_postreset_162749.pcap`
  - `/home/chirag/captures/session_rebind_162930.pcap`

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

### Confirmed early protocol sequence

The first confirmed non-descriptor command sent to the fingerprint sensor is:

```text
02 00 01 a5 a4
```

Observed in captures:

- `session_1616.pcap`: frames `1455` -> `1457`
- `session_postreset_162749.pcap`: frames `1037` -> `1039`
- `session_rebind_162930.pcap`: frames `995` -> `997`

Minimal sequence:

```text
CONTROL OUT ep0      SET_CONFIGURATION (wValue=1)
BULK OUT   ep0x01    02 00 01 a5 a4
BULK IN    ep0x82    host waits for response
```

Current interpretation:

- treat `02 00 01 a5 a4` as `CMD_INIT` / `CMD_WAKE`
- it is sent before the chipid read path
- the host then arms bulk IN on `0x82`
- in all current April 27 captures, the device never returns the expected first response packet

Practical implication:

- the current failure is consistent with the sensor not completing its wake/init handshake
- because the first response never arrives, later chipid/event traffic is never reached in these sessions

### Reset experiments run on 2026-04-27

Two reset strategies were tested before the fresh enroll attempt:

1. USB authorization toggle on `/sys/bus/usb/devices/3-8/authorized`
2. USB unbind/rebind via `/sys/bus/usb/drivers/usb/{unbind,bind}`

Observed result:

- both resets restored the device cleanly in `lsusb`
- both were followed immediately by a single `fprintd-enroll`
- both still reproduced the same early sequence:
  - `CMD_INIT/CMD_WAKE` bulk OUT
  - bulk IN wait on `0x82`
  - `failed to claim device: Timeout was reached`

This means the reset changes were not sufficient to reproduce the richer April 26 probe state.

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
CMD_INIT / CMD_WAKE:
  Transport: bulk OUT
  Endpoint: 0x01
  Request bytes: [02 00 01 a5 a4]
  Expected next step: bulk IN response on endpoint 0x82
  Current observed result: host waits on 0x82 and times out

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
1. Reproduce the April 26 cold-start environment: one fresh boot, usbmon started before any fingerprint access, one single enroll attempt.
2. Capture the first successful response after `CMD_INIT / CMD_WAKE` on bulk IN `0x82`.
3. Map the command immediately following the first successful wake response and identify where chipid traffic begins.
4. Locate key material/derivation around `PK11_ImportSymKey` in decompiler (Ghidra/r2) once tools are available.
5. Build driver state machine around: open -> init/wake -> chipid -> crypto init -> event loop -> enroll/verify.
