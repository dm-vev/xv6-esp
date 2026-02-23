#!/usr/bin/env python3
import difflib
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
    raise RuntimeError(f"timeout waiting for marker {marker!r}")


def sync_prompt(sock: socket.socket, timeout_s: float = 30.0) -> str:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        sock.sendall(b"\n")
        try:
            return recv_until(sock, b"xv6> ", timeout_s=1.5).decode(errors="ignore")
        except RuntimeError:
            continue
    raise RuntimeError("timeout waiting for shell prompt")


def cmd(sock: socket.socket, command: str, timeout_s: float = 60.0) -> str:
    def _sanitize(text: str) -> str:
        return "".join(ch for ch in text if ch in "\r\n\t" or 32 <= ord(ch) <= 126)

    sock.sendall((command + "\n").encode())
    out = recv_until(sock, b"xv6> ", timeout_s=timeout_s).decode(errors="ignore")
    print(f"$ {command}\n{_sanitize(out)}")
    return out


def prepare_shell_state(sock: socket.socket) -> None:
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
        out = cmd(sock, c, timeout_s=20.0)
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
        out = cmd(sock, c, timeout_s=20.0)
        assert_ok_output("setup", out)

    out = cmd(sock, "wc -l /tmp/integration/uniq.in", timeout_s=20.0)
    assert_ok_output("setup", out)
    if " 5 /tmp/integration/uniq.in" not in out and "\t5 /tmp/integration/uniq.in" not in out:
        raise AssertionError("setup: /tmp/integration/uniq.in was not created correctly")

    out = cmd(sock, "wc -l /tmp/integration/split.in", timeout_s=20.0)
    assert_ok_output("setup", out)
    if " 4 /tmp/integration/split.in" not in out and "\t4 /tmp/integration/split.in" not in out:
        raise AssertionError("setup: /tmp/integration/split.in was not created correctly")


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


def launch_qemu(qemu_bin: str) -> subprocess.Popen:
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
        "Backtrace:",
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


def test_matrix() -> dict[str, list[str]]:
    return {
        "basename": [
            "basename /bin/echo",
            "basename /bin/echo .x",
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

    matrix = test_matrix()
    missing = [a for a in applets if a not in matrix]
    if missing:
        raise RuntimeError(f"No test command defined for applets: {', '.join(missing)}")

    qemu_bin = idf_which("qemu-system-xtensa")
    for applet in applets:
        commands = build_applet_commands(applet, matrix)
        if not commands:
            raise RuntimeError(f"No commands generated for applet {applet}")
        qemu_proc = launch_qemu(qemu_bin)
        sock = None
        try:
            sock = wait_socket("127.0.0.1", 5555, timeout_s=20.0)
            _boot = sync_prompt(sock, timeout_s=30.0)
            cmd(sock, "export PATH=/bin:/usr/bin:.")
            prepare_shell_state(sock)
            for command in commands:
                out = cmd(sock, command, timeout_s=60.0)
                assert_ok_output(applet, out)
        finally:
            if sock is not None:
                sock.close()
            stop_qemu(qemu_proc)

    print(f"QEMU applet test passed ({len(applets)} applets, exhaustive flags)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
