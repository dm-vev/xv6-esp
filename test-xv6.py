#!/usr/bin/env python3

#
# python script that tests xv6 without having to boot it and type to its shell
#
# ./test-xv6.py usertests  (runs usertests)
# ./test-xv6.py -q usertests (runs the quick tests of usertests)
# ./test-xv6.py crash  (runs the crash tests)
# ./test-xv6.py log (runs the log crash test)

import argparse
import inspect
import os
import re
import select
import shlex
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path
from subprocess import run

ROOT = Path(__file__).resolve().parent
BUILD = ROOT / "build"

parser = argparse.ArgumentParser()
parser.add_argument("testrex", help="test name or regular expression")
parser.add_argument("-q", action="store_true", help="usertests quick")
args = parser.parse_args()


def run_shell(cmd: str) -> None:
    run(["bash", "-lc", cmd], cwd=ROOT, check=True)


def resolve_idf_export_script() -> Path:
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
            return (p / "export.sh").resolve()
    searched = ", ".join(str(p) for p in candidates)
    raise RuntimeError(f"ESP-IDF export script not found. Searched: {searched}")


IDF_EXPORT_SCRIPT = resolve_idf_export_script()


def idf_export_cmd() -> str:
    return f"source {shlex.quote(str(IDF_EXPORT_SCRIPT))} >/dev/null"


def generate_qemu_flash() -> None:
    merge = (
        f"{idf_export_cmd()} && "
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


def ensure_qemu_efuse() -> None:
    efuse = BUILD / "qemu_efuse.bin"
    efuse.parent.mkdir(parents=True, exist_ok=True)
    if not efuse.exists():
        efuse.write_bytes(bytes(1024))


class QEMU(object):
    _built = False

    def __init__(self, reset=False):
        if reset:
            self.build_xv6()
            self.reset_fs()
        ensure_qemu_efuse()
        qemu_cmd = (
            f"{idf_export_cmd()} && "
            "idf.py qemu "
            "--flash-file build/qemu_flash.bin "
            "--efuse-file build/qemu_efuse.bin"
        )
        self.proc = subprocess.Popen(
            ["bash", "-lc", qemu_cmd],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            cwd=ROOT,
            preexec_fn=os.setsid,
        )
        self.output = ""
        self.outbytes = bytearray()
        time.sleep(2)

    def reset_fs(self):
        try:
            generate_qemu_flash()
        except subprocess.CalledProcessError as e:
            print(f"Command failed with exit code {e.returncode}")
            raise

    def build_xv6(self):
        if QEMU._built:
            return
        try:
            ensure_cmake_build_dir()
            run_shell(f"{idf_export_cmd()} && idf.py set-target esp32s3 && idf.py build")
            QEMU._built = True
        except subprocess.CalledProcessError as e:
            print(f"Command failed with exit code {e.returncode}")
            raise

    def save_output(self):
        try:
            with open(ROOT / "test-xv6.out", "w") as f:
                f.write(self.output)
        except OSError as e:
            print("Provided a bad results path. Error:", e)

    def cmd(self, c):
        if isinstance(c, str):
            c = c.encode("utf-8")
        if self.proc.stdin is None:
            raise RuntimeError("qemu stdin is not available")
        self.proc.stdin.write(c)
        self.proc.stdin.flush()

    def crash(self):
        try:
            pgid = os.getpgid(self.proc.pid)
            print("kill process group", pgid)
            os.killpg(pgid, signal.SIGKILL)
        except ProcessLookupError:
            print("no qemu")
            sys.exit(1)

    def stop(self):
        if self.proc.poll() is not None:
            return
        try:
            os.killpg(os.getpgid(self.proc.pid), signal.SIGTERM)
        except ProcessLookupError:
            return
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass

    def read(self):
        if self.proc.stdout is None:
            return
        fd = self.proc.stdout.fileno()
        while True:
            ready, _, _ = select.select([fd], [], [], 0.2)
            if not ready:
                break
            buf = os.read(fd, 4096)
            if not buf:
                break
            self.outbytes.extend(buf)
        self.output = self.outbytes.decode("utf-8", "replace")

    def lines(self):
        return self.output.splitlines()

    def error(self, detail: str):
        print("FAIL:", detail)
        self.save_output()
        self.stop()
        sys.exit(1)

    def match(self, *regexps, exit=True):
        lines = self.lines()
        last = -1
        for i, line in enumerate(lines):
            if any(re.match(r, line) for r in regexps):
                print(line)
                last = i
        if last == -1 and exit:
            self.error(f"match failed: {regexps}")
        l = ""
        if last >= 0:
            l = lines[last]
        return last >= 0, l

    def monitor(self, *regexps, progress="", timeout):
        deadline = time.time() + timeout
        while True:
            time.sleep(1)
            timeleft = deadline - time.time()
            if timeleft < 0:
                self.error(f"timeout waiting for: {regexps}")
            self.read()
            ok, _ = self.match(*regexps, exit=False)
            if ok:
                return
            ok, line = self.match(progress, exit=False)
            if ok:
                print(line)


def crash_log():
    q = QEMU(True)
    q.cmd("logstress f0 f1 f2 f3 f4 f5\n")
    time.sleep(2)
    q.crash()
    q.stop()


def recover_log():
    q = QEMU()
    time.sleep(2)
    q.read()
    ok, _ = q.match("^recovering", exit=False)
    if ok:
        q.cmd("ls\n")
        time.sleep(2)
        q.read()
        q.match("f5")
    q.stop()
    return ok


def forphan():
    q = QEMU(True)
    q.cmd("forphan\n")
    time.sleep(5)
    q.read()
    q.match("wait")
    q.crash()
    q.stop()


def dorphan():
    q = QEMU(True)
    q.cmd("dorphan\n")
    time.sleep(5)
    q.read()
    q.match("wait")
    q.crash()
    q.stop()


def recover_orphan():
    q = QEMU()
    time.sleep(2)
    q.read()
    q.match("^ireclaim")
    q.stop()


def test_log():
    print("Test recovery of log")
    for i in range(5):
        crash_log()
        ok = recover_log()
        if ok:
            print("OK")
            return
        print("log attempt ", i + 1)
    print("FAIL")
    sys.exit(1)


def test_forphan():
    print("Test recovery of an orphaned file")
    forphan()
    recover_orphan()
    print("OK")


def test_dorphan():
    print("Test recovery of an orphaned file")
    dorphan()
    recover_orphan()
    print("OK")


def test_crash():
    test_log()
    test_forphan()
    test_dorphan()


def test_usertests(test=""):
    timeout = 600
    opt = ""
    if args.q:
        opt = " -q"
        timeout = 300
    elif test != "":
        opt += " " + test
    q = QEMU(True)
    q.cmd("usertests" + opt + "\n")
    q.monitor("^ALL TESTS PASSED", progress="test", timeout=timeout)
    q.stop()


def main():
    print(args)
    rex = r"%s" % args.testrex
    funcs = [
        (obj, name)
        for name, obj in inspect.getmembers(sys.modules[__name__])
        if (inspect.isfunction(obj) and name.startswith("test"))
    ]
    none = True
    for f, n in funcs:
        if re.search(rex, n):
            none = False
            f()
    if none:
        test_usertests(test=args.testrex)


main()
