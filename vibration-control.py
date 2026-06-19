#!/usr/bin/env python3
"""Small CLI client for the GALAKU PowerShell TCP serial bridge."""

from __future__ import annotations

import argparse
import socket
import sys
from typing import Iterable


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 25363
DEFAULT_TIMEOUT = 5.0
MAX_COMMAND_LENGTH = 64


def main(argv: list[str] | None = None) -> int:
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
