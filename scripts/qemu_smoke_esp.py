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
    home = Path.home()
    candidates.extend((home / "esp-idf", Path("/opt/esp-idf"), Path("/root/esp-idf"), Path("/tmp/esp-idf")))
    for p in candidates:
        if p and has_export_script(p):
            return f"source {p}/export.sh >/dev/null"
    raise RuntimeError("ESP-IDF not found. Set IDF_PATH or install under ~/esp-idf.")


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
        except TimeoutError:
            continue
        if not chunk:
            continue
        data.extend(chunk)
        if marker in data:
            return bytes(data)
    tail = data[-512:].decode(errors="ignore")
    raise RuntimeError(f"timeout waiting for marker {marker!r}; serial tail={tail!r}")


def sync_prompt(sock: socket.socket, timeout_s: float = 30.0) -> str:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        sock.sendall(b"\n")
        try:
            return recv_until(sock, b"xv6> ", timeout_s=2.0).decode(errors="ignore")
        except RuntimeError:
            continue
    raise RuntimeError("timeout waiting for shell prompt")


def cmd(sock: socket.socket, command: str) -> str:
    sock.sendall((command + "\n").encode())
    out = recv_until(sock, b"xv6> ", timeout_s=12.0).decode(errors="ignore")
    print(f"$ {command}\n{out}")
    return out


def send_ctrl_c(sock: socket.socket) -> str:
    sock.sendall(b"\x03")
    out = recv_until(sock, b"xv6> ", timeout_s=12.0).decode(errors="ignore")
    print("^C\n" + out)
    return out


def extract_job_id(out: str) -> str | None:
    m = re.search(r"\[(\d+)\]\s*started", out)
    if m is not None:
        return m.group(1)
    m = re.search(r"\[(\d+)", out)
    if m is not None:
        return m.group(1)
    return None


def start_bg_job(sock: socket.socket, command: str, job_hint: str) -> str:
    out = cmd(sock, command)
    job_id = extract_job_id(out)
    if job_id is not None:
        return job_id

    out_jobs = cmd(sock, "jobs")
    m = re.search(r"\[(\d+)\].*" + re.escape(job_hint), out_jobs)
    if m is not None:
        return m.group(1)

    raise RuntimeError(f"failed to parse background job id for {command!r}\n{out}\n{out_jobs}")


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


def main() -> int:
    if os.environ.get("XV6_SKIP_BUILD") != "1":
        run(f"{IDF_EXPORT} && idf.py set-target esp32s3 && idf.py build")
    generate_qemu_flash()
    ensure_qemu_efuse()
    run("pkill -x qemu-system-xtensa >/dev/null 2>&1 || true")

    qemu_proc = launch_qemu()
    sock = None
    try:
        sock = wait_socket("127.0.0.1", 5555, timeout_s=20.0)
        try:
            boot = recv_until(sock, b"xv6> ", timeout_s=60.0).decode(errors="ignore")
        except RuntimeError:
            boot = sync_prompt(sock, timeout_s=45.0)
        print(boot)

        out = cmd(sock, "echo xv6-esp > /tmp/motd.txt")
        assert "xv6> " in out

        out = cmd(sock, "head -1 /tmp/motd.txt")
        assert "xv6-esp" in out

        out = cmd(sock, "dd if=/dev/zero of=/tmp/dd.bin bs=16 count=2")
        assert "records out" in out

        out = cmd(sock, "ls /tmp")
        assert "dd.bin" in out

        out = cmd(sock, "cp /tmp/dd.bin /tmp/dd2.bin")
        assert "xv6> " in out

        out = cmd(sock, "mv /tmp/dd2.bin /tmp/dd3.bin")
        assert "xv6> " in out

        out = cmd(sock, "ls /tmp")
        assert "dd3.bin" in out

        bg_id = start_bg_job(sock, "sleep 400 &", "sleep 400")

        out = cmd(sock, "jobs")
        assert f"[{bg_id}]" in out
        assert "sleep 400" in out

        out = cmd(sock, "ps")
        assert "PID STATE EXIT REASON" in out
        assert "sh" in out

        out = cmd(sock, "health")
        assert "health: uptime_ms=" in out
        assert "free_heap_bytes=" in out

        out = cmd(sock, "echo \"hello world\"")
        assert "hello world" in out

        out = cmd(sock, "wait")
        assert "wait: done" in out

        out = cmd(sock, "pwd")
        assert "/" in out

        out = cmd(sock, "cd /tmp")
        assert "xv6> " in out

        out = cmd(sock, "pwd")
        assert "/tmp" in out

        out = cmd(sock, "head -1 /tmp/motd.txt")
        assert "xv6-esp" in out

        out = cmd(sock, "cd /")
        assert "xv6> " in out

        out = cmd(sock, "head -1 /tmp/motd.txt | cat")
        assert "xv6-esp" in out

        out = cmd(sock, "head -1 /tmp/motd.txt | cat | cat")
        assert "xv6-esp" in out

        out = cmd(sock, "head -1 /tmp/motd.txt > /tmp/r.txt")
        assert "xv6> " in out

        out = cmd(sock, "head -1 /tmp/motd.txt >> /tmp/r.txt")
        assert "xv6> " in out

        out = cmd(sock, "cat /tmp/r.txt")
        assert "xv6-esp" in out

        out = cmd(sock, "head -1 /no_such_file 2> /tmp/err.log")
        assert "xv6> " in out

        out = cmd(sock, "ls /tmp")
        assert "r.txt" in out
        assert "err.log" in out

        out = cmd(sock, "cp /bin/head /tmp/myhead")
        assert "xv6> " in out

        out = cmd(sock, "export PATH=/tmp")
        assert "xv6> " in out

        out = cmd(sock, "myhead -4 /tmp/motd.txt")
        assert "xv6-esp" in out

        out = cmd(sock, "unset PATH")
        assert "xv6> " in out

        out = cmd(sock, "head -1 /tmp/motd.txt")
        assert "xv6-esp" in out

        out = cmd(sock, "export PATH=/bin:/usr/bin:.")
        assert "xv6> " in out

        out = cmd(sock, "env")
        assert "PATH=/bin:/usr/bin:." in out

        out = cmd(sock, "ps > /tmp/ps.txt")
        assert "xv6> " in out

        out = cmd(sock, "cat /tmp/ps.txt")
        assert "PID" in out

        out = cmd(sock, "time head -1 /tmp/motd.txt 2> /tmp/time.err")
        assert "xv6-esp" in out

        out = cmd(sock, "cat /tmp/time.err")
        assert "time" in out

        out = cmd(sock, "ulimit -t 150")
        assert "xv6> " in out

        out = cmd(sock, "ulimit -t")
        assert "150" in out

        out = cmd(sock, "ulimit -t 0")
        assert "xv6> " in out

        out = cmd(sock, "time head -1 /tmp/motd.txt")
        assert "time:" in out

        fg_id = start_bg_job(sock, "sleep 800 &", "sleep 800")

        out = cmd(sock, f"fg {fg_id}")
        assert "fg: done 0" in out

        kill_id = start_bg_job(sock, "sleep 2000 &", "sleep 2000")

        out = cmd(sock, f"kill {kill_id}")
        assert "kill: ok" in out

        out = cmd(sock, f"wait {kill_id}")
        assert "wait: done 137" in out

        sock.sendall(b"sleep 5000\n")
        time.sleep(0.2)
        out = send_ctrl_c(sock)
        assert "^C" in out

        out = cmd(sock, "limit 100 1 sleep 500")
        assert "limit: timeout" in out

        out = cmd(sock, "ptydemo")
        if "command not found" not in out:
            assert "slave:ping" in out
            assert "master:pong" in out

            out = cmd(sock, "ptysend hello-from-elf")
            m = re.search(r"/dev/pts/[0-9]+", out)
            assert m is not None
            slave = m.group(0)

            out = cmd(sock, f"ptyrecv {slave}")
            assert "hello-from-elf" in out

        out = cmd(sock, "dd if=/dev/zero of=/dev/full bs=4 count=1")
        assert ("write: I/O error" in out) or ("dd: write failed" in out)

        out = cmd(sock, "hostabi_probe")
        assert "PROBE SUMMARY failures=0" in out
        assert "hostabi_probe: exit=" not in out
    finally:
        if sock is not None:
            sock.close()
        stop_qemu(qemu_proc)

    print("QEMU smoke test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
