from __future__ import annotations

import argparse
import random
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
CONTROL = ROOT / "vibration-control.py"
STOP_FILE = ROOT / "random-wave.stop"


def send(*args: str) -> None:
    subprocess.run(
        [sys.executable, str(CONTROL), *args],
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Bounded random vibration wave controller.")
    parser.add_argument("--duration", type=float, default=120.0, help="seconds, default 120")
    parser.add_argument("--min", dest="min_level", type=int, default=5, help="minimum level, default 5")
    parser.add_argument("--max", dest="max_level", type=int, default=35, help="maximum level, default 35")
    parser.add_argument("--min-sleep", type=float, default=0.6, help="minimum sleep seconds")
    parser.add_argument("--max-sleep", type=float, default=2.2, help="maximum sleep seconds")
    args = parser.parse_args()

    if args.min_level < 0 or args.max_level > 100 or args.min_level > args.max_level:
        parser.error("levels must satisfy 0 <= min <= max <= 100")
    if args.duration <= 0:
        parser.error("duration must be positive")

    STOP_FILE.unlink(missing_ok=True)
    deadline = time.monotonic() + args.duration
    current = random.randint(args.min_level, args.max_level)

    try:
        while time.monotonic() < deadline:
            if STOP_FILE.exists():
                break

            drift = random.randint(-10, 10)
            if random.random() < 0.18:
                drift += random.choice([-18, 18])
            current = max(args.min_level, min(args.max_level, current + drift))

            send("--set", str(current))
            remaining = max(0.0, deadline - time.monotonic())
            sleep_for = min(remaining, random.uniform(args.min_sleep, args.max_sleep))
            time.sleep(sleep_for)
    finally:
        send("--stop")
        STOP_FILE.unlink(missing_ok=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
