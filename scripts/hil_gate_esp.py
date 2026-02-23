#!/usr/bin/env python3
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TRIAGE_RUNNER = ROOT / "scripts" / "crash_triage_runner.py"


def run_triaged(step: str, shell_cmd: str) -> None:
    cmd = [
        "python3",
        str(TRIAGE_RUNNER),
        "--name",
        step,
        "--out-dir",
        str(ROOT / "build" / "triage"),
        "--",
        "bash",
        "-lc",
        shell_cmd,
    ]
    subprocess.run(cmd, cwd=ROOT, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Run HIL gate cycles without manual intervention.")
    parser.add_argument("--port", required=True, help="serial port, e.g. /dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=115200, help="serial baud rate")
    parser.add_argument("--cycles", type=int, default=3, help="full HIL cycles (default: 3)")
    parser.add_argument("--flash-first", action="store_true", help="flash firmware before first cycle")
    parser.add_argument("--max-seconds", type=int, default=2400, help="max seconds per hil_applets cycle")
    args = parser.parse_args()

    if args.cycles < 1:
        raise SystemExit("--cycles must be >= 1")

    for cycle in range(1, args.cycles + 1):
        print(f"[hil-gate] cycle {cycle}/{args.cycles}")
        smoke_flags = f"--port {args.port} --baud {args.baud}"
        smoke_env = ""
        if cycle == 1 and args.flash_first:
            smoke_flags += " --flash"
        if cycle > 1:
            smoke_env = "XV6_SKIP_BUILD=1 "
        run_triaged(f"hil_smoke_cycle{cycle}", f"{smoke_env}python3 ./scripts/hil_smoke_esp.py {smoke_flags}")

        applets_flags = f"--port {args.port} --baud {args.baud} --max-seconds {args.max_seconds}"
        if cycle == 1 and args.flash_first:
            applets_flags += " --flash"
        run_triaged(f"hil_applets_cycle{cycle}", f"python3 ./scripts/hil_applets_esp.py {applets_flags}")

    print(f"[hil-gate] passed {args.cycles} full cycle(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
