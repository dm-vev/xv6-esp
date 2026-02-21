#!/usr/bin/env python3
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path


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
    candidates.extend((Path("/root/esp-idf"), Path("/tmp/esp-idf")))
    for p in candidates:
        if p and has_export_script(p):
            return f"source {p}/export.sh >/dev/null"
    raise RuntimeError("ESP-IDF not found. Set IDF_PATH or install to /root/esp-idf.")


IDF_EXPORT = resolve_idf_export()


def run(cmd: str) -> None:
    subprocess.run(["bash", "-lc", cmd], cwd=ROOT, check=True)


def idf_which(binary: str) -> str:
    out = subprocess.check_output(
        ["bash", "-lc", f"{IDF_EXPORT} && which {binary}"],
        cwd=ROOT,
        text=True,
    )
    return out.strip().splitlines()[-1]


def wait_socket(host: str, port: int, timeout_s: float) -> socket.socket:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            return socket.create_connection((host, port), timeout=1.0)
        except OSError:
            time.sleep(0.25)
    raise RuntimeError("serial socket is not ready")


def recv_until(sock: socket.socket, marker: bytes, timeout_s: float = 10.0) -> bytes:
    sock.settimeout(0.8)
    data = bytearray()
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            continue
        data.extend(chunk)
        if marker in data:
            return bytes(data)
    raise RuntimeError(f"timeout waiting for marker {marker!r}")


def cmd(sock: socket.socket, command: str, timeout_s: float = 12.0, verbose: bool = False) -> str:
    sock.sendall((command + "\n").encode())
    out = recv_until(sock, b"xv6> ", timeout_s=timeout_s).decode(errors="ignore")
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
    if not efuse.exists():
        efuse.write_bytes(bytes(1024))


def launch_qemu() -> subprocess.Popen:
    qemu_bin = idf_which("qemu-system-xtensa")
    flash = BUILD / "qemu_flash.bin"
    efuse = BUILD / "qemu_efuse.bin"
    args = [
        qemu_bin,
        "-M",
        "esp32s3",
        "-m",
        "32M",
        "-drive",
        f"file={flash},if=mtd,format=raw",
        "-drive",
        f"file={efuse},if=none,format=raw,id=efuse",
        "-global",
        "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
        "-global",
        "driver=timer.esp32s3.timg,property=wdt_disable,value=true",
        "-nic",
        "user,model=open_eth",
        "-nographic",
        "-serial",
        "tcp::5555,server,nowait",
    ]
    return subprocess.Popen(
        args,
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def stop_qemu(proc: subprocess.Popen) -> None:
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
    run("pkill -x qemu-system-xtensa >/dev/null 2>&1 || true")


def parse_free_heap(out: str) -> int:
    m = re.search(r"free_heap=(\d+)\s+bytes", out)
    if m is None:
        raise RuntimeError("failed to parse free_heap")
    return int(m.group(1))


def main() -> int:
    run(f"{IDF_EXPORT} && idf.py build")
    generate_qemu_flash()
    ensure_qemu_efuse()
    run("pkill -x qemu-system-xtensa >/dev/null 2>&1 || true")

    qemu_proc = launch_qemu()
    sock = None
    try:
        sock = wait_socket("127.0.0.1", 5555, timeout_s=20.0)
        sock.sendall(b"\n")
        boot = recv_until(sock, b"xv6> ", timeout_s=30.0).decode(errors="ignore")
        print(boot)

        start_mem = parse_free_heap(cmd(sock, "mem", verbose=True))
        total_cmds = 1200

        for i in range(total_cmds):
            if i % 3 == 0:
                out = cmd(sock, "head -c 1 /etc/motd | stdinhead 1")
                assert "x" in out
            elif i % 3 == 1:
                out = cmd(sock, "head -c 4 /etc/motd > /tmp/soak.txt")
                assert "xv6> " in out
            else:
                out = cmd(sock, "stdinhead 4 < /tmp/soak.txt")
                assert "xv6-" in out

            if (i + 1) % 200 == 0:
                print(f"soak progress: {i + 1}/{total_cmds}")

        out = cmd(sock, "ps", verbose=True)
        assert "PID STATE EXIT REASON" in out
        assert "ksh" in out
        assert "- NOJOBS -" in out

        end_mem = parse_free_heap(cmd(sock, "mem", verbose=True))
        drift = start_mem - end_mem
        print(f"heap drift: {drift} bytes")
        assert drift < 32768
    finally:
        if sock is not None:
            sock.close()
        stop_qemu(qemu_proc)

    print("QEMU soak test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
