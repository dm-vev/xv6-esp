#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

from qemu_idf_session import launch_idf_qemu, stop_idf_qemu

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
PROMPT = b"xv6> "

FATAL_MARKERS = (
    "Guru Meditation Error",
    "panic'ed",
    "Backtrace:",
    "task_wdt: Task watchdog got triggered",
    "LoadProhibited",
    "StoreProhibited",
    "Traceback (most recent call last)",
    "assert failed:",
    "elf load failed",
    "unresolved symbol:",
)

APPLET_CASES: dict[str, list[str]] = {
    "basename": ["basename /bin/echo", "basename /bin/echo .x"],
    "cat": ["cat /etc/rc", "cat /home/README", "cat -n /no_such_file"],
    "chgrp": ["chgrp 0 /tmp/integration/uniq.in"],
    "chmod": ["chmod 600 /tmp/integration/uniq.in", "chmod -R 755 /tmp/integration"],
    "chown": ["chown 0:0 /tmp/integration/uniq.in"],
    "cmp": [
        "cmp -s /tmp/integration/cmp.a /tmp/integration/cmp.a",
        "cmp /tmp/integration/cmp.a /tmp/integration/cmp.b",
    ],
    "cp": ["cp /etc/rc /tmp/integration/cp.rc", "cp -r /tmp/integration /tmp/integration_copy"],
    "dd": ["dd if=/dev/zero of=/tmp/integration/dd.bin bs=8 count=2"],
    "dirname": ["dirname /bin/echo", "dirname /bin"],
    "dlhello": ["dlhello"],
    "echo": ["echo hello", "echo -n world"],
    "fd_test": ["fd_test"],
    "find": ["find /tmp/integration -name uniq.in -type f"],
    "fs_stress": ["fs_stress"],
    "fs_stress_test": ["fs_stress_test"],
    "head": ["head -2 /tmp/integration/uniq.in"],
    "hostabi_probe": ["hostabi_probe"],
    "init": ["init"],
    "kmod": ["kmod list"],
    "ln": [
        "ln -s /tmp/integration/uniq.in /tmp/integration/uniq.sym",
        "ln -f /tmp/integration/uniq.in /tmp/integration/uniq.hard",
    ],
    "ls": ["ls /", "ls /bin"],
    "mem_test": ["mem_test"],
    "mkdir": ["mkdir -p /tmp/integration/a/b/c"],
    "mv": ["mv -f /tmp/integration/cp.rc /tmp/integration/mv.rc"],
    "printenv": ["printenv", "printenv PATH"],
    "proc_test": ["proc_test"],
    "pwd": ["pwd"],
    "readlink": [
        "readlink /tmp/integration/uniq.sym",
        "readlink -n /tmp/integration/uniq.sym",
    ],
    "rev": ["rev /tmp/integration/uniq.in"],
    "rm": ["rm -f /tmp/integration/mv.rc", "rm -f /tmp/integration/dd.bin"],
    "rmdir": ["rmdir /tmp/integration_copy"],
    "sh": ['sh -c "echo sh-ok"'],
    "sleep": ["sleep 1"],
    "split": ["split -2 /tmp/integration/split.in /tmp/integration/x_"],
    "stat": ["stat /tmp/integration/uniq.in"],
    "sum": ["sum /tmp/integration/uniq.in"],
    "tee": ["echo tee-line | tee /tmp/integration/tee.out"],
    "touch": ["touch /tmp/integration/touch.out"],
    "tr": ["echo abc | tr a A", "echo aaabbb | tr -s a"],
    "uname": ["uname", "uname -a"],
    "uniq": [
        "uniq /tmp/integration/uniq.in /tmp/integration/uniq.out",
        "uniq -c /tmp/integration/uniq.in",
    ],
    "wc": ["wc -l /tmp/integration/uniq.in", "wc /tmp/integration/uniq.in"],
    "xargs": ["echo one two three | xargs -n 2 echo"],
}

HOSTABI_PROBE_STRICT = os.environ.get("XV6_HOSTABI_STRICT", "0") == "1"
APPLET_CASE_TIMEOUT_S = 90.0
GENERIC_APPLET_TIMEOUT_S = 30.0


class SuiteError(RuntimeError):
    pass


def clean_output(text: str) -> str:
    text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", text)
    return "".join(ch for ch in text if ch in "\n\r\t" or 32 <= ord(ch) <= 126)


def run_shell(cmd: str) -> None:
    subprocess.run(["bash", "-lc", cmd], cwd=ROOT, check=True)


def resolve_idf_export() -> str:
    def has_export_script(base: Path) -> bool:
        try:
            return (base / "export.sh").exists()
        except OSError:
            return False

    candidates = [ROOT.parent / "magnolia" / "esp-idf"]
    if os.environ.get("IDF_PATH"):
        candidates.append(Path(os.environ["IDF_PATH"]))
    home = Path.home()
    candidates.extend((home / "esp-idf", Path("/opt/esp-idf"), Path("/root/esp-idf"), Path("/tmp/esp-idf")))
    for p in candidates:
        if p and has_export_script(p):
            return f"source {shlex.quote(str((p / 'export.sh').resolve()))} >/dev/null"
    raise RuntimeError("ESP-IDF not found. Set IDF_PATH or install under ~/esp-idf.")


def ensure_cmake_build_dir() -> None:
    if not BUILD.exists():
        return
    if BUILD.is_dir() and (BUILD / "CMakeCache.txt").exists():
        return
    if BUILD.is_symlink() or BUILD.is_file():
        BUILD.unlink()
        return
    if BUILD.is_dir():
        shutil.rmtree(BUILD)


def build_project(idf_export: str) -> None:
    ensure_cmake_build_dir()
    run_shell(f"{idf_export} && idf.py set-target esp32s3 && idf.py build")


def generate_qemu_flash(idf_export: str) -> None:
    merge = (
        f"{idf_export} && "
        "esptool --chip=esp32s3 merge-bin "
        "--output=build/qemu_flash.bin --pad-to-size=2MB "
        "--flash-mode dio --flash-freq 80m --flash-size 2MB "
        "0x0 build/bootloader/bootloader.bin "
        "0x8000 build/partition_table/partition-table.bin "
        "0xf000 build/ota_data_initial.bin "
        "0x120000 build/xv6fs.bin "
        "0x20000 build/xv6_esp32s3.bin"
    )
    run_shell(merge)


def ensure_qemu_efuse() -> None:
    efuse = BUILD / "qemu_efuse.bin"
    efuse.parent.mkdir(parents=True, exist_ok=True)
    efuse.write_bytes(bytes(1024))


def prepare_qemu_images(idf_export: str) -> None:
    generate_qemu_flash(idf_export)
    ensure_qemu_efuse()
    run_shell("pkill -x qemu-system-xtensa >/dev/null 2>&1 || true")


class QemuShell:
    def __init__(self, idf_export: str) -> None:
        self.idf_export = idf_export
        self.proc = None
        self.sock = None

    def __enter__(self) -> QemuShell:
        flash = BUILD / "qemu_flash.bin"
        efuse = BUILD / "qemu_efuse.bin"
        self.proc, self.sock = launch_idf_qemu(ROOT, self.idf_export, flash, efuse)
        try:
            boot = self.recv_until(PROMPT, timeout_s=90.0).decode(errors="ignore")
        except RuntimeError:
            boot = self.sync_prompt(timeout_s=60.0)
        boot = clean_output(boot)
        self.assert_clean(boot, "boot")
        print(boot)
        self.sync_prompt(timeout_s=20.0)
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        if self.proc is not None:
            stop_idf_qemu(self.proc, self.sock)

    def assert_clean(self, out: str, context: str) -> None:
        for marker in FATAL_MARKERS:
            if marker in out:
                raise SuiteError(f"{context}: detected failure marker '{marker}'")

    def recv_until(self, marker: bytes, timeout_s: float) -> bytes:
        if self.sock is None:
            raise RuntimeError("qemu socket is not initialized")
        self.sock.settimeout(0.8)
        data = bytearray()
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if self.proc is not None and self.proc.poll() is not None:
                raise RuntimeError(f"qemu exited early rc={self.proc.returncode}")
            try:
                chunk = self.sock.recv(4096)
            except TimeoutError:
                continue
            if not chunk:
                continue
            data.extend(chunk)
            if marker in data:
                return bytes(data)
        tail = clean_output(data[-512:].decode(errors="ignore"))
        raise RuntimeError(f"timeout waiting for marker {marker!r}; serial tail={tail!r}")

    def drain_rx(self) -> None:
        if self.sock is None:
            return
        self.sock.settimeout(0.0)
        while True:
            try:
                chunk = self.sock.recv(4096)
            except TimeoutError:
                break
            if not chunk:
                break
        self.sock.settimeout(0.8)

    def sync_prompt(self, timeout_s: float = 30.0) -> str:
        if self.sock is None:
            raise RuntimeError("qemu socket is not initialized")
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            self.sock.sendall(b"\r")
            try:
                out = self.recv_until(PROMPT, timeout_s=2.0).decode(errors="ignore")
                return clean_output(out)
            except RuntimeError:
                continue
        raise RuntimeError("timeout waiting for shell prompt")

    def recover_prompt(self) -> None:
        if self.sock is None:
            return
        try:
            self.sock.sendall(b"\x03")
            out = self.recv_until(PROMPT, timeout_s=10.0).decode(errors="ignore")
            out = clean_output(out)
            self.assert_clean(out, "recover_prompt")
            print("^C\n" + out)
            return
        except (OSError, RuntimeError) as exc:
            print(f"[qemu-ci] recover_prompt fallback: {exc}")
        self.sync_prompt(timeout_s=20.0)

    def cmd(self, command: str, timeout_s: float = 20.0, retries: int = 2, echo: bool = True) -> str:
        if self.sock is None:
            raise RuntimeError("qemu socket is not initialized")
        last_err = ""
        for attempt in range(1, retries + 1):
            self.drain_rx()
            self.sock.sendall((command + "\r").encode())
            try:
                out = self.recv_until(PROMPT, timeout_s=timeout_s).decode(errors="ignore")
            except RuntimeError as exc:
                last_err = str(exc)
                if attempt < retries:
                    self.recover_prompt()
                    continue
                raise
            out = clean_output(out)
            if echo:
                print(f"$ {command}\n{out}")
            self.assert_clean(out, f"cmd:{command}")
            return out
        raise RuntimeError(last_err or f"command failed: {command}")

    def command_exists(self, name: str) -> bool:
        try:
            out = self.cmd(f"ls /bin/{name}", timeout_s=15.0, retries=2)
        except Exception as exc:  # noqa: BLE001
            print(f"[qemu-ci][warn] command_exists({name}) failed: {exc}")
            try:
                self.recover_prompt()
            except Exception as recover_exc:  # noqa: BLE001
                print(f"[qemu-ci][warn] recover_prompt after command_exists({name}) failed: {recover_exc}")
            return False
        return f"/bin/{name}" in out and "cannot access" not in out


def expect_contains(out: str, needle: str, context: str) -> None:
    if needle not in out:
        raise SuiteError(f"{context}: expected {needle!r} in output")


def extract_job_id(out: str) -> str | None:
    m = re.search(r"\[(\d+)\]\s+started", out)
    if m is not None:
        return m.group(1)
    m = re.search(r"\[(\d+)\]", out)
    if m is not None:
        return m.group(1)
    return None


def prepare_applet_fixtures(q: QemuShell) -> None:
    setup_cmds = [
        "export PATH=/bin:/usr/bin:.",
        "rm -f /tmp/integration/uniq.in /tmp/integration/uniq.out /tmp/integration/split.in",
        "rm -f /tmp/integration/cmp.a /tmp/integration/cmp.b",
        "rm -f /tmp/integration/touch.out /tmp/integration/tee.out /tmp/integration/dd.bin",
        "rmdir /tmp/integration_copy",
        "mkdir -p /tmp/integration",
        "echo alpha > /tmp/integration/uniq.in",
        "echo alpha >> /tmp/integration/uniq.in",
        "echo beta >> /tmp/integration/uniq.in",
        "echo gamma >> /tmp/integration/uniq.in",
        "cp /tmp/integration/uniq.in /tmp/integration/cmp.a",
        "cp /tmp/integration/uniq.in /tmp/integration/cmp.b",
        "echo delta >> /tmp/integration/cmp.b",
        "echo one > /tmp/integration/split.in",
        "echo two >> /tmp/integration/split.in",
        "echo three >> /tmp/integration/split.in",
        "echo four >> /tmp/integration/split.in",
    ]
    for c in setup_cmds:
        q.cmd(c, timeout_s=25.0)


def parse_bin_applets(out: str) -> list[str]:
    names: set[str] = set()
    for line in clean_output(out).splitlines():
        s = line.strip()
        if not s:
            continue
        if s.startswith("xv6>"):
            continue
        if s.startswith("ls /bin"):
            continue
        if s.startswith("ls:"):
            continue
        for tok in s.split():
            if re.fullmatch(r"[A-Za-z0-9_.+-]+", tok):
                names.add(tok)
    return sorted(names)


def run_suite_smoke(q: QemuShell) -> None:
    q.cmd("export PATH=/bin:/usr/bin:.")
    out = q.cmd("echo xv6-esp > /tmp/motd.txt")
    expect_contains(out, "xv6> ", "smoke: echo")
    out = q.cmd("head -1 /tmp/motd.txt")
    expect_contains(out, "xv6-esp", "smoke: head")
    out = q.cmd("cp /tmp/motd.txt /tmp/motd2.txt")
    expect_contains(out, "xv6> ", "smoke: cp")
    out = q.cmd("mv /tmp/motd2.txt /tmp/motd3.txt")
    expect_contains(out, "xv6> ", "smoke: mv")
    out = q.cmd("ls /tmp")
    expect_contains(out, "motd3.txt", "smoke: ls")

    out = q.cmd("sleep 300 &")
    jid = extract_job_id(out)
    if jid is None:
        raise SuiteError("smoke: failed to parse job id for background sleep")
    out = q.cmd(f"kill {jid}")
    expect_contains(out, "kill: ok", "smoke: kill")
    out = q.cmd(f"wait {jid}")
    expect_contains(out, "wait: done", "smoke: wait")

    out = q.cmd("dd if=/dev/zero of=/dev/full bs=4 count=1")
    if "I/O error" not in out and "dd: write failed" not in out:
        raise SuiteError("smoke: expected /dev/full write error")

    if q.command_exists("hostabi_probe"):
        out = q.cmd("hostabi_probe", timeout_s=45.0)
        expect_contains(out, "PROBE SUMMARY failures=0", "smoke: hostabi_probe")


def run_applet_cases(q: QemuShell, applet: str, failures: list[str], warnings: list[str]) -> bool:
    if applet in APPLET_CASES:
        for case in APPLET_CASES[applet]:
            try:
                out = q.cmd(case, timeout_s=APPLET_CASE_TIMEOUT_S)
            except Exception as exc:  # noqa: BLE001
                failures.append(f"{applet}: {case}: {exc}")
                q.recover_prompt()
                continue
            if "exec: command not found" in out:
                failures.append(f"{applet}: {case}: command not found")
        return True

    generic_cases = [f"{applet} -h", f"{applet} --help", applet]
    any_ok = False
    for case in generic_cases:
        try:
            out = q.cmd(case, timeout_s=GENERIC_APPLET_TIMEOUT_S, retries=2)
        except Exception as exc:  # noqa: BLE001
            warnings.append(f"{applet}: {case}: {exc}")
            q.recover_prompt()
            continue
        if "exec: command not found" in out:
            warnings.append(f"{applet}: {case}: command not found")
            continue
        any_ok = True
        break
    if not any_ok:
        failures.append(f"{applet}: generic invocation failed")
    return any_ok


def run_suite_applets(idf_export: str) -> None:
    failures: list[str] = []
    warnings: list[str] = []
    covered = 0

    with QemuShell(idf_export) as q:
        q.cmd("export PATH=/bin:/usr/bin:.")
        prepare_applet_fixtures(q)

        out = q.cmd("ls /bin", timeout_s=25.0)
        applets = parse_bin_applets(out)
        if not applets:
            raise SuiteError("applets: ls /bin returned no applets")

    for applet in applets:
        prepare_qemu_images(idf_export)
        with QemuShell(idf_export) as q:
            q.cmd("export PATH=/bin:/usr/bin:.")
            prepare_applet_fixtures(q)
            covered += int(run_applet_cases(q, applet, failures, warnings))

    if failures:
        msg = "\n".join(failures[:30])
        raise SuiteError(f"applets: {len(failures)} failures\n{msg}")

    print(f"applet coverage: covered={covered} total={len(applets)}")
    if warnings:
        print(f"applet warnings: {len(warnings)} (non-fatal)")


def run_suite_regressions(q: QemuShell) -> None:
    q.cmd("export PATH=/bin:/usr/bin:.")
    cmds = [
        "mkdir -p /tmp/reg",
        "echo alpha > /tmp/reg/in.txt",
        "echo alpha >> /tmp/reg/in.txt",
        "echo beta >> /tmp/reg/in.txt",
        "cat /../etc/../etc/rc | head -1",
        "cat ////etc////rc | head -1",
        "ls /tmp/../../tmp",
        "mkdir -p /tmp/reg/../reg2/./x",
        "touch /tmp/reg2/../../tmp/reg_touch",
        "uniq -999999999999 /tmp/reg/in.txt",
        "split -0 /tmp/reg/in.txt /tmp/reg/out_",
        "cat /no_such_file",
    ]
    for c in cmds:
        q.cmd(c, timeout_s=60.0)

    if q.command_exists("hostabi_probe"):
        out = q.cmd("hostabi_probe", timeout_s=75.0)
        probe_ok = "PROBE SUMMARY failures=0" in out and "hostabi_probe: exit=" not in out
        if not probe_ok:
            msg = "regressions: hostabi_probe summary is not clean"
            if HOSTABI_PROBE_STRICT:
                raise SuiteError(msg)
            print(f"[qemu-ci][warn] {msg}; set XV6_HOSTABI_STRICT=1 to fail on this check")


def run_suite_stress(q: QemuShell, iterations: int) -> None:
    q.cmd("export PATH=/bin:/usr/bin:.")
    for i in range(iterations):
        if i % 3 == 0:
            out = q.cmd("head -1 /etc/rc", timeout_s=15.0, echo=False)
            expect_contains(out, "export PATH=", "stress: head")
        elif i % 3 == 1:
            out = q.cmd("echo stress-check", timeout_s=15.0, echo=False)
            expect_contains(out, "stress-check", "stress: echo")
        else:
            out = q.cmd("wc -l /home/README", timeout_s=15.0, echo=False)
            expect_contains(out, "/home/README", "stress: wc")
        if (i + 1) % 50 == 0:
            print(f"stress progress: {i + 1}/{iterations}")
    out = q.cmd("ps")
    expect_contains(out, "sh", "stress: ps")


def parse_free_heap(out: str) -> int | None:
    m = re.search(r"free_heap=(\d+)\s+bytes", out)
    if m is None:
        return None
    return int(m.group(1))


def run_suite_soak(q: QemuShell, iterations: int) -> None:
    q.cmd("export PATH=/bin:/usr/bin:.")
    start_heap = None
    if q.command_exists("mem"):
        out = q.cmd("mem")
        start_heap = parse_free_heap(out)

    for i in range(iterations):
        if i % 4 == 0:
            out = q.cmd("head -1 /etc/rc", timeout_s=15.0, echo=False)
            expect_contains(out, "export PATH=", "soak: head")
        elif i % 4 == 1:
            q.cmd("cp /home/README /tmp/soak.txt", timeout_s=15.0, echo=False)
        elif i % 4 == 2:
            out = q.cmd("wc -c /tmp/soak.txt", timeout_s=15.0, echo=False)
            expect_contains(out, "/tmp/soak.txt", "soak: wc")
        else:
            out = q.cmd("ls /tmp", timeout_s=15.0, echo=False)
            expect_contains(out, "soak.txt", "soak: ls")
        if (i + 1) % 200 == 0:
            print(f"soak progress: {i + 1}/{iterations}")

    out = q.cmd("ps")
    expect_contains(out, "PID STATE EXIT REASON", "soak: ps header")

    if start_heap is not None:
        out = q.cmd("mem")
        end_heap = parse_free_heap(out)
        if end_heap is not None:
            drift = start_heap - end_heap
            print(f"heap drift: {drift} bytes")
            if drift > 65536:
                raise SuiteError(f"soak: heap drift too high ({drift} bytes)")


def run_suite(idf_export: str, suite: str, stress_iterations: int, soak_iterations: int) -> None:
    if suite == "applets":
        run_suite_applets(idf_export)
        return

    with QemuShell(idf_export) as q:
        if suite == "smoke":
            run_suite_smoke(q)
        elif suite == "regressions":
            run_suite_regressions(q)
        elif suite == "stress":
            run_suite_stress(q, stress_iterations)
        elif suite == "soak":
            run_suite_soak(q, soak_iterations)
        else:
            raise SuiteError(f"unknown suite: {suite}")


SUITE_ORDER = ["smoke", "applets", "regressions", "stress", "soak"]


def resolve_suites(values: list[str]) -> list[str]:
    if not values:
        return SUITE_ORDER[:]
    if "all" in values:
        return SUITE_ORDER[:]
    uniq: list[str] = []
    for v in values:
        if v in uniq:
            continue
        uniq.append(v)
    return uniq


def cli(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Unified QEMU CI suites for xv6-esp")
    parser.add_argument("--suite", action="append", choices=["all", *SUITE_ORDER], help="suite to run")
    parser.add_argument("--skip-build", action="store_true", help="skip idf.py build")
    parser.add_argument(
        "--prepare-only",
        action="store_true",
        help="only prepare qemu flash/efuse and exit",
    )
    parser.add_argument("--retries", type=int, default=int(os.environ.get("XV6_QEMU_SUITE_RETRIES", "2")))
    parser.add_argument(
        "--stress-iterations",
        type=int,
        default=int(os.environ.get("XV6_QEMU_STRESS_ITERATIONS", "200")),
    )
    parser.add_argument(
        "--soak-iterations",
        type=int,
        default=int(os.environ.get("XV6_QEMU_SOAK_ITERATIONS", "1200")),
    )
    args = parser.parse_args(argv)

    retries = max(1, args.retries)
    idf_export = resolve_idf_export()

    if not args.skip_build:
        print("[qemu-ci] build")
        build_project(idf_export)

    print("[qemu-ci] prepare qemu image")
    prepare_qemu_images(idf_export)
    if args.prepare_only:
        return 0

    suites = resolve_suites(args.suite or [])
    for suite in suites:
        ok = False
        for attempt in range(1, retries + 1):
            print(f"[qemu-ci] suite={suite} attempt={attempt}/{retries}")
            try:
                prepare_qemu_images(idf_export)
                run_suite(idf_export, suite, args.stress_iterations, args.soak_iterations)
                print(f"[qemu-ci] suite passed: {suite}")
                ok = True
                break
            except Exception as exc:  # noqa: BLE001
                print(f"[qemu-ci] suite failed: {suite}: {exc}")
                run_shell("pkill -x qemu-system-xtensa >/dev/null 2>&1 || true")
                time.sleep(1.0)
        if not ok:
            print(f"[qemu-ci] suite exhausted retries: {suite}")
            return 1

    print("[qemu-ci] all requested suites passed")
    return 0


if __name__ == "__main__":
    sys.exit(cli())
