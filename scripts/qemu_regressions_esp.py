#!/usr/bin/env python3
from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

from qemu_idf_session import launch_idf_qemu, stop_idf_qemu

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"


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


IDF_EXPORT = resolve_idf_export()


def run(cmd: str) -> None:
    subprocess.run(["bash", "-lc", cmd], cwd=ROOT, check=True)


def recv_until(sock, marker: bytes, timeout_s: float = 10.0) -> bytes:
    sock.settimeout(0.8)
    data = bytearray()
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            chunk = sock.recv(4096)
        except TimeoutError:
            continue
        if not chunk:
            continue
        data.extend(chunk)
        if marker in data:
            return bytes(data)
    tail = data[-512:].decode(errors="ignore")
    raise RuntimeError(f"timeout waiting for marker {marker!r}; serial tail={tail!r}")


def sync_prompt(sock, timeout_s: float = 30.0) -> str:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        sock.sendall(b"\r")
        try:
            out = recv_until(sock, b"xv6> ", timeout_s=2.0).decode(errors="ignore")
        except RuntimeError:
            continue
        return out
    raise RuntimeError("timeout waiting for shell prompt")


def drain_rx(sock) -> None:
    sock.settimeout(0.0)
    while True:
        try:
            chunk = sock.recv(4096)
        except TimeoutError:
            break
        if not chunk:
            break
    sock.settimeout(0.8)


def cmd(sock, command: str, timeout_s: float = 15.0) -> str:
    drain_rx(sock)
    sock.sendall((command + "\r").encode())
    out = recv_until(sock, b"xv6> ", timeout_s=timeout_s).decode(errors="ignore")
    print(f"$ {command}\n{out}")
    return out


def generate_qemu_flash() -> None:
    merge = (
        f"{IDF_EXPORT} && "
        "esptool --chip=esp32s3 merge-bin "
        "--output=build/qemu_flash.bin --pad-to-size=2MB "
        "--flash-mode dio --flash-freq 80m --flash-size 2MB "
        "0x0 build/bootloader/bootloader.bin "
        "0x8000 build/partition_table/partition-table.bin "
        "0xf000 build/ota_data_initial.bin "
        "0x120000 build/xv6fs.bin "
        "0x20000 build/xv6_esp32s3.bin"
    )
    run(merge)


def ensure_qemu_efuse() -> None:
    efuse = BUILD / "qemu_efuse.bin"
    efuse.write_bytes(bytes(1024))


def launch_qemu():
    flash = BUILD / "qemu_flash.bin"
    efuse = BUILD / "qemu_efuse.bin"
    return launch_idf_qemu(ROOT, IDF_EXPORT, flash, efuse)


def stop_qemu(proc, sock=None) -> None:
    stop_idf_qemu(proc, sock)


def assert_clean_output(out: str) -> None:
    bad_markers = (
        "Guru Meditation Error",
        "panic'ed",
        "Backtrace:",
        "task_wdt: Task watchdog got triggered",
        "LoadProhibited",
        "StoreProhibited",
        "Traceback (most recent call last)",
        "assert failed:",
    )
    for marker in bad_markers:
        if marker in out:
            raise RuntimeError(f"detected failure marker: {marker}")


def main() -> int:
    if os.environ.get("XV6_SKIP_BUILD") != "1":
        run(f"{IDF_EXPORT} && idf.py set-target esp32s3 && idf.py build")
    generate_qemu_flash()
    ensure_qemu_efuse()
    run("pkill -x qemu-system-xtensa >/dev/null 2>&1 || true")

    qemu_proc, sock = launch_qemu()
    try:
        try:
            boot = recv_until(sock, b"xv6> ", timeout_s=45.0).decode(errors="ignore")
        except RuntimeError:
            boot = sync_prompt(sock, timeout_s=45.0)
        print(boot)
        assert_clean_output(boot)
        sync_prompt(sock, timeout_s=20.0)

        setup_cmds = (
            "export PATH=/bin:/usr/bin:.",
            "mkdir -p /tmp/reg",
            "echo alpha > /tmp/reg/in.txt",
            "echo alpha >> /tmp/reg/in.txt",
            "echo beta >> /tmp/reg/in.txt",
        )
        for c in setup_cmds:
            out = cmd(sock, c)
            assert_clean_output(out)

        regressions = (
            "cat /../etc/../etc/rc | head -1",
            "cat ////etc////rc | head -1",
            "ls /tmp/../../tmp",
            "mkdir -p /tmp/reg/../reg2/./x",
            "touch /tmp/reg2/../../tmp/reg_touch",
            "uniq -999999999999 /tmp/reg/in.txt",
            "split -0 /tmp/reg/in.txt /tmp/reg/out_",
            "cat /no_such_file",
            "hostabi_probe",
        )
        for c in regressions:
            out = cmd(sock, c, timeout_s=30.0)
            assert_clean_output(out)
            if c == "hostabi_probe":
                probe_out = out
                for _ in range(2):
                    if "PROBE SUMMARY failures=0" in probe_out:
                        break
                    probe_out = cmd(sock, "hostabi_probe", timeout_s=30.0)
                    assert_clean_output(probe_out)
                if "PROBE SUMMARY failures=0" not in probe_out:
                    raise RuntimeError("hostabi_probe regression check failed")
                if "hostabi_probe: exit=" in probe_out:
                    raise RuntimeError("hostabi_probe exited non-zero")
    finally:
        stop_qemu(qemu_proc, sock)

    print("QEMU regressions test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
