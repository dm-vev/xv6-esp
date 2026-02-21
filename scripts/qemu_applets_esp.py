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
APPLETS_DIR = ROOT / "applets"


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


def cmd(sock: socket.socket, command: str, timeout_s: float = 12.0) -> str:
    sock.sendall((command + "\n").encode())
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


def parse_manifest_applets() -> list[str]:
    names: list[str] = []
    for manifest in sorted(APPLETS_DIR.glob("*/applet.cmake")):
        text = manifest.read_text(encoding="utf-8", errors="ignore")
        m_name = re.search(r"\bNAME\s+([A-Za-z0-9_.-]+)", text)
        if m_name is None:
            continue
        m_enabled = re.search(r"\bENABLED\s+([A-Za-z0-9_.-]+)", text)
        enabled = (m_enabled.group(1).upper() if m_enabled else "ON")
        if enabled == "OFF":
            continue
        names.append(m_name.group(1))
    return names


def assert_ok_output(applet: str, out: str) -> None:
    bad_markers = (
        "command not found",
        "elf load failed",
        "module '",
        "unresolved symbol:",
        "Guru Meditation Error",
        "panic'ed",
        "Backtrace:",
        "Traceback (most recent call last)",
    )
    for marker in bad_markers:
        if marker in out:
            raise AssertionError(f"{applet}: detected failure marker '{marker}'")


def test_matrix() -> dict[str, str]:
    return {
        "basename": "basename /sample.txt",
        "cat": "cat /sample.txt",
        "cmp": "cmp /sample.txt /sample.txt",
        "cp": "cp /sample.txt /sample.cp",
        "dd": "dd if=/sample.txt of=/dd.out bs=1 count=4",
        "dirname": "dirname /sample.txt",
        "echo": "echo hello",
        "head": "head -c 4 /sample.txt",
        "ls": "ls /",
        "mkdir": "mkdir /applet_dir",
        "mv": "mv /sample.cp /sample.mv",
        "printenv": "printenv",
        "pwd": "pwd",
        "rev": "rev /sample.txt",
        "rm": "rm /sample.mv",
        "rmdir": "rmdir /applet_dir",
        "sleep": "sleep 1",
        "split": "split -b 4 /sample.txt /split_",
        "sum": "sum /sample.txt",
        "tee": "head -c 4 /sample.txt | tee /tee.out",
        "touch": "touch /touch.out",
        "tr": "head -c 4 /sample.txt | tr a A",
        "uname": "uname",
        "uniq": "uniq /sample.txt",
        "wc": "wc /sample.txt",
    }


def main() -> int:
    run(f"{IDF_EXPORT} && idf.py build")
    generate_qemu_flash()
    ensure_qemu_efuse()
    run("pkill -x qemu-system-xtensa >/dev/null 2>&1 || true")

    applets = parse_manifest_applets()
    if not applets:
        raise RuntimeError("No enabled applets found in applets/*/applet.cmake")

    matrix = test_matrix()
    missing = [a for a in applets if a not in matrix]
    if missing:
        raise RuntimeError(f"No test command defined for applets: {', '.join(missing)}")

    for applet in applets:
        qemu_proc = launch_qemu()
        sock = None
        try:
            sock = wait_socket("127.0.0.1", 5555, timeout_s=20.0)
            sock.sendall(b"\n")
            boot = recv_until(sock, b"xv6> ", timeout_s=30.0).decode(errors="ignore")
            print(boot)
            cmd(sock, "echo sample > /sample.txt")
            out = cmd(sock, matrix[applet], timeout_s=20.0)
            assert_ok_output(applet, out)
        finally:
            if sock is not None:
                sock.close()
            stop_qemu(qemu_proc)

    print(f"QEMU applet test passed ({len(applets)} applets)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
