#!/usr/bin/env python3
import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

try:
    import serial  # type: ignore
except Exception:
    serial = None

ROOT = Path(__file__).resolve().parents[1]


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


def flash_cmd(port: str) -> str:
    quoted_port = port.replace("'", "'\"'\"'")
    if "ttyACM" in port:
        return f"{IDF_EXPORT} && idf.py -D ESPTOOLPY_AFTER=no_reset -p '{quoted_port}' flash"
    return f"{IDF_EXPORT} && idf.py -p '{quoted_port}' flash"


def serial_write(ser: "serial.Serial", data: bytes) -> None:
    try:
        ser.write(data)
        ser.flush()
    except Exception as exc:
        raise RuntimeError(f"serial write failed: {exc}") from exc


def read_until(ser: "serial.Serial", marker: bytes, timeout_s: float = 12.0) -> str:
    start = time.time()
    data = bytearray()
    while time.time() - start < timeout_s:
        chunk = ser.read(4096)
        if not chunk:
            time.sleep(0.02)
            continue
        data.extend(chunk)
        if marker in data:
            return data.decode(errors="ignore")
    raise RuntimeError(f"timeout waiting for marker {marker!r}")


def sync_prompt(ser: "serial.Serial", timeout_s: float = 30.0) -> str:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        serial_write(ser, b"\n")
        try:
            return read_until(ser, b"xv6> ", timeout_s=1.5)
        except RuntimeError:
            continue
    raise RuntimeError("timeout waiting for shell prompt")


def cmd(ser: "serial.Serial", command: str, timeout_s: float = 20.0) -> str:
    serial_write(ser, (command + "\n").encode())
    out = read_until(ser, b"xv6> ", timeout_s=timeout_s)
    print(f"$ {command}\n{out}")
    return out


def send_ctrl_c(ser: "serial.Serial") -> str:
    serial_write(ser, b"\x03")
    out = read_until(ser, b"xv6> ", timeout_s=12.0)
    print("^C\n" + out)
    return out


def expect_contains(out: str, needle: str, context: str) -> None:
    if needle not in out:
        raise RuntimeError(f"{context}: expected '{needle}' in output\n{out}")


def expect_any(out: str, needles: tuple[str, ...], context: str) -> None:
    for needle in needles:
        if needle in out:
            return
    raise RuntimeError(f"{context}: expected one of {needles!r} in output\n{out}")


def expect_match(pattern: str, out: str, context: str) -> re.Match[str]:
    m = re.search(pattern, out)
    if m is None:
        raise RuntimeError(f"{context}: pattern {pattern!r} not found in output\n{out}")
    return m


def extract_job_id(out: str) -> str | None:
    m = re.search(r"\[(\d+)\]\s*started", out)
    if m is not None:
        return m.group(1)
    m = re.search(r"\[(\d+)", out)
    if m is not None:
        return m.group(1)
    return None


def start_bg_job(ser: "serial.Serial", command: str, job_hint: str) -> str:
    out = cmd(ser, command)
    job_id = extract_job_id(out)
    if job_id is not None:
        return job_id

    out_jobs = cmd(ser, "jobs")
    m = re.search(r"\[(\d+)\].*" + re.escape(job_hint), out_jobs)
    if m is not None:
        return m.group(1)

    raise RuntimeError(f"failed to parse background job id for {command!r}\n{out}\n{out_jobs}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=os.environ.get("HIL_PORT", "/dev/ttyACM0"))
    parser.add_argument("--baud", type=int, default=int(os.environ.get("HIL_BAUD", "115200")))
    parser.add_argument("--flash", action="store_true", help="flash image before smoke run")
    args = parser.parse_args()

    if serial is None:
        raise RuntimeError("pyserial is not installed. Install with: pip install pyserial")

    if os.environ.get("XV6_SKIP_BUILD") != "1":
        run(f"{IDF_EXPORT} && idf.py set-target esp32s3 && idf.py build")
    if args.flash:
        run(flash_cmd(args.port))

    ser = serial.Serial(args.port, args.baud, timeout=0.2, write_timeout=1.0, dsrdtr=False, rtscts=False, xonxoff=False)
    try:
        ser.reset_input_buffer()
        boot = sync_prompt(ser, timeout_s=45.0)
        print(boot)

        out = cmd(ser, "echo xv6-esp > /tmp/motd.txt")
        expect_contains(out, "xv6> ", "create motd")

        out = cmd(ser, "cat /tmp/motd.txt")
        expect_contains(out, "xv6-esp", "read motd")

        out = cmd(ser, "dd if=/dev/zero of=/tmp/dd.bin bs=16 count=2")
        expect_contains(out, "records out", "dd create file")

        out = cmd(ser, "cp /tmp/dd.bin /tmp/dd2.bin")
        expect_contains(out, "xv6> ", "cp dd file")

        out = cmd(ser, "mv /tmp/dd2.bin /tmp/dd3.bin")
        expect_contains(out, "xv6> ", "mv dd file")

        out = cmd(ser, "ls /tmp")
        expect_contains(out, "dd3.bin", "ls tmp")

        bg_id = start_bg_job(ser, "sleep 400 &", "sleep 400")

        out = cmd(ser, "jobs")
        expect_contains(out, f"[{bg_id}]", "jobs list id")
        expect_contains(out, "sleep 400", "jobs list")

        out = cmd(ser, "ps")
        expect_contains(out, "PID STATE EXIT REASON", "ps header")
        expect_contains(out, "sh", "ps shell")

        out = cmd(ser, "health")
        expect_contains(out, "health: uptime_ms=", "health uptime")
        expect_contains(out, "free_heap_bytes=", "health free heap")

        out = cmd(ser, "wait")
        expect_contains(out, "wait: done", "wait bg sleep")

        out = cmd(ser, "cat /tmp/motd.txt | cat | cat")
        expect_contains(out, "xv6-esp", "pipeline cat")

        out = cmd(ser, "cat /tmp/motd.txt > /tmp/r.txt")
        expect_contains(out, "xv6> ", "redirect create")

        out = cmd(ser, "cat /tmp/motd.txt >> /tmp/r.txt")
        expect_contains(out, "xv6> ", "redirect append")

        out = cmd(ser, "cat /tmp/r.txt")
        expect_contains(out, "xv6-esp", "read redirected file")

        out = cmd(ser, "cp /bin/cat /tmp/mycat")
        expect_contains(out, "xv6> ", "copy applet")

        out = cmd(ser, "export PATH=/tmp")
        expect_contains(out, "xv6> ", "set path tmp")

        out = cmd(ser, "mycat /tmp/motd.txt")
        expect_contains(out, "xv6-esp", "run copied applet")

        out = cmd(ser, "unset PATH")
        expect_contains(out, "xv6> ", "unset path")

        out = cmd(ser, "cat /tmp/motd.txt")
        expect_contains(out, "xv6-esp", "cat with default path")

        out = cmd(ser, "export PATH=/bin:/usr/bin:.")
        expect_contains(out, "xv6> ", "restore path")

        out = cmd(ser, "time cat /tmp/motd.txt 2> /tmp/time.err")
        expect_contains(out, "xv6-esp", "time cat")

        out = cmd(ser, "cat /tmp/time.err")
        expect_contains(out, "time", "time stderr")

        kill_id = start_bg_job(ser, "sleep 2000 &", "sleep 2000")

        out = cmd(ser, f"kill {kill_id}")
        expect_contains(out, "kill: ok", "kill bg job")

        out = cmd(ser, f"wait {kill_id}")
        expect_contains(out, "wait: done 137", "wait killed job")

        serial_write(ser, b"sleep 5000\n")
        time.sleep(0.2)
        out = send_ctrl_c(ser)
        expect_contains(out, "^C", "ctrl-c")

        out = cmd(ser, "limit 100 1 sleep 500")
        expect_contains(out, "limit: timeout", "limit timeout")

        out = cmd(ser, "dd if=/dev/zero of=/dev/full bs=4 count=1")
        expect_any(out, ("write: I/O error", "dd: write failed"), "dd /dev/full")

        out = cmd(ser, "hostabi_probe")
        expect_contains(out, "PROBE SUMMARY failures=0", "hostabi probe summary")
        if "hostabi_probe: exit=" in out:
            raise RuntimeError("hostabi_probe: non-zero exit")

        print("HIL smoke test passed")
        return 0
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
