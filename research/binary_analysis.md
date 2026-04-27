# Binary Analysis Log

Date: 2026-04-26
Target: `libfprint-2.so.2.0.0` (from shim archive)

## Tooling available during analysis
- Available: `nm`, `readelf`, `objdump`, `strings`
- Missing: `ghidra`, `radare2`

## Requested string search groups

The exact required searches were executed:

```bash
strings libfprint-2.so.2.0.0 | grep -iE "aes|key|iv|encrypt|session|handshak|auth|token"
strings libfprint-2.so.2.0.0 | grep -iE "focal|ft9|rts|9366|a658|2808|vendor"
strings libfprint-2.so.2.0.0 | grep -iE "send|recv|request|response|packet|cmd|transfer"
```

Match counts:
- Search 1 (crypto terms): 1118
- Search 2 (device/vendor terms): 2312
- Search 3 (transport terms): 746

## Dynamic symbol evidence

```bash
nm -D libfprint-2.so.2.0.0 | grep -E "PK11_|NSS_|g_usb_device_(control|bulk|interrupt)_transfer|fp_context_new|fw9366|query_event"
```

Notable output:
- `fp_context_new@@LIBFPRINT_2.0.0`
- `g_usb_device_control_transfer@LIBGUSB_0.1.0`
- `g_usb_device_bulk_transfer@LIBGUSB_0.1.0`
- `g_usb_device_interrupt_transfer@LIBGUSB_0.1.0`
- `NSS_NoDB_Init`
- `PK11_CipherOp`
- `PK11_CreateContextBySymKey`
- `PK11_ImportSymKey`
- `PK11_ParamFromIV`

## Function symbols recovered (non-stripped locals present)

```bash
nm -a libfprint-2.so.2.0.0 | grep -iE "fw9366|chipid|query_event|fdt|focaltech"
readelf -Ws libfprint-2.so.2.0.0 | grep -iE "fw9366|chipid|query_event|fdt|focaltech"
```

High-value symbols:
- `fw9366_query_event_status` at `0x151822` (size 3641)
- `fw9366_probe_id` at `0x152975`
- `_Z17fw9366_chipid_getv` at `0x154955` (size 47)
- `_Z32ft_feature_devinit_JudgeByChipIdPh` at `0x1535ad` (size 100)

## Disassembly excerpts

### 1) Event query function

```bash
objdump -d -M intel --start-address=0x151822 --stop-address=0x15265b libfprint-2.so.2.0.0
```

Observed behavior:
- Heavily logs and branches on event/interrupt state
- Calls into communication helper(s) and status helpers
- No direct PK11 or direct control-transfer PLT calls in this wrapper

### 2) Chipid function

```bash
objdump -d -M intel --start-address=0x154930 --stop-address=0x154a20 libfprint-2.so.2.0.0
```

Observed behavior in `_Z17fw9366_chipid_getv`:
- `mov edi, 0x1a8b`
- call `_Z16fw9366_sram_readt`
- return register value

Interpretation:
- Chip ID is read from a specific register/SRAM path, likely requiring successful lower-level transport state.

### 3) Shared control transfer helper

```bash
objdump -d -M intel --start-address=0x56280 --stop-address=0x56330 libfprint-2.so.2.0.0
```

Observed:
- A call to `g_usb_device_control_transfer@plt` in helper path around `0x562e8`

## Initial reverse-engineering conclusions

1. Protocol stack is layered: fw9366 logic -> local helper wrappers -> libgusb transfer functions.
2. Crypto setup clearly depends on NSS PK11 API and is not optional.
3. Failing chipid (`0x0`) likely indicates broken lower-level transfer formatting or precondition sequence before SRAM read is valid.
4. The earliest viable breakpoint for behavioral parity is the path reaching `_Z17fw9366_chipid_getv` after successful communication checks.

## Gaps still open

- Exact control transfer tuple for chipid command (needs usbmon/strace capture)
- Key material source or derivation near `PK11_ImportSymKey` call site
- Expected interrupt payload format for down/up/data states
