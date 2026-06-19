#!/usr/bin/env python3
"""Small CLI client for the GALAKU PowerShell TCP serial bridge."""

from __future__ import annotations

import argparse
import os
import random
import socket
import subprocess
import sys
import time
from typing import Iterable


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 25363
DEFAULT_TIMEOUT = 5.0
MAX_COMMAND_LENGTH = 64
WINDOWS_DETACHED_PROCESS = 0x00000008
WINDOWS_CREATE_NEW_PROCESS_GROUP = 0x00000200


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] == "--advanced":
        return advanced_main(argv[1:])

    parser = build_parser()
    args = parser.parse_args(argv)

    try:
        command = build_command(args)
    except ValueError as error:
        parser.error(str(error))

    try:
        raw_reply = send_command(args.host, args.port, args.timeout, command)
    except OSError as error:
        print(f"connect/send failed: {error}", file=sys.stderr)
        return 1

    reply = select_protocol_reply(raw_reply, command) or raw_reply.strip() or f"OK SENT {command}"

    print(f"<= {command}")
    print(f"=> {reply}")

    return 3 if reply.startswith("ERR") else 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="vibration-control.py",
        description="Send one GALAKU control command to the PowerShell TCP serial bridge.",
    )
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"bridge host, default: {DEFAULT_HOST}")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"bridge TCP port, default: {DEFAULT_PORT}")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help=f"socket timeout seconds, default: {DEFAULT_TIMEOUT}")

    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--ping", action="store_true", help="send PING")
    group.add_argument("--status", action="store_true", help="send STATUS")
    group.add_argument("--scan", action="store_true", help="send SCAN")
    group.add_argument("--services", action="store_true", help="send SERVICES")
    group.add_argument("--stop", action="store_true", help="send STOP")
    group.add_argument("--set", dest="set_level", type=int, metavar="LEVEL", help="send SET <0-100>")
    group.add_argument("--hit", type=float, metavar="DAMAGE", help="send HIT <damage>")
    group.add_argument("--raw", metavar="COMMAND", help="send a raw printable ASCII command")

    return parser


def advanced_main(argv: list[str]) -> int:
    parser = build_advanced_parser()
    args = parser.parse_args(argv)

    if args.duration <= 0:
        parser.error("--time must be positive")
    if args.interval <= 0:
        parser.error("--interval must be positive")

    try:
        action = build_advanced_action(args)
    except ValueError as error:
        parser.error(str(error))

    if args.background:
        return start_advanced_background(argv)

    deadline = time.monotonic() + args.duration
    sent = 0

    try:
        while time.monotonic() < deadline:
            command = action()
            try:
                raw_reply = send_command(args.host, args.port, args.timeout, command)
            except OSError as error:
                print(f"connect/send failed: {error}", file=sys.stderr)
                return 1

            reply = select_protocol_reply(raw_reply, command) or raw_reply.strip() or f"OK SENT {command}"
            sent += 1
            print(f"[{sent}] <= {command}")
            print(f"[{sent}] => {reply}")

            if reply.startswith("ERR"):
                return 3

            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            time.sleep(min(args.interval, remaining))
    except KeyboardInterrupt:
        print("interrupted")
        return 130

    return 0


def build_advanced_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="vibration-control.py --advanced",
        description="Run bounded repeated GALAKU feedback commands.",
    )
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"bridge host, default: {DEFAULT_HOST}")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"bridge TCP port, default: {DEFAULT_PORT}")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help=f"socket timeout seconds, default: {DEFAULT_TIMEOUT}")
    parser.add_argument("--time", dest="duration", type=float, required=True, help="total run seconds")
    parser.add_argument("--interval", type=float, required=True, help="seconds between each command")
    parser.add_argument(
        "--background",
        action="store_true",
        help="start the advanced run in a detached background process",
    )

    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--hit", type=float, metavar="DAMAGE", help="send repeated HIT <damage>")
    group.add_argument("--randomhit", type=parse_range, metavar="MIN-MAX", help="send repeated random HIT values")
    group.add_argument("--set", dest="set_level", type=int, metavar="LEVEL", help="send repeated SET <0-100>")
    group.add_argument("--randomset", type=parse_range, metavar="MIN-MAX", help="send repeated random SET levels")

    return parser


def build_advanced_action(args: argparse.Namespace):
    if args.hit is not None:
        if args.hit <= 0:
            raise ValueError("--hit must be positive")
        command = normalize_command(f"HIT {args.hit:g}")
        return lambda: command

    if args.randomhit is not None:
        low, high = args.randomhit
        if low < 0 or high > 100:
            raise ValueError("--randomhit must be inside 0-100")
        return lambda: normalize_command(f"HIT {random.uniform(low, high):.2f}")

    if args.set_level is not None:
        if not 0 <= args.set_level <= 100:
            raise ValueError("--set must be between 0 and 100")
        command = f"SET {args.set_level}"
        return lambda: command

    if args.randomset is not None:
        low, high = args.randomset
        if low < 0 or high > 100:
            raise ValueError("--randomset must be inside 0-100")
        return lambda: f"SET {random.randint(int(low), int(high))}"

    raise ValueError("no advanced action selected")


def parse_range(value: str) -> tuple[float, float]:
    if "-" not in value:
        raise argparse.ArgumentTypeError("range must look like MIN-MAX")
    left, right = value.split("-", 1)
    try:
        low = float(left)
        high = float(right)
    except ValueError as error:
        raise argparse.ArgumentTypeError("range bounds must be numbers") from error
    if low > high:
        raise argparse.ArgumentTypeError("range must satisfy MIN <= MAX")
    return low, high


def start_advanced_background(argv: list[str]) -> int:
    child_args = [arg for arg in argv if arg != "--background"]
    command = [sys.executable, os.path.abspath(__file__), "--advanced", *child_args]
    kwargs: dict[str, object] = {
        "cwd": os.path.dirname(os.path.abspath(__file__)),
        "stdin": subprocess.DEVNULL,
        "stdout": subprocess.DEVNULL,
        "stderr": subprocess.DEVNULL,
        "close_fds": True,
    }
    if os.name == "nt":
        kwargs["creationflags"] = WINDOWS_DETACHED_PROCESS | WINDOWS_CREATE_NEW_PROCESS_GROUP
    else:
        kwargs["start_new_session"] = True

    process = subprocess.Popen(command, **kwargs)
    print(f"started background advanced run, pid={process.pid}")
    return 0


def build_command(args: argparse.Namespace) -> str:
    if args.ping:
        return "PING"
    if args.status:
        return "STATUS"
    if args.scan:
        return "SCAN"
    if args.services:
        return "SERVICES"
    if args.stop:
        return "STOP"
    if args.set_level is not None:
        if not 0 <= args.set_level <= 100:
            raise ValueError("--set must be between 0 and 100")
        return f"SET {args.set_level}"
    if args.hit is not None:
        if args.hit <= 0:
            raise ValueError("--hit must be positive")
        return normalize_command(f"HIT {args.hit:g}")
    if args.raw is not None:
        return normalize_command(args.raw)
    raise ValueError("no command selected")


def normalize_command(command: str) -> str:
    command = command.replace("\r", " ").replace("\n", " ").strip()
    if not command:
        raise ValueError("command is empty")
    if len(command) > MAX_COMMAND_LENGTH:
        raise ValueError(f"command is longer than {MAX_COMMAND_LENGTH} characters")
    if any(ord(ch) < 0x20 or ord(ch) > 0x7E for ch in command):
        raise ValueError("command must be printable ASCII")
    return command


def send_command(host: str, port: int, timeout: float, command: str) -> str:
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(command.encode("ascii") + b"\n")
        sock.shutdown(socket.SHUT_WR)

        chunks: list[bytes] = []
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            chunks.append(chunk)

    return b"".join(chunks).decode("utf-8", "replace")


def select_protocol_reply(raw_reply: str, command: str) -> str:
    lines = [
        line.strip()
        for line in raw_reply.splitlines()
        if is_protocol_line(line.strip())
    ]
    if not lines:
        return ""

    expected = expected_prefixes(command)
    matches = [line for line in lines if line.startswith(expected)]
    return (matches or lines)[-1]


def is_protocol_line(line: str) -> bool:
    return (
        line == "PONG"
        or line.startswith("STATUS ")
        or line.startswith("OK ")
        or line.startswith("ERR ")
        or line.startswith("SERVICES ")
    )


def expected_prefixes(command: str) -> tuple[str, ...]:
    verb = command.split(maxsplit=1)[0].upper()
    if verb == "PING":
        return ("PONG",)
    if verb == "STATUS":
        return ("STATUS ",)
    if verb == "SCAN":
        return ("OK SCAN",)
    if verb == "SERVICES":
        return ("OK SERVICES", "SERVICES ", "ERR ")
    if verb == "SET":
        return ("OK SET",)
    if verb == "HIT":
        return ("OK HIT",)
    if verb == "STOP":
        return ("OK STOP",)
    return ("PONG", "STATUS ", "OK ", "ERR ", "SERVICES ")


if __name__ == "__main__":
    raise SystemExit(main())
