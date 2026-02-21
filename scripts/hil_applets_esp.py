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
    candidates.extend((Path("/root/esp-idf"), Path("/tmp/esp-idf"), Path("/home/tikhon/Документы/magnolia/esp-idf")))
    for p in candidates:
        if p and has_export_script(p):
            return f"source {p}/export.sh >/dev/null"
    raise RuntimeError("ESP-IDF not found. Set IDF_PATH or install esp-idf.")


IDF_EXPORT = resolve_idf_export()


def run(cmd: str) -> None:
    subprocess.run(["bash", "-lc", cmd], cwd=ROOT, check=True)


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
        "cat": ["cat -u /no_such_file", "cat -n /no_such_file"],
        "cmp": ["cmp -s /no_such_file /no_such_file2", "cmp -l /no_such_file /no_such_file2"],
        "cp": ["cp -p", "cp -r"],
        "dd": ["dd if=/no_such_input of=/dd_echo bs=16 count=1", "dd conv=unknown if=/no_such_input of=/dd_cat"],
        "dirname": ["dirname /bin/echo", "dirname /bin"],
        "echo": ["echo -n hello", "echo world"],
        "head": ["head -2 /no_such_file"],
        "ls": ["ls /", "ls /bin"],
        "mkdir": ["mkdir -p /tmp/integration/a", "mkdir -p /tmp/integration/b"],
        "mv": ["mv -f /cp_echo /mv_echo", "mv -f /cp_cat /mv_cat"],
        "printenv": ["printenv", "printenv PATH"],
        "pwd": ["pwd"],
        "rev": ["rev /no_such_file"],
        "rm": ["rm -f /mv_echo", "rm -f /mv_cat"],
        "rmdir": ["rmdir /tmp/integration/a", "rmdir /tmp/integration/b"],
        "sleep": ["sleep 1"],
        "split": ["split -2 /no_such_file /split_echo_", "split -2 /no_such_file /split_cat_"],
        "sum": ["sum /no_such_file"],
        "tee": ["echo sample | tee -a /tee.out", "echo next | tee -a /tee.out"],
        "touch": ["touch /touch.out", "touch /touch2.out"],
        "tr": ["echo abc | tr a A", "echo xyz | tr x X"],
        "uname": ["uname -a", "uname -r"],
        "uniq": ["uniq -c /no_such_file", "uniq -u /no_such_file"],
        "wc": ["wc -l /no_such_file", "wc -wc /no_such_file"],
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
    raise RuntimeError(f"timeout waiting for marker {marker!r}")


def assert_ok_output(applet: str, out: str) -> None:
    bad_markers = (
        "command not found",
        "elf load failed",
        "module '",
        "unresolved symbol:",
        "jobs: spawn failed",
        "Guru Meditation Error",
        "panic'ed",
        "Backtrace:",
        "Traceback (most recent call last)",
    )
    for marker in bad_markers:
        if marker in out:
            raise AssertionError(f"{applet}: detected failure marker '{marker}'")


def cmd(ser: "serial.Serial", command: str, timeout_s: float = 90.0) -> str:
    def _sanitize(text: str) -> str:
        return "".join(ch for ch in text if ch in "\r\n\t" or 32 <= ord(ch) <= 126)

    ser.write((command + "\n").encode())
    ser.flush()
    out = read_until(ser, b"xv6> ", timeout_s)
    print(f"$ {command}\n{_sanitize(out)}")
    return out


def prepare_shell_state(ser: "serial.Serial") -> None:
    cleanup_cmds = (
        "rm -f /tee.out /tee_auto.out /touch.out /touch2.out /touch_auto.out",
        "rm -f /mv_echo /mv_cat /cp_echo /cp_cat /dd_echo /dd_cat",
        "rmdir /tmp/integration/a",
        "rmdir /tmp/integration/b",
        "rmdir /tmp/integration/auto",
    )
    for c in cleanup_cmds:
        _ = cmd(ser, c, timeout_s=30.0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=os.environ.get("APPLETS_PORT", ""))
    parser.add_argument("--baud", type=int, default=int(os.environ.get("APPLETS_BAUD", "115200")))
    parser.add_argument("--flash", action="store_true", help="flash before running tests")
    args = parser.parse_args()

    if serial is None:
        raise RuntimeError("pyserial is not installed. Install with: pip install pyserial")
    if not args.port:
        raise RuntimeError("serial port is required. Use --port or APPLETS_PORT env")

    if args.flash:
        run(f"{IDF_EXPORT} && idf.py -p {args.port} flash")

    applets = parse_manifest_applets()
    matrix = test_matrix()
    missing = [a for a in applets if a not in matrix]
    if missing:
        raise RuntimeError(f"No test command defined for applets: {', '.join(missing)}")

    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    try:
        ser.reset_input_buffer()
        ser.write(b"\n")
        _boot = read_until(ser, b"xv6> ", 60.0)
        cmd(ser, "export PATH=/bin:/usr/bin:.")
        prepare_shell_state(ser)
        for applet in applets:
            commands = build_applet_commands(applet, matrix)
            if not commands:
                raise RuntimeError(f"No commands generated for applet {applet}")
            for command in commands:
                out = cmd(ser, command, timeout_s=90.0)
                assert_ok_output(applet, out)
        print(f"HIL applet test passed ({len(applets)} applets, exhaustive flags)")
        return 0
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
