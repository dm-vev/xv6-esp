#!/usr/bin/env python3
import difflib
import os
import re
import subprocess
import sys
import time
from pathlib import Path

from qemu_idf_session import launch_idf_qemu, stop_idf_qemu

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
APPLETS_DIR = ROOT / "applets"
HOSTABI_EXPECTED = ROOT / "docs" / "tests" / "hostabi_probe.expected"


def resolve_idf_export() -> str:
    def has_export_script(base: Path) -> bool:
        try:
            return (base / "export.sh").exists()
        except OSError:
            return False

    candidates = []
    candidates.append(ROOT.parent / "magnolia" / "esp-idf")
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
            return recv_until(sock, b"xv6> ", timeout_s=1.5).decode(errors="ignore")
        except RuntimeError:
            continue
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


def cmd(sock, command: str, timeout_s: float = 60.0) -> str:
    def _sanitize(text: str) -> str:
        return "".join(ch for ch in text if ch in "\r\n\t" or 32 <= ord(ch) <= 126)

    drain_rx(sock)
    sock.sendall((command + "\r").encode())
    out = recv_until(sock, b"xv6> ", timeout_s=timeout_s).decode(errors="ignore")
    print(f"$ {command}\n{_sanitize(out)}")
    return out


def prepare_shell_state(sock) -> None:
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
        cmd_checked(sock, "setup", c, timeout_s=20.0)

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
        cmd_checked(sock, "setup", c, timeout_s=20.0)

    for _ in range(6):
        out = cmd_checked(sock, "setup", "ls /tmp/integration", timeout_s=20.0)
        if "uniq.in" in out and "split.in" in out:
            return
    raise AssertionError("setup: integration fixtures were not created")


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


def parse_manifest_applets() -> list[str]:
    names: list[str] = []
    for manifest in sorted(APPLETS_DIR.glob("*/applet.cmake")):
        text = manifest.read_text(encoding="utf-8", errors="ignore")
        m_name = re.search(r"\bNAME\s+([A-Za-z0-9_.-]+)", text)
        if m_name is None:
            continue
        m_enabled = re.search(r"\bENABLED\s+([A-Za-z0-9_.-]+)", text)
        enabled = m_enabled.group(1).upper() if m_enabled else "ON"
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
    if applet == "chmod":
        if fl == "R":
            return "chmod -R 700 /tmp/integration"
        if fl == "h":
            return "chmod -h"
        return None
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
    if applet == "ln":
        if fl == "s":
            return "ln -s /tmp/integration/uniq.in /tmp/integration/uniq_auto_sym"
        if fl == "f":
            return "ln -f /tmp/integration/uniq.in /tmp/integration/uniq_auto_hard"
        if fl == "h":
            return "ln -h"
        return None
    if applet == "readlink":
        if fl == "n":
            return "readlink -n /tmp/integration/uniq.sym"
        if fl == "h":
            return "readlink -h"
        return None
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
    if applet == "xargs":
        if fl == "0":
            return "echo one | xargs -0 echo"
        if fl == "r":
            return "echo | xargs -r echo"
        if fl == "n":
            return "echo one two three | xargs -n 2 echo"
        if fl == "h":
            return "xargs -h"
        return None
    return None


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
        "Traceback (most recent call last)",
        "alloc failed",
    )
    for marker in bad_markers:
        if marker in out:
            raise AssertionError(f"{applet}: detected failure marker '{marker}'")

    must_have_markers = {
        "dlhello": ("dlhello: libdemo sum=42",),
    }
    for marker in must_have_markers.get(applet, ()):
        if marker not in out:
            raise AssertionError(f"{applet}: expected output marker not found: '{marker}'")

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
                    tofile="qemu:hostabi_probe-output",
                    lineterm="",
                )
            )
            raise AssertionError(f"hostabi_probe output mismatch\n{diff}")


def cmd_checked(sock, applet: str, command: str, timeout_s: float = 60.0, retries: int = 3) -> str:
    last_err: AssertionError | None = None
    for _ in range(retries):
        out = cmd(sock, command, timeout_s=timeout_s)
        try:
            assert_ok_output(applet, out)
            return out
        except AssertionError as err:
            last_err = err
    if last_err is not None:
        raise last_err
    raise AssertionError(f"{applet}: command did not produce output")


def cmd_retry_contains(sock, command: str, expected: str, timeout_s: float = 30.0, retries: int = 4) -> str:
    last_out = ""
    for _ in range(retries):
        out = cmd(sock, command, timeout_s=timeout_s)
        assert_ok_output("smoke", out)
        if expected in out:
            return out
        last_out = out
    raise RuntimeError(f"expected {expected!r} in output for {command!r}\n{last_out}")


def test_matrix() -> dict[str, list[str]]:
    return {
        "basename": [
            "basename /bin/echo",
            "basename /bin/echo .x",
        ],
        "chgrp": [
            "chgrp 0 /tmp/integration/uniq.in",
            "chgrp 0 /tmp/integration/wc.in",
        ],
        "chmod": [
            "chmod 600 /tmp/integration/uniq.in",
            "chmod u+rw /tmp/integration/uniq.in",
            "chmod -R 755 /tmp/integration",
        ],
        "chown": [
            "chown 0:0 /tmp/integration/uniq.in",
            "chown 0 /tmp/integration/wc.in",
        ],
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
        "dd": [
            "dd if=/no_such_input of=/dd_echo bs=16 count=1",
            "dd conv=unknown if=/no_such_input of=/dd_cat",
        ],
        "dlhello": [
            "dlhello",
        ],
        "dirname": [
            "dirname /bin/echo",
            "dirname /bin",
        ],
        "find": [
            "find /tmp/integration -name uniq.in -type f",
            "find /tmp/integration -mindepth 1 -maxdepth 2 -type f",
        ],
        "sh": [
            'sh -c "echo sh-ok"',
        ],
        "echo": [
            "echo -n hello",
            "echo world",
        ],
        "fd_test": [
            "fd_test",
        ],
        "fs_stress": [
            "fs_stress",
        ],
        "fs_stress_test": [
            "fs_stress_test",
        ],
        "head": [
            "head -2 /no_such_file",
            "head -2 /tmp/integration/uniq.in",
        ],
        "hostabi_probe": [
            "hostabi_probe",
        ],
        "init": [
            "ls /bin/init",
        ],
        "kmod": [
            "kmod list",
        ],
        "ln": [
            "ln -s /tmp/integration/uniq.in /tmp/integration/uniq.sym",
            "ln -f /tmp/integration/uniq.in /tmp/integration/uniq.hard",
        ],
        "ls": [
            "ls /",
            "ls /bin",
        ],
        "mem_test": [
            "mem_test",
        ],
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
        "printenv": [
            "printenv",
            "printenv PATH",
        ],
        "proc_test": [
            "proc_test",
        ],
        "pwd": [
            "pwd",
        ],
        "readlink": [
            "ln -s /tmp/integration/uniq.in /tmp/integration/uniq.sym",
            "readlink /tmp/integration/uniq.sym",
            "readlink -n /tmp/integration/uniq.sym",
        ],
        "rev": [
            "rev /no_such_file",
        ],
        "rm": [
            "rm -f /mv_echo",
            "rm -f /mv_cat",
            "rm -f /tmp/integration/cmp.a.moved",
        ],
        "rmdir": [
            "rmdir /tmp/integration/a",
            "rmdir /tmp/integration/b",
        ],
        "sleep": [
            "sleep 1",
        ],
        "split": [
            "split -2 /tmp/integration/split.in /tmp/integration/x_",
            "split -2 /no_such_file /split_echo_",
            "split -2 /no_such_file /split_cat_",
            "split -0 /tmp/integration/split.in /tmp/integration/x_bad_",
        ],
        "sum": [
            "sum /no_such_file",
        ],
        "stat": [
            "stat /tmp/integration/uniq.in",
            "stat /tmp/integration/wc.in",
        ],
        "tee": [
            "echo sample | tee -a /tee.out",
            "echo next | tee -a /tee.out",
        ],
        "touch": [
            "touch /touch.out",
            "touch /touch2.out",
        ],
        "tr": [
            "echo abc | tr a A",
            "echo xyz | tr x X",
        ],
        "uname": [
            "uname",
            "uname -a",
            "uname -r",
        ],
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
        "xargs": [
            "echo one two | xargs echo",
            "echo one two three | xargs -n 2 echo",
            "echo one | xargs -r echo",
        ],
    }


def main() -> int:
    if os.environ.get("XV6_SKIP_BUILD") != "1":
        run(f"{IDF_EXPORT} && idf.py set-target esp32s3 && idf.py build")
    generate_qemu_flash()
    ensure_qemu_efuse()
    run("pkill -x qemu-system-xtensa >/dev/null 2>&1 || true")

    applets = parse_manifest_applets()
    if not applets:
        raise RuntimeError("No enabled applets found in applets/*/applet.cmake")

    qemu_proc, sock = launch_qemu()
    try:
        try:
            _boot = recv_until(sock, b"xv6> ", timeout_s=60.0).decode(errors="ignore")
        except RuntimeError:
            _boot = sync_prompt(sock, timeout_s=60.0)
        cmd_retry_contains(sock, "export PATH=/bin:/usr/bin:.", "xv6> ")

        # POSIX applet behavior smoke on recently added commands.
        cmd_retry_contains(sock, "echo alpha > /tmp/rl.txt", "xv6> ")
        cmd_retry_contains(sock, "ln -s /tmp/rl.txt /tmp/rl.lnk", "xv6> ")
        cmd_retry_contains(sock, "readlink /tmp/rl.lnk", "/tmp/rl.txt")
        cmd_retry_contains(sock, "stat /tmp/rl.txt", "File:")
        cmd_retry_contains(sock, "chmod 600 /tmp/rl.txt", "xv6> ")
        cmd_retry_contains(sock, "chown 0:0 /tmp/rl.txt", "xv6> ")
        cmd_retry_contains(sock, "chgrp 0 /tmp/rl.txt", "xv6> ")
        cmd_retry_contains(sock, "find /tmp -name rl.txt -type f", "/tmp/rl.txt")
        cmd_retry_contains(sock, "echo one two three | xargs -n 2 echo", "one two")

        probe = cmd_retry_contains(sock, "hostabi_probe", "PROBE SUMMARY failures=0", timeout_s=60.0)
        if "hostabi_probe: exit=" in probe:
            raise RuntimeError("hostabi_probe exited non-zero")
    finally:
        stop_qemu(qemu_proc, sock)

    print(f"QEMU applet test passed ({len(applets)} applets, smoke)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
