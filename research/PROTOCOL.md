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

## Update: rts5811_init_chip traced (2026-09-16)

Status: CONFIRMED (static disassembly), tested against real hardware (live session)

### Wake sequence confirmed working at the wire level
Standalone libusb test tool: `tools/rts5811_wake_test.c`

- Bulk OUT `4c 5a 01 00` on EP 0x01 -> Bulk IN 4 bytes on EP 0x82.
- CONFIRMED: real hardware responds immediately and consistently with `04 00 00 04` on every attempt (10/10 in testing).
- This is a real, reproducible response, in contrast to total silence previously observed for the generic `0xa5` CMD_INIT sent alone.
- Decode logic for the response (traced from `_Z25ft_tell_mcu_capture_starth`): resp[2]==0x02 -> retry (loop); else -> proceed. Our `04 00 00 04` response decodes to "proceed" (resp[3]==0x04 branch), confirmed via corrected disassembly reading (see below -- initial reading of the branch direction was backwards and has been corrected).

### rts5811_init_chip traced -- NOT a separate protocol

`_ZL17rts5811_init_chipP19rts5811_dev_info_st` (address `0x14d471`):

```
rts5811_init_chip(dev_info_st *info):
  if (info != NULL):
    info->field_0x0 = 0x40
    info->field_0x2 = 0x50
    info->field_0x4 = 0x9366   ; chip id literal
  call usb_fw9366_init_chip()  ; <-- SAME function already traced (SensorReset -> sleep(100ms) -> fw9366_init_chip)
  return 0
```

CONFIRMED: `rts5811_init_chip` is a thin wrapper that just stashes some static device-info fields (0x40, 0x50, chip id 0x9366) and then calls the exact same `usb_fw9366_init_chip()` path already traced. It is NOT a different/superset protocol. The earlier working theory ("rts5811_init_chip might use a distinct command set") is now RULED OUT.

### Result of testing full precondition sequence against real hardware

Sequence tested: wake ping (`4c 5a 01 00`, proceed confirmed) -> sleep(10ms) -> write `44 80 00 00` -> sleep(30ms) -> sleep(100ms) -> generic FocalTech CMD_INIT (`02 00 01 a5 a4`).

Result: CMD_INIT still times out with zero response (`LIBUSB_ERROR_TIMEOUT`), even after the full confirmed-working wake sequence.

### Open question (not yet resolved)

Since `rts5811_init_chip` -> `usb_fw9366_init_chip` -> `fw9366_init_chip()` is the one and only code path (no alternate RTS5811-specific protocol exists), the real next step is to disassemble `fw9366_init_chip()` itself (address `0x15265b`) directly to find the actual command bytes it sends after `SensorReset()` returns -- rather than assuming it is the same generic `0xa5` envelope that mainline's `focaltech_moc` driver uses for native-USB sibling chips. That assumption has not been directly verified from disassembly and may be wrong.

## Update: fw9366_init_chip traced -- generic 0xa5 envelope conclusively ruled out (2026-09-16)

Status: CONFIRMED (static disassembly)

`fw9366_init_chip()` (address `0x15265b`) does:
1. Retry loop calling `ft_tell_mcu_capture_start(1)` (same wake ping already confirmed working) until it returns != 1.
2. `fw9366_Get_OTP_Info(NULL, 0)`
3. `fw9366_get_SMIC_IC_flag()`
4. `fw9366_init_flag()`
5. `fw9366_intflag_clear(0xffff)`
6. `fw9366_cfg_init()`
7. `fw9366_Update_Base()`
8. `fw9366_fdt_auto_start(1)`
9. Direct writes: `REG9366[0x88] = 0`, `REG9366[0x8a] = 0`
10. `fw9366_poa_send_para(1)`

CONFIRMED: there is no call anywhere in this function (or in `rts5811_init_chip`, which just wraps it) to the generic FocalTech command envelope (`focaltech_moc_compose_cmd`-style magic/len/code/bcc framing, e.g. the `0xa5` CMD_INIT). The working theory from the previous session -- that mainline's generic FocalTech protocol is the wrong path for this RTS5811-bridged chip -- is now CONFIRMED, not just suspected. Do not spend further time trying variations of the `0xa5`-style envelope; it is not part of this chip's real protocol.

## Update: fw9366_poa_send_para inspected -- payload depends on unreplicated prior state (2026-09-16)

Status: CONFIRMED (static disassembly), NOT tested against hardware (would be meaningless without prior stages)

`fw9366_poa_send_para` is the first function in the `fw9366_init_chip` chain confirmed to call `ff_spi_write_then_read_buf_rts` (the same low-level SPI-bridge transfer helper already used for the wake ping). However, its outgoing payload buffer is assembled from live global state, not constants:
- Bytes copied from `REG9366` global struct at offsets including `0x87`, `0x89`, `0xa6`, `0xa8`, `0xaa`, `0xac`, `0xba`, `0xbc`, `0xbe`, `0xc0`
- Bytes copied from `Fw9366_cfg` global struct (offset `0xc`)
- A byte from global `smic_flag`

These globals are populated by the FOUR functions that run before `poa_send_para` in the real sequence: `fw9366_cfg_init`, `fw9366_Update_Base`, `fw9366_get_SMIC_IC_flag`, `fw9366_fdt_auto_start`. None of these four has been traced yet. Sending `poa_send_para`'s command in isolation (without replicating those four first) would send a garbage/zeroed payload and any hardware response (or lack of one) would not be meaningful -- explicitly NOT tested for this reason.

## Honest scope assessment (2026-09-16)

The real FT9366/RTS5811 init protocol is a genuine multi-stage sequence, not a single command. Full replication requires tracing at minimum:
- `fw9366_cfg_init` (~456 bytes disassembled)
- `fw9366_Update_Base` (~601 bytes)
- `fw9366_get_SMIC_IC_flag` (~628 bytes)
- `fw9366_fdt_auto_start` (~863 bytes)
- `fw9366_poa_send_para` (~733 bytes, already partially inspected above)

This likely also requires understanding the large embedded calibration/config binary blob visible via `strings` on the same .so (see earlier `binary_analysis.md` notes) since `fw9366_cfg_init`/`fw9366_Update_Base` are the most likely consumers of that data. This is realistically multiple more hours of focused disassembly work, not a same-session fix. Confirmed working building blocks so far (safe to build on in a future session): the wake ping sequence and its response-decode logic (see `tools/rts5811_wake_test.c`), and the definitive ruling-out of the generic FocalTech `0xa5` envelope for this specific chip variant.

## Update: fw9366_cfg_init traced -- no USB traffic, pure local config (2026-09-16)

Status: CONFIRMED (static disassembly). No hardware test possible/meaningful for this function alone (it sends nothing).

`fw9366_cfg_init()` (address `0x155566`, ~456 bytes disassembled including debug logging) performs **zero USB communication**. It only writes constants into the global `Fw9366_cfg` struct (base `0x30ded60`). Full decoded layout:

```
Fw9366_cfg[0x0] = 0x78
Fw9366_cfg[0x1] = 0x01
Fw9366_cfg[0x2] = 0x01
Fw9366_cfg[0x3] = 0x3c
Fw9366_cfg[0x4] = 0xc8
  ; branch reads Fw9366_cfg[0x2], which was just set to 0x01 above, so the
  ; "true" branch always executes in practice (the else branch at the
  ; disassembly level is statically dead given this code path):
Fw9366_cfg[0x5] = 0x04
Fw9366_cfg[0x6] = 0x04
Fw9366_cfg[0x7] = 0x32
Fw9366_cfg[0x8] = 0x2d
Fw9366_cfg[0x9] = 0x01
Fw9366_cfg[0xa] = 0x02
  ; only real data-dependent branch in this function:
if (smic_flag == 0xaa):
  Fw9366_cfg[0xc..0xd] (u16, LE) = 0x0096
else:
  Fw9366_cfg[0xc..0xd] (u16, LE) = 0x00c8
Fw9366_cfg[0xe] = 0x02
Fw9366_cfg[0xf] = 0x32
Fw9366_cfg[0x10] = 0x05
Fw9366_cfg[0x11] = 0x08
```

### Answers to open questions

- **Fixed sequence vs. calibration blob:** Fixed. This function does not read the large embedded binary blob seen in earlier `strings` analysis -- that blob (if used at all) must be consumed by a different function (`fw9366_Update_Base` and/or `fw9366_fdt_auto_start`, not yet traced).
- **Device-specific vs. generic:** Generic. Every byte here is a hardcoded constant except the one `smic_flag`-dependent field, and `smic_flag` reads as a foundry/IC-variant selector (SMIC = a chip foundry), not a per-unit serial or calibration value. This output should be identical for every FT9366 unit of the same foundry variant -- safe to hardcode/reuse/share.
- **Dependency on `rts5811_init_chip`:** None. `rts5811_init_chip` only writes to a separate `rts5811_dev_info_st` struct (`0x40`/`0x50`/`0x9366`), which `cfg_init` never reads. The only real dependency is on `smic_flag`, set by `fw9366_get_SMIC_IC_flag()` (the function called immediately before `cfg_init` in `fw9366_init_chip`'s real sequence) -- not yet traced. Its value is currently unknown, so the branch outcome above is undetermined until that function is traced.

### Test tool status

`tools/rts5811_wake_test.c` extended to compute and print the `Fw9366_cfg` struct locally (both branches, since `smic_flag` is not yet known). No new bytes are sent to hardware by this step -- there is nothing to test yet, since `cfg_init` itself never touches the USB device. This is local scaffolding for the later `poa_send_para` step, not a hardware result.

### Next concrete action
Trace `fw9366_get_SMIC_IC_flag()` (~628 bytes) next, both to resolve the `smic_flag` branch above and because it is the next function in the real call order (`fw9366_init_chip` calls it before `cfg_init`). Likely candidate for the first *real* USB traffic beyond the wake ping, since "get flag" implies reading something back from the device/OTP.

## Update: fw9366_get_SMIC_IC_flag traced and CONFIRMED against real hardware (2026-09-16)

Status: CONFIRMED (static disassembly AND live hardware test, clean single-attempt result, no ambiguity)

### fw9366_get_SMIC_IC_flag (address 0x1557cc)

Not a stub -- this is a real hardware read, distinct from the wake ping. Logic:

```
smic_flag = 0x00  ; default
for attempt in 0..9 (up to 10 tries, no sleep between attempts in traced code):
  val = fw9366_sfr_read(0x9b)
  shifted = val >> 2   ; zero-extended byte, then shift -- confirmed via movzx+sar,
                        ; sar==shr here since the zero-extended value is always 0-255
  if shifted == 0x13: smic_flag = 0xaa; break
  elif shifted == 0x00: smic_flag = 0x00; break
  else: retry
```

### fw9366_sfr_read(reg) (address 0x165f86) -- NEW confirmed command, distinct framing from the wake ping

Transport: `ff_spi_sfr_write_then_read_buf` (address 0x153a4d) -- structurally identical dispatch
to the wake ping's `ff_spi_write_then_read_buf_rts` (same BusType check, same underlying
`User_TL_Transmit_N_Byte` write(type=2)/read(type=1) pair, i.e. same bulk EP 0x01 OUT / EP 0x82 IN
transport), but different buffer framing:

```text
SFR_READ(reg):
  Bulk OUT ep0x01: [0x08, 0xf7, reg, 0x00, 0x00]   (5 bytes)
  Bulk IN  ep0x82: 1 byte response
  return response byte
```

### Live hardware test result (tools/rts5811_wake_test.c, real device)

```
sfr_read(0x9b) attempt 1:
  sent: 08 f7 9b 00 00
  recv: 00                (1 byte, immediate, no retry needed)
  raw=0x00  (raw>>2)=0x0
  -> smic_flag = 0x00
```

CONFIRMED for this physical unit: `smic_flag = 0x00`. This resolves the previously-undetermined branch in `fw9366_cfg_init`: `Fw9366_cfg[0xc..0xd] = 0x00c8` (not `0x0096`).

### Unit-specific vs. generic

`fw9366_sfr_read` genuinely reads live silicon (a real register on the actual chip) -- not host-computed. However the specific *value* it decodes to (0x00 vs 0x13) is a foundry/mask-revision identifier, not a per-device serial or calibration constant -- it should read the same for every unit fabbed in the same batch/foundry, not just this one laptop. Safe to treat as effectively generic for "FT9366 units from the same foundry as mine," but it IS a real read, not an assumption -- flagging the distinction as asked.

### Call order confirmed
`fw9366_get_SMIC_IC_flag()` runs BEFORE `fw9366_cfg_init()` in `fw9366_init_chip`'s real sequence (already established in the earlier `fw9366_init_chip` trace), and does NOT run as part of `rts5811_init_chip` (which only touches the separate `rts5811_dev_info_st` struct, unrelated). No dependency in the other direction: `cfg_init` depends on `get_SMIC_IC_flag`'s output (`smic_flag`), not vice versa.

### Running tally of confirmed-working real commands
1. Wake ping: `4c 5a 01 00` -> `04 00 00 04` (proceed)
2. SFR read: `08 f7 <reg> 00 00` -> 1 byte (tested with reg=0x9b -> `0x00`)

### Next concrete action
Trace `fw9366_init_flag()` and `fw9366_intflag_clear(0xffff)` (both run between `get_SMIC_IC_flag` and `cfg_init` in the real sequence -- note: `fw9366_init_chip`'s call order per the earlier trace is Get_OTP_Info -> get_SMIC_IC_flag -> init_flag -> intflag_clear -> cfg_init -> Update_Base -> fdt_auto_start -> poa_send_para). `fw9366_Get_OTP_Info` was skipped over in the original trace summary and has not been individually disassembled yet either -- check it next since it runs first in the chain, before get_SMIC_IC_flag.

## Update: fw9366_Get_OTP_Info / fw9366_sfr_write / fw9366_otp_read traced and CONFIRMED against real hardware (2026-09-16)

Status: CONFIRMED (static disassembly AND live hardware test, zero timeouts across the full sequence)

### fw9366_sfr_write(reg, val) (address 0x165e31) -- NEW confirmed command

```text
Bulk OUT ep0x01: [0x09, 0xf6, reg, val]   (4 bytes)
No bulk IN -- fire-and-forget (transport: ff_spi_write_buf_rts, single
User_TL_Transmit_N_Byte write call, no read pairing -- confirmed by
disassembly).
```

Combined with the already-confirmed SFR read, the low-level command family is now coherent:
- `[0x08, 0xf7, reg, 0x00, 0x00]` -> 1-byte read response (SFR_READ)
- `[0x09, 0xf6, reg, val]` -> no response (SFR_WRITE)

### fw9366_otp_read(addr) (address 0x166b99) -- built on the SFR primitives

```text
sfr_write(0xf1, addr)   ; set OTP address
sfr_write(0xf4, 0xc0)   ; trigger sequence
sfr_write(0xf4, 0xc1)
sfr_write(0xf4, 0xc0)
sfr_write(0xf4, 0xc0)
return sfr_read(0xf3)   ; read result
```

### fw9366_Get_OTP_Info(out1, out2) (address 0x155a40)

```text
*out1 = otp_read(0x03) & 0x1f   (if out1 != NULL)
*out2 = otp_read(0x13) & 0x0f   (if out2 != NULL)
```

Real call in `fw9366_init_chip` is `fw9366_Get_OTP_Info(NULL, NULL)` -- both outputs are discarded by the real driver, but the reads still happen (possibly a required priming/trigger side effect, or just vestigial logging-only code -- not yet clear which, and not critical to resolve since the reads are cheap to replicate either way).

### Live hardware test result (tools/rts5811_wake_test.c, real device)

Every single transfer in both full otp_read sequences (5 writes + 1 read, x2) completed successfully with zero timeouts:

```
otp_read(0x03) -> raw 0xf0  =>  & 0x1f = 0x10
otp_read(0x13) -> raw 0x14  =>  & 0x0f = 0x04
```

CONFIRMED for this physical unit: `otp_read(3)&0x1f = 0x10`, `otp_read(0x13)&0x0f = 0x04`.

### Running tally of confirmed-working real commands
1. Wake ping: `4c 5a 01 00` -> `04 00 00 04` (proceed)
2. SFR read: `08 f7 <reg> 00 00` -> 1 byte
3. SFR write: `09 f6 <reg> <val>` -> no response (fire-and-forget)
4. OTP read (composite: 5x SFR write/read) -- fully exercises both primitives above

Every command tried against real hardware in this protocol family (SFR read/write and everything built on them) has worked cleanly on the first attempt, no retries, no timeouts. This is strong positive signal that the SFR/OTP command family is the correct protocol track for this device.

### Next concrete action
Trace `fw9366_init_flag()` and `fw9366_intflag_clear(0xffff)` (both run between `get_SMIC_IC_flag`/`Get_OTP_Info` and `cfg_init` in the real `fw9366_init_chip` sequence).
