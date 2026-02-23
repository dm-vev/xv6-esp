#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import shlex
import signal
import subprocess
import sys
import time
from pathlib import Path

CRASH_MARKERS = (
    "Guru Meditation Error",
    "panic'ed",
    "Backtrace:",
    "task_wdt: Task watchdog got triggered",
    "LoadProhibited",
    "StoreProhibited",
    "Traceback (most recent call last)",
    "assert failed:",
)


def terminate_process(proc: subprocess.Popen[str]) -> None:
    if proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=5.0)
        return
    except subprocess.TimeoutExpired:
        pass
    proc.kill()
    proc.wait(timeout=5.0)


def main() -> int:
    parser = argparse.ArgumentParser(description="Run command with crash triage logging/fail-fast markers.")
    parser.add_argument("--name", default="run", help="logical step name")
    parser.add_argument("--out-dir", default="build/triage", help="artifact root directory")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="command after '--'")
    args = parser.parse_args()

    cmd = list(args.command)
    if cmd and cmd[0] == "--":
        cmd = cmd[1:]
    if not cmd:
        raise SystemExit("missing command; pass it after '--'")

    ts = time.strftime("%Y%m%d-%H%M%S")
    root = Path(args.out_dir).resolve()
    run_dir = root / f"{ts}-{args.name}"
    run_dir.mkdir(parents=True, exist_ok=True)
    log_path = run_dir / "output.log"
    cmd_path = run_dir / "command.txt"
    result_path = run_dir / "result.json"

    cmd_path.write_text(" ".join(shlex.quote(c) for c in cmd) + "\n", encoding="utf-8")

    print(f"[triage] step={args.name} artifacts={run_dir}")
    started = time.time()
    crash_marker = ""
    returncode = 0

    with log_path.open("w", encoding="utf-8") as log:
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            preexec_fn=os.setsid if os.name == "posix" else None,
        )
        assert proc.stdout is not None
        for line in proc.stdout:
            sys.stdout.write(line)
            log.write(line)
            for marker in CRASH_MARKERS:
                if marker in line:
                    crash_marker = marker
                    break
            if crash_marker:
                print(f"[triage] fail-fast marker='{crash_marker}', terminating step")
                if os.name == "posix":
                    try:
                        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                terminate_process(proc)
                break

        if crash_marker and proc.poll() is None:
            terminate_process(proc)
        returncode = proc.wait()

    if crash_marker:
        returncode = 99

    ended = time.time()
    result = {
        "name": args.name,
        "command": cmd,
        "started_at_unix": started,
        "ended_at_unix": ended,
        "duration_sec": ended - started,
        "returncode": returncode,
        "crash_marker": crash_marker or None,
        "ok": returncode == 0 and not crash_marker,
        "log_file": str(log_path),
    }
    result_path.write_text(json.dumps(result, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")

    if returncode != 0:
        print(f"[triage] step failed rc={returncode}; logs: {log_path}")
    else:
        print(f"[triage] step passed; logs: {log_path}")
    return returncode


if __name__ == "__main__":
    raise SystemExit(main())
