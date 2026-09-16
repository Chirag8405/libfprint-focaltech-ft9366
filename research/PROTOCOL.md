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

## Update: fw9366_init_flag, fw9366_intflag_clear, SRAM read/write traced -- CHIP ID SUCCESSFULLY READ (2026-09-16)

Status: CONFIRMED (static disassembly AND live hardware test, stable across 3 independent runs)

### fw9366_init_flag() (address 0x154cc2)
Pure local state again -- zeroes/constants written into `REG9366` and `fw9366_context` globals, zero USB traffic. Same category as `cfg_init`: nothing to test against hardware.

### fw9366_sram_write(addr, value) / fw9366_sram_read(addr) -- NEW confirmed command family, 16-bit address space

Distinct from the 8-bit SFR space (`08 f7`/`09 f6`) used earlier. Address encoding (identical logic in both write and read):

```text
enc_hi = ((addr >> 8) & 0x7f) | 0x80
enc_lo = addr & 0xff
```

```text
SRAM_WRITE(addr, value):        [opcode 0x05, 0xfa]
  Bulk OUT ep0x01: [0x05, 0xfa, enc_hi, enc_lo, 0x00, 0x01, (value>>8)&0xff, value&0xff]
  No response (fire-and-forget, via ff_spi_write_buf_rts, same as SFR_WRITE).

SRAM_READ(addr):                [opcode 0x04, 0xfb]
  Bulk OUT ep0x01: [0x04, 0xfb, enc_hi, enc_lo, 0x00, 0x01]
  Bulk IN  ep0x82: 6 bytes; result = (resp[0]<<8) | resp[1]
  (via ff_spi_write_then_read_buf_rts, same transport as the original wake ping)
```

The "0x00, 0x01" mid-field is a length-derived constant that happens to be fixed for this function (it always encodes a 2-byte address), not independently variable.

### fw9366_intflag_clear(0xffff) -> sram_write(0x1a84, 0xffff)

Tested live: `05 fa 9a 84 00 01 ff ff` sent successfully (fire-and-forget, no response to check).

### fw9366_chipid_get() -- CONFIRMED to be sram_read(0x1a8b), exact disassembly match

```
fw9366_chipid_get() (0x154955):
  return sram_read(0x1a8b)
```

This is the exact function referenced in the very first session's research notes ("chipid reads 0x0"). Not an approximation -- direct 1:1 disassembly match with what was tested.

### LIVE HARDWARE RESULT -- first non-zero, stable chip ID read in this project's history

```text
sram_read(0x1a8b):
  sent: 04 fb 9a 8b 00 01
  recv: 93 62
  result: 0x9362
```

Repeated 3x across independent fresh device-open sessions: **identical result every time (0x9362)**. Not noise/garbage -- stable, well-formed, non-zero.

### Honest caveat -- not independently verified against an authoritative "expected" constant

`ft_feature_devinit_JudgeByChipId` (the function that should validate a read chip ID) is a **no-op stub in this build**: it sets up two local byte arrays (`{0x06,0xf9,0x00}` and `{0x11,0xee,0x02,0x00}` -- possibly parameters for yet another undiscovered command opcode, not comparison constants) but never actually reads its input parameter or performs a comparison; it just logs and returns 0. So there is no in-binary authoritative constant to confirm `0x9362` is "the correct" value. What IS confirmed:
- The function tested is byte-for-byte the real `fw9366_chipid_get()`, not a guess.
- The result is stable and well-formed, not random/garbage.
- The top byte (`0x93`) matches the `FT9366` naming exactly; the low byte (`0x62` vs. a naively-assumed `0x66`) is plausibly a revision/stepping/mask field rather than evidence of a wrong read -- chip ID registers commonly differ from marketing part numbers in exactly this way.
- Do NOT read this as "enrollment/verify now works" -- it confirms the SRAM/SFR command family is correct and the chip is responding meaningfully for the first time, not that the full protocol (crypto/capture/enroll) is solved.

### Running tally of confirmed-working real commands
1. Wake ping: `4c 5a 01 00` -> `04 00 00 04`
2. SFR read: `08 f7 <reg> 00 00` -> 1 byte
3. SFR write: `09 f6 <reg> <val>` -> no response
4. OTP read (composite, built on SFR read/write)
5. SRAM write: `05 fa <hi> <lo> 00 01 <val_hi> <val_lo>` -> no response
6. SRAM read: `04 fb <hi> <lo> 00 01` -> 6 bytes, result = first 2 bytes big-endian
7. Chip ID read (= SRAM read of 0x1a8b) -> stable `0x9362`

Every single command attempted against real hardware in this session has worked cleanly -- zero timeouts, zero malformed responses, across two full protocol families (SFR and SRAM) and 7 distinct confirmed operations.

### Next concrete action
Continue the `fw9366_init_chip` chain: `fw9366_Update_Base()` (~601 bytes, not yet traced) is next. Given the strong track record so far, also worth checking whether `ft_feature_devinit_JudgeByChipId`'s two mystery byte sequences (`06 f9 00` / `11 ee 02 00`) correspond to a THIRD command opcode family, since `06 f9` and `04 fb`/`05 fa`/`08 f7`/`09 f6` all share the "byte0 < 0x10, byte1 in 0xf0-0xff range" pattern -- possibly worth a quick side-check.

## Update: fw9366_Update_Base chain started -- idle_enter confirmed, fdt_mode_init opening resolved (2026-09-16)

Status: CONFIRMED primitives tested clean; fdt_mode_init itself only ~40% traced (large function, see honest scope note below)

### Call chain confirmed (structural, from disassembly)
```
fw9366_Update_Base()
  fw9366_fdt_base_Stable_Update(h)
    fw9366_fdt_block()          -- local only (returns 4, since Fw9366_cfg[2]=1 confirmed)
    fw9366_fdt_AutoSDacUpdate()  -- no direct SFR/SRAM calls of its own; calls
                                    fdt_manual_start + fdt_get_a_frame_data + local DAC math
    fw9366_fdt_manual_start()
      fw9366_fdt_mode_init()    -- LARGE (8870 bytes), only opening traced so far
      ... (SRAM state machine, sram_bits_set, wm_switch(2), poll loop on intflag)
    fw9366_fdt_get_a_frame_data() -- not yet traced
    fw9392_fdt_base_fail_check()  -- not yet traced
    fw9366_fdt_base_Min_Updata()  -- not yet traced
  fw9366_img_base_Update(h)       -- separate large tree, not yet traced (img_data_get,
                                      calculate_crc, find_max_min_avg_1218, AutoSDacUpdate)
```

### NEW confirmed primitives (all tested live, zero timeouts)

`fw9366_wm_switch(mode)` (0x165d48): table lookup in `FW9366_WorkMode_Cmd` (.rodata @ 0x1c88c0, 3 bytes/mode, 12 modes) + write-only bulk OUT (3 bytes for modes 0-10, 1 byte for mode 11, via `ff_spi_write_buf_rts`). Full table extracted:

```
mode 0: c0 3f 00   mode 1: c1 3e 00   mode 2:  c2 3d 00   mode 3: c4 3b 00
mode 4: c8 37 00   mode 5: d8 27 00   mode 6:  d1 2e 00   mode 7: d2 2d 00
mode 8: d4 2b 00   mode 9: 5a a5 00   mode 10: a5 5a 00   mode 11: 70 (1 byte only)
```

`fw9366_wm_get()` (0x154c3b) = `sfr_read(0x80)`.

`fw9366_idle_enter()` (0x154c5a) -- fully self-contained, no unresolved deps:
```
wm_switch(9)
wm = wm_get()
if wm != 0x50: wm_switch(0)
wm_switch(0xa)
```

LIVE TEST: `wm_switch(9)` sent (`5a a5 00`), `wm_get()` returned **exactly 0x50** (matching the real driver's own check value -- the "not equal, do fallback reset" branch was correctly NOT triggered, a meaningful positive signal, not just "no timeout"), `wm_switch(0xa)` sent (`a5 5a 00`). Fallback `wm_switch(0)` correctly skipped since wm==0x50.

### fw9366_fdt_mode_init opening sequence -- resolved and tested (partial function)

First ~40% of this 8870-byte function traced. Confirmed opening logic:
```
fw9366_idle_enter()
if (REG9366[0x77] == 0): fw9366_img_mode_init(0)   -- REG9366[0x77]=0 confirmed via
    already-traced fw9366_init_flag -- this call WILL happen in the real sequence
sram_write(0x1801, sram_bits_set(0xfc80, hi=6, lo=0, new=REG9366[0x89]))
  = sram_write(0x1801, 0xfc9b)   -- REG9366[0x89]=0x1b confirmed via fw9366_init_flag
sram_write(0x1881, sram_bits_set(sram_bits_set(0, hi=15,lo=8,new=15), hi=4,lo=2,new=3))
  = sram_write(0x1881, 0x0f0c)   -- period=1000/Fw9366_cfg[3]-1=15 (cfg[3]=0x3c confirmed),
    Fw9366_cfg[5]-1=3 (cfg[5]=0x04 confirmed); fw9366_context[0xf8] CONFIRMED (via whole-binary
    search) never written anywhere -- permanently 0 from .bss -- so the cfg[3] branch (not
    cfg[4]) is confirmed taken, not assumed.
```

LIVE TEST: both `sram_write` calls sent cleanly (`05 fa 98 01 00 01 fc 9b` and `05 fa 98 81 00 01 0f 0c`), zero timeouts. **Note: this test deliberately skips `fw9366_img_mode_init(0)`** (untraced, 3074 bytes) which the real sequence calls BEFORE these two writes -- so this is a wire-level sanity check only (do the writes complete cleanly), not a claim that fdt_mode_init as a whole is correctly replicated or "working". Explicitly not overclaiming this.

### Honest scope assessment -- genuine complexity escalation, not a dead end

Everything traced in this phase has resolved deterministically so far (no true dead ends), but the character of the work has changed:
- `fw9366_img_mode_init` (3074 bytes) -- untraced, required next step, blocks further progress on fdt_mode_init's main body.
- The remainder of `fdt_mode_init` (~60% untraced) branches on `fw9366_context[0xfc]` (a state machine: 0xa0/0xa1/0xa2) and `AUTO_DAC_PRO_FLAG`/`FW9366_INIT_RE_CHECK` -- both confirmed to be mutable globals written from MULTIPLE call sites, including `fw9366_fdt_AutoSDacUpdate`, which itself calls `fw9366_fdt_manual_start` (hence `fdt_mode_init`) internally, while also being called BEFORE the direct `fdt_manual_start` call in the outer sequence. This means `fdt_mode_init` runs multiple times with different accumulated state depending on call path -- a qualitatively harder problem than tonight's earlier stateless primitives, with real risk of error if hand-traced without a decompiler.
- Still fully untraced: `fw9366_fdt_get_a_frame_data`, `fw9392_fdt_base_fail_check`, `fw9366_fdt_base_Min_Updata`, and the entire `fw9366_img_base_Update` tree (`fw9366_img_data_get`, `fw9366_calculate_crc`, `find_max_min_avg_1218`, `fw9366_AutoSDacUpdate`) -- likely several thousand more bytes combined.

This is a real, substantial remaining scope -- flagging clearly per session ground rules rather than continuing to push through stateful multi-entry-point logic with lower confidence.

## STEP 1 DELIVERABLE: fw9366_fdt_mode_init call-site / state map (2026-09-16)

Status: CONFIRMED (static disassembly, exhaustive whole-binary search for every read/write of every
relevant global -- not a linear single-path trace). This map was built BEFORE writing any further
integration code, per session methodology, after catching one real tracing error below.

### All 4 call sites of fw9366_fdt_mode_init in the binary

| # | Call site | Enclosing function | Reachable from real init chain? |
|---|---|---|---|
| 1 | 0x15052d | `fw9366_Chip_Paramter_Init` | **NO** -- zero callers anywhere in the binary (confirmed via whole-binary search). Dead/unused path. |
| 2 | 0x1553af | `fw9366_FDT_ESD_Handle` | **NO** -- only caller is `fw9366_Chip_Paramter_Init` (call site 1), itself unreachable. Dead/unused path. |
| 3 | 0x158357 | `fw9366_fdt_auto_start` | **YES** -- called directly by `fw9366_init_chip` after `Update_Base` returns (already-confirmed real sequence). |
| 4 | 0x1586a6 | `fw9366_fdt_manual_start` | **YES** -- reached via TWO different real paths (see below). |

Call sites 1 and 2 are excluded from further analysis -- confirmed unreachable, not guessed.

### The real invocation order and exhaustive state trace

`fw9366_fdt_manual_start` has exactly one call to `fdt_mode_init` (site 4), but is itself called from
two different places in the real sequence, so site 4 fires twice with different accumulated state:

```
fw9366_init_chip()
  fw9366_init_flag()              -- WRITES fw9366_context[0xfc] = 0xa0   (only write outside fdt_mode_init itself
                                        and the unreachable FDT_ESD_Handle -- confirmed via exhaustive search of
                                        all 16 load sites of the fw9366_context base address in the whole binary)
  ... (cfg_init, intflag_clear -- confirmed not touching this state)
  fw9366_Update_Base()
    fw9366_fdt_base_Stable_Update()
      fw9366_fdt_block()          -- no state touch
      fw9366_fdt_AutoSDacUpdate()
        [own logic] AUTO_DAC_PRO_FLAG = 1        (0x15885b, BEFORE its internal fdt_manual_start call)
        fw9366_fdt_manual_start()
          === INVOCATION A1 === entry state: fw9366_context[0xfc]=0xa0, AUTO_DAC_PRO_FLAG=1,
              FW9366_LAST_AUTO=0xaa (compiled-in .data default, NOT zero -- confirmed by reading .data bytes)
        [own logic] AUTO_DAC_PRO_FLAG = 0        (0x158ed7, AFTER, does not affect invocation A1)
      fw9366_fdt_manual_start()    -- DIRECT call, after AutoSDacUpdate returns
        === INVOCATION A2 === entry state: fw9366_context[0xfc]=0xa1 (set by A1's own body, see below)
      fw9366_fdt_get_a_frame_data()  -- no state touch (not in the 16-site list)
      fw9392_fdt_base_fail_check()   -- no state touch
      fw9366_fdt_base_Min_Updata()   -- no state touch
    fw9366_img_base_Update()         -- no state touch (not in the 16-site list)
  fw9366_fdt_auto_start(1)
    ... eventually calls fdt_mode_init via call site 3
        === INVOCATION B === entry state: fw9366_context[0xfc]=0xa1 (unchanged since A1; A2 didn't write it,
            neither did anything else in Update_Base's tree)
```

### Branch activation per invocation

**Invocation A1** (state 0xa0, AUTO_DAC_PRO_FLAG=1, FW9366_LAST_AUTO=0xaa) -- the ONLY invocation that does real work:
1. `idle_enter()` -- CONFIRMED, already tested clean live.
2. `if (REG9366[0x77]==0): img_mode_init(0)` -- REG9366[0x77]=0 confirmed (init_flag), so this call happens here,
   with `fw9366_context[0xfc]` STILL 0xa0 at this point (fdt_mode_init's own write to 0xfc happens LATER in its
   body, after this call) -- relevant for tracing img_mode_init's own internal state check in Step 3.
3. `sram_write(0x1801, 0xfc9b)` -- CONFIRMED, already tested clean live.
4. **NEWLY FOUND, not previously traced**: a gated `0x180c` write:
   - Check: `FW9366_LAST_AUTO(0xaa) == AUTO_DAC_PRO_FLAG(1)`? NO -> do not skip.
   - Check: `AUTO_DAC_PRO_FLAG(1) == 0`? NO -> takes the "else" branch (not the branch I originally,
     incorrectly assumed while first passing through this code):
     `sram_write(0x180c, sram_bits_set(0, hi=0xa, lo=0, new=0))` = `sram_write(0x180c, 0x0000)`
   - After either branch: `FW9366_LAST_AUTO = AUTO_DAC_PRO_FLAG` (becomes 1).
   - **This is exactly the kind of divergence flagged as highest-risk in Step 1 of this session's plan** --
     caught here BEFORE integrating/testing, not after.
5. `sram_write(0x1881, 0x0f0c)` -- CONFIRMED already tested clean live (this part was correctly resolved
   previously -- it's gated only by the a1/a2 state check at step 6, not by the AUTO_DAC_PRO_FLAG branch above,
   confirmed by re-reading the control flow order precisely).
6. At end of this section: `fw9366_context[0xfc] = 0xa1` (unconditional, since `fw9366_context[0xf8]` is
   permanently 0 -- confirmed via exhaustive search).
7. (Rest of fdt_mode_init's body beyond this point -- not yet traced, see Step 2.)

**Invocation A2** (state 0xa1): Reads `fw9366_context[0xfc]==0xa1` -> matches the "state==0xa1" branch ->
checks `fw9366_context[0xf8]==6` (permanently false) -> takes the LOG-AND-EARLY-EXIT path (jumps to near the
end of the function, ~offset 0x229a of 0x22a6) -> **this invocation is a no-op**. It does NOT call
`img_mode_init`, does NOT touch any SRAM registers. Only a diagnostic log line executes.

**Invocation B** (state 0xa1, unchanged since A1): Same as A2 -- **also a no-op**.

### Practical implication for integration (corrects prior session's test)

The PREVIOUS test in `tools/rts5811_wake_test.c` exercised `idle_enter()` + the `0x1801`/`0x1881` writes as if
they represented "the" fdt_mode_init path in general -- that conclusion was directionally right (this IS the
one meaningful invocation) but incomplete: it MISSED the `0x180c` write entirely, and had it been traced
further without this state-mapping step, a later linear pass could easily have picked the WRONG branch for
`0x180c` (the code superficially reads as if `AUTO_DAC_PRO_FLAG==0` is the "normal" path, but at the real
invocation point it is NOT 0 -- it's 1, set moments earlier by the very function that leads here). This is
now corrected before further integration, per session ground rules.

Since invocations A2 and B are both confirmed no-ops for `fdt_mode_init` specifically, **no further state
mapping is needed for those two call paths** -- they contribute nothing beyond a log line. All further tracing
of `fdt_mode_init`'s body should proceed from invocation A1's state only.

## Update: invocation A1 sequence corrected and re-tested clean (2026-09-16)

Status: CONFIRMED, tested live against real hardware, zero timeouts

Per the Step 1 state map above, `tools/rts5811_wake_test.c` updated to include the previously-missed
`sram_write(0x180c, 0x0000)` in its correct position (between the `0x1801` and `0x1881` writes), using the
now-confirmed `AUTO_DAC_PRO_FLAG=1` branch rather than the `==0` branch.

Live test, all three writes clean:
```
sram_write(0x1801, 0xfc9b) -> 05 fa 98 01 00 01 fc 9b
sram_write(0x180c, 0x0000) -> 05 fa 98 0c 00 01 00 00
sram_write(0x1881, 0x0f0c) -> 05 fa 98 81 00 01 0f 0c
```

This is now confirmed complete for invocation A1 up to the point `fdt_mode_init` sets
`fw9366_context[0xfc]=0xa1` (a host-side-only state write, no wire effect). Still not integrated: the
`fw9366_img_mode_init(0)` call (which the real sequence makes BEFORE these three writes) and the remaining
~60% of `fdt_mode_init`'s body after this point.

## Update: fw9366_img_mode_init opening traced and tested clean (2026-09-16)

Status: CONFIRMED (static disassembly + live hardware test), ~13% of this 3074-byte function covered

### Call-site check (same discipline as fdt_mode_init)
6 call sites total in the binary. Two are the already-confirmed-unreachable `fw9366_Chip_Paramter_Init` /
`fw9366_FDT_ESD_Handle`. Three more (`fw9366_img_scan_start`, `fw9366_Special_img_scan_start`,
`fw9366_GestureStart`) are later-phase scan/gesture entry points, not reachable from the init chain currently
being traced -- noted, not chased further right now (out of scope until the capture/scan phase). Only the
call from inside `fdt_mode_init` (param=0, confirmed via `fw9366_init_flag`'s `REG9366[0x77]=0`) is relevant.

### Resolved opening sequence (param=0)
```
img_mode_init(0):
  idle_enter()   -- called again; same primitive, harmless to repeat.
  sram_write(0x1801, sram_bits_set(0xfc80, hi=6, lo=0, new=REG9366[0x87]))
    = sram_write(0x1801, 0xfcb6)   -- REG9366[0x87]=0x36 (unconditional write, confirmed via fw9366_init_flag)
  FW9366_LAST_DAC = REG9366[0x87]   -- host-side only
  if (param==0): sram_write(0x1800, 0x4ffe)   -- FIXED constant, no host-state dependency (our case)
```

**Important ordering finding**: this `0x1801` write happens BEFORE `fdt_mode_init`'s own `0x1801` write
(`0xfc9b`, already confirmed) in the real sequence -- two writes to the same SRAM address with different
values, not one. Both are now sent in the correct order in `tools/rts5811_wake_test.c`, even though the
second overwrites the first -- wire-level fidelity to the real sequence, not just final-state correctness.

### Live hardware test result -- all clean, zero timeouts
```
sram_write(0x1801, 0xfcb6) -> 05 fa 98 01 00 01 fc b6
sram_write(0x1800, 0x4ffe) -> 05 fa 98 00 00 01 4f fe
sram_write(0x1801, 0xfc9b) -> 05 fa 98 01 00 01 fc 9b   (fdt_mode_init's own write, now correctly after)
sram_write(0x180c, 0x0000) -> 05 fa 98 0c 00 01 00 00
sram_write(0x1881, 0x0f0c) -> 05 fa 98 81 00 01 0f 0c
```

Remaining ~87% of `img_mode_init` and ~60% of `fdt_mode_init` (after the point where it calls `img_mode_init`)
still not traced. Running tally: 5 confirmed sram_write commands now integrated for this portion of the real
init sequence, all tested clean.

## Update: img_mode_init continued -- 0x1804/Set_Scan_Rate_2M/0x1807, tested clean (2026-09-16)

Status: CONFIRMED (static disassembly + live hardware test, zero timeouts, including live read-modify-write)

### Second fw9366_context[0xfc] gate found and correctly resolved

At 0x15bcec, img_mode_init checks `fw9366_context[0xfc] == 0xa3` (a state value distinct from fdt_mode_init's
0xa0/0xa1/0xa2). At the real invocation point, `+0xfc` is still 0xa0 (fdt_mode_init's own write to 0xa1 happens
AFTER img_mode_init returns) -- so `0xa0 != 0xa3`, gate passes through to the main block, which itself then
sets `+0xfc = 0xa3`.

### sram_write(0x1804, 0x27ca)
```
sram_bits_set(sram_bits_set(sram_bits_set(0x7c0, hi=1,lo=0,new=Fw9366_cfg[0xa]),
                             hi=3,lo=3,new=1 [since Fw9366_cfg[0xa]=2>1]),
               hi=0xd,lo=0xd,new=1)
= 0x27ca   -- Fw9366_cfg[0xa]=0x02 confirmed via already-traced cfg_init
```

### fw9366_Set_Scan_Rate_2M() -- NEW, fully self-contained on live hardware state
```
v = sram_read(0x1806); v = bits_set(v,13,7,9); sram_write(0x1806, v)
v = sram_read(0x180a); v = bits_set(v,13,7,9); v = bits_set(v,6,0,3); sram_write(0x180a, v)
v = sram_read(0x180b); v = bits_set(v,13,7,4); v = bits_set(v,6,0,8); sram_write(0x180b, v)
```
No host-state dependency at all -- reads the actual current hardware value each time. Added a real
`sram_bits_set()` C implementation (pure bitfield math, matching the traced logic exactly) to
`tools/rts5811_wake_test.c` for this.

### sram_write(0x1807, 0x18e1)
```
sram_bits_set(sram_bits_set(1, hi=0xd,lo=5,new=Fw9366_cfg[0xc]-1), hi=4,lo=4,new=0 [Fw9366_cfg[2]!=0])
= 0x18e1   -- Fw9366_cfg[0xc]=0xc8 (this session's earlier live smic_flag=0 measurement), Fw9366_cfg[2]=1 confirmed
```

### Live hardware test -- all clean, zero timeouts, including real read-modify-write cycles
```
sram_write(0x1804, 0x27ca)     -> 05 fa 98 04 00 01 27 ca
sram_read(0x1806)  -> 09 bb    -> bits_set -> sram_write(0x1806, 0x04bb) -> 05 fa 98 06 00 01 04 bb
sram_read(0x180a)  -> 09 87    -> bits_set -> sram_write(0x180a, 0x0483) -> 05 fa 98 0a 00 01 04 83
sram_read(0x180b)  -> 04 91    -> bits_set -> sram_write(0x180b, 0x0208) -> 05 fa 98 0b 00 01 02 08
sram_write(0x1807, 0x18e1)     -> 05 fa 98 07 00 01 18 e1
```

Running tally: img_mode_init now ~25% traced (up from ~13%). Remaining ~75% still untraced.

## MILESTONE: fw9366_img_mode_init(0) FULLY TRACED AND TESTED CLEAN, 100% (2026-09-16)

Status: CONFIRMED, complete function, all live hardware transfers clean, zero timeouts

Completed the remaining ~75% of `img_mode_init` (0x15b8a9-0x15c4aa, 3074 bytes) in this pass. Final tail
sequence:

```
sram_write(0x1887, sram_bits_set(0, hi=2,lo=0,new=2))   -- Fw9366_cfg[2]!=0 confirmed always true = write 2
if (REG9366[0x77] != 1):   [TRUE -- REG9366[0x77]=0 confirmed]
  v = sram_read(0x1805); v = bits_set(v,4,0,0); v = bits_set(v,7,5,0); sram_write(0x1805, v)
    -- clears the live-read low byte, no static value (host-independent, live-dependent only)
v = sram_read(0x1811); v = bits_set(v,9,0,0x1fe); sram_write(0x1811, v)   -- live read-modify-write
intflag_mask(5)   -- sram_read/write(0x1a83) | FW9366_INT_INDEX[5]=0x20
intflag_mask(6)   -- sram_read/write(0x1a83) | FW9366_INT_INDEX[6]=0x40
REG9366[0x77] = 1   -- host-side only; flips the guard that gated THIS call, irrelevant to current invocation
int_gap_set(0x64)     -- sfr_write(0x8e, (100*10000)>>12) = sfr_write(0x8e, 0xf4)
wdtcnt_gap_set(0x7d0) -- wdtcnt_int_en(0)=sfr_write(0x90,0); sfr_write(0x91,7); sfr_write(0x92,0xd0);
                          wdtcnt_int_en(1)=sfr_write(0x90,1)
```

New confirmed primitives, all built on already-known transports:
- `fw9366_intflag_mask(src)` = `sram_write(0x1a83, sram_read(0x1a83) | FW9366_INT_INDEX[src])`
- `fw9366_int_gap_set(gap)` = `sfr_write(0x8e, (min(gap,0x68)*10000)>>12)`
- `fw9366_wdtcnt_int_en(en)` = `sfr_write(0x90, en?1:0)`
- `fw9366_wdtcnt_gap_set(val)` = `wdtcnt_int_en(0); sfr_write(0x91,hi); sfr_write(0x92,lo); wdtcnt_int_en(1)`
- `FW9366_INT_INDEX` table extracted from .rodata (bit-flag table, entry[i]=1<<i)

### Live hardware test -- complete function, all clean, zero timeouts

```
sram_write(0x1887, 0x0002)  -> 05 fa 98 87 00 01 00 02
sram_read(0x1805)  -> 00 00 -> sram_write(0x1805, 0x0000) -> 05 fa 98 05 00 01 00 00
sram_read(0x1811)  -> 00 00 -> sram_write(0x1811, 0x01fe) -> 05 fa 98 11 00 01 01 fe
intflag_mask(5): sram_read(0x1a83)=0000 -> sram_write(0x1a83, 0x0020) -> 05 fa 9a 83 00 01 00 20
intflag_mask(6): sram_read(0x1a83)=0020 -> sram_write(0x1a83, 0x0060) -> 05 fa 9a 83 00 01 00 60
int_gap_set(0x64): sfr_write(0x8e, 0xf4) -> 09 f6 8e f4          [matches hand-computed value exactly]
wdtcnt_gap_set(0x7d0):
  sfr_write(0x90, 0x00) -> 09 f6 90 00
  sfr_write(0x91, 0x07) -> 09 f6 91 07                            [matches hand-computed value exactly]
  sfr_write(0x92, 0xd0) -> 09 f6 92 d0                            [matches hand-computed value exactly]
  sfr_write(0x90, 0x01) -> 09 f6 90 01
```

**`fw9366_img_mode_init(0)` is now completely and correctly traced end to end.** Every computed byte matched
hand-derived predictions exactly, and every transfer succeeded with zero timeouts. This resolves the item that
was called out at the very start of this session as the concrete next blocker after the wake sequence.

### Next concrete action
Return to `fw9366_fdt_mode_init`'s remaining ~60% (picking up right after the `img_mode_init(0)` call site,
which is now fully resolved) -- the `0x1801`/`0x180c`/`0x1881` writes already confirmed are what comes AFTER
this point in the real sequence.

## Update: fdt_mode_init continued past context[0xfc]=0xa1, tested clean (2026-09-16)

Status: CONFIRMED, tested live, zero timeouts

Three more writes resolved, all further writes to addresses img_mode_init already touched -- same
multiple-writes-to-same-address pattern caught twice already this session, preserved in real order:

```
sram_write(0x1800, sram_bits_set(sram_bits_set(0,hi=0xa,lo=1,new=0x3ff), hi=0xe,lo=0xb,new=0))
  = sram_write(0x1800, 0x07fe)   -- Fw9366_cfg[2]!=0 branch (confirmed always true);
    2nd write to 0x1800 (img_mode_init wrote 0x4ffe)
sram_write(0x1804, sram_bits_set(0x27c8, hi=2,lo=0,new=Fw9366_cfg[1]-1))
  = sram_write(0x1804, 0x27c8)   -- Fw9366_cfg[1]=0x01 confirmed via cfg_init;
    3rd write to 0x1804 total (img_mode_init wrote 0x27ca -- differs by exactly ONE BIT)
if (Fw9366_cfg[2]!=0): sram_write(0x1807, 0x1671)   -- FIXED constant, no computation;
    2nd write to 0x1807 (img_mode_init wrote 0x18e1)
```

Live test: all three clean, zero timeouts (`05 fa 98 00 00 01 07 fe`, `05 fa 98 04 00 01 27 c8`,
`05 fa 98 07 00 01 16 71`). `fdt_mode_init` now roughly 50% traced.

## Update: fdt_mode_init continued -- 0x1808/0x1887(3rd)/Set_Scan_Rate_Default/0x1805, tested clean (2026-09-16)

Status: CONFIRMED, tested live, zero timeouts, internal consistency verified across successive live reads

```
sram_write(0x1808, sram_bits_set(sram_bits_set(0x800,hi=7,lo=0,new=1), hi=0xa,lo=8,new=0))
  = sram_write(0x1808, 0x0801)   -- Fw9366_cfg[2]!=0 branch confirmed always true
sram_write(0x1887, sram_bits_set(0,hi=1,lo=0,new=5))
  = sram_write(0x1887, 0x0001)   -- 2nd write to 0x1887 in fdt_mode_init (img_mode_init wrote 2 earlier)
fw9366_Set_Scan_Rate_Default()   -- NEW, same pattern as Set_Scan_Rate_2M: 3 live read-modify-write ops,
    no host-state dependency:
  v=sram_read(0x1806); v=bits_set(v,13,7,0x13); sram_write(0x1806,v)
  v=sram_read(0x180a); v=bits_set(v,13,7,0x13); v=bits_set(v,6,0,7); sram_write(0x180a,v)
  v=sram_read(0x180b); v=bits_set(v,13,7,9); v=bits_set(v,6,0,0x11); sram_write(0x180b,v)
if (REG9366[0x78]==1): [FALSE, REG9366[0x78]=0 confirmed via init_flag -- real-work path taken]
v=sram_read(0x1805); v=bits_set(v,4,4,0); sram_write(0x1805,v)   -- another write to 0x1805
```

Live test: all clean, zero timeouts. Notable internal-consistency confirmation: `set_scan_rate_default()`'s
live read of `0x180a` returned `0x0483` -- exactly what `set_scan_rate_2m()` had written moments earlier in
this same run -- and correctly recomputed to `0x0987`; same pattern for `0x180b` (read `0x0208`, wrote
`0x0491`). This confirms the whole chain of live read-modify-writes is behaving consistently, not just
individually clean.

`fdt_mode_init` now roughly 65% traced.

## Update: fdt_mode_init loop resolved via radare2 CFG analysis, tested clean (2026-09-16)

Status: CONFIRMED (radare2 control-flow analysis + live hardware test, zero timeouts)

### Tooling note
Installed `radare2` mid-session specifically to resolve a real control-flow ambiguity that linear disassembly
reading could not confidently settle: address `0xC0` appeared to be computed a second time (`rbp-1 + 0xc0`),
suggesting either a loop or a genuine revisit. `r2`'s basic-block graph (`afb` on the function) showed a
backward edge from `0x157c10` to `0x1577b8`, confirming a loop, and the increment instruction at `0x157c10`
(`add BYTE[rbp-1], 0x9`) showed the counter advances by 9 per iteration, not 1.

### Resolved: 0x180d chaining + 0x1888 + sfr_write(0x9a) + the loop

```
sram_write(0x180d, sram_bits_set(<0x1805's just-computed value>, hi=9,lo=0,new=0x384))
  -- IMPORTANT: base is NOT fresh, it's the exact value already computed for the 0x1805 write
     immediately prior -- a compiler register/stack-slot reuse artifact, confirmed by the absence
     of any intervening read/reset of that local before this use.
v=sram_read(0x1888); v=bits_set(v,9,2,0); sram_write(0x1888,v)   -- fresh, independent
sfr_write(0x9a, 0x5a)   -- direct fixed-value SFR write, no SRAM involved

Loop (0x157373-0x157c19), Fw9366_cfg[2]!=0 branch (confirmed always true):
  Pre-loop, computed once: bitfield_hi = 0x2224>>3 = 0x444, bitfield_lo = 0x2224&7 = 4;
    separately, fixed_field = 4-1 = 3
  for i in {0, 9, 18}:   -- counter starts at 0, +9 per iteration, bound <=0x13(19) -> exactly 3 iterations
    addr1 = 0xbf+i: v=sram_read; v=bits_set(v,12,0,0x444); sram_write
    addr2 = 0xc0+i: v=sram_read; v=bits_set(v,5,3,4); v=bits_set(v,1,0,1); sram_write
    addr3 = 0xc1+i: v=sram_read; v=bits_set(v,7,6,3); sram_write
```

### Live hardware test -- all clean, zero timeouts, full loop executed correctly

All 9 register read-modify-writes across the 3 iterations completed at exactly the predicted addresses
(0xbf/0xc0/0xc1, 0xc8/0xc9/0xca, 0xd1/0xd2/0xd3), each showing real live-read values correctly transformed.

`fdt_mode_init` now roughly 85% traced.

### Methodology note
This is the clearest example yet of why the "map state/control-flow before tracing linearly" discipline
matters: a purely linear read would very likely have either mis-treated the repeated `0xC0`-style address
computation as a copy-paste artifact (silently dropping 6 of the 9 real writes) or guessed at a wrong loop
bound. Installing a decompiler-adjacent tool (radare2) once linear reading hit genuine ambiguity, rather than
guessing, resolved it with certainty.

## MILESTONE: fw9366_fdt_mode_init FULLY TRACED AND TESTED CLEAN, 100% (2026-09-16)

Status: CONFIRMED, complete function, all live hardware transfers clean, zero timeouts

Final section (0x158080-0x158223, function end):
```
sram_write(0x1a8a, sram_bits_set(0, hi=7,lo=0, new=0xff)) = sram_write(0x1a8a, 0x00ff)
intflag_mask(3)
REG9366[0x78] = 1   -- host-side only, no wire effect
```

Live test: `sram_write(0x1a8a, 0xff)` clean. `intflag_mask(3)` read back `0x1a83=0x0060` (the accumulated mask
from `img_mode_init`'s earlier `intflag_mask(5)`/`intflag_mask(6)` calls, `0x20|0x40`, within this same run)
and wrote `0x0068` (`0x60 | FW9366_INT_INDEX[3]=0x08`) -- another confirmed cross-function consistency check,
not just an isolated clean transfer.

**`fw9366_fdt_mode_init` is now completely and correctly traced end to end.** Combined with the already-complete
`fw9366_img_mode_init`, the two largest functions in the `fw9366_init_chip` -> `Update_Base` ->
`fdt_base_Stable_Update` -> `fdt_AutoSDacUpdate`/`fdt_manual_start` call path are now fully resolved and tested.

### Running summary of fully-complete functions this session
- `fw9366_idle_enter` -- 100%
- `fw9366_img_mode_init(0)` -- 100%
- `fw9366_fdt_mode_init` (invocation A1 path) -- 100%

### Next concrete action
Return to `fw9366_fdt_AutoSDacUpdate`'s own remaining body (the DAC-feedback arithmetic that surrounds its
calls to `fdt_manual_start`/`fdt_get_a_frame_data`, not yet traced) -- OR proceed directly to
`fw9366_fdt_get_a_frame_data` (called next in `fdt_base_Stable_Update`'s sequence after `fdt_manual_start`
returns) per the original call order. `fdt_get_a_frame_data` is the next untraced function in the direct
outer sequence.
