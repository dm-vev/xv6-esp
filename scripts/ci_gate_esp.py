#!/usr/bin/env python3
from __future__ import annotations

import os
import shlex
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

    candidates: list[Path] = []
    if os.environ.get("IDF_PATH"):
        candidates.append(Path(os.environ["IDF_PATH"]))
    candidates.append(ROOT.parent / "magnolia" / "esp-idf")
    home = Path.home()
    candidates.extend((home / "esp-idf", Path("/opt/esp-idf"), Path("/root/esp-idf"), Path("/tmp/esp-idf")))
    for p in candidates:
        if p and has_export_script(p):
            return f"source {shlex.quote(str((p / 'export.sh').resolve()))} >/dev/null"
    searched = ", ".join(str(p) for p in candidates)
    raise RuntimeError(f"ESP-IDF not found. Set IDF_PATH. Searched: {searched}")


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
    qemu_retries = os.environ.get("XV6_QEMU_SUITE_RETRIES", "2")

    steps: list[tuple[str, str]] = [
        ("build", f"{idf_export} && idf.py set-target esp32s3 && idf.py build"),
        ("abi_check", "python3 ./scripts/check_hostabi_abi.py --require-generated-checks"),
        (
            "qemu_smoke",
            f"XV6_SKIP_BUILD=1 python3 ./scripts/qemu_ci.py --skip-build --suite smoke --retries {qemu_retries}",
        ),
        (
            "qemu_applets",
            f"XV6_SKIP_BUILD=1 python3 ./scripts/qemu_ci.py --skip-build --suite applets --retries {qemu_retries}",
        ),
        (
            "qemu_regressions",
            f"XV6_SKIP_BUILD=1 python3 ./scripts/qemu_ci.py --skip-build --suite regressions --retries {qemu_retries}",
        ),
        (
            "qemu_stress",
            f"XV6_SKIP_BUILD=1 python3 ./scripts/qemu_ci.py --skip-build --suite stress --retries {qemu_retries}",
        ),
        (
            "qemu_soak",
            f"XV6_SKIP_BUILD=1 python3 ./scripts/qemu_ci.py --skip-build --suite soak --retries {qemu_retries}",
        ),
    ]

    for name, command in steps:
        print(f"[ci-gate] running: {name}")
        run_triaged(name, command)

    print("[ci-gate] all steps passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
