#!/usr/bin/env python3
"""Capture a FocalTech FT9366 session from usbmon.

This helper is intentionally minimal and shell-command transparent.
It wraps the sequence needed by protocol discovery in a reproducible way.
"""

from __future__ import annotations

import argparse
import os
import shlex
import signal
import subprocess
import sys
import time
from pathlib import Path


def _with_sudo(cmd: list[str], use_sudo: bool) -> list[str]:
    if not use_sudo:
        return cmd
    if os.geteuid() == 0:
        return cmd
    return ["sudo", *cmd]


def run(cmd: list[str], *, use_sudo: bool = False, check: bool = True) -> subprocess.CompletedProcess[str]:
    full = _with_sudo(cmd, use_sudo)
    print("+", shlex.join(full))
    return subprocess.run(full, text=True, check=check)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Capture FT9366 usbmon traffic")
    parser.add_argument("--bus", type=int, default=3, help="usbmon bus number (default: 3 -> usbmon3)")
    parser.add_argument("--output", default="research/captures/focaltech_session.pcap", help="Output pcap path")
    parser.add_argument("--restart-fprintd", action="store_true", help="Restart fprintd before enroll")
    parser.add_argument("--enroll-user", help="Run fprintd-enroll for this user")
    parser.add_argument("--finger", default="right-index-finger", help="Finger name for fprintd-enroll")
    parser.add_argument("--duration", type=float, default=0.0, help="Optional timed capture duration in seconds")
    parser.add_argument("--no-sudo", action="store_true", help="Do not prefix privileged commands with sudo")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    use_sudo = not args.no_sudo

    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    try:
        run(["modprobe", "usbmon"], use_sudo=use_sudo)
    except subprocess.CalledProcessError as exc:
        print(f"error: failed to enable usbmon: {exc}", file=sys.stderr)
        return 1

    iface = f"usbmon{args.bus}"
    tcpdump_cmd = _with_sudo(["tcpdump", "-i", iface, "-w", str(out_path)], use_sudo)
    print("+", shlex.join(tcpdump_cmd))

    proc = subprocess.Popen(tcpdump_cmd)
    try:
        time.sleep(0.8)

        if args.restart_fprintd:
            run(["systemctl", "restart", "fprintd"], use_sudo=use_sudo)

        if args.enroll_user:
            run(["fprintd-enroll", "-f", args.finger, args.enroll_user], check=False)

        if args.duration > 0:
            print(f"capturing for {args.duration:.1f}s")
            time.sleep(args.duration)
        elif not args.enroll_user:
            input("Capture running. Press Enter to stop... ")
    except KeyboardInterrupt:
        pass
    finally:
        proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    print(f"capture saved to: {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
