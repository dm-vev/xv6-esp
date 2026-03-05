#!/usr/bin/env python3
import argparse
import os
import sys
import time

try:
    import serial  # type: ignore
except Exception:
    serial = None

from hil_smoke_esp import (  # noqa: E402
    IDF_EXPORT,
    cmd,
    expect_contains,
    extract_job_id,
    flash_cmd,
    read_until,
    run,
    send_ctrl_c,
    serial_write,
    sync_prompt,
)


def send_ctrl_z(ser: "serial.Serial") -> str:
    serial_write(ser, b"\x1a")
    out = read_until(ser, b"xv6> ", timeout_s=12.0)
    print("^Z\n" + out)
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=os.environ.get("HIL_PORT", "/dev/ttyACM0"))
    parser.add_argument("--baud", type=int, default=int(os.environ.get("HIL_BAUD", "115200")))
    parser.add_argument("--flash", action="store_true", help="flash image before POSIX run")
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

        out = cmd(ser, "export PATH=/bin:/usr/bin:.")
        expect_contains(out, "xv6> ", "restore path")

        serial_write(ser, b"sleep 5000\n")
        time.sleep(0.2)
        out = send_ctrl_z(ser)
        expect_contains(out, "^Z", "hil posix ctrl-z")

        out = cmd(ser, "jobs")
        expect_contains(out, "stopped", "hil posix stopped job")
        expect_contains(out, "sleep 5000", "hil posix sleep job")
        jid = extract_job_id(out)
        if jid is None:
            raise RuntimeError("hil posix: failed to parse stopped job id")

        out = cmd(ser, f"bg {jid}")
        expect_contains(out, f"bg: continued {jid}", "hil posix bg")

        serial_write(ser, f"fg {jid}\n".encode())
        time.sleep(0.2)
        out = send_ctrl_c(ser)
        expect_contains(out, "^C", "hil posix ctrl-c")
        expect_contains(out, "fg: done 130", "hil posix fg reap")

        out = cmd(ser, "cat /dev/tty &")
        jid = extract_job_id(out)
        if jid is None:
            raise RuntimeError("hil posix: failed to parse tty-reader job id")
        time.sleep(0.2)

        out = cmd(ser, "jobs")
        expect_contains(out, f"[{jid}]", "hil posix tty reader id")
        expect_contains(out, "stopped", "hil posix tty reader stop")
        expect_contains(out, "cat /dev/tty", "hil posix tty reader command")

        out = cmd(ser, f"kill {jid}")
        expect_contains(out, "kill: ok", "hil posix kill tty reader")
        out = cmd(ser, f"wait {jid}")
        expect_contains(out, "wait: done 137", "hil posix wait tty reader")

        out = cmd(ser, "hostabi_probe", timeout_s=90.0)
        expect_contains(out, "PROBE SUMMARY failures=0", "hil posix hostabi probe")
        if "hostabi_probe: exit=" in out:
            raise RuntimeError("hil posix: hostabi_probe reported non-zero exit")

        print("HIL POSIX test passed")
        return 0
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
