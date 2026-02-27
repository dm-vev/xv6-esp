#!/usr/bin/env python3
import argparse
import difflib
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
APPLETS_DIR = ROOT / "applets"
HOSTABI_EXPECTED = ROOT / "docs" / "tests" / "hostabi_probe.expected"


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


def log(msg: str) -> None:
    ts = time.strftime("%H:%M:%S")
    print(f"[{ts}] {msg}", flush=True)


def run(cmd: str) -> None:
    subprocess.run(["bash", "-lc", cmd], cwd=ROOT, check=True)


def flash_cmd(port: str) -> str:
    quoted_port = port.replace("'", "'\"'\"'")
    if "ttyACM" in port:
        return f"{IDF_EXPORT} && idf.py -D ESPTOOLPY_AFTER=no_reset -p '{quoted_port}' flash"
    return f"{IDF_EXPORT} && idf.py -p '{quoted_port}' flash"


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


def parse_applet_flags(applet: str) -> list[str]:
    flags: set[str] = set()
    applet_dir = APPLETS_DIR / applet
    if not applet_dir.exists():
        return []
    for src in applet_dir.glob("*.c"):
        text = src.read_text(encoding="utf-8", errors="ignore")
        for m in re.finditer(r"case\s+'([A-Za-z0-9])'\s*:", text):
            flags.add(m.group(1))
    return sorted(flags)


def build_flag_command(applet: str, fl: str) -> str | None:
    if applet == "cat":
        return f"cat -{fl} /no_such_file"
    if applet == "cmp":
        return f"cmp -{fl} /no_such_file /no_such_file2"
    if applet == "cp":
        return f"cp -{fl}"
    if applet == "ls":
        return f"ls -{fl} /"
    if applet == "mkdir":
        return f"mkdir -{fl} /tmp/integration/auto"
    if applet == "mv":
        return f"mv -{fl} /no_src /no_dst"
    if applet == "rm":
        return f"rm -{fl} /no_such_file"
    if applet == "tee":
        return f"echo sample | tee -{fl} /tee_auto.out"
    if applet == "touch":
        return f"touch -{fl} /touch_auto.out"
    if applet == "tr":
        if fl == "d":
            return "echo abc | tr -d a"
        if fl == "c":
            return "echo abc | tr -c a b"
        if fl == "s":
            return "echo aaabbb | tr -s a"
        return None
    if applet == "uname":
        return f"uname -{fl}"
    if applet == "uniq":
        return f"uniq -{fl} /no_such_file"
    if applet == "wc":
        return f"wc -{fl} /no_such_file"
    return None


def test_matrix() -> dict[str, list[str]]:
    return {
        "basename": ["basename /bin/echo", "basename /bin/echo .x"],
        "cat": [
            "cat -u /no_such_file",
            "cat -n /no_such_file",
            "cat /etc/rc",
            "cat /home/README",
        ],
        "cmp": [
            "cmp -s /tmp/integration/cmp.a /tmp/integration/cmp.a",
            "cmp /tmp/integration/cmp.a /tmp/integration/cmp.b",
            "cmp -s /no_such_file /no_such_file2",
            "cmp -l /no_such_file /no_such_file2",
        ],
        "cp": [
            "cp /etc/rc /cp_echo",
            "cp /home/README /cp_cat",
            "cp -p /etc/rc /cp_echo",
            "cp -r /tmp/integration /tmp/integration_copy",
        ],
        "dd": ["dd if=/no_such_input of=/dd_echo bs=16 count=1", "dd conv=unknown if=/no_such_input of=/dd_cat"],
        "dirname": ["dirname /bin/echo", "dirname /bin"],
        "dlhello": ["dlhello"],
        "sh": ["sh -c \"echo sh-ok\""],
        "echo": ["echo -n hello", "echo world"],
        "head": ["head -2 /no_such_file", "head -2 /tmp/integration/uniq.in"],
        "hostabi_probe": ["hostabi_probe"],
        "ls": ["ls /", "ls /bin"],
        "mkdir": [
            "mkdir -p /tmp/integration/a",
            "mkdir -p /tmp/integration/b",
            "mkdir -p /tmp/integration/a/b/c",
            "mkdir -p /tmp/integration/a/b/c/",
        ],
        "mv": [
            "mv -f /cp_echo /mv_echo",
            "mv -f /cp_cat /mv_cat",
            "mv /tmp/integration/cmp.a /tmp/integration/cmp.a.moved",
        ],
        "net_diag": ["net_diag stats", "net_diag selftest"],
        "printenv": ["printenv", "printenv PATH"],
        "pwd": ["pwd"],
        "rev": ["rev /no_such_file"],
        "rm": ["rm -f /mv_echo", "rm -f /mv_cat", "rm -f /tmp/integration/cmp.a.moved"],
        "rmdir": ["rmdir /tmp/integration/a", "rmdir /tmp/integration/b"],
        "sleep": ["sleep 1"],
        "split": [
            "split -2 /tmp/integration/split.in /tmp/integration/x_",
            "split -2 /no_such_file /split_echo_",
            "split -2 /no_such_file /split_cat_",
            "split -0 /tmp/integration/split.in /tmp/integration/x_bad_",
        ],
        "sum": ["sum /no_such_file"],
        "tee": ["echo sample | tee -a /tee.out", "echo next | tee -a /tee.out"],
        "touch": ["touch /touch.out", "touch /touch2.out"],
        "tcp_loop_test": ["tcp_loop_test"],
        "tr": ["echo abc | tr a A", "echo xyz | tr x X"],
        "uname": ["uname", "uname -a", "uname -r"],
        "uniq": [
            "uniq /tmp/integration/uniq.in /tmp/integration/uniq.out",
            "uniq -c /tmp/integration/uniq.in",
            "uniq -c /no_such_file",
            "uniq -u /no_such_file",
            "uniq -999999999999 /tmp/integration/uniq.in",
        ],
        "wc": [
            "wc -l /tmp/integration/wc.in",
            "wc /tmp/integration/wc.in",
            "wc -l /no_such_file",
            "wc -wc /no_such_file",
        ],
    }


def build_applet_commands(applet: str, matrix: dict[str, list[str]]) -> list[str]:
    cmds: list[str] = []
    seen: set[str] = set()
    for c in matrix.get(applet, [applet]):
        if c not in seen:
            cmds.append(c)
            seen.add(c)
    for fl in parse_applet_flags(applet):
        c = build_flag_command(applet, fl)
        if c is None:
            continue
        if c not in seen:
            cmds.append(c)
            seen.add(c)
    return cmds


def read_until(ser: "serial.Serial", marker: bytes, timeout_s: float) -> str:
    start = time.time()
    data = bytearray()
    while time.time() - start < timeout_s:
        chunk = ser.read(4096)
        if chunk:
            data.extend(chunk)
            if marker in data:
                return data.decode(errors="ignore")
        else:
            time.sleep(0.02)
    tail = data[-512:].decode(errors="ignore")
    raise RuntimeError(f"timeout waiting for marker {marker!r}; serial tail={tail!r}")


def sync_prompt(ser: "serial.Serial", timeout_s: float = 30.0) -> str:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        ser.write(b"\n")
        ser.flush()
        try:
            return read_until(ser, b"xv6> ", 1.5)
        except RuntimeError:
            continue
    raise RuntimeError("timeout waiting for shell prompt")


def reboot_and_sync(ser: "serial.Serial", timeout_s: float = 75.0) -> str:
    log("rebooting target")
    ser.write(b"reboot\n")
    ser.flush()
    try:
        out = read_until(ser, b"xv6> ", timeout_s)
        log("prompt is ready after reboot")
        return out
    except RuntimeError:
        out = sync_prompt(ser, timeout_s=30.0)
        log("prompt recovered via sync after reboot timeout")
        return out


def hard_reset_uart(ser: "serial.Serial") -> None:
    """Try to reset ESP via USB CDC control lines (best-effort)."""
    try:
        log("attempting hardware reset via DTR/RTS")
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.12)
        ser.setDTR(True)
        ser.setRTS(False)
        time.sleep(0.12)
        ser.setDTR(False)
        ser.setRTS(False)
        time.sleep(0.12)
        ser.reset_input_buffer()
    except Exception as exc:
        log(f"hardware reset via DTR/RTS failed: {exc}")


def assert_ok_output(applet: str, out: str) -> None:
    bad_markers = (
        "command not found",
        "elf load failed",
        "module '",
        "unresolved symbol:",
        "jobs: spawn failed",
        "cat: read error",
        "cat: write error",
        "Guru Meditation Error",
        "panic'ed",
        "Backtrace:",
        "Traceback (most recent call last)",
        "alloc failed",
    )
    for marker in bad_markers:
        if marker in out:
            raise AssertionError(f"{applet}: detected failure marker '{marker}'")

    if applet == "printenv":
        if "printenv: exit=1" in out:
            raise AssertionError("printenv: unexpected exit=1")
        if "PATH=" not in out and "/bin:/usr/bin:." not in out:
            raise AssertionError("printenv: expected PATH output")
    if applet == "hostabi_probe":
        expected = [
            line.strip()
            for line in HOSTABI_EXPECTED.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.startswith("#")
        ]
        actual = [line.strip() for line in out.splitlines() if line.startswith("PROBE ")]
        if "hostabi_probe: exit=" in out:
            raise AssertionError("hostabi_probe: non-zero exit code")
        if actual != expected:
            diff = "\n".join(
                difflib.unified_diff(
                    expected,
                    actual,
                    fromfile=str(HOSTABI_EXPECTED),
                    tofile="hil:hostabi_probe-output",
                    lineterm="",
                )
            )
            raise AssertionError(f"hostabi_probe output mismatch\n{diff}")


def cmd(ser: "serial.Serial", command: str, timeout_s: float = 90.0) -> str:
    def _sanitize(text: str) -> str:
        return "".join(ch for ch in text if ch in "\r\n\t" or 32 <= ord(ch) <= 126)

    ser.write((command + "\n").encode())
    ser.flush()
    out = read_until(ser, b"xv6> ", timeout_s)
    print(f"$ {command}\n{_sanitize(out)}", flush=True)
    return out


def cmd_with_recovery(ser: "serial.Serial", command: str, timeout_s: float = 90.0) -> str:
    try:
        return cmd(ser, command, timeout_s=timeout_s)
    except RuntimeError as exc:
        log(f"command timeout: {command!r}: {exc}")
        try:
            sync_prompt(ser, timeout_s=5.0)
            log("prompt recovered after timeout with sync")
        except RuntimeError:
            log("sync recovery failed, trying hardware reset")
            hard_reset_uart(ser)
            try:
                sync_prompt(ser, timeout_s=10.0)
                log("prompt recovered after hardware reset")
            except RuntimeError:
                log("hardware reset recovery failed, forcing reboot")
                reboot_and_sync(ser, timeout_s=75.0)
        raise


def prepare_shell_state(ser: "serial.Serial") -> None:
    cleanup_cmds = (
        "rm -f /tee.out /tee_auto.out /touch.out /touch2.out /touch_auto.out",
        "rm -f /mv_echo /mv_cat /cp_echo /cp_cat /dd_echo /dd_cat",
        "rm -f /tmp/integration/uniq.in /tmp/integration/uniq.out",
        "rm -f /tmp/integration/split.in /tmp/integration/x_aa /tmp/integration/x_ab /tmp/integration/x_ac",
        "rm -f /tmp/integration/cmp.a /tmp/integration/cmp.b /tmp/integration/wc.in /tmp/integration/cmp.a.moved",
        "rmdir /tmp/integration_copy",
        "rmdir /tmp/integration/a",
        "rmdir /tmp/integration/b",
        "rmdir /tmp/integration/auto",
    )
    for c in cleanup_cmds:
        out = cmd_with_recovery(ser, c, timeout_s=30.0)
        assert_ok_output("setup", out)

    fixture_cmds = (
        "mkdir -p /tmp/integration",
        "echo alpha > /tmp/integration/uniq.in",
        "echo alpha >> /tmp/integration/uniq.in",
        "echo beta >> /tmp/integration/uniq.in",
        "echo beta >> /tmp/integration/uniq.in",
        "echo gamma >> /tmp/integration/uniq.in",
        "cp /tmp/integration/uniq.in /tmp/integration/wc.in",
        "cp /tmp/integration/uniq.in /tmp/integration/cmp.a",
        "cp /tmp/integration/uniq.in /tmp/integration/cmp.b",
        "echo delta >> /tmp/integration/cmp.b",
        "echo one > /tmp/integration/split.in",
        "echo two >> /tmp/integration/split.in",
        "echo three >> /tmp/integration/split.in",
        "echo four >> /tmp/integration/split.in",
    )
    for c in fixture_cmds:
        out = cmd_with_recovery(ser, c, timeout_s=30.0)
        assert_ok_output("setup", out)

    out = cmd_with_recovery(ser, "wc -l /tmp/integration/uniq.in", timeout_s=30.0)
    assert_ok_output("setup", out)
    if " 5 /tmp/integration/uniq.in" not in out and "\t5 /tmp/integration/uniq.in" not in out:
        raise AssertionError("setup: /tmp/integration/uniq.in was not created correctly")

    out = cmd_with_recovery(ser, "wc -l /tmp/integration/split.in", timeout_s=30.0)
    assert_ok_output("setup", out)
    if " 4 /tmp/integration/split.in" not in out and "\t4 /tmp/integration/split.in" not in out:
        raise AssertionError("setup: /tmp/integration/split.in was not created correctly")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=os.environ.get("APPLETS_PORT", ""))
    parser.add_argument("--baud", type=int, default=int(os.environ.get("APPLETS_BAUD", "115200")))
    parser.add_argument("--flash", action="store_true", help="flash before running tests")
    parser.add_argument(
        "--max-seconds",
        type=int,
        default=int(os.environ.get("HIL_APPLETS_MAX_SECONDS", "2400")),
        help="global timeout for full HIL applet suite",
    )
    args = parser.parse_args()

    if serial is None:
        raise RuntimeError("pyserial is not installed. Install with: pip install pyserial")
    if not args.port:
        raise RuntimeError("serial port is required. Use --port or APPLETS_PORT env")

    if args.flash:
        log(f"flashing firmware on {args.port}")
        run(flash_cmd(args.port))

    applets = parse_manifest_applets()
    matrix = test_matrix()
    missing = [a for a in applets if a not in matrix]
    if missing:
        raise RuntimeError(f"No test command defined for applets: {', '.join(missing)}")

    suite_start = time.time()
    ser = serial.Serial(args.port, args.baud, timeout=0.2, write_timeout=1.0)
    current_applet = "<boot>"
    current_command = "<none>"
    try:
        log(f"open serial {args.port} @ {args.baud}")
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        try:
            _boot = sync_prompt(ser, timeout_s=60.0)
        except RuntimeError:
            hard_reset_uart(ser)
            _boot = sync_prompt(ser, timeout_s=45.0)
        for applet in applets:
            if time.time() - suite_start > args.max_seconds:
                raise RuntimeError(f"global timeout reached after {args.max_seconds}s")
            current_applet = applet
            log(f"=== applet: {applet} ===")
            reboot_and_sync(ser, timeout_s=75.0)
            current_command = "export PATH=/bin:/usr/bin:."
            cmd_with_recovery(ser, current_command)
            prepare_shell_state(ser)
            commands = build_applet_commands(applet, matrix)
            if not commands:
                raise RuntimeError(f"No commands generated for applet {applet}")
            log(f"{applet}: {len(commands)} commands")
            for command in commands:
                current_command = command
                out = cmd_with_recovery(ser, command, timeout_s=90.0)
                assert_ok_output(applet, out)
        print(f"HIL applet test passed ({len(applets)} applets, exhaustive flags)")
        return 0
    except Exception as exc:
        log(f"FAILED at applet={current_applet!r}, command={current_command!r}: {exc}")
        try:
            ser.write(b"\n")
            ser.flush()
            trailer = read_until(ser, b"xv6> ", timeout_s=2.0)
            log(f"serial trailer after failure:\n{trailer}")
        except Exception as trailer_exc:
            log(f"serial trailer unavailable: {trailer_exc}")
        raise
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
