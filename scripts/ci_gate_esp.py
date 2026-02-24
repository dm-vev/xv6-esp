#!/usr/bin/env python3
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TRIAGE_RUNNER = ROOT / "scripts" / "crash_triage_runner.py"


def resolve_idf_export() -> str:
    def has_export_script(base: Path) -> bool:
        try:
            return (base / "export.sh").exists()
        except OSError:
            return False

    candidates = []
    if os.environ.get("IDF_PATH"):
        candidates.append(Path(os.environ["IDF_PATH"]))
    home = Path.home()
    candidates.extend((home / "esp-idf", Path("/opt/esp-idf"), Path("/root/esp-idf"), Path("/tmp/esp-idf")))
    for p in candidates:
        if p and has_export_script(p):
            return f"source {p}/export.sh >/dev/null"
    raise RuntimeError("ESP-IDF not found. Set IDF_PATH or install under ~/esp-idf.")


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
    idf_export = resolve_idf_export()
    steps: list[tuple[str, str]] = [
        ("build", f"{idf_export} && idf.py set-target esp32s3 && idf.py build"),
        ("abi_check", "python3 ./scripts/check_hostabi_abi.py"),
        ("qemu_smoke", "XV6_SKIP_BUILD=1 python3 ./scripts/qemu_smoke_esp.py"),
        ("qemu_applets", "XV6_SKIP_BUILD=1 python3 ./scripts/qemu_applets_esp.py"),
        ("qemu_soak", "XV6_SKIP_BUILD=1 python3 ./scripts/qemu_soak_esp.py"),
        ("qemu_stress", "XV6_SKIP_BUILD=1 python3 ./scripts/qemu_stress_esp.py"),
        ("qemu_regressions", "XV6_SKIP_BUILD=1 python3 ./scripts/qemu_regressions_esp.py"),
    ]

    for name, command in steps:
        print(f"[ci-gate] running: {name}")
        run_triaged(name, command)

    print("[ci-gate] all steps passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
