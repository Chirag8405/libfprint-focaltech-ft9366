#!/usr/bin/env python3
"""Extract likely FT9366 control transfer tuples from a pcap using tshark."""

from __future__ import annotations

import argparse
import csv
import subprocess
import sys
from pathlib import Path


FIELDS = [
    "frame.number",
    "usb.setup.bmRequestType",
    "usb.setup.bRequest",
    "usb.setup.wValue",
    "usb.setup.wIndex",
    "usb.setup.wLength",
    "usb.capdata",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Analyze FT9366 usbmon capture")
    parser.add_argument("pcap", help="Path to pcap/pcapng file")
    parser.add_argument("--vendor", default="0x2808", help="USB vendor id filter (default: 0x2808)")
    parser.add_argument("--markdown", help="Optional markdown output path")
    return parser.parse_args()


def run_tshark(pcap: str, vendor: str) -> list[list[str]]:
    display_filter = f"usb.idVendor == {vendor} && usb.setup.bRequest"
    cmd = [
        "tshark",
        "-r",
        pcap,
        "-Y",
        display_filter,
        "-T",
        "fields",
        "-E",
        "header=n",
        "-E",
        "separator=,",
    ]

    for field in FIELDS:
        cmd.extend(["-e", field])

    proc = subprocess.run(cmd, text=True, capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or "tshark failed")

    rows: list[list[str]] = []
    for row in csv.reader(proc.stdout.splitlines()):
        if not row:
            continue
        while len(row) < len(FIELDS):
            row.append("")
        rows.append(row)
    return rows


def to_markdown(rows: list[list[str]]) -> str:
    header = "| frame | bmRequestType | bRequest | wValue | wIndex | wLength | capdata |"
    sep = "|---:|---|---|---|---|---:|---|"
    lines = [header, sep]

    for row in rows:
        frame, bm_req, b_req, w_val, w_idx, w_len, cap = row
        lines.append(
            f"| {frame or '-'} | {bm_req or '-'} | {b_req or '-'} | {w_val or '-'} | {w_idx or '-'} | {w_len or '-'} | {cap or '-'} |"
        )

    return "\n".join(lines)


def main() -> int:
    args = parse_args()

    try:
        rows = run_tshark(args.pcap, args.vendor)
    except FileNotFoundError:
        print("error: tshark not found. Install wireshark/tshark.", file=sys.stderr)
        return 1
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    md = to_markdown(rows)
    print(md)

    if args.markdown:
        out = Path(args.markdown)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(md + "\n", encoding="utf-8")
        print(f"\nwritten markdown table to: {out}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
