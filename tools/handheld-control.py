#!/usr/bin/env python3
"""Inspect and operate a T-Deck over its existing USB diagnostic connection."""

from __future__ import annotations

import argparse
import fcntl
import json
import math
import os
import secrets
import select
import sys
import termios
import time


PREFIX = b"[UICTRL] "
MAX_LINE = 4096
KEYS = ("up", "down", "left", "right", "enter", "backspace", "escape", "tab")


class ControlError(RuntimeError):
    pass


class SerialPort:
    """POSIX serial I/O without DTR/RTS toggles or hangup-on-close resets."""

    def __init__(self, path: str):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            settings = termios.tcgetattr(self.fd)
            settings[0] = settings[1] = settings[3] = 0
            settings[2] = termios.CS8 | termios.CLOCAL | termios.CREAD
            settings[4] = settings[5] = termios.B115200
            settings[6][termios.VMIN] = settings[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, settings)
        except BaseException:
            os.close(self.fd)
            raise

    def close(self):
        os.close(self.fd)

    def write(self, data: bytes, deadline: float):
        while data:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([], [self.fd], [], remaining)[1]:
                raise ControlError("USB write timed out; input outcome is unknown")
            try:
                count = os.write(self.fd, data)
            except BlockingIOError:
                continue
            if count <= 0:
                raise ControlError("USB disconnected while writing; input outcome is unknown")
            data = data[count:]

    def read(self, timeout: float) -> bytes:
        if not select.select([self.fd], [], [], timeout)[0]:
            return b""
        try:
            data = os.read(self.fd, 1024)
        except BlockingIOError:
            return b""
        if not data:
            raise ControlError("USB disconnected; input outcome is unknown")
        return data


def command_body(action: str, value=None, ctrl: bool = False) -> str:
    if action == "hold" and value is None and not ctrl:
        return "hold"
    if action == "view" and not ctrl and isinstance(value, int) and not isinstance(value, bool) and 0 <= value <= 511:
        return f"view {value}"
    if action == "key" and value in KEYS and not ctrl:
        return f"key {value}"
    if action == "char" and isinstance(value, int) and not isinstance(value, bool) and 32 <= value <= 126:
        return f"char {value}" + (" ctrl" if ctrl else "")
    raise ControlError("invalid UI command")


class Controller:
    def __init__(self, port, timeout: float = 5.0, verbose: bool = False):
        if not math.isfinite(timeout) or timeout <= 0:
            raise ControlError("timeout must be finite and positive")
        self.port = port
        self.timeout = timeout
        self.verbose = verbose
        self.pending = bytearray()
        self.discarding = False
        self.sequence = secrets.randbelow(2147483647) + 1

    def _lines(self, data: bytes):
        for byte in data:
            if byte == 10:
                line = None if self.discarding else bytes(self.pending).rstrip(b"\r")
                self.pending.clear()
                self.discarding = False
                if line is not None:
                    yield line
            elif not self.discarding:
                if len(self.pending) == MAX_LINE:
                    self.pending.clear()
                    self.discarding = True
                else:
                    self.pending.append(byte)

    def request(self, action: str, value=None, ctrl: bool = False) -> dict:
        body = command_body(action, value, ctrl)
        request_id = self.sequence
        self.sequence = request_id % 2147483647 + 1
        deadline = time.monotonic() + self.timeout
        # Write this logical command exactly once. A lost acknowledgement is
        # ambiguous: never repeat a key, even if the reconnect succeeds.
        self.port.write(f"U {request_id} {body}\n".encode("ascii"), deadline)
        while (remaining := deadline - time.monotonic()) > 0:
            for line in self._lines(self.port.read(remaining)):
                start = line.find(PREFIX)
                if start < 0:
                    if self.verbose and line:
                        print(line.decode("utf-8", errors="replace"), file=sys.stderr)
                    continue
                try:
                    reply = json.loads(line[start + len(PREFIX):])
                except (ValueError, UnicodeError) as error:
                    raise ControlError("malformed UI reply; inspect with view before repeating input") from error
                if not isinstance(reply, dict) or reply.get("id") != request_id:
                    continue
                if reply.get("ok") is not True:
                    raise ControlError(f"device rejected command: {reply.get('error', 'invalid reply')}")
                return reply
        raise ControlError("no UI reply; input outcome is unknown—use view before repeating input")

    def text(self, value: str) -> dict:
        if not value or any(not 32 <= ord(char) <= 126 for char in value):
            raise ControlError("text must contain printable ASCII only; use key enter separately")
        if len(value) > 512:
            raise ControlError("text is limited to 512 characters per command")
        state = self.request("view", 0)
        if state.get("asleep"):
            raise ControlError("screen is asleep; send one key to wake it, then type text")
        for index, char in enumerate(value):
            state = self.request("char", ord(char))
            if state.get("woke_only") is True:
                raise ControlError(f"screen slept while typing after {index} characters; inspect before continuing")
        return state


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="T-Deck USB serial device")
    parser.add_argument("--timeout", type=float, default=5.0,
                        help="seconds for each request's write and reply (default: 5); text sends multiple requests")
    parser.add_argument("--verbose", action="store_true", help="show unrelated device logs on stderr")
    actions = parser.add_subparsers(dest="action", required=True)
    view = actions.add_parser("view", help="read the screen without waking it")
    view.add_argument("--offset", type=int, default=0, help="next cursor from a previous view (0-511; default: 0)")
    key = actions.add_parser("key", help="send one ordinary navigation key")
    key.add_argument("name", choices=KEYS)
    actions.add_parser("hold", help="perform the current screen's trackball long-press action")
    char = actions.add_parser("char", help="type one printable ASCII character or Ctrl shortcut")
    char.add_argument("value", help="one printable ASCII character (code 32-126)")
    char.add_argument("--ctrl", action="store_true")
    text = actions.add_parser("text", help="type printable ASCII through normal screen input")
    text.add_argument("value", help="1-512 printable ASCII characters; send key enter separately")
    args = parser.parse_args(argv)
    port = None
    try:
        if not math.isfinite(args.timeout) or args.timeout <= 0:
            raise ControlError("timeout must be finite and positive")
        if args.action == "char" and len(args.value) != 1:
            raise ControlError("char requires exactly one character")
        if args.action == "view":
            command_body("view", args.offset)
        elif args.action == "char":
            command_body("char", ord(args.value), args.ctrl)
        port = SerialPort(args.port)
        control = Controller(port, args.timeout, args.verbose)
        if args.action == "text":
            reply = control.text(args.value)
        elif args.action == "view":
            reply = control.request("view", args.offset)
        elif args.action == "key":
            reply = control.request("key", args.name)
        elif args.action == "hold":
            reply = control.request("hold")
        else:
            reply = control.request("char", ord(args.value), args.ctrl)
        print(json.dumps(reply, ensure_ascii=False, indent=2))
        return 0
    except (ControlError, OSError, termios.error) as error:
        print(f"handheld control: {error}", file=sys.stderr)
        return 1
    finally:
        if port is not None:
            port.close()


if __name__ == "__main__":
    raise SystemExit(main())
