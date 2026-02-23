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


def cmd(sock, command: str, timeout_s: float = 12.0) -> str:
    drain_rx(sock)
    sock.sendall((command + "\r\r").encode())
    out = recv_until_prompts(sock, prompts=2, timeout_s=timeout_s).decode(errors="ignore")
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
        "Traceback (most recent call last)",
        "assert failed:",
    )
    for marker in bad_markers:
        if marker in out:
            raise RuntimeError(f"detected failure marker: {marker}")


def extract_job_id(out: str) -> str | None:
    for pat in (
        r"\[(\d+)\]\s+started",
        r"\[(\d+)\]",
    ):
        m = re.search(pat, out)
        if m is not None:
            return m.group(1)
    return None


def start_bg_job(sock, command: str, job_hint: str) -> str | None:
    out = cmd(sock, command)
    if "jobs: spawn failed" in out:
        return None
    job_id = extract_job_id(out)
    if job_id is not None:
        return job_id

    out_jobs = cmd(sock, "jobs")
    m = re.search(r"\[(\d+)\].*" + re.escape(job_hint), out_jobs)
    if m is not None:
        return m.group(1)
    job_id = extract_job_id(out_jobs)
    if job_id is not None:
        return job_id

    raise RuntimeError(f"failed to parse background job id for {command!r}\n{out}\n{out_jobs}")


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
        assert_clean_output(boot)
        sync_prompt(sock, timeout_s=20.0)

        job_ids = []
        for _ in range(20):
            job_id = start_bg_job(sock, "sleep 1200 &", "sleep 1200")
            if job_id is None:
                break
            job_ids.append(job_id)
        assert len(job_ids) >= 3

        out = cmd(sock, "ps")
        assert_clean_output(out)
        assert "PID STATE EXIT REASON" in out
        assert "sh" in out

        out = cmd(sock, "jobs")
        assert_clean_output(out)
        assert "running sleep 1200" in out

        running_ids = re.findall(r"\[(\d+)\]\s+running\s+sleep 1200", out)
        assert len(running_ids) >= 2

        for jid in running_ids:
            out = cmd(sock, f"kill {jid}")
            assert_clean_output(out)
            assert "kill: ok" in out
            out = cmd(sock, f"wait {jid}")
            assert_clean_output(out)
            assert "wait: done 137" in out

        out = cmd(sock, "jobs")
        assert_clean_output(out)
        assert "jobs: empty" in out

        for i in range(200):
            if i % 3 == 0:
                out = cmd(sock, "head -1 /etc/rc")
                assert "export PATH=" in out
            elif i % 3 == 1:
                out = cmd(sock, "echo stress | tr s S")
                assert "StreSS" in out
            else:
                out = cmd(sock, "wc -l /home/README")
                assert "/home/README" in out
            assert_clean_output(out)
            if (i + 1) % 50 == 0:
                print(f"stress progress: {i + 1}/200")

        out = cmd(sock, "ps")
        assert_clean_output(out)
        assert "sh" in out
        assert "- NOJOBS -" in out
    finally:
        stop_qemu(qemu_proc, sock)

    print("QEMU stress test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
