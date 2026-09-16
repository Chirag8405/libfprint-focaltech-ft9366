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

## Update: fw9366_fdt_get_a_frame_data traced and tested -- new bulk-read primitive (2026-09-16)

Status: CONFIRMED (transfer mechanics, static disassembly + live test); DATA CONTENT not yet interpreted

### Call-site check
4 call sites total. Two (`fw9366_fdt_manual_check`, `fw9366_fdt_manual_store`) are NOT reachable from anything
traced in this session's confirmed chain -- noted, not chased. The two relevant ones are inside
`fw9366_fdt_AutoSDacUpdate` (internal call) and `fw9366_fdt_base_Stable_Update` (direct call, after
`fdt_manual_start` returns) -- same dual-invocation shape already seen for `fdt_mode_init`, but this function
itself has no `fw9366_context[0xfc]`-style state guard, so (unlike `fdt_mode_init`) both invocations likely do
real work -- not yet separately verified for the second call site.

### fw9366_sram_read_bulk_withecc(addr, out_buf, len_words) -- NEW primitive, first variable-length bulk read

```text
Bulk OUT ep0x01: [0x04, 0xfb, enc_hi, enc_lo, 0x00, half]   (6 bytes)
  where half = (len_words - 2) / 2   -- same divide-by-2 encoding already validated via the
  fixed len=2 case used by plain sram_read/sram_write (which produces "00 01" for half=1)
Bulk IN  ep0x82: (len_words - 2) bytes, directly into caller's buffer
  -- CONFIRMED the actual byte count read is len_words-2, not len_words, from the disassembly's
     internal length variable, not assumed
```

### fw9366_fdt_get_a_frame_data(out_buf)

```
block = fdt_block() = 4   (confirmed always, Fw9366_cfg[2]=1)
len_words = (block+1)*2 = 10
addr = (smic_flag==0xaa) ? 0xe8 : 0xb8   -- smic_flag=0 confirmed (this unit) -> addr=0xb8
sram_read_bulk_withecc(addr, out_buf, len_words)   -- reads 8 actual bytes
for i in 0..block-1: byte-swap out_buf[i*2..i*2+1] in place
```

### Live hardware test

```
sent: 04 fb 80 b8 00 04   (matches hand-derived header exactly)
recv: 00 00 00 00 00 00 00 00   (8 bytes, as predicted from len_words-2=8)
```

Transfer mechanics CONFIRMED correct (right header bytes, right length, zero timeout). **Data content is all
zeros** -- reporting this exactly as observed, not interpreting it as success or failure. This could be the
expected pre-scan/calibration state (no real finger-detect cycle has been armed/triggered yet in this
session's sequence), or could indicate frame data isn't populated at this point without a preceding trigger
not yet identified. Not yet resolved either way -- flagged as an open question, not glossed over.

### Next concrete action
`fw9392_fdt_base_fail_check` and `fw9366_fdt_base_Min_Updata` (both called after `fdt_get_a_frame_data` in
`fdt_base_Stable_Update`'s real sequence) -- one of these likely interprets/validates the frame data just read,
which may clarify whether the all-zeros result is expected at this stage.

## Update: fw9392_fdt_base_fail_check traced and tested -- confirms frame data is invalid at this point (2026-09-16)

Status: CONFIRMED (static disassembly, pure local logic) + live test against actual captured data

### fw9392_fdt_base_fail_check(data) -- pure local, no I/O

```
for i in 0..3:
  val = native_u16_read(data + i*2)   -- reads the buffer AS ALREADY BYTE-SWAPPED by
                                          fdt_get_a_frame_data; a plain little-endian
                                          re-read of that swapped buffer (NOT a further
                                          byte-order reinterpretation -- this was a real
                                          bug caught and fixed before testing: initially
                                          wrote this as a big-endian read of the swapped
                                          bytes, which is wrong; corrected to a plain LE
                                          read matching the disassembly's native uint16
                                          access exactly)
  if val > 0x2bc(700) or val <= 0x12b(299): return -1 (FAIL), stop checking further values
return 0 (PASS)
```

### Live test result -- decisive

Ran directly against this session's actual captured `frame_buf` (all zeros, from the previous
`fdt_get_a_frame_data` test):

```
fdt_base_fail_check: i=0 val=0 (0x0000) <= 0x12b -- FAIL
fdt_base_fail_check() returned: -1 (FAIL)
```

**This resolves the open question from the previous entry.** The all-zero frame data is not an artifact of a
bug in this session's replication -- the real driver's OWN validity check would ALSO reject it. Two
possibilities, not yet distinguished: (a) something must happen before this specific capture point to produce
valid data (e.g. `fdt_base_Min_Updata`, not yet traced, may perform a required prerequisite step), or (b) a
failure here is an EXPECTED, handled outcome at this stage of calibration (e.g. feeds into retry/fallback
logic elsewhere in `fdt_base_Stable_Update`, not yet traced beyond this point).

### Next concrete action
Trace `fw9366_fdt_base_Min_Updata` (the last untraced function directly in `fdt_base_Stable_Update`'s
sequence) -- may resolve which of the two possibilities above is correct, and/or reveal what "Min Update"
does when the fail-check result is -1 (e.g. does the caller check this return value and take a different path).

## MILESTONE: fw9366_calculate_crc algorithm fully resolved (2026-09-16)

Status: CONFIRMED (static disassembly + verified against standard CRC-16/CCITT-FALSE test vector)

### fw9366_calculate_crc(data, len) -- pure local, no I/O

This is the **standard CRC-16/CCITT-FALSE algorithm**: polynomial `0x1021`, initial value `0xffff`, MSB-first
bit order, no input/output reflection, no final XOR.

```
crc = 0xffff
for each byte in data:
  cur = byte << 8
  for bit in 0..7:
    xor_val = crc ^ cur
    crc <<= 1; cur <<= 1
    if (xor_val & 0x8000): crc ^= 0x1021
return crc
```

**Verified correct**, not just pattern-matched: computed against the standard CRC-16/CCITT-FALSE test vector
("123456789" -> expected `0x29b1`) -- got `0x29b1` exactly. This directly answers the "exact CRC
algorithm/polynomial/seed" question from this session's brief.

### fw9366_fdt_base_Min_Updata(data) -- pure local, no I/O at all

Confirmed via call-site check: 2 call sites, one in our confirmed chain (`fdt_base_Stable_Update`), the other
in an unrelated function (`fw9366_fdt_base_Update`, no "Stable" -- not reachable from anything traced this
session, likely part of the later capture/enroll flow, not chased now).

```
block = fdt_block() = 4
for i in 0..block-1:
  val = data[i]
  data[i] = (val <= 0x1e) ? 0 : val - 0x1e   -- baseline subtraction, in place
for i in 0..block-1:
  val = data[i]   -- the adjusted value
  REG9366[0x92+2i] = val   -- three separate copies of the same adjusted value,
  REG9366[0xa6+2i] = val      confirmed via disassembly (not assumed) -- all three
  REG9366[0xba+2i] = val      write blocks read the SAME data[i] and write the SAME val
crc1 = calculate_crc(&REG9366[0xba], 8); REG9366[0xca] = crc1
crc2 = calculate_crc(&REG9366[0xa6], 8); REG9366[0xb6] = crc2
```

Since both CRC'd regions are confirmed to hold identical copies of the adjusted frame data, both CRCs are
computed over the same effective 8 bytes.

### Live-data test (pure logic, no new hardware I/O needed -- function has none)

Run directly against this session's actual captured `frame_buf` (all zeros -> all adjusted values also 0
since 0 <= 30):

```
adjusted = [0, 0, 0, 0]
crc1 = crc2 = 0x313e
```

Both match hand-derived predictions exactly.

### Next concrete action
This completes `fw9366_fdt_base_Stable_Update`'s entire direct call list (`fdt_block`, `fdt_AutoSDacUpdate`,
`fdt_manual_start`, `fdt_get_a_frame_data`, `fw9392_fdt_base_fail_check`, `fdt_base_Min_Updata` -- all now
traced). Next: `fw9366_img_base_Update` (the second direct child of `fw9366_Update_Base`, not yet started --
contains `fw9366_img_data_get`, `find_max_min_avg_1218`, and its own `fw9366_AutoSDacUpdate`).

## MAJOR MILESTONE: real image capture pipeline working end-to-end (2026-09-16)

Status: CONFIRMED (static disassembly + live hardware test) -- first successful large structured data capture

### New primitives traced and integrated

`fw9366_fifo_read(addr, out_buf, len)` (0x166891): opcode `[0x06, 0xf9]`, same address encoding as sram_read,
general length-field encoding (see below), but **no -2 adjustment** (unlike sram_read_bulk_withecc) -- actual
bytes read equals `len` exactly, confirmed from disassembly. Uses a different low-level transport
(`ff_spi_read_image_buf` vs `ff_spi_write_then_read_buf_rts`) but confirmed structurally identical (same
`User_TL_Transmit_N_Byte` write/read pair, same bulk EP 0x01/0x82) -- no special large-transfer handling
needed; `libusb_bulk_transfer` already handles multi-packet transfers transparently.

**General length-field encoding, re-derived and corrected**: `half = len/2`, stored **big-endian**
(`byte_hi = (half>>8)&0xff`, `byte_lo = half&0xff`). This corrects an earlier simplification in
`sram_read_bulk_withecc` that only happened to work because its lengths were always small (half<256) --
verified the general formula against both previously-confirmed small cases (len=2->`00 01`, len=8->`00 04`)
before using it for large image-chunk lengths (half up to 5120 for a 10240-byte chunk, which does NOT fit in
one byte -- would have silently produced wrong bytes with the old shortcut).

`fw9366_image_read(out_buf, param)` (0x166a5b): param clamped to [0,5] (0->1), `total_len = param*5*2048`
bytes, read in chunks of up to 10240 bytes via `fifo_read(0x1a05, ...)` -- the SAME fixed address every chunk
(a streaming FIFO port, not a growing memory range, consistent with the function's name).

`fw9366_img_scan_start()` (0x15c4ab): calls `img_mode_init(0)` AGAIN (a second real invocation -- confirmed
via disassembly reachability that this call site is genuinely part of the real chain, correcting an earlier
dismissal of it as "later-phase only"), `wm_switch(3)`, polls `wm_get()` up to 10x for `0x54`, sets bit 0 of
`0x1800`, sleeps 1ms.

`fw9366_img_scan_end()` (0x15c5bf): `intflag_clear(FW9366_INT_INDEX[5]=0x20)`.

`fw9366_img_data_get(out_buf, param)` (0x15c64d): `img_scan_start()` -> `image_read()` -> `img_scan_end()`.

### LIVE HARDWARE TEST -- full real image capture, no finger present (baseline)

```
img_scan_start: wm_switch(3) -> c4 3b 00 (table entry for mode 3, confirmed)
img_scan_start: wm_get() -> 0x54 on the FIRST attempt (no retries needed -- clean signal)
img_scan_start: 0x1800 read (4ffe) -> bit0 set -> write 4fff
image_read: fifo_read(0x1a05, len=10240) -> sent: 06 f9 9a 05 14 00
  recv: 10240 bytes, ALL CLEAN (zero timeout/error)
  nonzero=9900/10240, min=0x00, max=0xff, avg=66.4
  first bytes: 00 00 08 7f 08 6d 08 67 08 8f 07 f8 08 bc 08 7b 09 0b 09 de 09 6a 0a 61 ...
img_scan_end: intflag_clear(0x20) -> clean
```

This is a categorically different result from the earlier 8-byte all-zero "frame data" -- this is a large,
structured, highly non-trivial data capture. The pattern (consistent high byte ~0x08-0x09 with varying low
byte, repeating across the buffer) is exactly what would be expected from packed 12-16 bit sensor ADC/pixel
readings, not noise or a garbage/error response.

**Honest interpretation**: this confirms the FULL capture pipeline (mode switch -> scan arm -> bulk image
transfer -> scan end) works correctly at the wire level and pulls real, structured data off the sensor. It
does NOT yet confirm this specific capture is a fingerprint pattern versus a no-finger baseline/dark
reference -- that requires a live touch comparison, not yet done. Next action: repeat this exact capture
while physically touching the sensor and compare.

## MAJOR MILESTONE: touch-responsive signal confirmed in real image capture (2026-09-16)

Status: CONFIRMED via live comparison (properly synchronized: user confirmed finger already on sensor before
capture was triggered)

### Comparison: no-finger baseline vs. confirmed-touch capture

Both captures used the identical `img_data_get(param=0)` path (full 10240-byte image, `fifo_read(0x1a05,...)`).

```
Baseline (no finger):  00 00 08 7f 08 6d 08 67 08 8f 07 f8 08 bc 08 7b 09 0b 09 de 09 6a 0a 61 ...
                        nonzero=9900/10240, avg=66.4

Touch (confirmed):      00 00 06 1f 05 a7 05 96 05 d9 05 98 06 d2 06 ce 07 4e 07 bf 06 d5 07 73 ...
                        nonzero=9892/10240, avg=65.1
```

Per-sample comparison (interpreting each 2-byte pair as big-endian u16):

| offset | baseline | touch | diff |
|---|---|---|---|
| 2-3 | 0x087f (2175) | 0x061f (1567) | -608 |
| 4-5 | 0x086d (2157) | 0x05a7 (1447) | -710 |
| 6-7 | 0x0867 (2151) | 0x0596 (1430) | -721 |
| 8-9 | 0x088f (2191) | 0x05d9 (1497) | -694 |
| 10-11 | 0x07f8 (2040) | 0x0598 (1432) | -608 |
| 12-13 | 0x08bc (2236) | 0x06d2 (1746) | -490 |

**A consistent, systematic downward shift of ~400-700 units across every sample point in the same direction.**
This is categorically different from an earlier (improperly-synchronized, no actual finger present) run-to-run
comparison, which showed only single-digit differences (noise-level). This coherent, structured shift is
exactly the kind of signature expected from a real physical touch changing the sensor's electrical/optical
readout baseline.

### Honest interpretation

CONFIRMED: the sensor's raw output changes in a consistent, structured, non-random way when a finger is
physically present, using the fully-traced capture pipeline (`img_scan_start` -> `image_read`/`fifo_read` ->
`img_scan_end`). This is strong evidence of genuine touch sensitivity, not noise.

NOT yet confirmed: whether this raw byte stream, once correctly reshaped/decoded (pixel layout, orientation,
bit-depth interpretation), forms a recognizable/viewable fingerprint ridge pattern. That requires further work
(likely image reconstruction/visualization, not yet attempted) and is the natural next validation step.

### Coordination note (process, not technical)
An earlier attempted touch-comparison in this session was invalid: it compared two no-finger baseline
captures against each other (the finger was not actually present during either capture due to a
synchronization mix-up) and showed only noise-level differences, as expected for two baseline reads. Corrected
by explicitly confirming finger placement before triggering, same as prior live hardware coordination in this
session.

## MAJOR MILESTONE: fingerprint ridge pattern visually confirmed in captured data (2026-09-16)

Status: CONFIRMED visually, with honest caveats on exact dimension confirmation

### Method
Two properly-synchronized captures were taken with `IMAGE_OUT_PATH` set to save the full 10240-byte raw
buffer (not just the truncated console preview): one with no finger present (baseline), one with finger
confirmed resting on the sensor before the capture was triggered (touch). Both saved to
`research/captures/2026-09-16-{baseline-nofinger,touch-confirmed}.raw`.

### Statistical comparison (full 5120-sample buffers, u16 big-endian)
```
baseline: min=0 max=3580 avg=2563.5
touch:    min=0 max=2955 avg=1884.2
diff:     min=-972 max=0 avg=-679.4   -- EVERY sample decreased, never increased
samples with |diff|>200: 4960/5120 (96.9%)
```
This is a massive, one-directional, near-universal shift -- not noise (an earlier, improperly-synchronized
comparison of two no-finger baselines showed only single-digit differences).

### Visual reconstruction
Reshaping the difference data as a 2D image, multiple candidate widths were tried (5120 total samples
factors cleanly into several aspect ratios). Width=64 (giving 64x80) produced a clearly recognizable
fingerprint ridge pattern -- flowing, naturally curved, roughly-parallel lines consistent with real ridge/
valley structure, including what appears to be ridge convergence structure. This was corroborated by
doubling the width (128x40) showing the exact same pattern repeated twice, and halving it (32x160) also
showing the pattern repeating -- strong evidence 64 is the true row width, not a coincidental reshape.

Saved: `research/captures/2026-09-16-ridge-pattern-diff-64x80.png` (contrast-enhanced, 2nd-98th percentile
stretch, upscaled for visibility).

### Honest assessment
CONFIRMED: real, substantial, spatially-structured signal correlated with physical touch, visually consistent
with fingerprint ridge/valley topology, obtained via this session's fully independently-derived (reverse
engineered from scratch, zero reliance on the proprietary binary at runtime) protocol implementation.

NOT independently confirmed: the exact true sensor resolution/orientation (64x80 is strongly evidence-backed
via the repeat-pattern test, but not confirmed against any authoritative spec), whether this is the raw
sensor's native pixel grid or requires further deinterleaving/correction, and how this diff-based
reconstruction relates to what a single raw (non-differenced) capture would need for real enrollment (which
presumably works from one capture, not a before/after diff -- the "baseline subtraction" logic already seen
in `fdt_base_Min_Updata` hints the real firmware does something conceptually similar itself).

This is not yet a working `fprintd-enroll`/`fprintd-verify` -- reaching that requires the remaining protocol
stages (AutoSDacUpdate calibration feedback, `poa_send_para`, and likely template extraction/matching, which
may be match-on-chip and not yet located). But this is the clearest, most direct evidence so far that this
project's reverse-engineered protocol can pull real, meaningful fingerprint data off the sensor.

## STEP 1 ANSWERED: match-on-chip architecture question resolved (2026-09-16)

Status: CONFIRMED via extensive symbol evidence in the proprietary binary

### Finding: this is HOST-SIDE software matching, NOT match-on-chip / secure-element comparison

The proprietary binary (`~/focaltech-ft9366-arch-shim/libfprint-2.so.2.0.0`) contains an entire proprietary
biometric SDK statically linked in, with hundreds of relevant symbols:

- Template extraction: `focal_GetImageTemplate`, `FtGetTemplate`, `FtGetTemplateForEnroll`,
  `FtDataToFocalTemplate`, `FtFocalTemplateToData`
- Template matching: `FtVerifyByTemplate`, `FtTemplate2TemplateMatch`, `FtVerifyTwoTemplate`,
  `focal_VerifyTwoTemplate`, `bz_match`, `bz_match_score`, `fpi_print_bz3_match`
- Enrollment flow: `focal_Enroll`, `focal_EnrollByImage`, `FtEnrollByTemplate`, `fp_device_enroll`,
  `fpi_device_enroll_complete`, `fpi_device_get_enroll_data`
- Verify flow: `fp_device_verify`, `fpi_device_verify_report`, `fpi_device_verify_complete`,
  `fpi_device_get_verify_data`

This confirms the binary is a **custom build of libfprint itself** (matching the earlier-found leftover build
path `chips/fw9366/fw9366_spider...` and "kylin" OS references) with a large proprietary FocalTech biometric
algorithm library statically linked in as the template-extraction/matching backend, exposed through
libfprint's own standard `fp_device_enroll`/`fp_device_verify` API. The sensor itself is confirmed to be a
"dumb" (relatively) image-capture device -- exactly matching what this session independently
reverse-engineered and validated (`img_data_get` pulling a real, visually-confirmed fingerprint ridge image).
All template extraction and comparison happens in **host-side software**, not inside a hidden secure element
on the chip.

### NSS/PK11 crypto usage -- CONFIRMED unrelated to the FT9366 path

Call sites of `NSS_NoDB_Init`, `PK11_ImportSymKey`, `PK11_CreateContextBySymKey`, `PK11_ParamFromIV`,
`PK11_CipherOp` (flagged in earlier binary analysis, before this session began) all trace to a single function
(`dev_init` at `0x31da0`) that also calls `fpi_device_uru4000_get_type` and references symbols `crkey` and
`uru4k_dev_info`. **URU4000** is a real, unrelated, mainline-libfprint-supported DigitalPersona fingerprint
reader with its own well-documented simple image-decryption scheme (a static key, historically called `crkey`
in real upstream libfprint source too). This crypto path belongs entirely to that separately-compiled-in
stock driver, not to any FocalTech/FT9366-specific code. **CONFIRMED: no crypto/secure-element layer protects
or is otherwise involved in the FT9366 capture, template, or matching path.**

### Practical implication for the rest of this session's plan (per Step 1's own instructions, not stopping to ask)

STEP 5 ("build the actual enroll/verify path") now clearly means: **host-side image/template matching is
required** -- there is no simple "ask the chip yes/no" command pair to find. This is confirmed to be a
larger, distinct sub-project (matching this session's own pre-flagged contingency): either integrating an
existing open fingerprint-matching approach (e.g. minutiae extraction + Bozorth3-style matching, as hinted by
the `bz_match`/`bz3` naming in the proprietary SDK, though that specific implementation is proprietary and not
being reused) or a simpler image-correlation approach sufficient to meet the stated bar (same finger reliably
matches, different finger/no finger reliably doesn't) as an initial milestone, with room to improve robustness
later. Proceeding to Steps 2-4 (calibration feedback, poa_send_para, frame data functions) first, since they
are still required regardless of the matching approach chosen, then returning to Step 5 with this scope
understood.

## Update: Img_Get_Avg_Middle traced, tested, and a real byte-order catch (2026-09-16)

Status: CONFIRMED (static disassembly + validated against real captured data + live hardware retest)

### fw9366_AutoSDacUpdate (img variant) call-site check
Only 1 caller, inside `fw9366_img_base_Update` -- confirmed part of our chain (matches the already-traced
call list). Calls `img_data_get` (already traced) twice, plus two new functions: `Img_Get_Avg_Middle` and
`fw9366_Img_Get_Better_DAC` (not yet traced, next).

### Img_Get_Avg_Middle(image_buf) -- median-via-histogram, pure local

```
divisor = 1 << Fw9366_cfg[0xa] = 4   (Fw9366_cfg[0xa]=2 confirmed via cfg_init)
histogram[0..1023] = 0
for row in 1..78:
  for col in 2..61:
    pixel = image_buf[row*64 + col]
    bucket = min(pixel / divisor, 1023)
    histogram[bucket]++; total++
return the bucket where the cumulative histogram sum first exceeds total/2 (the median)
```

**Independent confirmation of image width**: this function's row stride (64) exactly matches this session's
earlier VISUALLY-derived image width from the ridge-pattern reconstruction -- the real firmware's own
statistics code uses the same row width, not a coincidental reshape guess.

### Real byte-order bug caught and corrected via empirical validation, not literal disassembly reading

The literal disassembly shows a native (x86 little-endian) `movzx eax, WORD PTR [addr]` read directly on the
captured buffer, with no visible byte-swap step anywhere in `image_read`/`fifo_read`/`img_data_get` (unlike
the small frame_data path, which explicitly swaps). Taking this literally and computing the median with
little-endian interpretation on the real captured data produces **nonsense**: median pegged at the maximum
bucket (1023) for both baseline and touch, average ~31000 (consistent with near-random/noise data). The exact
same real data interpreted as **big-endian** produces physically plausible results matching everything already
confirmed: baseline median=665, touch median=489 (lower under touch, consistent with the already-confirmed
systematic brightness decrease).

**This implementation uses the empirically-validated big-endian interpretation**, not the literal disassembly
reading -- there is very likely a byte-swap step in the real chain not yet located (possibly inside
`ff_spi_read_image_buf` itself, not yet traced at the instruction level for its buffer handling, or elsewhere).
Flagging this as resolved-by-evidence rather than resolved-by-disassembly, and noting the discrepancy plainly
per this session's "never round an ambiguous result up" discipline -- the *behavior* is confirmed correct via
real data, the exact mechanism producing it in the original binary is not yet located.

### Live hardware retest
`Img_Get_Avg_Middle()` on a fresh no-finger capture returned 664, consistent with the earlier baseline (665).

### Next concrete action
Trace `fw9366_Img_Get_Better_DAC` (1700 bytes) -- takes the avg_middle value (or the image itself) and
computes an updated DAC setting; this is the actual feedback/convergence logic Step 2 is asking about.

## Update: STEP 2 -- AutoSDacUpdate architecture confirmed, deep threshold logic deliberately deprioritized (2026-09-16)

Status: CONFIRMED (architecture/call structure), NOT exhaustively traced (deliberate scope decision, explained below)

### fw9366_Img_Get_Better_DAC(...) call-site and structure

Called from `fw9366_AutoSDacUpdate` (img variant, single caller confirmed in our chain, inside
`img_base_Update`). Structure: performs up to 3 real image-capture cycles (`img_data_get`, already fully
traced and tested), evaluating each via `Img_Get_Out_Of_Range_Point` (new, un-traced helper -- counts
saturated/out-of-range pixels, ~245 bytes) and (for 2 of the 3 cycles) `Img_Get_Avg_Middle` (already traced).
Extensively reads/writes `REG9366` fields throughout (confirmed via disassembly) but **does not itself call
any sram_write/sfr_write** -- no direct hardware I/O beyond the image captures needed to evaluate each trial.

### Confirmed answer to Step 2's core question

**What gets tuned**: a DAC candidate value held in a `REG9366` field (the same field, `REG9366[0x87]`/related,
already confirmed consumed by `fdt_mode_init`'s and `img_mode_init`'s own SRAM writes to `0x1801` etc., both
already fully traced). This function does not apply the DAC to hardware directly -- it updates the host-side
candidate value, which takes effect the next time the already-traced init sequences run.

**Feedback signal**: derived from real captured images -- a count of out-of-range (saturated) pixels
(`Img_Get_Out_Of_Range_Point`) and the median pixel value (`Img_Get_Avg_Middle`, already traced and validated
against real data).

**Convergence/exit condition**: NOT exhaustively traced (see scope decision below) -- the function performs a
bounded number of capture-evaluate-adjust cycles (up to 3 visible in the call list) rather than an open-ended
loop.

### Deliberate scope decision, stated plainly

The remaining ~1700 bytes of fine-grained threshold/comparison logic (exact numeric conditions for how much
to adjust the DAC candidate per cycle) were NOT exhaustively traced. Reasoning: this session has already
empirically captured a real, visually-confirmed fingerprint ridge image (see the earlier milestone) using
ONLY the already-traced default/uncalibrated `img_mode_init`/`fdt_mode_init` DAC values -- i.e. this
calibration refinement is confirmed NOT to be a blocker for obtaining a usable capture, only a potential
image-quality optimization. Given the much larger and genuinely blocking remaining work (Step 5: real
host-side matching, which is currently entirely unimplemented and is the actual bar for "enroll/verify
works"), continuing to exhaustively trace this specific refinement logic now would be lower-value than
proceeding to the blocking work. This is a considered prioritization call, stated explicitly rather than
silently skipped -- revisit if captured image quality turns out to be insufficient for reliable matching.

### Next concrete action
Proceed to Step 3 (`poa_send_para`) and Step 4 (frame_data resolution) as planned, then prioritize Step 5
(real host-side matching) as the primary remaining focus.

## STEP 4 RESOLVED (mostly): frame_data all-zero was a pre-scan artifact, not a bug (2026-09-16)

Status: CONFIRMED via live retest -- hypothesis validated, one secondary nuance left open

### Test
Re-ran `fdt_get_a_frame_data()` (the small 8-byte calibration frame read at SRAM address `0xb8`) AFTER a full
real image scan (`img_data_get`, including `img_scan_start`'s arming sequence) had already run earlier in the
same session -- unlike the original test much earlier in this session, which called it chronologically BEFORE
any scan-arming had occurred.

### Result
```
sent: 04 fb 80 b8 00 04
recv: 08 79 08 79 08 79 08 79   -- stable, non-zero, structured (all 4 samples identical)
decoded value: 2169 (0x0879) for all 4 entries
fdt_base_fail_check(): FAIL (2169 > 0x2bc(700) -- now fails for being too HIGH, not too low/zero)
```

### Confirmed
The all-zero result from earlier in this session was a **pre-scan idle-state artifact**, not a protocol bug --
CONFIRMED by this retest showing real, non-zero, stable, sensor-derived data once a real scan had already
occurred. This resolves the primary question Step 4 asked.

### Still open (secondary, not blocking)
The value (2169) still fails `fdt_base_fail_check`'s valid range (300-700), now for being too high rather than
too low. Notably, 2169 (`0x0879`) is very close in magnitude to raw pixel values seen in this session's actual
captured images (e.g. baseline image's first pixel was `0x087f`=2175). This suggests the small FDT-mode frame
read may share underlying data with the full image capture, or requires additional FDT-specific
scan-priming distinct from the IMG-mode scan this session used to trigger it. Not resolved further -- flagged
honestly as a remaining nuance rather than claimed as fully understood, and not pursued further given it does
not block the higher-priority remaining work (Step 5: real matching).

## STEP 3 STATUS: poa_send_para -- deferred, not blocking

Per the same prioritization logic as Step 2: `poa_send_para`'s wire format was partially characterized in
earlier work (builds a command from several `REG9366` fields via `ff_spi_write_then_read_buf_rts`), but full
resolution was not completed this session. This is the final "commit calibration to chip" step in
`fw9366_init_chip`'s top-level sequence -- deferred in favor of Step 5 (real matching), which is the genuine
blocker for working enroll/verify. This session has already demonstrated that real fingerprint capture works
without needing `poa_send_para` resolved (the capture pipeline tested and confirmed end-to-end does not
depend on it). Revisit if real-world reliability issues arise that trace back to missing calibration
finalization.

## STEP 5 STATUS: matching approach tested against real data -- DOES NOT WORK, reporting honestly (2026-09-16)

Status: TESTED against real same-finger and different-finger captures. Result: FAILS to discriminate correctly.

### Test setup
Three additional live captures taken (same-finger x2, different-finger x1), saved to `research/captures/`:
`2026-09-16-baseline-nofinger.raw` (pre-existing), `match_touch1.raw`, `match_touch2.raw` (same finger, two
separate touches), `match_different.raw` (a different finger/contact, per user instruction).

Approach: background-subtract the no-finger baseline from each capture, optionally apply a local high-pass
filter (box-blur subtraction, radius 3) to emphasize fine texture over gross contact-area effects, then
compute normalized cross-correlation (NCC) with a small translational search (+/-6 px) to tolerate minor
repositioning, taking the best-shift correlation as the match score.

### Results -- INVERTED (wrong direction), tested twice with two variants

```
Without high-pass filter:
  SAME finger (touch1 vs touch2):        corr = 0.6343
  DIFFERENT (touch1 vs different):       corr = 0.7682
  DIFFERENT (touch2 vs different):       corr = 0.8212

With local high-pass filter (radius=3):
  SAME finger (touch1 vs touch2):        corr = 0.7021
  DIFFERENT (touch1 vs different):       corr = 0.8437
  DIFFERENT (touch2 vs different):       corr = 0.8488
```

**The same-finger comparison scores LOWER than both different-finger comparisons, in both variants tested.**
This is the wrong direction for a working matcher -- reporting this plainly rather than describing it as
"close" or "nearly working." This approach, as currently implemented, does NOT meet the stated bar (same
finger reliably matches, different finger reliably doesn't).

### Plausible contributing factors (hypotheses, NOT confirmed root causes)
- This session deliberately deferred the DAC auto-calibration feedback loop (Step 2's
  `Img_Get_Better_DAC`/`AutoSDacUpdate` fine-tuning) since it wasn't needed for basic capture. It's plausible
  that different touches land in different parts of the sensor's dynamic range without that calibration,
  introducing capture-to-capture variation that dominates over actual ridge differences.
- The sensor's effective resolution (64x80 = 5120 pixels total) is coarse; genuine fingerprint ridge pitch may
  not be well-resolved at this scale, especially without proper calibration.
- Translation-only alignment (no rotation search) may be insufficient even for same-finger comparisons if
  contact angle varies between touches.
- A single, possibly-stale no-finger baseline (captured earlier in the session) may not perfectly represent
  the true no-touch reference at comparison time, introducing systematic noise.
- Only one comparison pair per condition has been tested (small sample size) -- not enough data yet to
  distinguish a systematic problem from unlucky sampling.

### Honest status against this session's stated bar for Step 5
NOT MET. "Two consecutive real captures of the same finger reliably report match, different finger reliably
reports no match" has not been achieved. This is a genuinely open problem requiring further work -- either
debugging/improving the matching approach (with more sample data, calibration, rotation handling, or a
fundamentally different method such as real minutiae extraction) or accepting a different, lower initial bar.
Flagging this clearly rather than continuing to iterate silently, since it represents a real fork in how to
proceed for the remaining work (Steps 5 continuation and Step 6).

## STEP 1 (continuation session): real DAC calibration implemented and run to convergence (2026-09-16)

Status: CONFIRMED mechanism (traced from disassembly), pragmatic convergence achieved (not byte-exact
replication of the proprietary outer-loop structure -- stated honestly below)

### Newly traced and confirmed
`Img_Get_Out_Of_Range_Point(image, &out_low, &out_high)` (0x15cc47): pure local, same region as
`Img_Get_Avg_Middle` (rows 1-78, cols 2-61, stride 64). Counts pixels below `125<<Fw9366_cfg[0xa]`=500 into
`out_low`, and above `1003<<Fw9366_cfg[0xa]`=4012 into `out_high`.

`fw9366_Img_Get_Better_DAC`'s core adjustment step (confirmed via full disassembly trace): if both
`out_low<=19` and `out_high<=19`, decrement `REG9366[0x87]` (the DAC value) by 1; if either exceeds ~20,
increment by 1.

**This also resolved the earlier open byte-order question with certainty**: this function contains an
explicit in-place byte-swap of the captured image before its second internal capture, mechanistically
confirming (not just empirically inferring) that the wire data needs swapping before use -- matches this
session's earlier empirically-derived big-endian convention exactly.

### Honest scope note
The proprietary `AutoSDacUpdate`'s exact outer-loop structure (how many times it calls `Img_Get_Better_DAC`,
with what exact convergence/exit criteria across calls) was not fully traced. This session implements its own
outer convergence loop using the confirmed core per-step adjustment logic (+-1 based on out-of-range counts),
not a byte-exact replication of the original's iteration structure.

### Live test -- real calibration run to convergence, oscillation detected and handled

A naive +-1 stepping loop, run live against real hardware (no finger), **oscillated** between dac=0x33
(badly oversaturated: out_low=1413-1467, avg_middle~161-165) and dac=0x34 (good: out_low=7-9,
avg_middle~327-330) without settling -- reported plainly rather than claimed as clean convergence. Added
oscillation detection (stop when a dac value is revisited) plus a target-based "best observed" selection
(closest `avg_middle` to 500, the center of the already-confirmed valid range 300-700, among candidates
passing the out-of-range check) as a pragmatic fallback. Final result: **dac=0x35 selected, avg_middle=495**
-- close to the target, a well-centered exposure.

### Key finding: DAC value has a massive, real effect on image characteristics
`avg_middle` ranged from 165 (dac=0x33) to 660 (dac=0x36) across the values tried -- roughly 4x variation.
This confirms DAC calibration genuinely matters and is a very plausible contributor to the earlier matching
failure: uncalibrated captures could land at very different points in this range depending on incidental
conditions, adding large brightness-driven variance between captures of the same finger that has nothing to
do with the actual ridge pattern.

## STEP 2 RESULT: calibration + alignment tested with a full sample set -- STILL FAILS, robustly (2026-09-16)

Status: TESTED with a statistically meaningful sample set (10 live captures, 45 pairwise comparisons). Result:
correlation-based matching does NOT separate same-finger from different-finger, even with calibration and
rotation+translation alignment.

### Setup
All 10 captures taken at a SINGLE locked calibration point (`FIXED_DAC=0x35`, converged via the real
calibration loop above) to ensure consistent gain/fixed-pattern-noise conditions across the whole set --
critical, since calibrating separately per-capture would adapt to whatever's on the sensor at that moment and
invalidate background subtraction. avg_middle stayed tight (307-321) across all 10 captures, confirming
consistent gain was achieved.

- 1 no-finger baseline (`cal_baseline.raw`, avg_middle=496)
- 6 same-finger captures (`same1..6.raw`, avg_middle 309-321), repositioned slightly each time
- 4 different-finger captures (`diff1..4.raw`, avg_middle 307-315)

All saved to `research/captures/calibrated_set/`.

### Method
Background-subtract the baseline, local high-pass filter (radius 3), then search over rotation
(-6,-3,0,3,6 degrees, bilinear resample) x translation (+/-6px) for the best normalized cross-correlation.

### Full results (45 comparisons)
```
SAME-finger pairs (n=15):      min=0.7439  max=0.9401  avg=0.8305
SAME-vs-DIFFERENT pairs (n=24): min=0.7534  max=0.9548  avg=0.8489
DIFFERENT-vs-DIFFERENT (n=6):   min=0.7838  max=0.9435  avg=0.8651
```

**The three distributions overlap heavily and are not usefully separable.** DIFFERENT-vs-DIFFERENT actually
scores HIGHEST on average (0.8651), same-finger scores LOWEST (0.8305) -- backwards again, and now confirmed
with a much larger, more statistically meaningful sample than the earlier 3-capture test. This rules out
"small sample size" or "bad luck" as an explanation. Calibration and alignment (both now real, tested,
confirmed working individually) did not fix the underlying separation problem.

### Conclusion: this is not a tuning problem
Per this session's own pre-set trigger condition: correlation-based matching, even properly calibrated and
aligned, does not cleanly separate same vs. different finger on this sensor's captured data. Continuing to
adjust correlation parameters (filter radius, shift range, angle range, thresholds) is not expected to fix a
result this consistently and robustly wrong across 45 real comparisons. The likely explanation: at this
sensor's native resolution (64x80 = 5120 pixels total) and/or with the image quality achieved, whole-image
correlation is dominated by gross contact-area/pressure/moisture characteristics rather than the fine ridge
detail needed for identity discrimination -- exactly the class of problem real fingerprint systems solve with
minutiae extraction instead of raw image correlation, not incidentally.

### Next: STEP 3 -- evaluate minutiae-based matching
Per instructions, not tweaking correlation further. Proceeding to check whether libfprint itself exposes
reusable minutiae extraction/matching primitives before writing anything from scratch.

## STEP 3: real minutiae-based matching evaluated -- libfprint's own NBIS pipeline tested directly (2026-09-16)

Per this session's pre-set instruction: "check whether libfprint itself exposes reusable minutiae
extraction/matching primitives... before writing anything from scratch." It does: `libfprint/nbis/` bundles
NIST's MINDTCT (minutiae extraction, `get_minutiae()`) and Bozorth3 (minutiae matching,
`bozorth_probe_init()`/`bozorth_to_gallery()`), already built as `build/libfprint/libnbis.a` in the upstream
libfprint checkout at `~/Desktop/libfprint`. Every FpImageDevice-based driver in libfprint uses this pipeline
automatically via `fp_image_detect_minutiae()` (libfprint/fp-image.c) and `fpi_print_bz3_match()`
(libfprint/fpi-print.c) -- no driver writes its own minutiae code. This is a real, production, reusable
primitive, not something to build from scratch.

### Test performed
Wrote `tools/nbis_minutiae_test.c`, a standalone tool that links directly against `libnbis.a` and replicates
libfprint's own call sequence exactly (`get_minutiae()` with the real default `g_lfsparms_V2` params, then
`minutiae_to_xyt()` + `bozorth_probe_init()`/`bozorth_to_gallery()`, copied verbatim from
`libfprint/fpi-print.c`), run directly against the same 10 real captures from the calibrated set above (6
same-finger, 4 different-finger). Raw 16-bit BE pixel values were normalized to 8-bit via 1st/99th-percentile
linear scaling (real observed raw pixel range on this sensor: ~0-2022, not full 16-bit). Resolution passed to
MINDTCT: assumed 500 DPI (19.685 px/mm) -- the de-facto standard for fingerprint sensors (same constant
`libfprint/drivers/secugen.c` uses) -- since this sensor's true physical size/DPI is NOT independently
confirmed.

### Result: MINDTCT finds far too few minutiae per capture for Bozorth3 to ever produce a match
```
same1.raw: 1 minutiae   same4.raw: 1 minutiae   diff1.raw: 1 minutiae
same2.raw: 1 minutiae   same5.raw: 5 minutiae   diff2.raw: 1 minutiae
same3.raw: 3 minutiae   same6.raw: 2 minutiae   diff3.raw: 1 minutiae
                                                  diff4.raw: 1 minutiae
```
Reliability scores on the few minutiae found are very low (0.06-0.16; typical real MINDTCT output on
standard-sized fingerprint images has many minutiae with reliability 0.3-0.7+).

All 45 pairwise Bozorth3 scores (same-finger, same-vs-different, different-vs-different alike) are exactly
**0** -- reported plainly, not rounded up. This is not "barely failing to match" -- it's a floor effect.
Bozorth3 needs multiple corresponding minutiae with consistent geometric relationships between two prints to
produce any nonzero score at all; with only 1-5 minutiae total per capture, there usually aren't enough points
to even attempt correspondence, regardless of whether the two captures are the same finger or not. The
same-finger and different-finger cases are indistinguishable here for a trivial reason (no signal at all to
compare), not because a real discrimination attempt failed.

### Root cause: structural resolution mismatch, not a tunable parameter
MINDTCT's default block/window geometry (`MAP_BLOCKSIZE_V2`=8px, `MAP_WINDOWSIZE_V2`=24px sliding window for
ridge-flow direction estimation) is designed for images with hundreds of analysis blocks per axis (standard
NIST/FBI fingerprint images are typically 500x500px+ at 500 DPI). This sensor's 64x80px capture yields only a
handful of blocks with a full valid analysis window, especially near the edges where the 24px window doesn't
fit. This directly explains the observed 1-5 minutiae ceiling -- it is a structural property of the image
size relative to the algorithm's design assumptions, not a quality-threshold setting that can be relaxed away.
Lowering `MIN_CONTRAST_DELTA`/`PERCENTILE_MIN_MAX` etc. was considered but not expected to help materially,
since the limiting factor is the number of valid analysis *locations*, not per-location acceptance thresholds
-- not pursued further as a quick fix, per "don't keep tweaking parameters" guidance, since the mismatch is
structural.

### Honest conclusion
Stock NBIS/MINDTCT, as bundled in libfprint and used by every existing image-based driver, **does not work on
this sensor's native 64x80 single-shot captures** -- not a "needs tuning" result, a "wrong tool for this
resolution" result. This also retroactively explains why the vendor's own proprietary matcher
(`FtVerifyByTemplate`, confirmed host-side in STEP 1) almost certainly does NOT use generic minutiae
extraction either -- it was very likely custom-built/tuned specifically for this sensor's tiny native
resolution, which is presumably exactly why FocalTech didn't just reuse a stock algorithm.

This is a genuine fork in project direction with substantially different scope per option -- reporting to the
user for a decision rather than unilaterally committing to one:
1. Reverse-engineer the vendor's own proprietary matching algorithm (`FtVerifyByTemplate`/template format) --
   leverages an algorithm already proven to work at this exact resolution, but is a substantial new static
   RE sub-project on top of the wire-protocol work already done.
2. Multi-frame capture + stitching (analogous to how libfprint's `elan`/`elanspi` swipe drivers assemble many
   small frames into one larger composite image) to produce a larger effective image with enough real detail
   for stock MINDTCT to work on -- requires new capture-side design (frame registration/stitching), not just
   parameter tuning.
3. Design a small-area-sensor-specific matching approach (multiple enrollment captures per finger at
   different positions, direct patch correlation with more sophisticated preprocessing than the NCC approach
   already tried and shown not to separate finger identity) -- closer to what real small capacitive sensors
   in phones/laptops actually do, since minutiae-based approaches are known in the fingerprint literature to
   fail on small-area sensors for exactly the reason observed here.

### Decision: pursue vendor matcher reverse-engineering (2026-09-16)
User chose to reverse-engineer the vendor's own proprietary matching algorithm (`FtVerifyByTemplate`/
`focal_GetImageTemplate`) rather than multi-frame stitching or a custom small-area patch matcher, given it's
the only approach already proven to work at this sensor's actual resolution. Scope acknowledged upfront as a
real multi-day sub-project, not a quick fix. Starting with symbol enumeration and call-graph mapping in
`~/focaltech-ft9366-arch-shim/libfprint-2.so.2.0.0` before any linear tracing, per this session's standing
methodology.

### Major finding: the proprietary .so has full DWARF debug info, not stripped (2026-09-16)
`~/focaltech-ft9366-arch-shim/libfprint-2.so.2.0.0` retains full DWARF (`.debug_info`, `.debug_line`, etc.) --
real source file names (`FtAlg.c`, `FpSensorLib.c`), line numbers, and complete struct/function type
signatures, not just addresses. `gdb ptype /o` and `pahole` can pull exact struct layouts straight out of it.
This substantially changes the scope/risk of reverse-engineering the vendor matcher: it's closer to reading
labeled source than blind disassembly.

Confirmed via DWARF (not yet independently verified against real captures):
```c
typedef struct ST_FocalTemplate {           // 520 bytes total
    ST_Feature *pTemplateFeature;           // offset 0
    UINT8 *templateBinDiscr;                // offset 8
    UINT8 *templatePixValid;                // offset 16
    UINT32 headerSize, featBufSize, binBufSize, maskBufSize, templateSize, templateBinDiscrLen; // 24-47
    UINT16 templateExtendArea, subtemplatesPairIndex;   // 48-51
    FP32 subtemplatePairHmatrix[10];                    // 52-91
    ST_FocalSimpleHmatrix templateCoinHmatrix[96];       // 92-475 (384 bytes, member type not yet pulled)
    UINT8 nFeatureNum[2], templatePartsNum, templateArea, templateQuality,
          templateCondition, templateContrast, tempReplaceFlg,
          keepByte2..5, templateCoinFlag[25];            // 476-512
} ST_FocalTemplate;

typedef struct ST_Feature {    // 44 bytes -- a keypoint + BINARY DESCRIPTOR, not a classic ridge minutia
    FP32 x, y, ori;
    UINT32 bDescri[8];         // 256-bit binary descriptor (BRIEF/ORB-style), not just position+angle
} ST_Feature;

typedef struct ST_FocalSensorImageInfo {   // 5 bytes
    UINT8 quality, area, cond, contrast, reser;
} ST_FocalSensorImageInfo;

int    FtGetTemplate(UINT8 *image, ST_FocalTemplate *out_template, ST_FocalSensorImageInfo *info);
UINT16 FtVerifyTwoTemplate(ST_FocalTemplate *t1, ST_FocalTemplate *t2, FP32 *score_out, UINT8, UINT8);
SINT16 FtVerifyByTemplate(ST_FocalTemplate *, SINT16 *, SINT16 *, FP32 *, UINT8);
```

**This likely explains why the vendor's approach works at this sensor's tiny 64x80 resolution where stock
MINDTCT does not**: `ST_Feature` carries a 256-bit binary descriptor per keypoint (computer-vision-style
local descriptor matching, e.g. BRIEF/ORB-like), not just x/y/ridge-angle -- far more discriminating
information per keypoint than classic minutiae, so it needs far fewer keypoints to get a reliable match.

### Strategy fork: direct-offset-call the real .so functions vs. faithful reimplementation
`FtGetTemplate`/`FtVerifyTwoTemplate`/`FtVerifyByTemplate` are LOCAL symbols (confirmed via `readelf -s`: `FUNC
LOCAL`, not in `.dynsym`) -- not resolvable via plain `dlsym()`, but still callable in-process via a computed
address (`dlopen()` base + known file offset, cast to a function pointer), since the code is present and
loaded either way.

**This directly conflicts with this project's original goal, stated at the very start of the session: "zero
runtime dependency on any proprietary binary."** Not proceeding with direct-offset-calling as the shipped
architecture without checking with the user first. Flagged as a fork rather than assumed.

### Decision: reimplement cleanly, preserve zero-dependency goal (2026-09-16)
User chose to use the now-known struct layouts/function signatures to guide a faithful from-scratch
reimplementation, rather than calling into the real .so functions (even for quick validation) or shipping
against them directly. This preserves the original zero-runtime-dependency goal stated at the start of the
project. Proceeding to map FtGetTemplate's (feature extraction) and FtVerifyTwoTemplate's (matching) internal
call structure next, using DWARF-recovered function/variable names, before any linear reading.

### FtGetTemplate pipeline mapped (call graph, not yet linear-traced) (2026-09-16)
Per standing methodology (map call structure before linear reading), extracted `FtGetTemplate`'s real callee
list via `r2 axff` and cross-referenced against DWARF-recovered names. The debug log strings embedded in the
binary (e.g. `"FtGetTemplate...gAlgInfor.intvls=%d gAlgInfor.sigma=%f gAlgInfor.contrThr=%f gAlgInfor.curvThr=%d"`)
directly name the pipeline stages. Confirmed real pipeline, in execution order:

```
raw image bytes
  -> FtCreateImage                       (image buffer setup, x3 calls -- likely orig/working/scratch buffers)
  -> FtNonLinearStretch_U8                (42 cx, contrast stretch)                          [preprocessing]
  -> f9395_image_enhance                  (sensor-family-specific enhancement)                [preprocessing]
  -> FtLocalContrastEnhance               (52 cx, local contrast)                             [preprocessing]
  -> FtBadPixselDetect                    (bad pixel detection/correction)                    [preprocessing]
  -> FtGrayMeanSub                        (mean subtraction / normalization)                  [preprocessing]
  -> FtSegmentByLocalVariance             (25 cx, foreground/background segmentation
                                            -> produces templatePixValid mask)                [preprocessing]
  -> FtResize_8u                          (9 cx, resize -- direction/factor not yet confirmed) [preprocessing]
  -> InitSPAImageSize/MaskRadius/ImpactFactors + FtSpaSmooth   (smoothing pass)                [preprocessing]
  -> FtGetMfsFeatures                     (234 cx, 362 bbs, 17 args -- KEYPOINT DETECTION.
                                            Logs gAlgInfor.{intvls,sigma,contrThr,curvThr} --
                                            these are literally Lowe's SIFT parameter names
                                            [intervals-per-octave, sigma, contrast threshold,
                                            curvature threshold] -- strong evidence this is a
                                            SIFT-like DoG scale-space keypoint detector, not a
                                            classic ridge-ending/bifurcation minutiae detector.)
  -> FtGenBinImg / FtGenBinImgForSamllSensor / FtRepairGenBinImgForSamllSensor
                                           (186 / 27 cx -- binarization, WITH AN EXPLICIT
                                            SMALL-SENSOR-SPECIFIC CODE PATH, confirming the
                                            vendor built dedicated handling for exactly this
                                            sensor-size class)
  -> FtGetMfbFeatures                     (233 cx, 359 bbs, 10 args -- BINARY DESCRIPTOR
                                            computation per keypoint found above, populating
                                            ST_Feature.bDescri[8])
  -> assembled into ST_FocalTemplate
```

### Honest, now-concrete scope estimate
Not a small function -- `FtGetTemplate` orchestrates ~10 substantial subroutines. The two heaviest,
`FtGetMfsFeatures` (SIFT-like detection) and `FtGetMfbFeatures` (binary descriptor), are each comparable in
size/complexity to `FtVerifyTwoTemplate` itself (~230 cyclomatic complexity, ~360 basic blocks, ~8000 bytes of
code each). Combined with `FtVerifyTwoTemplate` (348 cx, 548 bbs) for matching, a faithful reimplementation
means tracing and reproducing roughly a dozen nontrivial functions, several architecturally comparable to a
full SIFT implementation plus a custom binary descriptor -- this is realistically **weeks, not days**, of
careful RE + reimplementation + testing, confirming and sharpening (not contradicting) the "real sub-project"
framing given when this direction was chosen. The DWARF debug info (real names, types, source line numbers)
meaningfully de-risks correctness during that work, but does not shrink the raw amount of logic involved.

Good news for feasibility: SIFT-like scale-space detection is a well-documented, publicly understood
algorithm family (unlike a from-scratch mystery algorithm), so the detection stage has strong reference
material available even though this exact implementation still needs to be traced from the binary. The binary
descriptor computation (`FtGetMfbFeatures`) and the small-sensor-specific binarization variants are the more
bespoke, FocalTech-specific parts requiring full RE from the binary with no external reference.

## STEP 1 (reimplementation): FtNonLinearStretch_U8 fully traced (2026-09-16)

Status: CONFIRMED (raw disassembly with DWARF-annotated register/parameter names via `r2 pdf`, cross-checked
against `gdb ptype` signatures for every callee -- not decompiler-guessed). r2dec (`r2pm -ci r2dec`, now
installed) was tried first for speed but its register-to-variable mapping was unreliable for args carried
across many blocks; raw DWARF-annotated disassembly was used for the authoritative trace instead. r2dec
remains useful as a fast first-pass overview for future functions, not as a final source of truth.

### Real algorithm (first pipeline stage after FtCreateImage)

```
FtNonLinearStretch_U8(UINT8 *src, SINT32 rows, SINT32 cols, UINT8 *dst):
  if src==NULL or dst==NULL: return -1
  n = rows*cols
  bufA = FP32[n]; bufB = FP32[n]; mask = UINT8[n]        (FtSafeAlloc, zeroed; return -2/-3 on alloc failure)
  for i in 0..n-1:
    bufA[i] = (float)src[i]
    bufB[i] = (float)src[i]
  FtImgBoxFilter(bufA, rows, cols, ksize=3, dst=bufA, normalize=1)   -- in-place 3x3 averaging box blur
  FtImgBoxFilter(bufB, rows, cols, ksize=5, dst=bufB, normalize=1)   -- in-place 5x5 averaging box blur
  for i in 0..n-1: bufA[i] -= bufB[i]                      -- band-pass: (3x3 blur) - (5x5 blur), DoG-like
  curved_surface_img_normalize_32f_2_8u(bufA, rows, cols, alpha=0.0, beta=250.0, dst=normImg[n])
                                                            -- min-max normalize float diff into byte [0,250]
  for i in 0..n-1: mask[i] = (src[i] > 0xfa) ? 1 : 0        -- saturation mask from ORIGINAL src (>250)
  curved_surface_img_localequalizehist_v2(src=normImg, mask=mask, rows, cols, dst=dst)
                                                            -- local histogram equalization, mask-aware
  FtImgGaussianblur(src=dst, rows, cols, ksize=3, sigma=-1.0 (auto), kernelPtr=NULL, dst=dst)
                                                            -- in-place 3x3 gaussian blur, auto-computed sigma
  for i in 0..n-1: if mask[i] != 0: dst[i] = 0xfe           -- force originally-saturated pixels to 254
  free bufA, bufB, mask
  return 9   -- CONFIRMED success sentinel is 9, not 0 (verified in disassembly, not assumed)
```

This is a local-contrast-enhancement filter (difference-of-box-blurs bandpass, i.e. DoG-like, akin to unsharp
masking) followed by masked local histogram equalization and a final smoothing blur, with explicit saturated-
pixel handling. Not a simple global contrast stretch despite the function name.

### Real signatures recovered (all via `gdb ptype`, parameter NAMES via DWARF-annotated `r2 pdf` comments)
```c
int  FtImgBoxFilter(FP32 *src, SINT32 rows, SINT32 cols, SINT32 ksize, FP32 *dst, UINT8 normalize);
int  FtImgGaussianblur(UINT8 *src, SINT32 rows, SINT32 cols, SINT32 ksize, FP32 sigma, SINT32 *kernelPtr, UINT8 *dst);
void curved_surface_img_normalize_32f_2_8u(FP32 *src, SINT32 rows, SINT32 cols, FP32 alpha, FP32 beta, UINT8 *dst);
int  curved_surface_img_localequalizehist_v2(UINT8 *src, UINT8 *mask, SINT32 rows, SINT32 cols, UINT8 *dst);
```
These four are shared image-utility primitives (the `curved_surface_img_` prefix suggests a distinct internal
utility library) -- almost certainly reused by the other preprocessing stages (`FtLocalContrastEnhance` etc.)
still to be traced. Plan: trace these primitives' own internals ONCE, then treat later pipeline stages as
compositions of already-understood primitives rather than re-deriving box-filter/gaussian-blur/histogram-eq
logic from scratch each time.

### Not yet done for this function
Internal logic of the four callees above is not yet traced (next). No live/intermediate-value validation
against the real .so yet -- deferred until enough of the pipeline is reimplemented to compare a real
end-to-end intermediate buffer (per Step 1's own validation methodology: dump intermediate buffers from the
real .so via gdb for the same input image, diff against our reimplementation).

## Finding: shared primitives are OpenCV-equivalent standard operations, not bespoke (2026-09-16)

Status: CONFIRMED for FtImgBorderInterpolate (exact signature/behavior match); FtImgBoxFilter/FtImgGaussianblur/
curved_surface_img_normalize_32f_2_8u confirmed by strong structural correspondence, not yet bit-exact-verified
against real OpenCV output.

`FtImgBorderInterpolate(SINT32 p, SINT32 len, SINT32 borderType)` is a byte-for-byte structural match to
OpenCV's public `cv::borderInterpolate(int p, int len, int borderType)` (same signature shape, same borderType
constant space: 0=CONSTANT, 1=REPLICATE, 2=REFLECT, 3=WRAP, 4=REFLECT_101). `FtImgBoxFilter` calls it with
`borderType=4` = `BORDER_REFLECT_101`, OpenCV's own default. `FtImgBoxFilter` itself uses the classic separable
running-sum box-filter algorithm (row-sum pass, then column-sum pass, single normalize-by-`ksize^2` division at
the end when `normalize=1`) -- textbook `cv::boxFilter(src, dst, CV_32F, Size(ksize,ksize), normalize=true,
BORDER_REFLECT_101)` semantics, not a bespoke algorithm.

**Practical implication for reimplementation**: `FtImgBoxFilter`, `FtImgGaussianblur` (almost certainly
`cv::GaussianBlur` with OpenCV's standard auto-sigma formula when sigma<=0, given identical calling
convention), and `curved_surface_img_normalize_32f_2_8u` (matches `cv::normalize(..., NORM_MINMAX, CV_8U)`)
can be reimplemented directly against **public, well-documented OpenCV semantics** rather than needing
bit-level RE of internal loop structure -- box filter and Gaussian blur are exact, well-specified operations
where any correct implementation matching kernel size/normalization/border-mode produces identical output, not
an approximation requiring the original's specific optimization (running-sum vs. naive) to be replicated.
This meaningfully narrows the genuinely-bespoke surface area needing full from-scratch RE down to:
`curved_surface_img_localequalizehist_v2` (mask-aware local histogram equalization -- not a stock OpenCV call,
since OpenCV's CLAHE has no mask parameter), the segmentation/resize/SPA-smoothing stages not yet traced, and
the two large SIFT-like/binary-descriptor stages.

Not yet done: bit-exact validation of this hypothesis against real captured data (deferred, per plan, until
enough of the pipeline is reimplemented to diff a real intermediate buffer via gdb).

## Update: curved_surface_img_localequalizehist_v2 partially traced -- genuine ambiguity flagged (2026-09-16)

Status: PARTIAL (structure confirmed via decompiler; inner histogram/counting logic NOT fully resolved --
flagging honestly rather than guessing)

### Confirmed structure
`curved_surface_img_localequalizehist_v2(UINT8 *src, UINT8 *mask, SINT32 rows, SINT32 cols, UINT8 *dst)`:
- Allocates a local `hist[256]` (SINT32) buffer, zeroed -- but notably it is NOT obviously indexed by pixel
  value in the visible decompiled logic (see ambiguity below), so "histogram" may be a misleading name for what
  this actually computes.
- Pads BOTH the source image and the mask via `curved_surface_img_makeborder_constprop_1` (an OpenCV
  `copyMakeBorder`-shaped helper, not yet independently traced) before processing -- confirmed via two
  back-to-back calls with matching argument shape.
- Padding size hints at an **asymmetric local window**: `(rows+54)` used for one buffer dimension vs. an inner
  loop bound of `cols+2`-ish for the other -- suggestive of a local window that is tall/narrow (elongated along
  one axis), which would be a sensible, deliberate design for ridge-oriented local processing rather than an
  arbitrary window shape. NOT independently confirmed as intentional -- flagged as a plausible reading, not
  fact.
- Per output pixel: scans a window of ~55 rows at the current column from the (bordered) buffer, accumulates
  some quantity into `edi` (count) and `edx` (sum) conditionally, then computes
  `output = (sum*256 - sum) / count = sum*255/count` as the final per-pixel value when `count != 0`
  (else leaves the pre-zeroed value, i.e. 0).

### Genuine ambiguity, not resolved
The decompiler's rendering of the inner 55-row scan loop reads a byte into a temporary (`esi = *(rcx)`) that is
never visibly used again before the loop's unconditional `edi += 0x37` -- i.e. it is not obviously gating on
the read value or writing it into a per-value histogram bucket the way a classic intensity histogram would.
This strongly suggests r2dec is dropping or obscuring a real conditional (very likely: only count/accumulate
when the corresponding MASK byte at that position is nonzero -- consistent with this function receiving a mask
parameter at all), but this has NOT been confirmed from raw disassembly yet -- explicitly not guessing further
here per this session's "don't round up ambiguous results" rule.

### Decision: defer full resolution, proceed with best-effort + empirical validation later
Fully hand-verifying this function's inner loop via raw disassembly (the same rigor applied to
`FtNonLinearStretch_U8`) would cost meaningfully more time, and this session's own validation plan already
calls for diffing real `.so` intermediate buffers via `gdb` once enough of the pipeline is reimplemented to do
so -- that empirical check will catch a wrong guess here regardless of how it's arrived at. Proceeding to the
next preprocessing stage now; will return to nail this function's exact inner logic with a raw-disassembly
pass (same method as `FtNonLinearStretch_U8`) before or during the intermediate-value validation pass, not
skipping it permanently.

## Update: four more preprocessing stages traced (2026-09-16)

Status: CONFIRMED (r2dec first-pass + spot-checked against DWARF-annotated signatures/constants; not yet
raw-disassembly-verified line-by-line the way FtNonLinearStretch_U8 was, since these four had no genuine
control-flow ambiguity in the decompiler output -- flagging that distinction honestly)

### f9395_image_enhance(UINT8 *src, SINT32 rows, SINT32 cols)
```
n = rows*cols
buf16 = UINT16[n]; for i: buf16[i] = (uint16)src[i]
FtImageEnhance_16u_v2(buf16, rows, cols, dst=buf16)     -- 16-bit enhancement, not yet traced internally
for i: src[i] = (uint8)(src[i]*0.65 + buf16[i]*0.35)    -- weighted blend, confirmed constants sum to 1.0
```

### FtGrayMeanSub(UINT8 *src, SINT32 rows, SINT32 cols, SINT32 ksize)
```
if src==NULL or ksize<=2: return -1
n = rows*cols; bufA=FP32[n]; bufB=FP32[n]; for i: bufA[i]=bufB[i]=(float)src[i]
FtBoxFilter_32f(bufA, rows, cols, ksize=3, dst=bufA, normalize=1)        -- fixed small blur
FtBoxFilter_32f(bufB, rows, cols, ksize=<param>, dst=bufB, normalize=1)  -- caller-specified larger blur
for i: bufA[i] -= bufB[i]                                                -- DoG-like bandpass, generalized
FtNormalize_32f_2_8u_constprop_11(bufA, rows, cols, alpha=0.0, beta=254.0, dst=src)  -- IN-PLACE overwrite
return 0
```
`FtBoxFilter_32f` shares the exact signature of the earlier-confirmed `FtImgBoxFilter` -- same shared
OpenCV-equivalent primitive family, different exported name (likely a compiler/linker artifact of multiple
translation units using the same static inline or a thin wrapper).

### FtBadPixselDetect(UINT8 *src, UINT16 rows, UINT16 cols, UINT8 *dst) -- produces a MASK, does not correct pixels
```
if src==NULL or dst==NULL: return -1
n = rows*cols
meanImg = UINT8[n]; FtMeanImage(src, rows, cols, ksize=1, dst=meanImg)   -- local mean filter
memset(dst, 1, n)
for each pixel i: if meanImg[i] != 0xff: dst[i] = 0    -- "bad" flag survives only where local mean saturates
FtErosion(dst, cols, rows, iterations=2)               -- morphological cleanup of the candidate mask
return 0
```
Correction to the earlier pipeline summary: this stage only DETECTS a bad-pixel-cluster mask (regions where the
local mean is fully saturated at 255, i.e. dead/stuck-high sensor pixel clusters); it does not itself correct
any pixel values. Not yet confirmed how/whether this mask is consumed downstream (`dst` buffer's later use not
yet traced -- flagged as open).

### FtLocalContrastEnhance(UINT8 *src, SINT32 rows, SINT32 cols, SINT32 ksize) -- full trace, real constants extracted
```
if src==NULL: return -1
n = rows*cols; bufMean=FP32[n]; bufVar=FP32[n]
FtGaussianBlur_8u(src, rows, cols, ksize=3, sigma=-1.0(auto), dst=src)    -- IN-PLACE pre-smoothing of src itself
sum=0
for i: bufMean[i]=(float)src[i]; bufVar[i]=(float)(src[i]*src[i]); sum+=src[i]
FtBoxFilter_32f(bufMean, rows, cols, ksize, dst=bufMean, normalize=1)     -- local mean
FtBoxFilter_32f(bufVar,  rows, cols, ksize, dst=bufVar,  normalize=1)     -- local mean-of-squares
globalMean = (float)(sum / n)             -- integer division then cast (invariant across pixels)
GAIN=0.2  FLOOR=1.0  (both CONFIRMED exact float constants extracted from .rodata)
for i:
  localVar = bufVar[i] - bufMean[i]^2
  localStd = (localVar > 0) ? max(sqrt(localVar), FLOOR) : FLOOR
  gain = (globalMean * GAIN) / localStd
  bufVar[i] = bufMean[i] + gain*(src[i] - bufMean[i])      -- adaptive contrast-enhanced float value (reuses bufVar)
(min,max) = min/max over bufVar[0..n-1]
epsilon = 1e-6 (CONFIRMED exact double constant); guards the (max-min) divide against a near-flat image
scale = 250.0 / (max-min)                 -- CONFIRMED exact float constant (same 250.0 used elsewhere)
for i: src[i] = (uint8)((bufVar[i]-min) * scale)   -- final min-max stretch, IN-PLACE overwrite of src
free bufMean, bufVar
return 0
```
This is a real, describable, standard-ish technique: local-mean/variance-based adaptive gain normalization,
structurally similar to the classic Hong/Wan/Jain fingerprint image normalization approach from the fingerprint
enhancement literature (target global statistics blended per-pixel based on local variance) -- not a fully
bespoke/mystery formula, which is good news for confident reimplementation. All four constants (GAIN=0.2,
FLOOR=1.0, epsilon=1e-6, SCALE=250.0) were extracted as exact IEEE-754 values from `.rodata`, not estimated.

### Running status of Step 1 (preprocessing chain)
Traced so far: FtNonLinearStretch_U8 (full), f9395_image_enhance (full, modulo FtImageEnhance_16u_v2 internals),
FtGrayMeanSub (full), FtBadPixselDetect (full), FtLocalContrastEnhance (full).
Remaining: FtSegmentByLocalVariance, FtResize_8u, SPA smoothing (InitSPAImageSize/MaskRadius/ImpactFactors +
FtSpaSmooth), FtGenBinImg/FtGenBinImgForSamllSensor/FtRepairGenBinImgForSamllSensor.

## STEP 1 WRAP-UP: preprocessing chain traced to a workable level (2026-09-16)

Status: MOSTLY CONFIRMED, with explicitly flagged lighter-touch items (not full raw-disassembly rigor on
every single function -- pragmatic pacing call given the much larger Step 2/3 functions still ahead; the
planned real-vs-reimplementation intermediate-buffer diff will empirically catch any error in these).

### FtResize_8u(UINT8 *src, SINT32 srcRows, SINT32 srcCols, UINT8 *dst, SINT32 dstRows, SINT32 dstCols)
CONFIRMED: standard fixed-point (12-bit, 0x1000 scale) bilinear interpolation resize -- a well-documented,
exactly-reproducible technique (same class of algorithm as OpenCV's `resize()` fixed-point path), not bespoke.
NOT YET DETERMINED: the actual srcRows/cols -> dstRows/cols values used at the real call site inside
`FtGetTemplate` (both src and dst are `ST_IplImage*` structs whose width/height fields are set earlier in the
function from local stack structs not yet traced back to their origin) -- deferred to the empirical validation
phase rather than chased further now, since it doesn't change the resize *algorithm*, only its parameters.

### SPA smoothing group -- lighter-touch trace
`InitSPAImageSize(col,row)` / `InitSPAMaskRadius(rad)` / `InitSPAImpactFactors(zoomRatio)`: trivial setters
into a global `gSPApara` struct (cyclomatic complexity 1 each, fully confirmed, nothing to misread).
`FtSpaSmooth(UINT8 *src, UINT16 impactFactor)`: computes an impact-factor-derived scale value from
`gSPApara`'s radius, calls `FastConv(src, col)` (a fast box/mean convolution, not independently traced) and
then a further step via a raw function-pointer tail call (address `0x11a0b0`, not resolved to a named symbol
this pass). Structurally a smoothing operation consistent with its place in the pipeline; NOT bit-exact traced
-- flagged honestly rather than guessed further.

### FtGenBinImgForSamllSensor(ST_IplImage *img, UINT64 **pArr, UINT16 *arrLen) -- CONFIRMED, clear algorithm
```
n = img->width * img->height
copyImg=UINT8[n]; medImg=UINT8[n]; binImg=UINT8[n]   (FtSafeAlloc)
copy img->imageData rows into copyImg (respecting img->widthStep)
FtMedianFilter(copyImg, cols, rows, ksize=1, dst=medImg)         -- median filter (small kernel)
FtLocalThreshold(medImg, cols, rows, 1, blockSize=5, constC=<const@0x188398>, dst=binImg)  -- adaptive threshold
bitArr = UINT64[ceil(n/64)]  (FtSafeAlloc, zeroed)               -- packed bitset output
for each pixel i where binImg[i] != 0: FtSetBitValue_1(bitArr, wordsPerRow, i, 1)   -- pack into bitset
*pArr = bitArr; *arrLen = ceil(n/64)
free copyImg, medImg, binImg (only the packed bitset is returned)
```
This is a real, standard technique (median filter -> adaptive threshold -> bit-packed binary mask), matching
the "binarization" role already inferred from the pipeline map. The bit-packed output is almost certainly what
`FtGetMfsFeatures` scans next to know which pixels are candidate foreground/ridge locations.
`FtRepairGenBinImgForSamllSensor` (33 cx, a small variant/fixup pass) NOT yet traced -- noted, deferred.

### Genuinely deferred items (explicit, not silently dropped)
- `curved_surface_img_localequalizehist_v2`'s exact inner accumulation/masking logic (ambiguity flagged earlier)
- `FtImageEnhance_16u_v2` internals (called by `f9395_image_enhance`)
- `FastConv` and the unresolved tail-call target inside `FtSpaSmooth`
- `FtRepairGenBinImgForSamllSensor`
- Exact resize dimensions (algorithm confirmed, parameters not)
- `FtMeanImage`, `FtErosion`/`FtErode`/`FtDilate`, `FtMedianFilter`, `FtLocalThreshold` internals -- all treated
  as standard, well-understood image-processing primitives (mean filter, morphological erode/dilate, median
  filter, adaptive threshold) by name and calling convention, not independently disassembled line-by-line.

### Decision: proceed to Step 2 (FtGetMfsFeatures)
The preprocessing chain is understood well enough to attempt a first reimplementation pass, with the explicit
plan to validate against real `.so` intermediate buffers (via `gdb`) once enough of the full pipeline exists to
make that comparison meaningful -- that step will also resolve the deferred ambiguities above empirically.
Continuing to perfect every preprocessing function in isolation before touching the much larger and more
critical `FtGetMfsFeatures`/`FtGetMfbFeatures` stages would not be the highest-value use of time right now.

## MAJOR FINDING: FtGetMfsFeatures is an adapted OpenSIFT implementation, not bespoke (2026-09-16)

Status: CONFIRMED with high confidence via exact struct-field-name and default-constant correspondence to the
public OpenSIFT reference implementation (Rob Hess, github.com/robwhess/opensift) -- verified via WebFetch
against opensift's actual source (`src/sift.c`, `include/sift.h`), not from memory/assumption.

### The decisive match
`FtGetMfsFeatures`'s real signature (via `gdb ptype`):
```c
struct ST_EXTREMUM_NUM { SINT32 nMaxExtremum; SINT32 nMinExtremum; }
  FtGetMfsFeatures(ST_InputForTemplate inPara, ST_Feature **feat1, ST_Feature **feat2);

struct ST_InputForTemplate {   // 72 bytes
  SINT32 intvls; FP32 sigma; FP32 contrThr; SINT32 curvThr; SINT32 imgDbl;
  SINT32 descrWidth; SINT32 descrHistBins; ST_IplImage *img; UINT8 octave;
  UINT8 validArea; UINT8 *validFlg; UINT8 *badPixselValidFlg; UINT8 isFT9391;
  UINT8 algType; UINT8 sensorCol; UINT8 isSpeedUp; FP32 imgScale;
};
```
Field names `intvls`, `sigma`, `contrThr`, `curvThr`, `imgDbl`, `descrWidth`, `descrHistBins` are an EXACT match
to OpenSIFT's `SIFT_INTVLS`/`SIFT_SIGMA`/`SIFT_CONTR_THR`/`SIFT_CURV_THR`/`SIFT_IMG_DBL`/`SIFT_DESCR_WIDTH`/
`SIFT_DESCR_HIST_BINS` macro names -- not a coincidental naming overlap (7 distinct field names matching in
both spelling and role is not plausible by chance). The return type's field names (`nMaxExtremum`,
`nMinExtremum`) match SIFT's own "scale-space extrema" terminology exactly. **FocalTech's implementation is
very likely a direct adaptation of OpenSIFT (or a shared common ancestor with identical parameter-naming
conventions), not an independently-designed bespoke algorithm.**

### Real runtime constants, extracted from `.data` (not `.bss` -- these are the actual compiled-in defaults)
`gAlgInfor` (`ST_FocalAlgInfo`, address `0x211a70`, 16 bytes, in `.data`) holds:
```
intvls   = 3       (OpenSIFT default: 3     -- IDENTICAL)
sigma    = 1.6     (OpenSIFT default: 1.6   -- IDENTICAL)
contrThr = 0.02    (OpenSIFT default: 0.04  -- HALVED)
curvThr  = 15      (OpenSIFT default: 10    -- +50%)
```
Both deviations from stock OpenSIFT make the detector MORE PERMISSIVE (lower contrast threshold admits more
candidate keypoints; higher curvature threshold rejects fewer edge-like points) -- a small, well-reasoned,
understandable customization consistent with compensating for this sensor's tiny 64x80 native resolution,
not an arbitrary or mysterious tuning choice.

### Practical implication: reference-guided reimplementation, not blind RE
OpenSIFT's public pipeline (verified from its actual source): `create_init_img` (grayscale + optional 2x
doubling) -> `build_gauss_pyr` -> `build_dog_pyr` -> `scale_space_extrema` -> `calc_feature_scales` ->
`adjust_for_img_dbl` -> `calc_feature_oris` -> `compute_descriptors`. Given `FtGetMfsFeatures` returns extrema
counts and populates two `ST_Feature**` arrays (max/min extrema) but does NOT itself produce the binary
descriptor (that's `FtGetMfbFeatures`, traced separately), it most likely covers OpenSIFT's stages 1-7
(through orientation assignment) or a subset ending at localized/oriented extrema, with `FtGetMfbFeatures`
covering only descriptor computation (using FocalTech's bespoke 256-bit binary descriptor instead of OpenSIFT's
128-float gradient histogram, per the earlier-confirmed `ST_Feature.bDescri[8]` layout) -- not yet confirmed
exactly where the split falls, to be resolved via call-graph mapping next.

This substantially de-risks and likely shortens the remaining Step 2/3 work: rather than blind disassembly,
the plan is now to map `FtGetMfsFeatures`'s real callees against OpenSIFT's known stage names/order, and focus
RE effort specifically on identifying WHERE FocalTech's implementation matches stock OpenSIFT behavior
(reusable as-is, verified against public reference) versus where it's been customized for this sensor
(genuinely needs disassembly-level attention). The `imgDbl` field's actual runtime value (0 or 1) at the real
call site was not yet pinned down (deferred -- the struct is passed by 9 raw stack pushes, not individually
labeled in the decompiler output; will resolve via the same intermediate-value validation pass rather than
hand-mapping 9 push offsets by hand right now), but `FtResize_8u`'s position immediately before this stage in
the pipeline is strong circumstantial support for `imgDbl=1` (OpenSIFT's `create_init_img` does exactly this
doubling step when enabled).

## FtGetMfsFeatures call graph mapped -- near-total OpenSIFT correspondence confirmed (2026-09-16)

Status: CONFIRMED (real callee names extracted from disassembly, not inferred)

Full list of named callees inside `FtGetMfsFeatures`, extracted directly from disassembly:
```
FtCreateInitImg   FtBuildGaussPyr   FtBuildDogPyr   FtScaleSpaceExtrema   FtCalcFeatureScales
FtAdjustForImgDbl FtComputeDescriptors  FtDominantOri  FtSmoothOriHist  FtAddGoodOriFeatures
FtMfsCalcGradMagOri
FtCreateMemStorage  FtCreateSeq  FtSeqPush  FtSeqPop  FtSeqPopFront  FtGetSeqElem  FtSeqSort
FtReleaseMemStorage  FtReleasePyr  FtClearDogPyr  FtReleaseImage
FtGetKpNumMode4  FtInValidPixelSet     (no OpenSIFT equivalent -- FocalTech-specific, see below)
```

**Nearly every name maps 1:1 to OpenSIFT's actual internal function names**: `FtCreateInitImg`/
`create_init_img`, `FtBuildGaussPyr`/`build_gauss_pyr`, `FtBuildDogPyr`/`build_dog_pyr`,
`FtScaleSpaceExtrema`/`scale_space_extrema`, `FtCalcFeatureScales`/`calc_feature_scales`,
`FtAdjustForImgDbl`/`adjust_for_img_dbl`, `FtDominantOri`/`FtSmoothOriHist`/`FtAddGoodOriFeatures` map to
OpenSIFT's own internal orientation-assignment helpers (dominant-orientation peak finding, histogram
smoothing, adding features for multiple strong orientation peaks), and `FtMfsCalcGradMagOri` matches OpenSIFT's
`calc_grad_mag_ori` gradient helper. Beyond the algorithm itself, **`FtCreateMemStorage`/`FtCreateSeq`/
`FtSeqPush`/`FtSeqPop`/`FtSeqPopFront`/`FtGetSeqElem`/`FtSeqSort`/`FtReleaseMemStorage` are functionally
OpenCV's `CvMemStorage`/`CvSeq` container API** (`cvCreateMemStorage`/`cvCreateSeq`/`cvSeqPush`/etc.) -- which
is exactly the dynamic keypoint-list container OpenSIFT itself uses internally. This is about as strong a
correspondence as static analysis can establish without literal source access: FocalTech's detector is a
ported/adapted OpenSIFT running on an OpenCV-CvSeq-equivalent container library, not an independent design.

**Two genuinely FocalTech-specific additions, no OpenSIFT equivalent**: `FtGetKpNumMode4` (name suggests a
sensor/mode-specific keypoint-count policy) and `FtInValidPixelSet` (very likely applies the
already-traced `FtSegmentByLocalVariance` foreground mask and/or `FtBadPixselDetect` bad-pixel mask to exclude
invalid image regions from keypoint consideration -- the natural integration point tying this stage to the
earlier-traced preprocessing chain). `FtComputeDescriptors` appearing here (not only in `FtGetMfbFeatures`) is
noted but not yet resolved -- possibly computes an intermediate gradient-histogram-based representation feeding
`ST_FocalTemplate.templateBinDiscr` (distinct from each `ST_Feature.bDescri`, which is presumably
`FtGetMfbFeatures`'s output) rather than a final descriptor; flagged as an open question for the
`FtGetMfbFeatures` phase rather than guessed now.

### Revised plan for the rest of Step 2
Cross-reference each of these functions against OpenSIFT's actual public C source (`src/sift.c`, verified
reachable via WebFetch) function-by-function, focusing effort on: (a) confirming stock behavior is unchanged
where no customization is evident, (b) precisely nailing the two confirmed parameter deviations' effects
(`contrThr=0.02`, `curvThr=15`), and (c) fully tracing the two FocalTech-specific functions
(`FtGetKpNumMode4`, `FtInValidPixelSet`) and resolving the `FtComputeDescriptors`-in-two-places question --
rather than disassembling every stock-equivalent function from zero as if its behavior were unknown. This
meaningfully changes the Step 2 time estimate for the better versus the pre-discovery "weeks" framing, though
the two FocalTech-specific functions and the descriptor-split question still require genuine RE.

## Update: small FtGetMfsFeatures helper functions confirmed against OpenSIFT reference (2026-09-16)

Status: CONFIRMED via WebFetch of OpenSIFT's actual public source (`src/sift.c`) cross-checked against real
disassembly/decompile output.

- `FtGetKpNumMode4()` -- trivial: calls `FtSensorTypeGet()` (return value discarded) and returns a fixed
  constant `0xa0` = 160. No OpenSIFT equivalent; this is FocalTech's max-keypoint-count cap for this sensor
  mode, confirmed as a plain fixed value, not computed.
- `FtCalcFeatureScales(ST_Seq *features, FP32 sigma, SINT32 intvls)` -- structurally matches OpenSIFT's
  `calc_feature_scales`: for each feature, `scl = sigma * pow(2.0, someIntervalField/intvls)`. Only ONE scale
  field is written per feature (not two, unlike OpenSIFT's separate `feat->scl`/`ddata->scl_octv`) -- plausible
  explanation: FocalTech's internal per-feature record folds octave scaling into the interval field differently
  than OpenSIFT's split representation; not fully disambiguated, flagged rather than asserted.
- `FtAdjustForImgDbl(ST_Seq *features)` -- matches OpenSIFT's `adjust_for_img_dbl`: multiplies each feature's
  x,y by a confirmed exact constant of **0.5** (extracted from `.rodata`, not estimated). Only x,y are scaled
  here (not a separate `scl`/`img_pt` pair like OpenSIFT, consistent with a more compact internal record used
  during detection before conversion to the final 44-byte `ST_Feature`). **This function's mere presence and
  active use of a 0.5 halving constant is itself strong indirect confirmation that `imgDbl=1` (image doubling)
  is genuinely active in the real pipeline** -- consistent with `FtResize_8u` running immediately before this
  stage (per the earlier-mapped pipeline), resolving that previously-open question with reasonable confidence
  without needing to hand-trace the 9-field stack-push struct construction.

### Next
`FtScaleSpaceExtrema` (89 cx, 139 bbs -- the core detection loop, directly uses `contrThr`/`curvThr`) and
`FtInValidPixelSet` (35 cx -- FocalTech-specific, likely integrates the earlier-traced segmentation/bad-pixel
masks) are next, being the two highest-value remaining pieces in this stage.
