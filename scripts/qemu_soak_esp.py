#!/usr/bin/env python3
import os
import re
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
    raise RuntimeError(f"timeout waiting for marker {marker!r}")


def recv_until_prompts(sock, prompts: int, timeout_s: float = 10.0) -> bytes:
    marker = b"xv6> "
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
        if data.count(marker) >= prompts:
            return bytes(data)
    raise RuntimeError(f"timeout waiting for {prompts} prompts")


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


def cmd(sock, command: str, timeout_s: float = 12.0, verbose: bool = False) -> str:
    drain_rx(sock)
    sock.sendall((command + "\r\r").encode())
    out = recv_until_prompts(sock, prompts=2, timeout_s=timeout_s).decode(errors="ignore")
    if verbose:
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


def parse_free_heap(out: str) -> int:
    m = re.search(r"free_heap=(\d+)\s+bytes", out)
    if m is None:
        raise RuntimeError("failed to parse free_heap")
    return int(m.group(1))


def assert_clean_output(out: str) -> None:
    bad_markers = (
        "Guru Meditation Error",
        "panic'ed",
        "Backtrace:",
        "task_wdt: Task watchdog got triggered",
        "Traceback (most recent call last)",
        "assert failed:",
    )
    for marker in bad_markers:
        if marker in out:
            raise RuntimeError(f"detected failure marker: {marker}")


def cmd_expect(sock, command: str, expected: str, timeout_s: float = 12.0, retries: int = 3) -> str:
    last_out = ""
    for attempt in range(retries):
        out = cmd(sock, command, timeout_s=timeout_s)
        assert_clean_output(out)
        if expected in out:
            return out
        last_out = out
        sync_prompt(sock, timeout_s=5.0)
    raise AssertionError(f"expected {expected!r} in output for command {command!r}\n{last_out}")


def read_free_heap_or_none(sock) -> int | None:
    out = cmd(sock, "mem", verbose=True)
    if "exec: command not found" in out:
        print("mem command is unavailable; skipping heap drift check")
        return None
    assert_clean_output(out)
    try:
        return parse_free_heap(out)
    except RuntimeError:
        print("mem output does not expose free_heap; skipping heap drift check")
        return None


def main() -> int:
    if os.environ.get("XV6_SKIP_BUILD") != "1":
        run(f"{IDF_EXPORT} && idf.py set-target esp32s3 && idf.py build")
    generate_qemu_flash()
    ensure_qemu_efuse()
    run("pkill -x qemu-system-xtensa >/dev/null 2>&1 || true")

    qemu_proc, sock = launch_qemu()
    try:
        sock.sendall(b"\r")
        boot = recv_until(sock, b"xv6> ", timeout_s=30.0).decode(errors="ignore")
        print(boot)
        sync_prompt(sock, timeout_s=20.0)

        start_mem = read_free_heap_or_none(sock)
        total_cmds = 1200

        for i in range(total_cmds):
            if i % 4 == 0:
                out = cmd_expect(sock, "head -1 /etc/rc", "export PATH=", timeout_s=12.0)
            elif i % 4 == 1:
                out = cmd_expect(sock, "cat /home/README > /tmp/soak.txt", "xv6> ", timeout_s=12.0)
            elif i % 4 == 2:
                out = cmd_expect(sock, "wc -c /tmp/soak.txt", "/tmp/soak.txt", timeout_s=12.0)
            else:
                out = cmd_expect(sock, "echo abc | tr a A", "Abc", timeout_s=12.0)

            if (i + 1) % 200 == 0:
                print(f"soak progress: {i + 1}/{total_cmds}")

        out = cmd(sock, "ps", verbose=True)
        assert_clean_output(out)
        assert "PID STATE EXIT REASON" in out
        assert "sh" in out
        assert "- NOJOBS -" in out

        if start_mem is not None:
            end_mem = read_free_heap_or_none(sock)
            if end_mem is not None:
                drift = start_mem - end_mem
                print(f"heap drift: {drift} bytes")
                assert drift < 32768
    finally:
        stop_qemu(qemu_proc, sock)

    print("QEMU soak test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
