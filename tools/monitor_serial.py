#!/usr/bin/env python3
"""
Opens the port with raw POSIX termios, matching tools/handheld-control.py's
SerialPort -- no DTR/RTS toggle on open, so connecting this doesn't reset the
board (pyserial's default open does, which would make an overnight capture
useless). Every line is appended to the log file with a timestamp; only
[POWER-DIAG] lines (or everything, with --all) are also echoed to the
terminal. Reconnects automatically if the device resets or is unplugged, so
this can be left running unattended overnight.
"""

from __future__ import annotations

import argparse
import fcntl
import glob
import os
import select
import sys
import termios
import time

BAUD = 115200
MARKER = "[POWER-DIAG]"
MAX_LINE = 4096


def find_port() -> str | None:
    candidates = sorted(
        glob.glob("/dev/cu.usbserial-*")
        + glob.glob("/dev/cu.wchusbserial*")
        + glob.glob("/dev/ttyUSB*")
        + glob.glob("/dev/ttyACM*")
    )
    return candidates[0] if candidates else None


def open_port(path: str) -> int:
    fd = os.open(path, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        settings = termios.tcgetattr(fd)
        settings[0] = settings[1] = settings[3] = 0  # iflag, oflag, lflag: raw
        settings[2] = termios.CS8 | termios.CLOCAL | termios.CREAD
        settings[4] = settings[5] = getattr(termios, f"B{BAUD}")
        settings[6][termios.VMIN] = settings[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, settings)
    except BaseException:
        os.close(fd)
        raise
    return fd


def read_lines(fd: int, pending: bytearray):
    if not select.select([fd], [], [], 0.5)[0]:
        return
    data = os.read(fd, 4096)
    if not data:
        raise OSError("device disconnected")
    for byte in data:
        if byte == 10:  # '\n'
            yield bytes(pending).rstrip(b"\r").decode("utf-8", "replace")
            pending.clear()
        elif len(pending) < MAX_LINE:
            pending.append(byte)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="serial device path (auto-detected if omitted)")
    parser.add_argument("--log", default="power_diag.log", help="path to append every line to (default: power_diag.log)")
    parser.add_argument("--all", action="store_true", help="echo every line to the terminal, not just [POWER-DIAG] ones")
    args = parser.parse_args()

    port = args.port or find_port()
    if not port:
        print("No serial device found. Pass --port explicitly (e.g. /dev/cu.usbserial-XXXX).", file=sys.stderr)
        return 1

    print(f"Watching {port} @ {BAUD} baud. Logging every line to {args.log}.")
    print(f"Showing {'all lines' if args.all else MARKER + ' lines only'} here. Ctrl+C to stop.\n")

    with open(args.log, "a", buffering=1) as log:
        while True:
            try:
                fd = open_port(port)
            except FileNotFoundError:
                time.sleep(1.0)
                continue
            except OSError as exc:
                print(f"Could not open {port}: {exc}. Retrying in 2s...", file=sys.stderr)
                time.sleep(2.0)
                continue

            pending = bytearray()
            try:
                while True:
                    for line in read_lines(fd, pending):
                        stamp = time.strftime("%H:%M:%S")
                        log.write(f"{stamp} {line}\n")
                        if args.all or MARKER in line:
                            tag = "\033[93m" + MARKER + "\033[0m" if MARKER in line else ""
                            shown = line.replace(MARKER, tag) if tag else line
                            print(f"{stamp} {shown}")
            except OSError:
                print(f"\n{time.strftime('%H:%M:%S')} -- disconnected, waiting for {port} to come back...", file=sys.stderr)
            except KeyboardInterrupt:
                return 0
            finally:
                os.close(fd)
            time.sleep(1.0)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
